<#
    在PowerShell中运行：
    .\flash.ps1

    这个脚本会先强制重新编译，再通过ST-Link的SWD接口烧录。
    编译失败时不会继续烧录，避免把旧的ELF误认为是当前代码。
#>

# 不要用 "Stop"：make/gcc/openocd 会把日志和警告写到 stderr，
# PowerShell 5.1 在 Stop 下会把 stderr 当成终止性错误（NativeCommandError）抛出，
# 导致脚本在检查 $LASTEXITCODE 之前就崩掉。这里用 Continue，成败一律看 $LASTEXITCODE。
$ErrorActionPreference = "Continue"

$project_path = Split-Path -Parent $MyInvocation.MyCommand.Path
$make_path = "D:\Ksoftware\mingw64\bin\mingw32-make.exe"
$openocd_path = "C:\Tools\openocd\bin\openocd.exe"
$openocd_script_path = "C:\Tools\openocd\share\openocd\scripts"
$openocd_config_path = Join-Path $project_path "openocd.cfg"
$elf_path = Join-Path $project_path "build\edegnode.elf"

function script_fail([string]$reason)
{
    Write-Host "[失败] $reason" -ForegroundColor Red
    exit 1
}

function script_success([string]$message)
{
    Write-Host "[成功] $message" -ForegroundColor Green
}

Write-Host "========== EdgeNode 编译并烧录 ==========" -ForegroundColor Cyan

Write-Host "[步骤 1/4] 检查编译工具"
if(-not (Test-Path -LiteralPath $make_path))
{
    script_fail "没有找到 mingw32-make：$make_path"
}
script_success "已找到 mingw32-make"

Write-Host "[步骤 2/4] 强制重新编译 EdgeNode"
try
{
    Push-Location $project_path
    & $make_path -B all
    if($LASTEXITCODE -ne 0)
    {
        script_fail "编译命令返回错误码 $LASTEXITCODE。请根据上方 GCC 输出修正代码后再烧录。"
    }
}
finally
{
    Pop-Location
}

if(-not (Test-Path -LiteralPath $elf_path))
{
    script_fail "编译命令结束后没有生成 ELF：$elf_path"
}
script_success "编译完成，已生成 build\\edegnode.elf"

Write-Host "[步骤 3/4] 检查 OpenOCD 和烧录配置"
if(-not (Test-Path -LiteralPath $openocd_path))
{
    script_fail "没有找到 OpenOCD：$openocd_path"
}
if(-not (Test-Path -LiteralPath $openocd_script_path))
{
    script_fail "没有找到 OpenOCD 脚本目录：$openocd_script_path"
}
if(-not (Test-Path -LiteralPath $openocd_config_path))
{
    script_fail "没有找到项目烧录配置：$openocd_config_path"
}
script_success "已找到 OpenOCD、ST-Link SWD 配置和 ELF"

Write-Host "[步骤 4/4] 通过 ST-Link SWD 烧录并校验"
# OpenOCD 的 -c 参数由 Tcl 解析：反斜杠是转义符（\U \m \b... 会被吃掉），
# 所以传给 OpenOCD 的路径必须换成正斜杠，否则它打不开 ELF 文件。
$elf_openocd_path = $elf_path -replace '\\', '/'
$openocd_command = "program `"$elf_openocd_path`" verify reset exit"
$openocd_output = & $openocd_path -s $openocd_script_path -f $openocd_config_path -c $openocd_command 2>&1 |
    ForEach-Object { $_.ToString() }
$openocd_output | ForEach-Object { Write-Host $_ }

if($LASTEXITCODE -ne 0)
{
    $openocd_text = $openocd_output | Out-String

    if($openocd_text -match "stlink_usb_usb_open\(\): open failed|unable to find a matching ST-LINK|No ST-LINK detected")
    {
        script_fail "OpenOCD 没有找到 ST-Link。请检查 ST-Link 是否连接、是否被 Keil 或 VS Code 占用，以及 USB 驱动。"
    }

    if($openocd_text -match "target not halted|timed out while waiting for target halted|unable to halt")
    {
        script_fail "ST-Link 已启动，但无法连接或暂停 MCU。请检查 SWDIO、SWCLK、GND、3V3，以及目标板是否上电。"
    }

    script_fail "OpenOCD 烧录失败，返回错误码 $LASTEXITCODE。上方 OpenOCD 原始输出包含具体错误。"
}

script_success "OpenOCD 已完成程序写入、校验和复位。"
Write-Host "========== 烧录结束 ==========" -ForegroundColor Cyan
