[CmdletBinding()]
param(
    [string]$Publisher = 'CN=AnthonyLorelli',
    [string]$FriendlyName = 'Mandelbrot Viewer Code Signing (Self-Signed)',
    [Parameter(Mandatory = $true)]
    [string]$PfxPath,
    [Parameter(Mandatory = $true)]
    [string]$CerPath,
    [Parameter(Mandatory = $true)]
    [string]$Password,
    [ValidateRange(1, 100)]
    [int]$ValidityYears = 5,
    [switch]$PrintBase64
)

$ErrorActionPreference = 'Stop'

$resolvedPfx = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($PfxPath)
$resolvedCer = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($CerPath)

foreach ($outPath in @($resolvedPfx, $resolvedCer)) {
    $dir = Split-Path -Parent $outPath
    if (-not [string]::IsNullOrWhiteSpace($dir) -and -not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
}

$notAfter = (Get-Date).AddYears($ValidityYears)
$cert = New-SelfSignedCertificate `
    -Type Custom `
    -Subject $Publisher `
    -FriendlyName $FriendlyName `
    -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA `
    -KeyLength 2048 `
    -HashAlgorithm SHA256 `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -TextExtension @(
        '2.5.29.37={text}1.3.6.1.5.5.7.3.3',
        '2.5.29.19={text}'
    ) `
    -NotAfter $notAfter

try {
    $securePassword = ConvertTo-SecureString -String $Password -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $resolvedPfx -Password $securePassword | Out-Null
    Export-Certificate -Cert $cert -FilePath $resolvedCer | Out-Null

    Write-Host "Generated self-signed code-signing certificate:"
    Write-Host "  Subject   : $($cert.Subject)"
    Write-Host "  Thumbprint: $($cert.Thumbprint)"
    Write-Host "  Expires   : $($cert.NotAfter.ToString('u'))"
    Write-Host "  PFX       : $resolvedPfx"
    Write-Host "  CER       : $resolvedCer"

    if ($PrintBase64) {
        $b64 = [Convert]::ToBase64String([System.IO.File]::ReadAllBytes($resolvedPfx))
        Write-Host ""
        Write-Host "Store the following as GitHub Actions secret MSIX_CERT_BASE64:"
        Write-Host $b64
    }
} finally {
    # Remove temporary private key from the user store
    if ($cert -and $cert.Thumbprint) {
        Remove-Item -Path "Cert:\CurrentUser\My\$($cert.Thumbprint)" -ErrorAction SilentlyContinue
    }
}
