#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$SdkFeed = (Join-Path $PSScriptRoot '..\gkms-localify-dmm-RE\out\sdk'),
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$SdkFeed = (Resolve-Path -LiteralPath $SdkFeed).Path
$parameters = @{ SdkFeed = $SdkFeed; Test = $Test }
& (Join-Path $PSScriptRoot 'gkms-dlss5\Build.ps1') @parameters
if (-not $?) { throw 'DLSS5 build failed.' }
$parameters = @{ SdkFeed = $SdkFeed }
& (Join-Path $PSScriptRoot 'gkms-localify-dmm-swap\Build.ps1') @parameters
if (-not $?) { throw 'PhotoSwap build failed.' }
& (Join-Path $PSScriptRoot 'Export-Repository.ps1')
if (-not $?) { throw 'PM repository export failed.' }
