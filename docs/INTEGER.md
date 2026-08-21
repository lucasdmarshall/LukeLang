# INTEGER semantics (Build mode)

Nail these rules before money / IDs / counters rely on them.

## Representation

| Luke | C | Notes |
| --- | --- | --- |
| `INTEGER` | `int64_t` | Exact in the full signed 64-bit range |
| `NUMBER` | `double` | IEEE-754; integers above 2⁵³ are not exact |

## Literals

- `42` → `INTEGER` (no `.` / `e` / `E`)
- `3.14` / `1e9` → `NUMBER`
- Out-of-range integer literals are a **Build error**

## Arithmetic

| Op | INTEGER ⊕ INTEGER | Mixed / NUMBER |
| --- | --- | --- |
| `ADD` / `SUBTRACT` / `MULTIPLY` | stays `INTEGER` (checked) | promote to `NUMBER` |
| `DIVIDE` / `DIVIDED BY` | always `NUMBER` | `NUMBER` |

**Overflow:** `INTEGER` add/sub/mul uses checked ops (`luke_i64_*`). Overflow **aborts** the process with `INTEGER error: … overflow` — silent wrap is forbidden for money/IDs.

## Conversions

- **INTEGER → NUMBER** — implicit widening allowed. Values with |n| > 2⁵³ may lose precision in the `double`.
- **NUMBER → INTEGER** — allowed on assignment / `AS INTEGER` via `luke_number_to_integer`: truncates toward zero; **aborts** if non-finite or outside `int64` range. Not a silent bit-cast.

## Comparisons

- INTEGER vs INTEGER: exact
- Mixed INTEGER/NUMBER: INTEGER is widened to `double` first (precision caveat above)
- `n IS DIVISIBLE BY d` → `FLAG` (`luke_i64_divisible` / `luke_divisible`; divisor `0` is false)

## Reactive cells

- `REMEMBER x AS INTEGER` stores an exact int64 cell
- `INCREASE` on INTEGER cells uses checked add
- `TIMELINE INTO` requires a **NUMBER** cell (fractional progress)

## What this is not

- No arbitrary-precision integers
- No saturating arithmetic
- JSON parses whole numbers as `LUKE_JSON_INTEGER` (`int64_t`) when they fit; fractions/exponents stay `NUMBER`. Use `jsonAsInteger` / stringify for exact round-trip past 2⁵³ (`examples/build/json_integer.luke`).
