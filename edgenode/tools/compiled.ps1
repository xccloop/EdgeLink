<#
    在PowerShell中运行：
    .\tools\compiled.ps1

    只编译 EdgeNode APP，分别生成默认、A 槽、B 槽产物。
    不连接设备，也不烧录 Flash。
#>

# 不要用 "Stop"：make/gcc 会把日志和警告写到 stderr，
# PowerShell 5.1 在 Stop 下会把 stderr 当成终止性错误（NativeCommandError）抛出，
# 导致脚本在检查 $LASTEXITCODE 之前就崩掉。这里用 Continue，成败一律看 $LASTEXITCODE。
$ErrorActionPreference = "Continue"

$project_path = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$make_path = "D:\Ksoftware\mingw64\bin\mingw32-make.exe"

function script_fail([string]$reason)
{
    Write-Host "[失败] $reason" -ForegroundColor Red
    exit 1
}

function script_success([string]$message)
{
    Write-Host "[成功] $message" -ForegroundColor Green
}

Write-Host "========== EdgeNode 编译三种 APP 布局 ==========" -ForegroundColor Cyan

Write-Host "[步骤 1/3] 检查编译工具"
if(-not (Test-Path -LiteralPath $make_path))
{
    script_fail "没有找到 mingw32-make：$make_path"
}
script_success "已找到 mingw32-make"

Write-Host "[步骤 2/3] 强制重新编译默认、A 槽和 B 槽 APP"
try
{
    Push-Location $project_path
    & $make_path -B SLOT=all all
    if($LASTEXITCODE -ne 0)
    {
        script_fail "编译命令返回错误码 $LASTEXITCODE。请根据上方 GCC 输出检查。"
    }
}
finally
{
    Pop-Location
}

Write-Host "[步骤 3/3] 检查三套编译产物"
foreach($slot in @("default", "a", "b"))
{
    $output_dir = Join-Path $project_path "build\$slot"
    foreach($file in @("edegnode.elf", "edegnode.bin"))
    {
        $output_path = Join-Path $output_dir $file
        if(-not (Test-Path -LiteralPath $output_path))
        {
            script_fail "缺少编译产物：$output_path"
        }
    }
    if($slot -ne "default")
    {
        $image_path = Join-Path $output_dir "edegnode_image.bin"
        if(-not (Test-Path -LiteralPath $image_path))
        {
            script_fail "缺少升级镜像：$image_path"
        }
    }
    script_success "build\$slot 已生成"
}

Write-Host "========== 编译结束 ==========" -ForegroundColor Cyan
