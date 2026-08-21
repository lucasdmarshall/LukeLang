# Luke Build Mode — The Real Language

> **Play** (`luke SHOW`) is a convenience: GC’d bytecode VM for demos and exploration.  
> **Build** (`luke BUILD`) is the language of record: conversational syntax → **native code**, **no GC**, Rust-class lightness.

This is Path A: a **cell-graph face** (cells + reactions) on the surface, Zig/Rust-shaped cost underneath. `WHILE`, mutation `SET`, and `THIS IS FUNCTION` are recipes — they compile, but they are not the teaching surface ([`STRATEGY.md`](./STRATEGY.md)).

## Commands

```bash
luke SHOW  examples/build/hello.luke              # prefers Build (native temp); --vm forces Play
luke BUILD examples/build/hello.luke              # Build — native binary, no GC
luke BUILD examples/build/hello.luke -o hello
luke BUILD examples/build/hello_wasm.luke -target wasm -o hello.wasm
luke BUILD examples/build/hello_browser.luke -target browser -o hello_web
luke BUILD examples/build/functions.luke -target debug -o fn  # -O0 -g for gdb
luke DEBUG examples/build/functions.luke --break 10           # interactive gdb (.luke:line)
luke DEBUG examples/build/functions.luke --break 10 --batch    # CI: break + next/step/finish
luke LSP                                          # stdio LSP: hover/outline/FMT/refs/rename/…
# run wasm (WASI): node scripts/run_wasi.cjs hello.wasm
# run browser wasm headless: node scripts/luke_browser_loader.cjs hello_web.wasm
# or open hello_web.html in a browser
```

Build emits C with `#line N "file.luke"` per statement, then compiles with the system C compiler
(`cc -O2 -g` native, or `-O0 -g -fno-inline` for `-target debug` / `luke DEBUG`). WASI SDK for
`-target wasm|browser`. Debugger skips `luke_rt.h` / stdlib headers so **next** = step over,
**step** = step into Luke FUNCTION, **finish** = step out. `luke DEBUG --inspect` dumps reactive
cell values and dependency edges (`luke_rx_inspect_cstr`). `luke DAP` is a stdio Debug Adapter
Protocol server (gdb backend) for editors — Reactive scope shows named cells + deps.
Browser also writes `*.html` + `luke_browser_loader.js` (copied beside the wasm) for `<script>` tags.

## IMPORT + stdlib + packages

```luke
IMPORT "./critter.luke"          # relative module
IMPORT std/files                 # read/write TEXT files
IMPORT std/json                  # JSON tree helpers
IMPORT std/http                  # httpGet (native)
IMPORT luke/greeter              # package from luke_modules/greeter
```

`std/*` resolves from `vm/stdlib/`. Relative paths are next to the entry file.

### Packages (`luke/<name>`)

Look up order:
1. `luke_modules/` next to the source file
2. `./luke_modules`, `LUKE_PACKAGES` (colon-separated roots)
3. Package dir must contain `luke.pkg` (with `entry=…`), or `main.luke`, or `<name>.luke`

```
luke_modules/greeter/luke.pkg    # entry=main.luke
luke_modules/greeter/main.luke
```

`IMPORT package:greeter` is an alias for `IMPORT luke/greeter`.

Foreign FFI imports (C/JS/Python bridges) are intentionally **not** magic `IMPORT numpy` — parked for later; C wrappers will come first.

| Module | Helpers |
| --- | --- |
| `std/files` | `readFile`, `writeFile`, `fileExists` |
| `std/json` | `jsonParse`, `jsonGet`, `jsonIndex`, `jsonLen`, `jsonHas`, `jsonAsText` / `Number` / `Flag`, `jsonStringify`, `jsonString` |
| `std/http` | `httpGet` (native via curl; empty on WASI) |
| `std/server` | `httpListen`, `httpAccept`, `httpReply`, `httpPath` / `Method` / `Query` / `Body`, `httpMatch`, `httpQueryMap`, `httpHeader` / `Cookie` / `SetCookie`, `httpJson`, SSE helpers |
| `std/sqlite` | `dbOpen`/`dbClose` **thread-local pool**, stmt cache, WAL; `dbExecBind` / `dbQueryBind`; auto `-lsqlite3` |
| `std/pg` | `pgOpen`/`pgClose`/`pgCheckout`/`pgCheckin`, `pgQueryBind`/`pgExecBind`/`pgRowsBind` — **Slipstream** default (`LUKE_PG_ASYNC=0` → blocking); auto `-lpq` |
| `std/auth` | `authInit`, `authCreateAccount`, `authLogin` / `Logout`, `authRequire`, `authCsrf` / `authCheckCsrf` (links `-lsodium`; set `LUKE_AUTH_SECURE=1` behind HTTPS) |
| `std/args` | `argCount`, `getArg` |
| `std/env` | `getEnv`, `setEnv` |
| `std/paths` | `cwd`, `pathJoin`, `pathBasename`, `pathDirname` |
| `std/process` | `shell`, `exitWith` |
| `std/js` | `jsSetText`, `jsSetHtml`, `jsGetValue`, `jsFetch`, `jsOnClick`, `jsLoadFont`, `jsAddStyle`, `jsSetTitle` |
| `luke/…` | Your packages under `luke_modules/` (`luke PKG init <name>`) |

