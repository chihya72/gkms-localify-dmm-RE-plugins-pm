#requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$packages = Join-Path $PSScriptRoot 'packages'
[void][IO.Directory]::CreateDirectory($packages)
$plugins = @()
$master = @()
foreach ($folder in @('gkms-dlss5', 'gkms-localify-dmm-swap')) {
    $root = Join-Path $PSScriptRoot $folder
    $manifest = Get-Content -LiteralPath (Join-Path $root 'plugin.json') -Raw | ConvertFrom-Json
    $relative = if ($folder -eq 'gkms-dlss5') { "out\gkms-dlss5-$($manifest.version).zip" } else { "build\$($manifest.id)-$($manifest.version).zip" }
    $source = (Resolve-Path -LiteralPath (Join-Path $root $relative)).Path
    $archive = [IO.Compression.ZipFile]::OpenRead($source)
    try {
        $entry = $archive.GetEntry('plugin.json')
        if (-not $entry -or -not $archive.GetEntry($manifest.assembly)) { throw "Incomplete plugin package: $source" }
        $reader = [IO.StreamReader]::new($entry.Open())
        try { $packaged = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        if (($packaged | ConvertTo-Json -Depth 50 -Compress) -cne ($manifest | ConvertTo-Json -Depth 50 -Compress)) {
            throw "Package metadata is stale; build $folder before exporting."
        }
    } finally { $archive.Dispose() }
    $zipName = [IO.Path]::GetFileNameWithoutExtension($manifest.assembly) + '-' + $manifest.version + '.zip'
    Copy-Item -LiteralPath $source -Destination (Join-Path $packages $zipName) -Force
    $url = 'packages/' + $zipName
    $restart = [bool]$manifest.requiresRestart
    $hotEnable = if ($null -ne $manifest.canHotEnable) { [bool]$manifest.canHotEnable } else { -not $restart }
    $hotDisable = if ($null -ne $manifest.canHotDisable) { [bool]$manifest.canHotDisable } else { -not $restart }
    $settings = @($manifest.settings).Count -gt 0 -or -not [string]::IsNullOrWhiteSpace($manifest.settingsUi)
    $resources = -not [string]::IsNullOrWhiteSpace($manifest.resourceUpdater)
    $plugins += [ordered]@{
        id = $manifest.id; name = $manifest.name; version = $manifest.version; apiVersion = $manifest.apiVersion
        author = $manifest.author; description = $manifest.description; downloadUrl = $url; assembly = $manifest.assembly
        punchline = $manifest.punchline; category = $manifest.category; tags = @($manifest.tags)
        repositoryUrl = $manifest.repositoryUrl; names = $manifest.names
        requiresRestart = -not ($hotEnable -and $hotDisable); hasSettingsUi = $settings; hasResourceUpdater = $resources
    }
    $master += [ordered]@{
        InternalName = $manifest.id; Name = $manifest.name; AssemblyVersion = $manifest.version
        GkmsApiLevel = $manifest.apiVersion; Author = $manifest.author; Description = $manifest.description
        DownloadLinkInstall = $url; DownloadLinkUpdate = $url; Assembly = $manifest.assembly
        Punchline = $manifest.punchline; Category = $manifest.category; Tags = @($manifest.tags)
        RepoUrl = $manifest.repositoryUrl; Names = $manifest.names
        RequiresRestart = -not ($hotEnable -and $hotDisable); HasSettingsUi = $settings; HasResourceUpdater = $resources
    }
}
$index = [ordered]@{ schemaVersion = 1; name = 'pm 个人插件仓库'; plugins = @($plugins) }
$index | ConvertTo-Json -Depth 50 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'index.json') -Encoding utf8NoBOM
ConvertTo-Json -InputObject @($master) -Depth 50 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'pluginmaster.json') -Encoding utf8NoBOM
Write-Output "PM repository: $PSScriptRoot ($($plugins.Count) plugins)"
