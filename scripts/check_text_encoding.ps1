param(
  [switch]$Fix
)

$ErrorActionPreference = "Stop"

function Get-TrackedFiles {
  $null = git rev-parse --is-inside-work-tree 2>$null
  if ($LASTEXITCODE -eq 0) {
    return @(git ls-files)
  }
  return @(Get-ChildItem -Recurse -File | ForEach-Object { $_.FullName })
}

function Starts-WithBytes {
  param(
    [byte[]]$Bytes,
    [byte[]]$Prefix
  )
  if ($Bytes.Length -lt $Prefix.Length) { return $false }
  for ($i = 0; $i -lt $Prefix.Length; $i++) {
    if ($Bytes[$i] -ne $Prefix[$i]) { return $false }
  }
  return $true
}

$repoRoot = (Get-Location).Path
$utf8Strict = New-Object System.Text.UTF8Encoding($false, $true)
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

$binaryExtensions = @(
  ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".webp", ".avif",
  ".pdf", ".zip", ".gz", ".7z", ".tar", ".rar",
  ".exe", ".dll", ".so", ".dylib", ".lib", ".a", ".o", ".obj", ".pdb",
  ".db", ".sqlite", ".sqlite3",
  ".fdf",
  ".mp3", ".wav", ".flac", ".ogg", ".mp4", ".mkv", ".webm",
  ".woff", ".woff2", ".ttf", ".otf",
  ".pptx", ".docx", ".xlsx",
  ".bin", ".glb"
)

$files = Get-TrackedFiles
$issues = New-Object System.Collections.Generic.List[object]
$fixed = 0
$checked = 0

foreach ($relativePath in $files) {
  if ([string]::IsNullOrWhiteSpace($relativePath)) { continue }
  $path = if ([System.IO.Path]::IsPathRooted($relativePath)) { $relativePath } else { Join-Path $repoRoot $relativePath }
  if (-not (Test-Path $path -PathType Leaf)) { continue }

  $ext = [System.IO.Path]::GetExtension($path).ToLowerInvariant()
  if ($binaryExtensions -contains $ext) { continue }

  $bytes = [System.IO.File]::ReadAllBytes($path)
  if ($bytes.Length -eq 0) {
    $checked++
    continue
  }

  $isUtf8Bom = Starts-WithBytes -Bytes $bytes -Prefix ([byte[]](0xEF, 0xBB, 0xBF))
  $isUtf16LeBom = Starts-WithBytes -Bytes $bytes -Prefix ([byte[]](0xFF, 0xFE))
  $isUtf16BeBom = Starts-WithBytes -Bytes $bytes -Prefix ([byte[]](0xFE, 0xFF))
  $isUtf32LeBom = Starts-WithBytes -Bytes $bytes -Prefix ([byte[]](0xFF, 0xFE, 0x00, 0x00))
  $isUtf32BeBom = Starts-WithBytes -Bytes $bytes -Prefix ([byte[]](0x00, 0x00, 0xFE, 0xFF))

  if (-not $isUtf8Bom -and -not $isUtf16LeBom -and -not $isUtf16BeBom -and -not $isUtf32LeBom -and -not $isUtf32BeBom) {
    if ($bytes -contains 0) {
      continue
    }
  }

  $checked++

  if ($isUtf8Bom) {
    if ($Fix) {
      if ($bytes.Length -gt 3) {
        $stripped = New-Object byte[] ($bytes.Length - 3)
        [Array]::Copy($bytes, 3, $stripped, 0, $stripped.Length)
        [System.IO.File]::WriteAllBytes($path, $stripped)
      } else {
        [System.IO.File]::WriteAllBytes($path, @())
      }
      $fixed++
      continue
    }
    $issues.Add([PSCustomObject]@{ Path = $relativePath; Issue = "UTF-8 BOM present" })
    continue
  }

  if ($isUtf16LeBom -or $isUtf16BeBom -or $isUtf32LeBom -or $isUtf32BeBom) {
    if ($Fix) {
      $text = [System.IO.File]::ReadAllText($path)
      [System.IO.File]::WriteAllText($path, $text, $utf8NoBom)
      $fixed++
      continue
    }
    $issues.Add([PSCustomObject]@{ Path = $relativePath; Issue = "UTF-16/UTF-32 BOM detected (not UTF-8)" })
    continue
  }

  try {
    [void]$utf8Strict.GetString($bytes)
  } catch {
    $issues.Add([PSCustomObject]@{ Path = $relativePath; Issue = "Invalid UTF-8 byte sequence" })
  }
}

if ($issues.Count -gt 0) {
  Write-Host "Encoding check failed ($($issues.Count) issue(s), $checked text file(s) checked):" -ForegroundColor Red
  foreach ($issue in $issues) {
    Write-Host " - $($issue.Path): $($issue.Issue)"
  }
  exit 1
}

Write-Host "Encoding check passed ($checked text file(s) checked)." -ForegroundColor Green
if ($Fix) {
  Write-Host "Auto-fixed files: $fixed"
}
