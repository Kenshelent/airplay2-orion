# Deploy aarch64 AirPlay 2 payload to a Linux Yandex Station (Station 3 / Orion).
param(
  [string]$IP,
  [string]$Adb,
  [string]$Name,
  [string]$Payload,
  [switch]$Yes,
  [switch]$Uninstall
)
$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$remote = "/data/shairport"

function Find-Adb {
  if ($Adb -and (Test-Path $Adb)) { return $Adb }
  $c = Get-Command adb -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  throw "adb.exe not found"
}

$adb = Find-Adb
$dev = $null
if ($IP) {
  $dev = if ($IP -match ":") { $IP } else { "${IP}:5555" }
  & $adb connect $dev | Out-Null
} else {
  $list = & $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match "\sdevice$" }
  if (-not $list) { throw "No adb device. Pass -IP" }
  $dev = ($list | Select-Object -First 1) -replace "\s+device$", ""
}
function Sh([string]$cmd) {
  $r = & $adb -s $dev shell $cmd
  if ($null -eq $r) { return "" }
  return (@($r) -join "`n")
}

$arch = (Sh "uname -m").Trim()
if ($arch -ne "aarch64") { throw "Need aarch64, got '$arch'. The Mini 3 zip is armhf; this payload is 64-bit." }

if ($Uninstall) {
  Sh "$remote/airplay.sh uninstall"
  Write-Host "Autostart removed."
  return
}

if (-not $Yes) {
  Write-Host "Install AirPlay 2. Alice stays on boot and pauses only while a stream plays."
  $a = Read-Host "Continue? [y/N]"
  if ($a -notmatch "^[yY]") { return }
}

$dir = $Payload
if (-not $dir) { $dir = Join-Path $here "out" }
if (-not (Test-Path (Join-Path $dir "airplay.sh"))) {
  throw "No payload at $dir (need airplay.sh). Build with docker --target out first."
}

Sh "mkdir -p $remote"
Sh "[ -x $remote/airplay.sh ] && $remote/airplay.sh stop >/dev/null 2>&1; rm -rf $remote/ap2 $remote/airplayd $remote/sps-libs"
& $adb -s $dev push $dir/. $remote/
Sh "chmod +x $remote/airplay.sh $remote/airplayd $remote/ap2/shairport-sync-ap2 $remote/ap2/nqptp $remote/ap2/avahi-daemon"
if ($Name) { Sh "$remote/airplay.sh setup `"$Name`"" } else { Sh "$remote/airplay.sh setup" }
Sh "$remote/airplay.sh install"
Sh "$remote/airplay.sh start"
Write-Host (Sh "$remote/airplay.sh status")
