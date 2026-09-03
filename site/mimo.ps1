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
$MimoVersion = "0.3.3"
$MimoHome = if ($env:MIMO_HOME) { $env:MIMO_HOME } else { Join-Path $HOME ".mimo" }
$MimoDist = if ($env:MIMO_DIST) { $env:MIMO_DIST } else { "https://lukelang.org/dist/mimo" }
$MimoRegistry = if ($env:MIMO_REGISTRY) { $env:MIMO_REGISTRY } else { "https://packages.lukelang.org/index.json" }
$MimoBin = Join-Path $MimoHome "bin"
$MimoToolchains = Join-Path $MimoHome "toolchains"
$MimoCache = Join-Path $MimoHome "cache"

function Write-Mimo($msg) { Write-Host "mimo: $msg" }
function Write-MimoStep([string]$Step, [string]$Msg) {
  Write-Host ""
  Write-Host ("[{0}] {1}" -f $Step, $Msg) -ForegroundColor Cyan
}
function Write-MimoOk($msg) { Write-Host "  OK  $msg" -ForegroundColor Green }
function Write-MimoFail($msg) { Write-Host " FAIL $msg" -ForegroundColor Red }

function Test-MimoInteractive {
  try {
    return [Environment]::UserInteractive -and $Host.Name -ne "Default Host"
  } catch {
    return $false
  }
}

function Wait-MimoExit([int]$Code = 0) {
  # Keep the window open when double-clicked / launched outside an existing shell.
  $keep = $env:MIMO_PAUSE
  if (-not $keep) {
    if (-not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected) {
      # Parent-less consoles (explorer double-click) often have no useful $Host parent.
      try {
        $ppid = (Get-CimInstance Win32_Process -Filter "ProcessId=$PID").ParentProcessId
        $parent = Get-Process -Id $ppid -ErrorAction SilentlyContinue
        if ($parent -and $parent.ProcessName -match '^(explorer|cmd|powershell|pwsh|WindowsTerminal|OpenConsole)$') {
          if ($parent.ProcessName -eq "explorer") { $keep = "1" }
        }
      } catch { }
    }
  }
  if ($keep -eq "1" -or $env:MIMO_PAUSE -eq "1") {
    Write-Host ""
    if ($Code -eq 0) {
      Write-Host "Install finished. You can close this window." -ForegroundColor Green
    } else {
      Write-Host "Install failed. Read the error above, then close this window." -ForegroundColor Red
    }
    Write-Host -NoNewline "Press Enter to exit..."
    try { [void](Read-Host) } catch { Start-Sleep -Seconds 8 }
  }
  exit $Code
}

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
  Write-Mimo "download $Url"
  $ProgressPreference = "Continue"
  Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
  if (-not (Test-Path $Dest)) { throw "download produced no file: $Dest" }
  $bytes = (Get-Item $Dest).Length
  Write-MimoOk ("saved {0} ({1:N0} bytes)" -f $Dest, $bytes)
}

function Get-Sha256([string]$Path) {
  (Get-FileHash -Algorithm SHA256 -Path $Path).Hash.ToLowerInvariant()
}

function Install-Self {
  Write-MimoStep "1/4" "Installing mimo into $MimoHome"
  Ensure-Dirs
  $dest = Join-Path $MimoBin "mimo.ps1"
  $wrapper = Join-Path $MimoBin "mimo.cmd"
  # Prefer re-fetching canonical copy so curl-less installs stay current.
  Download-File "$MimoDist/mimo.ps1" $dest
  @"
@echo off
setlocal
REM Double-click friendly: keep the window open after install.
if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mimo.ps1" 
  echo.
  pause
  exit /b %ERRORLEVEL%
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0mimo.ps1" %*
"@ | Set-Content -Encoding ASCII -Path $wrapper
  $tplSrc = Join-Path $PSScriptRoot "templates"
  if (Test-Path $tplSrc) {
    $tplDest = Join-Path $MimoHome "templates"
    New-Item -ItemType Directory -Force -Path $tplDest | Out-Null
    Copy-Item -Path (Join-Path $tplSrc "*") -Destination $tplDest -Recurse -Force
  }
  Write-MimoOk "mimo $MimoVersion → $dest"
  Write-MimoOk "launcher → $wrapper"
}

