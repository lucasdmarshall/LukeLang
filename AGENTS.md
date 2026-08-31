# AGENTS.md — LukeLang agent playbook

Canonical quick-start for AI and code agents working on LukeLang. If you have no LukeLang
pretraining, read this file first, then execute tasks. Everything here is verifiable from the
repository — when this file and the code disagree, the code wins and this file is the bug.

## 1) Project identity (do not drift)

- LukeLang is **Build-first**. `luke BUILD` is the language of record: native or WebAssembly,
  arena memory, no GC on the shipped path.
- `luke SHOW --vm` is the Play VM — a compatibility skateboard, not the product core.
- **Backend + Live Graph is the current beachhead.** Mobile, game and canvas tracks are parked.
- One dependency graph spans database, server, wire and browser. Never add a fetch/subscribe
  layer parallel to it.

Primary docs: [`docs/STRATEGY.md`](docs/STRATEGY.md) ·
[`docs/BUILD_MODE.md`](docs/BUILD_MODE.md) ·
[`docs/SYNTAX_V2_SPEC.md`](docs/SYNTAX_V2_SPEC.md) ·
[`docs/LIVE_GRAPH.md`](docs/LIVE_GRAPH.md) ·
[`docs/SCORECARD.md`](docs/SCORECARD.md)

## 2) Syntax v2 is the default — read this before writing any Luke code

Since PR #38, **`.luke` and `.lk` are both syntax v2**. Writing conversational v1 into a
`.luke` file is now a syntax error, and it is the single most common mistake an agent makes
here.

| Write this | Not this |
| --- | --- |
| `print(x)` | `SPEAK x` |
| `let n = 1` / `var n = 1` | `MY NAME IS n SET TO 1` |
| `let n: float = 1` | `MY NAME IS n AS NUMBER SET TO 1` |
| `fn f(a: float) -> float { … }` | `THIS IS FUNCTION f WITH a AS NUMBER … END FUNCTION` |
| `return a + b` | `GIVE BACK ADD a AND b` |
| `struct Dog : Animal { … }` | `BLUEPRINT Dog FOLLOWS Animal DO … END CLASS` |
| `import std/server` | `IMPORT std/server` |
| `signal count = 0` | `REMEMBER count SET TO 0` |
| `effect on cell { … }` | `WHEN REACTIVE cell CHANGES DO … END WHEN` |

Types: `int`, `float`, `str`, `bool`, `list`, `map`, `json`, plus `Server`, `Request`, `Db`.

Escape hatches, in order of preference:

1. `raw "…"` or `raw """…"""` passes a line straight through to the v1 surface. Use it for
   forms v2 does not express yet (`CONTRACT`, `ALWAYS`, layout phrases).
2. `luke MIGRATE file.luke` rewrites v1 source into v2 and marks anything it cannot translate
   with `TODO(migrate)`.
3. `luke --syntax=1 BUILD file.luke` forces the conversational surface for the deprecation
   window. Note the flag goes **before** the command.

Conversational sources are archived under `examples/v1_archive/` and `vm/stdlib_v1_archive/`
and are what the equivalence gates compare against. Do not "fix" them into v2.

Architecture note: v2 parses to an AST and **lowers to v1 text**, which `build_c.cpp` still
consumes. Deleting the v1 statement parser or the phrase prefixes in `build_c.cpp` is
explicitly post-deprecation-window work — see [`docs/SYNTAX_V2_PLAN.md`](docs/SYNTAX_V2_PLAN.md).

## 3) Build and run

End users install with **mimo** (`curl -fsSL https://lukelang.org/mimo | bash` →
`mimo inject lukelang`). Contributors build from this tree:

From the repository root:

```bash
cd vm && make
```

Core commands (from `vm/`):

```bash
./build/luke SHOW  ../examples/build/hello.luke
./build/luke BUILD ../examples/build/hello.luke -o build/hello && ./build/hello
./build/luke BUILD ../examples/build/hello_wasm.luke -target wasm -o build/hello.wasm
./build/luke MIGRATE ../examples/v1_archive/build/hello.luke
./build/luke FMT   ../examples/build/hello.luke
./build/luke LSP
./build/luke DAP
./build/luke DEBUG ../examples/build/functions.luke --break 10 --batch
make test
```

Optional toolchain, each needed only by the feature that uses it:

- WebAssembly and browser targets: WASI SDK at `.tools/wasi-sdk`, or `LUKE_WASI_SDK`
- `import std/sqlite`: `libsqlite3-dev` · `import std/auth`: `libsodium-dev` ·
  `import std/pg`: `libpq-dev` and a reachable Postgres
- `luke DEBUG` / `luke DAP` and their tests: `gdb`
- Headless browser and WASI smoke scripts: Node.js

The compiler passes `-DLUKE_HAVE_SQLITE` / `-DLUKE_HAVE_SODIUM` / `-DLUKE_HAVE_PG` only when
the matching module is imported, and the runtime headers gate their system includes on those
defines. **If you add a runtime header that pulls in a third-party system header, gate it the
same way** — otherwise hello-world stops building on a clean machine.

## 4) Canonical examples — read one before inventing syntax

The examples are the acceptance suite, not illustrations. If your code does not look like
them, your code is wrong.

Core: `examples/build/hello.luke` · `functions.luke` · `oop.luke` · `collections.luke` ·
`modules.luke` · `arena_scope.luke`

Reactive: `reactive_core.luke` · `reactive_conformance_batch.luke` ·
`reactive_conformance_order.luke` · `reactive_conformance_memory.luke` ·
`reactive_conformance_error.luke`

Live Graph: `live_graph_server.luke` · `live_graph_client.luke` · `live_graph_join.luke` ·
`live_graph_agg.luke` · `examples/deploy/wall/`

