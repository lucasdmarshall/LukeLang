# LukeLang Contributor Guide

Thanks for contributing. The **canonical codebase is `vm/`**.

## Where to work

| Do | Don’t |
| --- | --- |
| Change `vm/`, `docs/`, `examples/build/`, `scripts/` (browser/WASI loaders) | Reintroduce a JS transpiler or `mimo/` |
| Run `cd vm && make test` before landing | Commit `*.obj` / build products |

Legacy JS emitters were removed — see [`LEGACY.md`](./LEGACY.md).

## Workflow

Land work **directly on `main`** (this repo’s preferred flow). Keep commits small and green.

```bash
cd vm && make test
```

CI (GitHub Actions) runs the same `make test` target, including frontend demos and the reactive conformance suite. Debugger checks (`debug_break_step`, `debug_inspect`) need `gdb` on the runner.

## Code style (vm/)

- C++17 in `vm/src/`
- Prefer clear conversational Luke surfaces over clever C++
- Match existing formatting in the file you touch

## Docs

- Build truth: [`BUILD_MODE.md`](./BUILD_MODE.md)
- Getting started must teach `vm/build/luke`
