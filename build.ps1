# Builds sts-test.exe with MSVC's cl.exe - no CMake, no package manager.
# Just needs the "Desktop development with C++" workload (you already have
# VS 2022 Build Tools with MSVC installed).
#
# Usage:
#   .\build.ps1

$ErrorActionPreference = "Stop"

# If cl.exe isn't already on PATH (i.e. we're not in a "Developer Command
# Prompt"), locate vcvars64.bat via vswhere and import its environment.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "Could not find vswhere.exe - is Visual Studio / Build Tools installed?"
    }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) {
        throw "No Visual Studio installation with the C++ (VC.Tools.x86.x64) component was found."
    }
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

    # Run vcvars64.bat and capture the environment it sets, then apply it here.
    $envDump = cmd.exe /c "`"$vcvars`" && set"
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
        }
    }
}

cl.exe /nologo /std:c++20 /EHsc /W4 /I vendor main.cpp winhttp.lib /Fe:sts-test.exe

if ($LASTEXITCODE -eq 0) {
    Write-Host "Built .\sts-test.exe"
}
