CXX ?= g++
CC ?= cc
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS ?=

# Auto-generate header dependencies so a changed .hpp/.h rebuilds every object
# that includes it. Without this, an incremental build after a header change
# (e.g. a new field in a struct shared across translation units) reuses stale
# objects compiled against the old layout — an ODR mismatch that silently
# corrupts the heap. Kept out of CXXFLAGS so overriding CXXFLAGS never disables
# dependency tracking.
DEPFLAGS := -MMD -MP

SRCS := \
	src/value.cpp \
	src/heap.cpp \
	src/bytecode.cpp \
	src/vm.cpp \
	src/compiler.cpp \
	src/luke_expr.cpp \
	src/luke_parse.cpp \
	src/build_c.cpp \
	src/lsp.cpp \
	src/dap.cpp \
	src/main.cpp

OBJS := $(SRCS:src/%.cpp=build/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean test test-play test-build test-wall pressure test-fmt-roundtrip test-debug test-inspect test-dap test-lsp

all: build/luke

build/luke: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

build:
	mkdir -p build

-include $(DEPS)

test: test-play test-build

test-fmt-roundtrip: build/luke
	bash ../scripts/fmt_roundtrip_all.sh

test-debug: build/luke
	bash ../scripts/debug_break_step.sh

test-inspect: build/luke
	bash ../scripts/debug_inspect.sh

test-dap: build/luke
	bash ../scripts/dap_handshake.sh

test-lsp: build/luke
	bash ../scripts/lsp_providers.sh

test-play: build/luke
	./build/luke SHOW ../examples/native/hello.luke --vm
	./build/luke SHOW ../examples/native/counter.luke --vm
	./build/luke SHOW ../examples/native/functions.luke --vm
	./build/luke SHOW ../examples/native/oop.luke --vm
	./build/luke SHOW ../examples/native/advanced_oop.luke --vm
	./build/luke SHOW ../examples/native/closures.luke --vm
	./build/luke SHOW ../examples/native/privacy.luke --vm
	@# Build-first SHOW (no --vm) for a Build program
	./build/luke SHOW ../examples/build/hello.luke
	@# Play-only feature falls back to VM
	./build/luke SHOW ../examples/native/closures.luke

test-build: build/luke
	./build/luke BUILD ../examples/build/hello.luke -o build/ex_hello
	./build/ex_hello
	@# Source maps: #line N "file.luke" per statement (gdb/lldb → .luke)
	@grep -qE '#line 3 ".*hello\.luke"' build/ex_hello.luke.c
	@grep -qE '#line 11 ".*hello\.luke"' build/ex_hello.luke.c
	@test $$(grep -c '^#line ' build/ex_hello.luke.c) -ge 7
	@readelf --debug-dump=line build/ex_hello 2>/dev/null | grep -q 'hello\.luke'
	@# Cell-graph face: SPEAK EACH NUMBER FROM — compiler writes the loop
	./build/luke BUILD ../examples/build/numbers.luke -o build/ex_numbers
	./build/ex_numbers | tee /tmp/luke_numbers.txt
	@test $$(wc -l < /tmp/luke_numbers.txt) -eq 20
	@test "$$(head -n1 /tmp/luke_numbers.txt)" = "1"
	@test "$$(tail -n1 /tmp/luke_numbers.txt)" = "20"
	@# Cell-graph face: SPEAK EACH n FROM … WHERE n IS DIVISIBLE BY 2
	./build/luke BUILD ../examples/build/even_numbers.luke -o build/ex_even_numbers
	./build/ex_even_numbers | tee /tmp/luke_even_numbers.txt
	@test $$(wc -l < /tmp/luke_even_numbers.txt) -eq 10
	@test "$$(head -n1 /tmp/luke_even_numbers.txt)" = "2"
	@test "$$(tail -n1 /tmp/luke_even_numbers.txt)" = "20"
	./build/luke BUILD ../examples/build/counter.luke -o build/ex_counter
	./build/ex_counter
	./build/luke BUILD ../examples/build/functions.luke -o build/ex_functions
	./build/ex_functions
	@# Debugger: breakpoints by .luke:line + step over/into/out (gdb)
	@bash ../scripts/debug_break_step.sh | tee /tmp/luke_debug_break_step_make.txt | grep -q "debug_break_step_ok=1"
	@# Debugger: inspect reactive cells + dependency edges
	@bash ../scripts/debug_inspect.sh | tee /tmp/luke_debug_inspect_make.txt | grep -q "debug_inspect_ok=1"
	@# Debugger: DAP stdio handshake (initialize → initialized)
	@bash ../scripts/dap_handshake.sh | tee /tmp/luke_dap_handshake_make.txt | grep -q "dap_handshake_ok=1"
	@# LSP: hover types/signatures + providers + context completion + didChange
	@bash ../scripts/lsp_providers.sh | tee /tmp/luke_lsp_providers_make.txt | grep -q "lsp_providers_ok=1"
	./build/luke BUILD ../examples/build/oop.luke -o build/ex_oop
	./build/ex_oop
	./build/luke BUILD ../examples/build/modules.luke -o build/ex_modules
	./build/ex_modules
	@grep -qE '#line 5 ".*modules\.luke"' build/ex_modules.luke.c
	@grep -qE '#line .* ".*critter\.luke"' build/ex_modules.luke.c
	@grep -qE '#line .* ".*files\.luke"' build/ex_modules.luke.c
	./build/luke BUILD ../examples/build/packages.luke -o build/ex_packages
	./build/ex_packages
	./build/luke BUILD ../examples/build/tooling.luke -o build/ex_tooling
	./build/ex_tooling
	./build/luke BUILD ../examples/build/arena_scope.luke -o build/ex_arena
	./build/ex_arena
	./build/luke BUILD ../examples/build/ffi_math.luke -o build/ex_ffi
	./build/ex_ffi
	@rm -rf ../luke_modules/echo
	./build/luke PKG install echo
	./build/luke BUILD ../examples/build/registry_echo.luke -o build/ex_reg
	./build/ex_reg
	./build/luke IR ../examples/build/functions.luke >/tmp/luke_ir.txt
	@grep -q "luke-build-ir" /tmp/luke_ir.txt
	./build/luke SHOW ../examples/native/play_import.luke --vm
	./build/luke BUILD ../examples/build/json_demo.luke -o build/ex_json
	./build/ex_json
	./build/luke BUILD ../examples/build/http_demo.luke -o build/ex_http
	./build/ex_http
	@mkdir -p build/http_fixture && cp ../examples/build/luke.json build/http_fixture/luke.json
	@python3 -m http.server 8765 --directory build/http_fixture >/tmp/luke_http_server.log 2>&1 & echo $$! > /tmp/luke_http_server.pid; sleep 0.4
	./build/luke BUILD ../examples/build/http_json.luke -o build/ex_http_json
	./build/ex_http_json
	@kill `cat /tmp/luke_http_server.pid` 2>/dev/null || true
	./build/luke BUILD ../examples/build/collections.luke -o build/ex_collections
	./build/ex_collections | grep -q "caught:"
	./build/luke TEST ../examples/build/collections_test.luke
	./build/luke BUILD ../examples/build/sqlite_demo.luke -o build/ex_sqlite
	./build/ex_sqlite | grep -q "hello from LukeLang"
	./build/luke PKG lock
	@test -f ../luke.lock
	@# negative typechecks must fail
	@./build/luke BUILD ../examples/build/bad_types.luke -o build/bad_types 2>/tmp/luke_bad_types.txt; test $$? -ne 0
	@grep -qE "wants TEXT but got (NUMBER|INTEGER)" /tmp/luke_bad_types.txt
	@./build/luke BUILD ../examples/build/bad_arity.luke -o build/bad_arity 2>/tmp/luke_bad_arity.txt; test $$? -ne 0
	@grep -q "expects 2 arguments" /tmp/luke_bad_arity.txt
	./build/luke BUILD ../examples/build/hello_wasm.luke -target wasm -o build/ex_hello.wasm
	node ../scripts/run_wasi.cjs build/ex_hello.wasm
	./build/luke BUILD ../examples/build/hello_browser.luke -target browser -o build/ex_browser
	node ../scripts/luke_browser_loader.cjs build/ex_browser.wasm
	@test -f build/ex_browser.html
	@node ../scripts/luke_browser_loader.cjs build/ex_browser.wasm | grep -q "Hello from Luke in the browser"
	./build/luke BUILD ../examples/build/hello_js.luke -target browser -o build/ex_js
	@node ../scripts/luke_browser_loader.cjs build/ex_js.wasm | grep -q "jsSetText"
	@mkdir -p ../sample/dist
	./build/luke BUILD ../sample/landing.luke -target browser -o ../sample/dist/landing
	@grep -q '@font-face' ../sample/dist/landing.html
	@grep -q 'luke_when_' ../sample/dist/landing.html
	@test -f ../sample/dist/fonts/syne-700.woff2
	@test ! -f ../sample/dist/luke_browser_loader.js
	@node ../scripts/luke_browser_loader.cjs ../sample/dist/landing.wasm >/tmp/luke_sample_boot.txt
	./build/luke BUILD ../examples/build/argus_demo.luke -target browser -o build/argus_demo
	@node ../scripts/luke_browser_loader.cjs build/argus_demo.wasm 2>&1 | grep -q "argus_upsert"
	./build/luke BUILD ../examples/build/hanka_demo.luke -target browser -o build/hanka_demo
	@node ../scripts/luke_browser_loader.cjs build/hanka_demo.wasm 2>&1 | grep -q "argus_upsert"
	./build/luke BUILD ../examples/build/web_app.luke -target browser -o build/web_app
	@node ../scripts/luke_browser_loader.cjs build/web_app.wasm 2>&1 | grep -q "route_go"
	@grep -q 'event:"route"' build/web_app.html
	@grep -q 'event:"fetch"' build/web_app.html
	@grep -qE 'hanka_slot_input|argus_place_input' build/web_app.luke.c
	@grep -q '_luke_fe_i' build/web_app.luke.c
	@grep -q 'hanka_begin_row' build/web_app.luke.c
	./build/luke BUILD ../examples/build/hanka_align.luke -o build/ex_hanka_align
	./build/ex_hanka_align | grep -q "align=1"
	./build/ex_hanka_align | grep -q "axes=1"
	./build/ex_hanka_align | grep -q "measure="
	@grep -q 'hanka_set_align' build/ex_hanka_align.luke.c
	@grep -q 'hanka_set_align_axes' build/ex_hanka_align.luke.c
	./build/luke BUILD ../examples/build/frontend_widgets.luke -o build/ex_frontend_widgets
	./build/ex_frontend_widgets | grep -q "ok=1"
	./build/ex_frontend_widgets | grep -q "a11y=1"
	./build/ex_frontend_widgets | grep -q "vw="
	@grep -qE 'hanka_slot_select|hanka_slot_table|hanka_slot_modal' build/ex_frontend_widgets.luke.c
	@grep -q 'hanka_set_align_axes' build/ex_frontend_widgets.luke.c
	@grep -q 'argus_js_announce\|argus_js_focus_trap\|argus_js_focus_restore' build/ex_frontend_widgets.luke.c
	./build/luke PUBLISH WEB ../examples/build/frontend_widgets.luke -o build/frontend_widgets_pub
	@test -f build/frontend_widgets_pub.html
	@test -f build/frontend_widgets_pub.wasm
	@grep -q 'argus_a11y\|measure_text\|argus_select' build/frontend_widgets_pub.html
	@grep -q 'argus_focus_trap\|argus_announce' build/frontend_widgets_pub.html
	./build/luke BUILD ../examples/build/frontend_wrap_forms.luke -o build/ex_frontend_wrap_forms
	./build/ex_frontend_wrap_forms | grep -q "wrap=1"
	./build/ex_frontend_wrap_forms | grep -q "vh="
	@grep -q 'hanka_set_wrap' build/ex_frontend_wrap_forms.luke.c
	@grep -qE 'inputType|, 3\)|, 4\)' build/ex_frontend_wrap_forms.luke.c
	@grep -q 'argus_fade_to' build/ex_frontend_wrap_forms.luke.c
	./build/luke BUILD ../examples/build/frontend_wrap_forms.luke -target browser -o build/frontend_wrap_forms
	@grep -q 'event:"viewport"' build/frontend_wrap_forms.html
	@grep -q 'event:"breakpoint"' build/frontend_wrap_forms.html
	@grep -q 'min:800' build/frontend_wrap_forms.html
	@grep -q 'max:480' build/frontend_wrap_forms.html
	@grep -q 'min:480:max:799' build/frontend_wrap_forms.html
	@grep -q 'viewport_height\|argus_fade\|now_ms' build/frontend_wrap_forms.html
	@node ../scripts/luke_browser_loader.cjs build/frontend_wrap_forms.wasm 2>&1 | grep -q "argus_upsert\|ok=1\|wrap=1"
	./build/luke BUILD ../examples/build/frontend_done.luke -o build/ex_frontend_done
	./build/ex_frontend_done | grep -q "frontend_done=1"
	@grep -q 'hanka_set_stack_below' build/ex_frontend_done.luke.c
	@grep -q 'hanka_set_keep_roots' build/ex_frontend_done.luke.c
	@grep -q 'hanka_begin_grid' build/ex_frontend_done.luke.c
	@grep -q 'hanka_set_scroll' build/ex_frontend_done.luke.c
	@grep -q 'hanka_leaf_set_class\|hanka_leaf_set_live' build/ex_frontend_done.luke.c
	@grep -q 'argus_open_modal' build/ex_frontend_done.luke.c
	./build/luke BUILD ../examples/build/frontend_done.luke -target browser -o build/frontend_done
	@grep -q 'below:640' build/frontend_done.html
	@grep -q 'above:1000' build/frontend_done.html
	@grep -q 'luke_viewport_relayout\|argus_grid\|argus_class\|argus_modal_open\|argus_live\|argus_scroll' build/frontend_done.html
	@grep -q 'hanka_set_keep_roots' build/frontend_done.luke.c
	@# STACK BELOW must reflow on resize (keep_roots); modal must NOT open on mount
	@node -e 'globalThis.__lukeViewportWidth=800;const {runLukeWasm}=require("../scripts/luke_browser_loader.cjs");const fs=require("fs");const html=fs.readFileSync("build/frontend_done.html","utf8");const m=html.match(/var LUKE_WHENS = (\[[\s\S]*?\]);/);const whens=Function("return ("+m[1]+");")();const logs=[];const orig=console.log;console.log=function(){logs.push(Array.prototype.join.call(arguments," "));return orig.apply(console,arguments);};runLukeWasm(fs.readFileSync("build/frontend_done.wasm"),{whens}).then(()=>{const flex=()=>logs.filter(l=>l.indexOf("[argus_flex] __hk_0")===0);if(!flex().some(l=>/\s2\s/.test(l)))throw new Error("expected row at 800: "+flex().join("|"));if(logs.some(l=>l.indexOf("[argus_modal_open]")>=0))throw new Error("modal opened on mount");globalThis.__lukeViewportWidth=500;if(globalThis.__lukeDispatchViewport)globalThis.__lukeDispatchViewport();return new Promise(r=>setTimeout(r,80));}).then(()=>{const flex=()=>logs.filter(l=>l.indexOf("[argus_flex] __hk_0")===0);if(!flex().some(l=>/\s1\s/.test(l)))throw new Error("expected column after resize to 500: "+flex().join("|"));console.log=orig;console.log("reflow_ok");process.exit(0);}).catch(e=>{console.log=orig;console.error(e);process.exit(1);});' 2>&1 | tee /tmp/luke_fe_reflow.txt | grep -q reflow_ok
	@node ../scripts/luke_browser_loader.cjs build/frontend_done.wasm --dispatch cta:click 2>&1 | tee /tmp/luke_fe_modal.txt | grep -q '\[argus_modal_open\]'
	@! grep -q '\[argus_modal_open\]' /tmp/luke_fe_reflow.txt
	./build/luke BUILD ../examples/build/frontend_stress.luke -o build/ex_frontend_stress
	./build/ex_frontend_stress | grep -q "nodes=100"
	./build/ex_frontend_stress | grep -q "ms="
	./build/ex_frontend_stress | grep -q "ok=1"
	./build/luke BUILD ../examples/build/reactive_core.luke -o build/ex_reactive
	./build/ex_reactive | grep -q "total=300"
	./build/ex_reactive | grep -q "total=400"
	./build/ex_reactive | grep -q "total=500"
	./build/luke BUILD ../examples/build/reactive_cycle.luke -o build/ex_reactive_cycle
	@./build/ex_reactive_cycle 2>/tmp/luke_rx_cycle.txt >/dev/null; grep -q "cycle detected" /tmp/luke_rx_cycle.txt
	./build/luke BUILD ../examples/build/reactive_greeting.luke -target browser -o build/reactive_greeting
	@grep -q 'luke_rx_ui_set_text\|luke_rx_ui_enable' build/reactive_greeting.luke.c
	@grep -q 'luke_rx_write_text\|luke_rx_cell_text' build/reactive_greeting.luke.c
	@! grep -q 'argus_clear' build/reactive_greeting.luke.c || ! grep -A20 'luke_when_' build/reactive_greeting.luke.c | grep -q 'argus_clear'
	@node ../scripts/luke_browser_loader.cjs build/reactive_greeting.wasm --dispatch go:click 2>&1 | tee /tmp/luke_rx_ui.txt | grep -q 'region=1'
	@grep -q 'Welcome, Alex' /tmp/luke_rx_ui.txt
	@grep -q '\[argus_flex\]' /tmp/luke_rx_ui.txt
	@grep -q '\[argus_flow_frame\]' /tmp/luke_rx_ui.txt
	@grep -q '\[argus_parent\].*greeting' /tmp/luke_rx_ui.txt
	@! grep -q '\[argus_clear\]' /tmp/luke_rx_ui.txt
	./build/luke BUILD ../examples/build/reactive_counter_scope.luke -o build/ex_rx_scope
	./build/ex_rx_scope | grep -q "count=3"
	./build/ex_rx_scope | grep -q "doubled=6"
	./build/ex_rx_scope | grep -q "disposed"
	./build/luke BUILD ../examples/build/reactive_counter.luke -target browser -o build/reactive_counter
	@grep -q 'luke_rx_scope_begin\|luke_rx_scope_end' build/reactive_counter.luke.c
	@node ../scripts/luke_browser_loader.cjs build/reactive_counter.wasm --dispatch counter-inc:click 2>&1 | tee /tmp/luke_rx_counter.txt | grep -q 'Count: 1'
	@node ../scripts/luke_browser_loader.cjs build/reactive_counter.wasm --dispatch counter-destroy:click 2>&1 | grep -q 'Counter scope destroyed'
	./build/luke BUILD ../examples/build/reactive_fetch.luke -target browser -o build/reactive_fetch
	@grep -q 'luke_rx_write_text.*fetch_body\|luke_js_fetch_body' build/reactive_fetch.luke.c
	@grep -q 'luke_rx_batch_begin' build/reactive_fetch.luke.c
	@node -e 'const {runLukeWasm}=require("../scripts/luke_browser_loader.cjs");const fs=require("fs");const html=fs.readFileSync("build/reactive_fetch.html","utf8");const m=html.match(/var LUKE_WHENS = (\[[\s\S]*?\]);/);const whens=Function("return ("+m[1]+");")();runLukeWasm(fs.readFileSync("build/reactive_fetch.wasm"),{whens}).then(()=>{globalThis.__lukeDispatch("load","click");return new Promise(r=>setTimeout(r,80));}).then(()=>process.exit(0));' 2>&1 | tee /tmp/luke_rx_fetch.txt | grep -q 'status: 200'
	@grep -q 'stub/profile' /tmp/luke_rx_fetch.txt
	./build/luke BUILD ../examples/build/reactive_list.luke -o build/ex_rx_list
	./build/ex_rx_list | grep -q "n=2"
	./build/ex_rx_list | grep -q "item=Zoe"
	./build/ex_rx_list | grep -q "delta=1"
	./build/ex_rx_list | grep -q "Ada=10"
	./build/ex_rx_list | grep -q "fe=Zoe"
	@grep -q 'luke_rx_list_set' build/ex_rx_list.luke.c
	@grep -q 'luke_rx_ui_paint_list' build/ex_rx_list.luke.c
	./build/luke BUILD ../examples/build/reactive_list_ui.luke -target browser -o build/reactive_list_ui
	@grep -q 'luke_rx_ui_paint_list\|luke_rx_list_set' build/reactive_list_ui.luke.c
	@node ../scripts/luke_browser_loader.cjs build/reactive_list_ui.wasm --dispatch rename:click 2>&1 | tee /tmp/luke_rx_list.txt | grep -q 'Zoe'
	@! grep -q '\[argus_clear\]' /tmp/luke_rx_list.txt
	./build/luke BUILD ../examples/build/reactive_timeline.luke -o build/ex_rx_timeline
	./build/ex_rx_timeline | grep -q "progress=1"
	@grep -q 'luke_rx_timeline_run_sync' build/ex_rx_timeline.luke.c
	./build/luke BUILD ../examples/build/reactive_timeline_ui.luke -target browser -o build/reactive_timeline_ui
	@grep -q 'luke_timeline_progress\|BIND OPACITY\|luke_rx_ui_set_opacity' build/reactive_timeline_ui.luke.c
	@node -e 'const {runLukeWasm}=require("../scripts/luke_browser_loader.cjs");const fs=require("fs");const html=fs.readFileSync("build/reactive_timeline_ui.html","utf8");const m=html.match(/var LUKE_WHENS = (\[[\s\S]*?\]);/);const whens=Function("return ("+m[1]+");")();runLukeWasm(fs.readFileSync("build/reactive_timeline_ui.wasm"),{whens}).then(()=>{globalThis.__lukeDispatch("play","click");return new Promise(r=>setTimeout(r,350));}).then(()=>process.exit(0));' 2>&1 | tee /tmp/luke_rx_timeline.txt | grep -q 'Timeline finished'
	./build/luke BUILD ../examples/build/reactive_entity.luke -o build/ex_rx_entity
	./build/ex_rx_entity | grep -q "hp=100"
	./build/ex_rx_entity | grep -q "hp=75"
	./build/ex_rx_entity | grep -q "pct=0.75"
	@grep -q 'Player_health' build/ex_rx_entity.luke.c
	./build/luke BUILD ../examples/build/reactive_query.luke -o build/ex_rx_query
	./build/ex_rx_query | grep -q "alpha"
	./build/ex_rx_query | grep -q "gamma"
	@grep -q 'luke_rx_query_refresh' build/ex_rx_query.luke.c
	./build/luke BUILD ../examples/build/reactive_conformance_batch.luke -o build/ex_rx_conf_batch
	./build/ex_rx_conf_batch | grep -q "flush1=1"
	./build/ex_rx_conf_batch | grep -q "flush2=2"
	./build/ex_rx_conf_batch | grep -q "flush3=3"
	./build/luke BUILD ../examples/build/reactive_conformance_stale.luke -o build/ex_rx_conf_stale
	./build/ex_rx_conf_stale | grep -q "stale=2"
	./build/ex_rx_conf_stale | grep -q "total_stale=4"
	./build/ex_rx_conf_stale | grep -q "sum_stale=2"
	./build/luke BUILD ../examples/build/reactive_conformance_order.luke -o build/ex_rx_conf_order
	./build/ex_rx_conf_order | grep -q "chain=4"
	./build/ex_rx_conf_order | grep -q "chain=14"
	./build/ex_rx_conf_order | grep -q "derived_runs=3"
	./build/luke BUILD ../examples/build/reactive_conformance_priority.luke -o build/ex_rx_conf_prio
	./build/ex_rx_conf_prio | grep -q "log=10"
	./build/ex_rx_conf_prio | grep -q "ui_first=1"
	./build/luke BUILD ../examples/build/reactive_conformance_nested.luke -o build/ex_rx_conf_nested
	./build/ex_rx_conf_nested | grep -q "y=1"
	./build/ex_rx_conf_nested | grep -q "passes=1"
	./build/ex_rx_conf_nested | grep -q "turns=1"
	./build/ex_rx_conf_nested | grep -q "deferred=1"
	./build/luke BUILD ../examples/build/reactive_conformance_dedup.luke -o build/ex_rx_conf_dedup
	./build/ex_rx_conf_dedup | grep -q "dedup=2"
	./build/ex_rx_conf_dedup | grep -q "t=5"
	./build/luke BUILD ../examples/build/reactive_conformance_subtree.luke -o build/ex_rx_conf_subtree
	./build/ex_rx_conf_subtree | grep -q "subtree=1"
	./build/ex_rx_conf_subtree | grep -q "after=100"
	./build/luke BUILD ../examples/build/reactive_conformance_memory.luke -o build/ex_rx_conf_mem
	./build/ex_rx_conf_mem | grep -q "alive=2"
	./build/ex_rx_conf_mem | grep -q "disposed=2"
	./build/ex_rx_conf_mem | grep -q "dead=2"
	./build/ex_rx_conf_mem | grep -q "leaks=0"
	./build/ex_rx_conf_mem | grep -q "count=0"
	./build/luke BUILD ../examples/build/reactive_conformance_weak.luke -o build/ex_rx_conf_weak
	./build/ex_rx_conf_weak | grep -q "snap0=10"
	./build/ex_rx_conf_weak | grep -q "snap=10"
	./build/ex_rx_conf_weak | grep -q "live=15"
	./build/ex_rx_conf_weak | grep -q "weak_reads=1"
	./build/luke BUILD ../examples/build/reactive_conformance_scope_gc.luke -o build/ex_rx_conf_scope_gc
	./build/ex_rx_conf_scope_gc | grep -q "scopes=0"
	./build/ex_rx_conf_scope_gc | grep -q "gc=2"
	./build/ex_rx_conf_scope_gc | grep -q "alive=0"
	./build/ex_rx_conf_scope_gc | grep -q "dead=2"
	./build/luke BUILD ../examples/build/reactive_conformance_weak_effect.luke -o build/ex_rx_conf_weak_fx
	./build/ex_rx_conf_weak_fx | grep -q "tick"
	./build/ex_rx_conf_weak_fx | grep -q "leaks=0"
	./build/ex_rx_conf_weak_fx | grep -q "alive=1"
	./build/ex_rx_conf_weak_fx | grep -q "weak_reads=1"
	./build/luke BUILD ../examples/build/reactive_conformance_devtools.luke -o build/ex_rx_conf_devtools
	./build/ex_rx_conf_devtools | grep -q "write=1"
	./build/ex_rx_conf_devtools | grep -q "root=1"
	./build/ex_rx_conf_devtools | grep -q "depth=1"
	./build/ex_rx_conf_devtools | grep -q "deps=2"
	./build/ex_rx_conf_devtools | grep -q "subs=1"
	./build/ex_rx_conf_devtools | grep -q "cells=2"
	./build/ex_rx_conf_devtools | grep -q "derived=1"
	./build/ex_rx_conf_devtools | grep -q "edges=2"
	./build/ex_rx_conf_devtools | grep -q "step0=3"
	./build/ex_rx_conf_devtools | grep -q "wave0=2"
	./build/ex_rx_conf_devtools | grep -q "traces=1"
	./build/ex_rx_conf_devtools | grep -q "dumps=1"
	./build/luke BUILD ../examples/build/reactive_conformance_error.luke -o build/ex_rx_conf_error
	./build/ex_rx_conf_error 2>/dev/null | grep -q "errors=1"
	./build/ex_rx_conf_error 2>/dev/null | grep -q "iso=1"
	./build/ex_rx_conf_error 2>/dev/null | grep -q "ok=1"
	./build/ex_rx_conf_error 2>/dev/null | grep -q "async=1"
	./build/ex_rx_conf_error 2>/dev/null | grep -q "errors2=2"
	./build/ex_rx_conf_error 2>/dev/null | grep -q "retries=1"
	./build/luke BUILD ../examples/build/reactive_conformance_boundary.luke -o build/ex_rx_conf_boundary
	./build/ex_rx_conf_boundary 2>/dev/null | grep -q "global=1"
	./build/ex_rx_conf_boundary 2>/dev/null | grep -q "trips=1"
	./build/ex_rx_conf_boundary 2>/dev/null | grep -q "tripped=1"
	./build/ex_rx_conf_boundary 2>/dev/null | grep -q "reset=0"
	./build/luke BUILD ../examples/build/fullstack_cell_bind_proof.luke -o build/ex_fullstack_bind
	./build/ex_fullstack_bind | tee /tmp/luke_fullstack_bind.txt | grep -q "bind_region=1"
	@grep -q "ch1_region=1" /tmp/luke_fullstack_bind.txt
	@grep -q "ch2_region=1" /tmp/luke_fullstack_bind.txt
	@grep -q "ch1_granular=2" /tmp/luke_fullstack_bind.txt
	@grep -q "ch2_granular=3" /tmp/luke_fullstack_bind.txt
	./build/luke BUILD ../examples/build/fullstack_cell_server.luke -o build/fullstack_cell_server
	./build/luke BUILD ../examples/build/fullstack_cell_client.luke -target browser -o build/fullstack_cell_client
	@grep -q 'luke_rx_ui_set_text\|luke_rx_batch_begin' build/fullstack_cell_client.luke.c
	@rm -f /tmp/luke_fullstack_server.pid /tmp/luke_fullstack_cell.txt
	@./build/fullstack_cell_server > /tmp/luke_fullstack_server.log 2>&1 & echo $$! > /tmp/luke_fullstack_server.pid
	@sleep 0.15
	@node ../scripts/luke_browser_loader.cjs build/fullstack_cell_client.wasm --dispatch start:click --wait-ms 700 2>&1 | tee /tmp/luke_fullstack_cell.txt | grep -q "region=1"
	@grep -q "granular_delta=1" /tmp/luke_fullstack_cell.txt
	@grep -q "cell=" /tmp/luke_fullstack_cell.txt
	@if [ -f /tmp/luke_fullstack_server.pid ]; then kill $$(cat /tmp/luke_fullstack_server.pid) 2>/dev/null || true; fi
	./build/luke BUILD ../examples/build/reactive_benchmark.luke -o build/reactive_benchmark
	./build/reactive_benchmark | tee /tmp/luke_bench.txt | grep -q "benchmark_ok=1"
	@grep -q "nodes=1000" /tmp/luke_bench.txt
	@grep -q "nodes=10000" /tmp/luke_bench.txt
	@grep -q "granular_samples=100" /tmp/luke_bench.txt
	@grep -q "full_samples=20" /tmp/luke_bench.txt
	@test $$(grep -c 'region=100' /tmp/luke_bench.txt) -ge 2
	@grep -q "granular_median_ms=" /tmp/luke_bench.txt
	@grep -q "full_median_ms=" /tmp/luke_bench.txt
	@grep -q 'argus_index_insert\|argus_hash_id\|argus_own_id' build/reactive_benchmark.luke.c || \
	  grep -q 'luke_bench_median' build/reactive_benchmark.luke.c
	@grep -q 'luke_bench_median' build/reactive_benchmark.luke.c
	@grep -q 'luke_arena_init(arena, 1u<<20)' build/reactive_benchmark.luke.c
	@! grep -q 'luke_arena_init(arena, 1u<<24)' build/reactive_benchmark.luke.c
	./build/luke BUILD ../examples/build/dashboard_server.luke -o build/dashboard_server
	./build/luke BUILD ../examples/build/dashboard_client.luke -target browser -o build/dashboard_client
	@grep -q 'luke_rx_ui_set_text\|luke_rx_batch_begin' build/dashboard_client.luke.c
	@grep -q 'hanka_begin_column\|hanka_begin_row' build/dashboard_client.luke.c
	@rm -f /tmp/luke_dash_server.pid /tmp/luke_dash.txt
	@./build/dashboard_server > /tmp/luke_dash_server.log 2>&1 & echo $$! > /tmp/luke_dash_server.pid
	@sleep 0.15
	@node ../scripts/luke_browser_loader.cjs build/dashboard_client.wasm --dispatch go:click --wait-ms 600 2>&1 | tee /tmp/luke_dash.txt | grep -q "region=1"
	@grep -q "granular_delta=1" /tmp/luke_dash.txt
	@grep -q "\[argus_flex\]" /tmp/luke_dash.txt
	@if [ -f /tmp/luke_dash_server.pid ]; then kill $$(cat /tmp/luke_dash_server.pid) 2>/dev/null || true; fi
	./build/luke BUILD ../examples/build/subscribe_cell_server.luke -o build/subscribe_cell_server
	./build/luke BUILD ../examples/build/subscribe_cell_client.luke -target browser -o build/subscribe_cell_client
	@grep -q 'luke_js_subscribe_start\|luke_js_subscribe_body' build/subscribe_cell_client.luke.c
	@! grep -qE 'START FETCH|START TIMELINE' ../examples/build/subscribe_cell_client.luke
	@rm -f /tmp/luke_sub_server.pid /tmp/luke_sub.txt
	@./build/subscribe_cell_server > /tmp/luke_sub_server.log 2>&1 & echo $$! > /tmp/luke_sub_server.pid
	@sleep 0.15
	@node ../scripts/luke_browser_loader.cjs build/subscribe_cell_client.wasm --wait-ms 900 > /tmp/luke_sub.txt 2>&1
	@grep -q "region=1" /tmp/luke_sub.txt
	@grep -q "cell=" /tmp/luke_sub.txt
	@grep -q "events=" /tmp/luke_sub.txt
	@grep -q "\[subscribe_start\]" /tmp/luke_sub.txt
	@test $$(grep -c 'region=1' /tmp/luke_sub.txt) -ge 3
	@if [ -f /tmp/luke_sub_server.pid ]; then kill $$(cat /tmp/luke_sub_server.pid) 2>/dev/null || true; fi
	./build/luke BUILD ../examples/build/dashboard_push_server.luke -o build/dashboard_push_server
	./build/luke BUILD ../examples/build/dashboard_push_client.luke -target browser -o build/dashboard_push_client
	@grep -q 'luke_js_subscribe_start\|luke_js_subscribe_body' build/dashboard_push_client.luke.c
	@grep -q 'hanka_begin_column\|hanka_begin_row' build/dashboard_push_client.luke.c
	@! grep -qE 'START FETCH|START TIMELINE' ../examples/build/dashboard_push_client.luke
	@rm -f /tmp/luke_dash_push_server.pid /tmp/luke_dash_push.txt
	@./build/dashboard_push_server > /tmp/luke_dash_push_server.log 2>&1 & echo $$! > /tmp/luke_dash_push_server.pid
	@sleep 0.15
	@node ../scripts/luke_browser_loader.cjs build/dashboard_push_client.wasm --wait-ms 1000 > /tmp/luke_dash_push.txt 2>&1
	@grep -q "region=1" /tmp/luke_dash_push.txt
	@grep -q "reqs=" /tmp/luke_dash_push.txt
	@grep -q "\[argus_flex\]" /tmp/luke_dash_push.txt
	@grep -q "\[argus_flow_frame\]" /tmp/luke_dash_push.txt
	@test $$(grep -c 'region=1' /tmp/luke_dash_push.txt) -ge 3
	@if [ -f /tmp/luke_dash_push_server.pid ]; then kill $$(cat /tmp/luke_dash_push_server.pid) 2>/dev/null || true; fi
	./build/luke BUILD ../examples/build/integer_exact.luke -o build/integer_exact
	@grep -q 'int64_t\|luke_rx_cell_int\|luke_speak_integer' build/integer_exact.luke.c
	@./build/integer_exact | tee /tmp/luke_integer.txt | grep -q "exact=1"
	@grep -q "cents=200" /tmp/luke_integer.txt
	@grep -q "id=9007199254740994" /tmp/luke_integer.txt
	@grep -q "rx=15" /tmp/luke_integer.txt
	./build/luke BUILD ../examples/build/expr_parens_ok.luke -o build/expr_parens_ok
	@./build/expr_parens_ok | tee /tmp/luke_expr_parens.txt | grep -q "expr_parens_ok=1"
	@grep -q 'elapsed=4' /tmp/luke_expr_parens.txt
	@grep -q 'nested=14' /tmp/luke_expr_parens.txt
	@test -f include/luke_ast.hpp
	@grep -q 'stripOuterParens\|AstKind' src/build_c.cpp include/luke_ast.hpp
	./build/luke BUILD ../examples/build/integer_rules.luke -o build/integer_rules
	@grep -q 'luke_i64_mul\|luke_i64_add' build/integer_rules.luke.c
	@./build/integer_rules | tee /tmp/luke_integer_rules.txt | grep -q "mix=1"
	@grep -q "div=1" /tmp/luke_integer_rules.txt
	@grep -q "cents=398" /tmp/luke_integer_rules.txt
	./build/luke BUILD ../examples/build/integer_overflow.luke -o build/integer_overflow
	@./build/integer_overflow >/tmp/luke_i64_ov.txt 2>/tmp/luke_i64_ov.err; test $$? -ne 0
	@grep -q "INTEGER error" /tmp/luke_i64_ov.err
	@! grep -q "should_not_reach" /tmp/luke_i64_ov.txt
	./build/luke BUILD ../examples/build/concurrent_server.luke -o build/concurrent_server
	@grep -q 'luke_http_serve\|luke_http_wrap_' build/concurrent_server.luke.c
	@grep -q 'LUKE_HTTP_POOL_WORKERS\|luke_http__pool_worker' runtime/luke_net.h
	@grep -q 'epoll_wait\|kevent\|luke_http__ev_wait' runtime/luke_net.h
	@grep -q 'SO_REUSEPORT\|TCP_NODELAY\|writev\|accept4\|luke_http__arena_acquire' runtime/luke_net.h
	@grep -q 'luke_http_read_request\|luke_http_chunk_open\|luke_http__stop_flag' runtime/luke_net.h
	@grep -q 'LUKE_HTTP_KEEPALIVE_MAX 100000\|luke_http__ka_budget' runtime/luke_net.h
	@set -e; \
	  rm -f /tmp/luke_conc_server.pid /tmp/luke_conc_fast.txt /tmp/luke_conc_slow.txt; \
	  ./build/concurrent_server > /tmp/luke_conc_server.log 2>&1 & echo $$! > /tmp/luke_conc_server.pid; \
	  sleep 0.35; \
	  curl -s -m 3 http://127.0.0.1:8797/slow -o /tmp/luke_conc_slow.txt & echo $$! > /tmp/luke_conc_slow.pid; \
	  sleep 0.08; \
	  curl -s -m 2 http://127.0.0.1:8797/fast -o /tmp/luke_conc_fast.txt; \
	  wait $$(cat /tmp/luke_conc_slow.pid); \
	  test -f /tmp/luke_conc_fast.txt && grep -q '^fast$$' /tmp/luke_conc_fast.txt; \
	  test -f /tmp/luke_conc_slow.txt && grep -q '^slow$$' /tmp/luke_conc_slow.txt; \
	  kill $$(cat /tmp/luke_conc_server.pid) 2>/dev/null || true; \
	  wait $$(cat /tmp/luke_conc_server.pid) 2>/dev/null || true
	./build/luke BUILD ../examples/build/http_c10k_ok.luke -o build/http_c10k_ok
	@grep -q 'luke_http_chunk_open\|luke_http_client_ip' build/http_c10k_ok.luke.c
	@set -e; \
	  fuser -k 8804/tcp 2>/dev/null || true; \
	  rm -f /tmp/luke_c10k.pid /tmp/luke_c10k.log /tmp/luke_c10k_out.txt; \
	  ./build/http_c10k_ok > /tmp/luke_c10k.log 2>&1 & echo $$! > /tmp/luke_c10k.pid; \
	  sleep 0.35; \
	  python3 ../scripts/luke_c10k_probe.py; \
	  grep -q ka_chunk_ok /tmp/luke_c10k_out.txt; \
	  wait $$(cat /tmp/luke_c10k.pid); \
	  grep -q "c10k_done=1" /tmp/luke_c10k.log; \
	  grep -q "served=ka" /tmp/luke_c10k.log; \
	  grep -q "served=chunk" /tmp/luke_c10k.log; \
	  fuser -k 8804/tcp 2>/dev/null || true
	@set -e; \
	  fuser -k 8805/tcp 2>/dev/null || true; \
	  rm -f /tmp/luke_c10k_grace.pid /tmp/luke_c10k_grace.log; \
	  printf '%s\n' \
	    '// graceful shutdown probe' \
	    'IMPORT std/server' \
	    'THIS IS FUNCTION handle WITH req AS REQUEST DO' \
	    '  ASK httpReply WITH req, 200, "text/plain", "ok"' \
	    'END FUNCTION' \
	    'MY NAME IS server SET TO ASK httpListen WITH 8805' \
	    'ASK httpServe WITH server, handle, 0' \
	    'SPEAK "graceful=1"' > /tmp/luke_c10k_grace.luke; \
	  ./build/luke BUILD /tmp/luke_c10k_grace.luke -o build/http_c10k_grace; \
	  ./build/http_c10k_grace > /tmp/luke_c10k_grace.log 2>&1 & echo $$! > /tmp/luke_c10k_grace.pid; \
	  sleep 0.35; \
	  kill -TERM $$(cat /tmp/luke_c10k_grace.pid); \
	  wait $$(cat /tmp/luke_c10k_grace.pid); \
	  grep -q "graceful=1" /tmp/luke_c10k_grace.log; \
	  fuser -k 8805/tcp 2>/dev/null || true
	./build/luke BUILD ../examples/build/http_event_idle.luke -o build/http_event_idle
	@grep -q 'luke_http_serve_ev\|epoll_wait' runtime/luke_net.h
	@set -e; \
	  fuser -k 8806/tcp 2>/dev/null || true; \
	  rm -f /tmp/luke_event_idle.pid /tmp/luke_event_idle.log /tmp/luke_event_idle_out.txt; \
	  ./build/http_event_idle > /tmp/luke_event_idle.log 2>&1 & echo $$! > /tmp/luke_event_idle.pid; \
	  sleep 0.35; \
	  python3 ../scripts/luke_event_idle_probe.py; \
	  grep -q event_idle_ok /tmp/luke_event_idle_out.txt; \
	  kill -TERM $$(cat /tmp/luke_event_idle.pid); \
	  wait $$(cat /tmp/luke_event_idle.pid); \
	  grep -q "event_idle_done=1" /tmp/luke_event_idle.log; \
	  fuser -k 8806/tcp 2>/dev/null || true
	./build/luke BUILD ../examples/build/http_db_bench.luke -o build/http_db_bench
	@grep -q 'luke_db__stmt_acquire\|luke_db__tls\|SQLITE_OPEN_NOMUTEX\|journal_mode=WAL' runtime/luke_db.h
	@set -e; \
	  fuser -k 8810/tcp 2>/dev/null || true; \
	  rm -f /tmp/luke_http_db_bench.db /tmp/luke_http_db_bench.db-wal /tmp/luke_http_db_bench.db-shm \
	    /tmp/luke_db_bench.pid /tmp/luke_db_bench.log /tmp/luke_db_bench_out.txt; \
	  ./build/http_db_bench > /tmp/luke_db_bench.log 2>&1 & echo $$! > /tmp/luke_db_bench.pid; \
	  sleep 0.4; \
	  python3 ../scripts/luke_db_bench_probe.py; \
	  grep -q db_bench_ok /tmp/luke_db_bench_out.txt; \
	  kill -TERM $$(cat /tmp/luke_db_bench.pid); \
	  wait $$(cat /tmp/luke_db_bench.pid); \
	  grep -q "http_db_bench_done=1" /tmp/luke_db_bench.log; \
	  fuser -k 8810/tcp 2>/dev/null || true
	@set -e; \
	  if command -v pg_isready >/dev/null 2>&1 && pg_isready -h 127.0.0.1 -p 5432 -q; then \
	    ./build/luke BUILD ../examples/build/pg_api.luke -o build/pg_api; \
	    grep -q 'luke_pg_open\|PQenterPipelineMode\|luke_pg__async\|LukePgShard\|LUKE_PG_DRAFT_MIN' runtime/luke_pg.h; \
	    grep -q 'push_back("pq")\|"pq"' src/build_c.cpp; \
	    grep -q 'luke_pg_checkout\|luke_pg_checkin' src/build_c.cpp; \
	    fuser -k 8811/tcp 2>/dev/null || true; \
	    rm -f /tmp/luke_pg_api.pid /tmp/luke_pg_api.log /tmp/luke_pg_bench_out.txt; \
	    ./build/pg_api > /tmp/luke_pg_api.log 2>&1 & echo $$! > /tmp/luke_pg_api.pid; \
	    sleep 0.7; \
	    python3 ../scripts/luke_pg_bench_probe.py 50; \
	    grep -q pg_bench_ok /tmp/luke_pg_bench_out.txt; \
	    python3 ../scripts/luke_pg_bench_probe.py 200; \
	    grep -q pg_bench_ok /tmp/luke_pg_bench_out.txt; \
	    python3 ../scripts/luke_pg_bench_probe.py 300; \
	    grep -q pg_bench_ok /tmp/luke_pg_bench_out.txt; \
	    kill -TERM $$(cat /tmp/luke_pg_api.pid); \
	    wait $$(cat /tmp/luke_pg_api.pid); \
	    grep -q "pg_api_done=1" /tmp/luke_pg_api.log; \
	    fuser -k 8811/tcp 2>/dev/null || true; \
	    rm -f /tmp/luke_pg_api.pid /tmp/luke_pg_api.log /tmp/luke_pg_bench_out.txt; \
	    LUKE_PG_ASYNC=0 ./build/pg_api > /tmp/luke_pg_api.log 2>&1 & echo $$! > /tmp/luke_pg_api.pid; \
	    sleep 0.6; \
	    python3 ../scripts/luke_pg_bench_probe.py 240; \
	    grep -q pg_bench_ok /tmp/luke_pg_bench_out.txt; \
	    kill -TERM $$(cat /tmp/luke_pg_api.pid); \
	    wait $$(cat /tmp/luke_pg_api.pid); \
	    grep -q "pg_api_done=1" /tmp/luke_pg_api.log; \
	    fuser -k 8811/tcp 2>/dev/null || true; \
	  else \
	    echo "skip pg_api — Postgres not ready (install libpq-dev + postgresql)"; \
	  fi
	./build/luke BUILD ../examples/build/live_graph_server.luke -o build/live_graph_server
	./build/luke BUILD ../examples/build/live_graph_client.luke -target browser -o build/live_graph_client
	./build/luke BUILD ../examples/build/live_graph_updater.luke -o build/live_graph_updater
	./build/luke BUILD ../examples/build/live_graph_updater_rapid.luke -o build/live_graph_updater_rapid
	@grep -q 'luke_db_data_version' build/live_graph_server.luke.c
	@grep -q 'luke_db_query_text\|httpSseData' build/live_graph_server.luke.c
	@grep -q 'luke_ivm_' build/live_graph_server.luke.c
	@grep -q 'group_concat(name\|NEW.name' build/live_graph_server.luke.c
	@grep -q 'luke_ivm_log_' build/live_graph_server.luke.c
	@grep -q 'luke_db_log_append\|luke_http_last_event_id' build/live_graph_server.luke.c
	@grep -qE 'WATCH user FROM db' ../examples/build/live_graph_server.luke
	@grep -qE 'PUSH WATCH' ../examples/build/live_graph_server.luke
	@! grep -qE 'dbQuery|httpSseData|httpSseOpen|REFRESH QUERY' ../examples/build/live_graph_server.luke
	@grep -q 'luke_js_subscribe_start\|luke_js_subscribe_body' build/live_graph_client.luke.c
	@! grep -qE 'START FETCH|START TIMELINE|START SUBSCRIBE|WHILE |dbQuery|REFRESH QUERY' ../examples/build/live_graph_client.luke
	@grep -qE 'WATCH user' ../examples/build/live_graph_client.luke
	@grep -q 'poll(\|O_NONBLOCK' runtime/luke_net.h
	@bash ../scripts/wall_smoke.sh
	./build/luke BUILD ../examples/build/live_graph_join.luke -o build/live_graph_join
	@./build/live_graph_join | tee /tmp/luke_live_graph_join.txt | grep -q "join_ivm_ok=1"
	@grep -q "v0=Ada · Engineer" /tmp/luke_live_graph_join.txt
	@grep -q "v1=Ada Lovelace · Founder" /tmp/luke_live_graph_join.txt
	@grep -q 'luke_ivm_card_jd_t1_\|luke_ivm_card_jd_t2_' build/live_graph_join.luke.c
	@grep -q 'WHEN NEW.id = 1\|WHEN NEW.user_id = 1' build/live_graph_join.luke.c
	@grep -q "SELECT NEW.name\|NEW.title" build/live_graph_join.luke.c
	@! grep -q 'luke_ivm_card_t1_ai AFTER INSERT ON users BEGIN' build/live_graph_join.luke.c
	./build/luke BUILD ../examples/build/live_graph_join_filter.luke -o build/live_graph_join_filter
	@./build/live_graph_join_filter | tee /tmp/luke_lgjf.txt | grep -q "join_filter_ok=1"
	@grep -q "vUn=Ada · Engineer" /tmp/luke_lgjf.txt
	@grep -q "v2=$$" /tmp/luke_lgjf.txt
	@grep -q "v3=Ada · Founder" /tmp/luke_lgjf.txt
	@# non-key filter join: multi-row join bag (not scalar recompute / EXISTS theater)
	@grep -q "luke_ivm_jbag_card\|luke_ivm_jbag_" build/live_graph_join_filter.luke.c
	@grep -q "_jb_t1_\|_jb_t2_" build/live_graph_join_filter.luke.c
	@# Forbid plain recompute triggers (card_t1_ai) — jbag uses card_jb_t1_ai
	@! grep -qE 'luke_ivm_card_t[12]_a[iud] AFTER INSERT ON (users|profiles) BEGIN' build/live_graph_join_filter.luke.c
	./build/luke BUILD ../examples/build/live_graph_join_multi.luke -o build/live_graph_join_multi
	@./build/live_graph_join_multi | tee /tmp/luke_join_multi.txt | grep -q "join_multi_ok=1"
	@grep -q "v0=Ada · Engineer" /tmp/luke_join_multi.txt
	@grep -q "v3=Ada · Founder" /tmp/luke_join_multi.txt
	@grep -q "luke_ivm_jbag_card\|luke_ivm_jbag_" build/live_graph_join_multi.luke.c
	@grep -q "group_concat(v, char(10))" build/live_graph_join_multi.luke.c
	@! grep -qE 'luke_ivm_card_t[12]_a[iud] AFTER INSERT ON (users|profiles) BEGIN' build/live_graph_join_multi.luke.c
	./build/luke BUILD ../examples/build/live_graph_join3.luke -o build/live_graph_join3
	@./build/live_graph_join3 | tee /tmp/luke_join3.txt | grep -q "join3_ok=1"
	@grep -q "vUn=Ada · Engineer · Admin" /tmp/luke_join3.txt
	@grep -q "v1=Ada Lovelace · Founder · Owner" /tmp/luke_join3.txt
	@grep -q 'luke_ivm_card_jd_t1_\|luke_ivm_card_jd_t2_\|luke_ivm_card_jd_t3_' build/live_graph_join3.luke.c
	@grep -q 'WHEN NEW.id = 1\|WHEN NEW.user_id = 1' build/live_graph_join3.luke.c
	@! grep -q '_t1_ai AFTER INSERT ON users BEGIN' build/live_graph_join3.luke.c
	./build/luke BUILD ../examples/build/live_graph_agg.luke -o build/live_graph_agg
	@./build/live_graph_agg | tee /tmp/luke_agg.txt | grep -q "agg_ok=1"
	@grep -q "luke_ivm_bag_feed\|luke_ivm_bag_" build/live_graph_agg.luke.c
	@grep -q "_bag_ai\|_bag_ad\|group_concat(v" build/live_graph_agg.luke.c
	@! grep -q 'group_concat(body' build/live_graph_agg.luke.c
	./build/luke BUILD ../examples/build/live_graph_agg_range.luke -o build/live_graph_agg_range
	@./build/live_graph_agg_range | tee /tmp/luke_agg_range.txt | grep -q "agg_range_ok=1"
	@grep -q "rowcount=2" /tmp/luke_agg_range.txt
	@# inequality filter takes the bag path with NEW.age qualification (not ungated recompute)
	@grep -q 'luke_ivm_bag_adults' build/live_graph_agg_range.luke.c
	@grep -q 'WHEN NEW.age > 30' build/live_graph_agg_range.luke.c
	@! grep -q '_ai AFTER INSERT ON people BEGIN' build/live_graph_agg_range.luke.c
	./build/luke BUILD ../examples/build/live_graph_agg_fn.luke -o build/live_graph_agg_fn
	@./build/live_graph_agg_fn | tee /tmp/luke_agg_fn.txt | grep -q "agg_fn_ok=1"
	@grep -qF "maintained=[Al] truth=[Al]" /tmp/luke_agg_fn.txt
	@# function call in a filter must stay unqualified (substr, never NEW.substr)
	@grep -q 'WHEN substr(NEW.name' build/live_graph_agg_fn.luke.c
	@! grep -q 'NEW.substr' build/live_graph_agg_fn.luke.c
	@grep -q '_luke_watch_alive\|httpSseOpen' build/live_graph_server.luke.c
	@grep -q 'LUKE_SSE_ORIGIN\|Access-Control-Allow-Origin' runtime/luke_net.h
	./build/luke BUILD ../examples/build/expr_pratt.luke -o build/expr_pratt
	@./build/expr_pratt | tee /tmp/luke_expr_pratt.txt | grep -q "expr_pratt_ok=1"
	@grep -q 'luke_expr\|tokenizeExpr\|compileExpr' src/luke_expr.cpp
	@./build/luke IR ../examples/build/hello.luke 2>/dev/null | tee /tmp/luke_ast_ir.txt | grep -q "luke-ast-program"
	@grep -q 'Speak expr=String' /tmp/luke_ast_ir.txt
	@grep -q 'Let name=' /tmp/luke_ast_ir.txt
	@./build/luke IR ../examples/build/functions.luke 2>/dev/null | tee /tmp/luke_ast_fn.txt | grep -q "Function name=add"
	@grep -q 'parseLuke\|flattenProgram' src/luke_parse.cpp
	@./build/luke FMT ../examples/build/expr_pratt.luke 2>/dev/null | grep -q "expr_pratt_ok"
	@./build/luke FMT -e "a ADD ( b SUBTRACT c )" | grep -q "ADD"
	@# FMT must be semantics-preserving: BUILD(FMT(x)) ≡ BUILD(x)
	@./build/luke FMT ../examples/build/fmt_roundtrip.luke > build/fmt_roundtrip.luke
	@grep -q 'ASK add WITH 2, 3' build/fmt_roundtrip.luke
	@grep -q 'ASK mul WITH 4, 5' build/fmt_roundtrip.luke
	@! grep -q 'ASK ADD WITH' build/fmt_roundtrip.luke
	@! grep -q 'ASK MUL WITH' build/fmt_roundtrip.luke
	@! grep -qE '[0-9] ,|[0-9],  ,' build/fmt_roundtrip.luke
	./build/luke BUILD build/fmt_roundtrip.luke -o build/fmt_roundtrip
	@./build/fmt_roundtrip | tee /tmp/luke_fmt_rt.txt | grep -q "fmt_roundtrip_ok=1"
	@./build/luke FMT ../examples/build/functions.luke > build/fmt_functions.luke
	./build/luke BUILD build/fmt_functions.luke -o build/fmt_functions
	@./build/fmt_functions | tee /tmp/luke_fmt_fn.txt | grep -q "Build functions"
	@grep -q 'ASK add WITH' build/fmt_functions.luke
	@! grep -q 'ASK ADD WITH' build/fmt_functions.luke
	@# Every examples/build/*.luke: FMT → BUILD → identical stdout (see scripts/fmt_roundtrip_all.sh)
	@bash ../scripts/fmt_roundtrip_all.sh | tee /tmp/luke_fmt_roundtrip_all.txt | grep -q "fmt_roundtrip_ok=1"
	./build/luke BUILD ../examples/build/ast_roundtrip.luke -o build/ast_roundtrip
	@./build/ast_roundtrip | tee /tmp/luke_ast_rt.txt | grep -q "ast_roundtrip_ok=1"
	./build/luke BUILD ../examples/build/live_graph_scrub.luke -target browser -o build/live_graph_scrub
	@grep -q '◀ Back\|Forward ▶\|Live' build/live_graph_scrub.html
	@node ../scripts/luke_browser_loader.cjs build/live_graph_scrub.wasm 2>&1 | tee /tmp/luke_live_graph_scrub.txt | grep -q "scrub_ok=1"
	@grep -q 'luke_rx_list_add\|luke_list_get' build/live_graph_scrub.luke.c
	@python3 -c 'import json,subprocess; req=lambda o:(lambda b:f"Content-Length: {len(b)}\r\n\r\n".encode()+b)(json.dumps(o).encode()); msgs=b"".join([req({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}),req({"jsonrpc":"2.0","method":"initialized","params":{}}),req({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/bad.luke","languageId":"luke","version":1,"text":"MY NAME IS n AS NUMBER SET TO 1\nSPEAK n\n"}}}),req({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/bad.luke"},"position":{"line":0,"character":12}}}),req({"jsonrpc":"2.0","id":4,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/bad.luke"},"position":{"line":1,"character":0}}}),req({"jsonrpc":"2.0","id":5,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/bad.luke"},"position":{"line":1,"character":6}}}),req({"jsonrpc":"2.0","id":2,"method":"shutdown","params":None}),req({"jsonrpc":"2.0","method":"exit"})]); out=subprocess.check_output(["./build/luke","LSP"],input=msgs,timeout=5); assert b"hoverProvider\":true" in out and b"completionProvider" in out and b"definitionProvider\":true" in out and b"documentSymbolProvider\":true" in out and b"documentFormattingProvider\":true" in out and b"publishDiagnostics" in out and b"lukelang" in out'
	@bash ../scripts/lsp_providers.sh | tee /tmp/luke_lsp_providers_build.txt | grep -q "lsp_providers_ok=1"
	./build/luke BUILD ../examples/build/json_integer.luke -o build/json_integer
	@./build/json_integer | tee /tmp/luke_json_integer.txt | grep -q "json_integer_ok=1"
	@grep -q "id=9007199254740993" /tmp/luke_json_integer.txt
	@grep -q '9007199254740993' /tmp/luke_json_integer.txt
	./build/luke BUILD ../examples/build/sql_bind.luke -o build/sql_bind
	@./build/sql_bind | tee /tmp/luke_sql_bind.txt | grep -q "sql_bind_ok=1"
	@grep -qF "name2=Bobby'); DROP TABLE users;--" /tmp/luke_sql_bind.txt
	@grep -q 'luke_db_exec_bind\|luke_db_query_bind_text' build/sql_bind.luke.c
	./build/luke BUILD ../examples/build/auth_unit.luke -o build/auth_unit
	@./build/auth_unit | tee /tmp/luke_auth_unit.txt | grep -q "auth_unit_ok=1"
	@grep -q argon2id /tmp/luke_auth_unit.txt
	@grep -q 'crypto_pwhash\|luke_auth_' runtime/luke_auth.h
	./build/luke BUILD ../examples/build/auth_scoped.luke -o build/auth_scoped
	@./build/auth_scoped | tee /tmp/luke_auth_scoped.txt | grep -q "auth_scoped_ok=1"
	@grep -q 'luke_auth_current_user\|FOR CURRENT USER\|luke_db_query_bind_text' build/auth_scoped.luke.c
	./build/luke BUILD ../examples/build/auth_secret_ok.luke -o build/auth_secret_ok
	@./build/auth_secret_ok | tee /tmp/luke_auth_secret_ok.txt | grep -q "auth_secret_ok=1"
	@grep -q '078-05-1120' /tmp/luke_auth_secret_ok.txt
	@grep -q 'luke_auth_mark_saw\|luke_auth_revoke\|luke_auth_who_saw' build/auth_secret_ok.luke.c
	@grep -q 'after_revoke_user=' /tmp/luke_auth_secret_ok.txt
	@grep 'after_revoke_ssn=' /tmp/luke_auth_secret_ok.txt | grep -qv '078-05-1120'
	@set -e; \
	  if ./build/luke BUILD ../examples/build/auth_secret_bad_watch.luke -o build/auth_secret_bad_watch > /tmp/luke_auth_secret_bad_watch.txt 2>&1; then \
	    echo "expected auth_secret_bad_watch compile failure"; exit 1; \
	  fi; \
	  grep -q 'FOR CURRENT USER\|unauthorized\|compile error\|SECRET' /tmp/luke_auth_secret_bad_watch.txt
	@set -e; \
	  if ./build/luke BUILD ../examples/build/auth_secret_bad_bind.luke -o build/auth_secret_bad_bind > /tmp/luke_auth_secret_bad_bind.txt 2>&1; then \
	    echo "expected auth_secret_bad_bind compile failure"; exit 1; \
	  fi; \
	  grep -q 'FOR CURRENT USER\|unauthorized\|compile error\|SECRET' /tmp/luke_auth_secret_bad_bind.txt
	./build/luke BUILD ../examples/build/auth_flow_ok.luke -o build/auth_flow_ok
	@./build/auth_flow_ok | tee /tmp/luke_auth_flow_ok.txt | grep -q "auth_flow_ok=1"
	@grep -q 'flow_uid=' /tmp/luke_auth_flow_ok.txt
	@grep -q 'step2=done' /tmp/luke_auth_flow_ok.txt
	@set -e; \
	  if ./build/luke BUILD ../examples/build/auth_flow_bad.luke -o build/auth_flow_bad > /tmp/luke_auth_flow_bad.txt 2>&1; then \
	    echo "expected auth_flow_bad compile failure"; exit 1; \
	  fi; \
	  grep -q 'DONE without VERIFY\|impossible auth state\|compile error' /tmp/luke_auth_flow_bad.txt
	./build/luke BUILD ../examples/build/auth_lang_ok.luke -o build/auth_lang_ok
	@./build/auth_lang_ok | tee /tmp/luke_auth_lang_ok.txt | grep -q "auth_lang_ok=1"
	@grep -q 'masked=1120' /tmp/luke_auth_lang_ok.txt
	@grep -q 'remaining=5' /tmp/luke_auth_lang_ok.txt
	@grep -q 'scrub=' /tmp/luke_auth_lang_ok.txt
	@grep -q 'luke_auth_configure_limit\|luke_auth_reveal_last\|luke_auth_scrub_to_access' build/auth_lang_ok.luke.c
	./build/luke BUILD ../examples/build/auth_api.luke -o build/auth_api
	@grep -q 'luke_auth_login\|luke_auth_check_csrf\|luke_auth_require' build/auth_api.luke.c
	@set -e; \
	  fuser -k 8802/tcp 2>/dev/null || true; \
	  rm -f /tmp/luke_auth_api.db /tmp/luke_auth_api.db-wal /tmp/luke_auth_api.db-shm \
	    /tmp/luke_auth_api.pid /tmp/luke_auth_api.log /tmp/luke_auth_cookies.txt; \
	  ./build/auth_api > /tmp/luke_auth_api.log 2>&1 & echo $$! > /tmp/luke_auth_api.pid; \
	  sleep 0.40; \
	  REG=$$(curl -s -m 3 -H 'Content-Type: application/json' -d '{"email":"ada@example.com","password":"correct horse battery"}' http://127.0.0.1:8802/register); \
	  echo "$$REG" | grep -q '^uid='; \
	  LOGIN=$$(curl -s -m 3 -c /tmp/luke_auth_cookies.txt -H 'Content-Type: application/json' -d '{"email":"ada@example.com","password":"correct horse battery"}' http://127.0.0.1:8802/login); \
	  echo "$$LOGIN" | grep -q 'csrf='; \
	  CSRF=$$(echo "$$LOGIN" | sed -n 's/.*csrf=\([0-9a-f]*\).*/\1/p'); \
	  test -n "$$CSRF"; \
	  ME=$$(curl -s -m 3 -b /tmp/luke_auth_cookies.txt http://127.0.0.1:8802/me); \
	  echo "$$ME" | grep -q '^me='; \
	  NOTE=$$(curl -s -m 3 -b /tmp/luke_auth_cookies.txt -H "Content-Type: application/json" -H "X-CSRF-Token: $$CSRF" -d '{"body":"secret-a"}' http://127.0.0.1:8802/notes); \
	  test "$$NOTE" = "note_ok"; \
	  NOTES=$$(curl -s -m 3 -b /tmp/luke_auth_cookies.txt http://127.0.0.1:8802/notes); \
	  echo "$$NOTES" | grep -q 'secret-a'; \
	  BYE=$$(curl -s -m 3 -b /tmp/luke_auth_cookies.txt -X POST http://127.0.0.1:8802/logout); \
	  test "$$BYE" = "bye"; \
	  wait $$(cat /tmp/luke_auth_api.pid); \
	  grep -q "auth_api_done=1" /tmp/luke_auth_api.log; \
	  fuser -k 8802/tcp 2>/dev/null || true
	./build/luke BUILD ../examples/build/backend_api.luke -o build/backend_api
	@grep -q 'luke_http_match\|luke_http_query_map\|luke_http_set_cookie' build/backend_api.luke.c
	./build/luke BUILD ../examples/build/backend_lang_ok.luke -o build/backend_lang_ok
	@./build/backend_lang_ok | tee /tmp/luke_backend_lang_ok.txt | grep -q "backend_lang_ok=1"
	@grep -q 'form_ok=login' /tmp/luke_backend_lang_ok.txt
	@grep -q 'form_cell_ok=1' /tmp/luke_backend_lang_ok.txt
	@grep -q 'CREATE TABLE IF NOT EXISTS notes' build/backend_lang_ok.luke.c
	./build/luke BUILD ../examples/build/backend_form_errors.luke -o build/backend_form_errors
	@./build/backend_form_errors | tee /tmp/luke_form_errors.txt | grep -q "form_errors_ok=1"
	@grep -q 'email_err=invalid_email' /tmp/luke_form_errors.txt
	@grep -q 'pass_err=too_short' /tmp/luke_form_errors.txt
	@grep -q 'ok_cell=0' /tmp/luke_form_errors.txt
	@grep -q 'ok_cell2=1' /tmp/luke_form_errors.txt
	./build/luke BUILD ../examples/build/backend_migrate_ok.luke -o build/backend_migrate_ok
	@./build/backend_migrate_ok | tee /tmp/luke_migrate_ok.txt | grep -q "migrate_ok=1"
	@grep -q 'migrated=2' /tmp/luke_migrate_ok.txt
	@grep -q 'rewound=1' /tmp/luke_migrate_ok.txt
	@grep -q 'version=2' /tmp/luke_migrate_ok.txt
	@grep -q 'luke_db_migrate_ensure\|luke_db_migrate_set' build/backend_migrate_ok.luke.c
	./build/luke BUILD ../examples/build/backend_routes_serve.luke -o build/backend_routes_serve
	@grep -q 'luke_http_wrap___luke_routes\|__luke_routes' build/backend_routes_serve.luke.c
	@set -e; \
	  fuser -k 8803/tcp 2>/dev/null || true; \
	  rm -f /tmp/luke_routes_serve.pid /tmp/luke_routes_serve.log; \
	  ./build/backend_routes_serve > /tmp/luke_routes_serve.log 2>&1 & echo $$! > /tmp/luke_routes_serve.pid; \
	  sleep 0.35; \
	  test "$$(curl -s -m 2 http://127.0.0.1:8803/ok)" = "ok"; \
	  test "$$(curl -s -m 2 http://127.0.0.1:8803/user/42)" = "user=42"; \
	  test "$$(curl -s -m 2 http://127.0.0.1:8803/missing)" = "not found"; \
	  wait $$(cat /tmp/luke_routes_serve.pid); \
	  grep -q "routes_serve_done=1" /tmp/luke_routes_serve.log; \
	  kill $$(cat /tmp/luke_routes_serve.pid) 2>/dev/null || true; \
	  fuser -k 8803/tcp 2>/dev/null || true
	@set -e; \
	  if ./build/luke BUILD ../examples/build/backend_routes_bad_link.luke -o build/backend_routes_bad_link > /tmp/luke_br_bad_link.txt 2>&1; then \
	    echo "expected backend_routes_bad_link compile failure"; exit 1; \
	  fi; \
	  grep -q 'broken route\|LINK TO\|compile error' /tmp/luke_br_bad_link.txt
	@set -e; \
	  if ./build/luke BUILD ../examples/build/backend_routes_bad_secret.luke -o build/backend_routes_bad_secret > /tmp/luke_br_bad_secret.txt 2>&1; then \
	    echo "expected backend_routes_bad_secret compile failure"; exit 1; \
	  fi; \
	  grep -q 'TOUCHES SECRET\|REQUIRES AUTH\|compile error' /tmp/luke_br_bad_secret.txt
	@set -e; \
	  if ./build/luke BUILD ../examples/build/backend_mw_bad_order.luke -o build/backend_mw_bad_order > /tmp/luke_mw_bad.txt 2>&1; then \
	    echo "expected backend_mw_bad_order compile failure"; exit 1; \
	  fi; \
	  grep -q 'RATE LIMIT before AUTH\|compile error' /tmp/luke_mw_bad.txt
	./build/luke BUILD ../examples/build/backend_flow_oauth_ok.luke -o build/backend_flow_oauth_ok
	@./build/backend_flow_oauth_ok | tee /tmp/luke_oauth_ok.txt | grep -q "backend_oauth_ok=1"
	@set -e; \
	  if ./build/luke BUILD ../examples/build/backend_flow_oauth_bad.luke -o build/backend_flow_oauth_bad > /tmp/luke_oauth_bad.txt 2>&1; then \
	    echo "expected backend_flow_oauth_bad compile failure"; exit 1; \
	  fi; \
	  grep -q 'OAUTH\|VERIFY\|compile error' /tmp/luke_oauth_bad.txt
	@set -e; \
	  fuser -k 8799/tcp 2>/dev/null || true; \
	  rm -f /tmp/luke_backend_api.db /tmp/luke_backend_api.db-wal /tmp/luke_backend_api.db-shm \
	    /tmp/luke_be_api.pid /tmp/luke_be_api.log /tmp/luke_be_cookies.txt; \
	  ./build/backend_api > /tmp/luke_be_api.log 2>&1 & echo $$! > /tmp/luke_be_api.pid; \
	  sleep 0.35; \
	  test "$$(curl -s -m 2 http://127.0.0.1:8799/ok)" = "ok"; \
	  test "$$(curl -s -m 2 'http://127.0.0.1:8799/user/1?tag=hi%20there')" = "Ada|hi there"; \
	  test "$$(curl -s -m 2 -c /tmp/luke_be_cookies.txt -H 'Content-Type: application/json' -d '{"user":"Ada"}' http://127.0.0.1:8799/login)" = "ok"; \
	  test "$$(curl -s -m 2 -b /tmp/luke_be_cookies.txt http://127.0.0.1:8799/me)" = "me=1"; \
	  wait $$(cat /tmp/luke_be_api.pid); \
	  grep -q "backend_api_done=1" /tmp/luke_be_api.log; \
	  grep -q "luke_sid" /tmp/luke_be_cookies.txt; \
	  fuser -k 8799/tcp 2>/dev/null || true
	./build/luke BUILD ../examples/build/syntax_stress.luke -target browser -o build/syntax_stress
	@grep -q 'Luke Syntax Stress' build/syntax_stress.html
	@node ../scripts/luke_browser_loader.cjs build/syntax_stress.wasm 2>&1 | tee /tmp/luke_syntax_stress.txt | grep -q "syntax_stress_ok=1"
	@set -e; \
	  fuser -k 8798/tcp 2>/dev/null || true; \
	  rm -f /tmp/luke_live_graph.db /tmp/luke_live_graph.db-wal /tmp/luke_live_graph.db-shm \
	    /tmp/luke_lg_server.pid /tmp/luke_lg_client.pid /tmp/luke_lg.txt /tmp/luke_lg_server.log; \
	  stdbuf -oL -eL ./build/live_graph_server > /tmp/luke_lg_server.log 2>&1 & echo $$! > /tmp/luke_lg_server.pid; \
	  sleep 0.25; \
	  node ../scripts/luke_browser_loader.cjs build/live_graph_client.wasm --wait-ms 2800 > /tmp/luke_lg.txt 2>&1 & echo $$! > /tmp/luke_lg_client.pid; \
	  # Give the browser loader time to connect and receive the initial seed push.
	  sleep 2.00; \
	  ./build/live_graph_updater_rapid | tee /tmp/luke_lg_updater.txt | grep -q "updated=Ada3"; \
	  wait $$(cat /tmp/luke_lg_client.pid); \
	  grep -q "region=1" /tmp/luke_lg.txt; \
	  grep -q "user=Ada3" /tmp/luke_lg.txt; \
	  grep -q "display=seed" /tmp/luke_lg.txt; \
	  grep -q "\[subscribe_start\]" /tmp/luke_lg.txt; \
	  # We may or may not capture the initial seed depending on connection timing, \
	  # but we must observe the final Ada3 value and still repaint region-minimally. \
	  test $$(grep -c 'region=1' /tmp/luke_lg.txt) -ge 1; \
	  for _i in 1 2 3 4 5 6 7 8 9 10; do \
	    grep -q 'watch_queries=' /tmp/luke_lg_server.log && break; \
	    sleep 0.3; \
	  done; \
	  grep -q 'watch_queries=' /tmp/luke_lg_server.log; \
	  WQ=$$(grep -oE 'watch_queries=[0-9]+' /tmp/luke_lg_server.log | head -1 | cut -d= -f2); \
	  test -n "$$WQ"; \
	  test "$$WQ" -ge 1; \
	  test "$$WQ" -le 10; \
	  if [ -f /tmp/luke_lg_server.pid ]; then kill $$(cat /tmp/luke_lg_server.pid) 2>/dev/null || true; fi; \
	  if [ -f /tmp/luke_lg_server.pid ]; then wait $$(cat /tmp/luke_lg_server.pid) 2>/dev/null || true; fi; \
	  fuser -k 8798/tcp 2>/dev/null || true
	@rm -rf ../luke_modules/_test_pkg && ./build/luke PKG init _test_pkg && test -f ../luke_modules/_test_pkg/main.luke && rm -rf ../luke_modules/_test_pkg

# Pressure suite — not part of every CI lap; proves ceilings under load.
pressure: build/luke
	./build/luke BUILD ../examples/build/reactive_pressure.luke -o build/reactive_pressure
	./build/reactive_pressure | tee /tmp/luke_pressure_rx.txt | grep -q "pressure_ok=1"
	@grep -q "nodes=100000" /tmp/luke_pressure_rx.txt
	@grep -q "region=50" /tmp/luke_pressure_rx.txt
	@python3 -c 'import re,sys; t=open("/tmp/luke_pressure_rx.txt").read();\
g=float(re.search(r"granular_median_ms=([0-9.]+)",t).group(1));\
b=float(re.search(r"build_ms=([0-9.]+)",t).group(1));\
assert g < 1.0, g; assert b < 5000.0, b; print("reactive_pressure thresholds ok", g, b)'
	./build/luke BUILD ../examples/build/frontend_pressure.luke -o build/frontend_pressure
	./build/frontend_pressure | tee /tmp/luke_pressure_fe.txt | grep -q "pressure_ok=1"
	@grep -q "nodes=2500" /tmp/luke_pressure_fe.txt
	@python3 -c 'import re; t=open("/tmp/luke_pressure_fe.txt").read();\
ms=float(re.search(r"ms=([0-9.]+)",t).group(1));\
assert ms < 5000.0, ms; print("frontend_pressure thresholds ok", ms)'
	./build/luke BUILD ../examples/build/concurrent_pressure.luke -o build/concurrent_pressure
	@set -e; \
	  fuser -k 8799/tcp 2>/dev/null || true; \
	  rm -f /tmp/luke_pressure_conc.log /tmp/luke_pressure_conc_out.txt /tmp/luke_pressure_curl.pids; \
	  ./build/concurrent_pressure > /tmp/luke_pressure_conc.log 2>&1 & echo $$! > /tmp/luke_pressure_conc.pid; \
	  sleep 0.35; \
	  : > /tmp/luke_pressure_curl.pids; \
	  for i in $$(seq 1 32); do \
	    curl -s -m 2 http://127.0.0.1:8799/fast -o /tmp/luke_pressure_fast_$$i.txt & echo $$! >> /tmp/luke_pressure_curl.pids; \
	  done; \
	  for i in $$(seq 1 8); do \
	    curl -s -m 3 http://127.0.0.1:8799/slow -o /tmp/luke_pressure_slow_$$i.txt & echo $$! >> /tmp/luke_pressure_curl.pids; \
	  done; \
	  wait $$(cat /tmp/luke_pressure_curl.pids); \
	  for i in $$(seq 1 32); do grep -q '^fast$$' /tmp/luke_pressure_fast_$$i.txt; done; \
	  for i in $$(seq 1 8); do grep -q '^slow$$' /tmp/luke_pressure_slow_$$i.txt; done; \
	  echo "concurrent_pressure ok fast=32 slow=8" | tee /tmp/luke_pressure_conc_out.txt; \
	  if [ -f /tmp/luke_pressure_conc.pid ]; then kill $$(cat /tmp/luke_pressure_conc.pid) 2>/dev/null || true; fi; \
	  wait $$(cat /tmp/luke_pressure_conc.pid) 2>/dev/null || true; \
	  fuser -k 8799/tcp 2>/dev/null || true
	@echo "pressure suite passed"
	@cat /tmp/luke_pressure_rx.txt /tmp/luke_pressure_fe.txt /tmp/luke_pressure_conc_out.txt

# One deployed Live Graph proof (local smoke; TLS via Caddy in examples/deploy/wall)
test-wall: build/luke
	@bash ../scripts/wall_smoke.sh

clean:
	rm -rf build
