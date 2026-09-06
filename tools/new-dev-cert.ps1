<#
.SYNOPSIS
    Creates a self-signed code-signing certificate for AmberSSH development
    builds, and prints the thumbprint package.ps1 needs.

.DESCRIPTION
    This exists so AmberSSH does not borrow another project's signing
    identity. A signature carries a name, and that name should be the name of
    the thing being signed.

    WHAT THIS GIVES YOU
      Tamper-evidence. A signed binary that is modified afterwards fails
      verification, and that holds regardless of who issued the certificate.

    WHAT THIS DOES NOT GIVE YOU
      Trust. Nothing vouches for a self-signed certificate, so SmartScreen
      will still warn, the publisher will still read as unknown, and this is
      not suitable for anything handed to another person. Public distribution
      needs a certificate from a CA already in the Windows trust store.

    The certificate goes into your personal store only. It is deliberately NOT
    installed into Root or TrustedPublisher, because that would tell this
    machine to trust anything signed with this key, and a development key that
    lives unencrypted in a user profile has not earned that.
#>
[CmdletBinding()]
param(
    [string]$Subject = 'CN=AmberSSH Development',
    [int]$Years = 3
)

$ErrorActionPreference = 'Stop'

$existing = @(Get-ChildItem Cert:\CurrentUser\My |
              Where-Object { $_.Subject -eq $Subject -and $_.NotAfter -gt (Get-Date) })
if ($existing.Count -gt 0) {
    Write-Host "A certificate for $Subject already exists:" -ForegroundColor Yellow
    $existing | ForEach-Object {
        Write-Host ("  {0}  expires {1:yyyy-MM-dd}" -f $_.Thumbprint, $_.NotAfter)
    }
    Write-Host "`nUse it, or delete it first if you want a fresh key."
    exit 0
}

$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -CertStoreLocation Cert:\CurrentUser\My `
    -KeyExportPolicy Exportable `
    -KeyUsage DigitalSignature `
    -KeyLength 3072 `
    -HashAlgorithm SHA256 `
    -NotAfter (Get-Date).AddYears($Years)

Write-Host "Created a development code-signing certificate." -ForegroundColor Green
Write-Host "  subject:    $($cert.Subject)"
Write-Host "  thumbprint: $($cert.Thumbprint)"
Write-Host "  expires:    $($cert.NotAfter.ToString('yyyy-MM-dd'))"
Write-Host "  store:      Cert:\CurrentUser\My  (not trusted anywhere, by design)"
Write-Host ""
Write-Host "Tell package.ps1 to use it, either per-run:" -ForegroundColor Cyan
Write-Host "  .\package.ps1 -CertThumbprint $($cert.Thumbprint)"
Write-Host "or once for this account:" -ForegroundColor Cyan
Write-Host "  setx AMBERSSH_CERT_THUMBPRINT $($cert.Thumbprint)"
Write-Host ""
Write-Host "This is a development signature. It is tamper-evident and it is NOT" -ForegroundColor Yellow
Write-Host "publicly trusted: SmartScreen will still warn anyone who downloads it." -ForegroundColor Yellow
