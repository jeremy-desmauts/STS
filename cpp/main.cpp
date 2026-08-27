// Command sts-test exercises the GCP Workload Identity Federation flow by
// hand, using nothing but WinHTTP:
//
//  1. Read a projected Kubernetes ServiceAccount OIDC token (the "subject
//     token") from disk.
//  2. Exchange it at the STS token endpoint for a short-lived federated
//     access token.
//  3. Call the GCS JSON API directly (list / upload / download an object)
//     with that federated token. No Google Cloud C++ SDK anywhere.
//
// This uses "direct resource access": the bucket's IAM policy grants
// roles/storage.objectAdmin straight to the federated principal (see
// README step 5), so the federated token from step 2 is used as-is - no
// service account impersonation, no iamcredentials.googleapis.com call.
//
// Dependencies (not in the standard library):
//   - WinHTTP        (HTTP/TLS transport - ships with Windows, no install)
//   - nlohmann/json  (JSON encode/decode - vendored single header, see
//                     vendor/nlohmann/json.hpp)
//
// Build (no CMake needed - just cl.exe from a "Developer Command Prompt",
// or run build.ps1 which locates it for you):
//   cl.exe /std:c++20 /EHsc /I vendor main.cpp winhttp.lib /Fe:sts-test.exe

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;

namespace {

constexpr const wchar_t* kStsHost      = L"sts.googleapis.com";
constexpr const char*    kStsPath      = "/v1/token";
constexpr const wchar_t* kGcsHost      = L"storage.googleapis.com";
constexpr const char*    kGcsAPIPath   = "/storage/v1";
constexpr const char*    kGcsUploadPath = "/upload/storage/v1";

// logPrintf mimics Go's log.Printf: a "YYYY/MM/DD HH:MM:SS " prefix on stderr.
template <typename... Args>
void logPrintf(std::format_string<Args...> fmt, Args&&... args) {
    auto now = std::chrono::system_clock::now();
    std::cerr << std::format("{:%Y/%m/%d %H:%M:%S} ", std::chrono::floor<std::chrono::seconds>(now))
               << std::format(fmt, std::forward<Args>(args)...) << '\n';
}

[[noreturn]] void fatalf(const std::string& msg) {
    logPrintf("{}", msg);
    std::exit(1);
}

// config is read entirely from environment variables set on the pod spec.
struct Config {
    std::string ksaTokenPath;     // path to the projected KSA OIDC token file
    std::string workloadAudience; // //iam.googleapis.com/projects/.../workloadIdentityPools/.../providers/...
    std::string bucket;           // GCS bucket to exercise
    std::string object;           // object name to upload/download during the test
};

std::string getEnvOr(const char* name, const std::string& fallback = "") {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

Config loadConfig() {
    Config c{
        .ksaTokenPath     = getEnvOr("KSA_TOKEN_PATH"),
        .workloadAudience = getEnvOr("WORKLOAD_AUDIENCE"),
        .bucket           = getEnvOr("BUCKET_NAME"),
        .object           = getEnvOr("OBJECT_NAME"),
    };
    if (c.object.empty()) {
        c.object = "sts-test-object.txt";
    }

    std::vector<std::string> missing;
    if (c.ksaTokenPath.empty())     missing.push_back("KSA_TOKEN_PATH");
    if (c.workloadAudience.empty()) missing.push_back("WORKLOAD_AUDIENCE");
    if (c.bucket.empty())           missing.push_back("BUCKET_NAME");
    if (!missing.empty()) {
        std::string joined;
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i) joined += " ";
            joined += missing[i];
        }
        throw std::runtime_error(std::format("missing required env vars: [{}]", joined));
    }
    return c;
}

std::string trim(const std::string& s) {
    auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    auto begin = std::find_if_not(s.begin(), s.end(), isSpace);
    auto end   = std::find_if_not(s.rbegin(), s.rend(), isSpace).base();
    return (begin < end) ? std::string(begin, end) : std::string();
}

// readSubjectToken reads the projected KSA OIDC token from disk.
std::string readSubjectToken(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error(std::format("reading subject token from {}: could not open file", path));
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    if (f.bad()) {
        throw std::runtime_error(std::format("reading subject token from {}: read error", path));
    }
    return trim(buf.str());
}

struct StsTokenResponse {
    std::string accessToken;
    std::string issuedTokenType;
    std::string tokenType;
    int expiresIn = 0;
};

// --- Minimal WinHTTP plumbing (host/path split - no runtime URL parsing) ---

struct HttpResponse {
    int status = 0;
    std::string body;
};

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), size);
    return result;
}

