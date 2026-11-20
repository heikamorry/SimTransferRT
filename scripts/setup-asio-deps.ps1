param(
    [string] $CacheDir = (Join-Path $env:LOCALAPPDATA 'SimTransferRT/deps'),
    [string] $ArchivePath
)
$ErrorActionPreference = 'Stop'
$archiveName = 'boost_1_88_0.tar.bz2'
$url = "https://archives.boost.io/release/1.88.0/source/$archiveName"
$sha = '46d9d2c06637b219270877c9e16155cbd015b6dc84349af064c088e9b5b12f7b'
$cache = [IO.Directory]::CreateDirectory($CacheDir).FullName
$root = Join-Path $cache 'boost_1_88_0'
$marker = Join-Path $root '.archive-sha256'
if ((Test-Path -LiteralPath $marker -PathType Leaf) -and
    ([IO.File]::ReadAllText($marker) -eq $sha) -and
    (Test-Path -LiteralPath (Join-Path $root 'boost/asio/version.hpp')) -and
    (Test-Path -LiteralPath (Join-Path $root 'boost/version.hpp'))) {
    Write-Output "Boost 1.88.0 / Asio 1.34.2: $root"
    return
}
if (Test-Path -LiteralPath $root) { throw "Incomplete or unverified installation at $root; move it aside before retrying." }

$stage = [IO.Directory]::CreateDirectory((Join-Path $cache ([Guid]::NewGuid().ToString()))).FullName
try {
    $archive = Join-Path $cache $archiveName
    if ($ArchivePath) { $archive = (Resolve-Path -LiteralPath $ArchivePath).Path }
    $download = -not (Test-Path -LiteralPath $archive -PathType Leaf)
    if ($download) {
        $archive = Join-Path $stage $archiveName
        & curl.exe --fail --location --proto '=https' --proto-redir '=https' --retry 2 --output $archive $url
        if ($LASTEXITCODE) { throw "Download failed: $url" }
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $sha) { throw "SHA-256 mismatch: $archive" }
    if ($download) {
        [IO.File]::Move($archive, (Join-Path $cache $archiveName))
        $archive = Join-Path $cache $archiveName
    }
    & tar.exe -xf $archive -C $stage boost_1_88_0/boost boost_1_88_0/LICENSE_1_0.txt
    if ($LASTEXITCODE) { throw "Boost header extraction failed: $archive" }
    $headers = Join-Path $stage 'boost_1_88_0'
    [IO.File]::WriteAllText((Join-Path $headers '.archive-sha256'), $sha)
    [IO.Directory]::Move($headers, $root)
} finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -Confirm:$false
}
Write-Output "Boost 1.88.0 / Asio 1.34.2: $root"
