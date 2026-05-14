param(
    [string]$InputPath = "docs/research/feature_compare_matrix.md",
    [string]$OutputPath = "docs/research/feature_compare_matrix_table.md",
    [switch]$Stdout
)

$provided = [char]0x2705
$partial = [char]0x2714
$notProvided = [char]0x274C

$featureRe = '^# Feature\s*(\d+)\s*[–-]?\s*(.*)$'
$vendorRe = '^##\s+(.+?)\s*$'
$statusRe = '^\*\*(Provided|Partially Provided|Not Provided|Not Applicable)\b'

$features = @{}
$vendors = New-Object System.Collections.Generic.List[string]
$currentFeature = $null
$currentVendor = $null

foreach ($line in Get-Content -Path $InputPath) {
    if ($line -match $featureRe) {
        $number = [int]$Matches[1]
        $title = $Matches[2].Trim()
        $features[$number] = @{
            title = $title
            vendors = @{}
        }
        $currentFeature = $number
        $currentVendor = $null
        continue
    }

    if ($null -eq $currentFeature) {
        continue
    }

    if ($line -match $vendorRe) {
        $currentVendor = $Matches[1].Trim()
        if (-not $vendors.Contains($currentVendor)) {
            $vendors.Add($currentVendor) | Out-Null
        }
        continue
    }

    if ($null -eq $currentVendor) {
        continue
    }

    $vendorMap = $features[$currentFeature].vendors
    if ($vendorMap.ContainsKey($currentVendor)) {
        continue
    }

    if ($line.Trim() -match $statusRe) {
        $status = $Matches[1]
        switch ($status) {
            "Provided" { $vendorMap[$currentVendor] = $provided }
            "Partially Provided" { $vendorMap[$currentVendor] = $partial }
            "Not Provided" { $vendorMap[$currentVendor] = $notProvided }
            "Not Applicable" { $vendorMap[$currentVendor] = $notProvided }
        }
    }
}

$header = @("Feature") + $vendors
$lines = @()
$lines += "| " + ($header -join " | ") + " |"
$lines += "| " + (($header | ForEach-Object { "---" }) -join " | ") + " |"

for ($i = 1; $i -le 22; $i++) {
    $row = @("Feature $i")
    $vendorMap = $null
    if ($features.ContainsKey($i)) {
        $vendorMap = $features[$i].vendors
    }
    foreach ($vendor in $vendors) {
        if ($null -ne $vendorMap -and $vendorMap.ContainsKey($vendor)) {
            $row += $vendorMap[$vendor]
        }
        else {
            $row += ""
        }
    }
    $lines += "| " + ($row -join " | ") + " |"
}

$legend = "Legend: $provided provided, $partial partial, $notProvided not provided."
$outputText = $legend + "`n`n" + ($lines -join "`n") + "`n"

if ($Stdout) {
    Write-Output $outputText
}
else {
    Set-Content -Path $OutputPath -Value $outputText -Encoding UTF8
}
