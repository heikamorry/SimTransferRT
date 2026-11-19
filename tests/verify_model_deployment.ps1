param(
    [Parameter(Mandatory)][string] $Package,
    [Parameter(Mandatory)][string] $Model,
    [Parameter(Mandatory)][string] $Evidence
)
$ErrorActionPreference = 'Stop'
$packageDir = (Resolve-Path -LiteralPath $Package).Path
$modelPath = (Resolve-Path -LiteralPath $Model).Path
$evidenceDir = [IO.Directory]::CreateDirectory($Evidence).FullName
$work = [IO.Directory]::CreateDirectory((Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid()))).FullName
$exe = Join-Path $packageDir 'bin\SimTransfer.exe'
$screenshot = Join-Path $evidenceDir 'packaged-surface.png'
$loaded = @{}

function Test-Launch([string] $Arguments) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $exe
    $start.Arguments = $Arguments
    $start.WorkingDirectory = $work
    $start.UseShellExecute = $false
    foreach ($key in @($start.EnvironmentVariables.Keys)) {
        if ($key -match '^(QT_|QTDIR$|QML|VTK|HDF5|CMAKE_PREFIX_PATH$)') {
            $start.EnvironmentVariables.Remove($key)
        }
    }
    $start.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    $process = [Diagnostics.Process]::Start($start)
    try {
        $timer = [Diagnostics.Stopwatch]::StartNew()
        while (!$process.WaitForExit(50)) {
            if ($timer.Elapsed.TotalSeconds -gt 120) { throw "Timed out: $Arguments" }
            $process.Refresh()
            try { $modules = @($process.Modules) } catch {
                if ($process.HasExited) { break }
                throw
            }
            foreach ($module in $modules) {
                if ($module.ModuleName -match '^(Qt6|vtk|hdf5|qwindows)') {
                    $path = $module.FileName
                    if (!$path.StartsWith($packageDir + '\', [StringComparison]::OrdinalIgnoreCase)) {
                        throw "Dependency loaded outside the package: $path"
                    }
                    $loaded[$module.ModuleName] = $path.Substring($packageDir.Length + 1)
                }
            }
        }
        if ($process.ExitCode -ne 0) { throw "Exit code $($process.ExitCode): $Arguments" }
    } finally {
        if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}

try {
    Test-Launch ('--model "{0}" --screenshot "{1}"' -f $modelPath, $screenshot)
    foreach ($required in 'Qt6Core.dll','qwindows.dll','hdf5.dll','Qt6OpenGLWidgets.dll') {
        if (!$loaded.ContainsKey($required)) { throw "Did not observe $required while previewing" }
    }
    if (!($loaded.Keys -like 'vtkRenderingOpenGL2-*.dll')) { throw 'VTK OpenGL renderer missing' }
    if ((Get-Item -LiteralPath $screenshot).Length -eq 0) { throw 'Empty screenshot' }
    Test-Launch '--v1 --smoke-test'
    $json = [ordered]@{
        executableSHA256 = (Get-FileHash -LiteralPath $exe).Hash
        model = [IO.Path]::GetFileName($modelPath)
        searchPath = '%SystemRoot%\System32;%SystemRoot%'
        v1ExitCode = 0
        modelScreenshotExitCode = 0
        observedModelAndQtLibraries = @($loaded.Values | Sort-Object)
    } | ConvertTo-Json -Depth 3
    # JSON must not carry a BOM: strict readers reject the first byte.
    [IO.File]::WriteAllText((Join-Path $evidenceDir 'deployment.json'), $json,
        (New-Object Text.UTF8Encoding($false)))
} finally {
    # This directory was created empty for this run. Preserve any unexpected contents.
    if ([IO.Directory]::Exists($work) -and ![IO.Directory]::GetFileSystemEntries($work).Length) {
        [IO.Directory]::Delete($work)
    }
}