// httpRequest performs a single HTTPS call to https://host + path. method is
// "GET" or "POST". headers are "Key: Value" strings; body (if any) is sent
// as the request body.
HttpResponse httpRequest(const std::string& method, const wchar_t* host, const std::string& path,
                          const std::vector<std::string>& headers,
                          const std::optional<std::string>& body = std::nullopt) {
    HINTERNET hSession = WinHttpOpen(L"sts-test-cpp/1.0",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        throw std::runtime_error(std::format("WinHttpOpen failed: error {}", GetLastError()));
    }

    // Matches Go's http.Client{Timeout: 10 * time.Second}: resolve/connect/
    // send/receive each get up to 10s.
    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hSession);
        throw std::runtime_error(std::format("WinHttpConnect failed: error {}", err));
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, widen(method).c_str(), widen(path).c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        throw std::runtime_error(std::format("WinHttpOpenRequest failed: error {}", err));
    }

    std::wstring headerBlock;
    for (const auto& h : headers) {
        headerBlock += widen(h);
        headerBlock += L"\r\n";
    }

    void* bodyPtr  = body ? const_cast<char*>(body->data()) : nullptr;
    DWORD bodyLen  = body ? static_cast<DWORD>(body->size()) : 0;

    BOOL ok = WinHttpSendRequest(hRequest,
                                  headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                                  headerBlock.empty() ? 0 : static_cast<DWORD>(headerBlock.size()),
                                  bodyPtr, bodyLen, bodyLen, 0);
    if (ok) {
        ok = WinHttpReceiveResponse(hRequest, nullptr);
    }

    HttpResponse resp;
    if (ok) {
        DWORD statusCode = 0, statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        resp.status = static_cast<int>(statusCode);

        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::string chunk(bytesAvailable, '\0');
            DWORD bytesRead = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), bytesAvailable, &bytesRead)) {
                ok = FALSE;
                break;
            }
            chunk.resize(bytesRead);
            resp.body += chunk;
        }
    }

    DWORD lastErr = ok ? 0 : GetLastError();
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!ok) {
        throw std::runtime_error(std::format("HTTP request to {} failed: WinHTTP error {}", path, lastErr));
    }
    return resp;
}

// postJSON is a small helper for the STS call: POST a JSON body, optionally
// with a bearer token, and decode a JSON response - surfacing the raw error
// body on non-200 so GCP's descriptive errors aren't swallowed.
json postJSON(const wchar_t* host, const std::string& path, const std::string& bearerToken, const json& reqBody) {
    std::string payload = reqBody.dump();

    std::vector<std::string> headers{"Content-Type: application/json"};
    if (!bearerToken.empty()) {
        headers.push_back("Authorization: Bearer " + bearerToken);
    }

    std::string joinedHeaders;
    for (size_t i = 0; i < headers.size(); ++i) {
        if (i) joinedHeaders += " ";
        joinedHeaders += headers[i];
    }
    logPrintf("POST {} \nwith body: {} \nand headers: [{}]", path, payload, joinedHeaders);

    HttpResponse resp = httpRequest("POST", host, path, headers, payload);
    if (resp.status != 200) {
        throw std::runtime_error(std::format("request to {} failed ({}): {}", path, resp.status, resp.body));
    }
    return json::parse(resp.body);
}

// exchangeForFederatedToken performs the STS token-exchange grant, trading
// the KSA OIDC subject token for a GCP federated access token.
StsTokenResponse exchangeForFederatedToken(const std::string& subjectToken, const std::string& audience) {
    json reqBody = {
        {"audience", audience},
        {"grantType", "urn:ietf:params:oauth:grant-type:token-exchange"},
        {"requestedTokenType", "urn:ietf:params:oauth:token-type:access_token"},
        {"subjectToken", subjectToken},
        {"subjectTokenType", "urn:ietf:params:oauth:token-type:jwt"},
        {"scope", "https://www.googleapis.com/auth/cloud-platform"},
    };

    json out;
    try {
        out = postJSON(kStsHost, kStsPath, "", reqBody);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::format("STS token exchange: {}", e.what()));
    }

    StsTokenResponse sts{
        .accessToken     = out.value("access_token", ""),
        .issuedTokenType = out.value("issued_token_type", ""),
        .tokenType       = out.value("token_type", ""),
        .expiresIn       = out.value("expires_in", 0),
    };
    logPrintf("Response token creation - {} (expires in {} seconds)", sts.accessToken, sts.expiresIn);
    return sts;
}

// --- GCS JSON API calls, all via raw HTTP with the federated Bearer token ---

