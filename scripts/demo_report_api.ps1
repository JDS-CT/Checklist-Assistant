Param(
  [string]$BaseUrl = "http://127.0.0.1:8080",
  [string]$ChecklistPrefix = "demo-report",
  [switch]$Clean
)

$ErrorActionPreference = "Stop"

function New-RandomSuffix {
  return [System.DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds().ToString()
}

function Invoke-Json {
  param(
    [Parameter(Mandatory = $true)][ValidateSet("GET", "POST", "PATCH")] [string]$Method,
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $false)][object]$Body
  )

  $uri = "$BaseUrl$Path"
  $jsonBody = $null
  if ($PSBoundParameters.ContainsKey("Body")) {
    $jsonBody = $Body | ConvertTo-Json -Depth 6
  }
  return Invoke-RestMethod -Method $Method -Uri $uri -Body $jsonBody -ContentType "application/json"
}

$suffix = New-RandomSuffix
$checklist = "$ChecklistPrefix-$suffix"
$instancePrincipal = "instance||$checklist"

Write-Host "Using checklist '$checklist' with instance principal '$instancePrincipal' against $BaseUrl"

$rows = @(
  @{ section = "Alpha"; procedure = "Inspect & Clean"; spec = "Spec_1"; result = "Ready"; status = "Pass"; comment = "Keep tidy" },
  @{ section = "Beta"; procedure = "Tighten"; spec = "Spec B"; result = "Tight %1"; status = "Fail"; comment = "" },
  @{ section = "Gamma"; procedure = "Skip NA"; spec = "Spec G"; result = "None"; status = "NA"; comment = "Skip row" }
)

$instanceId = $null
foreach ($row in $rows) {
  $payload = @{
    checklist = $checklist
    section = $row.section
    procedure = $row.procedure
    action = $row.procedure
    spec = $row.spec
    instructions = "Auto generated demo row"
    instance_principal = $instancePrincipal
    result = $row.result
    status = $row.status
    comment = $row.comment
  }

  $resp = Invoke-Json -Method POST -Path "/api/v1/slugs" -Body $payload
  if (-not $resp.ok) {
    throw "Slug creation failed: $($resp | ConvertTo-Json -Depth 5)"
  }
  if (-not $instanceId) {
    $instanceId = $resp.data.instance_id
  }
  Write-Host ("Created slug {0} -> address_id={1}" -f $row.procedure, $resp.data.address_id)
}

if (-not $instanceId) {
  throw "No instance_id returned from slug creation."
}

$instanceId = $instanceId.Trim()
Write-Host ("Instance ID for report export: {0}" -f $instanceId)

$reportPayload = @{ checklist = $checklist; instance_id = $instanceId }
$reportResp = Invoke-Json -Method POST -Path "/api/v1/export/report" -Body $reportPayload
if (-not $reportResp.ok) {
  throw "Report export failed: $($reportResp | ConvertTo-Json -Depth 5)"
}

Write-Host "Report written to: $($reportResp.data.path)"
Write-Host "Report directory:  $($reportResp.data.directory)"
if ($reportResp.data.fillable) {
  $fillable = $reportResp.data.fillable
  Write-Host "Fillable directory: $($fillable.directory)"
  Write-Host "Fillable FDF path: $($fillable.fdf_path)"
  Write-Host "Fillable JSONL:    $($fillable.jsonl_path)"
  if ($fillable.pdf_copy_path) {
    Write-Host "Fillable PDF copy: $($fillable.pdf_copy_path)"
  }
}

if ($Clean) {
  if (Test-Path $reportResp.data.directory) {
    Remove-Item -Recurse -Force $reportResp.data.directory
    Write-Host "Cleaned report directory."
  }
} else {
  Write-Host "Report retained on disk for inspection."
}
