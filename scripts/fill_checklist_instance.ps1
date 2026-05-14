[CmdletBinding()]
param(
  [string]$Checklist = "PM Checklist EM",
  [string]$InstanceId = "EBEPDCE9WPPX195K",
  [string]$Section = "Title Page",
  [int]$Count = 12,
  [int]$DelayMs = 1000
)

function Fail($msg) {
  throw $msg
}

function Normalize-WebResponse($resp) {
  $responseUri = $null
  if ($resp -is [Microsoft.PowerShell.Commands.WebResponseObject]) {
    if ($resp.BaseResponse -and $resp.BaseResponse.ResponseUri) {
      $responseUri = $resp.BaseResponse.ResponseUri.AbsoluteUri
    }
  } elseif ($resp -is [System.Net.HttpWebResponse]) {
    if ($resp.ResponseUri) {
      $responseUri = $resp.ResponseUri.AbsoluteUri
    }
  }
  if ($resp -is [System.Net.HttpWebResponse]) {
    $content = ""
    try {
      $stream = $resp.GetResponseStream()
      if ($stream) {
        $reader = New-Object System.IO.StreamReader($stream)
        $content = $reader.ReadToEnd()
        $reader.Close()
      }
    } catch {
      $content = ""
    }
    return [PSCustomObject]@{
      StatusCode = [int]$resp.StatusCode
      Headers    = $resp.Headers
      Content    = $content
      ResponseUri = $responseUri
    }
  }
  if ($resp -is [Microsoft.PowerShell.Commands.WebResponseObject]) {
    return [PSCustomObject]@{
      StatusCode = [int]$resp.StatusCode
      Headers    = $resp.Headers
      Content    = $resp.Content
      ResponseUri = $responseUri
    }
  }
  return $resp
}

function Invoke-Json($Method, $Url, $Body = $null, $Headers = @{}, $ExpectRedirect = $false, $WebSession = $null) {
  $params = @{
    Method      = $Method
    Uri         = $Url
    Headers     = $Headers
    ErrorAction = 'Stop'
    UseBasicParsing = $true
  }
  if ($WebSession) {
    $params.WebSession = $WebSession
  }
  $params.TimeoutSec = 8
  if ($Body -ne $null) {
    if ($Body -is [string]) {
      $params.Body = $Body
    } else {
      $params.Body = ($Body | ConvertTo-Json -Depth 6)
    }
    $params.ContentType = 'application/json'
  }
  try {
    $resp = Invoke-WebRequest @params -MaximumRedirection 5
  } catch {
    if ($_.Exception.Response) {
      return (Normalize-WebResponse $_.Exception.Response)
    }
    throw
  }
  return (Normalize-WebResponse $resp)
}

function Parse-CodeFromLocation($location) {
  if (-not $location) { return $null }
  $uri = [System.Uri]$location
  $query = @{}
  if ($uri.Query.StartsWith('?')) {
    $pairs = $uri.Query.Substring(1) -split '&'
    foreach ($p in $pairs) {
      if ($p -match '=') {
        $kv = $p -split '=', 2
        $query[$kv[0]] = [System.Uri]::UnescapeDataString($kv[1])
      }
    }
  }
  return $query['code']
}

$baseUrl = $env:CHAX_BASE_URL
if ([string]::IsNullOrWhiteSpace($baseUrl)) { $baseUrl = 'http://127.0.0.1:8080' }
$clientId = $env:CHAX_CLIENT_ID
if (-not $clientId) { $clientId = 'chax-ui-client' }
$clientSecret = $env:CHAX_CLIENT_SECRET
if (-not $clientSecret) { $clientSecret = 'chax-ui-secret' }
$adminUser = $env:CHAX_ADMIN_USER
if (-not $adminUser) { $adminUser = 'admin' }
$adminPassword = $env:CHAX_ADMIN_PASSWORD
$token = $env:CHAX_TOKEN
$tokenValid = $false
$webSession = New-Object Microsoft.PowerShell.Commands.WebRequestSession