function Ensure-UserPath {
  $path = [Environment]::GetEnvironmentVariable("Path", "User")
  if (-not $path) { $path = "" }
  $parts = @($path -split ";" | Where-Object { $_ -and $_.Trim() -ne "" })
  if ($parts | Where-Object { $_ -eq $MimoBin }) {
    Write-MimoOk "User PATH already contains $MimoBin"
    return
  }
  $newPath = ($MimoBin + ";" + $path).TrimEnd(";")
  [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
  $env:Path = "$MimoBin;$env:Path"
  Write-MimoOk "added $MimoBin to User PATH (new terminals pick this up)"
}

function Get-Channel([string]$Channel = "stable") {
  $dest = Join-Path $MimoCache "$Channel.json"
  Download-File "$MimoDist/channel/$Channel.json" $dest
  return (Get-Content -Raw $dest | ConvertFrom-Json)
}

function Inject-LukeLang([string]$Spec = "lukelang") {
  Write-MimoStep "2/4" "Downloading LukeLang compiler"
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
    Write-MimoStep "3/4" "Verifying sha256"
    $got = Get-Sha256 $archive
    if ($got -ne $sha.ToLowerInvariant()) { throw "checksum mismatch: $got != $sha" }
    Write-MimoOk "sha256 matches"
  } else {
    Write-MimoStep "3/4" "Skip checksum (pinned version without channel sha)"
  }

  Write-MimoStep "4/4" "Installing into $MimoToolchains"
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
  $runtime = Join-Path $stage "runtime"
  if (Test-Path $runtime) { Copy-Item $runtime (Join-Path $root "runtime") -Recurse -Force }
  $stdlib = Join-Path $stage "stdlib"
  if (Test-Path $stdlib) { Copy-Item $stdlib (Join-Path $root "stdlib") -Recurse -Force }
  Remove-Item -Recurse -Force $stage

  Copy-Item (Join-Path $root "luke.exe") (Join-Path $MimoBin "luke.exe") -Force
  Set-Content -Path (Join-Path $MimoHome "current-lukelang") -Value $version -NoNewline
  Ensure-UserPath
  Write-MimoOk "lukelang $version ready"
  Write-MimoOk ("binary: {0}" -f (Join-Path $MimoBin "luke.exe"))
}

function Show-PathHint {
  Ensure-UserPath
}

