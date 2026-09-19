# deploy.ps1 — 拷贝编译产物到 TrafficMonitor 的 plugins 目录（改名式替换，TM 运行中也可用）。
param(
    [string]$TMDir = 'E:\软件\TrafficMonitor',
    [ValidateSet('Release', 'Debug')]
    [string]$Cfg = 'Release'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "build\out\$Cfg\WorkBuddy2ApiPlugin.dll"
if (-not (Test-Path $src)) { throw "未找到 $src —— 先运行 build\build.ps1" }
$dstDir = Join-Path $TMDir 'plugins'
if (-not (Test-Path $dstDir)) { throw "TrafficMonitor 目录不存在：$TMDir（用 -TMDir 指定正确路径）" }
$dst = Join-Path $dstDir 'WorkBuddy2ApiPlugin.dll'
if (Test-Path $dst) {
    # TM 运行中时 DLL 映像被映射，无法覆盖（报 err 32 共享冲突）；同卷改名不受映射限制，
    # 先把旧文件改名挪走再拷入新文件，两种情况都走得通。
    $bak = Join-Path $dstDir ("WorkBuddy2ApiPlugin.dll.old-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Move-Item $dst $bak -Force
    Write-Host "旧 DLL 已改名保留：$(Split-Path -Leaf $bak)"
}
Copy-Item $src $dst -Force
Write-Host "已部署：$dst"
Write-Host "注意：必须重启 TrafficMonitor 才会真正加载新代码；"
Write-Host "「插件管理→重新加载」不生效（worker 线程钉住模块，重载后跑的仍是旧实例）。"
