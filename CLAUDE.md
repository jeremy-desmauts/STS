# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Minimal Go proof-of-concept for **GCP Workload Identity Federation** — demonstrates how a Kubernetes workload authenticates to Google Cloud Storage using OIDC token exchange (STS), deliberately without cloud SDKs or Service Account impersonation.

## Commands

```bash
# Run locally (requires env vars set)
go run .

# Build Docker image
docker build -t $IMAGE .

# Deploy to GKE and tail logs
./rerun.ps1

# Local test (mints a KSA token, then runs go run .)
./rerun.ps1 -Local
```

No linter or test suite is configured — this is a single-shot integration test tool.

## Architecture

Authentication flows through three stages in sequence (`main.go`):

1. **Read KSA token** — reads a projected Kubernetes ServiceAccount OIDC token from disk (`KSA_TOKEN_PATH`, default `/var/run/secrets/tokens/gcp-ksa/token`)
2. **STS token exchange** — POSTs to `https://sts.googleapis.com/v1/token` with the KSA token, receives a short-lived GCP access token (~1h TTL). Uses `urn:ietf:params:oauth:grant-type:token-exchange` grant type.
3. **GCS operations** — uses the federated access token as a Bearer token to List, Upload, and Download an object via the GCS JSON REST API (not the Go SDK)

The bucket IAM policy grants `roles/storage.objectAdmin` directly to the federated principal — no GSA impersonation step, no `iamcredentials.googleapis.com` calls.

## Key Design Decisions

- **No external Go dependencies** (`go.mod` has none) — intentionally uses only `net/http`, `encoding/json`, etc. to expose the raw OAuth2 mechanics
- **Distroless runtime image** (`gcr.io/distroless/static-debian12`) with a statically compiled binary (`CGO_ENABLED=0`)
- **One-shot execution** — Kubernetes pod has `restartPolicy: Never`; the app exits after all three GCS operations succeed or the first failure

## Required Environment Variables

| Variable | Description |
|---|---|
| `KSA_TOKEN_PATH` | Path to projected OIDC token file |
| `WORKLOAD_AUDIENCE` | Full workload identity pool+provider resource name |
| `BUCKET_NAME` | Target GCS bucket |
| `OBJECT_NAME` | GCS object key to test (defaults to `sts-test-object.txt`) |

See `env.sh` / `env.ps1` for how these (and the GCP/GKE setup variables) are derived.
