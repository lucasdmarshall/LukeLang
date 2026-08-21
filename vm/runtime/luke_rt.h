#ifndef LUKE_RT_H
#define LUKE_RT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tiny no-GC runtime for Luke Build mode.
 * Memory: bump arena. No mark-sweep. No tracing. */

typedef struct LukeText {
  const char *ptr;
  size_t len;
} LukeText;

typedef struct LukeArenaBlock {
  struct LukeArenaBlock *next;
  size_t cap;
  size_t len;
  char *data;
} LukeArenaBlock;

typedef struct LukeArena {
  LukeArenaBlock *block; /* current bump block */
  LukeArenaBlock *first; /* oldest block (for free) */
  size_t len;            /* total bytes allocated across blocks (stats) */
  size_t cap;            /* sum of block caps (stats) */
} LukeArena;

/* Process argv — set once from main(). */
static int luke_rt_argc = 0;
static char **luke_rt_argv = NULL;

static inline void luke_runtime_set_args(int argc, char **argv) {
  luke_rt_argc = argc;
  luke_rt_argv = argv;
}

static inline int luke_arg_count(void) { return luke_rt_argc; }

static inline LukeText luke_text(const char *s) {
  LukeText t;
  t.ptr = s ? s : "";
  t.len = s ? strlen(s) : 0;
  return t;
}

static inline LukeText luke_text_n(const char *s, size_t n) {
  LukeText t;
  t.ptr = s ? s : "";
  t.len = n;
  return t;
}

static inline LukeArenaBlock *luke_arena_new_block(size_t cap) {
  if (cap < 4096) cap = 4096;
  LukeArenaBlock *b = (LukeArenaBlock *)malloc(sizeof(LukeArenaBlock));
  if (!b) {
    fprintf(stderr, "Luke arena: out of memory\n");
    abort();
  }
  b->next = NULL;
  b->cap = cap;
  b->len = 0;
  b->data = (char *)malloc(cap);
  if (!b->data) {
    fprintf(stderr, "Luke arena: out of memory\n");
    abort();
  }
  return b;
}

static inline void luke_arena_init(LukeArena *a, size_t cap) {
  LukeArenaBlock *b = luke_arena_new_block(cap ? cap : (1u << 20));
  a->block = b;
  a->first = b;
  a->len = 0;
  a->cap = b->cap;
}

static inline void luke_arena_free(LukeArena *a) {
  LukeArenaBlock *b = a->first;
  while (b) {
    LukeArenaBlock *n = b->next;
    free(b->data);
    free(b);
    b = n;
  }
  a->block = a->first = NULL;
  a->cap = a->len = 0;
}

/* Keep the first block; free extras; rewind bump to 0. For request arena pools. */
static inline void luke_arena_clear(LukeArena *a) {
  if (!a || !a->first) return;
  LukeArenaBlock *extra = a->first->next;
  while (extra) {
    LukeArenaBlock *n = extra->next;
    free(extra->data);
    free(extra);
    extra = n;
  }
  a->first->next = NULL;
  a->first->len = 0;
  a->block = a->first;
  a->len = 0;
  a->cap = a->first->cap;
}

/* Checkpoint / restore — IN ARENA scopes (bulk free back to mark).
 * Mark is total bytes allocated; reset only supports rewind within the
 * current block (scopes that didn't cross a block boundary). */
typedef size_t LukeArenaMark;

static inline LukeArenaMark luke_arena_mark(LukeArena *a) { return a->len; }

static inline void luke_arena_reset(LukeArena *a, LukeArenaMark m) {
  if (m > a->len) return;
  /* Best-effort: rewind current block if mark lies inside it. */
  size_t before = a->len - a->block->len;
  if (m >= before && m <= a->len) {
    a->block->len = m - before;
    a->len = m;
  }
}

static inline void *luke_arena_alloc(LukeArena *a, size_t n, size_t align) {
  size_t mask = align - 1;
  LukeArenaBlock *b = a->block;
  size_t off = (b->len + mask) & ~mask;
  if (off + n > b->cap) {
    /* Chain a new block — never realloc, so existing pointers stay valid. */
    size_t need = n + align + 64;
    size_t ncap = b->cap ? b->cap * 2 : (1u << 20);
    if (ncap < need) ncap = need;
    LukeArenaBlock *nb = luke_arena_new_block(ncap);
    b->next = nb;
    a->block = nb;
    a->cap += nb->cap;
    b = nb;
    off = (b->len + mask) & ~mask;
  }
  size_t old_len = b->len;
  void *p = b->data + off;
  b->len = off + n;
  a->len += b->len - old_len;
  return p;
}

