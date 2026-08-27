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
$env:BUCKET_NAME    = "origin-sts"

# --- derived values, computed automatically from the above ---
$env:PROJECT_NUMBER = (gcloud projects describe $env:PROJECT_ID --format='value(projectNumber)')
$env:ISSUER_URI = "https://container.googleapis.com/v1/projects/$($env:PROJECT_ID)/locations/$($env:LOCATION)/clusters/$($env:CLUSTER_NAME)"
$env:WORKLOAD_AUDIENCE = "//iam.googleapis.com/projects/$($env:PROJECT_NUMBER)/locations/global/workloadIdentityPools/$($env:POOL_ID)/providers/$($env:PROVIDER_ID)"
$env:IMAGE = "$($env:LOCATION)-docker.pkg.dev/$($env:PROJECT_ID)/sts-test/sts-test:latest"