function Install-LukeBootstrap {
  Write-Host ""
  Write-Host "========================================" -ForegroundColor Yellow
  Write-Host "  LukeLang installer (mimo $MimoVersion)" -ForegroundColor Yellow
  Write-Host "========================================" -ForegroundColor Yellow
  Write-Host "  Home: $MimoHome"
  Write-Host "  Dist: $MimoDist"
  Write-Host ""
  try {
    Install-Self
    Inject-LukeLang "lukelang"
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "  SUCCESS" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "  luke.exe  →  $(Join-Path $MimoBin 'luke.exe')"
    Write-Host "  mimo.cmd  →  $(Join-Path $MimoBin 'mimo.cmd')"
    Write-Host ""
    Write-Host "  Open a NEW PowerShell or terminal, then run:"
    Write-Host "    mimo doctor"
    Write-Host "    luke"
    Write-Host ""
    Write-Host "  Note: unsigned downloads may trigger Windows SmartScreen."
    Write-Host "  That is expected until we ship Authenticode-signed builds."
    Write-Host "  Prefer: irm https://lukelang.org/mimo.ps1 | iex"
    Write-Host "  Or unblock the zip: right-click → Properties → Unblock."
    Write-Host ""
    return 0
  } catch {
    Write-Host ""
    Write-MimoFail $_.Exception.Message
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "  FAILED" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    Write-Host "  See https://lukelang.org/download/ for manual steps."
    Write-Host ""
    return 1
  }
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

function Sync-TemplatesFromDist {
  $apiDir = Join-Path $MimoHome "templates\api"
  New-Item -ItemType Directory -Force -Path $apiDir | Out-Null
  foreach ($f in @("main.lk", "README.md", ".gitignore")) {
    $dest = Join-Path $apiDir $f
    if (-not (Test-Path $dest)) {
      try {
        Download-File "$MimoDist/templates/api/$f" $dest
      } catch {
        continue
      }
    }
  }
}

function Get-MimoTemplatesRoot {
  $local = Join-Path $PSScriptRoot "templates"
  if (Test-Path $local) { return $local }
  $homeTpl = Join-Path $MimoHome "templates"
  if (Test-Path $homeTpl) { return $homeTpl }
  Sync-TemplatesFromDist
  if (Test-Path $homeTpl) { return $homeTpl }
  return $null
}

function Write-EmbeddedApiMain {
  @'
// LukeLang HTTP API — scaffolded by: mimo init --template api
// Routes: GET /health, GET /ok, GET /user/:id, GET /me, POST /login
// Run: mimo run

import std/server
import std/sqlite
import std/json

let setup = dbOpen("data/app.db")
dbExec(setup, "CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY, name TEXT)")
dbExec(setup, "CREATE TABLE IF NOT EXISTS sessions(sid TEXT PRIMARY KEY, user_id TEXT)")
var seed: list = []
seed.push("1")
seed.push("Ada")
dbExecBind(setup, "INSERT OR IGNORE INTO users(id, name) VALUES(?, ?)", seed)
dbClose(setup)

fn handle(req: Request) {
  let db = dbOpen("data/app.db")
  let method = httpMethod(req)
  let path = httpPath(req)
  var params: map = {}
  var answered: bool = false

  if method == "GET" {
    if httpMatch(path, "/health", params) {
      httpReply(req, 200, "text/plain", "ok")
      answered = true
    }
  }

  if !answered {
    if method == "GET" {
      if httpMatch(path, "/user/:id", params) {
        let id = params["id"]
        let qmap: map = httpQueryMap(req)
        let tag = qmap["tag"]
        var binds: list = []
        binds.push(id)
        let name = dbQueryBind(db, "SELECT name FROM users WHERE id = ?", binds)
        httpReply(req, 200, "text/plain", name + "|" + tag)
        answered = true
      }
    }
  }

  if !answered {
    if method == "GET" {
      if httpMatch(path, "/me", params) {
        let meSid = httpCookie(req, "luke_sid")
        var meBinds: list = []
        meBinds.push(meSid)
        let uid = dbQueryBind(db, "SELECT user_id FROM sessions WHERE sid = ?", meBinds)
        httpReply(req, 200, "text/plain", "me=" + uid)
        answered = true
      }
    }
  }

  if !answered {
    if method == "POST" {
      if httpMatch(path, "/login", params) {
        let body = httpJson(req)
        let userNode = jsonGet(body, "user")
        let user = jsonAsText(userNode)
        let loginSid = "s-" + user
        var loginBinds: list = []
        loginBinds.push(loginSid)
        loginBinds.push("1")
        dbExecBind(db, "INSERT OR REPLACE INTO sessions(sid, user_id) VALUES(?, ?)", loginBinds)
        httpSetCookie(req, "luke_sid", loginSid)
        httpReply(req, 200, "text/plain", "ok")
        answered = true
      }
    }
  }

  if !answered {
    if method == "GET" {
      if httpMatch(path, "/ok", params) {
        httpReply(req, 200, "text/plain", "ok")
        answered = true
      }
    }
  }

  if !answered {
    httpReply(req, 404, "text/plain", "not found")
  }

  dbClose(db)
}

let port: int = 8080
print("api listening on :" + port)
let server = httpListen(port)
httpServe(server, handle, 4)
'@ | Set-Content -Encoding UTF8 "main.lk"
}

function Apply-TemplateApi([string]$Name) {
  New-Item -ItemType Directory -Force -Path "luke_modules", "data" | Out-Null
  $root = Get-MimoTemplatesRoot
  $mainTpl = if ($root) { Join-Path $root "api\main.lk" } else { $null }
  if ($mainTpl -and (Test-Path $mainTpl)) {
    Copy-Item $mainTpl "main.lk" -Force
    $readme = Join-Path $root "api\README.md"
    if (Test-Path $readme) { Copy-Item $readme "README.md" -Force }
    $gitignore = Join-Path $root "api\.gitignore"
    if (Test-Path $gitignore) { Copy-Item $gitignore ".gitignore" -Force }
  } else {
    Write-EmbeddedApiMain
  }
  @{
    name = $Name
    version = "0.1.0"
    main = "main.lk"
    template = "api"
    dependencies = @{}
  } | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "luke.json"
  Write-Mimo "initialized API project '$Name'"
  Write-Mimo "next: mimo run"
  Write-Mimo "      curl http://localhost:8080/health"
}

function Parse-InitArgs([string[]]$Args) {
  $template = ""
  $name = ""
  $i = 0
  while ($i -lt $Args.Count) {
    $arg = $Args[$i]
    switch -Regex ($arg) {
      '^(--template|-t)$' {
        if ($i + 1 -ge $Args.Count) { throw "usage: mimo init [--template api] [name]" }
        $template = $Args[$i + 1]
        $i += 2
        continue
      }
      '^--template=(.+)$' {
        $template = $Matches[1]
        $i++
        continue
      }
      '^-' { throw "unknown option: $arg (try: mimo init --help)" }
      default {
        $name = $arg
        $i++
      }
    }
  }
  return @{ Template = $template; Name = $name }
}

function Initialize-Project {
  param([string[]]$InitArgs = @())

  if ($InitArgs -contains "-h" -or $InitArgs -contains "--help") {
    @"
mimo init — create a new LukeLang project

Usage:
  mimo init [name]
  mimo init --template api [name]

Templates:
  (default)   Hello-world main.lk
  api         HTTP API with SQLite, routes, and health check
"@ | Write-Host
    return
  }

  $parsed = Parse-InitArgs $InitArgs
  $template = $parsed.Template
  $name = $parsed.Name
  if (-not $name) { $name = Split-Path -Leaf (Get-Location) }
  if (Test-Path "luke.json") { throw "luke.json already exists" }

  if ($template -eq "api") {
    Apply-TemplateApi $name
    return
  }
  if ($template) { throw "unknown template '$template' (try: api)" }

  New-Item -ItemType Directory -Force -Path "luke_modules" | Out-Null
  @{
    name = $name
    version = "0.1.0"
    main = "main.lk"
    dependencies = @{}
  } | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "luke.json"
  if (-not (Test-Path "main.lk") -and -not (Test-Path "main.luke")) {
    'print("Hello from LukeLang")' | Set-Content -Encoding UTF8 "main.lk"
  }
  Write-Mimo "initialized project '$name'"
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
  $tcRoot = Split-Path -Parent $luke
  if (Test-Path (Join-Path $tcRoot "runtime")) {
    $env:LUKE_RUNTIME = Join-Path $tcRoot "runtime"
  }
  if (Test-Path (Join-Path $tcRoot "stdlib")) {
    $env:LUKE_STDLIB = Join-Path $tcRoot "stdlib"
  }
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
  mimo init --template api [name]
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
$pipedInstall = ($MyInvocation.InvocationName -eq "." -or "$($MyInvocation.Line)" -match "iex")

function Invoke-MimoMain {
  param(
    [object[]]$ArgList,
    [bool]$Piped = $false
  )

  if ($Piped -and $ArgList.Count -eq 0) {
    return (Install-LukeBootstrap)
  }

  $cmd = if ($ArgList.Count -gt 0) { $ArgList[0] } else { "" }
  $rest = if ($ArgList.Count -gt 1) { $ArgList[1..($ArgList.Count - 1)] } else { @() }

  switch ($cmd) {
    ""              { return (Install-LukeBootstrap) }
    "inject"        { Inject-LukeLang $(if ($rest.Count) { $rest[0] } else { "lukelang" }); return 0 }
    "update"        { Inject-LukeLang $(if ($rest.Count) { $rest[0] } else { "lukelang" }); return 0 }
    "list"          { Show-List; return 0 }
    "ls"            { Show-List; return 0 }
    "eject"         { Eject-LukeLang; return 0 }
    "uninstall"     { Eject-LukeLang; return 0 }
    "init"          { Initialize-Project $rest; return 0 }
    "forge"         {
      if (-not $rest.Count) { throw "usage: mimo forge <package>[@version]" }
      Add-Package $rest[0]
      return 0
    }
    "add"           {
      if (-not $rest.Count) { throw "usage: mimo forge <package>[@version]" }
      Add-Package $rest[0]
      return 0
    }
    "install"       {
      if (-not $rest.Count) { throw "usage: mimo forge <package>[@version]" }
      Add-Package $rest[0]
      return 0
    }
    "remove"        {
      if (-not $rest.Count) { throw "usage: mimo remove <package>" }
      Remove-Package $rest[0]
      return 0
    }
    "run"           { Invoke-MimoRun $(if ($rest.Count) { $rest[0] } else { "" }); return 0 }
    "doctor"        { Show-Doctor; return 0 }
    "self-install"  { Install-Self; Show-PathHint; return 0 }
    "setup"         { return (Install-LukeBootstrap) }
    "help"          { Show-Help; return 0 }
    "-h"            { Show-Help; return 0 }
    "--help"        { Show-Help; return 0 }
    "version"       { Write-Host "mimo $MimoVersion"; return 0 }
    "--version"     { Write-Host "mimo $MimoVersion"; return 0 }
    default         { throw "unknown command '$cmd' (try: mimo help)" }
  }
}

try {
  $code = Invoke-MimoMain -ArgList $argsList -Piped:$pipedInstall
  if ($null -eq $code) { $code = 0 }
  if ($env:MIMO_PAUSE -eq "1") { Wait-MimoExit $code }
  if ($code -ne 0) { exit $code }
} catch {
  Write-MimoFail $_.Exception.Message
  if ($env:MIMO_PAUSE -eq "1") { Wait-MimoExit 1 }
  exit 1
}
