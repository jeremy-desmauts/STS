#!/usr/bin/env bash
# Environment variables for the STS / Workload Identity Federation test.
#
# Fill in the placeholders below, then load them into your shell with:
#   source env.sh
#
# Requires `gcloud` to already be authenticated and pointed at the right
# project (the PROJECT_NUMBER lookup below calls out to it).

export PROJECT_ID=originsolution
export LOCATION=europe-west9
export CLUSTER_NAME=bks350e-gke
export NAMESPACE=default
export KSA_NAME=sts-jitp-ksa
export POOL_ID=sts-origin-pool
export PROVIDER_ID=sts-jitp-provider
export CUSTOM_AUDIENCE=sts-jitp-audience   # arbitrary, just must match on both sides
# Every bucket this test creates/uses must start with BUCKET_PREFIX: the
# IAM grants in README steps 7-8 are conditioned on that prefix, so they
# cover buckets that don't exist yet. Bucket names are global, hence the
# project ID suffix.
export BUCKET_PREFIX=sts-
export BUCKET_NAME="${BUCKET_PREFIX}${PROJECT_ID}"

# --- bootstrap identity (automatic bucket creation) ---
# Same KSA, a second WIF provider + audience -> a different, narrower
# principal, granted only storage.buckets.create/.get on BUCKET_PREFIX* buckets.
# See README "Automatic bucket creation".
export PROVIDER_ID_BOOTSTRAP="${PROVIDER_ID}-bootstrap"
export CUSTOM_AUDIENCE_BOOTSTRAP=sts-jitp-bootstrap-audience
export BOOTSTRAP_ROLE_ID=bucketCreator   # custom project role, created once in Part 1

# Lifecycle rules applied to buckets stage 0 creates (existing buckets are
# left alone). Unset it to create buckets without rules.
export LIFECYCLE_FILE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lifecycle.json"

# --- derived values, computed automatically from the above ---
export PROJECT_NUMBER=$(gcloud projects describe "$PROJECT_ID" --format='value(projectNumber)')
export ISSUER_URI="https://container.googleapis.com/v1/projects/${PROJECT_ID}/locations/${LOCATION}/clusters/${CLUSTER_NAME}"
export WORKLOAD_AUDIENCE="//iam.googleapis.com/projects/${PROJECT_NUMBER}/locations/global/workloadIdentityPools/${POOL_ID}/providers/${PROVIDER_ID}"
export WORKLOAD_AUDIENCE_BOOTSTRAP="//iam.googleapis.com/projects/${PROJECT_NUMBER}/locations/global/workloadIdentityPools/${POOL_ID}/providers/${PROVIDER_ID_BOOTSTRAP}"
export IMAGE="${LOCATION}-docker.pkg.dev/${PROJECT_ID}/sts-test/sts-test:latest"
