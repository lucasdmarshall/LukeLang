# Getting Started with LukeLang

LukeLang is **Build-first**: conversational syntax → native / WASM via `vm/`.

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
**install build tools -> build `luke` -> compile your `.luke` app to a native binary**.

## Your first program

Create `hello.luke`:

```luke
SPEAK "Hello, World!"
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
```

## The face (cells and reactions)

Hello-world is a greeting. The **face** of a Build app is cells and reactions — not `WHILE`, mutation `SET`, or `THIS IS FUNCTION`. Print 1 through 20 by naming the range; the compiler writes the loop:

```luke
SPEAK EACH NUMBER FROM 1 TO 20
```

From `vm/`:

```bash
./build/luke BUILD ../examples/build/numbers.luke -o numbers && ./numbers
```

You should see twenty lines, `1` then `2` … then `20`.

Even numbers from the same range — still no `WHILE`. Name the item, filter with `WHERE`. `EVEN` is not a keyword; `IS DIVISIBLE BY` is the same kind of comparison as `EQUALS`:

```luke
SPEAK EACH n FROM 1 TO 20 WHERE n IS DIVISIBLE BY 2
```

```bash
./build/luke BUILD ../examples/build/even_numbers.luke -o even && ./even
```

Ten lines: `2` `4` … `20`. Change `2` to `3` for multiples of 3 — no new syntax.

A reactive screen names cells and reactions the same way:

```luke
REMEMBER count AS NUMBER SET TO 0
WHEN THE BUTTON "inc" IS CLICKED DO
  INCREASE count BY 1
END WHEN
BIND "label" TO count
```

`WATCH` / `BIND` pull a row onto the graph (Live Graph). Recipes still exist for servers and algorithms; they are not the teaching surface. See [`STRATEGY.md`](./STRATEGY.md).

Browser ship:

```bash
./build/luke PUBLISH WEB ../examples/build/frontend_widgets.luke -o /tmp/luke_web
```

## Next

- Language of record: [`BUILD_MODE.md`](./BUILD_MODE.md)
- Frontend stack: [`FRONTEND_ROADMAP.md`](./FRONTEND_ROADMAP.md)
- Reactive: [`REACTIVE.md`](./REACTIVE.md)
- History of removed JS emitters: [`LEGACY.md`](./LEGACY.md)