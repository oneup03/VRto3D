param(
    [ValidateSet('Build', 'Clean', 'Rebuild')]
    [string]$Target = 'Build',

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$steamPath = if ($env:STEAM_PATH) { $env:STEAM_PATH } else { 'C:\Program Files (x86)\Steam' }

$msbuildCmd = Get-Command msbuild -ErrorAction SilentlyContinue | Select-Object -First 1
$msbuildExe = if ($msbuildCmd) { $msbuildCmd.Source } else { $null }

if (-not $msbuildExe) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "MSBuild was not found in PATH and vswhere.exe was not found at: $vswhere"
    }

    $msbuildExe = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
}

if (-not $msbuildExe) {
    throw 'Unable to locate MSBuild.exe via PATH or vswhere.'
}

& $msbuildExe 'vrto3d.sln' '/m' "/t:$Target" "/p:Configuration=$Configuration;Platform=$Platform"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if ($Target -in @('Build', 'Rebuild')) {
    cmd /c "xcopy \"vrto3d\\vrto3d\" \"output\\drivers\\vrto3d\" /S /Y /I"
    if ($LASTEXITCODE -gt 1) {
        exit $LASTEXITCODE
    }

    cmd /c "xcopy \"output\\drivers\\\" \"$steamPath\\steamapps\\common\\SteamVR\\drivers\\\" /S /Y /I"
    if ($LASTEXITCODE -gt 1) {
        exit $LASTEXITCODE
    }
}

