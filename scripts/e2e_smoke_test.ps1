[CmdletBinding()]
param()

function Fail($msg) {
  throw $msg
}

$script:startedByTest = $false
$script:stateFile = $null
$script:serverExe = $null
$script:serverPid = $null
$script:tempDir = $null

$baseUrl = $env:CHAX_BASE_URL
if ([string]::IsNullOrWhiteSpace($baseUrl)) { $baseUrl = 'http://127.0.0.1:8080' }
$allowWrite = $env:CHAX_ALLOW_REMOTE -eq '1'
$readOnly = -not $allowWrite -or ($env:CHAX_READONLY -eq '1')
$clientId = $env:CHAX_CLIENT_ID
if (-not $clientId) { $clientId = 'chax-ui-client' }
$clientSecret = $env:CHAX_CLIENT_SECRET
if (-not $clientSecret) { $clientSecret = 'chax-ui-secret' }
$adminUser = $env:CHAX_ADMIN_USER
if (-not $adminUser) { $adminUser = 'admin' }
$adminPassword = $env:CHAX_ADMIN_PASSWORD
$token = $env:CHAX_TOKEN

Write-Host "[smoke] baseUrl=$baseUrl readOnly=$readOnly allowWrite=$allowWrite" -ForegroundColor Cyan

function Invoke-Json($Method, $Url, $Body = $null, $Headers = @{}, $ExpectRedirect = $false) {
  $params = @{
    Method      = $Method
    Uri         = $Url
    Headers     = $Headers
    ErrorAction = 'Stop'
    UseBasicParsing = $true
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
    $resp = Invoke-WebRequest @params -MaximumRedirection 0
  } catch [System.Management.Automation.RuntimeException] {
    if ($ExpectRedirect -and $_.Exception.Response.StatusCode -eq 302) {
      $resp = $_.Exception.Response
    } else {
      throw
    }
  }
  return $resp
}

function Resolve-ServerExe() {
  if ($env:CHAX_SERVER_EXE -and (Test-Path -LiteralPath $env:CHAX_SERVER_EXE)) {
    return (Resolve-Path -LiteralPath $env:CHAX_SERVER_EXE).Path
  }
  $root = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')
  $candidates = @(
    (Join-Path $root 'build/checklist_assistant_server.exe'),
    (Join-Path $root 'build/checklist_assistant_server')
  )
  foreach ($cand in $candidates) {
    if (Test-Path -LiteralPath $cand) { return (Resolve-Path -LiteralPath $cand).Path }
  }
  return $null
}

function Test-Health($Url) {
  try {
    $resp = Invoke-WebRequest -UseBasicParsing -Method Get -Uri "$Url/api/v1/health" -ErrorAction Stop -TimeoutSec 2 -MaximumRedirection 0
    return $resp.StatusCode -eq 200
  } catch {
    return $false
  }
}

