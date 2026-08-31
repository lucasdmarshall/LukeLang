#requires -Version 5.0
<#
.SYNOPSIS
  mimo — LukeLang toolchain installer + package manager.

.EXAMPLE
  irm https://lukelang.org/mimo.ps1 | iex
  mimo inject lukelang
  mimo init
  mimo forge greeter
#>

$ErrorActionPreference = "Stop"
$MimoVersion = "0.3.0"
$MimoHome = if ($env:MIMO_HOME) { $env:MIMO_HOME } else { Join-Path $HOME ".mimo" }
$MimoDist = if ($env:MIMO_DIST) { $env:MIMO_DIST } else { "https://lukelang.org/dist/mimo" }
$MimoRegistry = if ($env:MIMO_REGISTRY) { $env:MIMO_REGISTRY } else { "https://packages.lukelang.org/index.json" }
$MimoBin = Join-Path $MimoHome "bin"
$MimoToolchains = Join-Path $MimoHome "toolchains"
$MimoCache = Join-Path $MimoHome "cache"

function Write-Mimo($msg) { Write-Host "mimo: $msg" }

function Parse-Spec([string]$Spec) {
  if ($Spec -match "^(?<n>[^@]+)@(?<v>.+)$") {
    return @{ Name = $Matches.n; Version = $Matches.v }
  }
  return @{ Name = $Spec; Version = "latest" }
}

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
  Write-Host "registry:  $MimoRegistry"
  Write-Host "target:    $(Get-MimoTarget)"
  Write-Host ""
  Write-Host "toolchains:"
  $any = $false
  Get-ChildItem -Path $MimoToolchains -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "lukelang-*" } |
    ForEach-Object {
      $any = $true
      $ver = $_.Name -replace "^lukelang-", ""
      $cur = if (Test-Path (Join-Path $MimoHome "current-lukelang")) {
        (Get-Content (Join-Path $MimoHome "current-lukelang") -Raw).Trim()
      } else { "" }
      if ($ver -eq $cur) { Write-Host "  lukelang $ver  (active)" }
      else { Write-Host "  lukelang $ver" }
    }
  if (-not $any) { Write-Host "  (none — mimo inject lukelang)" }
  Write-Host ""
  Write-Host "packages (./luke_modules):"
  if (Test-Path "luke_modules") {
    $found = $false
    Get-ChildItem -Path "luke_modules" -Directory -ErrorAction SilentlyContinue | ForEach-Object {
      $found = $true
      Write-Host "  luke/$($_.Name)"
    }
    if (-not $found) { Write-Host "  (none — mimo forge <name>)" }
  } else {
    Write-Host "  (none — mimo forge <name>)"
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
  Write-Host "home     $MimoHome"
  Write-Host "dist     $MimoDist"
  Write-Host "registry $MimoRegistry"
  Write-Host "target   $(Get-MimoTarget)"
  $luke = Join-Path $MimoBin "luke.exe"
  if (Test-Path $luke) { Write-Host "luke     present ($luke)" } else { Write-Host "luke     not injected" }
}

function Get-RegistryIndex {
  Ensure-Dirs
  $dest = Join-Path $MimoCache "packages-index.json"
  $urls = @($MimoRegistry, "https://packages.lukelang.org/index.json", "https://lukelang.org/packages/index.json") |
    Select-Object -Unique
  foreach ($url in $urls) {
    try {
      Download-File $url $dest
      return (Get-Content -Raw $dest | ConvertFrom-Json)
    } catch {
      continue
    }
  }
  throw "cannot fetch package registry"
}

