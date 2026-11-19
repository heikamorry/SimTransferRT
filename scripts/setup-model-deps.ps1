param([ValidateSet('Release','Debug')][string] $Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$sdkRoot = Join-Path $projectRoot 'build\model-deps'
[IO.Directory]::CreateDirectory($sdkRoot) | Out-Null

function Get-CheckedPackage([string] $Name, [string] $Address, [string] $Checksum) {
    $destination = Join-Path $sdkRoot $Name
    if ((Test-Path -LiteralPath $destination -PathType Leaf) -and
        (Get-FileHash -LiteralPath $destination).Hash -eq $Checksum) { return $destination }
    $download = Join-Path $sdkRoot ([Guid]::NewGuid().ToString() + '.download')
    try {
        & curl.exe --fail --location --retry 3 --output $download $Address
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $Address" }
        if ((Get-FileHash -LiteralPath $download).Hash -ne $Checksum) { throw "SHA-256 mismatch: $Name" }
        if (Test-Path -LiteralPath $destination) { throw "An existing package has a different checksum: $destination" }
        [IO.File]::Move($download,$destination)
    } finally {
        if (Test-Path -LiteralPath $download) { Remove-Item -LiteralPath $download }
    }
    return $destination
}

$vtkPackage = Get-CheckedPackage 'official-9.4.2.tar.gz' 'https://vtk.org/files/release/9.4/VTK-9.4.2.tar.gz' '36c98e0da96bb12a30fe53708097aa9492e7b66d5c3b366e1c8dc251e2856a02'
$hdfPackage = Get-CheckedPackage 'hdf-sdk.zip' 'https://github.com/HDFGroup/hdf5/releases/download/hdf5_1.14.6/hdf5-1.14.6-win-vs2022_cl.zip' '976075012b8e395d3b89439f5d63b94481d0de79b69a0d17b99f587abf23100a'
if (!(Test-Path -LiteralPath (Join-Path $sdkRoot 'VTK-9.4.2\CMakeLists.txt'))) {
    & tar.exe -xf $vtkPackage -C $sdkRoot
    if ($LASTEXITCODE) { throw 'VTK extraction failed' }
}
$hdfRoot = Join-Path $sdkRoot 'hdf-sdk'
if (!(Test-Path -LiteralPath (Join-Path $hdfRoot 'HDF5-1.14.6-win64\cmake\hdf5-config.cmake'))) {
    [IO.Directory]::CreateDirectory($hdfRoot) | Out-Null
    & tar.exe -xf $hdfPackage -C $hdfRoot
    if ($LASTEXITCODE) { throw 'HDF5 package extraction failed' }
    & tar.exe -xf (Join-Path $hdfRoot 'hdf5\HDF5-1.14.6-win64.zip') -C $hdfRoot
    if ($LASTEXITCODE) { throw 'HDF5 SDK extraction failed' }
}
& (Join-Path $PSScriptRoot 'build-model-vtk.cmd') $Configuration
if ($LASTEXITCODE) { throw "VTK $Configuration build failed ($LASTEXITCODE)" }