if ($token) {
  try {
    $probe = Invoke-Json -Method Get -Url "$baseUrl/api/v1/me" -Headers @{Authorization = "Bearer $token"}
    $tokenValid = $probe.StatusCode -eq 200
  } catch {
    $tokenValid = $false
  }
}

if (-not $tokenValid) {
  if (-not $adminPassword) {
    Fail 'CHAX_TOKEN is invalid or expired. Clear CHAX_TOKEN or set CHAX_ADMIN_PASSWORD.'
  }
  $state = [Guid]::NewGuid().ToString('N')
  $redirect = "$baseUrl/oauth/callback"
  $loginPayload = @{response_type='code'; client_id=$clientId; redirect_uri=$redirect; scope='checklist:read checklist:write'; state=$state; username=$adminUser; password=$adminPassword; action='login'}
  $loginResp = Invoke-Json -Method Post -Url "$baseUrl/oauth/authorize" -Body $loginPayload -WebSession $webSession
  if ($loginResp.StatusCode -ne 200) { Fail "login failed: $($loginResp.StatusCode)" }
  $approvePayload = @{response_type='code'; client_id=$clientId; redirect_uri=$redirect; scope='checklist:read checklist:write'; state=$state; action='approve'}
  $approveResp = Invoke-Json -Method Post -Url "$baseUrl/oauth/authorize" -Body $approvePayload -ExpectRedirect $true -WebSession $webSession
  $location = $approveResp.Headers['Location']
  $code = $null
  if ($location) { $code = Parse-CodeFromLocation $location }
  if (-not $code -and $approveResp.ResponseUri) { $code = Parse-CodeFromLocation $approveResp.ResponseUri }
  if (-not $code) {
    $bodyPreview = $approveResp.Content
    if ($bodyPreview) { $bodyPreview = $bodyPreview -replace '\\s+', ' ' }
    Fail "approve missing authorization code (status $($approveResp.StatusCode)) body=$bodyPreview"
  }
  $tokenPayload = @{grant_type='authorization_code'; client_id=$clientId; client_secret=$clientSecret; code=$code; redirect_uri=$redirect}
  $tokenResp = Invoke-Json -Method Post -Url "$baseUrl/oauth/token" -Body $tokenPayload
  if ($tokenResp.StatusCode -ne 200) { Fail "token failed: $($tokenResp.StatusCode)" }
  $tokenJson = $tokenResp.Content | ConvertFrom-Json
  if (-not $tokenJson.access_token) { Fail 'token response missing access_token' }
  $token = $tokenJson.access_token
}

$authHeader = @{Authorization = "Bearer $token"}
$checklistParam = [System.Uri]::EscapeDataString($Checklist)
$instanceParam = [System.Uri]::EscapeDataString($InstanceId)
$slugsResp = Invoke-Json -Method Get -Url "$baseUrl/api/v1/slugs?checklist=$checklistParam&instance_id=$instanceParam" -Headers $authHeader
if ($slugsResp.StatusCode -ne 200) { Fail "slug list failed: $($slugsResp.StatusCode)" }
$slugsJson = $slugsResp.Content | ConvertFrom-Json
$items = $slugsJson.data.items
if (-not $items) { Fail "no slugs returned for checklist '$Checklist' instance '$InstanceId'" }

$targets = $items | Where-Object { $_.section -eq $Section }
if (-not $targets) { Fail "no rows found in section '$Section'" }
if ($Count -gt 0) {
  $targets = $targets | Select-Object -First $Count
}

$index = 1
foreach ($row in $targets) {
  $resultValue = "AutoFill $index"
  $commentValue = "autofill " + (Get-Date -Format o)
  $payload = @{result=$resultValue; status='Pass'; comment=$commentValue}
  $updateResp = Invoke-Json -Method Patch -Url "$baseUrl/api/v1/slugs/$($row.address_id)" -Body $payload -Headers $authHeader
  if ($updateResp.StatusCode -ne 200) {
    Fail "update failed for $($row.address_id): $($updateResp.StatusCode)"
  }
  Write-Host ("[fill] {0} -> {1}" -f $row.procedure, $resultValue) -ForegroundColor Green
  if ($DelayMs -gt 0) { Start-Sleep -Milliseconds $DelayMs }
  $index++
}