### Browser page ownership (conversational)

```luke
NAME THE PAGE "LukeLang"
BRING FONT "Syne" FROM "./fonts/syne-700.woff2"   # local pack → @font-face + copy
WEAR STYLE """
  body { font-family: Syne, sans-serif; }
"""
FILL "root" WITH """
  <h1>LukeLang</h1>
  <button id="go">Go</button>
  <p id="out"></p>
"""
WHEN "go" IS CLICKED DO
  FILL "out" WITH "Still LukeLang."
END WHEN
```

`-target browser` emits HTML with Luke title/CSS/body/fonts **baked in**, wasm beside it, and **inlines** `vm/runtime/luke_browser_boot.js` (runtime — not app JS). Dist has no `luke_browser_loader.js`.

See `sample/landing.luke`.

### Collections + problems (conversational)

```luke
MY NAME IS nums AS LIST
ADD "one" TO nums
SPEAK ITEM 0 OF nums
SPEAK HOW MANY IN nums
SPEAK EACH OF nums
SPEAK EACH NUMBER FROM 1 TO 20
SPEAK EACH n FROM 1 TO 20 WHERE n IS DIVISIBLE BY 2
SPEAK EACH OF THE NUMBERS FROM 1 TO 5

MY NAME IS bag AS MAP
PUT "name" TO "Luke" IN bag
SPEAK GET "name" FROM bag

ATTEMPT DO
  GIVE UP WITH "could not finish"
OTHERWISE WITH problem DO
  SPEAK problem
END ATTEMPT
```

### TEST

```luke
TEST "math" DO
  MAKE SURE ADD 1 AND 1 EQUALS 2
END TEST
```

```bash
luke TEST examples/build/collections_test.luke
```

### Packages

```bash
luke PKG init mylib
luke PKG install echo
luke PKG publish mylib
luke PKG lock          # writes luke.lock
```

### Arena scopes

```luke
IN ARENA DO
  MY NAME IS tmp AS TEXT SET TO "ephemeral"
  SPEAK tmp
END ARENA
```

Bump pointer is restored at `END ARENA` — request/frame-scoped memory without GC.

## Guarantees (Build)

| Rule | Meaning |
| --- | --- |
| **No GC** | Memory comes from stack, statics, or a bump **arena** freed in bulk |
| **Fixed layouts** | `BLUEPRINT` fields are a C struct — not open hash maps |
| **Known types** | Locals/fields are `NUMBER`, `FLAG`, `TEXT`, or a blueprint type |
| **Native code** | Ahead-of-time C → machine code (and later WASM) |
| **Same voice** | `SPEAK`, `SPEAK EACH`, `MY NAME IS`, `ASK`, `BLUEPRINT` still work |

## Types (Luke words)

| Luke | Build representation |
| --- | --- |
| `NUMBER` | `double` |
| `INTEGER` | `int64_t` (exact IDs / money cents / counters) |
| `FLAG` | `int` (0/1) |
| `TEXT` | `LukeText { ptr, len }` (arena or literal) |
| `JSON` | `LukeJson *` (arena tree — parse / get / stringify) |
| `LIST` | `LukeList *` (arena-backed text items) |
| `MAP` | `LukeMap *` (arena-backed text keys/values) |
| `SERVER` / `REQUEST` | HTTP server / request handles |
| `DATABASE` | SQLite handle |
| `BLUEPRINT Foo` | `typedef struct Foo { ... } Foo` |

