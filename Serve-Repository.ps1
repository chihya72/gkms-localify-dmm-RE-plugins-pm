#requires -Version 7.0
[CmdletBinding()]
param([ValidateRange(1024, 65535)][int]$Port = 18765)
$ErrorActionPreference = 'Stop'
$listener = [Net.HttpListener]::new()
$listener.Prefixes.Add("http://localhost:$Port/")
try {
    $listener.Start()
    Write-Output "订阅地址：http://localhost:$Port/index.json"
    Write-Output '按 Ctrl+C 停止。本服务仅提供索引和 packages 下的 ZIP，不提供源码或 .git。'
    while ($listener.IsListening) {
        $context = $listener.GetContext()
        try {
            $relative = [Uri]::UnescapeDataString($context.Request.Url.AbsolutePath).TrimStart('/')
            if ($context.Request.HttpMethod -notin @('GET', 'HEAD') -or
                $relative -cnotmatch '^(index\.json|pluginmaster\.json|packages/[A-Za-z0-9._-]+\.zip)$') {
                $context.Response.StatusCode = 404
                continue
            }
            $file = Join-Path $PSScriptRoot $relative.Replace('/', '\')
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { $context.Response.StatusCode = 404; continue }
            $stream = [IO.File]::OpenRead($file)
            try {
                $context.Response.ContentLength64 = $stream.Length
                $context.Response.ContentType = if ($relative.EndsWith('.json')) { 'application/json; charset=utf-8' } else { 'application/zip' }
                if ($context.Request.HttpMethod -eq 'GET') { $stream.CopyTo($context.Response.OutputStream) }
            } finally { $stream.Dispose() }
        } catch [Net.HttpListenerException] {
            Write-Warning $_.Exception.Message
        } finally { $context.Response.Close() }
    }
} finally { $listener.Close() }
