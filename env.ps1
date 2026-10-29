# Environment variables for the STS / Workload Identity Federation test.
#
# Fill in the placeholders below, then load them into your shell with:
#   . .\env.ps1
#
# Requires gcloud to already be authenticated and pointed at the right
# project (the PROJECT_NUMBER lookup below calls out to it).

$env:PROJECT_ID     = "originsolution"
$env:LOCATION       = "europe-west9"    # e.g. us-central1
$env:CLUSTER_NAME   = "bks350e-gke"
$env:NAMESPACE      = "default"
$env:KSA_NAME       = "sts-jitp-ksa"
$env:POOL_ID        = "sts-origin-pool"
$env:PROVIDER_ID    = "sts-jitp-provider"
$env:CUSTOM_AUDIENCE = "sts-jitp-audience"       # arbitrary, just must match on both sides
# Every bucket this test creates/uses must start with BUCKET_PREFIX: the
# IAM grants in README steps 7-8 are conditioned on that prefix, so they
# cover buckets that don't exist yet. Bucket names are global, hence the
# project ID suffix.
$env:BUCKET_PREFIX  = "sts-"
$env:BUCKET_NAME    = "$($env:BUCKET_PREFIX)$($env:PROJECT_ID)"

# --- bootstrap identity (automatic bucket creation) ---
# Same KSA, a second WIF provider + audience -> a different, narrower
# principal, granted only storage.buckets.create/.get on BUCKET_PREFIX* buckets.
# See README "Automatic bucket creation".
$env:PROVIDER_ID_BOOTSTRAP     = "$($env:PROVIDER_ID)-bootstrap"
$env:CUSTOM_AUDIENCE_BOOTSTRAP = "sts-jitp-bootstrap-audience"
$env:BOOTSTRAP_ROLE_ID         = "bucketCreator"   # custom project role, created once in Part 1

# Lifecycle rules applied to buckets stage 0 creates (existing buckets are
# left alone). Clear it ($env:LIFECYCLE_FILE = "") to create buckets without rules.
$env:LIFECYCLE_FILE = "$PSScriptRoot\lifecycle.json"

# --- derived values, computed automatically from the above ---
$env:PROJECT_NUMBER = (gcloud projects describe $env:PROJECT_ID --format='value(projectNumber)')
$env:ISSUER_URI = "https://container.googleapis.com/v1/projects/$($env:PROJECT_ID)/locations/$($env:LOCATION)/clusters/$($env:CLUSTER_NAME)"
$env:WORKLOAD_AUDIENCE = "//iam.googleapis.com/projects/$($env:PROJECT_NUMBER)/locations/global/workloadIdentityPools/$($env:POOL_ID)/providers/$($env:PROVIDER_ID)"
$env:WORKLOAD_AUDIENCE_BOOTSTRAP = "//iam.googleapis.com/projects/$($env:PROJECT_NUMBER)/locations/global/workloadIdentityPools/$($env:POOL_ID)/providers/$($env:PROVIDER_ID_BOOTSTRAP)"
$env:IMAGE = "$($env:LOCATION)-docker.pkg.dev/$($env:PROJECT_ID)/sts-test/sts-test:latest"