function Ensure-ServerRunning($Url) {
  if (Test-Health $Url) { return }

  $exe = Resolve-ServerExe
  if (-not $exe) {
    Fail "Server not reachable at $Url and checklist_assistant_server not found (build the project or set CHAX_SERVER_EXE)."
  }

  $uri = [System.Uri]$Url
  $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("chax-e2e-" + [Guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

  $script:startedByTest = $true
  $script:serverExe = $exe
  $script:tempDir = $tempDir
  $script:stateFile = Join-Path $tempDir 'server.json'
  $outLog = Join-Path $tempDir 'server.out.log'
  $errLog = Join-Path $tempDir 'server.err.log'
  $dbPath = Join-Path $tempDir 'checklists.db'
  $shutdownToken = [Guid]::NewGuid().ToString('N')

  $env:CHAX_DB = $dbPath
  $env:CHAX_SEED_DEMO = '0'
  $env:CHAX_OPEN_BROWSER = '0'

  $proc = Start-Process -FilePath $exe -ArgumentList @(
    'run',
    '--host', $uri.Host,
    '--port', $uri.Port,
    '--no-ui',
    '--no-browser',
    '--state-file', $script:stateFile,
    '--shutdown-token', $shutdownToken
  ) -WorkingDirectory (Split-Path -Parent $exe) -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru -WindowStyle Hidden
  $script:serverPid = $proc.Id

  $deadline = [DateTime]::UtcNow.AddSeconds(10)
  while ([DateTime]::UtcNow -lt $deadline) {
    if (Test-Health $Url) { return }
    Start-Sleep -Milliseconds 250
  }

  Fail "Started server but health did not become ready within 10s. Logs: $outLog / $errLog"
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

try {
  Ensure-ServerRunning $baseUrl

# 1) health
$health = Invoke-Json -Method Get -Url "$baseUrl/api/v1/health"
if ($health.StatusCode -ne 200) { Fail "health failed: $($health.StatusCode)" }
Write-Host "[smoke] health ok" -ForegroundColor Green

# 2) commands (tolerate auth failure in read-only mode)
$commands = $null
try {
  $commands = Invoke-Json -Method Get -Url "$baseUrl/api/v1/commands"
  if ($commands.StatusCode -ne 200) { throw "commands status $($commands.StatusCode)" }
  Write-Host "[smoke] commands ok" -ForegroundColor Green
} catch {
  if ($readOnly) {
    Write-Warning "[smoke] commands skipped/unauthorized in read-only mode: $_"
  } else {
    Fail "commands failed: $_"
  }
}

if ($readOnly) {
  Write-Host "[smoke] read-only mode; skipping write checks" -ForegroundColor Yellow
  return
}

if (-not $token) {
  if (-not $adminPassword) { Fail 'CHAX_ADMIN_PASSWORD or CHAX_TOKEN required for write tests' }
  $state = [Guid]::NewGuid().ToString('N')
  $redirect = "$baseUrl/oauth/callback"
  $loginPayload = @{response_type='code'; client_id=$clientId; redirect_uri=$redirect; scope='checklist:read checklist:write'; state=$state; username=$adminUser; password=$adminPassword; action='login'}
  $loginResp = Invoke-Json -Method Post -Url "$baseUrl/oauth/authorize" -Body $loginPayload
  if ($loginResp.StatusCode -ne 200) { Fail "login failed: $($loginResp.StatusCode)" }
  $cookie = $loginResp.Headers['Set-Cookie']
  if (-not $cookie) { Fail 'login missing Set-Cookie' }
  $approvePayload = @{response_type='code'; client_id=$clientId; redirect_uri=$redirect; scope='checklist:read checklist:write'; state=$state; action='approve'}
  $approveResp = Invoke-Json -Method Post -Url "$baseUrl/oauth/authorize" -Body $approvePayload -Headers @{Cookie=$cookie} -ExpectRedirect $true
  if ($approveResp.StatusCode -ne 302) { Fail "approve failed: $($approveResp.StatusCode)" }
  $location = $approveResp.Headers['Location']
  if (-not $location) { Fail 'approve missing Location' }
  $code = Parse-CodeFromLocation $location
  if (-not $code) { Fail 'missing authorization code' }
  $tokenPayload = @{grant_type='authorization_code'; client_id=$clientId; client_secret=$clientSecret; code=$code; redirect_uri=$redirect}
  $tokenResp = Invoke-Json -Method Post -Url "$baseUrl/oauth/token" -Body $tokenPayload
  if ($tokenResp.StatusCode -ne 200) { Fail "token failed: $($tokenResp.StatusCode)" }
  $tokenJson = $tokenResp.Content | ConvertFrom-Json
  if (-not $tokenJson.access_token) { Fail 'token response missing access_token' }
  $token = $tokenJson.access_token
  Write-Host "[smoke] obtained token" -ForegroundColor Green
}

$authHeader = @{Authorization = "Bearer $token"}
$slugPrefix = "smoke-" + [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$checklistName = "$slugPrefix-checklist"
$linkedChecklistName = "$slugPrefix-linked-checklist"
$instancePrincipal = "$slugPrefix-instance"

# Create slug
$createPayload = @{checklist=$checklistName; section='Smoke'; procedure='Ping'; action='Ping'; spec='Spec'; instructions='Step1'; instance_principal=$instancePrincipal}
$createResp = Invoke-Json -Method Post -Url "$baseUrl/api/v1/slugs" -Body $createPayload -Headers $authHeader
if ($createResp.StatusCode -ne 201) { Fail "create slug failed: $($createResp.StatusCode) body=$($createResp.Content)" }
$createJson = $createResp.Content | ConvertFrom-Json
$addressId = $createJson.data.address_id
if (-not $addressId) { Fail 'create slug missing address_id' }
Write-Host "[smoke] created slug $addressId" -ForegroundColor Green

# Update slug
$updatePayload = @{comment='smoke update'; status='Pass'}
$updateResp = Invoke-Json -Method Patch -Url "$baseUrl/api/v1/slugs/$addressId" -Body $updatePayload -Headers $authHeader
if ($updateResp.StatusCode -ne 200) { Fail "update failed: $($updateResp.StatusCode) body=$($updateResp.Content)" }
Write-Host "[smoke] updated slug" -ForegroundColor Green

# Cross-checklist AndGate precondition smoke
$sourceCrossPayload = @{checklist=$checklistName; section='Smoke'; procedure='Cross Source'; action='Cross Source'; spec='Spec'; instructions='Step1'; instance_principal=$instancePrincipal}
$sourceCrossResp = Invoke-Json -Method Post -Url "$baseUrl/api/v1/slugs" -Body $sourceCrossPayload -Headers $authHeader
if ($sourceCrossResp.StatusCode -ne 201) { Fail "cross source create failed: $($sourceCrossResp.StatusCode) body=$($sourceCrossResp.Content)" }
$sourceCrossJson = $sourceCrossResp.Content | ConvertFrom-Json
$sourceCrossAddress = $sourceCrossJson.data.address_id
if (-not $sourceCrossAddress) { Fail 'cross source create missing address_id' }

$sourceLocalPayload = @{checklist=$linkedChecklistName; section='Smoke'; procedure='Local Source'; action='Local Source'; spec='Spec'; instructions='Step1'; instance_principal=$instancePrincipal}
$sourceLocalResp = Invoke-Json -Method Post -Url "$baseUrl/api/v1/slugs" -Body $sourceLocalPayload -Headers $authHeader
if ($sourceLocalResp.StatusCode -ne 201) { Fail "local source create failed: $($sourceLocalResp.StatusCode) body=$($sourceLocalResp.Content)" }
$sourceLocalJson = $sourceLocalResp.Content | ConvertFrom-Json
$sourceLocalAddress = $sourceLocalJson.data.address_id
if (-not $sourceLocalAddress) { Fail 'local source create missing address_id' }

$targetPayload = @{checklist=$linkedChecklistName; section='Smoke'; procedure='Gate Target'; action='Gate Target'; spec='Spec'; instructions='Step1'; instance_principal=$instancePrincipal}
$targetResp = Invoke-Json -Method Post -Url "$baseUrl/api/v1/slugs" -Body $targetPayload -Headers $authHeader
if ($targetResp.StatusCode -ne 201) { Fail "gate target create failed: $($targetResp.StatusCode) body=$($targetResp.Content)" }
$targetJson = $targetResp.Content | ConvertFrom-Json
$targetAddress = $targetJson.data.address_id
if (-not $targetAddress) { Fail 'gate target create missing address_id' }

$predicateListResp = Invoke-Json -Method Get -Url "$baseUrl/api/v1/predicates?limit=10" -Headers $authHeader
if ($predicateListResp.StatusCode -ne 200) { Fail "predicates list failed: $($predicateListResp.StatusCode)" }

$andPredicate = 'passPropagateAndGatePass'
$relCrossPayload = @{subject_address_id=$sourceCrossAddress; predicate=$andPredicate; target_address_id=$targetAddress}
$relCrossResp = Invoke-Json -Method Post -Url "$baseUrl/api/v1/relationships/address" -Body $relCrossPayload -Headers $authHeader
if ($relCrossResp.StatusCode -ne 201) { Fail "cross relationship create failed: $($relCrossResp.StatusCode) body=$($relCrossResp.Content)" }
$relLocalPayload = @{subject_address_id=$sourceLocalAddress; predicate=$andPredicate; target_address_id=$targetAddress}
$relLocalResp = Invoke-Json -Method Post -Url "$baseUrl/api/v1/relationships/address" -Body $relLocalPayload -Headers $authHeader
if ($relLocalResp.StatusCode -ne 201) { Fail "local relationship create failed: $($relLocalResp.StatusCode) body=$($relLocalResp.Content)" }

$sourceOnePassPayload = @{status='Pass'; comment='source-one-pass'}
$sourceOnePassResp = Invoke-Json -Method Patch -Url "$baseUrl/api/v1/slugs/$sourceCrossAddress" -Body $sourceOnePassPayload -Headers $authHeader
if ($sourceOnePassResp.StatusCode -ne 200) { Fail "first source update failed: $($sourceOnePassResp.StatusCode) body=$($sourceOnePassResp.Content)" }
$targetMidResp = Invoke-Json -Method Get -Url "$baseUrl/api/v1/slugs/$targetAddress" -Headers $authHeader
if ($targetMidResp.StatusCode -ne 200) { Fail "target read after first source failed: $($targetMidResp.StatusCode)" }
$targetMidJson = $targetMidResp.Content | ConvertFrom-Json
if ($targetMidJson.data.status -eq 'Pass') {
  Fail "AndGate target should not pass until all preconditions are met."
}

$sourceTwoPassPayload = @{status='Pass'; comment='source-two-pass'}
$sourceTwoPassResp = Invoke-Json -Method Patch -Url "$baseUrl/api/v1/slugs/$sourceLocalAddress" -Body $sourceTwoPassPayload -Headers $authHeader
if ($sourceTwoPassResp.StatusCode -ne 200) { Fail "second source update failed: $($sourceTwoPassResp.StatusCode) body=$($sourceTwoPassResp.Content)" }
$targetFinalResp = Invoke-Json -Method Get -Url "$baseUrl/api/v1/slugs/$targetAddress" -Headers $authHeader
if ($targetFinalResp.StatusCode -ne 200) { Fail "target read after second source failed: $($targetFinalResp.StatusCode)" }
$targetFinalJson = $targetFinalResp.Content | ConvertFrom-Json
if ($targetFinalJson.data.status -ne 'Pass') {
  Fail "AndGate target should pass after all preconditions are met."
}
Write-Host "[smoke] cross-checklist AndGate relationship checks ok" -ForegroundColor Green

# Retrieve
$afterList = Invoke-Json -Method Get -Url "$baseUrl/api/v1/slugs?checklist=$checklistName" -Headers $authHeader
if ($afterList.StatusCode -ne 200) { Fail "list failed: $($afterList.StatusCode)" }
$afterJson = $afterList.Content | ConvertFrom-Json
if (-not $afterJson.data.items) { Fail 'list returned no items' }
Write-Host "[smoke] list ok" -ForegroundColor Green

# Report
$reportPayload = @{checklist=$checklistName; instance_id=$createJson.data.instance_id}
$reportResp = Invoke-Json -Method Post -Url "$baseUrl/api/v1/export/report" -Body $reportPayload -Headers $authHeader
if ($reportResp.StatusCode -ne 201) { Fail "report failed: $($reportResp.StatusCode)" }
$reportJson = $reportResp.Content | ConvertFrom-Json
if (-not $reportJson.data.path) { Fail 'report missing path' }
Write-Host "[smoke] report ok" -ForegroundColor Green

# Cleanup if possible
try {
  $delResp = Invoke-Json -Method Delete -Url "$baseUrl/api/v1/checklists/$checklistName" -Headers $authHeader
  if ($delResp.StatusCode -eq 200) { Write-Host "[smoke] cleanup deleted $checklistName" -ForegroundColor Green }
  $delLinkedResp = Invoke-Json -Method Delete -Url "$baseUrl/api/v1/checklists/$linkedChecklistName" -Headers $authHeader
  if ($delLinkedResp.StatusCode -eq 200) { Write-Host "[smoke] cleanup deleted $linkedChecklistName" -ForegroundColor Green }
} catch {
  Write-Warning "Cleanup failed: $_"
}

} catch {
  Write-Error $_
  exit 1
} finally {
  if ($script:startedByTest -and $script:serverExe -and $script:stateFile) {
    try {
      & $script:serverExe stop --state-file $script:stateFile *> $null
    } catch {
      Write-Warning "Failed to stop test-started server: $_"
      if ($script:serverPid) {
        try {
          Stop-Process -Id $script:serverPid -Force -ErrorAction SilentlyContinue
        } catch {
        }
      }
    }
  }
  if ($script:tempDir) {
    try {
      Remove-Item -LiteralPath $script:tempDir -Recurse -Force -ErrorAction SilentlyContinue
    } catch {
    }
  }
}

exit 0