static inline LukeText luke_text_concat(LukeArena *a, LukeText x, LukeText y) {
  size_t xl = x.len, yl = y.len;
  char *p = (char *)luke_arena_alloc(a, xl + yl + 1, 1);
  if (xl && x.ptr) memcpy(p, x.ptr, xl);
  if (yl && y.ptr) memcpy(p + xl, y.ptr, yl);
  p[xl + yl] = '\0';
  return luke_text_n(p, xl + yl);
}

static inline void luke_speak_text(LukeText t) {
  fwrite(t.ptr, 1, t.len, stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

static inline void luke_speak_number(double n) {
  /* Trim trailing zeros for friendlier output */
  char buf[64];
  snprintf(buf, sizeof(buf), "%.10g", n);
  puts(buf);
  fflush(stdout);
}

static inline void luke_speak_integer(int64_t n) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", (long long)n);
  puts(buf);
  fflush(stdout);
}

static inline LukeText luke_integer_to_text(LukeArena *a, int64_t n) {
  char buf[32];
  int k = snprintf(buf, sizeof(buf), "%lld", (long long)n);
  if (k < 0) k = 0;
  char *p = (char *)luke_arena_alloc(a, (size_t)k + 1, 1);
  memcpy(p, buf, (size_t)k + 1);
  return luke_text_n(p, (size_t)k);
}

/* INTEGER overflow / conversion — abort; money and IDs must not wrap silently. */
static inline void luke_integer_fail(const char *why) {
  fprintf(stderr, "INTEGER error: %s\n", why ? why : "invalid");
  fflush(stderr);
  exit(1);
}

static inline int64_t luke_i64_add(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_add_overflow(a, b, &r)) luke_integer_fail("ADD overflow");
  return r;
}

static inline int64_t luke_i64_sub(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_sub_overflow(a, b, &r)) luke_integer_fail("SUBTRACT overflow");
  return r;
}

static inline int64_t luke_i64_mul(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_mul_overflow(a, b, &r)) luke_integer_fail("MULTIPLY overflow");
  return r;
}

/* NUMBER → INTEGER: truncate toward zero. Non-finite or out of int64 range aborts. */
static inline int64_t luke_number_to_integer(double n) {
  if (n != n || n > (double)INT64_MAX || n < (double)INT64_MIN)
    luke_integer_fail("NUMBER→INTEGER out of range or not finite");
  return (int64_t)n;
}

static inline void luke_speak_flag(int f) {
  puts(f ? "true" : "false");
  fflush(stdout);
}

/* ---------- Micro-benchmark sample buffer (median / min) ---------- */
#define LUKE_BENCH_MAX 256
static double luke_bench_buf[LUKE_BENCH_MAX];
static size_t luke_bench_len = 0;

static inline void luke_bench_reset(void) { luke_bench_len = 0; }

static inline void luke_bench_push(double ms) {
  if (luke_bench_len < LUKE_BENCH_MAX) luke_bench_buf[luke_bench_len++] = ms;
}

static inline double luke_bench_sample_count(void) { return (double)luke_bench_len; }

static inline double luke_bench_min(void) {
  if (!luke_bench_len) return 0.0;
  double m = luke_bench_buf[0];
  for (size_t i = 1; i < luke_bench_len; ++i)
    if (luke_bench_buf[i] < m) m = luke_bench_buf[i];
  return m;
}

static inline double luke_bench_median(void) {
  if (!luke_bench_len) return 0.0;
  double tmp[LUKE_BENCH_MAX];
  memcpy(tmp, luke_bench_buf, luke_bench_len * sizeof(double));
  for (size_t i = 1; i < luke_bench_len; ++i) {
    double v = tmp[i];
    size_t j = i;
    while (j > 0 && tmp[j - 1] > v) {
      tmp[j] = tmp[j - 1];
      j--;
    }
    tmp[j] = v;
  }
  return tmp[luke_bench_len / 2];
}

/* ---------- collections (arena-backed; Luke LIST / MAP) ---------- */

typedef struct LukeList {
  LukeText *items;
  size_t len;
  size_t cap;
} LukeList;

typedef struct LukeMap {
  LukeText *keys;
  LukeText *vals;
  size_t len;
  size_t cap;
} LukeMap;

static inline LukeList *luke_list_new(LukeArena *a) {
  LukeList *l = (LukeList *)luke_arena_alloc(a, sizeof(LukeList), sizeof(void *));
  l->items = NULL;
  l->len = 0;
  l->cap = 0;
  return l;
}

static inline void luke_list_add(LukeArena *a, LukeList *l, LukeText v) {
  if (!l) return;
  if (l->len + 1 > l->cap) {
    size_t ncap = l->cap ? l->cap * 2 : 4;
    LukeText *ni = (LukeText *)luke_arena_alloc(a, ncap * sizeof(LukeText), sizeof(void *));
    if (l->items && l->len) memcpy(ni, l->items, l->len * sizeof(LukeText));
    l->items = ni;
    l->cap = ncap;
  }
  l->items[l->len++] = v;
}

