<div align="center">

<img src="assets/lukelang-logo.png" alt="LukeLang" width="440" />

### Myanmar's first official programming language

**Reactive-native. Full-stack. Compiled.**

Change finds its own way — from database row to screen pixel.

[![Website](https://img.shields.io/badge/lukelang.org-0B3D28?style=flat-square&labelColor=0B3D28&color=19b96f)](https://lukelang.org)
[![Documentation](https://img.shields.io/badge/docs-40%20documents-19b96f?style=flat-square&labelColor=0B3D28)](https://lukelang.org/docs/)
[![VS Code](https://img.shields.io/badge/VS%20Code-extension-19b96f?style=flat-square&labelColor=0B3D28)](https://marketplace.visualstudio.com/items?itemName=lukelang.lukelang-vscode)
[![CI](https://img.shields.io/github/actions/workflow/status/lucasdmarshall/LukeLang/ci.yml?branch=main&style=flat-square&label=CI&labelColor=0B3D28)](https://github.com/lucasdmarshall/LukeLang/actions/workflows/ci.yml)
[![Status](https://img.shields.io/badge/status-operational-19b96f?style=flat-square&labelColor=0B3D28)](https://status.lukelang.org)

[Website](https://lukelang.org) ·
[Learn](https://lukelang.org/learn/) ·
[Documentation](https://lukelang.org/docs/) ·
[Examples](https://lukelang.org/examples/) ·
[Download](https://lukelang.org/download/) ·
[Community](https://lukelang.org/community/)

</div>

---

## Why LukeLang

Mainstream stacks glue **three worlds** together with fetch calls, cache invalidation,
subscriptions and diffing. Every one of those is a place your app can be wrong.

LukeLang is **one dependency graph** that the compiler can see, end to end:

```
database row  →  server cell  →  [wire]  →  client cell  →  pixel
     └────────────── one graph, compiler-known ──────────────┘
```

You never fetch. Never invalidate. Never subscribe. Never diff. You declare the dependency
once, and exactly one region repaints when the row changes.

That substrate is the **[Live Graph](https://lukelang.org/docs/live-graph/)**.

## Hello, world

```luke
print("Hello from LukeLang")

let name = "Luke"
print("My name is " + name)

fn add(a: float, b: float) -> float {
  return a + b
}

print(add(21, 21))
```

```bash
luke BUILD hello.lk -o hello && ./hello
```

That compiles to C, then to a native binary with **arena memory and no garbage collector**.

## Reactive cells are language, not library

```luke
signal price = 100
signal quantity = 3
derived total = price * quantity

effect on total {
  print("total=" + total)
}

price = 200            // total=600

batch {
  price = 120
  quantity = 5         // still one flush
}
```

Flush order, deduplication, error isolation and scope collection are specified in
[`REACTIVE_SPEC.md`](https://lukelang.org/docs/reactive-spec/) and pinned by fourteen
conformance programs in CI.

## A row becomes a pixel

**Server** — a cell backed by a database row, streamed to a client:

```luke
import std/server
import std/sqlite

let db = dbOpen("app.db")
watch user from db where "id = 1"

let server = httpListen(8798)
let req = httpAccept(server)
push watch user on req for 50 beats every 50 ms
```

**Client** — a cell backed by that stream, bound to an element:

```luke
import std/js

signal user = ""
bind("name", user)
watch user from "http://127.0.0.1:8798/watch"
```

Run `UPDATE users SET name = 'Ada' WHERE id = 1` from any other process. The browser
repaints one region. There is no fetch call, no cache key and no virtual DOM diff anywhere
in that path.

## Install

```bash
# Linux / macOS / WSL — mimo injects luke onto your PATH
curl -fsSL https://lukelang.org/mimo | bash

# Windows (PowerShell)
irm https://lukelang.org/mimo.ps1 | iex
```

```bash
mimo inject lukelang          # or lukelang@0.3.0
luke BUILD hello.lk -o hello && ./hello
```

Covers Linux, macOS and Windows — Intel, Apple Silicon, ARM, 32-bit and 64-bit.
Details and the contributor source build are on
[download](https://lukelang.org/download/).

| Command | What you get |
| --- | --- |
| `luke BUILD file.lk` | Native binary, WebAssembly, or a browser page — no GC |
| `luke SHOW file.lk` | Build when possible, Play VM fallback |
| `luke SHOW file.lk --vm` | Bytecode VM with GC (compatibility layer) |
| `luke MIGRATE file.luke` | Conversational v1 source rewritten as syntax v2 |
| `luke FMT file.lk` | Format, round-trip safe and checked in CI |
| `luke LSP` / `luke DAP` | Language server and debug adapter, over stdio |
| `luke PKG init\|install\|publish\|lock` | Package management into `luke_modules/` |

Targets: native C, WebAssembly (WASI), and the browser (`-target browser` emits a page with
the runtime inlined).

## Editor support

```bash
code --install-extension lukelang.lukelang-vscode
```

Syntax highlighting, the language server (hover, diagnostics, definitions, symbols, rename,
formatting), and gdb-backed debugging with a Reactive scope that shows cells and their
dependency edges. Source in [`tools/vscode/lukelang`](tools/vscode/lukelang).

## What ships today

| Layer | State |
| --- | --- |
| **Syntax v2** | `fn`, `let`, `struct`, braces, operators — default for `.lk` and `.luke` |
| **Build → native C** | Functions, structs, imports, typechecks, arena memory, no GC |
| **Build → WebAssembly** | `-target wasm` (WASI) · `-target browser` (page + runtime) |
| **Reactive engine** | Cells, derived, effects, lists, maps, batching — 14 conformance programs |
| **Live Graph** | `watch … from db`, `push watch`, SSE ordering, incremental views, resume-from-log, time travel |
| **HTTP** | Event-loop I/O, keep-alive, `SO_REUSEPORT`, chunked responses, graceful shutdown |
| **Databases** | SQLite with pooling and statement cache; Postgres via libpq with the Slipstream pipelined executor |
| **Auth** | Argon2id passwords, sessions, CSRF — with rules the compiler enforces |
| **Frontend** | Argus reactive patcher, Hanka layout → DOM/CSS, breakpoints, accessibility |
| **Tooling** | LSP, DAP, formatter, migrator, package manager, VS Code extension |
| **Tests** | 118 examples compiled and asserted on every commit |

Runnable programs live in [`examples/build/`](examples/build). Play-VM-only demos are in
[`examples/native/`](examples/native).

## Design principles

1. **Be different where it matters** — the reactive model and the Live Graph, not an
   unfamiliar syntax. The surface should read like the languages you already know.
2. **Build is the language of record** — types, layouts and arenas are decided at compile
   time. The Play VM is a skateboard, not the product.
3. **The browser is the renderer** — compile to DOM and CSS, and patch the smallest region
   that changed.
4. **One beachhead** — win reactive full-stack first. Mobile, game and canvas tracks are
   parked until that is earned.

The full decision record, including the reversals, is in
[`STRATEGY.md`](https://lukelang.org/docs/strategy/).

## Documentation

All 40 documents are hosted at **[lukelang.org/docs](https://lukelang.org/docs/)** and live in
[`docs/`](docs) as Markdown.

| Start here | |
| --- | --- |
| [Getting started](https://lukelang.org/docs/getting-started/) | First program, all three execution paths |
| [Build mode](https://lukelang.org/docs/build-mode/) | Types, memory, imports, packages, packaging |
| [Syntax v2 specification](https://lukelang.org/docs/syntax-v2-spec/) | The normative surface, machine-checked against codegen |
| [Live Graph](https://lukelang.org/docs/live-graph/) | The row-to-pixel thesis and the wire |
| [Reactive](https://lukelang.org/docs/reactive/) | Cells, derived values, effects, batching |
| [Backend roadmap](https://lukelang.org/docs/backend-roadmap/) | HTTP, routes, forms, migrations |
| [Auth](https://lukelang.org/docs/auth/) | Secure by compiler, not by review |
| [Editor tooling](https://lukelang.org/docs/editor-tooling/) | LSP, DAP, extension |
| [Scorecard](https://lukelang.org/docs/scorecard/) | What is proven, provisional, or parked |

Working on LukeLang with an AI agent? Start at [`AGENTS.md`](AGENTS.md).

## Contributing

Get a green baseline first, so any failure afterwards is yours:

```bash
cd vm && make && make test
```

Then read the [contributor guide](https://lukelang.org/docs/contributor-guide/). New
behaviour arrives with an example under `examples/` that CI compiles and asserts on — there
is usually no second reviewer, so the automated gates *are* the review.

Bugs and questions: [issues](https://github.com/lucasdmarshall/LukeLang/issues) ·
[discussions](https://github.com/lucasdmarshall/LukeLang/discussions) ·
[community](https://lukelang.org/community/)

People who have helped are credited in [`CONTRIBUTORS.md`](CONTRIBUTORS.md).

---

<div align="center">

Designed and developed by **Kaung Myat San** · Myanmar

[lukelang.org](https://lukelang.org) · [status](https://status.lukelang.org)

</div>
