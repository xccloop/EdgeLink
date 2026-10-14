<#
    在PowerShell中运行：
    .\tools\compiled.ps1 -v7
    在 tools 目录也可运行：./compiled.ps1 -v7（7 为版本号）。

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

# 保留 -v7 这种紧凑写法；不使用参数绑定，直接检查脚本参数。
[uint32]$image_version = 0
if(($args.Count -notin @(1, 3)) -or ([string]$args[0] -notmatch '^-[vV]([0-9]+)$'))
{
    script_fail "用法：./compiled.ps1 -v7（7 为版本号）"
}
if((-not [uint32]::TryParse($Matches[1], [ref]$image_version)) -or ($image_version -eq 0))
{
    script_fail "版本号必须是 1 到 4294967295 的整数。"
}

# 默认使用仓库外的本机密钥；也可用环境变量或 -SigningKey 显式指定。
$signing_key_path = $env:EDGENODE_SIGNING_KEY
if([string]::IsNullOrWhiteSpace($signing_key_path))
{
    $signing_key_path = Join-Path $env:USERPROFILE ".edgelink\keys\ota-signing.pem"
}
if($args.Count -eq 3)
{
    if([string]$args[1] -ne "-SigningKey") { script_fail "第二个参数应为 -SigningKey" }
    $signing_key_path = [string]$args[2]
}
if(-not (Test-Path -LiteralPath $signing_key_path -PathType Leaf))
{
    script_fail "缺少OTA私钥：$signing_key_path；不会生成无签名镜像。"
}
$signing_key_path = (Resolve-Path -LiteralPath $signing_key_path).Path

Write-Host "========== EdgeNode 编译三种 APP 布局（V$image_version） ==========" -ForegroundColor Cyan

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
    & $make_path -B SLOT=all "IMAGE_VERSION=$image_version" "SIGNING_KEY=$signing_key_path" all
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
