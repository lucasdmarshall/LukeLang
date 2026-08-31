#requires -Version 5.0
<#
.SYNOPSIS
  mimo — inject LukeLang onto Windows.

.EXAMPLE
  irm https://lukelang.org/mimo.ps1 | iex
  mimo inject lukelang
#>

$ErrorActionPreference = "Stop"
$MimoVersion = "0.3.0"
$MimoHome = if ($env:MIMO_HOME) { $env:MIMO_HOME } else { Join-Path $HOME ".mimo" }
$MimoDist = if ($env:MIMO_DIST) { $env:MIMO_DIST } else { "https://lukelang.org/dist/mimo" }
$MimoBin = Join-Path $MimoHome "bin"
$MimoToolchains = Join-Path $MimoHome "toolchains"
$MimoCache = Join-Path $MimoHome "cache"

function Write-Mimo($msg) { Write-Host "mimo: $msg" }

function Get-MimoTarget {
  $arch = $env:PROCESSOR_ARCHITECTURE
  switch -Regex ($arch) {
    '^(AMD64|X86_64)$' { return "windows-x86_64" }
    '^(x86|X86)$'      { return "windows-i686" }
    '^(ARM64)$'        { return "windows-arm64" }
    default            { throw "unsupported Windows arch: $arch" }
  }
}

function Ensure-Dirs {
  New-Item -ItemType Directory -Force -Path $MimoBin, $MimoToolchains, $MimoCache | Out-Null
}

function Download-File([string]$Url, [string]$Dest) {
  Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
}

