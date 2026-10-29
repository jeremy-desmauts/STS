# Re-runs the sts-test flow, as many times as you like.
#
#   .\rerun.ps1            -> deletes/reapplies the GKE pod and streams its logs
#   .\rerun.ps1 -Local     -> mints a fresh KSA token and runs main.go locally (faster iteration)
#
# Both modes prompt for bucket names in a loop: each name runs the whole flow
# (stage 0 creates the bucket if it doesn't exist yet), then asks again.
# Press Enter on an empty line, or type q, to stop. Pass -Bucket to run once
# without prompting, e.g.
#   .\rerun.ps1 -Local -Bucket sts-demo-2026
#
# Assumes env.ps1 has already been dot-sourced once in this session
# (re-sources it automatically if $env:PROJECT_ID isn't set yet).
param(
    [switch]$Local,
    [string]$Bucket
)

if (-not $env:PROJECT_ID) {
    . "$PSScriptRoot\env.ps1"
}

# Returns the normalized bucket name, or $null if it isn't valid.
function Resolve-BucketName([string]$name) {
    # No BUCKET_PREFIX is added: a name without it is passed through as is,
    # so the IAM conditions (README steps 7-8) can be seen rejecting it.
    $name = $name.Trim().ToLower()

    # GCS naming rules: 3-63 chars, lowercase letters/digits/-/_/., starting and
    # ending with a letter or digit. Names are global across all of GCP.
    if ($name -notmatch '^[a-z0-9][a-z0-9._-]{1,61}[a-z0-9]$') {
        Write-Warning "'$name' is not a valid bucket name (3-63 chars: a-z, 0-9, '-', '_', '.'; must start and end with a letter or digit)"
        return $null
    }
    return $name
}

function Invoke-StsTest([string]$bucketName) {
    Write-Host "Bucket: gs://$bucketName"

    if ($Local) {
        kubectl create token $env:KSA_NAME -n $env:NAMESPACE --audience=$env:CUSTOM_AUDIENCE --duration=1h | Set-Content -Encoding ascii -NoNewline "$PSScriptRoot\token.txt"
        kubectl create token $env:KSA_NAME -n $env:NAMESPACE --audience=$env:CUSTOM_AUDIENCE_BOOTSTRAP --duration=1h | Set-Content -Encoding ascii -NoNewline "$PSScriptRoot\token-bootstrap.txt"
        $env:KSA_TOKEN_PATH = (Resolve-Path "$PSScriptRoot\token.txt").Path
        $env:KSA_BOOTSTRAP_TOKEN_PATH = (Resolve-Path "$PSScriptRoot\token-bootstrap.txt").Path

        # Override BUCKET_NAME for this run only, so the session keeps env.ps1's default.
        $defaultBucket = $env:BUCKET_NAME
        $env:BUCKET_NAME = $bucketName
        try {
            & "C:\Program Files\Go\bin\go.exe" -C "$PSScriptRoot\go" run .
        } finally {
            $env:BUCKET_NAME = $defaultBucket
        }
        return
    }

    # Swap BUCKET_NAME in the manifest on the fly; k8s/pod.yaml itself is not modified.
    $manifest = (Get-Content -Raw "$PSScriptRoot\k8s\pod.yaml") -replace '(name: BUCKET_NAME\s*\r?\n\s*value: )"[^"]*"', "`${1}`"$bucketName`""

    kubectl delete pod sts-test -n $env:NAMESPACE --ignore-not-found
    $manifest | kubectl apply -f -
    # Logs aren't readable until the container has started.
    do {
        Start-Sleep -Seconds 1
        $phase = kubectl get pod sts-test -n $env:NAMESPACE -o jsonpath='{.status.phase}'
    } while ($phase -eq 'Pending')
    kubectl logs -f sts-test -n $env:NAMESPACE
}

if ($PSBoundParameters.ContainsKey('Bucket')) {
    $name = Resolve-BucketName $Bucket
    if ($name) { Invoke-StsTest $name }
    return
}

while ($true) {
    $answer = Read-Host "`nBucket name (e.g. $($env:BUCKET_NAME); Enter or q = quit)"
    if (-not $answer.Trim() -or $answer.Trim() -eq 'q') {
        break
    }
    $name = Resolve-BucketName $answer
    if ($name) { Invoke-StsTest $name }
}
