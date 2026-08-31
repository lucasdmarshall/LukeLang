# LukeLang Language Reference

The syntax v2 surface, which is what `.lk` and `.luke` files use. Every form here is drawn from
[`SYNTAX_V2_SPEC.md`](./SYNTAX_V2_SPEC.md), which is machine-checked against code generation —
if the two disagree, the spec is right and this page is the bug.

The conversational v1 surface is documented in the spec's mapping tables and remains available
behind `luke --syntax=1` for the deprecation window.

## 1. Values and bindings

`let` binds a value you will not reassign. `var` binds one you will.

```luke
let name = "Luke"
var count = 0
count = count + 1
```

Types are optional where the compiler can infer them and required where it cannot — most often
around `+`, which is both arithmetic and concatenation:

```luke
let a: int = 21
var total: float = 0
let greeting: str = "hello"
let ready: bool = false
```

| Type | Holds |
| --- | --- |
| `int` | Exact integer — see [`INTEGER.md`](./INTEGER.md) for the exactness rules |
| `float` | Double-precision number |
| `str` | Text |
| `bool` | `true` / `false` |
| `list` | Ordered values |
| `map` | Key to value |
| `json` | Parsed JSON tree |
| `Server`, `Request`, `Db` | Standard library handles |
| `Foo` | Any `struct` you declared |

A declaration without an initialiser is mutable and needs its type:

```luke
var buffer: str
var items: list = []
var index: map = {}
```

## 2. Operators

| Kind | Operators |
| --- | --- |
| Arithmetic | `+` `-` `*` `/` `%` |
| Comparison | `==` `!=` `<` `>` `<=` `>=` |
| Logical | `&&` `\|\|` `!` |
| Compound assignment | `+=` `-=` |
| Concatenation | `+` on `str` |

`+` needs to know whether it is adding or concatenating. If neither operand's type is known,
the compiler asks you to annotate rather than guessing:

```luke
fn describe(n: float) -> str {
  return "n=" + n          // str + float — concatenation
}
```

`%` has no Build-mode equivalent yet and is rejected with an error.

## 3. Control flow

```luke
if count > 10 {
  print("many")
} else if count > 0 {
  print("some")
} else {
  print("none")
}
```

```luke
while count > 0 {
  print(count)
  count = count - 1
}
```

```luke
for item in items {
  print(item)
}
```

Range loops (`for i in 0..n`) are not part of the surface — use `while`. `break` leaves the
nearest loop.

## 4. Functions

```luke
fn add(a: float, b: float) -> float {
  return a + b
}

fn greet(name: str) {
  print("Hello, " + name)
}

print(add(21, 21))
greet("Luke")
```

The return type is optional; omit `->` when the function returns nothing. Parameters carry
types wherever Build mode needs them, which is most places.

Functions nest, and an inner function captures the enclosing scope:

```luke
fn makeAdder(x: float) {
  fn add(y: float) -> float {
    return x + y
  }
  return add
}
```

## 5. Structs

A `struct` is a blueprint: fields, an `init` constructor, and methods. `self` is the instance.

```luke
struct Animal {
  name: str
  sound = "…"

  init(name: str) {
    self.name = name
  }

  fn speak() {
    print(self.name + " says " + self.sound)
  }
}
```

Fields declare a type (`name: str`) or an initialiser (`sound = "…"`), and `private` keeps a
field or method out of reach from outside:

```luke
struct Vault {
  secret: str
  private fn raw() -> str {
    return self.secret
  }
  fn reveal() -> str {
    return self.raw()
  }
}
```

### Inheritance

```luke
struct Dog : Animal {
  sound = "Woof!"

  fn speak() {
    super.speak()
    print(self.name + " wags happily.")
  }
}
```

`super.m()` calls the parent's implementation. Construct with the struct name:

```luke
let buddy = Dog("Rex")
buddy.speak()
```

