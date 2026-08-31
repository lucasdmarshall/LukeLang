# mimo

**mimo** is the LukeLang toolchain installer **and** package manager.

```bash
# Install compiler
curl -fsSL https://lukelang.org/mimo | bash
mimo inject lukelang

# Packages (npm/pip-style)
mimo init
mimo add greeter              # → luke_modules/greeter
mimo add http                 # builtin → import std/http
mimo run
```

```powershell
irm https://lukelang.org/mimo.ps1 | iex
```

## Commands

| Command | Role |
|---|---|
| `mimo inject lukelang` | Install / activate the compiler |
| `mimo init [name]` | Create `luke.json` + `main.lk` |
| `mimo add <pkg>` | Install from [lukelang.org/packages](https://lukelang.org/packages/) |
| `mimo remove <pkg>` | Remove from `luke_modules/` |
| `mimo run [file]` | `luke BUILD` + execute |
| `mimo list` | Toolchains + packages |
| `mimo doctor` | Diagnose PATH / registry |

Builtin modules (`http`, `json`, `postgres`, …) ship with the compiler — `mimo add` prints the `import std/…` line. Registry packages install as `import luke/<name>`.

## Maintainers

```bash
scripts/mimo_publish_packages.sh   # rebuild site/packages from registry/packages
scripts/mimo_publish_dist.sh       # toolchain binaries
deploy/enable_packages_vhost.sh    # nginx + TLS for packages.lukelang.org
```
