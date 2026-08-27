# Re-runs the sts-test flow, as many times as you like.
#
#   .\rerun.ps1            -> deletes/reapplies the GKE pod and streams its logs
#   .\rerun.ps1 -Local     -> mints a fresh KSA token and runs main.go locally (faster iteration)
#
# Assumes env.ps1 has already been dot-sourced once in this session
# (re-sources it automatically if $env:PROJECT_ID isn't set yet).
param(
    [switch]$Local
)

if (-not $env:PROJECT_ID) {
    . "$PSScriptRoot\env.ps1"
}

if ($Local) {
    kubectl create token $env:KSA_NAME -n $env:NAMESPACE --audience=$env:CUSTOM_AUDIENCE --duration=1h > "$PSScriptRoot\token.txt"
    $env:KSA_TOKEN_PATH = (Resolve-Path "$PSScriptRoot\token.txt").Path
    & "C:\Program Files\Go\bin\go.exe" run "$PSScriptRoot"
    return
}

kubectl delete pod sts-test -n $env:NAMESPACE --ignore-not-found
kubectl apply -f "$PSScriptRoot\k8s\pod.yaml"
kubectl logs -f sts-test -n $env:NAMESPACE
