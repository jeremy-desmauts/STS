// Command sts-test exercises the GCP Workload Identity Federation flow by
// hand, using nothing but net/http:
//
//  1. Read a projected Kubernetes ServiceAccount OIDC token (the "subject
//     token") from disk.
//  2. Exchange it at the STS token endpoint for a short-lived federated
//     access token.
//  3. Call the GCS JSON API directly (list / upload / download an object)
//     with that federated token. No cloud.google.com/go/storage import
//     anywhere.
//
// This uses "direct resource access": the bucket's IAM policy grants
// roles/storage.objectAdmin straight to the federated principal (see
// README step 5), so the federated token from step 2 is used as-is - no
// service account impersonation, no iamcredentials.googleapis.com call.
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"time"
)

const (
	stsTokenURL   = "https://sts.googleapis.com/v1/token"
	gcsAPIBase    = "https://storage.googleapis.com/storage/v1"
	gcsUploadBase = "https://storage.googleapis.com/upload/storage/v1"
)

var httpClient = &http.Client{Timeout: 10 * time.Second}

// config is read entirely from environment variables set on the pod spec.
type config struct {
	ksaTokenPath     string // path to the projected KSA OIDC token file
	workloadAudience string // //iam.googleapis.com/projects/.../workloadIdentityPools/.../providers/...
	bucket           string // GCS bucket to exercise
	object           string // object name to upload/download during the test
}

func loadConfig() (config, error) {
	c := config{
		ksaTokenPath:     os.Getenv("KSA_TOKEN_PATH"),
		workloadAudience: os.Getenv("WORKLOAD_AUDIENCE"),
		bucket:           os.Getenv("BUCKET_NAME"),
		object:           os.Getenv("OBJECT_NAME"),
	}
	if c.object == "" {
		c.object = "sts-test-object.txt"
	}
	missing := []string{}
	if c.ksaTokenPath == "" {
		missing = append(missing, "KSA_TOKEN_PATH")
	}
	if c.workloadAudience == "" {
		missing = append(missing, "WORKLOAD_AUDIENCE")
	}
	if c.bucket == "" {
		missing = append(missing, "BUCKET_NAME")
	}
	if len(missing) > 0 {
		return config{}, fmt.Errorf("missing required env vars: %v", missing)
	}
	return c, nil
}

// readSubjectToken reads the projected KSA OIDC token from disk.
func readSubjectToken(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", fmt.Errorf("reading subject token from %s: %w", path, err)
	}
	return string(bytes.TrimSpace(data)), nil
}

type stsTokenRequest struct {
	Audience           string `json:"audience"`
	GrantType          string `json:"grantType"`
	RequestedTokenType string `json:"requestedTokenType"`
	SubjectToken       string `json:"subjectToken"`
	SubjectTokenType   string `json:"subjectTokenType"`
	Scope              string `json:"scope"`
}

type stsTokenResponse struct {
	AccessToken     string `json:"access_token"`
	IssuedTokenType string `json:"issued_token_type"`
	TokenType       string `json:"token_type"`
	ExpiresIn       int    `json:"expires_in"`
}

// exchangeForFederatedToken performs the STS token-exchange grant, trading
// the KSA OIDC subject token for a GCP federated access token.
func exchangeForFederatedToken(subjectToken, audience string) (stsTokenResponse, error) {
	reqBody := stsTokenRequest{
		Audience:           audience,
		GrantType:          "urn:ietf:params:oauth:grant-type:token-exchange",
		RequestedTokenType: "urn:ietf:params:oauth:token-type:access_token",
		SubjectToken:       subjectToken,
		SubjectTokenType:   "urn:ietf:params:oauth:token-type:jwt",
		Scope:              "https://www.googleapis.com/auth/cloud-platform",
	}

	var out stsTokenResponse
	if err := postJSON(stsTokenURL, "", reqBody, &out); err != nil {
		return stsTokenResponse{}, fmt.Errorf("STS token exchange: %w", err)
	}
	log.Printf("Response token creation - %s", out)
	return out, nil
}

// --- GCS JSON API calls, all via raw HTTP with the federated Bearer token ---

type gcsObject struct {
	Name string `json:"name"`
	Size string `json:"size"`
}

type gcsListResponse struct {
	Items []gcsObject `json:"items"`
}

