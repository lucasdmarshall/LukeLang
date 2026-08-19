# Exam — LukeLang watching itself

Live traffic / ops dashboard: the Exam server logs **every request** to SQLite;
IVM `WATCH` cells stay hot under write load; the Luke browser UI streams req/s,
status breakdown, top paths, p50/p99, and live path feeds over SSE.

Zero external runtime dependencies — **server, UI, and load generator are all LukeLang**.

## Quick start

From the repo root (needs a built `vm/build/luke`):

```bash
# UI (dark ops console — WEAR STYLE + class hatch)
./vm/build/luke PUBLISH WEB Exam/client.luke -o Exam/dist/client --tailwind Exam/exam.tailwind.css
cp Exam/exam.utilities.css Exam/dist/client.css   # skin if Tailwind CLI is unavailable

# Server
./vm/build/luke BUILD Exam/server.luke -o build/exam_server
./build/exam_server &

# Generate traffic (or point your Go bench client at :8820)
./vm/build/luke BUILD Exam/load.luke -o build/exam_load
./build/exam_load
```

Open **http://127.0.0.1:8820/** — metrics and feeds move as hits land.

## Routes

| Path | Role |
|------|------|
| `/` | Dashboard (Luke `PUBLISH WEB` HTML + WASM) |
| `/client.wasm` `/client.css` | Dashboard assets |
| `/watch/rps` `/watch/total` `/watch/latency` `/watch/status` `/watch/top` | Metric SSE |
| `/watch/ok` `/watch/err` | Live path feeds (IVM bag tables under write load) |
| `/ok` `/api/ping` `/slow` `/err` `/missing` | Traffic surfaces |
| `/health` | Liveness |

## Why this exists

- Data moves the instant anything hits the server — no cron, no fake feed.
- Every request is a write; bag + point `WATCH` IVM stays warm under sustained load.
- Dogfood: the language monitoring itself.