function Get-Sha256([string]$Path) {
  (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
}

function Install-Self {
  Ensure-Dirs
  $dest = Join-Path $MimoBin "mimo.ps1"
  $wrapper = Join-Path $MimoBin "mimo.cmd"
  # Prefer re-fetching canonical copy so curl-less installs stay current.
  Download-File "$MimoDist/mimo.ps1" $dest
  @"
@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mimo.ps1" %*
"@ | Set-Content -Encoding ASCII -Path $wrapper
  Write-Mimo "installed mimo $MimoVersion → $dest"
}

function Get-Channel([string]$Channel = "stable") {
  $dest = Join-Path $MimoCache "$Channel.json"
  Download-File "$MimoDist/channel/$Channel.json" $dest
  return (Get-Content -Raw $dest | ConvertFrom-Json)
}

function Inject-LukeLang([string]$Spec = "lukelang") {
  Ensure-Dirs
  $name = $Spec
  $want = "latest"
  if ($Spec -match "^(?<n>[^@]+)@(?<v>.+)$") {
    $name = $Matches.n
    $want = $Matches.v
  }
  if ($name -notin @("lukelang", "luke")) { throw "unknown tool '$name' (try: lukelang)" }

  $manifest = Get-Channel "stable"
  $target = Get-MimoTarget
  $version = if ($want -eq "latest") { $manifest.version } else { $want }

  $sha = $null
  if ($want -eq "latest") {
    if (-not ($manifest.targets.PSObject.Properties.Name -contains $target)) {
      throw "no $target build in channel yet — see https://lukelang.org/download/"
    }
    $sha = $manifest.targets.$target.sha256
  }

  $url = "$MimoDist/lukelang/$version/luke-$target.zip"
  $archive = Join-Path $MimoCache "luke-$target-$version.zip"
  Write-Mimo "inject lukelang@$version ($target)"
  Download-File $url $archive
  if ($sha) {
    $got = Get-Sha256 $archive
    if ($got -ne $sha.ToLowerInvariant()) { throw "checksum mismatch: $got != $sha" }
    Write-Mimo "sha256 ok"
  }

  $root = Join-Path $MimoToolchains "lukelang-$version"
  if (Test-Path $root) { Remove-Item -Recurse -Force $root }
  New-Item -ItemType Directory -Force -Path $root | Out-Null
  $stage = Join-Path $MimoCache "extract-$PID"
  if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
  New-Item -ItemType Directory -Force -Path $stage | Out-Null
  Expand-Archive -Path $archive -DestinationPath $stage -Force

  $exe = Get-ChildItem -Path $stage -Recurse -Filter "luke.exe" | Select-Object -First 1
  if (-not $exe) { throw "archive missing luke.exe" }
  Copy-Item $exe.FullName (Join-Path $root "luke.exe")
  Remove-Item -Recurse -Force $stage

  Copy-Item (Join-Path $root "luke.exe") (Join-Path $MimoBin "luke.exe") -Force
  Set-Content -Path (Join-Path $MimoHome "current-lukelang") -Value $version -NoNewline
  Write-Mimo "lukelang $version ready"
  Write-Mimo "binary: $(Join-Path $MimoBin 'luke.exe')"
  Show-PathHint
}

function Show-PathHint {
  $path = [Environment]::GetEnvironmentVariable("Path", "User")
  if ($path -split ";" | Where-Object { $_ -eq $MimoBin }) {
    Write-Mimo "User PATH already contains $MimoBin"
    return
  }
  Write-Mimo "add to User PATH (PowerShell):"
  Write-Host ""
  Write-Host "  [Environment]::SetEnvironmentVariable('Path', `"$MimoBin;`" + [Environment]::GetEnvironmentVariable('Path','User'), 'User')"
  Write-Host ""
}

function Show-List {
  Ensure-Dirs
  Write-Host "mimo home: $MimoHome"
  Write-Host "target:    $(Get-MimoTarget)"
  Write-Host ""
  Get-ChildItem -Path $MimoToolchains -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "lukelang-*" } |
    ForEach-Object {
      $ver = $_.Name -replace "^lukelang-", ""
      $cur = if (Test-Path (Join-Path $MimoHome "current-lukelang")) {
        (Get-Content (Join-Path $MimoHome "current-lukelang") -Raw).Trim()
      } else { "" }
      if ($ver -eq $cur) { Write-Host "  lukelang $ver  (active)" }
      else { Write-Host "  lukelang $ver" }
    }
}

function Eject-LukeLang {
  Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $MimoBin "luke.exe")
  Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $MimoHome "current-lukelang")
  Get-ChildItem -Path $MimoToolchains -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "lukelang-*" } |
    Remove-Item -Recurse -Force
  Write-Mimo "ejected lukelang"
}

function Show-Doctor {
  Write-Host "mimo $MimoVersion"
  Write-Host "home   $MimoHome"
  Write-Host "dist   $MimoDist"
  Write-Host "target $(Get-MimoTarget)"
  $luke = Join-Path $MimoBin "luke.exe"
  if (Test-Path $luke) { Write-Host "luke   present ($luke)" } else { Write-Host "luke   not injected" }
}

function Show-Help {
  @"
mimo $MimoVersion — inject LukeLang onto your machine

Usage:
  mimo inject lukelang[@version]
  mimo update lukelang
  mimo list
  mimo eject lukelang
  mimo doctor
  mimo self-install
  mimo help

Bootstrap:
  irm https://lukelang.org/mimo.ps1 | iex
"@
}

# Entry when executed as a script (including irm | iex).
$argsList = @($args)
if ($MyInvocation.InvocationName -eq "." -or $MyInvocation.Line -match "iex") {
  # Piped install: no args → self-install + inject.
  if ($argsList.Count -eq 0) {
    Install-Self
    Inject-LukeLang "lukelang"
    return
  }
}

$cmd = if ($argsList.Count -gt 0) { $argsList[0] } else { "" }
$rest = if ($argsList.Count -gt 1) { $argsList[1..($argsList.Count - 1)] } else { @() }

switch ($cmd) {
  ""              { Install-Self; Inject-LukeLang "lukelang" }
  "inject"        { Inject-LukeLang $(if ($rest.Count) { $rest[0] } else { "lukelang" }) }
  "update"        { Inject-LukeLang $(if ($rest.Count) { $rest[0] } else { "lukelang" }) }
  "list"          { Show-List }
  "ls"            { Show-List }
  "eject"         { Eject-LukeLang }
  "remove"        { Eject-LukeLang }
  "uninstall"     { Eject-LukeLang }
  "doctor"        { Show-Doctor }
  "self-install"  { Install-Self; Show-PathHint }
  "setup"         { Install-Self; Show-PathHint }
  "help"          { Show-Help }
  "-h"            { Show-Help }
  "--help"        { Show-Help }
  "version"       { Write-Host "mimo $MimoVersion" }
  "--version"     { Write-Host "mimo $MimoVersion" }
  default         { throw "unknown command '$cmd' (try: mimo help)" }
}