static inline LukeText luke_list_get(LukeList *l, double index) {
  if (!l || index < 0) return luke_text("");
  size_t i = (size_t)index;
  if (i >= l->len) return luke_text("");
  return l->items[i];
}

static inline void luke_list_set(LukeList *l, double index, LukeText v) {
  if (!l || index < 0) return;
  size_t i = (size_t)index;
  if (i >= l->len) return;
  l->items[i] = v;
}

static inline double luke_list_len(LukeList *l) { return l ? (double)l->len : 0; }

/* Cell-graph face: the language iterates; user code does not write WHILE. */
static inline void luke_speak_each_number(double lo, double hi) {
  if (hi < lo) return;
  for (double i = lo; i <= hi + 1e-9; i += 1) luke_speak_number(i);
}

static inline void luke_speak_each_of(LukeList *l) {
  double n = luke_list_len(l);
  for (double i = 0; i < n; i += 1) luke_speak_text(luke_list_get(l, i));
}

static inline LukeList *luke_numbers_from_to(LukeArena *a, double lo, double hi) {
  LukeList *l = luke_list_new(a);
  if (hi < lo) return l;
  for (double i = lo; i <= hi + 1e-9; i += 1) {
    char buf[64];
    int k = snprintf(buf, sizeof(buf), "%.10g", i);
    if (k < 0) k = 0;
    char *p = (char *)luke_arena_alloc(a, (size_t)k + 1, 1);
    memcpy(p, buf, (size_t)k + 1);
    luke_list_add(a, l, luke_text_n(p, (size_t)k));
  }
  return l;
}

/* Closed core comparison — not EVEN/ODD adjectives. 0 divisor → false. */
static inline int luke_i64_divisible(int64_t n, int64_t d) {
  return d != 0 && (n % d) == 0;
}

static inline int luke_divisible(double n, double d) {
  if (d == 0) return 0;
  long long di = (long long)(d >= 0 ? d + 0.5 : d - 0.5);
  long long ni = (long long)(n >= 0 ? n + 0.5 : n - 0.5);
  return di != 0 && (ni % di) == 0;
}

static inline LukeMap *luke_map_new(LukeArena *a) {
  LukeMap *m = (LukeMap *)luke_arena_alloc(a, sizeof(LukeMap), sizeof(void *));
  m->keys = NULL;
  m->vals = NULL;
  m->len = 0;
  m->cap = 0;
  return m;
}

static inline int luke_map_key_eq(LukeText a, LukeText b) {
  return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

static inline void luke_map_put(LukeArena *a, LukeMap *m, LukeText key, LukeText val) {
  if (!m) return;
  for (size_t i = 0; i < m->len; ++i) {
    if (luke_map_key_eq(m->keys[i], key)) {
      m->vals[i] = val;
      return;
    }
  }
  if (m->len + 1 > m->cap) {
    size_t ncap = m->cap ? m->cap * 2 : 4;
    LukeText *nk = (LukeText *)luke_arena_alloc(a, ncap * sizeof(LukeText), sizeof(void *));
    LukeText *nv = (LukeText *)luke_arena_alloc(a, ncap * sizeof(LukeText), sizeof(void *));
    if (m->keys && m->len) {
      memcpy(nk, m->keys, m->len * sizeof(LukeText));
      memcpy(nv, m->vals, m->len * sizeof(LukeText));
    }
    m->keys = nk;
    m->vals = nv;
    m->cap = ncap;
  }
  m->keys[m->len] = key;
  m->vals[m->len] = val;
  m->len++;
}

static inline LukeText luke_map_get(LukeMap *m, LukeText key) {
  if (!m) return luke_text("");
  for (size_t i = 0; i < m->len; ++i)
    if (luke_map_key_eq(m->keys[i], key)) return m->vals[i];
  return luke_text("");
}

static inline int luke_map_has(LukeMap *m, LukeText key) {
  if (!m) return 0;
  for (size_t i = 0; i < m->len; ++i)
    if (luke_map_key_eq(m->keys[i], key)) return 1;
  return 0;
}

static inline double luke_map_len(LukeMap *m) { return m ? (double)m->len : 0; }

/* Problem / last-error for conversational ATTEMPT */
static LukeText luke_last_problem = { "", 0 };
static inline void luke_set_problem(LukeText msg) { luke_last_problem = msg; }
static inline LukeText luke_the_problem(void) { return luke_last_problem; }
static inline int luke_has_problem(void) { return luke_last_problem.len > 0; }
static inline void luke_clear_problem(void) { luke_last_problem = luke_text(""); }

#ifdef __cplusplus
}
#endif

#endif /* LUKE_RT_H */
