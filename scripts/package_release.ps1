[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [ValidateNotNullOrEmpty()]
  [string]$Version,

  [string]$BuildRoot = "build",
  [string]$ReleaseRoot = "release",

  [switch]$IncludeVui
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:WebFiles = @(
  "about.html",
  "checklist_assistant.html",
  "checklist_prototype_common.js",
  "cvmewt-logo_0.1.7.svg",
  "import_export.js",
  "index.html",
  "oauth_callback.html",
  "portal_help.html",
  "portal_settings.html",
  "save_controller.js",
  "theme.css"
)
$script:RootRuntimeFiles = @(
  "checklist_assistant_server.exe",
  "checklist_assistant.exe",
  "libgcc_s_seh-1.dll",
  "libstdc++-6.dll",
  "libwinpthread-1.dll"
)
$script:VuiFiles = @(
  "index.html",
  "vui.css",
  "vui.js",
  "vui_config.js"
)

function Write-Utf8NoBomFile {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$Content
  )

  $encoding = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Assert-PathExists {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [Parameter(Mandatory = $true)]
    [string]$Label
  )

  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "$Label not found: $Path"
  }
}

function Copy-RepoFile {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RelativePath,

    [Parameter(Mandatory = $true)]
    [string]$ReleasePath
  )

  $source = Join-Path $script:RepoRoot $RelativePath
  Assert-PathExists -Path $source -Label $RelativePath
  $destination = Join-Path $ReleasePath $RelativePath
  $destinationDir = Split-Path -Parent $destination
  if (-not (Test-Path -LiteralPath $destinationDir)) {
    New-Item -ItemType Directory -Path $destinationDir -Force | Out-Null
  }
  Copy-Item -LiteralPath $source -Destination $destination -Force
}

function Copy-TrackedTree {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Prefix,

    [Parameter(Mandatory = $true)]
    [string]$ReleasePath
  )

  $trackedFiles = @(& git -C $script:RepoRoot ls-files -- "$Prefix/**")
  if ($LASTEXITCODE -ne 0) {
    throw "git ls-files failed for prefix '$Prefix'."
  }

  foreach ($relativePath in $trackedFiles) {
    if ([string]::IsNullOrWhiteSpace($relativePath)) {
      continue
    }
    $normalizedPath = $relativePath -replace "/", [System.IO.Path]::DirectorySeparatorChar
    $source = Join-Path $script:RepoRoot $normalizedPath
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
      continue
    }
    $destination = Join-Path $ReleasePath $normalizedPath
    $destinationDir = Split-Path -Parent $destination
    if (-not (Test-Path -LiteralPath $destinationDir)) {
      New-Item -ItemType Directory -Path $destinationDir -Force | Out-Null
    }
    Copy-Item -LiteralPath $source -Destination $destination -Force
  }

  return ,$trackedFiles
}

function Disable-VuiReferences {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ReleasePath
  )

  $webRoot = Join-Path $ReleasePath "CHAX-CLIENT\web"
  $indexPath = Join-Path $webRoot "index.html"
  $checklistPath = Join-Path $webRoot "checklist_assistant.html"

  $indexContent = Get-Content -LiteralPath $indexPath -Raw
  $indexContent = [System.Text.RegularExpressions.Regex]::Replace(
    $indexContent,
    '(?s)\s*<div class="tile" id="vuiTile">.*?</div>',
    ""
  )
  $indexContent = $indexContent.Replace(
    '<script>',
    "<script>`r`n    window.CHAX_CONFIG = Object.assign({}, window.CHAX_CONFIG, { enableVui: false });"
  )
  Write-Utf8NoBomFile -Path $indexPath -Content $indexContent

  $checklistContent = Get-Content -LiteralPath $checklistPath -Raw
  $checklistContent = [System.Text.RegularExpressions.Regex]::Replace(
    $checklistContent,
    '\s*<span class="pill"><a href="/vui/">VUI</a></span>',
    ""
  )
  Write-Utf8NoBomFile -Path $checklistPath -Content $checklistContent
}

