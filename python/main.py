#!/usr/bin/env python3
"""sts_test exercises the GCP Workload Identity Federation flow by hand,
using nothing but the standard library (urllib + json):

 0. Read a *second*, more narrowly-scoped projected KSA token and exchange
    it at STS for a "bootstrap" federated token, used only to create the
    target bucket if it doesn't exist yet.
 1. Read a projected Kubernetes ServiceAccount OIDC token (the "subject
    token") from disk.
 2. Exchange it at the STS token endpoint for a short-lived federated
    access token.
 3. Call the GCS JSON API directly (list / upload / download an object)
    with that federated token. No google-cloud-storage import anywhere.

This uses "direct resource access": the bucket's IAM policy grants
roles/storage.objectAdmin straight to the federated principal (see
README step 8), so the federated token from step 2 is used as-is - no
service account impersonation, no iamcredentials.googleapis.com call.

The stage 0 bootstrap token is deliberately a *different* federated
principal (same KSA, different token audience -> different WIF provider,
see README "Automatic bucket creation") holding only a custom
storage.buckets.create/.get role at the project level - it can never
touch objects, and the stage 1-3 token can never create/delete a bucket.
"""

from __future__ import annotations

import dataclasses
import datetime
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

STS_TOKEN_URL = "https://sts.googleapis.com/v1/token"
GCS_API_BASE = "https://storage.googleapis.com/storage/v1"
GCS_UPLOAD_BASE = "https://storage.googleapis.com/upload/storage/v1"

TIMEOUT_SECONDS = 10  # matches Go's http.Client{Timeout: 10 * time.Second}


def log(msg: str) -> None:
    """Mimics Go's log.Printf: a 'YYYY/MM/DD HH:MM:SS ' prefix on stderr."""
    now = datetime.datetime.now().strftime("%Y/%m/%d %H:%M:%S")
    print(f"{now} {msg}", file=sys.stderr)


def fatal(msg: str) -> None:
    log(msg)
    sys.exit(1)


# config is read entirely from environment variables set on the pod spec.
@dataclasses.dataclass
class Config:
    ksa_token_path: str              # path to the projected KSA OIDC token file (runtime, object ops)
    workload_audience: str           # //iam.googleapis.com/projects/.../workloadIdentityPools/.../providers/... (runtime)
    ksa_bootstrap_token_path: str    # path to the second projected KSA OIDC token file (bootstrap, bucket create)
    workload_audience_bootstrap: str  # same shape as workload_audience, but for the bootstrap provider
    project_id: str                  # GCP project ID, needed for the bucket-create/-check calls
    location: str                    # bucket location (e.g. europe-west9), used only if the bucket needs creating
    bucket: str                      # GCS bucket to exercise
    object_name: str = "sts-test-object.txt"  # object name to upload/download during the test
    lifecycle_file: str = ""         # optional path to the lifecycle rules applied when stage 0 creates the bucket
    lifecycle: dict[str, Any] | None = None  # the "lifecycle" object read from lifecycle_file, None if unset


def load_config() -> Config:
    ksa_token_path = os.environ.get("KSA_TOKEN_PATH", "")
    workload_audience = os.environ.get("WORKLOAD_AUDIENCE", "")
    ksa_bootstrap_token_path = os.environ.get("KSA_BOOTSTRAP_TOKEN_PATH", "")
    workload_audience_bootstrap = os.environ.get("WORKLOAD_AUDIENCE_BOOTSTRAP", "")
    project_id = os.environ.get("PROJECT_ID", "")
    location = os.environ.get("LOCATION", "")
    bucket = os.environ.get("BUCKET_NAME", "")
    object_name = os.environ.get("OBJECT_NAME") or "sts-test-object.txt"

    missing = []
    if not ksa_token_path:
        missing.append("KSA_TOKEN_PATH")
    if not workload_audience:
        missing.append("WORKLOAD_AUDIENCE")
    if not ksa_bootstrap_token_path:
        missing.append("KSA_BOOTSTRAP_TOKEN_PATH")
    if not workload_audience_bootstrap:
        missing.append("WORKLOAD_AUDIENCE_BOOTSTRAP")
    if not project_id:
        missing.append("PROJECT_ID")
    if not location:
        missing.append("LOCATION")
    if not bucket:
        missing.append("BUCKET_NAME")
    if missing:
        raise RuntimeError(f"missing required env vars: [{' '.join(missing)}]")

    lifecycle_file = os.environ.get("LIFECYCLE_FILE", "")
    lifecycle = read_lifecycle(lifecycle_file) if lifecycle_file else None

    return Config(
        ksa_token_path,
        workload_audience,
        ksa_bootstrap_token_path,
        workload_audience_bootstrap,
        project_id,
        location,
        bucket,
        object_name,
        lifecycle_file,
        lifecycle,
    )