function Initialize-Project([string]$Name = "") {
  if (-not $Name) { $Name = Split-Path -Leaf (Get-Location) }
  if (Test-Path "luke.json") { throw "luke.json already exists" }
  New-Item -ItemType Directory -Force -Path "luke_modules" | Out-Null
  @{
    name = $Name
    version = "0.1.0"
    main = "main.lk"
    dependencies = @{}
  } | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "luke.json"
  if (-not (Test-Path "main.lk") -and -not (Test-Path "main.luke")) {
    'print("Hello from LukeLang")' | Set-Content -Encoding UTF8 "main.lk"
  }
  Write-Mimo "initialized project '$Name'"
  Write-Mimo "next: mimo forge greeter   or   mimo run"
}

function Write-LukeLock {
  $lines = @("# luke.lock — written by mimo", "lock_version=1", "")
  if (Test-Path "luke_modules") {
    Get-ChildItem "luke_modules" -Directory | Sort-Object Name | ForEach-Object {
      $blob = [byte[]]@()
      foreach ($part in @("luke.pkg", "main.luke", "main.lk")) {
        $f = Join-Path $_.FullName $part
        if (Test-Path $f) {
          $blob = $blob + [System.IO.File]::ReadAllBytes($f)
        }
      }
      $ver = "0.0.0"
      $pkg = Join-Path $_.FullName "luke.pkg"
      if (Test-Path $pkg) {
        Get-Content $pkg | ForEach-Object {
          if ($_ -match "^version=(.+)$") { $ver = $Matches[1].Trim() }
        }
      }
      $sha = if ($blob.Length) {
        ([System.BitConverter]::ToString(
          [System.Security.Cryptography.SHA256]::Create().ComputeHash($blob)
        )).Replace("-", "").ToLowerInvariant()
      } else { "" }
      $lines += "package $($_.Name) version $ver sha256 $sha"
    }
  }
  ($lines -join "`n") + "`n" | Set-Content -Encoding UTF8 "luke.lock"
  Write-Mimo "wrote luke.lock"
}

function Record-Dependency([string]$Name, [string]$Version) {
  if (-not (Test-Path "luke.json")) { Initialize-Project }
  $data = Get-Content -Raw "luke.json" | ConvertFrom-Json
  if (-not $data.dependencies) {
    $data | Add-Member -NotePropertyName dependencies -NotePropertyValue ([pscustomobject]@{}) -Force
  }
  $data.dependencies | Add-Member -NotePropertyName $Name -NotePropertyValue $Version -Force
  $data | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "luke.json"
}

function Add-Package([string]$Spec) {
  $parsed = Parse-Spec $Spec
  $name = $parsed.Name
  if ($name -eq "postgresql") { $name = "postgres" }
  $index = Get-RegistryIndex
  $entry = $index.packages.$name
  if (-not $entry) { throw "package '$name' not in registry — see $MimoRegistry" }

  if ($entry.kind -eq "builtin") {
    Write-Mimo "'$name' ships with LukeLang"
    Write-Mimo "  add to your source:  import $($entry.import)"
    $ver = if ($entry.version) { $entry.version } else { "0.3.0" }
    Record-Dependency $name "builtin:$ver"
    return
  }
  if ($entry.kind -eq "planned") {
    throw "'$name' is planned but not published yet"
  }

  $version = if ($parsed.Version -ne "latest") { $parsed.Version } else { $entry.version }
  $regBase = $MimoRegistry -replace "/index\.json$", ""
  $url = if ($entry.tarball) { "$regBase/$($entry.tarball)" } else { "$regBase/$name/$version/$name.tar.gz" }
  Ensure-Dirs
  $archive = Join-Path $MimoCache "$name-$version.tar.gz"
  Write-Mimo "forge luke/${name}@${version}"
  try {
    Download-File $url $archive
  } catch {
    $fb = if ($entry.url_fallback) { $entry.url_fallback } elseif ($entry.url) { $entry.url } else { "" }
    if (-not $fb -or $fb -eq $url) { throw "download failed: $url" }
    Write-Mimo "trying fallback URL"
    Download-File $fb $archive
  }
  if ($entry.archive_sha256) {
    $got = Get-Sha256 $archive
    if ($got -ne $entry.archive_sha256.ToLowerInvariant()) {
      throw "checksum mismatch: $got != $($entry.archive_sha256)"
    }
    Write-Mimo "sha256 ok"
  }

  New-Item -ItemType Directory -Force -Path "luke_modules" | Out-Null
  $dest = Join-Path "luke_modules" $name
  if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
  & tar -xzf $archive -C luke_modules
  if (-not ((Test-Path (Join-Path $dest "luke.pkg")) -or (Test-Path (Join-Path $dest "main.luke")) -or (Test-Path (Join-Path $dest "main.lk")))) {
    throw "archive did not produce luke_modules/$name"
  }
  Record-Dependency $name $version
  Write-LukeLock
  Write-Mimo "installed luke/$name → $dest"
  Write-Mimo "  import luke/$name"
}