function Build-ManifestContent {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ManifestVersion,

    [Parameter(Mandatory = $true)]
    [bool]$BundledVui
  )

  $lines = @(
    "# Minimal Operational Release $ManifestVersion",
    "",
    "Database policy: no DB shipped. On first run the server creates .chax/checklists.db (with demo seed unless disabled by env). Removing .chax wipes runtime data.",
    "",
    "Root:",
    "- checklist_assistant_server.exe - server binary; required to run API/UI/OAuth.",
    "- checklist_assistant.exe - turnkey launcher for double-click local use; starts the managed server and opens /ui/.",
    "- libgcc_s_seh-1.dll - MinGW runtime; both executables fail to start on a stock Windows host without it.",
    "- libstdc++-6.dll - MinGW runtime; both executables fail to start on a stock Windows host without it.",
    "- libwinpthread-1.dll - MinGW runtime; both executables fail to start on a stock Windows host without it.",
    "",
    "CHAX-CLIENT/web (static UI/OAuth assets):",
    "- index.html - home/landing page and OAuth launch; missing breaks the root redirect UI.",
    "- checklist_assistant.html - main checklist UI; missing breaks the primary interface.",
    "- checklist_prototype_common.js - shared UI/OAuth/API logic; missing breaks login and data flow.",
    "- import_export.js - JSONL import/export helpers; missing disables import/export controls.",
    "- save_controller.js - client save handlers; missing breaks save operations.",
    "- oauth_callback.html - OAuth code exchange; missing breaks OAuth login.",
    "- theme.css - shared styling; missing degrades layout and readability.",
    "- cvmewt-logo_0.1.7.svg - brand asset; missing causes broken images on home/about.",
    "- about.html - about page linked from home; missing yields a broken link.",
    "- portal_settings.html - settings page linked from UI; missing yields a broken link.",
    "- portal_help.html - help page linked from UI; missing yields a broken link.",
    ""
  )

  if ($BundledVui) {
    $lines += @(
      "CHAX-CLIENT/vui (optional UI only):",
      "- index.html, vui.css, vui.js, vui_config.js - optional VUI front end pages.",
      "- Whisper backend binaries/models are intentionally not bundled in this release.",
      "- Configure a compatible whisper server/model externally before enabling voice entry points.",
      ""
    )
  } else {
    $lines += @(
      "CHAX-CLIENT/vui:",
      "- Not bundled in this release.",
      "- scripts/package_release.ps1 removed the VUI landing-page tile and UI nav link in the packaged web copy.",
      "- Whisper backend binaries/models are intentionally not bundled in this release.",
      ""
    )
  }

  $lines += @(
    "checklists:",
    "- checklists/** - git-tracked checklist templates and supporting assets from the current worktree, copied without untracked runtime leftovers such as local logs/, reports/, or customer-only scratch files."
  )

  return ($lines -join "`r`n")
}

$buildRootPath = Join-Path $script:RepoRoot $BuildRoot
$releaseBasePath = Join-Path $script:RepoRoot $ReleaseRoot
$releasePath = Join-Path $releaseBasePath $Version

if (-not (Test-Path -LiteralPath $buildRootPath -PathType Container)) {
  throw "Build root not found: $buildRootPath"
}

if (Test-Path -LiteralPath $releasePath) {
  Remove-Item -LiteralPath $releasePath -Recurse -Force
}

New-Item -ItemType Directory -Path $releasePath | Out-Null
New-Item -ItemType Directory -Path (Join-Path $releasePath "CHAX-CLIENT\web") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $releasePath "checklists") -Force | Out-Null

foreach ($fileName in $script:RootRuntimeFiles) {
  $source = Join-Path $buildRootPath $fileName
  Assert-PathExists -Path $source -Label $fileName
  Copy-Item -LiteralPath $source -Destination $releasePath -Force
}

foreach ($fileName in $script:WebFiles) {
  Copy-RepoFile -RelativePath (Join-Path "CHAX-CLIENT\web" $fileName) -ReleasePath $releasePath
}

if ($IncludeVui) {
  foreach ($fileName in $script:VuiFiles) {
    Copy-RepoFile -RelativePath (Join-Path "CHAX-CLIENT\vui" $fileName) -ReleasePath $releasePath
  }
} else {
  Disable-VuiReferences -ReleasePath $releasePath
}

$trackedChecklistFiles = Copy-TrackedTree -Prefix "checklists" -ReleasePath $releasePath
$manifestPath = Join-Path $releasePath "MANIFEST.md"
$manifestContent = Build-ManifestContent -ManifestVersion $Version -BundledVui:$IncludeVui
Write-Utf8NoBomFile -Path $manifestPath -Content $manifestContent

Write-Host "Packaged release at $releasePath"
Write-Host "Tracked checklist files copied: $($trackedChecklistFiles.Count)"
Write-Host "VUI bundled: $($IncludeVui.IsPresent)"
