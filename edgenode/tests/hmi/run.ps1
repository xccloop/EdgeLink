$ErrorActionPreference = 'Stop'
$nodeRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$outputDir = Join-Path $nodeRoot 'build/hmi-tests'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$sources = @(
    (Join-Path $PSScriptRoot 'test_hmi.c'),
    (Join-Path $nodeRoot 'User/App/Presentation/Buffer/display_buffer.c'),
    (Join-Path $nodeRoot 'User/App/Presentation/Hmi/Widget/hmi_widget.c'),
    (Join-Path $nodeRoot 'User/App/Presentation/Hmi/Render/hmi_render.c'),
    (Join-Path $nodeRoot 'User/App/Presentation/Hmi/Page/hmi_page.c'),
    (Join-Path $nodeRoot 'User/App/Presentation/Hmi/Control/hmi_control.c')
)
$exe = Join-Path $outputDir 'test_hmi.exe'
& gcc -std=c11 -Wall -Wextra -Werror -I (Join-Path $nodeRoot 'User/App') -I (Join-Path $nodeRoot 'Drivers/BSP') @sources -o $exe
if ($LASTEXITCODE -ne 0) { throw 'HMI host compilation failed' }
Push-Location $outputDir
try {
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'HMI host tests failed' }
} finally { Pop-Location }
