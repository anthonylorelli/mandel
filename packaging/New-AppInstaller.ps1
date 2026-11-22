[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$PackageName,

    [Parameter(Mandatory = $true)]
    [string]$Publisher,

    [Parameter(Mandatory = $true)]
    [string]$AppInstallerUri,

    [Parameter(Mandatory = $true)]
    [string]$MainBundleUri,

    [ValidateRange(0, 255)]
    [int]$HoursBetweenUpdateChecks = 0
)

$ErrorActionPreference = 'Stop'

foreach ($part in $Version.Split('.')) {
    $num = [int]$part
    if ($num -lt 0 -or $num -gt 65535) {
        throw "Invalid MSIX version component '$part' in '$Version'; each component must be in 0..65535."
    }
}

$resolvedOutput = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
$dir = Split-Path -Parent $resolvedOutput
if (-not [string]::IsNullOrWhiteSpace($dir) -and -not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

$escapedAppInstallerUri = [System.Security.SecurityElement]::Escape($AppInstallerUri)
$escapedMainBundleUri = [System.Security.SecurityElement]::Escape($MainBundleUri)
$escapedName = [System.Security.SecurityElement]::Escape($PackageName)
$escapedPublisher = [System.Security.SecurityElement]::Escape($Publisher)

$content = @"
<?xml version="1.0" encoding="utf-8"?>
<AppInstaller
  Uri="$escapedAppInstallerUri"
  Version="$Version"
  xmlns="http://schemas.microsoft.com/appx/appinstaller/2021">
  <MainBundle
    Name="$escapedName"
    Version="$Version"
    Publisher="$escapedPublisher"
    Uri="$escapedMainBundleUri" />
  <UpdateSettings>
    <OnLaunch
      HoursBetweenUpdateChecks="$HoursBetweenUpdateChecks"
      ShowPrompt="true" />
    <AutomaticBackgroundTask />
    <ForceUpdateFromAnyVersion>true</ForceUpdateFromAnyVersion>
  </UpdateSettings>
</AppInstaller>
"@

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($resolvedOutput, $content + "`n", $utf8NoBom)

Write-Host "Generated AppInstaller manifest at $resolvedOutput"
Write-Host "  AppInstaller Uri: $AppInstallerUri"
Write-Host "  MainBundle Uri  : $MainBundleUri"
Write-Host "  Version         : $Version"
