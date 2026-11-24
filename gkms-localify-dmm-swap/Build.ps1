#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$SdkFeed = (Join-Path $PSScriptRoot '..\..\gkms-localify-dmm-RE\out\sdk'),
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$SdkFeed = (Resolve-Path -LiteralPath $SdkFeed).Path
$tests = Join-Path $PSScriptRoot 'Gkms.PhotoSwap.Tests.csproj'
[string[]]$arguments = @('restore', $tests, "-p:SdkFeed=$SdkFeed")
& dotnet @arguments
if ($LASTEXITCODE -ne 0) { throw "依赖还原失败：$LASTEXITCODE" }
[string[]]$arguments = @('build', $tests, '-c', $Configuration, '--no-restore', "-p:SdkFeed=$SdkFeed")
& dotnet @arguments
if ($LASTEXITCODE -ne 0) { throw "编译失败：$LASTEXITCODE" }
$testDll = Join-Path $PSScriptRoot "build\re\bin\Gkms.PhotoSwap.Tests\$Configuration\net6.0\Gkms.PhotoSwap.Tests.dll"
[string[]]$arguments = @($testDll)
& dotnet @arguments
if ($LASTEXITCODE -ne 0) { throw "离线测试失败：$LASTEXITCODE" }

$output = Join-Path $PSScriptRoot "build\re\bin\Gkms.PhotoSwap\$Configuration\net6.0"
$manifest = Get-Content -LiteralPath (Join-Path $output 'plugin.json') -Raw | ConvertFrom-Json
$package = Join-Path $PSScriptRoot ("build\{0}-{1}.zip" -f $manifest.id, $manifest.version)
# Rebuild the local artifact only; this script does not install or publish it.
$stream = [IO.File]::Open($package, [IO.FileMode]::Create)
try {
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        foreach ($name in @('plugin.json', 'Gkms.PhotoSwap.dll', 'Gkms.PhotoSwap.deps.json', 'System.Drawing.Common.dll', 'Microsoft.Win32.SystemEvents.dll', 'runtimes\win\lib\net6.0\Microsoft.Win32.SystemEvents.dll')) {
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $output $name), $name.Replace('\', '/'))
        }
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $PSScriptRoot 'LICENSE'), 'LICENSE')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, (Join-Path $PSScriptRoot 'THIRD-PARTY-NOTICES.txt'), 'THIRD-PARTY-NOTICES.txt')
    } finally { $archive.Dispose() }
} finally { $stream.Dispose() }
[string[]]$arguments = @($testDll, '--package', $package)
& dotnet @arguments
if ($LASTEXITCODE -ne 0) { throw "插件包隔离加载测试失败：$LASTEXITCODE" }
Write-Output "RE 插件包：$package"
