#requires -Version 7.0
param(
    [string]$SdkFeed = (Join-Path $PSScriptRoot '..\..\gkms-localify-dmm-RE\out\sdk'),
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'out'
$package = Join-Path $out 'package'
$nativeObj = Join-Path $out 'native-obj'
$managed = Join-Path $out 'managed'
foreach ($directory in @($out, $package, $nativeObj, $managed)) { [void][IO.Directory]::CreateDirectory($directory) }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
[string[]]$arguments = @('-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
$vsRoot = & $vswhere @arguments
if ($LASTEXITCODE -ne 0) { throw "vswhere failed: $LASTEXITCODE" }
if (!$vsRoot) { throw 'Visual Studio C++ x64 tools are required.' }
$vc = Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdk = Get-ChildItem -LiteralPath (Join-Path $kits 'Include') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' -and (Test-Path -LiteralPath (Join-Path $_.FullName 'um\d3d12.h')) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$sdk) { throw 'Windows SDK is required.' }
$compiler = Join-Path $vc.FullName 'bin\Hostx64\x64\cl.exe'
$savedInclude = $env:INCLUDE; $savedLib = $env:LIB
try {
    $env:INCLUDE = @((Join-Path $vc.FullName 'include'), (Join-Path $sdk.FullName 'ucrt'), (Join-Path $sdk.FullName 'shared'), (Join-Path $sdk.FullName 'um'), (Join-Path $sdk.FullName 'winrt')) -join ';'
    $sdkLib = Join-Path $kits ('Lib\' + $sdk.Name)
    $env:LIB = @((Join-Path $vc.FullName 'lib\x64'), (Join-Path $sdkLib 'ucrt\x64'), (Join-Path $sdkLib 'um\x64')) -join ';'
    [string[]]$arguments = @('/nologo', '/LD', '/O2', '/MT', '/EHsc', '/std:c++17', '/W4', '/WX',
        ('/Fo' + (Join-Path $nativeObj 'Native.obj')), (Join-Path $root 'Native.cpp'),
        '/link', ('/OUT:' + (Join-Path $package 'Gkms.Dlss5.Native.dll')),
        ('/IMPLIB:' + (Join-Path $nativeObj 'Gkms.Dlss5.Native.lib')), '/INCREMENTAL:NO',
        'd3d11.lib', 'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'advapi32.lib')
    & $compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }
    [string[]]$arguments = @('/nologo', '/LD', '/O2', '/MT', '/EHsc', '/std:c++17', '/W4', '/WX',
        ('/Fo' + (Join-Path $nativeObj 'NrCaller.obj')), (Join-Path $root 'NrCaller.cpp'), '/link',
        ('/OUT:' + (Join-Path $package 'nvngx.dll_dlssnr.dll')), ('/IMPLIB:' + (Join-Path $nativeObj 'NrCaller.lib')), '/INCREMENTAL:NO')
    & $compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "NR caller build failed: $LASTEXITCODE" }
    Copy-Item -LiteralPath (Join-Path $package 'nvngx.dll_dlssnr.dll') -Destination (Join-Path $out 'nvngx.dll_dlssnr.dll') -Force
    if ($Test) {
        [string[]]$arguments = @('/nologo', '/O2', '/MT', '/EHsc', '/std:c++17', '/W4', '/WX', '/DGKMS_NR_TEST_MAIN',
            ('/Fo' + (Join-Path $nativeObj 'NativeTests.obj')), ('/Fe' + (Join-Path $out 'NativeTests.exe')),
            (Join-Path $root 'Native.cpp'), '/link', ('/IMPLIB:' + (Join-Path $nativeObj 'NativeTests.lib')),
            'd3d11.lib', 'd3d12.lib', 'dxgi.lib', 'd3dcompiler.lib', 'advapi32.lib')
        & $compiler @arguments
        if ($LASTEXITCODE -ne 0) { throw "Native test build failed: $LASTEXITCODE" }
        $testExe = Join-Path $out 'NativeTests.exe'
        & $testExe
        if ($LASTEXITCODE -ne 0) { throw "GPU tests failed: $LASTEXITCODE" }
    }
} finally { $env:INCLUDE = $savedInclude; $env:LIB = $savedLib }
$project = Join-Path $root 'Gkms.Dlss5.csproj'
[string[]]$arguments = @('restore', $project, '--configfile', (Join-Path $root 'NuGet.Config'), ('-p:RestoreAdditionalProjectSources=' + $SdkFeed), '--nologo')
& dotnet @arguments
if ($LASTEXITCODE -ne 0) { throw "SDK restore failed: $LASTEXITCODE" }
[string[]]$arguments = @('build', $project, '-c', 'Release', '--no-restore', '--nologo', '-o', $managed)
& dotnet @arguments
if ($LASTEXITCODE -ne 0) { throw "Plugin build failed: $LASTEXITCODE" }
if ($Test) {
    [string[]]$arguments = @('run', '--project', (Join-Path $root 'ManagedTests.csproj'), '-c', 'Release')
    & dotnet @arguments
    if ($LASTEXITCODE -ne 0) { throw "Managed tests failed: $LASTEXITCODE" }
}
# Explicit file selection keeps SDK/ImGui and unrelated build output out of the plugin.
foreach ($name in @('Gkms.Dlss5.dll', 'Gkms.Dlss5.deps.json', 'plugin.json')) {
    Copy-Item -LiteralPath (Join-Path $managed $name) -Destination (Join-Path $package $name) -Force
}
foreach ($name in @('README.md', 'THIRD_PARTY.md', 'LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $root $name) -Destination (Join-Path $package $name) -Force
}
$manifest = Get-Content -LiteralPath (Join-Path $root 'plugin.json') -Raw | ConvertFrom-Json
$archive = Join-Path $out ('gkms-dlss5-' + $manifest.version + '.zip')
[string[]]$sourceFiles = @('Build.ps1','Gkms.Dlss5.csproj','Directory.Build.props','NuGet.Config','Plugin.cs','Options.cs','NativeBridge.cs','FrameSelection.cs',
    'Native.cpp','NgxApi.h','NrCaller.cpp','ColorPipeline.h','ColorPipeline.inl','ExtendedShaders.h','ExtendedPipeline.inl','ExtendedTests.inl','NativeTests.inl','ManagedTests.cs','ManagedTests.csproj','plugin.json','README.md','THIRD_PARTY.md','LICENSE') |
    ForEach-Object { Join-Path $root $_ }
Compress-Archive -LiteralPath $sourceFiles -DestinationPath (Join-Path $package 'source.zip') -Force
[string[]]$packageFiles = @('Gkms.Dlss5.dll', 'Gkms.Dlss5.deps.json', 'plugin.json', 'Gkms.Dlss5.Native.dll', 'nvngx.dll_dlssnr.dll', 'README.md', 'THIRD_PARTY.md', 'LICENSE', 'source.zip') |
    ForEach-Object { Join-Path $package $_ }
Compress-Archive -LiteralPath $packageFiles -DestinationPath $archive -Force
Write-Output "Plugin package: $archive"