Inference (v0):
- `42` → `INTEGER` (no `.` / exponent); `3.14` → `NUMBER`
- `"hi"` / wordy strings → `TEXT`
- `TRUE` / `FALSE` → `FLAG`
- `ASK jsonParse WITH …` → `JSON`
- `HAS name SET TO "..."` → field `TEXT`
- `HAS count SET TO 0` → field `INTEGER`
- `HAS x AS NUMBER` / `AS INTEGER` / `AS TEXT` / `AS FLAG` / `AS JSON` — explicit when needed
- First assignment to a local fixes its type; later `SET` must match (`INTEGER` widens to `NUMBER`)
- Function args/arity and `GIVE BACK` types are checked
- Optional: `THIS IS FUNCTION f … GIVES BACK TEXT DO`
- Concurrent HTTP: `ASK httpServe WITH server, handler, maxConn` (SO_REUSEPORT multi-loop + handler pools; links `-lpthread`)
- Routing / request shape: `httpMatch`, `httpQueryMap`, `httpHeader` / `httpCookie` / `httpSetCookie`, `httpJson`
- Parameterized SQL: `dbExecBind` / `dbQueryBind` (prefer over string-concat `dbExec` / `dbQuery`)
- Auth: `IMPORT std/auth` — Argon2id passwords (libsodium), `REQUIRE LOGIN`, `THE CURRENT USER`, CSRF; see [`BACKEND_ROADMAP.md`](./BACKEND_ROADMAP.md)
- SSE: `httpSseOpen` / `httpSseData` / `httpSseId` / `httpSseComment`; `httpLastEventId` for resume
- INTEGER rules: see [`INTEGER.md`](./INTEGER.md) — checked overflow, `DIVIDE`→`NUMBER`, widening vs truncating conversion
- Backend track: [`BACKEND_ROADMAP.md`](./BACKEND_ROADMAP.md) · master checklist [`TaskList.md`](../TaskList.md)

### Backend concurrency ceiling

`httpServe` is **N SO_REUSEPORT event loops** (default: one per CPU) plus **per-loop handler pools**:

- **Event-loop I/O** — `epoll` / `kqueue` / `poll`; non-blocking `accept4`, read, `writev`. Edge-triggered on Linux (`EPOLLET`). Idle / half-open clients do not occupy a thread.
- **Multi-core accept** — each loop owns its listen socket with `SO_REUSEPORT` so the kernel load-balances connections (nginx / Go-netpoller model). `LUKE_HTTP_LOOPS` overrides the count.
- **Handler pool** — workers run only after a complete request is buffered. Pools are **per loop** (no global enqueue mutex across cores). `LUKE_HTTP_INLINE=1` runs the handler on the loop thread (REST bench / run-to-completion).
- **Per-request cost** — pooled arenas (`luke_arena_clear`, default first block 8 KiB), embedded `LukeHttpServeJob` on the connection, `TCP_NODELAY` on accept.
- **HTTP/1.1 keep-alive** — default `LUKE_HTTP_KEEPALIVE_MAX=100000` (env `0` = unlimited).
- **Timeouts** — idle `READ`/`WRITE` sweep (`LUKE_HTTP_TIMEOUT_MS`).
- **Graceful stop** — `SIGTERM`/`SIGINT` stop accepts, close idle sockets, drain in-flight handlers.
- **Chunked / SSE** — leave the loop; may block one worker for the stream lifetime.
- **Escape hatches** — `LUKE_HTTP_IO=pool` (legacy blocking recv); see [`DEPLOY.md`](./DEPLOY.md).

Positive `maxConn` is a lifetime **accept** budget across all loops; `maxConn ≤ 0` accepts until signal/failure.

## Memory (Luke words)

| Word | Meaning in Build |
| --- | --- |
| *(default locals)* | Stack / registers |
| `NEW` instances | Allocated in the **thread arena** (bump); live until arena reset/program end |
| `TEXT` concat / dynamic strings | Arena-backed |
| `IN ARENA` … `END ARENA` | Checkpoint + reset bump pointer (scoped bulk free) |
| `DROP` *(roadmap)* | Early release of a single value |

There is **no** mark-sweep collector in Build binaries. Peak memory is predictable: stack + arena high-water mark.

## Blueprints

```luke
BLUEPRINT Dog FOLLOWS Animal DO
  HAS sound SET TO "Woof!"
  WHEN BORN WITH name DO
    SET SELF.name TO name
  END BORN
  METHOD speak DO
    SPEAK SELF.name AND " says " AND SELF.sound
  END METHOD
END CLASS
```

Lowers to roughly:

```c
typedef struct Dog {
  Animal base; /* or flattened parent fields */
  LukeText sound;
} Dog;

void Dog_speak(Dog *self);
Dog *Dog_born(LukeArena *a, LukeText name);
```