Instances live in an arena released at scope exit — there is no garbage collector on the Build
path. `drop(x)` releases one early.

Multiple inheritance and `trait` contracts exist only on the Play VM path today, using the
conversational surface; see [`advanced_topics.md`](./advanced_topics.md).

## 6. Collections

```luke
var xs: list = []
xs.push("Ada")
print(xs[0])
print(xs.len())
print(xs.last())
xs.remove(0)
```

```luke
var m: map = {}
m["name"] = "Ada"
print(m["name"])
if m.has("name") {
  print("present")
}
```

## 7. Errors

```luke
try {
  risky()
} catch (e) {
  print("caught: " + e)
}

throw "not allowed"
assert count == 5
```

## 8. Reactive cells

Cells are language, not a library. This is the part of LukeLang that is genuinely different, so
[`REACTIVE.md`](./REACTIVE.md) covers it in depth.

```luke
signal price = 100
signal quantity = 3
derived total = price * quantity

effect on total {
  print("total=" + total)
}

price = 200              // total=600

batch {
  price = 120
  quantity = 5           // still one flush
}
```

| Form | Means |
| --- | --- |
| `signal c = e` | A cell |
| `secret signal c = e` | A cell the compiler refuses to bind into a page |
| `derived t = expr` | Recomputed when its inputs change |
| `effect on c { … }` | Runs when `c` changes |
| `effect background on c { … }` | Same, at background priority |
| `effect weak on c { … }` | Same, without keeping `c` alive |
| `batch { … }` | Many writes, one flush |
| `c.value()` | Read without subscribing |
| `c.deps()` / `c.subs()` / `c.id()` | Graph introspection |
| `why(c)` | Trace what last wrote to `c` |

## 9. Modules

```luke
import std/server         // standard library
import "./critter.lk"     // sibling file
import luke/greeter       // package from luke_modules/
```

`std/*` resolves from `vm/stdlib/`. Relative paths resolve next to the importing file. See
[`standard_library.md`](./standard_library.md) for the module list and
[`BUILD_MODE.md`](./BUILD_MODE.md) for package resolution.

## 10. Live Graph

The same cell model crosses the network. A server cell can be backed by a database row and
pushed to clients; a client cell can be backed by that stream and bound to an element.

```luke
// server
watch user from db where "id = 1"
push watch user on req for 50 beats every 50 ms

// client
signal user = ""
bind("name", user)
watch user from "http://127.0.0.1:8798/watch"
```

An external `UPDATE` becomes one push, one reactive write, and exactly one repainted region.
[`LIVE_GRAPH.md`](./LIVE_GRAPH.md) has the wire format, incremental view maintenance and
resume-from-log.

## 11. Memory

```luke
arena {
  let big = buildSomething()
  print(big)
}                          // released here
```

Build mode allocates from an arena and frees at scope exit. Nothing is reference-counted and
nothing is collected in the background. [`BUILD_MODE.md`](./BUILD_MODE.md) covers the details.

## 12. Tests and benchmarks

```luke
test "addition" {
  assert add(2, 3) == 5
}

bench.sample(elapsed)
```

Run with `luke TEST file.lk`.

## 13. Escaping to the conversational surface

Some forms — contracts, statics, layout phrases — are not on the v2 surface yet. `raw` passes a
line through untouched:

```luke
raw "CONTRACT Eater DO"
raw "MUST METHOD eat WITH food"
raw "END CONTRACT"

raw """WHEN "cta" IS CLICKED DO
  OPEN THE MODAL "dlg"
END WHEN"""
```

`luke MIGRATE old.luke` rewrites a conversational source into v2 and marks anything it could
not translate with `TODO(migrate)`.

---

Next: [`BUILD_MODE.md`](./BUILD_MODE.md) for the language of record ·
[`SYNTAX_V2_SPEC.md`](./SYNTAX_V2_SPEC.md) for the normative surface ·
[`REACTIVE.md`](./REACTIVE.md) for the reactive engine
