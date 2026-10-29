param(
    [ValidateSet('windows-vs2026','windows-vs2022')][string]$Preset='windows-vs2026',
    [ValidateSet('Release','Debug')][string]$Configuration='Release'
)
$ErrorActionPreference='Stop'
$ambibinRoot=Split-Path -Parent $PSScriptRoot
$cmakeCommand=Get-Command cmake -ErrorAction SilentlyContinue
$cmakeBinary=if($cmakeCommand){$cmakeCommand.Source}else{'C:\Program Files\CMake\bin\cmake.exe'}
if(-not (Test-Path -LiteralPath $cmakeBinary)){throw 'Install CMake and make it available on PATH.'}
Push-Location $ambibinRoot
try {
    & $cmakeBinary --preset $Preset
    if($LASTEXITCODE -ne 0){throw 'CMake configuration failed.'}
    $ambibinBuild=Join-Path $ambibinRoot "build/$Preset"
    & $cmakeBinary --build $ambibinBuild --config $Configuration --parallel 4
    if($LASTEXITCODE -ne 0){throw 'Build failed.'}
} finally { Pop-Location }
