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
export BUCKET_NAME=origin-sts

# --- derived values, computed automatically from the above ---
export PROJECT_NUMBER=$(gcloud projects describe "$PROJECT_ID" --format='value(projectNumber)')
export ISSUER_URI="https://container.googleapis.com/v1/projects/${PROJECT_ID}/locations/${LOCATION}/clusters/${CLUSTER_NAME}"
export WORKLOAD_AUDIENCE="//iam.googleapis.com/projects/${PROJECT_NUMBER}/locations/global/workloadIdentityPools/${POOL_ID}/providers/${PROVIDER_ID}"
export IMAGE="${LOCATION}-docker.pkg.dev/${PROJECT_ID}/sts-test/sts-test:latest"
