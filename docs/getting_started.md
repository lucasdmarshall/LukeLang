# Getting Started with LukeLang

LukeLang is **Build-first**: technical syntax → native or WebAssembly via `vm/`.

Both `.lk` and `.luke` are syntax v2. The conversational v1 surface is still available for the
deprecation window behind `luke --syntax=1`, and `luke MIGRATE` rewrites old sources into v2.

## Install

1. Clone the repo and enter it:
   ```bash
   git clone https://github.com/lucasdmarshall/LukeLang.git
   cd LukeLang
   ```
2. Build the Luke toolchain (needs a C++17 compiler + `make`):
   ```bash
   cd vm && make
   ```
3. Optional: for `-target wasm|browser`, install [WASI SDK](https://github.com/WebAssembly/wasi-sdk) under `.tools/wasi-sdk` (or set `LUKE_WASI_SDK`).

Node is only required for headless browser/WASI smoke tests (`scripts/*.cjs`), not for native `BUILD`.

## What LukeLang needs on your machine (and why)

LukeLang is a compiler/runtime toolchain, so requirements are split by target:

- **Required (native backend apps):**
  - C/C++ toolchain (`g++`, `cc`) + `make`
  - Why: `vm/build/luke` is built from C++, then `luke BUILD` emits C and calls the system C compiler.
- **Required for CI debugger tests / debug workflows:**
  - `gdb`
  - Why: `luke DEBUG`, `luke DAP`, and debug smoke tests rely on gdb.
- **Optional (WASM/browser targets):**
  - WASI SDK (`clang`)
  - Why: `-target wasm|browser` compiles generated C to wasm.
- **Optional (headless smoke scripts):**
  - Node.js
  - Why: helper scripts run wasm/browser artifacts in CI-like checks.

So unlike Python/Node/Java where you install one runtime first, LukeLang's primary path is:
**install build tools -> build `luke` -> compile your `.lk` app to a native binary**.

## Your first program

Create `hello.luke`:

```luke
print("Hello, World!")

let name = "Luke"
print("My name is " + name)

fn add(a: float, b: float) -> float {
  return a + b
}

print(add(21, 21))
```

## Run it

From `vm/`:

```bash
./build/luke BUILD ../path/to/hello.luke -o hello && ./hello
# or:
./build/luke SHOW ../path/to/hello.luke
```

You should see:

```text
Hello, World!
My name is Luke
42
```

## Then make something change on its own

Reactive cells are language, not a library. This is the shortest program that shows the idea:

```luke
signal price = 100
signal quantity = 3
derived total = price * quantity

effect on total {
  print("total=" + total)
}

price = 200
```

```text
total=300
total=600
```

You never told `total` to recompute, and you never subscribed to `price`. Declaring the
dependency was the whole instruction. [`REACTIVE.md`](./REACTIVE.md) covers batching, effect
ordering and scopes; [`LIVE_GRAPH.md`](./LIVE_GRAPH.md) extends the same model across the
network, so a database row can reach a pixel the same way.

Browser ship:

```bash
./build/luke PUBLISH WEB ../examples/build/frontend_widgets.luke -o /tmp/luke_web
```

## Next

- Language of record: [`BUILD_MODE.md`](./BUILD_MODE.md)
- Frontend stack: [`FRONTEND_ROADMAP.md`](./FRONTEND_ROADMAP.md)
- Reactive: [`REACTIVE.md`](./REACTIVE.md)
- Syntax v2: [`SYNTAX_V2_SPEC.md`](./SYNTAX_V2_SPEC.md)
- History of removed JS emitters: [`LEGACY.md`](./LEGACY.md)