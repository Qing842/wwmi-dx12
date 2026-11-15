param(
    [string]$GameWin64 = "D:\Games\Wuthering Waves\Wuthering Waves Game\Client\Binaries\Win64",
    [switch]$LauncherOnly
)

$ErrorActionPreference = "Stop"

if (-not $LauncherOnly -and (Get-Process -Name "Client-Win64-Shipping" -ErrorAction SilentlyContinue)) {
    Write-Error "Wuthering Waves (Client-Win64-Shipping) is running. Close the game before deploying."
    exit 1
}

if (-not [IO.Directory]::Exists($GameWin64)) {
    Write-Error "Target game Win64 directory not found: $GameWin64"
    exit 1
}

$addonSrc = [IO.Path]::Combine($PSScriptRoot, "build_addon\wwmi_dx12.addon64")
$launchSrc = [IO.Path]::Combine($PSScriptRoot, "package\wwmi_dx12_launch.ps1")

if (-not $LauncherOnly -and -not [IO.File]::Exists($addonSrc)) {
    Write-Error "Built addon not found at '$addonSrc'. Compile the project first or pass -LauncherOnly."
    exit 1
}
if (-not [IO.File]::Exists($launchSrc)) {
    Write-Error "Launcher script not found at '$launchSrc'."
    exit 1
}

function Backup-Once([string]$target) {
    if ([IO.File]::Exists($target)) {
        $bak = "$target.bak"
        if (-not [IO.File]::Exists($bak)) {
            [IO.File]::Copy($target, $bak, $false)
            Write-Host "Backed up: $bak"
        }
    }
}

if (-not $LauncherOnly) {
    $addonDst = [IO.Path]::Combine($GameWin64, "wwmi_dx12.addon64")
    Backup-Once $addonDst
    [IO.File]::Copy($addonSrc, $addonDst, $true)
    Write-Host "Deployed: $addonDst"
}

$launchDst = [IO.Path]::Combine($GameWin64, "wwmi_dx12_launch.ps1")
Backup-Once $launchDst
[IO.File]::Copy($launchSrc, $launchDst, $true)
Write-Host "Deployed: $launchDst"

$csoSrcDir = [IO.Path]::Combine($PSScriptRoot, "shaders_cso")
if (-not $LauncherOnly -and [IO.Directory]::Exists($csoSrcDir)) {
    $csoDstDir = [IO.Path]::Combine($GameWin64, "wwmi_dx12")
    [void][IO.Directory]::CreateDirectory($csoDstDir)
    foreach ($cso in [IO.Directory]::GetFiles($csoSrcDir, "*.cso")) {
        $name = [IO.Path]::GetFileName($cso)
        $dst = [IO.Path]::Combine($csoDstDir, $name)
        [IO.File]::Copy($cso, $dst, $true)
        Write-Host "Deployed shader: $dst"
    }
}

Write-Host "Deployment complete (preserved wwmi_dx12.ini and wwmi_dx12\cache\)."
