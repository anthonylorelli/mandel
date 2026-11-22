[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ManifestPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')]
    [string]$Version,

    [string]$Publisher,

    [string]$PackageName
)

$ErrorActionPreference = 'Stop'

$resolvedPath = (Resolve-Path -Path $ManifestPath).Path
[xml]$xml = Get-Content -Path $resolvedPath -Raw -Encoding UTF8

$ns = New-Object System.Xml.XmlNamespaceManager($xml.NameTable)
$ns.AddNamespace('ns', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')

$identity = $xml.SelectSingleNode('/ns:Package/ns:Identity', $ns)
if (-not $identity) {
    throw "Could not locate <Identity> element in $resolvedPath"
}

$identity.SetAttribute('Version', $Version)
if (-not [string]::IsNullOrWhiteSpace($Publisher)) {
    $identity.SetAttribute('Publisher', $Publisher)
}
if (-not [string]::IsNullOrWhiteSpace($PackageName)) {
    $identity.SetAttribute('Name', $PackageName)
}

$settings = New-Object System.Xml.XmlWriterSettings
$settings.Indent = $true
$settings.IndentChars = '  '
$settings.Encoding = New-Object System.Text.UTF8Encoding($false)

$writer = [System.Xml.XmlWriter]::Create($resolvedPath, $settings)
try {
    $xml.Save($writer)
} finally {
    $writer.Close()
}

Write-Host "Updated $resolvedPath -> Name=$($identity.GetAttribute('Name')), Publisher=$($identity.GetAttribute('Publisher')), Version=$($identity.GetAttribute('Version'))"
