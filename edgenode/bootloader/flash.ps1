<#
    Build the Bootloader and program it through the existing EdgeNode ST-Link
    OpenOCD configuration.

    The first scaffold has no A/B jump implementation yet. It can still be
    programmed through SWD as an ordinary standalone project.
#>

$ErrorActionPreference = "Continue"

$project_path = Split-Path -Parent $MyInvocation.MyCommand.Path
$make_path = "D:\Ksoftware\mingw64\bin\mingw32-make.exe"
$openocd_path = "C:\Tools\openocd\bin\openocd.exe"
$openocd_script_path = "C:\Tools\openocd\share\openocd\scripts"
$openocd_config_path = Join-Path (Split-Path -Parent $project_path) "openocd.cfg"
$bin_path = Join-Path $project_path "build\bootloader.bin"

function script_fail([string]$reason)
{
    Write-Host "[失败] $reason" -ForegroundColor Red
    exit 1
}

Push-Location $project_path
try
{
    & $make_path -B all
    if($LASTEXITCODE -ne 0)
    {
        script_fail "Bootloader 编译失败，错误码：$LASTEXITCODE"
    }
}
finally
{
    Pop-Location
}

if((-not (Test-Path -LiteralPath $bin_path)) -or
   (-not (Test-Path -LiteralPath $openocd_path)) -or
   (-not (Test-Path -LiteralPath $openocd_script_path)) -or
   (-not (Test-Path -LiteralPath $openocd_config_path)))
{
    script_fail "缺少 BIN、OpenOCD 或 OpenOCD 配置。"
}

$bin_openocd_path = $bin_path -replace '\\', '/'
& $openocd_path -s $openocd_script_path -f $openocd_config_path -c "program `"$bin_openocd_path`" 0x08000000 verify reset exit"
if($LASTEXITCODE -ne 0)
{
    script_fail "Bootloader 烧录或校验失败，错误码：$LASTEXITCODE"
}

Write-Host "[成功] Bootloader 已写入并校验。" -ForegroundColor Green