- `ASK buddy TO speak` → `Dog_speak(buddy)` (static dispatch when type known)
- `CALL PARENT speak` → `Animal_speak((Animal*)self)`
- `PRIVATE` fields → C name mangling; only methods of that blueprint may touch them
- `ALWAYS HAS` → static/globals on the blueprint’s C file scope

## What Play allows that Build may reject

- Adding undeclared fields at runtime
- Truly dynamic `ASK` on unknown types (Build wants a known blueprint type)
- Unlimited runtime mutation of object shapes
- Relying on GC to clean cycles (Build uses arenas — don’t build immortal graphs by accident)

Play remains for sketching. **Shipping artifacts should `BUILD`.**

## Roadmap toward “cell-graph face + Rust light”

1. ~~Build → C + arena runtime~~
2. ~~Clearer Luke-voice Build errors; `AS TYPE` annotations~~
3. ~~Packages / relative `IMPORT` + `std/files` + `std/json`~~
4. ~~`luke BUILD -target wasm` (WASI)~~
5. ~~Richer typechecking; fuller JSON; thin HTTP GET~~
6. ~~Browser-oriented WASM packaging (`-target browser`)~~
7. ~~Package registry (`IMPORT luke/<name>` + `luke_modules/`)~~
8. ~~SHOW prefers Build; Play VM is the compatibility layer (`--vm`)~~
9. ~~Tooling stdlib (args/env/paths/process) + `luke PKG init`~~
10. ~~Browser JS bridge (`std/js`)~~
11. ~~`IN ARENA` / `END ARENA` scopes~~
12. ~~Remote package registry (`luke PKG install` + `registry/index.json`)~~
13. ~~Foreign imports (`IMPORT c:` + `FOREIGN FUNCTION`)~~
14. ~~Build IR shared frontend (expand/soften for Play; `luke IR` dump)~~
15. ~~LIST / MAP + ATTEMPT / OTHERWISE + `luke TEST`~~
16. ~~`std/server` + `std/sqlite` + browser fetch/click~~
17. ~~`luke PKG publish` + `luke.lock`~~
18. ~~`luke LSP` stdio diagnostics beachhead~~
19. Richer remote registry (signed packages)
20. Explicit Python bridges (beyond C FFI)
21. Optional: emit Play bytecode opcodes directly from Build IR nodes

### Rendering / layout (engine track)

- **Argus (rendering):** [`ARGUS.md`](./ARGUS.md) — scene paint → DOM presentment.
- **Hanka (layout):** [`HANKA.md`](./HANKA.md) — `COLUMN` / `ROW` / `STACK` → frames → Argus.
- **Production web:** [`PRODUCTION_WEB.md`](./PRODUCTION_WEB.md) — inputs, routes, deploy checklist.
- **Reactive (Phases 1–8):** [`REACTIVE.md`](./REACTIVE.md) — full reactive stack shipped.

```luke
IMPORT std/hanka
IMPORT std/argus
BEGIN COLUMN AT 48, 420 SIZE 1184, 280 PAD 0 GAP 16
  SLOT TEXT "brand" SIZE 900, 80 SAY "LukeLang"
END COLUMN
LAY OUT THE SCREEN
PAINT THE SCREEN
```

```bash
luke BUILD examples/build/hanka_demo.luke -target browser -o build/hanka_demo
```

## Philosophy

Conversational syntax is the **UI**.  
The face of a Build app is **cells and reactions** (`REMEMBER` / `WATCH` / `BIND` / `WHEN` / `SPEAK EACH`).  
Build-mode layouts, types, and arenas are the **truth**.  
That split is how Luke can feel like speech and weigh like Rust. Recipes (`WHILE`, mutation `SET`, `THIS IS FUNCTION`) compile; they are not the teaching surface.

## Parser ceiling (known limit)

Build codegen (`vm/src/build_c.cpp`) is **line-based**: one statement per line, `startsWithCI` / keyword scans, not a full lexer + AST.

That is why today’s surface is productive and conversational — and also why multi-line expressions, richer nesting, and flexible punctuation will eventually need a real parse pipeline.

| Today | Ceiling |
| --- | --- |
| Prefix/`findOutsideQuotes` stmt match | Awkward multi-line constructs |
| One line ≈ one stmt | Soft line-wrap / continued statements |
| Hand-rolled expression splits (`AND`, `ADD`, …) | Precedence tables / proper AST |

**Roadmap:** expression parentheses + paren-aware op scan shipped (`stripOuterParens`, `luke_ast.hpp` IR stub). Next: tokenize → Pratt expr AST → lower into Build codegen; then statement AST so LSP/formatter share one tree. Do not reintroduce a parallel JS parser.
