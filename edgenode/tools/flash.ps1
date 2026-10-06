<#
    在 edgenode 目录运行：
    .\tools\flash.ps1 default
    .\tools\flash.ps1 a
    .\tools\flash.ps1 b

    只烧录已编译的产物；先运行 .\tools\compiled.ps1 生成三套镜像。
    defalut 作为 default 的兼容拼写也可使用。
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet("default", "defalut", "a", "b")]
    [string]$Slot
)

# PowerShell 5.1 可能把原生程序写入 stderr 的日志当作错误；以退出码判定结果。
$ErrorActionPreference = "Continue"

$project_path = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$tools_path = Split-Path -Parent $MyInvocation.MyCommand.Path
$openocd_path = "C:\Tools\openocd\bin\openocd.exe"
$openocd_script_path = "C:\Tools\openocd\share\openocd\scripts"
$openocd_config_path = Join-Path $project_path "openocd.cfg"

function script_fail([string]$reason)
{
    Write-Host "[失败] $reason" -ForegroundColor Red
    exit 1
}

if($Slot -eq "defalut")
{
    $Slot = "default"
}

switch($Slot)
{
    "default" {
        $bin_path = Join-Path $project_path "build\default\edegnode.bin"
        $address = 0x08000000
        $max_length = 256 * 1024
        $vector_offset = 0
    }
    "a" {
        $bin_path = Join-Path $project_path "build\a\edegnode_image.bin"
        $address = 0x08004000
        $max_length = 118 * 1024
        $vector_offset = 256
    }
    "b" {
        $bin_path = Join-Path $project_path "build\b\edegnode_image.bin"
        $address = 0x08021800
        $max_length = 118 * 1024
        $vector_offset = 256
    }
}

if(-not (Test-Path -LiteralPath $bin_path -PathType Leaf))
{
    script_fail "没有找到已编译镜像：$bin_path。请先运行 tools\compiled.ps1。"
}

try
{
    $bytes = [System.IO.File]::ReadAllBytes($bin_path)
}
catch
{
    script_fail "无法读取镜像：$bin_path"
}

if($bytes.Length -lt ($vector_offset + 8) -or $bytes.Length -gt $max_length)
{
    script_fail "镜像大小不符合 $Slot 布局：$($bytes.Length) 字节。"
}

if($Slot -ne "default")
{
    $magic = [System.BitConverter]::ToUInt32($bytes, 0)
    $header_size = [System.BitConverter]::ToUInt16($bytes, 6)
    $target_slot = [System.BitConverter]::ToUInt32($bytes, 16)
    if($magic -ne 0x3141544F -or $header_size -ne 256 -or $target_slot -ne $address)
    {
        script_fail "镜像头与 $Slot 槽地址不匹配，停止烧录。"
    }

    if(-not (Get-Command python -ErrorAction SilentlyContinue))
    {
        script_fail "没有找到 Python，无法校验 A/B 镜像完整性。"
    }
    & python (Join-Path $tools_path "verify_ota_image.py") --input $bin_path --slot $Slot
    if($LASTEXITCODE -ne 0)
    {
        script_fail "镜像完整性校验失败，停止烧录。"
    }
}
else
{
    Write-Warning "default 会从 0x08000000 烧录 APP，覆盖该地址上已有的 Bootloader。"
}

$reset_handler = [System.BitConverter]::ToUInt32($bytes, $vector_offset + 4)
$reset_address = $reset_handler -band 0xFFFFFFFE
if(($reset_handler -band 1) -eq 0 -or
   $reset_address -lt ($address + $vector_offset) -or
   $reset_address -ge ($address + $bytes.Length))
{
    script_fail "Reset_Handler 不在 $Slot 镜像范围内，停止烧录。"
}

if(-not (Test-Path -LiteralPath $openocd_path -PathType Leaf))
{
    script_fail "没有找到 OpenOCD：$openocd_path"
}
if(-not (Test-Path -LiteralPath $openocd_script_path -PathType Container))
{
    script_fail "没有找到 OpenOCD 脚本目录：$openocd_script_path"
}
if(-not (Test-Path -LiteralPath $openocd_config_path -PathType Leaf))
{
    script_fail "没有找到 OpenOCD 配置：$openocd_config_path"
}

$bin_openocd_path = $bin_path -replace '\\', '/'
$address_text = '0x{0:X8}' -f $address
$openocd_command = "program `"$bin_openocd_path`" $address_text verify reset exit"
Write-Host "烧录 $Slot：$bin_path → $address_text" -ForegroundColor Cyan
if(-not $PSCmdlet.ShouldProcess("$bin_path → $address_text", "OpenOCD 烧录并校验"))
{
    return
}
& $openocd_path -s $openocd_script_path -f $openocd_config_path -c $openocd_command
if($LASTEXITCODE -ne 0)
{
    script_fail "OpenOCD 烧录或校验失败，错误码：$LASTEXITCODE"
}

Write-Host "[成功] $Slot 镜像已烧录、校验并复位。" -ForegroundColor Green