def read_lifecycle(path: str) -> dict[str, Any]:
    """Loads the lifecycle rules sent with the bucket-create request. The file
    can hold either {"lifecycle": {"rule": [...]}} - the same format
    `gcloud storage buckets update --lifecycle-file` accepts - or just
    {"rule": [...]}."""
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        raise RuntimeError(f"reading lifecycle file {path}: {e}") from e
    if "lifecycle" in doc:
        return doc["lifecycle"]
    if "rule" in doc:
        return doc
    raise RuntimeError(f'lifecycle file {path} has neither a "lifecycle" nor a "rule" key')


def read_subject_token(path: str) -> str:
    """Reads the projected KSA OIDC token from disk."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise RuntimeError(f"reading subject token from {path}: {e}") from e
    return data.decode("utf-8").strip()


@dataclasses.dataclass
class StsTokenResponse:
    access_token: str
    issued_token_type: str
    token_type: str
    expires_in: int


# --- Minimal urllib HTTP plumbing ------------------------------------------

def http_request(method: str, url: str, headers: dict[str, str], body: bytes | None = None) -> tuple[int, bytes]:
    """Performs a single HTTP call, returning (status_code, body). Unlike
    urllib's default behaviour, non-2xx responses are returned rather than
    raised, so callers can surface the response body in their own error."""
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_SECONDS) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


# postJSON is a small helper for the STS call: POST a JSON body, optionally
# with a bearer token, and decode a JSON response - surfacing the raw error
# body on non-200 so GCP's descriptive errors aren't swallowed.
def post_json(url: str, bearer_token: str, req_body: dict[str, Any]) -> dict[str, Any]:
    payload = json.dumps(req_body).encode("utf-8")

    headers = {"Content-Type": "application/json"}
    if bearer_token:
        headers["Authorization"] = f"Bearer {bearer_token}"

    log(f"POST {url} \nwith body: {payload.decode('utf-8')} \nand headers: {headers}")

    status, body = http_request("POST", url, headers, payload)
    if status != 200:
        raise RuntimeError(f"request to {url} failed ({status}): {body.decode('utf-8', 'replace')}")
    return json.loads(body)


# exchangeForFederatedToken performs the STS token-exchange grant, trading
# the KSA OIDC subject token for a GCP federated access token.
def exchange_for_federated_token(subject_token: str, audience: str) -> StsTokenResponse:
    req_body = {
        "audience": audience,
        "grantType": "urn:ietf:params:oauth:grant-type:token-exchange",
        "requestedTokenType": "urn:ietf:params:oauth:token-type:access_token",
        "subjectToken": subject_token,
        "subjectTokenType": "urn:ietf:params:oauth:token-type:jwt",
        "scope": "https://www.googleapis.com/auth/cloud-platform",
    }

    try:
        out = post_json(STS_TOKEN_URL, "", req_body)
    except RuntimeError as e:
        raise RuntimeError(f"STS token exchange: {e}") from e

    sts = StsTokenResponse(
        access_token=out.get("access_token", ""),
        issued_token_type=out.get("issued_token_type", ""),
        token_type=out.get("token_type", ""),
        expires_in=out.get("expires_in", 0),
    )
    log(f"Response token creation - {sts.access_token} (expires in {sts.expires_in} seconds)")
    return sts


# --- GCS JSON API calls, all via raw HTTP with the federated Bearer token ---

@dataclasses.dataclass
class GcsObject:
    name: str
    size: str


def list_objects(token: str, bucket: str) -> list[GcsObject]:
    url = f"{GCS_API_BASE}/b/{bucket}/o"
    status, body = http_request("GET", url, {"Authorization": f"Bearer {token}"})
    if status != 200:
        raise RuntimeError(f"list objects failed ({status}): {body.decode('utf-8', 'replace')}")

    try:
        parsed = json.loads(body)
    except json.JSONDecodeError as e:
        raise RuntimeError(f"decoding list response: {e}") from e

    return [GcsObject(item.get("name", ""), item.get("size", "")) for item in parsed.get("items", [])]


def bucket_exists(token: str, project_id: str, bucket: str) -> bool:
    """Checks whether the bucket already exists, using the bootstrap token
    (this needs storage.buckets.get, not storage.objectAdmin)."""
    url = f"{GCS_API_BASE}/b/{bucket}?project={project_id}"
    status, body = http_request("GET", url, {"Authorization": f"Bearer {token}"})
    if status == 200:
        return True
    if status == 404:
        return False
    raise RuntimeError(f"checking bucket {bucket} failed ({status}): {body.decode('utf-8', 'replace')}")


def create_bucket(token: str, project_id: str, bucket: str, location: str,
                  lifecycle: dict[str, Any] | None = None) -> None:
    """Creates the bucket, using the bootstrap token (this needs
    storage.buckets.create, which the runtime token deliberately doesn't have).
    Lifecycle rules, if any, are part of the same request, so setting them needs
    no extra permission - unlike changing them later (storage.buckets.update)."""
    url = f"{GCS_API_BASE}/b?project={project_id}"
    req_body = {
        "name": bucket,
        "location": location,
        "iamConfiguration": {"uniformBucketLevelAccess": {"enabled": True}},
    }
    if lifecycle is not None:
        req_body["lifecycle"] = lifecycle
    try:
        post_json(url, token, req_body)
    except RuntimeError as e:
        raise RuntimeError(f"creating bucket {bucket}: {e}") from e


def upload_object(token: str, bucket: str, object_name: str, data: bytes) -> None:
    # Escape the object name so a "folder/" prefix stays part of the name.
    url = f"{GCS_UPLOAD_BASE}/b/{bucket}/o?uploadType=media&name={urllib.parse.quote(object_name, safe='')}"
    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/octet-stream",
    }
    status, body = http_request("POST", url, headers, data)
    if status != 200:
        raise RuntimeError(f"upload failed ({status}): {body.decode('utf-8', 'replace')}")


def download_object(token: str, bucket: str, object_name: str) -> bytes:
    # GCS expects the whole object name as one path segment ("/" -> "%2F").
    url = f"{GCS_API_BASE}/b/{bucket}/o/{urllib.parse.quote(object_name, safe='')}?alt=media"
    status, body = http_request("GET", url, {"Authorization": f"Bearer {token}"})
    if status != 200:
        raise RuntimeError(f"download failed ({status}): {body.decode('utf-8', 'replace')}")
    return body


def main() -> None:
    try:
        cfg = load_config()
    except RuntimeError as e:
        fatal(f"config error: {e}")

    log(f"stage 0a: reading bootstrap subject token from {cfg.ksa_bootstrap_token_path}")
    try:
        bootstrap_subject_token = read_subject_token(cfg.ksa_bootstrap_token_path)
    except RuntimeError as e:
        fatal(str(e))
    log(f"stage 0a: OK (token length {len(bootstrap_subject_token)})")

    log(f"stage 0b: exchanging bootstrap subject token at STS for audience {cfg.workload_audience_bootstrap}")
    try:
        bootstrap_sts = exchange_for_federated_token(bootstrap_subject_token, cfg.workload_audience_bootstrap)
    except RuntimeError as e:
        fatal(str(e))
    log(f"stage 0b: OK, bootstrap token type={bootstrap_sts.token_type} expires_in={bootstrap_sts.expires_in}s")

    log(f"stage 0c: checking whether gs://{cfg.bucket} exists")
    try:
        exists = bucket_exists(bootstrap_sts.access_token, cfg.project_id, cfg.bucket)
    except RuntimeError as e:
        fatal(str(e))
    if exists:
        log("stage 0c: OK, bucket already exists")
    else:
        log(f"stage 0c: bucket missing, creating gs://{cfg.bucket} in {cfg.location}")
        if cfg.lifecycle is not None:
            log(f"stage 0c: with lifecycle rules from {cfg.lifecycle_file}")
        try:
            create_bucket(bootstrap_sts.access_token, cfg.project_id, cfg.bucket, cfg.location, cfg.lifecycle)
        except RuntimeError as e:
            fatal(str(e))
        log("stage 0c: OK, bucket created")

    log(f"stage 1: reading subject token from {cfg.ksa_token_path}")
    try:
        subject_token = read_subject_token(cfg.ksa_token_path)
    except RuntimeError as e:
        fatal(str(e))
    log(f"stage 1: OK (token length {len(subject_token)})")

    log(f"stage 2: exchanging subject token at STS for audience {cfg.workload_audience}")
    try:
        sts = exchange_for_federated_token(subject_token, cfg.workload_audience)
    except RuntimeError as e:
        fatal(str(e))
    log(f"stage 2: OK, federated token type={sts.token_type} expires_in={sts.expires_in}s")

    token = sts.access_token

    log(f"stage 3a: listing objects in gs://{cfg.bucket}")
    try:
        objects = list_objects(token, cfg.bucket)
    except RuntimeError as e:
        fatal(str(e))
    log(f"stage 3a: OK, found {len(objects)} object(s)")
    for o in objects:
        log(f"  - {o.name} ({o.size} bytes)")

    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    payload = f"sts-test upload at {now}\n".encode("utf-8")
    log(f"stage 3b: uploading gs://{cfg.bucket}/{cfg.object_name} ({len(payload)} bytes)")
    try:
        upload_object(token, cfg.bucket, cfg.object_name, payload)
    except RuntimeError as e:
        fatal(str(e))
    log("stage 3b: OK")

    log(f"stage 3c: downloading gs://{cfg.bucket}/{cfg.object_name}")
    try:
        downloaded = download_object(token, cfg.bucket, cfg.object_name)
    except RuntimeError as e:
        fatal(str(e))
    if downloaded != payload:
        fatal(
            "stage 3c: downloaded content does not match uploaded content\n"
            f"  uploaded:   {payload!r}\n"
            f"  downloaded: {downloaded!r}"
        )
    log("stage 3c: OK, downloaded content matches uploaded content")

    log("all stages passed")


if __name__ == "__main__":
    main()
