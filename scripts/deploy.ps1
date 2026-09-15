# deploy.ps1 — 拷贝编译产物到 TrafficMonitor 的 plugins 目录并提示重载。
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
Copy-Item $src $dstDir -Force
Write-Host "已部署：$dstDir\WorkBuddy2ApiPlugin.dll"
Write-Host "如 TrafficMonitor 正在运行：托盘右键 → 其他功能 → 插件管理 → 重新加载，或重启 TrafficMonitor。"
