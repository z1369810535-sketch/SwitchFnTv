# SwitchFnTv anonymous connectivity check.
# Requests the public login page and a signed GET /v/api/v1/sys/config.
# Does not send credentials, cookies, or tokens. Does not print response bodies.

param(
    [string]$BaseUrl = "http://192.168.31.137:5666"
)

$ErrorActionPreference = "Stop"

function Get-Md5Hex([string]$Text) {
    $md5 = [System.Security.Cryptography.MD5]::Create()
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
    $hash = $md5.ComputeHash($bytes)
    return ([BitConverter]::ToString($hash) -replace "-", "").ToLowerInvariant()
}

function Normalize-BaseUrl([string]$Url) {
    $value = $Url.Trim().TrimEnd("/")
    if (-not $value) { $value = "http://192.168.31.137:5666" }
    if ($value -notmatch "^https?://") { $value = "http://$value" }
    $loginAt = $value.IndexOf("/v/login")
    if ($loginAt -ge 0) { $value = $value.Substring(0, $loginAt) }
    if ($value.EndsWith("/v")) { $value = $value.Substring(0, $value.Length - 2) }
    return $value.TrimEnd("/")
}

function New-Authx([string]$Path) {
    $apiKey = "NDzZTVxnRKP8Z0jXg1VAMonaG8akvh"
    $apiSecret = "16CCEB3D-AB42-077D-36A1-F355324E4237"
    $nonce = Get-Random -Minimum 100000 -Maximum 1000000
    $timestamp = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $bodyMd5 = Get-Md5Hex ""
    $signSrc = "${apiKey}_${Path}_${nonce}_${timestamp}_${bodyMd5}_${apiSecret}"
    $sign = Get-Md5Hex $signSrc
    return "nonce=$nonce&timestamp=$timestamp&sign=$sign"
}

$base = Normalize-BaseUrl $BaseUrl
$result = [ordered]@{
    server = $base
    login_page = [ordered]@{ url = "$base/v/login"; status = $null; result = "unknown" }
    sys_config = [ordered]@{
        url = "$base/v/api/v1/sys/config"
        status = $null
        result = "unknown"
        field_names = @()
        nas_oauth_present = $false
    }
}

try {
    $login = Invoke-WebRequest -Uri $result.login_page.url -Method GET -UseBasicParsing -TimeoutSec 15
    $result.login_page.status = [int]$login.StatusCode
    $result.login_page.result = if ($login.StatusCode -eq 200) { "ok" } else { "http_error" }
} catch {
    $result.login_page.result = "network_error"
    if ($_.Exception.Response) { $result.login_page.status = [int]$_.Exception.Response.StatusCode }
}

try {
    $path = "/v/api/v1/sys/config"
    $headers = @{
        "Content-Type" = "application/json"
        "Authx" = New-Authx $path
    }
    $cfg = Invoke-WebRequest -Uri ($base + $path) -Method GET -Headers $headers -UseBasicParsing -TimeoutSec 15
    $result.sys_config.status = [int]$cfg.StatusCode
    $json = $cfg.Content | ConvertFrom-Json
    if ($json.code -eq 0) {
        $result.sys_config.result = "ok"
        $data = $json.data
        if ($data) {
            $result.sys_config.field_names = @($data.PSObject.Properties.Name)
            $result.sys_config.nas_oauth_present = [bool]($data.PSObject.Properties.Name -contains "nas_oauth")
        }
    } elseif ($json.code -eq 5000) {
        $result.sys_config.result = "invalid_sign"
    } else {
        $result.sys_config.result = "api_error"
    }
} catch {
    $result.sys_config.result = "network_error"
    if ($_.Exception.Response) { $result.sys_config.status = [int]$_.Exception.Response.StatusCode }
}

$out = Join-Path (Get-Location) "FnTv-check.json"
($result | ConvertTo-Json -Depth 6) | Set-Content -Encoding utf8 $out
Write-Host "Wrote $out"
Write-Host "login_page=$($result.login_page.status)/$($result.login_page.result) sys_config=$($result.sys_config.status)/$($result.sys_config.result) oauth=$($result.sys_config.nas_oauth_present)"