struct GcsObject {
    std::string name;
    std::string size;
};

std::vector<GcsObject> listObjects(const std::string& token, const std::string& bucket) {
    std::string path = std::format("{}/b/{}/o", kGcsAPIPath, bucket);
    HttpResponse resp = httpRequest("GET", kGcsHost, path, {"Authorization: Bearer " + token});

    if (resp.status != 200) {
        throw std::runtime_error(std::format("list objects failed ({}): {}", resp.status, resp.body));
    }

    json parsed;
    try {
        parsed = json::parse(resp.body);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::format("decoding list response: {}", e.what()));
    }

    std::vector<GcsObject> items;
    for (const auto& it : parsed.value("items", json::array())) {
        items.push_back(GcsObject{
            .name = it.value("name", ""),
            .size = it.value("size", ""),
        });
    }
    return items;
}

void uploadObject(const std::string& token, const std::string& bucket, const std::string& object,
                   const std::string& data) {
    std::string path = std::format("{}/b/{}/o?uploadType=media&name={}", kGcsUploadPath, bucket, object);
    std::vector<std::string> headers{
        "Authorization: Bearer " + token,
        "Content-Type: application/octet-stream",
    };

    HttpResponse resp = httpRequest("POST", kGcsHost, path, headers, data);
    if (resp.status != 200) {
        throw std::runtime_error(std::format("upload failed ({}): {}", resp.status, resp.body));
    }
}

std::string downloadObject(const std::string& token, const std::string& bucket, const std::string& object) {
    std::string path = std::format("{}/b/{}/o/{}?alt=media", kGcsAPIPath, bucket, object);
    HttpResponse resp = httpRequest("GET", kGcsHost, path, {"Authorization: Bearer " + token});

    if (resp.status != 200) {
        throw std::runtime_error(std::format("download failed ({}): {}", resp.status, resp.body));
    }
    return resp.body;
}

// nowRFC3339UTC mirrors Go's time.Now().UTC().Format(time.RFC3339).
std::string nowRFC3339UTC() {
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

} // namespace

int main() {
    Config cfg;
    try {
        cfg = loadConfig();
    } catch (const std::exception& e) {
        fatalf(std::format("config error: {}", e.what()));
    }

    logPrintf("stage 1: reading subject token from {}", cfg.ksaTokenPath);
    std::string subjectToken;
    try {
        subjectToken = readSubjectToken(cfg.ksaTokenPath);
    } catch (const std::exception& e) {
        fatalf(e.what());
    }
    logPrintf("stage 1: OK (token length {})", subjectToken.size());

    logPrintf("stage 2: exchanging subject token at STS for audience {}", cfg.workloadAudience);
    StsTokenResponse sts;
    try {
        sts = exchangeForFederatedToken(subjectToken, cfg.workloadAudience);
    } catch (const std::exception& e) {
        fatalf(e.what());
    }
    logPrintf("stage 2: OK, federated token type={} expires_in={}s", sts.tokenType, sts.expiresIn);

    const std::string& token = sts.accessToken;

    logPrintf("stage 3a: listing objects in gs://{}", cfg.bucket);
    std::vector<GcsObject> objects;
    try {
        objects = listObjects(token, cfg.bucket);
    } catch (const std::exception& e) {
        fatalf(e.what());
    }
    logPrintf("stage 3a: OK, found {} object(s)", objects.size());
    for (const auto& o : objects) {
        logPrintf("  - {} ({} bytes)", o.name, o.size);
    }

    std::string payload = std::format("sts-test upload at {}\n", nowRFC3339UTC());
    logPrintf("stage 3b: uploading gs://{}/{} ({} bytes)", cfg.bucket, cfg.object, payload.size());
    try {
        uploadObject(token, cfg.bucket, cfg.object, payload);
    } catch (const std::exception& e) {
        fatalf(e.what());
    }
    logPrintf("stage 3b: OK");

    logPrintf("stage 3c: downloading gs://{}/{}", cfg.bucket, cfg.object);
    std::string downloaded;
    try {
        downloaded = downloadObject(token, cfg.bucket, cfg.object);
    } catch (const std::exception& e) {
        fatalf(e.what());
    }
    if (downloaded != payload) {
        fatalf(std::format(
            "stage 3c: downloaded content does not match uploaded content\n  uploaded:   \"{}\"\n  downloaded: \"{}\"",
            payload, downloaded));
    }
    logPrintf("stage 3c: OK, downloaded content matches uploaded content");

    logPrintf("all stages passed");

    return 0;
}
