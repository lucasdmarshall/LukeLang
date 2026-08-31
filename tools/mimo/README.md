# mimo

**mimo** injects LukeLang onto your machine — one command, every supported OS/arch.

```bash
# Linux / macOS / WSL
curl -fsSL https://lukelang.org/mimo | bash

# then later
mimo inject lukelang
mimo inject lukelang@0.3.0
mimo list
mimo update lukelang
mimo eject lukelang
mimo doctor
```

```powershell
# Windows PowerShell
irm https://lukelang.org/mimo.ps1 | iex
```

## Targets

| Target | Machine |
|---|---|
| `linux-x86_64` | Linux 64-bit Intel/AMD |
| `linux-i686` | Linux 32-bit Intel |
| `linux-aarch64` | Linux 64-bit ARM |
| `linux-armv7` | Linux 32-bit ARM |
| `darwin-arm64` | Apple Silicon |
| `darwin-x86_64` | Intel Mac |
| `windows-x86_64` | Windows 64-bit |
| `windows-i686` | Windows 32-bit |
| `windows-arm64` | Windows on ARM |

## Layout on disk

```
~/.mimo/
  bin/mimo
  bin/luke          → active toolchain
  toolchains/lukelang-<ver>/luke
  cache/
  current-lukelang
```

## Maintainers

```bash
# build one target
scripts/mimo_build_release.sh linux-x86_64

# assemble site/dist/mimo + stable.json from dist/mimo-out/
scripts/mimo_publish_dist.sh
```

Artifacts are published under `https://lukelang.org/dist/mimo/` and on GitHub Releases via `.github/workflows/mimo-release.yml`.
