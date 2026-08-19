# AGENTS.md

## Cursor Cloud specific instructions

LukeLang is a single product: a conversational, reactive-native, full-stack language
and toolchain. Everything is driven by the `luke` CLI built from the C++ sources in
`vm/`. It is CLI/backend-first — the "frontend" targets compile to WASM and are
validated headlessly with Node, so there is no long-running GUI app to launch.

### Layout
- `vm/` — the toolchain (C++17). `make` builds `vm/build/luke` (`BUILD`, `SHOW`, `TEST`,
  `PUBLISH WEB`, `PKG`, `LSP`, `DAP`, `FMT`, `IR`).
- `examples/build/` — canonical Build programs (HTTP servers, Live Graph, auth, frontend).
- `examples/native/` — Play-VM-only demos (run with `--vm`).
- `scripts/` — test/smoke harnesses (`wall_smoke.sh`, `*_probe.py`, `luke_browser_loader.cjs`).

### Build / lint / test / run (standard commands, already documented in `README.md`,
`docs/getting_started.md`, `package.json`, and `vm/Makefile`)
- Build the toolchain: `cd vm && make` (or `npm run build`). Fast + incremental.
- Full test suite (Play VM, Build/native, Frontend, Reactive, SQLite, Postgres, Auth,
  WASM/browser, Live Graph): `cd vm && make test` (or `npm test`). There is no separate
  lint step; `make test` is the gate. `-Wall -Wextra -Wpedantic` are on in the Makefile.
- Run a program: `cd vm && ./build/luke BUILD ../path/to/x.luke -o /tmp/x && /tmp/x`.

### Non-obvious caveats
- WASM/browser targets (`-target wasm|browser`) and the full `make test` require the
  **WASI SDK**. It lives at `vm/.tools/wasi-sdk` (gitignored) and is captured in the VM
  snapshot. `make test` must be run with `LUKE_WASI_SDK` pointing at it:
  `cd vm && LUKE_WASI_SDK=$PWD/.tools/wasi-sdk make test`.
  If it is ever missing, reinstall WASI SDK 22 as `.github/workflows/ci.yml` does.
- The `luke` binary itself links no external libs, but **generated C programs do**:
  SQLite examples need `libsqlite3-dev`, auth examples need `libsodium-dev` (Argon2id),
  and `pg_api` needs `libpq-dev`. These are installed by the update script.
- The **`pg_api` Postgres test is optional and self-skipping**: it only runs when
  `pg_isready -h 127.0.0.1 -p 5432` succeeds. To exercise it, start Postgres and ensure a
  `luke`/`luke` superuser + `luke` database exist (see `.github/workflows/ci.yml`):
  `sudo service postgresql start`. The cluster + `luke` role/db are captured in the VM
  snapshot, so normally you only need to start the service.
- `make test` uses `gdb` (debugger tests), `fuser`/`psmisc` (frees TCP ports between
  server tests), and `readelf` (source-map assertions). All are installed by the update
  script; without them the suite fails early.
- Server example tests bind fixed localhost ports (e.g. 8797–8811, 8798 Live Graph,
  8802 auth, 8820 Exam). Tests free these ports themselves via `fuser -k`, but if a run is
  interrupted, a lingering process can hold a port — kill it by PID (never `pkill -f`).
