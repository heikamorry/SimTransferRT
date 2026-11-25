param(
    [switch]$Check,
    [string]$ClangFormat = 'clang-format'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$formatter = Get-Command $ClangFormat -CommandType Application -ErrorAction Stop

# Limit formatting to project-owned C++ sources; never traverse build/dependency trees.
$sourceFiles = @('src', 'tests', 'tools', 'installer') | ForEach-Object {
    Get-ChildItem -LiteralPath (Join-Path $projectRoot $_) -Recurse -File |
        Where-Object { $_.Extension -in '.cpp', '.h' } |
        Sort-Object FullName
}
$arguments = @('--style=file', '--fallback-style=none')
if ($Check) {
    $arguments += @('--dry-run', '--Werror')
} else {
    $arguments += '-i'
}

$failed = $false
foreach ($file in $sourceFiles) {
    & $formatter.Source @arguments $file.FullName
    if ($LASTEXITCODE -ne 0) {
        $failed = $true
    }
}
if ($failed) {
    throw 'C++ formatting failed. Run scripts/format.ps1 to apply the repository style.'
}
Write-Host "C++ formatting OK ($($sourceFiles.Count) files)."
