$ErrorActionPreference = 'Stop'

[CmdletBinding()]
param(
  [ValidateSet("export", "restore")]
  [string]$Mode = "export",
  [string]$Host = "127.0.0.1",
  [int]$Port = 8080,
  [string]$Pack = "",
  [switch]$ResetDb
)

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$libraryRoot = if ($env:CHAX_CHECKLISTS_ROOT) { $env:CHAX_CHECKLISTS_ROOT } else { Join-Path $repoRoot "checklists" }
$effectivePack = if ($Pack) { $Pack } elseif ($env:CHAX_DEFAULT_PACK) { $env:CHAX_DEFAULT_PACK } else { "chax" }
$packRoot = Join-Path $libraryRoot $effectivePack
$baseUrl = "http://$Host`:$Port/api/v1"

function Ensure-Workspace {
  foreach ($dir in @($libraryRoot, $packRoot)) {
    if (-not (Test-Path $dir)) {
      New-Item -ItemType Directory -Path $dir | Out-Null
    }
  }
}

function Invoke-ChaxApi {
  param(
    [string]$Method,
    [string]$Path,
    $Body,
    [string]$ContentType = "application/json"
  )
  $uri = "$baseUrl$Path"
  $args = @{
    Uri         = $uri
    Method      = $Method
    ErrorAction = "Stop"
  }
  if ($PSBoundParameters.ContainsKey("Body")) {
    $args["Body"] = $Body
    $args["ContentType"] = $ContentType
  }
  $resp = Invoke-RestMethod @args
  if ($resp.ok -eq $false) {
    $code = $resp.error.code
    $msg = $resp.error.message
    throw "API $Method $Path failed: $code $msg"
  }
  if ($resp.PSObject.Properties.Name -contains "data") {
    return $resp.data
  }
  return $resp
}

function Export-Workspace {
  Write-Host "Exporting checklists to $packRoot"
  $names = @()
  $checklists = Invoke-ChaxApi -Method Get -Path "/checklists"
  if ($checklists.checklists) {
    $names = $checklists.checklists
  } elseif ($checklists.items) {
    $names = $checklists.items
  }
  foreach ($name in $names) {
    $enc = [Uri]::EscapeDataString($name)
    $checklistDir = Join-Path $packRoot $name
    $checklistFile = Join-Path $checklistDir "checklist.md"
    $checklistSaves = Join-Path $checklistDir "saves"
    if (-not (Test-Path $checklistDir)) {
      New-Item -ItemType Directory -Path $checklistDir | Out-Null
    }
    if (-not (Test-Path $checklistSaves)) {
      New-Item -ItemType Directory -Path $checklistSaves | Out-Null
    }
    Write-Host "  - $name (markdown)"
    $md = Invoke-RestMethod -Method Get -Uri "$baseUrl/export/markdown/$enc" -ErrorAction Stop
    Set-Content -Path $checklistFile -Value $md -Encoding UTF8

    Write-Host "  - $name (jsonl state)"
    $slugs = Invoke-ChaxApi -Method Get -Path "/slugs?checklist=$enc"
    $items = @()
    if ($slugs.items) {
      $items = $slugs.items
    } elseif ($slugs.slugs) {
      $items = $slugs.slugs
    }
    if ($items.Count -gt 0) {
      $lines = $items | ForEach-Object { $_ | ConvertTo-Json -Compress }
      $jsonlPath = Join-Path $checklistSaves "$name.jsonl"
      Set-Content -Path $jsonlPath -Value $lines -Encoding UTF8
    }
  }
}

function Restore-Workspace {
  if ($ResetDb) {
    Write-Host "Resetting .chax runtime store"
    Get-ChildItem -Path (Join-Path $repoRoot ".chax") -Filter "checklists.db*" -ErrorAction SilentlyContinue | ForEach-Object {
      Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
    }
  }

  $checklistDirs = Get-ChildItem -Path $packRoot -Directory -ErrorAction SilentlyContinue
  foreach ($dir in $checklistDirs) {
    $mdPath = Join-Path $dir.FullName "checklist.md"
    if (-not (Test-Path $mdPath)) {
      continue
    }
    $checklistName = $dir.Name
    $content = Get-Content -Raw -Path $mdPath
    $enc = [Uri]::EscapeDataString($checklistName)
    Write-Host "Importing template $checklistName"
    Invoke-ChaxApi -Method Post -Path "/import/markdown?checklist=$enc" -Body $content -ContentType "text/markdown"
  }

  $relationshipsSeen = New-Object System.Collections.Generic.HashSet[string]
  foreach ($dir in $checklistDirs) {
    $savesDir = Join-Path $dir.FullName "saves"
    if (-not (Test-Path $savesDir)) {
      continue
    }
    $stateFiles = Get-ChildItem -Path $savesDir -Filter "*.jsonl" -File -ErrorAction SilentlyContinue
    foreach ($file in $stateFiles) {
      Write-Host "Replaying state from $($file.FullName)"
      foreach ($line in Get-Content -Path $file.FullName) {
        if (-not $line.Trim()) { continue }
        $obj = $line | ConvertFrom-Json
        $address = $obj.address_id
        if (-not $address) { continue }

        $patch = @{}
        if ($null -ne $obj.result) { $patch["result"] = $obj.result }
        if ($obj.status) { $patch["status"] = $obj.status }
        if ($null -ne $obj.comment) { $patch["comment"] = $obj.comment }
        if ($patch.Count -gt 0) {
          $body = $patch | ConvertTo-Json -Compress
          Invoke-ChaxApi -Method Patch -Path "/slugs/$([Uri]::EscapeDataString($address))" -Body $body
        }

        if ($obj.relationships) {
          foreach ($rel in $obj.relationships) {
            $subject = $address
            $predicate = $rel.predicate
            $target = $rel.target
            if (-not $predicate -or -not $target) { continue }
            $key = "$subject|$predicate|$target"
            if ($relationshipsSeen.Contains($key)) { continue }
            $relationshipsSeen.Add($key) | Out-Null
            $relBody = @{ subject_address_id = $subject; predicate = $predicate; target_address_id = $target } | ConvertTo-Json -Compress
            Invoke-ChaxApi -Method Post -Path "/relationships/address" -Body $relBody
          }
        }
      }
    }
  }
}

Ensure-Workspace
switch ($Mode) {
  "export" { Export-Workspace }
  "restore" { Restore-Workspace }
}