Backend: `backend_api.luke` · `auth_api.luke` · `http_c10k_ok.luke` · `pg_api.luke` ·
`sql_bind.luke`

Frontend: `frontend_widgets.luke` · `frontend_done.luke` · `reactive_list_ui.luke` ·
`web_app.luke`

Golden v2 corpus with hand-written twins: `examples/v2/*.lk`

Programs that **must fail** to compile: `bad_types.luke` · `bad_arity.luke` ·
`auth_secret_bad_bind.luke` · `backend_mw_bad_order.luke`. CI fails if any of them succeeds.

## 5) Gates — what has to be green

```bash
cd vm
make                 # toolchain
make test            # test-play + test-build (the full gate)
make test-play       # Play VM paths
make test-build      # Build, reactive, Live Graph, frontend, debugger, LSP, DAP
make test-syntax-v2  # spec coverage, corpus pairing, v1↔v2 equivalence
make test-lsp        # language server providers
make test-fmt-roundtrip
```

Focused scripts:

```bash
bash scripts/debug_break_step.sh
bash scripts/debug_inspect.sh
bash scripts/dap_handshake.sh
bash scripts/lsp_providers.sh
bash scripts/fmt_roundtrip_all.sh
bash scripts/syntax_v2_equiv.sh
python3 scripts/syntax_v2_spec_check.py
python3 scripts/syntax_v2_corpus_check.py
```

Run a single example first, then the focused script, then `make test`. If the full gate is too
slow while iterating, state exactly which subset you ran.

## 6) The website is generated — regenerate it or CI fails

After editing `docs/**` or `documentations/papers/**`:

```bash
python3 scripts/build_site_docs.py
python3 scripts/build_site_meta.py
```

After changing `tools/mimo/*` or rebuilding release binaries:

```bash
scripts/mimo_publish_dist.sh    # syncs site/mimo + site/dist/mimo
python3 scripts/build_site_meta.py
```

`site/` is the deployed lukelang.org. Two parts of it are **generated from the repository**:

```bash
python3 scripts/build_site_docs.py   # docs/*.md + documentations/papers/*.md → site/docs/
python3 scripts/build_site_meta.py   # canonical/OG/JSON-LD, sitemap.xml, robots.txt
```

**If you edit any file under `docs/` or `documentations/papers/`, run both and commit the
result.** The `site-docs` CI job regenerates and fails when the committed HTML has drifted.

Deploy (needs ssh access to the host):

```bash
scripts/deploy_site.sh
```

`site/status/` is a standalone page for `status.lukelang.org` — self-contained on purpose, so
it keeps its styling when the main host is down.

## 7) Module boundaries

Compiler and runtime:

| Path | Owns |
| --- | --- |
| `vm/src/luke2_lex.cpp` | v2 lexer, path/mode rules (`isV2Path`, `wantsV2`) |
| `vm/src/luke2_parse.cpp` | v2 parser → AST |
| `vm/src/luke2_lower.cpp` | AST → v1 text for codegen |
| `vm/src/luke2_migrate.cpp` | v1 → v2 source rewriting |
| `vm/src/luke_parse.cpp` | v1 statement recognition (deprecation window) |
| `vm/src/luke_expr.cpp` | v1 expression parsing |
| `vm/src/build_c.cpp` | Build codegen, types, IR lowering, linked libraries |
| `vm/src/lsp.cpp` / `dap.cpp` | LSP and DAP over stdio |
| `vm/src/main.cpp` | CLI surface, compiler invocation, feature defines |
| `vm/runtime/*.h` | Build runtime — arena, net, db, auth, reactive, layout |
| `vm/stdlib/*.luke` | Standard library, written in LukeLang |

**Rule:** do not add a parallel parser or toolchain path for editor features. LSP, FMT and
diagnostics stay on the Build compiler's truth.

Editor extension: `tools/vscode/lukelang/` is a thin client. It launches
`vm/build/luke LSP` / `DAP`. Do not reimplement language logic in JavaScript.

```bash
bash scripts/vscode_extension_package.sh   # build the .vsix
```

## 8) Search patterns

```bash
rg "fn |let |struct |signal |derived |effect on|watch |push watch" examples/build
rg "isV2Path|wantsV2|maybeLowerSource|SyntaxMode" vm/src
rg "THIS IS FUNCTION|MY NAME IS|SPEAK |REMEMBER " vm/src/luke_parse.cpp   # v1 surface
rg "linkLibs|LUKE_HAVE_" vm/src/build_c.cpp vm/src/main.cpp
rg "hover|completion|semanticTokens" vm/src/lsp.cpp scripts/lsp_providers.sh
rg "httpServe|dbExecBind|dbQueryBind|pgQueryBind" vm/runtime vm/stdlib examples/build
```

## 9) What a feature PR must include

1. Documentation in `docs/` — or an explicit reason none is needed.
2. At least one example under `examples/` that CI compiles and asserts on.
3. Regenerated `site/docs/` if any Markdown changed.
4. A green `make test`, and the exact commands to reproduce.
5. If it changes the language surface, the same commit updates
   [`docs/SYNTAX_V2_SPEC.md`](docs/SYNTAX_V2_SPEC.md) — the spec is machine-checked against
   codegen and will fail otherwise.

Label anything provisional as provisional, in the document and in
[`docs/SCORECARD.md`](docs/SCORECARD.md). An honest "not proven yet" is worth more here than a
confident claim the tests do not back.

## 10) Non-goals while backend-first

- Do not shift effort to mobile, game or canvas tracks.
- Do not add distribution or ecosystem work unless asked.
- Do not do stylistic syntax rewrites ahead of backend and Live Graph milestones.
- Do not delete the v1 parser or the `build_c.cpp` phrase prefixes — that is scheduled work,
  not cleanup.