func listObjects(token, bucket string) ([]gcsObject, error) {
	url := fmt.Sprintf("%s/b/%s/o", gcsAPIBase, bucket)
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+token)

	resp, err := httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("listing objects in %s: %w", bucket, err)
	}
	defer resp.Body.Close()

	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("list objects failed (%d): %s", resp.StatusCode, body)
	}

	var out gcsListResponse
	if err := json.Unmarshal(body, &out); err != nil {
		return nil, fmt.Errorf("decoding list response: %w", err)
	}
	return out.Items, nil
}

func uploadObject(token, bucket, object string, data []byte) error {
	url := fmt.Sprintf("%s/b/%s/o?uploadType=media&name=%s", gcsUploadBase, bucket, object)
	req, err := http.NewRequest(http.MethodPost, url, bytes.NewReader(data))
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/octet-stream")

	resp, err := httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("uploading %s to %s: %w", object, bucket, err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("upload failed (%d): %s", resp.StatusCode, body)
	}
	return nil
}

func downloadObject(token, bucket, object string) ([]byte, error) {
	url := fmt.Sprintf("%s/b/%s/o/%s?alt=media", gcsAPIBase, bucket, object)
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", "Bearer "+token)

	resp, err := httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("downloading %s from %s: %w", object, bucket, err)
	}
	defer resp.Body.Close()

	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("download failed (%d): %s", resp.StatusCode, body)
	}
	return body, nil
}

// postJSON is a small helper for the STS call: POST a JSON body, optionally
// with a bearer token, and decode a JSON response - surfacing the raw error
// body on non-200 so GCP's descriptive errors aren't swallowed.
func postJSON(url, bearerToken string, reqBody, out any) error {
	payload, err := json.Marshal(reqBody)
	if err != nil {
		return fmt.Errorf("encoding request body: %w", err)
	}

	req, err := http.NewRequest(http.MethodPost, url, bytes.NewReader(payload))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	if bearerToken != "" {
		req.Header.Set("Authorization", "Bearer "+bearerToken)
	}

	log.Printf("POST %s \nwith body: %s \nand headers: %v", url, payload, req.Header)
	resp, err := httpClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("request to %s failed (%d): %s", url, resp.StatusCode, body)
	}
	return json.Unmarshal(body, out)
}

func main() {
	cfg, err := loadConfig()
	if err != nil {
		log.Fatalf("config error: %v", err)
	}

	log.Printf("stage 1: reading subject token from %s", cfg.ksaTokenPath)
	subjectToken, err := readSubjectToken(cfg.ksaTokenPath)
	if err != nil {
		log.Fatalf("%v", err)
	}
	log.Printf("stage 1: OK (token length %d)", len(subjectToken))

	log.Printf("stage 2: exchanging subject token at STS for audience %s", cfg.workloadAudience)
	sts, err := exchangeForFederatedToken(subjectToken, cfg.workloadAudience)
	if err != nil {
		log.Fatalf("%v", err)
	}
	log.Printf("stage 2: OK, federated token type=%s expires_in=%ds", sts.TokenType, sts.ExpiresIn)

	token := sts.AccessToken

	log.Printf("stage 3a: listing objects in gs://%s", cfg.bucket)
	objects, err := listObjects(token, cfg.bucket)
	if err != nil {
		log.Fatalf("%v", err)
	}
	log.Printf("stage 3a: OK, found %d object(s)", len(objects))
	for _, o := range objects {
		log.Printf("  - %s (%s bytes)", o.Name, o.Size)
	}

	payload := fmt.Appendf(nil, "sts-test upload at %s\n", time.Now().UTC().Format(time.RFC3339))
	log.Printf("stage 3b: uploading gs://%s/%s (%d bytes)", cfg.bucket, cfg.object, len(payload))
	if err := uploadObject(token, cfg.bucket, cfg.object, payload); err != nil {
		log.Fatalf("%v", err)
	}
	log.Printf("stage 3b: OK")

	log.Printf("stage 3c: downloading gs://%s/%s", cfg.bucket, cfg.object)
	downloaded, err := downloadObject(token, cfg.bucket, cfg.object)
	if err != nil {
		log.Fatalf("%v", err)
	}
	if !bytes.Equal(downloaded, payload) {
		log.Fatalf("stage 3c: downloaded content does not match uploaded content\n  uploaded:   %q\n  downloaded: %q", payload, downloaded)
	}
	log.Printf("stage 3c: OK, downloaded content matches uploaded content")

	log.Printf("all stages passed")
}