function Remove-Package([string]$Name) {
  if ($Name -in @("lukelang", "luke")) {
    Eject-LukeLang
    return
  }
  $dest = Join-Path "luke_modules" $Name
  if (Test-Path $dest) {
    Remove-Item -Recurse -Force $dest
    Write-Mimo "removed $dest"
  } else {
    Write-Mimo "no $dest"
  }
  if (Test-Path "luke.json") {
    $data = Get-Content -Raw "luke.json" | ConvertFrom-Json
    if ($data.dependencies -and ($data.dependencies.PSObject.Properties.Name -contains $Name)) {
      $data.dependencies.PSObject.Properties.Remove($Name)
      $data | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "luke.json"
    }
  }
  if (Test-Path "luke_modules") { Write-LukeLock }
}

function Find-Luke {
  $local = Join-Path $MimoBin "luke.exe"
  if (Test-Path $local) { return $local }
  $cmd = Get-Command luke -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  throw "luke not found — run: mimo inject lukelang"
}

function Invoke-MimoRun([string]$File = "") {
  $luke = Find-Luke
  if (-not $File) {
    if (Test-Path "luke.json") {
      $File = (Get-Content -Raw "luke.json" | ConvertFrom-Json).main
      if (-not $File) { $File = "main.lk" }
    } elseif (Test-Path "main.lk") { $File = "main.lk" }
    elseif (Test-Path "main.luke") { $File = "main.luke" }
    else { throw "no main.lk — pass a file: mimo run app.lk" }
  }
  if (-not (Test-Path $File)) { throw "file not found: $File" }
  New-Item -ItemType Directory -Force -Path ".mimo/run" | Out-Null
  $out = ".mimo/run/app.exe"
  Write-Mimo "BUILD $File"
  & $luke BUILD $File -o $out
  Write-Mimo "run $out"
  & $out
}

function Show-Help {
  @"
mimo $MimoVersion — LukeLang toolchain + packages

Toolchain:
  mimo inject lukelang[@version]
  mimo update lukelang
  mimo eject lukelang
  mimo doctor

Packages:
  mimo init [name]
  mimo forge <package>[@version]
  mimo remove <package>
  mimo run [file]
  mimo list

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
  "uninstall"     { Eject-LukeLang }
  "init"          { Initialize-Project $(if ($rest.Count) { $rest[0] } else { "" }) }
  "forge"         {
    if (-not $rest.Count) { throw "usage: mimo forge <package>[@version]" }
    Add-Package $rest[0]
  }
  "add"           {
    if (-not $rest.Count) { throw "usage: mimo forge <package>[@version]" }
    Add-Package $rest[0]
  }
  "install"       {
    if (-not $rest.Count) { throw "usage: mimo forge <package>[@version]" }
    Add-Package $rest[0]
  }
  "remove"        {
    if (-not $rest.Count) { throw "usage: mimo remove <package>" }
    Remove-Package $rest[0]
  }
  "run"           { Invoke-MimoRun $(if ($rest.Count) { $rest[0] } else { "" }) }
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
