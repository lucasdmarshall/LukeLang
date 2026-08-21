#include "luke/build.hpp"
#include "luke_ast.hpp"
#include "luke_expr.hpp"
#include "luke_parse.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace luke {
namespace {

std::string trim(const std::string &s) {
  std::size_t b = 0;
  while (b < s.size() && std::isspace((unsigned char)s[b])) ++b;
  std::size_t e = s.size();
  while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
  return s.substr(b, e - b);
}
std::string toUpper(std::string s) {
  for (char &c : s) c = (char)toupper((unsigned char)c);
  return s;
}
bool startsWithCI(const std::string &s, const std::string &p) {
  if (s.size() < p.size()) return false;
  for (size_t i = 0; i < p.size(); ++i)
    if (toupper((unsigned char)s[i]) != toupper((unsigned char)p[i])) return false;
  return true;
}

/* Rest after SPEAK/SAY/YELL/SHOUT EACH , or empty if not that statement. */
std::string afterSpeakEach(const std::string &text) {
  static const char *verbs[] = {"SPEAK ", "SAY ", "YELL ", "SHOUT "};
  for (const char *v : verbs) {
    std::string verb(v);
    if (!startsWithCI(text, verb)) continue;
    auto rest = trim(text.substr(verb.size()));
    if (!startsWithCI(rest, "EACH ")) continue;
    return trim(rest.substr(5));
  }
  return {};
}

bool isIdentName(const std::string &n) {
  if (n.empty()) return false;
  unsigned char c0 = (unsigned char)n[0];
  if (!std::isalpha(c0) && n[0] != '_') return false;
  for (char c : n)
    if (!std::isalnum((unsigned char)c) && c != '_') return false;
  return true;
}
bool stripDo(std::string &s) {
  auto U = toUpper(s);
  auto p = U.rfind(" DO");
  if (p != std::string::npos && p + 3 == U.size()) {
    s = trim(s.substr(0, p));
    return true;
  }
  return false;
}
std::string cIdent(const std::string &n) {
  std::string o;
  for (char c : n) o.push_back(isalnum((unsigned char)c) ? c : '_');
  if (o.empty() || isdigit((unsigned char)o[0])) o = "_" + o;
  /* Avoid C/C++ keywords that appear as Luke locals (e.g. short). */
  static const char *kw[] = {"auto",  "break",  "case",   "char",   "const",   "continue",
                             "default", "do",    "double", "else",   "enum",    "extern",
                             "float", "for",    "goto",   "if",     "int",     "long",
                             "register", "return", "short", "signed", "sizeof", "static",
                             "struct", "switch", "typedef", "union", "unsigned", "void",
                             "volatile", "while", "class", "new", "delete", "template",
                             "this", "friend", "inline", "virtual", "bool", "true", "false",
                             NULL};
  for (int i = 0; kw[i]; ++i)
    if (o == kw[i]) return o + "_";
  return o;
}
/* Conversational "the price" → "price" for reactive / locals. */
std::string stripThe(std::string n) {
  n = trim(n);
  if (startsWithCI(n, "THE ")) n = trim(n.substr(4));
  return n;
}
/* Phase 7 — Entity.field cell id when inside BEGIN ENTITY. */
std::string rxScopedCellName(const std::vector<std::string> &entityStack, const std::string &name) {
  auto n = stripThe(trim(name));
  if (!entityStack.empty()) return entityStack.back() + "." + n;
  return n;
}
std::string resolveRxCellName(const std::map<std::string, bool> &rxCells,
                              const std::vector<std::string> &entityStack, std::string name) {
  name = stripThe(trim(name));
  if (rxCells.count(name)) return name;
  if (name.find('.') != std::string::npos && rxCells.count(name)) return name;
  if (!entityStack.empty()) {
    auto scoped = entityStack.back() + "." + name;
    if (rxCells.count(scoped)) return scoped;
  }
  return name;
}
std::string esc(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '"') o += "\\\"";
    else if (c == '\n') o += "\\n";
    else o.push_back(c);
  }
  return o;
}
std::vector<std::string> splitArgs(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  bool q = false;
  char qc = 0;
  for (char c : s) {
    if (q) {
      cur.push_back(c);
      if (c == qc) q = false;
      continue;
    }
    if (c == '"' || c == '\'') {
      q = true;
      qc = c;
      cur.push_back(c);
      continue;
    }
    if (c == ',') {
      auto t = trim(cur);
      if (!t.empty()) out.push_back(t);
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  auto t = trim(cur);
  if (!t.empty()) out.push_back(t);
  return out;
}

/* Find needle in haystack (already uppercased) but ignore quoted spans in original. */
size_t findOutsideQuotes(const std::string &orig, const std::string &origUpper,
                         const std::string &needleUpper) {
  bool q = false;
  char qc = 0;
  int depth = 0; /* skip operators nested inside ( … ) — expr AST beachhead */
  for (size_t i = 0; i + needleUpper.size() <= origUpper.size(); ++i) {
    char c = orig[i];
    if (q) {
      if (c == qc) q = false;
      continue;
    }
    if (c == '"' || c == '\'') {
      q = true;
      qc = c;
      continue;
    }
    if (c == '(') {
      ++depth;
      continue;
    }
    if (c == ')') {
      if (depth > 0) --depth;
      continue;
    }
    if (depth != 0) continue;
    if (origUpper.compare(i, needleUpper.size(), needleUpper) == 0) return i;
  }
  return std::string::npos;
}

/* Strip one layer of matching outer parentheses: "(t1 SUBTRACT t0)" → "t1 SUBTRACT t0". */
std::string stripOuterParens(std::string e) {
  e = trim(e);
  for (;;) {
    if (e.size() < 2 || e.front() != '(') return e;
    int depth = 0;
    bool wrapped = false;
    for (size_t i = 0; i < e.size(); ++i) {
      char c = e[i];
      if (c == '"' || c == '\'') {
        char qc = c;
        ++i;
        while (i < e.size() && e[i] != qc) ++i;
        continue;
      }
      if (c == '(') ++depth;
      else if (c == ')') {
        --depth;
        if (depth == 0) {
          wrapped = (i + 1 == e.size());
          break;
        }
        if (depth < 0) return e;
      }
    }
    if (!wrapped || depth != 0) return e;
    e = trim(e.substr(1, e.size() - 2));
  }
}

std::string unquoteText(std::string s) {
  s = trim(s);
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
    s = s.substr(1, s.size() - 2);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        char n = s[++i];
        if (n == 'n') out.push_back('\n');
        else if (n == 't') out.push_back('\t');
        else if (n == 'r') out.push_back('\r');
        else out.push_back(n);
      } else
        out.push_back(s[i]);
    }
    return out;
  }
  return s;
}

enum class K { Num, Int, Flag, Text, Json, List, Map, Void, Ptr };
struct Ty {
  K k = K::Void;
  std::string klass;
  static Ty num() { return {K::Num, ""}; }
  static Ty integer() { return {K::Int, ""}; }
  static Ty flag() { return {K::Flag, ""}; }
  static Ty text() { return {K::Text, ""}; }
  static Ty json() { return {K::Json, ""}; }
  static Ty list() { return {K::List, ""}; }
  static Ty map() { return {K::Map, ""}; }
  static Ty vod() { return {K::Void, ""}; }
  static Ty ptr(const std::string &c) { return {K::Ptr, c}; }
};
std::string cTy(const Ty &t) {
  switch (t.k) {
    case K::Num: return "double";
    case K::Int: return "int64_t";
    case K::Flag: return "int";
    case K::Text: return "LukeText";
    case K::Json: return "LukeJson *";
    case K::List: return "LukeList *";
    case K::Map: return "LukeMap *";
    case K::Ptr:
      if (t.klass == "__HttpServer") return "LukeHttpServer *";
      if (t.klass == "__HttpReq") return "LukeHttpRequest *";
      if (t.klass == "__Db") return "LukeDb *";
      if (t.klass == "__Pg") return "LukePg *";
      return cIdent(t.klass) + " *";
    default: return "void";
  }
}
std::string tyName(const Ty &t) {
  switch (t.k) {
    case K::Num: return "NUMBER";
    case K::Int: return "INTEGER";
    case K::Flag: return "FLAG";
    case K::Text: return "TEXT";
    case K::Json: return "JSON";
    case K::List: return "LIST";
    case K::Map: return "MAP";
    case K::Ptr:
      if (t.klass == "__HttpServer") return "SERVER";
      if (t.klass == "__HttpReq") return "REQUEST";
      if (t.klass == "__Db") return "DATABASE";
      if (t.klass == "__Pg") return "PGCONN";
      return t.klass.empty() ? "blueprint" : t.klass;
    default: return "nothing";
  }
}
bool typesEqual(const Ty &a, const Ty &b) {
  if (a.k != b.k) return false;
  if (a.k == K::Ptr) return a.klass == b.klass;
  return true;
}

struct Param {
  std::string name;
  Ty ty;
};
struct Field {
  std::string name;
  Ty ty;
  std::string defRaw;
  bool priv = false;
  bool secret = false; /* auth-as-types: information-flow SECRET (≠ OOP PRIVATE) */
  std::string owner;
};
struct Method {
  std::string name;
  std::vector<Param> params;
  std::vector<std::string> body;
  std::vector<size_t> lines;
  std::vector<std::string> files;
  bool ctor = false;
};
struct BP {
  std::string name, parent;
  std::vector<Field> fields;
  std::vector<Method> methods;
};
struct Fn {
  std::string name;
  std::vector<Param> params;
  Ty ret = Ty::num();
  bool retDeclared = false;
  bool foreign = false;
  std::vector<std::string> body;
  std::vector<size_t> lines;
  std::vector<std::string> files;
};

struct Expr {
  std::string code;
  Ty ty;
};

struct TopStmt {
  size_t line = 0;
  std::string file;
  std::string text;
};

struct BC {
  std::string err;
  bool bad = false;
  bool unsupportedHint = false;
  bool forBrowser = false;
  std::string sourcePath;
  std::string curFile;
  int arenaSeq = 0;
  int attemptSeq = 0;
  int whenSeq = 0;
  bool needsViewportRelayout = false; /* STACK/WRAP BELOW → luke_viewport_relayout */
  std::vector<std::string> arenaMarks;
  std::vector<std::string> hankaStack; /* "COLUMN" | "ROW" | "STACK" | "GRID" */
  std::vector<std::string> forEachVars;
  int forEachSeq = 0;
  std::vector<std::string> attemptLabels;
  std::vector<bool> attemptHasOtherwise;
  std::map<std::string, BP> bps;
  std::vector<std::string> bpOrder;
  std::map<std::string, Fn> fns;
  std::vector<std::string> fnOrder;
  std::vector<TopStmt> top;
  std::map<std::string, Ty> locals;
  std::string curClass;
  Ty curRet = Ty::vod();
  bool hasCurRet = false;

  /* Page manifest (browser) */
  bool hasPage = false;
  std::string pageTitle;
  std::string pageStyle;
  std::string pageCssHref; /* optional <link> for PUBLISH --tailwind output */
  std::string pageBody;
  std::vector<BrowserFont> pageFonts;
  std::vector<BrowserWhen> pageWhens;
  BrowserWhen *curWhen = nullptr;

  /* Reactive Phase 1/2 — cells / derived / UI binds (see docs/REACTIVE.md) */
  bool usesRx = false;
  bool usesRxUi = false;
  std::string rxGraphVar = "_luke_rx";
  std::map<std::string, bool> rxCells;     /* name → cell or derived id */
  std::map<std::string, bool> rxDerived;   /* name → derived */
  std::map<std::string, bool> rxSecretCells; /* auth-as-types: SECRET cells */
  std::map<std::string, bool> rxScopedSecretOk; /* SECRET cell allowed via FOR CURRENT USER WATCH */
  /* Declarative auth FLOW — impossible states = compile error (VERIFY before DONE). */
  struct FlowDef {
    std::string name;
    std::vector<std::string> steps; /* collect | verify | done */
    std::vector<std::string> collectFields;
    bool hasVerify = false;
    bool hasDone = false;
    std::string verifyKind; /* CODE | TOTP | OAUTH | "" */
    std::string doneAction;
    size_t line = 0;
  };
  std::map<std::string, FlowDef> flows;
  std::vector<std::string> flowOrder;
  /* LIMIT name TO N PER MINUTE [PER ip] — reactive remaining cell name.remaining */
  struct LimitDef {
    std::string name;
    int maxN = 5;
    int windowSecs = 60;
    bool perIp = false;
    size_t line = 0;
  };
  std::map<std::string, LimitDef> limits;
  /* Declarative ROUTES — broken/unauthorized/mistyped link = compile error. */
  struct RouteDef {
    std::string method;  /* GET | POST | PUT | DELETE */
    std::string pattern; /* /user/:id */
    std::string paramName;
    std::string paramTy; /* INTEGER | TEXT | NUMBER */
    std::string handler; /* HANDLE name — for SERVE ROUTES dispatch */
    bool requiresAuth = false;
    bool touchesSecret = false;
    size_t line = 0;
  };
  std::vector<RouteDef> routes;
  bool hasRoutesBlock = false;
  bool serveRoutes = false; /* SERVE ROUTES ON … emits __luke_routes wrap */
  /* Versioned schema migrations (conventional up/down; no invented crypto). */
  struct MigrateStep {
    int version = 0;
    std::string upSql;
    std::string downSql;
    size_t line = 0;
  };
  struct MigrationDef {
    std::string name;
    std::vector<MigrateStep> steps;
    size_t line = 0;
  };
  std::map<std::string, MigrationDef> migrations;
  std::vector<std::string> migrationOrder;
  /* FORM name — one declaration for parse/validate beachhead. */
  struct FormField {
    std::string name;
    std::string ty; /* TEXT | EMAIL | INTEGER | PASSWORD */
    bool hasRange = false;
    long minV = 0;
    long maxV = 0;
  };
  struct FormDef {
    std::string name;
    std::vector<FormField> fields;
    size_t line = 0;
  };
  std::map<std::string, FormDef> forms;
  std::vector<std::string> formOrder;
  /* SCHEMA table — schema-as-types beachhead. */
  struct SchemaField {
    std::string name;
    std::string sqlTy; /* INTEGER | TEXT | REAL */
  };
  struct SchemaDef {
    std::string name;
    std::vector<SchemaField> fields;
    size_t line = 0;
  };
  std::map<std::string, SchemaDef> schemas;
  std::vector<std::string> schemaOrder;
  /* Middleware capability order — AUTH before RATE LIMIT. */
  std::vector<std::string> middlewareOrder;
  bool hasMiddlewareOrder = false;
  std::map<std::string, Ty> rxCellTy;      /* name → NUMBER or TEXT */
  std::vector<std::string> rxCellOrder;
  struct RxDerivedDef {
    std::string name;
    std::string exprRaw;
    size_t line = 0;
  };
  std::vector<RxDerivedDef> rxDerivedDefs;
  struct RxBindDef {
    std::string argusId; /* unquoted element id */
    std::string exprRaw;
    size_t line = 0;
    int seq = 0;
  };
  std::vector<RxBindDef> rxBindDefs;
  int rxBindSeq = 0;
  /* Phase 5 — BIND LIST name AS "prefix" */
  struct RxListBindDef {
    std::string listName;
    std::string prefixRaw;
    size_t line = 0;
    int seq = 0;
  };
  std::vector<RxListBindDef> rxListBindDefs;
  struct RxOpacityBindDef {
    std::string argusId;
    std::string exprRaw;
    size_t line = 0;
    int seq = 0;
  };
  std::vector<RxOpacityBindDef> rxOpacityBindDefs;
  std::vector<std::string> rxEntityStack;
  struct RxTimelineBind {
    std::string jobId;
    std::string progressCell;
    double from = 0;
    double to = 1;
    double ms = 0;
    size_t line = 0;
  };
  std::vector<RxTimelineBind> rxTimelineBinds;
  bool usesTimeline = false;
  struct RxQueryDef {
    std::string name;
    std::string dbLocal;
    std::string sql;
    std::string readSql;   /* sql used to read the current cell value */
    std::string baseTable; /* primary table for triggers */
    std::string baseTable2; /* optional second table for JOIN IVM */
    std::string ivmTable;   /* maintained view table */
    std::string ivmValueSql; /* scalar SELECT that produces the maintained TEXT v */
    std::string eventLog;    /* append-only causal log for SSE resume / time-travel */
    std::string wherePred;
    std::string ivmColExpr;
    bool hasIvm = false;
    bool hasDiffTrig = false; /* NEW/OLD differential triggers (simple id= pred) */
    bool hasJoin = false;
    bool scopedToUser = false; /* FOR CURRENT USER — per-request bind, no shared IVM */
    size_t line = 0;
  };
  std::vector<RxQueryDef> rxQueryDefs;
  struct RxWhenDef {
    std::string cellName;
    std::vector<std::string> body;
    std::vector<size_t> lines;
    std::vector<std::string> files;
    size_t line = 0;
    int seq = 0;
    bool background = false;
    bool weak = false;
  };
  std::vector<RxWhenDef> rxWhenDefs;
  int rxWhenSeq = 0;
  std::vector<std::string> rxComponentStack; /* open BEGIN COMPONENT names */
  std::vector<std::string> rxBoundaryStack;  /* open BEGIN ERROR BOUNDARY names */
  /* Phase 4 — async fetch → reactive cells */
  struct RxFetchBind {
    std::string jobId;
    std::string bodyCell;
    std::string statusCell;
    std::string readyCell;
    size_t line = 0;
  };
  std::vector<RxFetchBind> rxFetchBinds;
  /* Spike A part 2 — SSE subscribe → reactive cells (same write path as FETCH) */
  struct RxSubscribeBind {
    std::string jobId;
    std::string bodyCell;
    std::string readyCell;
    size_t line = 0;
  };
  std::vector<RxSubscribeBind> rxSubscribeBinds;
  std::vector<std::string> httpServeHandlers;
  bool needsPthread = false;

  void fail(size_t line, const std::string &m) {
    if (bad) return;
    bad = true;
    err = "Build error on line " + std::to_string(line) + ": " + m;
  }

  void expectTy(size_t line, const Ty &got, const Ty &want, const std::string &what) {
    if (want.k == K::Void || got.k == K::Void) return;
    if (typesEqual(got, want)) return;
    if (want.k == K::Num && got.k == K::Int) return;
    if (want.k == K::Int && got.k == K::Num) return;
    fail(line, what + " wants " + tyName(want) + " but got " + tyName(got));
  }

  Expr coerceTo(size_t line, const Expr &e, const Ty &want, const std::string &what) {
    expectTy(line, e.ty, want, what);
    if (bad) return e;
    if (want.k == K::Num && e.ty.k == K::Int)
      return {"((double)(" + e.code + "))", Ty::num()};
    if (want.k == K::Int && e.ty.k == K::Num)
      return {"luke_number_to_integer(" + e.code + ")", Ty::integer()};
    Expr out = e;
    if (want.k != K::Void) out.ty = want;
    return out;
  }

  bool isNumeric(const Ty &t) { return t.k == K::Num || t.k == K::Int; }

  std::vector<Expr> checkCallArgs(size_t line, const std::string &callee,
                                  const std::vector<Param> &params,
                                  const std::vector<std::string> &args) {
    std::vector<Expr> out;
    if (args.size() != params.size()) {
      fail(line, "'" + callee + "' expects " + std::to_string(params.size()) + " argument" +
                     (params.size() == 1 ? "" : "s") + " but got " + std::to_string(args.size()));
      return out;
    }
    for (size_t i = 0; i < params.size(); ++i) {
      auto e = expr(args[i], line);
      if (bad) return out;
      if (params[i].ty.k == K::Void) {
        fail(line, "'" + callee + "' parameter '" + params[i].name +
                       "' has an unknown type — use AS NUMBER/TEXT/FLAG/JSON or a blueprint name");
        return out;
      }
      out.push_back(coerceTo(line, e, params[i].ty,
                             "'" + callee + "' argument '" + params[i].name + "'"));
    }
    return out;
  }

  void expectArgs(size_t line, const std::string &callee, const std::vector<Param> &params,
                  const std::vector<std::string> &args) {
    (void)checkCallArgs(line, callee, params, args);
  }

  Ty parseTy(const std::string &t) {
    auto U = toUpper(t);
    if (U == "NUMBER" || U == "NUM") return Ty::num();
    if (U == "INTEGER" || U == "INT") return Ty::integer();
    if (U == "FLAG" || U == "BOOL") return Ty::flag();
    if (U == "TEXT" || U == "STRING") return Ty::text();
    if (U == "JSON") return Ty::json();
    if (U == "LIST") return Ty::list();
    if (U == "MAP") return Ty::map();
    if (U == "SERVER" || U == "HTTPSERVER") return Ty::ptr("__HttpServer");
    if (U == "REQUEST" || U == "HTTPREQ") return Ty::ptr("__HttpReq");
    if (U == "DATABASE" || U == "DB") return Ty::ptr("__Db");
    if (U == "PGCONN" || U == "PG" || U == "POSTGRES") return Ty::ptr("__Pg");
    if (bps.count(t)) return Ty::ptr(t);
    return Ty::vod();
  }

  /* Strip leading SECRET from a type phrase; returns remaining type string. */
  std::string stripSecretTy(std::string t, bool *secretOut) {
    t = trim(t);
    if (startsWithCI(t, "SECRET ")) {
      if (secretOut) *secretOut = true;
      return trim(t.substr(7));
    }
    if (toUpper(t) == "SECRET") {
      if (secretOut) *secretOut = true;
      return "TEXT";
    }
    return t;
  }

  /* Names referenced by a simple BIND expression (identifiers + dotted cells). */
  std::vector<std::string> secretTouchNames(const std::string &exprRaw) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
      auto n = stripThe(trim(cur));
      cur.clear();
      if (n.empty()) return;
      auto U = toUpper(n);
      if (U == "AND" || U == "OR" || U == "NOT" || U == "THE" || U == "CURRENT" || U == "USER")
        return;
      out.push_back(n);
    };
    for (size_t i = 0; i < exprRaw.size(); ++i) {
      char c = exprRaw[i];
      if (isalnum((unsigned char)c) || c == '_' || c == '.') {
        cur.push_back(c);
      } else {
        flush();
      }
    }
    flush();
    return out;
  }

  void lintSecretBind(size_t line, const std::string &exprRaw) {
    for (auto &n : secretTouchNames(exprRaw)) {
      auto key = resolveRxCellName(rxCells, rxEntityStack, n);
      bool isSecret = rxSecretCells.count(key) || rxSecretCells.count(n);
      if (!isSecret) {
        /* Blueprint field: name after last dot, or bare field marked secret on any BP. */
        auto dot = n.find('.');
        std::string field = dot == std::string::npos ? n : n.substr(dot + 1);
        for (auto &kv : bps) {
          for (auto &f : kv.second.fields)
            if (f.name == field && f.secret) {
              isSecret = true;
              key = field;
              break;
            }
          if (isSecret) break;
        }
      }
      if (!isSecret) continue;
      if (rxScopedSecretOk.count(key) || rxScopedSecretOk.count(n)) continue;
      fail(line, "SECRET '" + n + "' can only BIND when WATCH … FOR CURRENT USER scopes it — "
                 "unauthorized access is a compile error");
      return;
    }
  }

  Param parseParam(const std::string &raw) {
    Param p;
    auto s = trim(raw);
    auto U = toUpper(s);
    auto as = U.find(" AS ");
    if (as != std::string::npos) {
      p.name = trim(s.substr(0, as));
      auto tyRaw = trim(s.substr(as + 4));
      p.ty = parseTy(tyRaw);
      if (p.ty.k == K::Void) {
        // Unknown annotation — keep Void so callers can fail with context.
        p.ty = Ty::vod();
      }
    } else {
      p.name = s;
      p.ty = Ty::num();
    }
    return p;
  }

  std::vector<Field> flatFields(const std::string &klass) {
    std::vector<std::string> chain;
    for (std::string c = klass; !c.empty(); c = bps[c].parent) chain.push_back(c);
    std::map<std::string, Field> latest;
    for (int i = (int)chain.size() - 1; i >= 0; --i)
      for (auto &f : bps[chain[(size_t)i]].fields) latest[f.name] = f;
    // preserve order: base to derived unique
    std::vector<Field> out;
    std::set<std::string> seen;
    for (int i = (int)chain.size() - 1; i >= 0; --i) {
      for (auto &f : bps[chain[(size_t)i]].fields) {
        if (seen.count(f.name)) continue;
        seen.insert(f.name);
        out.push_back(latest[f.name]);
      }
    }
    return out;
  }

  std::string fname(const Field &f) {
    return f.priv ? ("_priv_" + cIdent(f.name)) : cIdent(f.name);
  }

  Expr coerceText(const Expr &e) {
    if (e.ty.k == K::Text) return e;
    if (e.ty.k == K::Num) return {"luke_number_to_text(arena, (" + e.code + "))", Ty::text()};
    if (e.ty.k == K::Int) return {"luke_integer_to_text(arena, (" + e.code + "))", Ty::text()};
    if (e.ty.k == K::Flag)
      return {"luke_text((" + e.code + ") ? \"true\" : \"false\")", Ty::text()};
    return {"luke_text(\"\")", Ty::text()};
  }

  Expr primary(std::string e, size_t line);
  Expr expr(std::string e, size_t line);
  Expr exprLegacy(std::string e, size_t line); /* THE_/ASK/IF path — no Pratt re-entry */
};

Expr BC::primary(std::string e, size_t line) {
  e = trim(e);

  // Native stdlib / runtime calls: __luke_read_file(path), etc.
  if (e.size() > 2 && e[0] == '_' && e[1] == '_') {
    auto lp = e.find('(');
    auto rp = e.rfind(')');
    if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
      auto callee = trim(e.substr(0, lp));
      auto args = splitArgs(e.substr(lp + 1, rp - lp - 1));
      auto mapCall = [&](const std::string &cName, Ty ret, bool arenaFirst) -> Expr {
        std::ostringstream call;
        call << cName << "(";
        if (arenaFirst) call << "arena";
        for (size_t i = 0; i < args.size(); ++i) {
          if (arenaFirst || i) call << ", ";
          call << expr(args[i], line).code;
        }
        call << ")";
        return {call.str(), ret};
      };
      if (callee == "__luke_read_file") return mapCall("luke_read_file", Ty::text(), true);
      if (callee == "__luke_write_file") return mapCall("luke_write_file", Ty::flag(), false);
      if (callee == "__luke_file_exists") return mapCall("luke_file_exists", Ty::flag(), false);
      if (callee == "__luke_json_string") return mapCall("luke_json_string", Ty::text(), true);
      if (callee == "__luke_json_parse") return mapCall("luke_json_parse", Ty::json(), true);
      if (callee == "__luke_json_get") return mapCall("luke_json_get", Ty::json(), false);
      if (callee == "__luke_json_index") return mapCall("luke_json_index", Ty::json(), false);
      if (callee == "__luke_json_len") return mapCall("luke_json_len", Ty::num(), false);
      if (callee == "__luke_json_has") return mapCall("luke_json_has", Ty::flag(), false);
      if (callee == "__luke_json_as_text") return mapCall("luke_json_as_text", Ty::text(), true);
      if (callee == "__luke_json_as_number") return mapCall("luke_json_as_number", Ty::num(), false);
      if (callee == "__luke_json_as_integer")
        return mapCall("luke_json_as_integer", Ty::integer(), false);
      if (callee == "__luke_json_as_flag") return mapCall("luke_json_as_flag", Ty::flag(), false);
      if (callee == "__luke_json_stringify") return mapCall("luke_json_stringify", Ty::text(), true);
      if (callee == "__luke_json_is_null") return mapCall("luke_json_is_null", Ty::flag(), false);
      if (callee == "__luke_http_get") return mapCall("luke_http_get", Ty::text(), true);
      if (callee == "__luke_http_listen")
        return mapCall("luke_http_listen", Ty::ptr("__HttpServer"), true);
      if (callee == "__luke_http_accept")
        return mapCall("luke_http_accept", Ty::ptr("__HttpReq"), true);
      if (callee == "__luke_http_reply") return mapCall("luke_http_reply", Ty::flag(), false);
      if (callee == "__luke_http_chunk_open")
        return mapCall("luke_http_chunk_open", Ty::flag(), false);
      if (callee == "__luke_http_chunk") return mapCall("luke_http_chunk", Ty::flag(), false);
      if (callee == "__luke_http_chunk_end")
        return mapCall("luke_http_chunk_end", Ty::flag(), false);
      if (callee == "__luke_http_client_ip")
        return mapCall("luke_http_client_ip", Ty::text(), true);
      if (callee == "__luke_http_sse_open") return mapCall("luke_http_sse_open", Ty::flag(), false);
      if (callee == "__luke_http_sse_data") return mapCall("luke_http_sse_data", Ty::flag(), false);
      if (callee == "__luke_http_sse_id") return mapCall("luke_http_sse_id", Ty::flag(), false);
      if (callee == "__luke_http_sse_comment")
        return mapCall("luke_http_sse_comment", Ty::flag(), false);
      if (callee == "__luke_http_close") return mapCall("luke_http_close", Ty::flag(), false);
      if (callee == "__luke_http_path") return mapCall("luke_http_path", Ty::text(), false);
      if (callee == "__luke_http_method") return mapCall("luke_http_method", Ty::text(), false);
      if (callee == "__luke_http_query") return mapCall("luke_http_query", Ty::text(), false);
      if (callee == "__luke_http_body") return mapCall("luke_http_body", Ty::text(), false);
      if (callee == "__luke_http_last_event_id")
        return mapCall("luke_http_last_event_id", Ty::text(), false);
      if (callee == "__luke_http_match") return mapCall("luke_http_match", Ty::flag(), true);
      if (callee == "__luke_http_query_map")
        return mapCall("luke_http_query_map", Ty::map(), true);
      if (callee == "__luke_http_form_map") return mapCall("luke_http_form_map", Ty::map(), true);
      if (callee == "__luke_http_header") return mapCall("luke_http_header", Ty::text(), true);
      if (callee == "__luke_http_cookie") return mapCall("luke_http_cookie", Ty::text(), true);
      if (callee == "__luke_http_set_cookie")
        return mapCall("luke_http_set_cookie", Ty::flag(), true);
      if (callee == "__luke_db_open") return mapCall("luke_db_open", Ty::ptr("__Db"), true);
      if (callee == "__luke_db_exec") return mapCall("luke_db_exec", Ty::flag(), false);
      if (callee == "__luke_db_exec_bind") return mapCall("luke_db_exec_bind", Ty::flag(), false);
      if (callee == "__luke_db_query") return mapCall("luke_db_query_text", Ty::text(), true);
      if (callee == "__luke_db_query_bind")
        return mapCall("luke_db_query_bind_text", Ty::text(), true);
      if (callee == "__luke_db_close") return mapCall("luke_db_close", Ty::flag(), false);
      if (callee == "__luke_pg_open") return mapCall("luke_pg_open", Ty::ptr("__Pg"), true);
      if (callee == "__luke_pg_exec_bind") return mapCall("luke_pg_exec_bind", Ty::flag(), false);
      if (callee == "__luke_pg_query_bind")
        return mapCall("luke_pg_query_bind", Ty::text(), true);
      if (callee == "__luke_pg_rows_bind") return mapCall("luke_pg_rows_bind", Ty::text(), true);
      if (callee == "__luke_pg_close") return mapCall("luke_pg_close", Ty::flag(), false);
      if (callee == "__luke_pg_checkout") return mapCall("luke_pg_checkout", Ty::flag(), false);
      if (callee == "__luke_pg_checkin") return mapCall("luke_pg_checkin", Ty::flag(), false);
      if (callee == "__luke_auth_init") return mapCall("luke_auth_init", Ty::flag(), false);
      if (callee == "__luke_auth_create_account")
        return mapCall("luke_auth_create_account", Ty::text(), true);
      if (callee == "__luke_auth_login") return mapCall("luke_auth_login", Ty::text(), true);
      if (callee == "__luke_auth_logout") return mapCall("luke_auth_logout", Ty::flag(), true);
      if (callee == "__luke_auth_require") return mapCall("luke_auth_require", Ty::flag(), true);
      if (callee == "__luke_auth_csrf") return mapCall("luke_auth_csrf", Ty::text(), true);
      if (callee == "__luke_auth_check_csrf")
        return mapCall("luke_auth_check_csrf", Ty::flag(), true);
      if (callee == "__luke_auth_who_saw") return mapCall("luke_auth_who_saw", Ty::text(), true);
      if (callee == "__luke_auth_who_saw_since")
        return mapCall("luke_auth_who_saw_since", Ty::text(), true);
      if (callee == "__luke_auth_assume") return mapCall("luke_auth_assume", Ty::flag(), false);
      if (callee == "__luke_auth_attempts_left")
        return mapCall("luke_auth_attempts_left", Ty::text(), true);
      if (callee == "__luke_auth_scrub_to_access")
        return mapCall("luke_auth_scrub_to_access", Ty::text(), true);
      if (callee == "__luke_auth_saw_verify")
        return {"((double)luke_auth_saw_verify())", Ty::num()};
      if (callee == "__luke_list_new") return mapCall("luke_list_new", Ty::list(), true);
      if (callee == "__luke_list_add") return mapCall("luke_list_add", Ty::vod(), true);
      if (callee == "__luke_list_get") return mapCall("luke_list_get", Ty::text(), false);
      if (callee == "__luke_list_len") return mapCall("luke_list_len", Ty::num(), false);
      if (callee == "__luke_map_new") return mapCall("luke_map_new", Ty::map(), true);
      if (callee == "__luke_map_put") return mapCall("luke_map_put", Ty::vod(), true);
      if (callee == "__luke_map_get") return mapCall("luke_map_get", Ty::text(), false);
      if (callee == "__luke_map_has") return mapCall("luke_map_has", Ty::flag(), false);
      if (callee == "__luke_map_len") return mapCall("luke_map_len", Ty::num(), false);
      if (callee == "__luke_js_fetch") return mapCall("luke_js_fetch", Ty::text(), true);
      if (callee == "__luke_js_on_click") return mapCall("luke_js_on_click", Ty::flag(), false);
      if (callee == "__luke_arg_count") return {"((double)luke_arg_count())", Ty::num()};
      if (callee == "__luke_get_arg") return mapCall("luke_get_arg", Ty::text(), true);
      if (callee == "__luke_get_env") return mapCall("luke_get_env", Ty::text(), true);
      if (callee == "__luke_set_env") return mapCall("luke_set_env", Ty::flag(), false);
      if (callee == "__luke_cwd") return mapCall("luke_cwd", Ty::text(), true);
      if (callee == "__luke_path_join") return mapCall("luke_path_join", Ty::text(), true);
      if (callee == "__luke_path_basename") return mapCall("luke_path_basename", Ty::text(), true);
      if (callee == "__luke_path_dirname") return mapCall("luke_path_dirname", Ty::text(), true);
      if (callee == "__luke_shell") return mapCall("luke_shell", Ty::text(), true);
      if (callee == "__luke_exit") return mapCall("luke_exit_code", Ty::num(), false);
      if (callee == "__luke_js_set_text") return mapCall("luke_js_set_text", Ty::flag(), false);
      if (callee == "__luke_js_set_html") return mapCall("luke_js_set_html", Ty::flag(), false);
      if (callee == "__luke_js_get_value") return mapCall("luke_js_get_value", Ty::text(), true);
      if (callee == "__luke_js_add_style") return mapCall("luke_js_add_style", Ty::flag(), false);
      if (callee == "__luke_js_load_font") return mapCall("luke_js_load_font", Ty::flag(), false);
      if (callee == "__luke_js_set_title") return mapCall("luke_js_set_title", Ty::flag(), false);
      if (callee == "__argus_place_text" || callee == "__luke_render_place_text")
        return mapCall("argus_place_text", Ty::flag(), true);
      if (callee == "__argus_place_button" || callee == "__luke_render_place_button")
        return mapCall("argus_place_button", Ty::flag(), true);
      if (callee == "__argus_place_image" || callee == "__luke_render_place_image")
        return mapCall("argus_place_image", Ty::flag(), true);
      if (callee == "__argus_place_box" || callee == "__luke_render_place_box")
        return mapCall("argus_place_box", Ty::flag(), true);
      if (callee == "__argus_place_input") return mapCall("argus_place_input", Ty::flag(), true);
      if (callee == "__argus_paint" || callee == "__luke_render_paint") {
        return {"(argus_paint(arena), 1)", Ty::flag()};
      }
      if (callee == "__argus_clear") {
        return {"(argus_clear(arena), 1)", Ty::flag()};
      }
      if (callee == "__luke_js_route_go") return mapCall("luke_js_route_go", Ty::flag(), false);
      if (callee == "__luke_js_fetch_start") return mapCall("luke_js_fetch_start", Ty::flag(), false);
      if (callee == "__luke_js_fetch_body") return mapCall("luke_js_fetch_body", Ty::text(), true);
      if (callee == "__luke_js_fetch_status")
        return mapCall("luke_js_fetch_status", Ty::num(), false);
      if (callee == "__luke_js_fetch_ready") return mapCall("luke_js_fetch_ready", Ty::flag(), false);
      if (callee == "__luke_looks_like_email")
        return mapCall("luke_looks_like_email", Ty::flag(), false);
      if (callee == "__luke_text_nonempty") return mapCall("luke_text_nonempty", Ty::flag(), false);
      if (callee == "__hanka_begin_column")
        return mapCall("hanka_begin_column", Ty::flag(), true);
      if (callee == "__hanka_begin_row") return mapCall("hanka_begin_row", Ty::flag(), true);
      if (callee == "__hanka_begin_stack") return mapCall("hanka_begin_stack", Ty::flag(), true);
      if (callee == "__hanka_set_align") return mapCall("hanka_set_align", Ty::flag(), true);
      if (callee == "__hanka_set_wrap") return mapCall("hanka_set_wrap", Ty::flag(), true);
      if (callee == "__hanka_slot_text") return mapCall("hanka_slot_text", Ty::flag(), true);
      if (callee == "__hanka_slot_button") return mapCall("hanka_slot_button", Ty::flag(), true);
      if (callee == "__hanka_slot_image") return mapCall("hanka_slot_image", Ty::flag(), true);
      if (callee == "__hanka_slot_input") return mapCall("hanka_slot_input", Ty::flag(), true);
      if (callee == "__hanka_end") return {"(hanka_end(arena), 1)", Ty::flag()};
      if (callee == "__hanka_layout") return {"(hanka_layout(arena), 1)", Ty::flag()};
      fail(line, "Unknown native helper '" + callee +
                     "' — IMPORT std/files, std/json, std/http, std/server, std/sqlite, "
                     "std/pg, std/auth, std/args, std/env, std/paths, std/process, or std/js");
      return {"0", Ty::num()};
    }
  }

  if (e.size() >= 2 && ((e.front() == '"' && e.back() == '"') || (e.front() == '\'' && e.back() == '\''))) {
    auto raw = e.substr(1, e.size() - 2);
    std::string unesc;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '\\' && i + 1 < raw.size()) {
        char n = raw[++i];
        if (n == 'n') unesc.push_back('\n');
        else if (n == 't') unesc.push_back('\t');
        else if (n == 'r') unesc.push_back('\r');
        else unesc.push_back(n);
      } else
        unesc.push_back(raw[i]);
    }
    return {"luke_text(\"" + esc(unesc) + "\")", Ty::text()};
  }
  auto U = toUpper(e);
  if (U == "TRUE" || U == "YES") return {"1", Ty::flag()};
  if (U == "FALSE" || U == "NO") return {"0", Ty::flag()};
  if (U == "SELF") {
    if (curClass.empty()) {
      fail(line, "SELF only works inside a METHOD or WHEN BORN — you're not in a blueprint method here");
      return {"0", Ty::num()};
    }
    return {"self", Ty::ptr(curClass)};
  }
  if (startsWithCI(e, "SELF.")) {
    auto field = trim(e.substr(5));
    for (auto &f : flatFields(curClass)) {
      if (f.name == field) {
        if (f.priv && f.owner != curClass) {
          fail(line, "Field '" + field + "' is PRIVATE/SECRET on " + f.owner +
                          " — only that blueprint's methods may touch it");
          return {"0", Ty::num()};
        }
        return {"self->" + fname(f), f.ty};
      }
    }
    fail(line, "No field '" + field + "' on blueprint " + curClass + " — declare it with HAS");
    return {"0", Ty::num()};
  }
  char *end = nullptr;
  std::strtod(e.c_str(), &end);
  if (end && end != e.c_str() && *end == '\0') {
    bool isIntLit = true;
    for (char c : e) {
      if (c == '.' || c == 'e' || c == 'E') { isIntLit = false; break; }
    }
    if (isIntLit) {
      errno = 0;
      long long v = std::strtoll(e.c_str(), &end, 10);
      if (errno == ERANGE || !end || *end != '\0') {
        fail(line, "INTEGER literal '" + e + "' is outside int64 range");
        return {"0LL", Ty::integer()};
      }
      (void)v;
      return {e + "LL", Ty::integer()};
    }
    return {e, Ty::num()};
  }

  /* "THE price" → local/reactive "price" when declared. */
  if (startsWithCI(e, "THE ")) {
    auto naked = trim(e.substr(4));
    if (locals.count(naked) || rxCells.count(naked)) e = naked;
  }

  if (!rxCells.count(e)) {
    for (auto &kv : rxCells) {
      auto dot = kv.first.find('.');
      if (dot == std::string::npos) continue;
      if (kv.first.substr(dot + 1) == e) {
        e = kv.first;
        break;
      }
    }
  }

  if (!rxEntityStack.empty()) {
    auto scoped = rxEntityStack.back() + "." + e;
    if (rxCells.count(scoped)) e = scoped;
  }

  bool words = !e.empty();
  for (char c : e)
    if (!(isalpha((unsigned char)c) || isspace((unsigned char)c))) words = false;
  if (words && e.find(' ') != std::string::npos)
    return {"luke_text(\"" + esc(e) + "\")", Ty::text()};

  if (rxCells.count(e)) {
    Ty ty = rxCellTy.count(e) ? rxCellTy[e] : (locals.count(e) ? locals[e] : Ty::num());
    if (ty.k == K::List)
      return {"luke_rx_list_ptr(" + rxGraphVar + ", _luke_rx_id_" + cIdent(e) + ")", Ty::list()};
    if (ty.k == K::Map)
      return {"luke_rx_map_ptr(" + rxGraphVar + ", _luke_rx_id_" + cIdent(e) + ")", Ty::map()};
    if (ty.k == K::Text)
      return {"luke_rx_read_text(" + rxGraphVar + ", _luke_rx_id_" + cIdent(e) + ")", Ty::text()};
    if (ty.k == K::Int)
      return {"luke_rx_read_int(" + rxGraphVar + ", _luke_rx_id_" + cIdent(e) + ")", Ty::integer()};
    return {"luke_rx_read_num(" + rxGraphVar + ", _luke_rx_id_" + cIdent(e) + ")", Ty::num()};
  }
  if (locals.count(e)) return {cIdent(e), locals[e]};

  auto dot = e.find('.');
  if (dot != std::string::npos) {
    auto obj = e.substr(0, dot), field = e.substr(dot + 1);
    if (locals.count(obj) && locals[obj].k == K::Ptr) {
      for (auto &f : flatFields(locals[obj].klass))
        if (f.name == field) return {cIdent(obj) + "->" + fname(f), f.ty};
      fail(line, "No field '" + field + "' on " + locals[obj].klass);
      return {"0", Ty::num()};
    }
  }

  fail(line, "I don't know '" + e + "' yet — declare it with MY NAME IS … SET TO … "
             "(or AS NUMBER/INTEGER/TEXT/FLAG)");
  return {"0", Ty::num()};
}

Expr BC::expr(std::string e, size_t line) {
  e = stripOuterParens(trim(e));
  if (e.empty()) return {"0", Ty::num()};

  /* Production expr path: tokenize → Pratt AST → C (idents resolve via legacy). */
  {
    bool wasBad = bad;
    std::string wasErr = err;
    luke::ExprLower ctx;
    ctx.fail = [&](size_t ln, const std::string &msg) { fail(ln, msg); };
    ctx.resolve = [&](const std::string &atom, size_t ln) {
      /* Probe resolve must not poison the compile — undo fail on unknown atoms. */
      bool probeBad = bad;
      std::string probeErr = err;
      Expr p = exprLegacy(atom, ln);
      if (bad && !probeBad) {
        bad = probeBad;
        err = probeErr;
        return std::make_pair(std::string("0"), std::string("err"));
      }
      if (p.ty.k == K::Int) return std::make_pair(p.code, std::string("int"));
      if (p.ty.k == K::Num) return std::make_pair(p.code, std::string("num"));
      if (p.ty.k == K::Text) return std::make_pair(p.code, std::string("text"));
      if (p.ty.k == K::Flag) return std::make_pair(p.code, std::string("flag"));
      return std::make_pair(p.code, std::string("err"));
    };
    auto compiled = luke::compileExpr(e, line, ctx);
    if (compiled.second == "legacy" || compiled.second == "err" || compiled.first.empty()) {
      /* Restore pre-probe error state if Pratt only explored. */
      if (!wasBad && bad && (compiled.second == "legacy" || compiled.second == "err")) {
        bad = wasBad;
        err = wasErr;
      }
    } else {
      if (compiled.second == "int") return {compiled.first, Ty::integer()};
      if (compiled.second == "text") return {compiled.first, Ty::text()};
      if (compiled.second == "flag") return {compiled.first, Ty::flag()};
      return {compiled.first, Ty::num()};
    }
  }
  return exprLegacy(e, line);
}

Expr BC::exprLegacy(std::string e, size_t line) {
  e = stripOuterParens(trim(e));
  if (e.empty()) return {"0", Ty::num()};

  {
    auto U0 = toUpper(e);
    if (startsWithCI(e, "THE NUMBERS FROM ")) {
      auto rest = trim(e.substr(17));
      auto U = toUpper(rest);
      auto toPos = findOutsideQuotes(rest, U, " TO ");
      if (toPos == std::string::npos) {
        fail(line, "THE NUMBERS FROM needs: THE NUMBERS FROM lo TO hi");
        return {"luke_list_new(arena)", Ty::list()};
      }
      auto lo = expr(trim(rest.substr(0, toPos)), line);
      auto hi = expr(trim(rest.substr(toPos + 4)), line);
      if (!isNumeric(lo.ty) || !isNumeric(hi.ty)) {
        fail(line, "THE NUMBERS FROM … TO … wants NUMBER or INTEGER");
        return {"luke_list_new(arena)", Ty::list()};
      }
      Expr loN = coerceTo(line, lo, Ty::num(), "THE NUMBERS FROM");
      Expr hiN = coerceTo(line, hi, Ty::num(), "THE NUMBERS FROM");
      return {"luke_numbers_from_to(arena, " + loN.code + ", " + hiN.code + ")", Ty::list()};
    }
    if (U0 == "THE VIEWPORT WIDTH" || U0 == "THE WINDOW WIDTH")
      return {"argus_viewport_width()", Ty::num()};
    if (U0 == "THE VIEWPORT HEIGHT" || U0 == "THE WINDOW HEIGHT")
      return {"argus_viewport_height()", Ty::num()};
    if (U0 == "THE CLOCK" || U0 == "THE TIME IN MILLISECONDS" || U0 == "THE CLOCK IN MILLISECONDS")
      return {"argus_now_ms()", Ty::num()};
    if (U0 == "THE CURRENT USER" || U0 == "THE CURRENT USER ID")
      return {"luke_auth_current_user()", Ty::text()};
    if (U0 == "THE BENCH MEDIAN" || U0 == "THE BENCHMARK MEDIAN")
      return {"luke_bench_median()", Ty::num()};
    if (U0 == "THE BENCH MIN" || U0 == "THE BENCHMARK MIN")
      return {"luke_bench_min()", Ty::num()};
    if (U0 == "THE BENCH SAMPLE COUNT" || U0 == "THE BENCHMARK SAMPLE COUNT")
      return {"luke_bench_sample_count()", Ty::num()};
    if (U0 == "THE GRANULAR PAINT COUNT" || U0 == "GRANULAR PAINTS" ||
        U0 == "THE GRANULAR PAINTS") {
      usesRx = true;
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->granular_paints : 0.0)", Ty::num()};
    }
    if (U0 == "THE EPOCH" || U0 == "THE REACTIVE EPOCH")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->epoch : 0.0)", Ty::num()};
    if (U0 == "THE FLUSH COUNT" || U0 == "THE REACTIVE FLUSH COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->flush_count : 0.0)", Ty::num()};
    if (U0 == "THE DERIVED RUN COUNT" || U0 == "THE LAST DERIVED RUN COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_flush_derived : 0.0)",
              Ty::num()};
    if (U0 == "THE EFFECT RUN COUNT" || U0 == "THE LAST EFFECT RUN COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_flush_effects : 0.0)",
              Ty::num()};
    if (U0 == "THE STALE EDGE COUNT" || U0 == "THE STALE EDGES CLEARED" ||
        U0 == "THE LAST STALE EDGE COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_flush_deps_cleared : 0.0)",
              Ty::num()};
    if (U0 == "THE TOTAL STALE EDGE COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->total_deps_cleared : 0.0)",
              Ty::num()};
    if (U0 == "THE FLUSH PASS COUNT" || U0 == "THE REACTIVE FLUSH PASS COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_flush_passes : 0.0)",
              Ty::num()};
    if (U0 == "THE DEFERRED FLUSH COUNT" || U0 == "THE NESTED FLUSH COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->deferred_flush_count : 0.0)",
              Ty::num()};
    if (U0 == "THE DIRTY DEDUP COUNT" || U0 == "THE DIRTY DEDUP HITS")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_flush_dedup_hits : 0.0)",
              Ty::num()};
    if (U0 == "THE DIRTY QUEUE SIZE" || U0 == "THE LAST DIRTY QUEUE SIZE")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_dirty_q_size : 0.0)",
              Ty::num()};
    if (U0 == "THE SCHEDULER STEP COUNT" || U0 == "THE REACTIVE STEP COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_flush_steps : 0.0)",
              Ty::num()};
    if (U0 == "THE SCHEDULER UI BEFORE BACKGROUND" || U0 == "THE UI BEFORE BACKGROUND")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->ui_before_bg : 0.0)", Ty::num()};
    if (U0 == "THE SCHEDULER FIRST STEP" || U0 == "THE FIRST EFFECT STEP")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_first_effect : 0.0)",
              Ty::num()};
    if (U0 == "THE SCHEDULER LAST STEP" || U0 == "THE LAST EFFECT STEP")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_last_effect : 0.0)",
              Ty::num()};
    if (U0 == "THE REGION PAINT COUNT" || U0 == "THE GRANULAR REGION PAINTS")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->region_paints : 0.0)", Ty::num()};
    if (U0 == "THE REGION LAYOUT COUNT" || U0 == "THE GRANULAR REGION LAYOUTS")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->region_layouts : 0.0)", Ty::num()};
    if (U0 == "THE SUBTREE INVALID COUNT" || U0 == "THE SUBTREE INVALIDATIONS")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->subtree_invals : 0.0)", Ty::num()};
    if (U0 == "THE ALIVE NODE COUNT" || U0 == "THE LIVE NODE COUNT")
      return {"(double)luke_rx_alive_count(" + rxGraphVar + ")", Ty::num()};
    if (U0 == "THE DEAD NODE COUNT")
      return {"(double)luke_rx_dead_count(" + rxGraphVar + ")", Ty::num()};
    if (U0 == "THE DISPOSED COUNT" || U0 == "THE DISPOSED NODE COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->disposed_count : 0.0)", Ty::num()};
    if (U0 == "THE LEAK EDGE COUNT" || U0 == "THE LEAK COUNT") {
      usesRx = true;
      return {"(double)((" + rxGraphVar + " ? luke_rx_audit_graph(" + rxGraphVar + ") : 0), "
              + rxGraphVar + " ? (double)" + rxGraphVar + "->last_leak_edges : 0.0)",
              Ty::num()};
    }
    if (U0 == "THE WEAK READ COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->weak_read_count : 0.0)", Ty::num()};
    if (U0 == "THE SCOPE GC COUNT" || U0 == "THE SCOPE FRAMES GC'D")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->scope_gc_count : 0.0)", Ty::num()};
    if (U0 == "THE SCOPE FRAME COUNT" || U0 == "THE OPEN SCOPE COUNT")
      return {"(double)luke_rx_scope_frame_count(" + rxGraphVar + ")", Ty::num()};
    if (U0 == "THE GRAPH CELL COUNT" || U0 == "THE REACTIVE CELL COUNT")
      return {"(double)luke_rx_count_kind(" + rxGraphVar + ", LUKE_RX_CELL)", Ty::num()};
    if (U0 == "THE GRAPH DERIVED COUNT" || U0 == "THE REACTIVE DERIVED COUNT")
      return {"(double)luke_rx_count_kind(" + rxGraphVar + ", LUKE_RX_DERIVED)", Ty::num()};
    if (U0 == "THE GRAPH EFFECT COUNT" || U0 == "THE REACTIVE EFFECT COUNT")
      return {"(double)luke_rx_count_kind(" + rxGraphVar + ", LUKE_RX_EFFECT)", Ty::num()};
    if (U0 == "THE GRAPH EDGE COUNT" || U0 == "THE REACTIVE EDGE COUNT")
      return {"(double)luke_rx_edge_count(" + rxGraphVar + ")", Ty::num()};
    if (U0 == "THE LAST WRITE ID" || U0 == "THE LAST WRITE NODE")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_write_id : 0.0)", Ty::num()};
    if (U0 == "THE GRAPH DUMP COUNT" || U0 == "THE REACTIVE DUMP COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->graph_dump_count : 0.0)", Ty::num()};
    if (U0 == "THE WHY TRACE COUNT" || U0 == "THE REACTIVE TRACE COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->why_trace_count : 0.0)", Ty::num()};
    if (U0 == "THE NEED PAINT FLAG" || U0 == "THE NEED PAINT COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->need_paint : 0.0)", Ty::num()};
    if (U0 == "THE NEED LAYOUT FLAG" || U0 == "THE NEED LAYOUT COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->need_layout : 0.0)", Ty::num()};
    if (U0 == "THE REACTIVE ERROR COUNT" || U0 == "THE ERROR COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->error_count : 0.0)", Ty::num()};
    if (U0 == "THE ERROR ISOLATION COUNT" || U0 == "THE ISOLATED ERROR COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->error_isolation_count : 0.0)",
              Ty::num()};
    if (U0 == "THE REACTIVE RETRY COUNT" || U0 == "THE RETRY COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->retry_count : 0.0)", Ty::num()};
    if (U0 == "THE LAST ERROR NODE" || U0 == "THE ERROR NODE")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_error_node : 0.0)", Ty::num()};
    if (U0 == "THE ASYNC FAILURE COUNT" || U0 == "THE FETCH FAILURE COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->async_failure_count : 0.0)",
              Ty::num()};
    if (U0 == "THE BOUNDARY TRIP COUNT" || U0 == "THE ERROR BOUNDARY TRIP COUNT")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->boundary_trip_count : 0.0)",
              Ty::num()};
    if (U0 == "THE LAST BOUNDARY TRIPPED" || U0 == "THE BOUNDARY TRIPPED ID")
      return {"(" + rxGraphVar + " ? (double)" + rxGraphVar + "->last_boundary_tripped : 0.0)",
              Ty::num()};
  }

  if (startsWithCI(e, "THE TEXT WIDTH OF ") || startsWithCI(e, "THE MEASURED WIDTH OF ")) {
    size_t prefix = startsWithCI(e, "THE TEXT WIDTH OF ") ? 18 : 22;
    auto t = coerceText(expr(trim(e.substr(prefix)), line));
    return {"argus_measure_text(" + t.code + ")", Ty::num()};
  }

  if (startsWithCI(e, "THE BOUNDARY TRIPPED FOR ") ||
      startsWithCI(e, "THE ERROR BOUNDARY TRIPPED FOR ")) {
    usesRx = true;
    size_t prefix = startsWithCI(e, "THE BOUNDARY TRIPPED FOR ") ? 25 : 31;
    auto name = stripThe(trim(e.substr(prefix)));
    return {"(double)luke_rx_boundary_tripped(" + rxGraphVar + ", \"" + esc(name) + "\")",
            Ty::num()};
  }

  if (startsWithCI(e, "THE TIMELINE STEP ID AT ") || startsWithCI(e, "THE TIMELINE STEP WAVE AT ")) {
    usesRx = true;
    bool wave = startsWithCI(e, "THE TIMELINE STEP WAVE AT ");
    size_t prefix = wave ? 26 : 24;
    auto idxE = expr(trim(e.substr(prefix)), line);
    expectTy(line, idxE.ty, Ty::num(), wave ? "THE TIMELINE STEP WAVE AT" : "THE TIMELINE STEP ID AT");
    if (wave)
      return {"(double)luke_rx_timeline_step_wave(" + rxGraphVar + ", (size_t)(" + idxE.code + "))",
              Ty::num()};
    return {"(double)luke_rx_timeline_step_id(" + rxGraphVar + ", (size_t)(" + idxE.code + "))",
            Ty::num()};
  }

  if (startsWithCI(e, "THE NODE ID OF ")) {
    usesRx = true;
    auto name = resolveRxCellName(rxCells, rxEntityStack, stripThe(trim(e.substr(15))));
    if (!rxCells.count(name)) {
      fail(line, "THE NODE ID OF needs a reactive cell — not '" + trim(e.substr(15)) + "'");
      return {"0", Ty::num()};
    }
    return {"(double)_luke_rx_id_" + cIdent(name), Ty::num()};
  }
  if (startsWithCI(e, "THE DEP COUNT OF ")) {
    usesRx = true;
    auto name = resolveRxCellName(rxCells, rxEntityStack, stripThe(trim(e.substr(17))));
    if (!rxCells.count(name)) {
      fail(line, "THE DEP COUNT OF needs a reactive cell — not '" + trim(e.substr(17)) + "'");
      return {"0", Ty::num()};
    }
    return {"(double)luke_rx_dep_count(" + rxGraphVar + ", _luke_rx_id_" + cIdent(name) + ")",
            Ty::num()};
  }
  if (startsWithCI(e, "THE SUB COUNT OF ")) {
    usesRx = true;
    auto name = resolveRxCellName(rxCells, rxEntityStack, stripThe(trim(e.substr(17))));
    if (!rxCells.count(name)) {
      fail(line, "THE SUB COUNT OF needs a reactive cell — not '" + trim(e.substr(17)) + "'");
      return {"0", Ty::num()};
    }
    return {"(double)luke_rx_sub_count(" + rxGraphVar + ", _luke_rx_id_" + cIdent(name) + ")",
            Ty::num()};
  }
  if (startsWithCI(e, "THE WHY ROOT OF ")) {
    usesRx = true;
    auto name = resolveRxCellName(rxCells, rxEntityStack, stripThe(trim(e.substr(16))));
    if (!rxCells.count(name)) {
      fail(line, "THE WHY ROOT OF needs a reactive cell — not '" + trim(e.substr(16)) + "'");
      return {"0", Ty::num()};
    }
    return {"(double)luke_rx_why_root(" + rxGraphVar + ", _luke_rx_id_" + cIdent(name) + ")",
            Ty::num()};
  }
  if (startsWithCI(e, "THE WHY DEPTH OF ")) {
    usesRx = true;
    auto name = resolveRxCellName(rxCells, rxEntityStack, stripThe(trim(e.substr(17))));
    if (!rxCells.count(name)) {
      fail(line, "THE WHY DEPTH OF needs a reactive cell — not '" + trim(e.substr(17)) + "'");
      return {"0", Ty::num()};
    }
    return {"(double)luke_rx_why_depth(" + rxGraphVar + ", _luke_rx_id_" + cIdent(name) + ")",
            Ty::num()};
  }

  if (startsWithCI(e, "THE WEAK VALUE OF ")) {
    usesRx = true;
    auto inner = trim(e.substr(18));
    auto pe = primary(inner, line);
    std::string w = pe.code;
    for (size_t pos = 0; (pos = w.find("luke_rx_read_num(", pos)) != std::string::npos;) {
      w.replace(pos, 17, "luke_rx_read_num_weak(");
      pos += 22;
    }
    for (size_t pos = 0; (pos = w.find("luke_rx_read_int(", pos)) != std::string::npos;) {
      w.replace(pos, 17, "luke_rx_read_int_weak(");
      pos += 22;
    }
    for (size_t pos = 0; (pos = w.find("luke_rx_read_text(", pos)) != std::string::npos;) {
      w.replace(pos, 18, "luke_rx_read_text_weak(");
      pos += 23;
    }
    pe.code = w;
    return pe;
  }

  if (startsWithCI(e, "THE VALUE OF ")) {
    auto id = coerceText(expr(trim(e.substr(13)), line));
    return {"luke_js_get_value(arena, " + id.code + ")", Ty::text()};
  }
  if (startsWithCI(e, "THE BODY OF FETCH ")) {
    auto id = coerceText(expr(trim(e.substr(18)), line));
    return {"luke_js_fetch_body(arena, " + id.code + ")", Ty::text()};
  }
  if (startsWithCI(e, "THE STATUS OF FETCH ")) {
    auto id = coerceText(expr(trim(e.substr(20)), line));
    return {"luke_js_fetch_status(" + id.code + ")", Ty::num()};
  }
  if (startsWithCI(e, "FETCH ") && toUpper(e).find(" IS READY") != std::string::npos) {
    auto U = toUpper(e);
    auto ready = U.find(" IS READY");
    auto id = coerceText(expr(trim(e.substr(6, ready - 6)), line));
    return {"luke_js_fetch_ready(" + id.code + ")", Ty::flag()};
  }

  if (startsWithCI(e, "ASK ")) {
    auto rest = trim(e.substr(4));
    auto U = toUpper(rest);
    auto toPos = U.find(" TO ");
    if (toPos != std::string::npos) {
      auto obj = trim(rest.substr(0, toPos));
      auto after = trim(rest.substr(toPos + 4));
      auto aU = toUpper(after);
      auto w = aU.find(" WITH ");
      std::string method;
      std::vector<std::string> args;
      if (w == std::string::npos) method = after;
      else {
        method = trim(after.substr(0, w));
        args = splitArgs(trim(after.substr(w + 6)));
      }
      auto recv = expr(obj, line);
      if (recv.ty.k != K::Ptr) {
        fail(line, "ASK TO needs a blueprint instance — got " + tyName(recv.ty));
        return {"0", Ty::num()};
      }
      // Resolve method on klass or ancestors.
      std::string owner = recv.ty.klass;
      Method *meth = nullptr;
      for (std::string c = owner; !c.empty(); c = bps[c].parent) {
        for (auto &m : bps[c].methods) {
          if (!m.ctor && m.name == method) {
            owner = c;
            meth = &m;
            break;
          }
        }
        if (meth) break;
      }
      if (!meth) {
        fail(line, "Unknown method '" + method + "' on " + recv.ty.klass +
                       " — add METHOD " + method + " or CALL PARENT");
        return {"0", Ty::num()};
      }
      auto checked = checkCallArgs(line, method, meth->params, args);
      if (bad) return {"0", Ty::num()};
      std::ostringstream call;
      if (owner == recv.ty.klass) {
        call << cIdent(owner) << "_" << cIdent(method) << "(arena, " << recv.code;
      } else {
        call << cIdent(owner) << "_" << cIdent(method) << "(arena, (" << cIdent(owner) << "*)"
             << recv.code;
      }
      for (auto &a : checked) call << ", " << a.code;
      call << ")";
      return {call.str(), Ty::vod()};
    }
    auto w = U.find(" WITH ");
    std::string name;
    std::vector<std::string> args;
    if (w == std::string::npos) name = rest;
    else {
      name = trim(rest.substr(0, w));
      args = splitArgs(trim(rest.substr(w + 6)));
    }
    /* Backend concurrency: ASK httpServe WITH server, handlerFn, maxConn */
    if (name == "httpServe") {
      if (args.size() != 3) {
        fail(line, "httpServe needs: ASK httpServe WITH server, handler, maxConn");
        return {"0", Ty::flag()};
      }
      auto serverE = expr(args[0], line);
      expectTy(line, serverE.ty, Ty::ptr("__HttpServer"), "httpServe server");
      auto handlerName = trim(args[1]);
      if (!fns.count(handlerName)) {
        fail(line, "httpServe handler '" + handlerName +
                       "' — define THIS IS FUNCTION " + handlerName + " WITH req AS REQUEST");
        return {"0", Ty::flag()};
      }
      auto &hfn = fns[handlerName];
      if (hfn.params.size() != 1 || hfn.params[0].ty.k != K::Ptr ||
          hfn.params[0].ty.klass != "__HttpReq") {
        fail(line, "httpServe handler must take one REQUEST argument");
        return {"0", Ty::flag()};
      }
      auto maxE = coerceTo(line, expr(args[2], line), Ty::num(), "httpServe maxConn");
      needsPthread = true;
      bool seen = false;
      for (auto &h : httpServeHandlers)
        if (h == handlerName) seen = true;
      if (!seen) httpServeHandlers.push_back(handlerName);
      return {"luke_http_serve(" + serverE.code + ", luke_http_wrap_" + cIdent(handlerName) + ", " +
                  maxE.code + ")",
              Ty::flag()};
    }
    if (!fns.count(name)) {
      fail(line, "Unknown function '" + name + "' — define it with THIS IS FUNCTION, or IMPORT it");
      return {"0", Ty::num()};
    }
    auto checked = checkCallArgs(line, name, fns[name].params, args);
    if (bad) return {"0", Ty::num()};
    std::ostringstream call;
    if (fns[name].foreign) {
      call << cIdent(name) << "(";
      for (size_t i = 0; i < checked.size(); ++i) {
        if (i) call << ", ";
        call << checked[i].code;
      }
      call << ")";
    } else {
      call << cIdent(name) << "(arena";
      for (auto &a : checked) call << ", " << a.code;
      call << ")";
    }
    return {call.str(), fns[name].ret};
  }

  if (startsWithCI(e, "NEW ")) {
    auto rest = trim(e.substr(4));
    auto U = toUpper(rest);
    auto w = U.find(" WITH ");
    std::string cls;
    std::vector<std::string> args;
    if (w == std::string::npos) cls = rest;
    else {
      cls = trim(rest.substr(0, w));
      args = splitArgs(trim(rest.substr(w + 6)));
    }
    if (!bps.count(cls)) {
      fail(line, "Unknown blueprint '" + cls + "' — declare BLUEPRINT " + cls + " or IMPORT it");
      return {"0", Ty::num()};
    }
    std::vector<Param> ctorP;
    for (auto &m : bps[cls].methods)
      if (m.ctor || m.name == "born") ctorP = m.params;
    if (ctorP.empty()) {
      for (std::string c = bps[cls].parent; !c.empty(); c = bps[c].parent) {
        for (auto &m : bps[c].methods) {
          if (m.ctor || m.name == "born") {
            ctorP = m.params;
            break;
          }
        }
        if (!ctorP.empty()) break;
      }
    }
    auto checked = checkCallArgs(line, "NEW " + cls, ctorP, args);
    if (bad) return {"0", Ty::num()};
    std::ostringstream call;
    call << cIdent(cls) << "_new(arena";
    for (auto &a : checked) call << ", " << a.code;
    call << ")";
    return {call.str(), Ty::ptr(cls)};
  }

  auto U = toUpper(e);

  /* Collections / problems — conversational expressions */
  if (U == "THE PROBLEM") return {"luke_the_problem()", Ty::text()};
  if (startsWithCI(e, "ITEM ")) {
    auto rest = trim(e.substr(5));
    auto rU = toUpper(rest);
    auto of = rU.find(" OF ");
    if (of != std::string::npos) {
      auto idx = expr(trim(rest.substr(0, of)), line);
      auto listE = expr(trim(rest.substr(of + 4)), line);
      expectTy(line, idx.ty, Ty::num(), "ITEM … OF");
      expectTy(line, listE.ty, Ty::list(), "ITEM … OF");
      return {"luke_list_get(" + listE.code + ", " + idx.code + ")", Ty::text()};
    }
  }
  if (startsWithCI(e, "HOW MANY IN ")) {
    auto col = expr(trim(e.substr(12)), line);
    if (col.ty.k == K::List) return {"luke_list_len(" + col.code + ")", Ty::num()};
    if (col.ty.k == K::Map) return {"luke_map_len(" + col.code + ")", Ty::num()};
    fail(line, "HOW MANY IN needs a LIST or MAP — got " + tyName(col.ty));
    return {"0", Ty::num()};
  }
  if (startsWithCI(e, "GET ")) {
    auto rest = trim(e.substr(4));
    auto rU = toUpper(rest);
    auto fr = rU.find(" FROM ");
    if (fr != std::string::npos) {
      auto key = coerceText(expr(trim(rest.substr(0, fr)), line));
      auto mapE = expr(trim(rest.substr(fr + 6)), line);
      expectTy(line, mapE.ty, Ty::map(), "GET … FROM");
      return {"luke_map_get(" + mapE.code + ", " + key.code + ")", Ty::text()};
    }
  }
  if (startsWithCI(e, "HAS KEY ")) {
    auto rest = trim(e.substr(8));
    auto rU = toUpper(rest);
    auto in = rU.find(" IN ");
    if (in != std::string::npos) {
      auto key = coerceText(expr(trim(rest.substr(0, in)), line));
      auto mapE = expr(trim(rest.substr(in + 4)), line);
      expectTy(line, mapE.ty, Ty::map(), "HAS KEY … IN");
      return {"luke_map_has(" + mapE.code + ", " + key.code + ")", Ty::flag()};
    }
  }

  auto cmp = [&](const std::string &needle, const char *op) -> Expr * {
    static Expr r;
    auto pos = findOutsideQuotes(e, U, needle);
    if (pos == std::string::npos) return nullptr;
    auto L = expr(trim(e.substr(0, pos)), line);
    auto R = expr(trim(e.substr(pos + needle.size())), line);
    if (!typesEqual(L.ty, R.ty) && !(isNumeric(L.ty) && isNumeric(R.ty))) {
      // Allow numeric comparisons; otherwise require matching types.
      if (L.ty.k != R.ty.k) {
        fail(line, "Cannot compare " + tyName(L.ty) + " with " + tyName(R.ty));
        r = {"0", Ty::flag()};
        return &r;
      }
    }
    if (L.ty.k == K::Text) {
      // Text equality via length+memcmp
      if (std::string(op) == "==") {
        r = {"(luke_text_eq((" + L.code + "),(" + R.code + ")))", Ty::flag()};
        return &r;
      }
      fail(line, "TEXT only supports EQUALS comparisons in Build for now");
      r = {"0", Ty::flag()};
      return &r;
    }
    if (!isNumeric(L.ty) && L.ty.k != K::Flag) {
      fail(line, "Can only compare NUMBER, INTEGER, or FLAG values here (got " + tyName(L.ty) + ")");
      r = {"0", Ty::flag()};
      return &r;
    }
    if (isNumeric(L.ty) && isNumeric(R.ty) && L.ty.k != R.ty.k) {
      Expr Lc = L.ty.k == K::Int ? Expr{"((double)(" + L.code + "))", Ty::num()} : L;
      Expr Rc = R.ty.k == K::Int ? Expr{"((double)(" + R.code + "))", Ty::num()} : R;
      r = {Lc.code + op + Rc.code, Ty::flag()};
      return &r;
    }
    r = {L.code + op + R.code, Ty::flag()};
    return &r;
  };
  if (auto *r = cmp(" EQUALS ", "==")) return *r;
  if (auto *r = cmp(" IS EQUAL TO ", "==")) return *r;
  {
    auto pos = findOutsideQuotes(e, U, " IS DIVISIBLE BY ");
    if (pos != std::string::npos) {
      auto L = expr(trim(e.substr(0, pos)), line);
      auto R = expr(trim(e.substr(pos + 17)), line);
      if (!isNumeric(L.ty) || !isNumeric(R.ty)) {
        fail(line, "IS DIVISIBLE BY wants NUMBER or INTEGER");
        return {"0", Ty::flag()};
      }
      if (L.ty.k == K::Int && R.ty.k == K::Int)
        return {"luke_i64_divisible(" + L.code + ", " + R.code + ")", Ty::flag()};
      Expr Lc = L.ty.k == K::Int ? Expr{"((double)(" + L.code + "))", Ty::num()} : L;
      Expr Rc = R.ty.k == K::Int ? Expr{"((double)(" + R.code + "))", Ty::num()} : R;
      return {"luke_divisible(" + Lc.code + ", " + Rc.code + ")", Ty::flag()};
    }
  }
  if (auto *r = cmp(" IS LESS THAN ", "<")) return *r;
  if (auto *r = cmp(" IS GREATER THAN ", ">")) return *r;
  if (auto *r = cmp(" IS LESS THAN OR EQUAL TO ", "<=")) return *r;
  if (auto *r = cmp(" IS GREATER THAN OR EQUAL TO ", ">=")) return *r;

  auto arith = [&](const std::string &mid, const std::string &pref, char op) -> Expr * {
    static Expr r;
    auto finish = [&](Expr L, Expr R) {
      if (!isNumeric(L.ty) || !isNumeric(R.ty)) {
        fail(line, "Arithmetic wants NUMBER or INTEGER");
        r = {"0", Ty::num()};
        return &r;
      }
      if (op == '/') {
        Expr Lc = L.ty.k == K::Int ? Expr{"((double)(" + L.code + "))", Ty::num()} : L;
        Expr Rc = R.ty.k == K::Int ? Expr{"((double)(" + R.code + "))", Ty::num()} : R;
        r = {"(" + Lc.code + "/" + Rc.code + ")", Ty::num()};
        return &r;
      }
      if (L.ty.k == K::Int && R.ty.k == K::Int) {
        const char *fn = op == '+'   ? "luke_i64_add"
                         : op == '-' ? "luke_i64_sub"
                         : op == '*' ? "luke_i64_mul"
                                     : nullptr;
        if (!fn) {
          fail(line, "INTEGER arithmetic only supports ADD, SUBTRACT, MULTIPLY");
          r = {"0LL", Ty::integer()};
          return &r;
        }
        r = {std::string(fn) + "(" + L.code + ", " + R.code + ")", Ty::integer()};
        return &r;
      }
      Expr Lc = L.ty.k == K::Int ? Expr{"((double)(" + L.code + "))", Ty::num()} : L;
      Expr Rc = R.ty.k == K::Int ? Expr{"((double)(" + R.code + "))", Ty::num()} : R;
      r = {"(" + Lc.code + op + Rc.code + ")", Ty::num()};
      return &r;
    };
    auto pos = findOutsideQuotes(e, U, mid);
    if (pos != std::string::npos) {
      return finish(expr(trim(e.substr(0, pos)), line), expr(trim(e.substr(pos + mid.size())), line));
    }
    if (startsWithCI(e, pref)) {
      auto rest = trim(e.substr(pref.size()));
      auto ap = findOutsideQuotes(rest, toUpper(rest), " AND ");
      if (ap != std::string::npos) {
        return finish(expr(trim(rest.substr(0, ap)), line), expr(trim(rest.substr(ap + 5)), line));
      }
    }
    return nullptr;
  };
  {
    size_t pos = findOutsideQuotes(e, U, " MULTIPLIED BY ");
    size_t mid = 15;
    if (pos == std::string::npos) {
      pos = findOutsideQuotes(e, U, " MULTIPLY BY ");
      mid = 13;
    }
    if (pos != std::string::npos) {
      auto L = expr(trim(e.substr(0, pos)), line);
      auto R = expr(trim(e.substr(pos + mid)), line);
      if (!isNumeric(L.ty) || !isNumeric(R.ty)) {
        fail(line, "Arithmetic wants NUMBER or INTEGER");
        return {"0", Ty::num()};
      }
      if (L.ty.k == K::Int && R.ty.k == K::Int)
        return {"luke_i64_mul(" + L.code + ", " + R.code + ")", Ty::integer()};
      Expr Lc = L.ty.k == K::Int ? Expr{"((double)(" + L.code + "))", Ty::num()} : L;
      Expr Rc = R.ty.k == K::Int ? Expr{"((double)(" + R.code + "))", Ty::num()} : R;
      return {"(" + Lc.code + "*" + Rc.code + ")", Ty::num()};
    }
  }
  if (auto *r = arith(" ADD ", "ADD ", '+')) return *r;
  if (auto *r = arith(" SUBTRACT ", "SUBTRACT ", '-')) return *r;
  if (auto *r = arith(" MULTIPLY ", "MULTIPLY ", '*')) return *r;
  if (auto *r = arith(" DIVIDE ", "DIVIDE ", '/')) return *r;
  {
    size_t pos = findOutsideQuotes(e, U, " DIVIDED BY ");
    if (pos != std::string::npos) {
      auto L = expr(trim(e.substr(0, pos)), line);
      auto R = expr(trim(e.substr(pos + 12)), line);
      if (!isNumeric(L.ty) || !isNumeric(R.ty)) {
        fail(line, "Arithmetic wants NUMBER or INTEGER");
        return {"0", Ty::num()};
      }
      Expr Lc = L.ty.k == K::Int ? Expr{"((double)(" + L.code + "))", Ty::num()} : L;
      Expr Rc = R.ty.k == K::Int ? Expr{"((double)(" + R.code + "))", Ty::num()} : R;
      return {"(" + Lc.code + "/" + Rc.code + ")", Ty::num()};
    }
  }

  {
    auto pos = findOutsideQuotes(e, U, " AND ");
    if (pos != std::string::npos) {
      auto L = coerceText(expr(trim(e.substr(0, pos)), line));
      auto R = coerceText(expr(trim(e.substr(pos + 5)), line));
      return {"luke_text_concat(arena,(" + L.code + "),(" + R.code + "))", Ty::text()};
    }
  }
  if (startsWithCI(e, "NOT ")) {
    auto x = expr(trim(e.substr(4)), line);
    return {"(!(" + x.code + "))", Ty::flag()};
  }
  if (startsWithCI(e, "IF ")) {
    auto rest = trim(e.substr(3));
    auto rU = toUpper(rest);
    auto thenPos = rU.find(" THEN ");
    auto otherwisePos = rU.find(" OTHERWISE ");
    if (thenPos != std::string::npos && otherwisePos != std::string::npos &&
        otherwisePos > thenPos + 6) {
      auto cond = expr(trim(rest.substr(0, thenPos)), line);
      auto thenE = expr(trim(rest.substr(thenPos + 6, otherwisePos - (thenPos + 6))), line);
      auto elseE = expr(trim(rest.substr(otherwisePos + 11)), line);
      if (cond.ty.k != K::Flag && cond.ty.k != K::Num && cond.ty.k != K::Int)
        fail(line, "IF … THEN … OTHERWISE needs a FLAG, NUMBER, or INTEGER condition");
      expectTy(line, thenE.ty, Ty::num(), "IF … THEN … OTHERWISE");
      expectTy(line, elseE.ty, Ty::num(), "IF … THEN … OTHERWISE");
      Expr thenC = coerceTo(line, thenE, Ty::num(), "IF … THEN … OTHERWISE");
      Expr elseC = coerceTo(line, elseE, Ty::num(), "IF … THEN … OTHERWISE");
      return {"((" + cond.code + ") ? (" + thenC.code + ") : (" + elseC.code + "))", Ty::num()};
    }
  }
  return primary(e, line);
}

void speak(const Expr &e, std::ostringstream &o) {
  if (e.ty.k == K::Text) o << "  luke_speak_text(" << e.code << ");\n";
  else if (e.ty.k == K::Flag) o << "  luke_speak_flag(" << e.code << ");\n";
  else if (e.ty.k == K::Int) o << "  luke_speak_integer(" << e.code << ");\n";
  else if (e.ty.k == K::Num) o << "  luke_speak_number(" << e.code << ");\n";
  else if (e.ty.k == K::Json)
    o << "  luke_speak_text(luke_json_stringify(arena, " << e.code << "));\n";
  else if (e.ty.k == K::List)
    o << "  luke_speak_number(luke_list_len(" << e.code << "));\n";
  else if (e.ty.k == K::Map)
    o << "  luke_speak_number(luke_map_len(" << e.code << "));\n";
  else if (e.ty.k == K::Void) o << "  " << e.code << ";\n";
  else o << "  luke_speak_text(luke_text(\"<obj>\"));\n";
}

void emitLineDir(const std::string &file, size_t line, std::ostringstream &o) {
  if (line == 0) return;
  std::string f = file.empty() ? "luke" : file;
  o << "#line " << line << " \"" << escapeLukePath(f) << "\"\n";
}

/* After a Luke statement, drop sticky #line so inlined luke_rt.h helpers are not
 * attributed to the .luke file (gdb step/next stay at statement granularity). */
void emitLineReset(std::ostringstream &o) { o << "#line 1 \"luke-generated.c\"\n"; }

/* Wrap cell/derived/effect creation so debugger/DAP can name reactive nodes. */
void emitRxNamedAssign(std::ostringstream &o, const std::string &idExpr,
                       const std::string &createExpr, const std::string &displayName) {
  o << "  " << idExpr << " = luke_rx_named(_luke_rx, " << createExpr << ", \"" << esc(displayName)
    << "\");\n";
}

struct LineScope {
  std::ostringstream &o;
  bool armed = true;
  explicit LineScope(std::ostringstream &out) : o(out) {}
  ~LineScope() {
    if (armed) emitLineReset(o);
  }
  LineScope(const LineScope &) = delete;
  LineScope &operator=(const LineScope &) = delete;
};

void pushTop(BC &bc, size_t line, const std::string &text) {
  bc.top.push_back({line, bc.curFile, text});
}

void stmt(BC &bc, const std::string &text, size_t line, std::ostringstream &o,
          const std::string &file = {}) {
  std::string srcFile = file;
  if (srcFile.empty()) srcFile = bc.curFile;
  if (srcFile.empty()) srcFile = bc.sourcePath;
  if (srcFile.empty()) srcFile = "luke";
  emitLineDir(srcFile, line, o);
  LineScope lineScope(o);
  {
    auto after = afterSpeakEach(text);
    if (!after.empty() && startsWithCI(after, "NUMBER FROM ")) {
      auto rest = trim(after.substr(12));
      auto U = toUpper(rest);
      if (findOutsideQuotes(rest, U, " WHERE ") != std::string::npos) {
        bc.fail(line, "WHERE needs a name — SPEAK EACH n FROM lo TO hi WHERE …");
        return;
      }
      auto toPos = findOutsideQuotes(rest, U, " TO ");
      if (toPos == std::string::npos) {
        bc.fail(line, "SPEAK EACH NUMBER FROM needs: SPEAK EACH NUMBER FROM lo TO hi");
        return;
      }
      auto lo = bc.expr(trim(rest.substr(0, toPos)), line);
      auto hi = bc.expr(trim(rest.substr(toPos + 4)), line);
      if (!bc.isNumeric(lo.ty) || !bc.isNumeric(hi.ty)) {
        bc.fail(line, "SPEAK EACH NUMBER FROM … TO … wants NUMBER or INTEGER");
        return;
      }
      Expr loN = bc.coerceTo(line, lo, Ty::num(), "SPEAK EACH NUMBER FROM");
      Expr hiN = bc.coerceTo(line, hi, Ty::num(), "SPEAK EACH NUMBER FROM");
      o << "  luke_speak_each_number(" << loN.code << ", " << hiN.code << ");\n";
      return;
    }
    if (!after.empty() && !startsWithCI(after, "OF ")) {
      auto U = toUpper(after);
      auto fromPos = findOutsideQuotes(after, U, " FROM ");
      if (fromPos != std::string::npos) {
        auto varName = trim(after.substr(0, fromPos));
        if (!isIdentName(varName) || toUpper(varName) == "NUMBER" || toUpper(varName) == "OF") {
          bc.fail(line, "SPEAK EACH name FROM needs a name — SPEAK EACH n FROM 1 TO 20 WHERE …");
          return;
        }
        auto rest = trim(after.substr(fromPos + 6));
        auto RU = toUpper(rest);
        auto toPos = findOutsideQuotes(rest, RU, " TO ");
        if (toPos == std::string::npos) {
          bc.fail(line, "SPEAK EACH n FROM needs: SPEAK EACH n FROM lo TO hi [WHERE pred]");
          return;
        }
        auto hiRaw = trim(rest.substr(toPos + 4));
        auto HU = toUpper(hiRaw);
        auto wherePos = findOutsideQuotes(hiRaw, HU, " WHERE ");
        std::string hiSrc, whereSrc;
        if (wherePos == std::string::npos) {
          hiSrc = hiRaw;
        } else {
          hiSrc = trim(hiRaw.substr(0, wherePos));
          whereSrc = trim(hiRaw.substr(wherePos + 7));
        }
        auto lo = bc.expr(trim(rest.substr(0, toPos)), line);
        auto hi = bc.expr(hiSrc, line);
        if (!bc.isNumeric(lo.ty) || !bc.isNumeric(hi.ty)) {
          bc.fail(line, "SPEAK EACH n FROM … TO … wants NUMBER or INTEGER");
          return;
        }
        Expr loN = bc.coerceTo(line, lo, Ty::num(), "SPEAK EACH n FROM");
        Expr hiN = bc.coerceTo(line, hi, Ty::num(), "SPEAK EACH n FROM");
        bool had = bc.locals.count(varName) != 0;
        Ty oldTy = had ? bc.locals[varName] : Ty::vod();
        bc.locals[varName] = Ty::num();
        Expr pred{"1", Ty::flag()};
        if (!whereSrc.empty()) {
          pred = bc.expr(whereSrc, line);
          if (pred.ty.k != K::Flag && pred.ty.k != K::Num && pred.ty.k != K::Int)
            bc.fail(line, "WHERE needs a FLAG (or NUMBER/INTEGER) — got " + tyName(pred.ty));
        }
        if (had) bc.locals[varName] = oldTy;
        else bc.locals.erase(varName);
        if (bc.bad) return;
        int id = ++bc.forEachSeq;
        std::string vn = cIdent(varName);
        o << "  {\n";
        o << "    double _luke_each_lo" << id << " = " << loN.code << ";\n";
        o << "    double _luke_each_hi" << id << " = " << hiN.code << ";\n";
        o << "    for (double " << vn << " = _luke_each_lo" << id << "; " << vn << " <= _luke_each_hi"
          << id << " + 1e-9; " << vn << " += 1) {\n";
        o << "      if (" << pred.code << ") luke_speak_number(" << vn << ");\n";
        o << "    }\n";
        o << "  }\n";
        return;
      }
    }
  }
  if (startsWithCI(text, "SPEAK EACH OF ") || startsWithCI(text, "SAY EACH OF ") ||
      startsWithCI(text, "YELL EACH OF ") || startsWithCI(text, "SHOUT EACH OF ")) {
    auto rest = trim(text.substr(text.find(' ') + 1));
    rest = trim(rest.substr(rest.find(' ') + 1)); /* EACH */
    rest = trim(rest.substr(rest.find(' ') + 1)); /* OF */
    auto listE = bc.expr(rest, line);
    bc.expectTy(line, listE.ty, Ty::list(), "SPEAK EACH OF");
    o << "  luke_speak_each_of(" << listE.code << ");\n";
    return;
  }
  if (startsWithCI(text, "SPEAK ") || startsWithCI(text, "SAY ") || startsWithCI(text, "YELL ") ||
      startsWithCI(text, "SHOUT ")) {
    auto sp = text.find(' ');
    speak(bc.expr(trim(text.substr(sp + 1)), line), o);
    return;
  }
  if (startsWithCI(text, "ASK ")) {
    o << "  " << bc.expr(text, line).code << ";\n";
    return;
  }
  if (startsWithCI(text, "CALL PARENT ") || startsWithCI(text, "CALL SUPER ")) {
    auto rest = startsWithCI(text, "CALL PARENT ") ? trim(text.substr(12)) : trim(text.substr(11));
    auto U = toUpper(rest);
    auto w = U.find(" WITH ");
    std::string method;
    std::vector<std::string> args;
    if (w == std::string::npos) method = rest;
    else {
      method = trim(rest.substr(0, w));
      args = splitArgs(trim(rest.substr(w + 6)));
    }
    auto parent = bc.bps[bc.curClass].parent;
    if (parent.empty()) {
      bc.fail(line, "CALL PARENT needs a parent — this blueprint doesn't FOLLOWS anyone");
      return;
    }
    Method *meth = nullptr;
    for (std::string c = parent; !c.empty(); c = bc.bps[c].parent) {
      for (auto &m : bc.bps[c].methods) {
        if (!m.ctor && m.name == method) {
          meth = &m;
          parent = c;
          break;
        }
      }
      if (meth) break;
    }
    if (!meth) {
      bc.fail(line, "Parent has no method '" + method + "'");
      return;
    }
    auto checked = bc.checkCallArgs(line, "CALL PARENT " + method, meth->params, args);
    if (bc.bad) return;
    o << "  " << cIdent(parent) << "_" << cIdent(method) << "(arena, (" << cIdent(parent)
      << "*)self";
    for (auto &a : checked) o << ", " << a.code;
    o << ");\n";
    return;
  }
  if (startsWithCI(text, "GIVE BACK ") || startsWithCI(text, "SEND BACK ") ||
      startsWithCI(text, "HAND BACK ")) {
    auto U = toUpper(text);
    auto b = U.find(" BACK ");
    auto e = bc.expr(trim(text.substr(b + 6)), line);
    if (bc.hasCurRet) e = bc.coerceTo(line, e, bc.curRet, "GIVE BACK");
    o << "  return " << e.code << ";\n";
    return;
  }
  if (toUpper(text) == "GIVE BACK") {
    if (bc.hasCurRet && bc.curRet.k != K::Void && bc.curRet.k != K::Num)
      bc.fail(line, "GIVE BACK with no value — this function should GIVE BACK " + tyName(bc.curRet));
    o << "  return 0;\n";
    return;
  }
  if (startsWithCI(text, "SECRET REMEMBER ") || startsWithCI(text, "REMEMBER ")) {
    bool forceSecret = startsWithCI(text, "SECRET REMEMBER ");
    auto rest = forceSecret ? trim(text.substr(16)) : trim(text.substr(9));
    auto U = toUpper(rest);
    auto asPos = U.find(" AS ");
    if (asPos == std::string::npos) {
      bc.fail(line, "REMEMBER needs: REMEMBER name AS value");
      return;
    }
    auto name = stripThe(trim(rest.substr(0, asPos)));
    auto valRaw = trim(rest.substr(asPos + 4));
    if (name.empty()) {
      bc.fail(line, "REMEMBER needs a name");
      return;
    }
    /* REMEMBER notes AS QUERY ON db AS "SELECT …" */
    if (startsWithCI(valRaw, "QUERY ON ")) {
      auto qrest = trim(valRaw.substr(9));
      auto qU = toUpper(qrest);
      auto qAs = qU.find(" AS ");
      if (qAs == std::string::npos) {
        bc.fail(line, "REMEMBER QUERY needs: REMEMBER name AS QUERY ON db AS \"sql\"");
        return;
      }
      auto dbName = trim(qrest.substr(0, qAs));
      auto sqlE = bc.coerceText(bc.expr(trim(qrest.substr(qAs + 4)), line));
      if (!bc.locals.count(dbName) || bc.locals[dbName].k != K::Ptr ||
          bc.locals[dbName].klass != "__Db") {
        bc.fail(line, "QUERY ON needs a DATABASE — MY NAME IS " + dbName + " AS DATABASE");
        return;
      }
      std::string key = rxScopedCellName(bc.rxEntityStack, name);
      bc.usesRx = true;
      bc.locals[name] = Ty::text();
      bc.rxCellTy[key] = Ty::text();
      if (!bc.rxCells.count(key)) bc.rxCellOrder.push_back(key);
      bc.rxCells[key] = true;
      if (forceSecret) bc.rxSecretCells[key] = true;
      {
        auto sql = unquoteText(trim(qrest.substr(qAs + 4)));
        BC::RxQueryDef qd;
        qd.name = key;
        qd.dbLocal = dbName;
        qd.sql = sql;
        qd.readSql = sql;
        qd.line = line;
        bc.rxQueryDefs.push_back(qd);
      }
      emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(key),
                        "luke_rx_cell_text(_luke_rx, luke_text(\"\"))", key);
      o << "  luke_rx_query_refresh(_luke_rx, _luke_rx_id_" << cIdent(key) << ", "
        << cIdent(dbName) << ", " << sqlE.code << ");\n";
      return;
    }
    /* Optional: REMEMBER x AS [SECRET] NUMBER SET TO 100 */
    auto vU = toUpper(valRaw);
    auto setPos = vU.find(" SET TO ");
    Ty forced = Ty::vod();
    bool isSecret = forceSecret;
    if (setPos != std::string::npos) {
      auto tyPart = bc.stripSecretTy(trim(valRaw.substr(0, setPos)), &isSecret);
      forced = bc.parseTy(tyPart);
      valRaw = trim(valRaw.substr(setPos + 8));
    } else {
      auto tyPart = bc.stripSecretTy(valRaw, &isSecret);
      Ty maybe = bc.parseTy(tyPart);
      if (maybe.k != K::Void && tyPart.find(' ') == std::string::npos &&
          !std::isdigit((unsigned char)tyPart[0]) && tyPart[0] != '-' && tyPart[0] != '"' &&
          toUpper(tyPart) != "TRUE" && toUpper(tyPart) != "FALSE") {
        /* REMEMBER x AS [SECRET] NUMBER|TEXT|LIST|MAP — typed empty cell/collection */
        forced = maybe;
        valRaw.clear();
      } else if (isSecret && maybe.k == K::Void && toUpper(tyPart) == "TEXT") {
        forced = Ty::text();
        valRaw.clear();
      } else {
        valRaw = tyPart;
      }
    }
    /* Reactive collections */
    std::string cellKey = rxScopedCellName(bc.rxEntityStack, name);
    if (forced.k == K::List || (valRaw.empty() && forced.k == K::List)) {
      bc.usesRx = true;
      bc.locals[name] = Ty::list();
      bc.rxCellTy[cellKey] = Ty::list();
      if (!bc.rxCells.count(cellKey)) bc.rxCellOrder.push_back(cellKey);
      bc.rxCells[cellKey] = true;
      emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(cellKey), "luke_rx_list(_luke_rx)", cellKey);
      return;
    }
    if (forced.k == K::Map) {
      bc.usesRx = true;
      bc.locals[name] = Ty::map();
      bc.rxCellTy[cellKey] = Ty::map();
      if (!bc.rxCells.count(cellKey)) bc.rxCellOrder.push_back(cellKey);
      bc.rxCells[cellKey] = true;
      emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(cellKey), "luke_rx_map(_luke_rx)", cellKey);
      return;
    }
    Expr e{"0.0", Ty::num()};
    if (valRaw.empty()) {
      if (forced.k == K::Text) e = {"luke_text(\"\")", Ty::text()};
      else if (forced.k == K::Flag) e = {"0", Ty::flag()};
      else if (forced.k == K::Int) e = {"0LL", Ty::integer()};
      else e = {"0.0", Ty::num()};
      if (forced.k != K::Void) e.ty = forced;
    } else {
      e = bc.expr(valRaw, line);
      if (forced.k != K::Void) {
        e = bc.coerceTo(line, e, forced, "REMEMBER " + name);
      }
    }
    if (e.ty.k != K::Num && e.ty.k != K::Int && e.ty.k != K::Text) {
      bc.fail(line, "REMEMBER supports NUMBER, INTEGER, TEXT, LIST, or MAP — got " + tyName(e.ty));
      return;
    }
    bc.usesRx = true;
    bc.locals[name] = e.ty;
    bc.rxCellTy[cellKey] = e.ty;
    if (!bc.rxCells.count(cellKey)) bc.rxCellOrder.push_back(cellKey);
    bc.rxCells[cellKey] = true;
    if (isSecret) bc.rxSecretCells[cellKey] = true;
    if (e.ty.k == K::Text)
      emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(cellKey),
                        "luke_rx_cell_text(_luke_rx, " + e.code + ")", cellKey);
    else if (e.ty.k == K::Int)
      emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(cellKey),
                        "luke_rx_cell_int(_luke_rx, " + e.code + ")", cellKey);
    else
      emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(cellKey),
                        "luke_rx_cell(_luke_rx, " + e.code + ")", cellKey);
    return;
  }
  if (startsWithCI(text, "CHANGE ")) {
    auto rest = trim(text.substr(7));
    auto U = toUpper(rest);
    auto to = U.find(" TO ");
    if (to == std::string::npos) {
      bc.fail(line, "CHANGE needs: CHANGE name TO value");
      return;
    }
    auto name = resolveRxCellName(bc.rxCells, bc.rxEntityStack, trim(rest.substr(0, to)));
    auto e = bc.expr(trim(rest.substr(to + 4)), line);
    if (!bc.rxCells.count(name)) {
      bc.fail(line, "CHANGE '" + name + "' — REMEMBER it first (reactive cell)");
      return;
    }
    if (bc.rxDerived.count(name)) {
      bc.fail(line, "Cannot CHANGE derived '" + name + "' — change its inputs instead");
      return;
    }
    Ty want = bc.rxCellTy.count(name) ? bc.rxCellTy[name] : Ty::num();
    if (want.k == K::List || want.k == K::Map) {
      bc.fail(line, "Cannot CHANGE a LIST/MAP — use ADD / SET ITEM / PUT");
      return;
    }
    e = bc.coerceTo(line, e, want, "CHANGE " + name);
    bc.usesRx = true;
    if (want.k == K::Text)
      o << "  luke_rx_write_text(_luke_rx, _luke_rx_id_" << cIdent(name) << ", " << e.code << ");\n";
    else if (want.k == K::Int)
      o << "  luke_rx_write_int(_luke_rx, _luke_rx_id_" << cIdent(name) << ", " << e.code << ");\n";
    else
      o << "  luke_rx_write_num(_luke_rx, _luke_rx_id_" << cIdent(name) << ", " << e.code << ");\n";
    return;
  }
  if (startsWithCI(text, "INCREASE ")) {
    auto rest = trim(text.substr(9));
    auto U = toUpper(rest);
    auto by = U.find(" BY ");
    if (by == std::string::npos) {
      bc.fail(line, "INCREASE needs: INCREASE name BY amount");
      return;
    }
    auto name = resolveRxCellName(bc.rxCells, bc.rxEntityStack, trim(rest.substr(0, by)));
    auto e = bc.expr(trim(rest.substr(by + 4)), line);
    if (!bc.rxCells.count(name) || bc.rxDerived.count(name)) {
      bc.fail(line, "INCREASE needs a REMEMBER'd NUMBER/INTEGER cell — not '" + name + "'");
      return;
    }
    Ty want = bc.rxCellTy.count(name) ? bc.rxCellTy[name] : Ty::num();
    if (!bc.isNumeric(want)) {
      bc.fail(line, "INCREASE needs a NUMBER or INTEGER cell — not '" + name + "'");
      return;
    }
    if (!bc.isNumeric(e.ty)) {
      bc.fail(line, "INCREASE BY wants NUMBER or INTEGER");
      return;
    }
    bc.usesRx = true;
    if (want.k == K::Int) {
      Expr amt = e.ty.k == K::Num ? Expr{"luke_number_to_integer(" + e.code + ")", Ty::integer()} : e;
      o << "  luke_rx_write_int(_luke_rx, _luke_rx_id_" << cIdent(name)
        << ", luke_i64_add(luke_rx_read_int(_luke_rx, _luke_rx_id_" << cIdent(name) << "), ("
        << amt.code << ")));\n";
    } else {
      Expr amt = e.ty.k == K::Int ? Expr{"((double)(" + e.code + "))", Ty::num()} : e;
      o << "  luke_rx_write_num(_luke_rx, _luke_rx_id_" << cIdent(name)
        << ", luke_rx_read_num(_luke_rx, _luke_rx_id_" << cIdent(name) << ") + (" << amt.code
        << "));\n";
    }
    return;
  }
  if (toUpper(text) == "BEGIN REACTIVE BATCH" || toUpper(text) == "BEGIN BATCH") {
    bc.usesRx = true;
    o << "  luke_rx_batch_begin(_luke_rx);\n";
    return;
  }
  if (toUpper(text) == "END REACTIVE BATCH" || toUpper(text) == "END BATCH") {
    bc.usesRx = true;
    o << "  luke_rx_batch_end(_luke_rx);\n";
    return;
  }
  if (toUpper(text) == "FLUSH REACTIVE" || toUpper(text) == "FLUSH THE REACTIVE GRAPH") {
    bc.usesRx = true;
    o << "  luke_rx_flush(_luke_rx);\n";
    return;
  }
  if (toUpper(text) == "AUDIT REACTIVE" || toUpper(text) == "AUDIT THE REACTIVE GRAPH") {
    bc.usesRx = true;
    o << "  luke_rx_audit_graph(_luke_rx);\n";
    return;
  }
  if (toUpper(text) == "DUMP REACTIVE GRAPH" || toUpper(text) == "DUMP THE REACTIVE GRAPH") {
    bc.usesRx = true;
    o << "  luke_rx_dump_graph(_luke_rx);\n";
    return;
  }
  if (startsWithCI(text, "TRACE WHY ") || startsWithCI(text, "WHY DID ")) {
    std::string nameRaw;
    if (startsWithCI(text, "TRACE WHY "))
      nameRaw = trim(text.substr(10));
    else {
      auto rest = trim(text.substr(8));
      auto U = toUpper(rest);
      auto ch = U.find(" CHANGE");
      nameRaw = ch != std::string::npos ? trim(rest.substr(0, ch)) : rest;
    }
    auto name = resolveRxCellName(bc.rxCells, bc.rxEntityStack, stripThe(nameRaw));
    if (!bc.rxCells.count(name)) {
      bc.fail(line, "TRACE WHY needs a reactive cell — not '" + nameRaw + "'");
      return;
    }
    bc.usesRx = true;
    o << "  luke_rx_trace_why(_luke_rx, _luke_rx_id_" << cIdent(name) << ");\n";
    return;
  }
  if (toUpper(text) == "RETRY REACTIVE ERROR" || toUpper(text) == "RETRY THE REACTIVE ERROR") {
    bc.usesRx = true;
    o << "  luke_rx_retry_error(_luke_rx);\n";
    return;
  }
  if (toUpper(text) == "CLEAR REACTIVE ERROR" || toUpper(text) == "CLEAR THE REACTIVE ERROR") {
    bc.usesRx = true;
    o << "  luke_rx_clear_reactive_error(_luke_rx);\n";
    return;
  }
  if (startsWithCI(text, "REPORT REACTIVE FAILURE FOR ") ||
      startsWithCI(text, "REPORT ASYNC FAILURE FOR ")) {
    size_t prefix =
        startsWithCI(text, "REPORT REACTIVE FAILURE FOR ") ? 28 : 26;
    auto rest = trim(text.substr(prefix));
    auto U = toUpper(rest);
    auto withPos = U.find(" WITH ");
    if (withPos == std::string::npos) {
      bc.fail(line, "REPORT REACTIVE FAILURE needs: … FOR cell WITH message");
      return;
    }
    auto cellRaw = trim(rest.substr(0, withPos));
    auto msgE = bc.coerceText(bc.expr(trim(rest.substr(withPos + 6)), line));
    auto cellName = resolveRxCellName(bc.rxCells, bc.rxEntityStack, stripThe(cellRaw));
    if (!bc.rxCells.count(cellName)) {
      bc.fail(line, "REPORT REACTIVE FAILURE needs a reactive cell — not '" + cellRaw + "'");
      return;
    }
    bc.usesRx = true;
    o << "  luke_rx_report_async_failure(_luke_rx, _luke_rx_id_" << cIdent(cellName) << ", "
      << msgE.code << ");\n";
    return;
  }
  if (toUpper(text) == "PAINT DIRTY" || toUpper(text) == "PAINT THE DIRTY NODES") {
    o << "  argus_paint(arena);\n";
    return;
  }
  if (startsWithCI(text, "REACTIVE WATCH REGISTER ")) {
    auto seq = std::atoi(trim(text.substr(24)).c_str());
    const BC::RxWhenDef *wd = nullptr;
    for (auto &w : bc.rxWhenDefs)
      if (w.seq == seq) {
        wd = &w;
        break;
      }
    if (!wd) {
      bc.fail(line, "Internal: unknown reactive watch #" + std::to_string(seq));
      return;
    }
    bc.usesRx = true;
    std::string whenName = "when_" + std::to_string(seq);
    if (wd->weak)
      emitRxNamedAssign(
          o, "_luke_rx_id_when_" + std::to_string(seq),
          std::string("luke_rx_effect_weak(_luke_rx, _luke_rx_when_") + std::to_string(seq) +
              ", NULL, " + (wd->background ? "LUKE_RX_PRIO_BACKGROUND" : "LUKE_RX_PRIO_UI") + ")",
          whenName);
    else
      emitRxNamedAssign(
          o, "_luke_rx_id_when_" + std::to_string(seq),
          std::string("luke_rx_effect_prio(_luke_rx, _luke_rx_when_") + std::to_string(seq) +
              ", NULL, " + (wd->background ? "LUKE_RX_PRIO_BACKGROUND" : "LUKE_RX_PRIO_UI") + ")",
          whenName);
    return;
  }
  /* BIND BACKGROUND "tag" TO expr — low-priority reactive effect (Scheduler 2.0). */
  if (startsWithCI(text, "BIND BACKGROUND ")) {
    auto rest = trim(text.substr(16));
    auto U = toUpper(rest);
    auto toPos = U.find(" TO ");
    if (toPos == std::string::npos) {
      bc.fail(line, "BIND BACKGROUND needs: BIND BACKGROUND \"element\" TO expression");
      return;
    }
    auto idE = bc.coerceText(bc.expr(trim(rest.substr(0, toPos)), line));
    auto exprRaw = trim(rest.substr(toPos + 4));
    if (exprRaw.empty()) {
      bc.fail(line, "BIND BACKGROUND needs an expression after TO");
      return;
    }
    bc.usesRx = true;
    int seq = ++bc.rxBindSeq;
    bc.rxBindDefs.push_back({idE.code, exprRaw, line, seq});
    emitRxNamedAssign(o, "_luke_rx_id_bind_" + std::to_string(seq),
                      std::string("luke_rx_effect_prio(_luke_rx, _luke_rx_bind_") +
                          std::to_string(seq) + ", NULL, LUKE_RX_PRIO_BACKGROUND)",
                      "bind_bg_" + std::to_string(seq));
    o << "  luke_rx_flush(_luke_rx);\n";
    return;
  }
  /* BIND OPACITY "panel" TO progress — reactive Argus opacity (layout frame). */
  if (startsWithCI(text, "BIND OPACITY ")) {
    auto rest = trim(text.substr(13));
    auto U = toUpper(rest);
    auto toPos = U.find(" TO ");
    if (toPos == std::string::npos) {
      bc.fail(line, "BIND OPACITY needs: BIND OPACITY \"element\" TO expression");
      return;
    }
    auto idE = bc.coerceText(bc.expr(trim(rest.substr(0, toPos)), line));
    auto exprRaw = trim(rest.substr(toPos + 4));
    if (exprRaw.empty()) {
      bc.fail(line, "BIND OPACITY needs an expression after TO");
      return;
    }
    bc.usesRx = true;
    bc.usesRxUi = true;
    int seq = ++bc.rxBindSeq;
    bc.rxOpacityBindDefs.push_back({idE.code, exprRaw, line, seq});
    emitRxNamedAssign(o, "_luke_rx_id_bind_" + std::to_string(seq),
                      std::string("luke_rx_effect(_luke_rx, _luke_rx_bind_opacity_") +
                          std::to_string(seq) + ", NULL)",
                      "bind_opacity_" + std::to_string(seq));
    o << "  luke_rx_flush(_luke_rx);\n";
    return;
  }
  /* BIND LIST players AS "row" — granular Argus row paints (prefix_index). */
  if (startsWithCI(text, "BIND LIST ")) {
    auto rest = trim(text.substr(10));
    auto U = toUpper(rest);
    auto asPos = U.find(" AS ");
    if (asPos == std::string::npos) {
      bc.fail(line, "BIND LIST needs: BIND LIST name AS \"prefix\"");
      return;
    }
    auto listName = stripThe(trim(rest.substr(0, asPos)));
    auto prefixRaw = trim(rest.substr(asPos + 4));
    auto prefixE = bc.coerceText(bc.expr(prefixRaw, line));
    (void)prefixE;
    if (!bc.rxCells.count(listName) ||
        (bc.rxCellTy.count(listName) && bc.rxCellTy[listName].k != K::List)) {
      bc.fail(line, "BIND LIST needs a REMEMBER'd LIST — not '" + listName + "'");
      return;
    }
    bc.usesRx = true;
    bc.usesRxUi = true;
    int seq = ++bc.rxBindSeq;
    bc.rxListBindDefs.push_back({listName, prefixRaw, line, seq});
    emitRxNamedAssign(o, "_luke_rx_id_bind_" + std::to_string(seq),
                      std::string("luke_rx_effect(_luke_rx, _luke_rx_bind_list_") +
                          std::to_string(seq) + ", NULL)",
                      "bind_list_" + listName);
    o << "  luke_rx_flush(_luke_rx);\n";
    return;
  }
  /* BIND "greeting" TO "Welcome, " AND username — reactive Argus text (no CLEAR). */
  if (startsWithCI(text, "BIND ")) {
    auto rest = trim(text.substr(5));
    auto U = toUpper(rest);
    auto toPos = U.find(" TO ");
    if (toPos == std::string::npos) {
      bc.fail(line, "BIND needs: BIND \"element\" TO expression");
      return;
    }
    auto idE = bc.coerceText(bc.expr(trim(rest.substr(0, toPos)), line));
    auto exprRaw = trim(rest.substr(toPos + 4));
    if (exprRaw.empty()) {
      bc.fail(line, "BIND needs an expression after TO");
      return;
    }
    bc.lintSecretBind(line, exprRaw);
    if (bc.bad) return;
    bc.usesRx = true;
    bc.usesRxUi = true;
    int seq = ++bc.rxBindSeq;
    std::string argusId;
    /* Best-effort extract literal id for metadata; runtime uses expr. */
    if (!idE.code.empty()) {
      /* keep seq-based symbol; store raw id expr in def */
    }
    (void)argusId;
    bc.rxBindDefs.push_back({idE.code, exprRaw, line, seq});
    emitRxNamedAssign(o, "_luke_rx_id_bind_" + std::to_string(seq),
                      std::string("luke_rx_effect(_luke_rx, _luke_rx_bind_") + std::to_string(seq) +
                          ", NULL)",
                      "bind_" + std::to_string(seq));
    o << "  luke_rx_flush(_luke_rx);\n";
    /* Auth audit: mark that CURRENT USER saw each SECRET cell in this bind. */
    for (auto &n : bc.secretTouchNames(exprRaw)) {
      auto key = resolveRxCellName(bc.rxCells, bc.rxEntityStack, n);
      bool isSecret = bc.rxSecretCells.count(key) || bc.rxSecretCells.count(n);
      std::string auditName = key;
      if (!isSecret) {
        auto dot = n.find('.');
        std::string field = dot == std::string::npos ? n : n.substr(dot + 1);
        for (auto &kv : bc.bps) {
          for (auto &f : kv.second.fields)
            if (f.name == field && f.secret) {
              isSecret = true;
              auditName = field;
              break;
            }
          if (isSecret) break;
        }
      }
      if (isSecret)
        o << "  luke_auth_mark_saw(arena, luke_text(\"" << esc(auditName) << "\"));\n";
    }
    return;
  }
  /* UPDATE "greeting" WITH expr — one-shot Argus text write + dirty paint. */
  if (startsWithCI(text, "UPDATE ")) {
    auto rest = trim(text.substr(7));
    auto U = toUpper(rest);
    auto withPos = U.find(" WITH ");
    if (withPos == std::string::npos) {
      bc.fail(line, "UPDATE needs: UPDATE \"element\" WITH expression");
      return;
    }
    auto idE = bc.coerceText(bc.expr(trim(rest.substr(0, withPos)), line));
    auto t = bc.coerceText(bc.expr(trim(rest.substr(withPos + 6)), line));
    bc.usesRx = true;
    bc.usesRxUi = true;
    o << "  luke_rx_ui_set_text(_luke_rx, " << idE.code << ", " << t.code << ");\n";
    o << "  if (_luke_rx) { _luke_rx->need_paint = 1; luke_rx_ui_after_flush(_luke_rx); }\n";
    return;
  }
  /* Phase 14 — error boundaries (component-scoped containment) */
  if (startsWithCI(text, "BEGIN ERROR BOUNDARY ") || startsWithCI(text, "ERROR BOUNDARY ")) {
    std::string rest = startsWithCI(text, "BEGIN ERROR BOUNDARY ") ? trim(text.substr(21))
                                                                   : trim(text.substr(15));
    stripDo(rest);
    auto name = stripThe(rest);
    if (name.empty()) {
      bc.fail(line, "ERROR BOUNDARY needs a name — BEGIN ERROR BOUNDARY Panel");
      return;
    }
    bool simple = true;
    for (char c : name)
      if (!(isalnum((unsigned char)c) || c == '_')) simple = false;
    if (!simple) {
      bc.fail(line, "ERROR BOUNDARY name must be a simple identifier");
      return;
    }
    bc.usesRx = true;
    bc.rxBoundaryStack.push_back(name);
    o << "  luke_rx_boundary_begin(_luke_rx, \"" << esc(name) << "\");\n";
    return;
  }
  if (startsWithCI(text, "END ERROR BOUNDARY") || toUpper(text) == "ENDERROR BOUNDARY") {
    std::string rest;
    if (startsWithCI(text, "END ERROR BOUNDARY"))
      rest = trim(text.substr(19));
    else
      rest = "";
    if (bc.rxBoundaryStack.empty()) {
      bc.fail(line, "END ERROR BOUNDARY without matching BEGIN ERROR BOUNDARY");
      return;
    }
    std::string open = bc.rxBoundaryStack.back();
    if (!rest.empty() && stripThe(rest) != open) {
      bc.fail(line, "END ERROR BOUNDARY '" + stripThe(rest) + "' but open boundary is '" + open +
                         "'");
      return;
    }
    bc.rxBoundaryStack.pop_back();
    bc.usesRx = true;
    o << "  luke_rx_boundary_end(_luke_rx, \"" << esc(open) << "\");\n";
    return;
  }
  if (startsWithCI(text, "RESET ERROR BOUNDARY ") || startsWithCI(text, "CLEAR ERROR BOUNDARY ")) {
    size_t prefix = startsWithCI(text, "RESET ERROR BOUNDARY ") ? 21 : 21;
    auto name = stripThe(trim(text.substr(prefix)));
    if (name.empty()) {
      bc.fail(line, "RESET ERROR BOUNDARY needs a name");
      return;
    }
    bc.usesRx = true;
    o << "  luke_rx_boundary_reset(_luke_rx, \"" << esc(name) << "\");\n";
    return;
  }
  /* Phase 3/7 — component / entity scopes */
  if (startsWithCI(text, "BEGIN ENTITY ") || startsWithCI(text, "ENTITY ")) {
    std::string rest = startsWithCI(text, "BEGIN ENTITY ") ? trim(text.substr(13))
                                                         : trim(text.substr(7));
    stripDo(rest);
    auto name = stripThe(rest);
    if (name.empty()) {
      bc.fail(line, "ENTITY needs a name — BEGIN ENTITY Player");
      return;
    }
    bool simple = true;
    for (char c : name)
      if (!(isalnum((unsigned char)c) || c == '_')) simple = false;
    if (!simple) {
      bc.fail(line, "ENTITY name must be a simple identifier");
      return;
    }
    bc.usesRx = true;
    bc.rxEntityStack.push_back(name);
    bc.rxComponentStack.push_back(name);
    o << "  luke_rx_scope_begin(_luke_rx, \"" << esc(name) << "\");\n";
    return;
  }
  if (startsWithCI(text, "BEGIN COMPONENT ") || startsWithCI(text, "COMPONENT ")) {
    std::string rest = startsWithCI(text, "BEGIN COMPONENT ") ? trim(text.substr(16))
                                                              : trim(text.substr(10));
    stripDo(rest);
    auto name = stripThe(rest);
    if (name.empty()) {
      bc.fail(line, "COMPONENT needs a name — BEGIN COMPONENT Counter");
      return;
    }
    bool simple = true;
    for (char c : name)
      if (!(isalnum((unsigned char)c) || c == '_')) simple = false;
    if (!simple) {
      bc.fail(line, "COMPONENT name must be a simple identifier");
      return;
    }
    bc.usesRx = true;
    bc.rxComponentStack.push_back(name);
    o << "  luke_rx_scope_begin(_luke_rx, \"" << esc(name) << "\");\n";
    return;
  }
  if (startsWithCI(text, "END COMPONENT") || startsWithCI(text, "END ENTITY") ||
      toUpper(text) == "ENDCOMPONENT" || toUpper(text) == "ENDENTITY") {
    std::string rest;
    if (startsWithCI(text, "END COMPONENT"))
      rest = trim(text.substr(13));
    else if (startsWithCI(text, "END ENTITY"))
      rest = trim(text.substr(10));
    else
      rest = "";
    if (bc.rxComponentStack.empty()) {
      bc.fail(line, "END COMPONENT without matching BEGIN COMPONENT / ENTITY");
      return;
    }
    std::string open = bc.rxComponentStack.back();
    if (!rest.empty() && stripThe(rest) != open) {
      bc.fail(line, "END COMPONENT '" + stripThe(rest) + "' but open scope is '" + open + "'");
      return;
    }
    if (!bc.rxEntityStack.empty() && bc.rxEntityStack.back() == open) bc.rxEntityStack.pop_back();
    bc.rxComponentStack.pop_back();
    bc.usesRx = true;
    o << "  luke_rx_scope_pause(_luke_rx);\n";
    /* Scope stays alive until DESTROY COMPONENT — handlers can still touch cells. */
    return;
  }
  if (startsWithCI(text, "DESTROY COMPONENT ") || startsWithCI(text, "DESTROY ENTITY ") ||
      startsWithCI(text, "UNMOUNT COMPONENT ") || startsWithCI(text, "UNMOUNT ENTITY ") ||
      (startsWithCI(text, "DESTROY ") && !startsWithCI(text, "DESTROY COMPONENT ") &&
       !startsWithCI(text, "DESTROY ENTITY "))) {
    std::string rest;
    if (startsWithCI(text, "DESTROY COMPONENT "))
      rest = trim(text.substr(18));
    else if (startsWithCI(text, "DESTROY ENTITY "))
      rest = trim(text.substr(15));
    else if (startsWithCI(text, "UNMOUNT COMPONENT "))
      rest = trim(text.substr(18));
    else if (startsWithCI(text, "UNMOUNT ENTITY "))
      rest = trim(text.substr(15));
    else
      rest = trim(text.substr(8));
    auto name = stripThe(rest);
    if (name.empty()) {
      bc.fail(line, "DESTROY COMPONENT needs a name");
      return;
    }
    bc.usesRx = true;
    o << "  luke_rx_scope_end(_luke_rx, \"" << esc(name) << "\");\n";
    return;
  }
  /* Derived: THE total IS price MULTIPLIED BY quantity */
  if (startsWithCI(text, "REFRESH QUERY ")) {
    auto qname = resolveRxCellName(bc.rxCells, bc.rxEntityStack, trim(text.substr(14)));
    const BC::RxQueryDef *qd = nullptr;
    for (auto &q : bc.rxQueryDefs)
      if (q.name == qname) {
        qd = &q;
        break;
      }
    if (!qd) {
      bc.fail(line, "REFRESH QUERY needs a REMEMBER'd QUERY cell — not '" + qname + "'");
      return;
    }
    bc.usesRx = true;
    o << "  luke_rx_query_refresh(_luke_rx, _luke_rx_id_" << cIdent(qname) << ", "
      << cIdent(qd->dbLocal) << ", luke_text(\"" << esc(qd->readSql.empty() ? qd->sql : qd->readSql)
      << "\"));\n";
    return;
  }
  if (startsWithCI(text, "START TIMELINE ") || startsWithCI(text, "RUN TIMELINE ")) {
    size_t prefix = startsWithCI(text, "START TIMELINE ") ? 15 : 13;
    auto rest = trim(text.substr(prefix));
    auto U = toUpper(rest);
    /* Parse: "id" FOR ms MILLISECONDS FROM a TO b INTO cell */
    auto forPos = findOutsideQuotes(rest, U, " FOR ");
    auto fromPos = findOutsideQuotes(rest, U, " FROM ");
    auto toKw = findOutsideQuotes(rest, U, " TO ");
    auto intoPos = findOutsideQuotes(rest, U, " INTO ");
    if (forPos == std::string::npos || fromPos == std::string::npos || toKw == std::string::npos ||
        intoPos == std::string::npos || intoPos < toKw) {
      bc.fail(line, "TIMELINE needs: START TIMELINE \"id\" FOR ms MILLISECONDS FROM a TO b INTO cell");
      return;
    }
    auto idRaw = trim(rest.substr(0, forPos));
    auto idLit = unquoteText(idRaw);
    auto forClause = trim(rest.substr(forPos + 5, fromPos - (forPos + 5)));
    auto fU = toUpper(forClause);
    auto msPos = fU.find(" MILLISECONDS");
    if (msPos != std::string::npos) forClause = trim(forClause.substr(0, msPos));
    auto msE = bc.expr(forClause, line);
    auto fromE = bc.expr(trim(rest.substr(fromPos + 6, toKw - (fromPos + 6))), line);
    auto toE = bc.expr(trim(rest.substr(toKw + 4, intoPos - (toKw + 4))), line);
    auto intoName = resolveRxCellName(bc.rxCells, bc.rxEntityStack, trim(rest.substr(intoPos + 6)));
    if (!bc.rxCells.count(intoName)) {
      bc.fail(line, "TIMELINE INTO needs a REMEMBER'd NUMBER cell — '" + intoName + "'");
      return;
    }
    {
      Ty ity = bc.rxCellTy.count(intoName) ? bc.rxCellTy[intoName] : Ty::num();
      if (ity.k == K::Int) {
        bc.fail(line, "TIMELINE INTO '" + intoName +
                          "' is INTEGER — use REMEMBER " + intoName + " AS NUMBER for fractional progress");
        return;
      }
      if (ity.k != K::Num) {
        bc.fail(line, "TIMELINE INTO needs a NUMBER cell — '" + intoName + "'");
        return;
      }
    }
    if (idLit.empty()) idLit = idRaw;
    bc.usesRx = true;
    bc.usesTimeline = true;
    bc.rxTimelineBinds.push_back({idLit, intoName, 0, 1, 0, line});
    o << "  _luke_active_timeline_id = luke_text(\"" << esc(idLit) << "\");\n";
    o << "  luke_rx_timeline_register(_luke_rx, _luke_active_timeline_id, _luke_rx_id_"
      << cIdent(intoName) << ", " << fromE.code << ", " << toE.code << ");\n";
    if (bc.forBrowser) {
      o << "  luke_js_timeline_start(luke_text(\"" << esc(idLit) << "\"), " << msE.code << ");\n";
    } else {
      o << "  luke_rx_timeline_run_sync(_luke_rx, luke_text(\"" << esc(idLit) << "\"), "
        << fromE.code << ", " << toE.code << ", _luke_rx_id_" << cIdent(intoName) << ", 10);\n";
    }
    return;
  }
  if (startsWithCI(text, "THE ") && !startsWithCI(text, "THE VALUE OF ") &&
      !startsWithCI(text, "THE WEAK VALUE OF ") &&
      !startsWithCI(text, "THE NODE ID OF ") && !startsWithCI(text, "THE DEP COUNT OF ") &&
      !startsWithCI(text, "THE SUB COUNT OF ") && !startsWithCI(text, "THE WHY ROOT OF ") &&
      !startsWithCI(text, "THE WHY DEPTH OF ") &&
      !startsWithCI(text, "THE TIMELINE STEP ID AT ") &&
      !startsWithCI(text, "THE TIMELINE STEP WAVE AT ") &&
      !startsWithCI(text, "THE BODY OF ") && !startsWithCI(text, "THE STATUS OF ")) {
    auto rest = trim(text.substr(4));
    auto U = toUpper(rest);
    auto isPos = U.find(" IS ");
    if (isPos != std::string::npos) {
      auto name = stripThe(trim(rest.substr(0, isPos)));
      auto exprRaw = trim(rest.substr(isPos + 4));
      bool simple = !name.empty();
      for (char c : name)
        if (!(isalnum((unsigned char)c) || c == '_' || c == '.')) simple = false;
      if (simple && !exprRaw.empty()) {
        std::string dkey =
            name.find('.') != std::string::npos ? name : rxScopedCellName(bc.rxEntityStack, name);
        bc.usesRx = true;
        bc.locals[name] = Ty::num();
        if (!bc.rxCells.count(dkey)) bc.rxCellOrder.push_back(dkey);
        bc.rxCells[dkey] = true;
        bc.rxDerived[dkey] = true;
        int found = 0;
        for (auto &d : bc.rxDerivedDefs)
          if (d.name == dkey) {
            d.exprRaw = exprRaw;
            d.line = line;
            found = 1;
          }
        if (!found) bc.rxDerivedDefs.push_back({dkey, exprRaw, line});
        emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(dkey),
                          "luke_rx_derived(_luke_rx, _luke_rx_fn_" + cIdent(dkey) + ", NULL)",
                          dkey);
        return;
      }
    }
  }
  if (startsWithCI(text, "MY NAME IS ")) {
    auto rest = trim(text.substr(11));
    auto U = toUpper(rest);
    Ty forced = Ty::vod();
    auto asPos = U.find(" AS ");
    auto setPos = U.find(" SET TO ");
    std::string name;
    if (asPos != std::string::npos && (setPos == std::string::npos || asPos < setPos)) {
      name = trim(rest.substr(0, asPos));
      auto after = trim(rest.substr(asPos + 4));
      auto set2 = toUpper(after).find(" SET TO ");
      if (set2 != std::string::npos) {
        auto tyRaw = trim(after.substr(0, set2));
        forced = bc.parseTy(tyRaw);
        if (forced.k == K::Void) {
          bc.fail(line, "Unknown type '" + tyRaw +
                            "' — use NUMBER, INTEGER, TEXT, FLAG, JSON, LIST, MAP, SERVER, "
                            "REQUEST, DATABASE, or a blueprint name");
          return;
        }
        rest = name + " SET TO " + trim(after.substr(set2 + 8));
        U = toUpper(rest);
        setPos = U.find(" SET TO ");
      } else {
        forced = bc.parseTy(after);
        if (forced.k == K::Void) {
          bc.fail(line, "Unknown type '" + after +
                            "' — use NUMBER, INTEGER, TEXT, FLAG, JSON, LIST, MAP, SERVER, "
                            "REQUEST, DATABASE, or a blueprint name");
          return;
        }
        rest = name;
        setPos = std::string::npos;
      }
    }
    Expr e{"0", Ty::num()};
    if (setPos == std::string::npos) {
      name = trim(rest);
      if (forced.k == K::Text) e = {"luke_text(\"\")", Ty::text()};
      else if (forced.k == K::Flag) e = {"0", Ty::flag()};
      else if (forced.k == K::Int) e = {"0LL", Ty::integer()};
      else if (forced.k == K::Json) e = {"((LukeJson*)0)", Ty::json()};
      else if (forced.k == K::List) e = {"luke_list_new(arena)", Ty::list()};
      else if (forced.k == K::Map) e = {"luke_map_new(arena)", Ty::map()};
      else if (forced.k == K::Ptr) {
        if (forced.klass == "__HttpServer")
          e = {"((LukeHttpServer*)0)", forced};
        else if (forced.klass == "__HttpReq")
          e = {"((LukeHttpRequest*)0)", forced};
        else if (forced.klass == "__Db")
          e = {"((LukeDb*)0)", forced};
        else
          e = {"((" + cIdent(forced.klass) + "*)0)", forced};
      } else if (forced.k == K::Void) e = {"luke_text(\"\")", Ty::text()};
      else e = {"0.0", Ty::num()};
      if (forced.k != K::Void) e.ty = forced;
    } else {
      name = trim(rest.substr(0, setPos));
      e = bc.expr(trim(rest.substr(setPos + 8)), line);
      if (forced.k != K::Void) {
        e = bc.coerceTo(line, e, forced, "MY NAME IS " + name + " AS " + tyName(forced));
      }
    }
    if (!bc.locals.count(name)) {
      bc.locals[name] = e.ty;
      o << "  " << cTy(e.ty) << " " << cIdent(name) << " = " << e.code << ";\n";
    } else {
      e = bc.coerceTo(line, e, bc.locals[name], "MY NAME IS " + name);
      o << "  " << cIdent(name) << " = " << e.code << ";\n";
    }
    return;
  }
  if (startsWithCI(text, "SET ")) {
    auto rest = trim(text.substr(4));
    auto U = toUpper(rest);
    /* SET ITEM n OF list TO v — reactive or plain list slot write */
    if (startsWithCI(rest, "ITEM ")) {
      auto itemRest = trim(rest.substr(5));
      auto iU = toUpper(itemRest);
      auto ofPos = iU.find(" OF ");
      auto toPos = iU.find(" TO ");
      if (ofPos != std::string::npos && toPos != std::string::npos && toPos > ofPos) {
        auto idxE = bc.expr(trim(itemRest.substr(0, ofPos)), line);
        auto listName = stripThe(trim(itemRest.substr(ofPos + 4, toPos - (ofPos + 4))));
        auto valE = bc.coerceText(bc.expr(trim(itemRest.substr(toPos + 4)), line));
        bc.expectTy(line, idxE.ty, Ty::num(), "SET ITEM … OF");
        if (bc.rxCells.count(listName) && bc.rxCellTy.count(listName) &&
            bc.rxCellTy[listName].k == K::List) {
          bc.usesRx = true;
          o << "  luke_rx_list_set(_luke_rx, _luke_rx_id_" << cIdent(listName) << ", " << idxE.code
            << ", " << valE.code << ");\n";
          return;
        }
        if (!bc.locals.count(listName) || bc.locals[listName].k != K::List) {
          bc.fail(line, "SET ITEM … OF needs a LIST — not '" + listName + "'");
          return;
        }
        o << "  luke_list_set(" << cIdent(listName) << ", " << idxE.code << ", " << valE.code
          << ");\n";
        return;
      }
    }
    auto to = U.find(" TO ");
    if (to == std::string::npos) {
      bc.fail(line, "Expected SET x TO v — tell me what to change and what to put there");
      return;
    }
    auto target = trim(rest.substr(0, to));
    auto e = bc.expr(trim(rest.substr(to + 4)), line);
    if (startsWithCI(target, "SELF.")) {
      auto field = trim(target.substr(5));
      for (auto &f : bc.flatFields(bc.curClass)) {
        if (f.name == field) {
          if (f.priv && f.owner != bc.curClass) {
            bc.fail(line, "Field '" + field + "' is PRIVATE/SECRET on " + f.owner +
                              " — only that blueprint's methods may touch it");
            return;
          }
          bc.expectTy(line, e.ty, f.ty, "SET SELF." + field);
          o << "  self->" << bc.fname(f) << " = " << e.code << ";\n";
          return;
        }
      }
      bc.fail(line, "Unknown field '" + field + "' on " + bc.curClass + " — declare it with HAS");
      return;
    }
    auto dot = target.find('.');
    if (dot != std::string::npos) {
      auto obj = target.substr(0, dot), field = target.substr(dot + 1);
      if (bc.locals.count(obj) && bc.locals[obj].k == K::Ptr) {
        for (auto &f : bc.flatFields(bc.locals[obj].klass)) {
          if (f.name == field) {
            bc.expectTy(line, e.ty, f.ty, "SET " + target);
            o << "  " << cIdent(obj) << "->" << bc.fname(f) << " = " << e.code << ";\n";
            return;
          }
        }
        bc.fail(line, "No field '" + field + "' on " + bc.locals[obj].klass);
        return;
      }
    }
    if (!bc.locals.count(target)) {
      bc.locals[target] = e.ty;
      o << "  " << cTy(e.ty) << " " << cIdent(target) << " = " << e.code << ";\n";
    } else {
      e = bc.coerceTo(line, e, bc.locals[target], "SET " + target);
      o << "  " << cIdent(target) << " = " << e.code << ";\n";
    }
    return;
  }
  /* REQUIRE LOGIN ON req WITH db — secure session gate (401 + return). */
  if (startsWithCI(text, "REQUIRE LOGIN ")) {
    auto rest = trim(text.substr(14));
    stripDo(rest);
    auto U = toUpper(rest);
    size_t onPos = std::string::npos;
    size_t withPos = U.find(" WITH ");
    if (startsWithCI(rest, "ON ")) {
      onPos = 0;
    } else {
      onPos = U.find(" ON ");
    }
    if (onPos == std::string::npos || withPos == std::string::npos || withPos < onPos) {
      bc.fail(line, "REQUIRE LOGIN needs — REQUIRE LOGIN ON req WITH db");
      return;
    }
    std::string reqName;
    if (onPos == 0)
      reqName = stripThe(trim(rest.substr(3, withPos - 3)));
    else
      reqName = stripThe(trim(rest.substr(onPos + 4, withPos - (onPos + 4))));
    auto dbName = stripThe(trim(rest.substr(withPos + 6)));
    if (!bc.locals.count(reqName) || bc.locals[reqName].k != K::Ptr ||
        bc.locals[reqName].klass != "__HttpReq") {
      bc.fail(line, "REQUIRE LOGIN ON needs a REQUEST");
      return;
    }
    if (!bc.locals.count(dbName) || bc.locals[dbName].k != K::Ptr ||
        bc.locals[dbName].klass != "__Db") {
      bc.fail(line, "REQUIRE LOGIN WITH needs a DATABASE");
      return;
    }
    o << "  if (!luke_auth_require(arena, " << cIdent(dbName) << ", " << cIdent(reqName) << ")) {\n";
    o << "    httpReply(arena, " << cIdent(reqName)
      << ", 401, luke_text(\"text/plain\"), luke_text(\"login required\"));\n";
    o << "    dbClose(arena, " << cIdent(dbName) << ");\n";
    o << "    return 0;\n";
    o << "  }\n";
    return;
  }
  /* REQUIRE CSRF ON req WITH db — middleware you can't forget (beachhead). */
  if (startsWithCI(text, "REQUIRE CSRF ")) {
    auto rest = trim(text.substr(13));
    stripDo(rest);
    auto U = toUpper(rest);
    size_t onPos = std::string::npos;
    size_t withPos = U.find(" WITH ");
    if (startsWithCI(rest, "ON "))
      onPos = 0;
    else
      onPos = U.find(" ON ");
    if (onPos == std::string::npos || withPos == std::string::npos || withPos < onPos) {
      bc.fail(line, "REQUIRE CSRF needs — REQUIRE CSRF ON req WITH db");
      return;
    }
    std::string reqName;
    if (onPos == 0)
      reqName = stripThe(trim(rest.substr(3, withPos - 3)));
    else
      reqName = stripThe(trim(rest.substr(onPos + 4, withPos - (onPos + 4))));
    auto dbName = stripThe(trim(rest.substr(withPos + 6)));
    if (!bc.locals.count(reqName) || bc.locals[reqName].k != K::Ptr ||
        bc.locals[reqName].klass != "__HttpReq") {
      bc.fail(line, "REQUIRE CSRF ON needs a REQUEST");
      return;
    }
    if (!bc.locals.count(dbName) || bc.locals[dbName].k != K::Ptr ||
        bc.locals[dbName].klass != "__Db") {
      bc.fail(line, "REQUIRE CSRF WITH needs a DATABASE");
      return;
    }
    o << "  if (!luke_auth_check_csrf(arena, " << cIdent(dbName) << ", " << cIdent(reqName)
      << ")) {\n";
    o << "    httpReply(arena, " << cIdent(reqName)
      << ", 403, luke_text(\"text/plain\"), luke_text(\"csrf required\"));\n";
    o << "    dbClose(arena, " << cIdent(dbName) << ");\n";
    o << "    return 0;\n";
    o << "  }\n";
    return;
  }
  /* MIDDLEWARE ORDER AUTH THEN RATE LIMIT — capability order compile check. */
  if (startsWithCI(text, "MIDDLEWARE ORDER ")) {
    auto rest = trim(text.substr(17));
    std::vector<std::string> caps;
    std::string cur;
    /* Normalize "RATE LIMIT" → "RATE_LIMIT" */
    for (size_t i = 0; i < rest.size(); ++i) {
      if (isspace((unsigned char)rest[i]) || rest[i] == ',') {
        if (!cur.empty()) {
          auto nu = toUpper(cur);
          cur.clear();
          if (nu == "THEN" || nu == "BEFORE" || nu == "AND") continue;
          if (nu == "RATE" && i + 1 < rest.size()) {
            /* peek LIMIT */
            size_t j = i;
            while (j < rest.size() && isspace((unsigned char)rest[j])) ++j;
            if (j + 4 < rest.size() && toUpper(rest.substr(j, 5)) == "LIMIT") {
              caps.push_back("RATE_LIMIT");
              i = j + 4;
              continue;
            }
          }
          if (nu == "LIMIT" && !caps.empty() && caps.back() == "RATE") {
            caps.back() = "RATE_LIMIT";
            continue;
          }
          caps.push_back(nu);
        }
      } else {
        cur.push_back(rest[i]);
      }
    }
    if (!cur.empty()) {
      auto nu = toUpper(cur);
      if (nu == "LIMIT" && !caps.empty() && caps.back() == "RATE")
        caps.back() = "RATE_LIMIT";
      else if (nu != "THEN" && nu != "BEFORE" && nu != "AND")
        caps.push_back(nu);
    }
    if (caps.size() < 2) {
      bc.fail(line, "MIDDLEWARE ORDER needs at least two capabilities — "
                    "MIDDLEWARE ORDER AUTH THEN RATE LIMIT");
      return;
    }
    /* Reject RATE_LIMIT before AUTH */
    int authPos = -1, ratePos = -1;
    for (size_t i = 0; i < caps.size(); ++i) {
      if (caps[i] == "AUTH" || caps[i] == "LOGIN" || caps[i] == "AUTHENTICATION")
        authPos = (int)i;
      if (caps[i] == "RATE_LIMIT" || caps[i] == "RATELIMIT") ratePos = (int)i;
    }
    if (authPos >= 0 && ratePos >= 0 && ratePos < authPos) {
      bc.fail(line, "MIDDLEWARE ORDER — RATE LIMIT before AUTH is a compile error "
                    "(auth must run first)");
      return;
    }
    bc.middlewareOrder = caps;
    bc.hasMiddlewareOrder = true;
    o << "  /* middleware order:";
    for (auto &c : caps) o << " " << c;
    o << " */\n";
    return;
  }
  /* LINK TO "/path" — compile-time route integrity (self-inflicted 404 = error). */
  if (startsWithCI(text, "LINK TO ")) {
    auto raw = trim(text.substr(8));
    std::string path = unquoteText(raw);
    if (path.empty()) path = raw;
    if (!path.empty() && path.front() == '"' && path.back() == '"')
      path = path.substr(1, path.size() - 2);
    if (path.empty() || path[0] != '/') {
      bc.fail(line, "LINK TO needs a path — LINK TO \"/user/1\"");
      return;
    }
    if (!bc.hasRoutesBlock || bc.routes.empty()) {
      bc.fail(line, "LINK TO requires a ROUTES table — declare ROUTES … END ROUTES first");
      return;
    }
    auto pathSegs = [](const std::string &p) {
      std::vector<std::string> s;
      std::string cur;
      for (char c : p) {
        if (c == '/') {
          if (!cur.empty()) {
            s.push_back(cur);
            cur.clear();
          }
        } else
          cur.push_back(c);
      }
      if (!cur.empty()) s.push_back(cur);
      return s;
    };
    auto want = pathSegs(path);
    bool ok = false;
    for (auto &r : bc.routes) {
      auto pat = pathSegs(r.pattern);
      if (pat.size() != want.size()) continue;
      bool match = true;
      for (size_t i = 0; i < pat.size(); ++i) {
        if (!pat[i].empty() && pat[i][0] == ':') continue;
        if (pat[i] != want[i]) {
          match = false;
          break;
        }
      }
      if (match) {
        ok = true;
        break;
      }
    }
    if (!ok) {
      bc.fail(line, "LINK TO \"" + path + "\" — no matching ROUTES entry (broken route = compile error)");
      return;
    }
    o << "  /* link-ok " << esc(path) << " */\n";
    return;
  }
  /* ENSURE SCHEMA name ON db — emit CREATE TABLE IF NOT EXISTS from SCHEMA block. */
  if (startsWithCI(text, "ENSURE SCHEMA ")) {
    auto rest = trim(text.substr(14));
    auto U = toUpper(rest);
    auto onPos = U.find(" ON ");
    if (onPos == std::string::npos) {
      bc.fail(line, "ENSURE SCHEMA needs — ENSURE SCHEMA notes ON db");
      return;
    }
    auto sname = stripThe(trim(rest.substr(0, onPos)));
    auto dbName = stripThe(trim(rest.substr(onPos + 4)));
    if (!bc.schemas.count(sname)) {
      bc.fail(line, "ENSURE SCHEMA '" + sname + "' — declare SCHEMA " + sname + " … END SCHEMA first");
      return;
    }
    if (!bc.locals.count(dbName) || bc.locals[dbName].k != K::Ptr ||
        bc.locals[dbName].klass != "__Db") {
      bc.fail(line, "ENSURE SCHEMA ON needs a DATABASE");
      return;
    }
    auto &sch = bc.schemas[sname];
    std::ostringstream sql;
    sql << "CREATE TABLE IF NOT EXISTS " << sname << "(";
    for (size_t i = 0; i < sch.fields.size(); ++i) {
      if (i) sql << ", ";
      sql << sch.fields[i].name << " " << sch.fields[i].sqlTy;
    }
    sql << ")";
    o << "  luke_db_exec(" << cIdent(dbName) << ", luke_text(\"" << esc(sql.str()) << "\"));\n";
    return;
  }
  /* VALIDATE FORM name FROM map — writes form_<name>_<field>_error + form_<name>_ok cells. */
  if (startsWithCI(text, "VALIDATE FORM ")) {
    auto rest = trim(text.substr(14));
    auto U = toUpper(rest);
    auto fromPos = U.find(" FROM ");
    if (fromPos == std::string::npos) {
      bc.fail(line, "VALIDATE FORM needs — VALIDATE FORM login FROM params");
      return;
    }
    auto fname = stripThe(trim(rest.substr(0, fromPos)));
    auto mapName = stripThe(trim(rest.substr(fromPos + 6)));
    if (!bc.forms.count(fname)) {
      bc.fail(line, "VALIDATE FORM '" + fname + "' — declare FORM " + fname + " … END FORM first");
      return;
    }
    if (!bc.locals.count(mapName) || bc.locals[mapName].k != K::Map) {
      bc.fail(line, "VALIDATE FORM FROM needs a MAP");
      return;
    }
    auto &form = bc.forms[fname];
    std::string okCell = "form_" + fname + "_ok";
    bc.usesRx = true;
    o << "  {\n";
    o << "    int _luke_form_ok = 1;\n";
    o << "    if (_luke_rx) luke_rx_batch_begin(_luke_rx);\n";
    for (auto &f : form.fields) {
      std::string errCell = "form_" + fname + "_" + f.name + "_error";
      o << "    {\n";
      o << "      LukeText _luke_fv = luke_map_get(" << cIdent(mapName) << ", luke_text(\""
        << esc(f.name) << "\"));\n";
      o << "      LukeText _luke_ferr = luke_text(\"\");\n";
      if (toUpper(f.ty) == "EMAIL") {
        o << "      if (!_luke_fv.len) _luke_ferr = luke_text(\"required\");\n";
        o << "      else if (!memchr(_luke_fv.ptr, '@', _luke_fv.len)) _luke_ferr = luke_text(\"invalid_email\");\n";
      } else if (toUpper(f.ty) == "INTEGER" && f.hasRange) {
        o << "      {\n";
        o << "        long _luke_iv = _luke_fv.len ? atol(_luke_fv.ptr) : 0;\n";
        o << "        if (!_luke_fv.len) _luke_ferr = luke_text(\"required\");\n";
        o << "        else if (_luke_iv < " << f.minV << " || _luke_iv > " << f.maxV
          << ") _luke_ferr = luke_text(\"out_of_range\");\n";
        o << "      }\n";
      } else if (toUpper(f.ty) == "PASSWORD") {
        o << "      if (_luke_fv.len < 8) _luke_ferr = luke_text(\"too_short\");\n";
      } else {
        o << "      if (!_luke_fv.len) _luke_ferr = luke_text(\"required\");\n";
      }
      o << "      if (_luke_ferr.len) _luke_form_ok = 0;\n";
      o << "      if (_luke_rx) luke_rx_write_text(_luke_rx, _luke_rx_id_" << cIdent(errCell)
        << ", _luke_ferr);\n";
      o << "    }\n";
    }
    o << "    if (_luke_rx) luke_rx_write_text(_luke_rx, _luke_rx_id_" << cIdent(okCell)
      << ", _luke_form_ok ? luke_text(\"1\") : luke_text(\"0\"));\n";
    o << "    if (_luke_rx) luke_rx_batch_end(_luke_rx);\n";
    o << "    if (!_luke_form_ok) luke_speak_text(luke_text(\"form_invalid=" << esc(fname)
      << "\"));\n";
    o << "    else luke_speak_text(luke_text(\"form_ok=" << esc(fname) << "\"));\n";
    o << "  }\n";
    return;
  }
  /* SERVE ROUTES ON server WITH n — codegen dispatch from ROUTES table (no clever invention). */
  if (startsWithCI(text, "SERVE ROUTES ")) {
    auto rest = trim(text.substr(13));
    auto U = toUpper(rest);
    size_t onPos = std::string::npos;
    auto withPos = U.find(" WITH ");
    if (startsWithCI(rest, "ON "))
      onPos = 0;
    else
      onPos = U.find(" ON ");
    if (onPos == std::string::npos || withPos == std::string::npos ||
        (onPos != 0 && withPos < onPos)) {
      bc.fail(line, "SERVE ROUTES needs — SERVE ROUTES ON server WITH 8");
      return;
    }
    std::string serverName;
    if (onPos == 0)
      serverName = stripThe(trim(rest.substr(3, withPos - 3)));
    else
      serverName = stripThe(trim(rest.substr(onPos + 4, withPos - (onPos + 4))));
    auto workersE = bc.coerceTo(line, bc.expr(trim(rest.substr(withPos + 6)), line), Ty::num(),
                                "SERVE ROUTES WITH");
    if (!bc.hasRoutesBlock || bc.routes.empty()) {
      bc.fail(line, "SERVE ROUTES needs ROUTES … END ROUTES first");
      return;
    }
    if (!bc.locals.count(serverName) || bc.locals[serverName].k != K::Ptr ||
        bc.locals[serverName].klass != "__HttpServer") {
      bc.fail(line, "SERVE ROUTES ON needs an HTTP server — ASK httpListen first");
      return;
    }
    for (auto &r : bc.routes) {
      if (r.handler.empty()) {
        bc.fail(r.line, "SERVE ROUTES needs HANDLE on every route — " + r.method + " \"" +
                            r.pattern + "\" HANDLE name");
        return;
      }
      if (!bc.fns.count(r.handler)) {
        bc.fail(r.line, "ROUTE HANDLE '" + r.handler +
                            "' — define THIS IS FUNCTION " + r.handler + " WITH req AS REQUEST");
        return;
      }
      auto &hfn = bc.fns[r.handler];
      if (hfn.params.size() != 1 || hfn.params[0].ty.k != K::Ptr ||
          hfn.params[0].ty.klass != "__HttpReq") {
        bc.fail(r.line, "ROUTE HANDLE '" + r.handler + "' must take one REQUEST argument");
        return;
      }
    }
    bc.serveRoutes = true;
    bc.needsPthread = true;
    o << "  luke_http_serve(" << cIdent(serverName) << ", luke_http_wrap___luke_routes, "
      << workersE.code << ");\n";
    return;
  }
  /* MIGRATE name ON db TO n — apply UP steps to target version. */
  if (startsWithCI(text, "MIGRATE ")) {
    auto rest = trim(text.substr(8));
    auto U = toUpper(rest);
    auto onPos = U.find(" ON ");
    auto toPos = U.find(" TO ");
    if (onPos == std::string::npos || toPos == std::string::npos || toPos < onPos) {
      bc.fail(line, "MIGRATE needs — MIGRATE app ON db TO 2");
      return;
    }
    auto mname = stripThe(trim(rest.substr(0, onPos)));
    auto dbName = stripThe(trim(rest.substr(onPos + 4, toPos - (onPos + 4))));
    auto verE = bc.coerceTo(line, bc.expr(trim(rest.substr(toPos + 4)), line), Ty::num(), "MIGRATE TO");
    if (!bc.migrations.count(mname)) {
      bc.fail(line, "MIGRATE '" + mname + "' — declare MIGRATION " + mname + " … END MIGRATION first");
      return;
    }
    if (!bc.locals.count(dbName) || bc.locals[dbName].k != K::Ptr ||
        bc.locals[dbName].klass != "__Db") {
      bc.fail(line, "MIGRATE ON needs a DATABASE");
      return;
    }
    auto &mig = bc.migrations[mname];
    o << "  {\n";
    o << "    luke_db_migrate_ensure(" << cIdent(dbName) << ");\n";
    o << "    int64_t _luke_mig_cur = luke_db_migrate_version(" << cIdent(dbName) << ");\n";
    o << "    int64_t _luke_mig_to = (int64_t)(" << verE.code << ");\n";
    o << "    if (_luke_mig_to < _luke_mig_cur) {\n";
    o << "      luke_speak_text(luke_text(\"migrate_use_rewind\"));\n";
    o << "    } else {\n";
    o << "      for (int64_t _luke_v = _luke_mig_cur + 1; _luke_v <= _luke_mig_to; ++_luke_v) {\n";
    o << "        int _luke_did = 0;\n";
    for (auto &st : mig.steps) {
      o << "        if (_luke_v == " << st.version << "LL) {\n";
      o << "          if (!luke_db_exec(" << cIdent(dbName) << ", luke_text(\"" << esc(st.upSql)
        << "\"))) luke_speak_text(luke_text(\"migrate_up_fail=" << st.version << "\"));\n";
      o << "          luke_db_migrate_set(" << cIdent(dbName) << ", " << st.version << "LL);\n";
      o << "          _luke_did = 1;\n";
      o << "        }\n";
    }
    o << "        if (!_luke_did) luke_speak_text(luke_text(\"migrate_missing_version\"));\n";
    o << "      }\n";
    o << "    }\n";
    o << "  }\n";
    return;
  }
  /* REWIND name ON db TO n — apply DOWN steps back to target version. */
  if (startsWithCI(text, "REWIND ")) {
    auto rest = trim(text.substr(7));
    auto U = toUpper(rest);
    auto onPos = U.find(" ON ");
    auto toPos = U.find(" TO ");
    if (onPos == std::string::npos || toPos == std::string::npos || toPos < onPos) {
      bc.fail(line, "REWIND needs — REWIND app ON db TO 1");
      return;
    }
    auto mname = stripThe(trim(rest.substr(0, onPos)));
    auto dbName = stripThe(trim(rest.substr(onPos + 4, toPos - (onPos + 4))));
    auto verE = bc.coerceTo(line, bc.expr(trim(rest.substr(toPos + 4)), line), Ty::num(), "REWIND TO");
    if (!bc.migrations.count(mname)) {
      bc.fail(line, "REWIND '" + mname + "' — declare MIGRATION " + mname + " … END MIGRATION first");
      return;
    }
    if (!bc.locals.count(dbName) || bc.locals[dbName].k != K::Ptr ||
        bc.locals[dbName].klass != "__Db") {
      bc.fail(line, "REWIND ON needs a DATABASE");
      return;
    }
    auto &mig = bc.migrations[mname];
    o << "  {\n";
    o << "    luke_db_migrate_ensure(" << cIdent(dbName) << ");\n";
    o << "    int64_t _luke_mig_cur = luke_db_migrate_version(" << cIdent(dbName) << ");\n";
    o << "    int64_t _luke_mig_to = (int64_t)(" << verE.code << ");\n";
    o << "    if (_luke_mig_to > _luke_mig_cur) {\n";
    o << "      luke_speak_text(luke_text(\"rewind_use_migrate\"));\n";
    o << "    } else {\n";
    o << "      for (int64_t _luke_v = _luke_mig_cur; _luke_v > _luke_mig_to; --_luke_v) {\n";
    o << "        int _luke_did = 0;\n";
    for (auto &st : mig.steps) {
      o << "        if (_luke_v == " << st.version << "LL) {\n";
      o << "          if (!luke_db_exec(" << cIdent(dbName) << ", luke_text(\"" << esc(st.downSql)
        << "\"))) luke_speak_text(luke_text(\"migrate_down_fail=" << st.version << "\"));\n";
      o << "          luke_db_migrate_set(" << cIdent(dbName) << ", " << (st.version - 1) << "LL);\n";
      o << "          _luke_did = 1;\n";
      o << "        }\n";
    }
    o << "        if (!_luke_did) luke_speak_text(luke_text(\"rewind_missing_version\"));\n";
    o << "      }\n";
    o << "    }\n";
    o << "  }\n";
    return;
  }
  /* REVOKE ACCESS — clear CURRENT USER + empty SECRET reactive cells (live revoke). */
  if (toUpper(text) == "REVOKE ACCESS") {
    o << "  luke_auth_revoke();\n";
    for (auto &kv : bc.rxSecretCells) {
      if (!bc.rxCells.count(kv.first)) continue;
      Ty ty = bc.rxCellTy.count(kv.first) ? bc.rxCellTy[kv.first] : Ty::text();
      if (ty.k == K::Text)
        o << "  if (_luke_rx) luke_rx_write_text(_luke_rx, _luke_rx_id_" << cIdent(kv.first)
          << ", luke_text(\"\"));\n";
      else if (ty.k == K::Int)
        o << "  if (_luke_rx) luke_rx_write_int(_luke_rx, _luke_rx_id_" << cIdent(kv.first)
          << ", 0LL);\n";
      else if (ty.k == K::Num)
        o << "  if (_luke_rx) luke_rx_write_num(_luke_rx, _luke_rx_id_" << cIdent(kv.first)
          << ", 0.0);\n";
    }
    o << "  if (_luke_rx) luke_rx_flush(_luke_rx);\n";
    return;
  }
  /* LIMIT login TO 5 PER MINUTE [PER ip] — scheduler-native rate-limit beachhead. */
  if (startsWithCI(text, "LIMIT ")) {
    auto rest = trim(text.substr(6));
    auto U = toUpper(rest);
    auto toPos = U.find(" TO ");
    if (toPos == std::string::npos) {
      bc.fail(line, "LIMIT needs: LIMIT login TO 5 PER MINUTE [PER ip]");
      return;
    }
    auto limName = stripThe(trim(rest.substr(0, toPos)));
    auto after = trim(rest.substr(toPos + 4));
    auto aU = toUpper(after);
    if (limName.empty()) {
      bc.fail(line, "LIMIT needs a resource name — LIMIT login TO 5 PER MINUTE");
      return;
    }
    int maxN = 0;
    size_t i = 0;
    while (i < after.size() && isdigit((unsigned char)after[i])) {
      maxN = maxN * 10 + (after[i] - '0');
      ++i;
    }
    if (maxN <= 0) {
      bc.fail(line, "LIMIT needs a positive count — LIMIT login TO 5 PER MINUTE");
      return;
    }
    auto unit = trim(after.substr(i));
    auto uU = toUpper(unit);
    int windowSecs = 60;
    bool perIp = false;
    if (startsWithCI(unit, "PER MINUTE")) {
      windowSecs = 60;
      auto restU = trim(unit.substr(10));
      if (startsWithCI(restU, "PER IP") || startsWithCI(restU, "PER ip")) perIp = true;
    } else if (startsWithCI(unit, "PER SECOND")) {
      windowSecs = 1;
      auto restU = trim(unit.substr(10));
      if (startsWithCI(restU, "PER IP")) perIp = true;
    } else if (startsWithCI(unit, "PER HOUR")) {
      windowSecs = 3600;
      auto restU = trim(unit.substr(8));
      if (startsWithCI(restU, "PER IP")) perIp = true;
    } else {
      bc.fail(line, "LIMIT window needs PER MINUTE / PER SECOND / PER HOUR");
      return;
    }
    (void)uU;
    BC::LimitDef ld;
    ld.name = limName;
    ld.maxN = maxN;
    ld.windowSecs = windowSecs;
    ld.perIp = perIp;
    ld.line = line;
    bc.limits[limName] = ld;
    std::string remCell = limName + ".remaining";
    bc.usesRx = true;
    bc.locals[remCell] = Ty::text();
    bc.rxCellTy[remCell] = Ty::text();
    if (!bc.rxCells.count(remCell)) bc.rxCellOrder.push_back(remCell);
    bc.rxCells[remCell] = true;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", maxN);
    o << "  luke_auth_configure_limit(" << maxN << ", " << windowSecs << ");\n";
    emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(remCell),
                      std::string("luke_rx_cell_text(_luke_rx, luke_text(\"") + buf + "\"))",
                      remCell);
    return;
  }
  /* REFRESH LIMIT login WITH db, email — push remaining into login.remaining cell. */
  if (startsWithCI(text, "REFRESH LIMIT ")) {
    auto rest = trim(text.substr(14));
    auto U = toUpper(rest);
    auto withPos = U.find(" WITH ");
    if (withPos == std::string::npos) {
      bc.fail(line, "REFRESH LIMIT needs: REFRESH LIMIT login WITH db, email");
      return;
    }
    auto limName = stripThe(trim(rest.substr(0, withPos)));
    auto args = trim(rest.substr(withPos + 6));
    auto comma = args.find(',');
    if (comma == std::string::npos) {
      bc.fail(line, "REFRESH LIMIT needs db, email — REFRESH LIMIT login WITH db, email");
      return;
    }
    auto dbName = stripThe(trim(args.substr(0, comma)));
    auto emailE = bc.coerceText(bc.expr(trim(args.substr(comma + 1)), line));
    if (!bc.limits.count(limName)) {
      bc.fail(line, "REFRESH LIMIT '" + limName + "' — declare LIMIT " + limName + " TO … first");
      return;
    }
    if (!bc.locals.count(dbName) || bc.locals[dbName].k != K::Ptr ||
        bc.locals[dbName].klass != "__Db") {
      bc.fail(line, "REFRESH LIMIT WITH needs a DATABASE");
      return;
    }
    std::string remCell = limName + ".remaining";
    bc.usesRx = true;
    o << "  luke_rx_write_text(_luke_rx, _luke_rx_id_" << cIdent(remCell)
      << ", luke_auth_attempts_left(arena, " << cIdent(dbName) << ", " << emailE.code << "));\n";
    o << "  luke_rx_flush(_luke_rx);\n";
    return;
  }
  /* ADVANCE FLOW signup — legal step transition only (collect→verify→done). */
  if (startsWithCI(text, "ADVANCE FLOW ")) {
    auto fname = stripThe(trim(text.substr(13)));
    if (!bc.flows.count(fname)) {
      bc.fail(line, "ADVANCE FLOW '" + fname + "' — declare FLOW " + fname + " … END FLOW first");
      return;
    }
    auto &fl = bc.flows[fname];
    std::string stepCell = fname + "_step";
    bc.usesRx = true;
    o << "  {\n";
    o << "    LukeText _luke_fs = luke_rx_read_text(_luke_rx, _luke_rx_id_" << cIdent(stepCell)
      << ");\n";
    for (size_t si = 0; si + 1 < fl.steps.size(); ++si) {
      o << "    if (luke_text_eq(_luke_fs, luke_text(\"" << esc(fl.steps[si]) << "\"))) {\n";
      o << "      luke_rx_write_text(_luke_rx, _luke_rx_id_" << cIdent(stepCell) << ", luke_text(\""
        << esc(fl.steps[si + 1]) << "\"));\n";
      o << "    } else ";
    }
    o << "    { /* already at terminal or unknown */ }\n";
    o << "    luke_rx_flush(_luke_rx);\n";
    o << "  }\n";
    return;
  }
  /* CREATE ACCOUNT FROM FLOW signup WITH db — only legal at DONE step. */
  if (startsWithCI(text, "CREATE ACCOUNT FROM FLOW ")) {
    auto rest = trim(text.substr(25));
    auto U = toUpper(rest);
    auto withPos = U.find(" WITH ");
    if (withPos == std::string::npos) {
      bc.fail(line, "CREATE ACCOUNT FROM FLOW needs: CREATE ACCOUNT FROM FLOW signup WITH db");
      return;
    }
    auto fname = stripThe(trim(rest.substr(0, withPos)));
    auto dbName = stripThe(trim(rest.substr(withPos + 6)));
    if (!bc.flows.count(fname)) {
      bc.fail(line, "CREATE ACCOUNT FROM FLOW '" + fname + "' — unknown FLOW");
      return;
    }
    auto &fl = bc.flows[fname];
    if (!fl.hasVerify || !fl.hasDone) {
      bc.fail(line, "FLOW '" + fname + "' is incomplete — VERIFY before DONE required");
      return;
    }
    if (!bc.locals.count(dbName) || bc.locals[dbName].k != K::Ptr ||
        bc.locals[dbName].klass != "__Db") {
      bc.fail(line, "CREATE ACCOUNT FROM FLOW WITH needs a DATABASE");
      return;
    }
    std::string emailCell, passCell;
    for (auto &f : fl.collectFields) {
      auto Uf = toUpper(f);
      if (Uf == "EMAIL" || Uf == "USERNAME") emailCell = fname + "_" + f;
      if (Uf == "PASSWORD" || Uf == "PASS") passCell = fname + "_" + f;
    }
    if (emailCell.empty() || passCell.empty()) {
      bc.fail(line, "FLOW '" + fname + "' COLLECT must include email and password");
      return;
    }
    std::string stepCell = fname + "_step";
    bc.usesRx = true;
    o << "  {\n";
    o << "    LukeText _luke_fs = luke_rx_read_text(_luke_rx, _luke_rx_id_" << cIdent(stepCell)
      << ");\n";
    o << "    if (!luke_text_eq(_luke_fs, luke_text(\"done\"))) {\n";
    o << "      luke_speak_text(luke_text(\"FLOW create blocked — VERIFY not completed\"));\n";
    o << "    } else {\n";
    o << "      LukeText _luke_uid = luke_auth_create_account(arena, " << cIdent(dbName)
      << ", luke_rx_read_text(_luke_rx, _luke_rx_id_" << cIdent(emailCell)
      << "), luke_rx_read_text(_luke_rx, _luke_rx_id_" << cIdent(passCell) << "));\n";
    o << "      luke_speak_text(luke_text_concat(arena, luke_text(\"flow_uid=\"), _luke_uid));\n";
    o << "    }\n";
    o << "  }\n";
    return;
  }
  /* REVEAL last N OF secret AS masked — sole auditable declassification escape. */
  if (startsWithCI(text, "REVEAL ")) {
    auto rest = trim(text.substr(7));
    auto U = toUpper(rest);
    if (!startsWithCI(rest, "LAST ")) {
      bc.fail(line, "REVEAL needs: REVEAL last 4 OF ssn AS masked");
      return;
    }
    auto afterLast = trim(rest.substr(5));
    int n = 0;
    size_t i = 0;
    while (i < afterLast.size() && isdigit((unsigned char)afterLast[i])) {
      n = n * 10 + (afterLast[i] - '0');
      ++i;
    }
    if (n <= 0) {
      bc.fail(line, "REVEAL last N needs a positive N");
      return;
    }
    auto mid = trim(afterLast.substr(i));
    auto mU = toUpper(mid);
    size_t ofPos = std::string::npos;
    size_t ofLen = 4;
    if (startsWithCI(mid, "OF ")) {
      ofPos = 0;
      ofLen = 3;
    } else {
      ofPos = mU.find(" OF ");
      ofLen = 4;
    }
    auto asPos = mU.find(" AS ");
    if (ofPos == std::string::npos || asPos == std::string::npos || asPos < ofPos) {
      bc.fail(line, "REVEAL needs: REVEAL last 4 OF ssn AS masked");
      return;
    }
    auto srcName = stripThe(trim(mid.substr(ofPos + ofLen, asPos - (ofPos + ofLen))));
    auto destName = stripThe(trim(mid.substr(asPos + 4)));
    auto srcKey = resolveRxCellName(bc.rxCells, bc.rxEntityStack, srcName);
    if (!bc.rxSecretCells.count(srcKey) && !bc.rxSecretCells.count(srcName)) {
      bc.fail(line, "REVEAL source '" + srcName + "' must be SECRET");
      return;
    }
    if (destName.empty()) {
      bc.fail(line, "REVEAL needs a destination name after AS");
      return;
    }
    std::string destKey = rxScopedCellName(bc.rxEntityStack, destName);
    bc.usesRx = true;
    bc.locals[destName] = Ty::text();
    bc.rxCellTy[destKey] = Ty::text();
    if (!bc.rxCells.count(destKey)) bc.rxCellOrder.push_back(destKey);
    bc.rxCells[destKey] = true;
    /* Destination is intentionally NOT secret — declassified. */
    o << "  {\n";
    o << "    LukeText _luke_uid = luke_auth_current_user();\n";
    o << "    if (!_luke_uid.len) {\n";
    o << "      luke_speak_text(luke_text(\"REVEAL denied — CURRENT USER required\"));\n";
    emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(destKey),
                      "luke_rx_cell_text(_luke_rx, luke_text(\"\"))", destKey);
    o << "    } else {\n";
    o << "      LukeText _luke_rev = luke_auth_reveal_last(arena, luke_rx_read_text(_luke_rx, _luke_rx_id_"
      << cIdent(srcKey) << "), " << n << ");\n";
    emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(destKey),
                      "luke_rx_cell_text(_luke_rx, _luke_rev)", destKey);
    o << "      luke_auth_mark_reveal(arena, luke_text(\"" << esc(srcKey) << "\"), luke_text(\""
      << esc(destKey) << "\"));\n";
    o << "    }\n";
    o << "  }\n";
    return;
  }
  /* WHO SAW ssn [SINCE ts|"last week"] — audit trail (+ time filter). */
  if (startsWithCI(text, "WHO SAW ")) {
    auto rest = trim(text.substr(8));
    auto U = toUpper(rest);
    auto sincePos = U.find(" SINCE ");
    std::string name;
    std::string sinceExpr;
    if (sincePos != std::string::npos) {
      name = stripThe(trim(rest.substr(0, sincePos)));
      sinceExpr = trim(rest.substr(sincePos + 7));
    } else {
      name = stripThe(rest);
    }
    if (name.empty()) {
      bc.fail(line, "WHO SAW needs a field/cell name — WHO SAW ssn");
      return;
    }
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
      name = name.substr(1, name.size() - 2);
    if (sinceExpr.empty()) {
      o << "  luke_speak_text(luke_auth_who_saw(arena, luke_text(\"" << esc(name) << "\")));\n";
    } else {
      int64_t since = 0;
      auto sU = toUpper(sinceExpr);
      if (sU == "\"LAST WEEK\"" || sU == "LAST WEEK") {
        o << "  luke_speak_text(luke_auth_who_saw_since(arena, luke_text(\"" << esc(name)
          << "\"), (int64_t)time(NULL) - 7*86400));\n";
      } else if (sinceExpr.size() >= 2 && sinceExpr.front() == '"' && sinceExpr.back() == '"') {
        auto lit = sinceExpr.substr(1, sinceExpr.size() - 2);
        if (toUpper(lit) == "LAST WEEK")
          o << "  luke_speak_text(luke_auth_who_saw_since(arena, luke_text(\"" << esc(name)
            << "\"), (int64_t)time(NULL) - 7*86400));\n";
        else {
          since = (int64_t)strtoll(lit.c_str(), nullptr, 10);
          o << "  luke_speak_text(luke_auth_who_saw_since(arena, luke_text(\"" << esc(name)
            << "\"), " << since << "LL));\n";
        }
      } else {
        auto se = bc.expr(sinceExpr, line);
        if (se.ty.k == K::Int)
          o << "  luke_speak_text(luke_auth_who_saw_since(arena, luke_text(\"" << esc(name)
            << "\"), " << se.code << "));\n";
        else if (se.ty.k == K::Num)
          o << "  luke_speak_text(luke_auth_who_saw_since(arena, luke_text(\"" << esc(name)
            << "\"), (int64_t)" << se.code << "));\n";
        else if (se.ty.k == K::Text)
          o << "  luke_speak_text(luke_auth_who_saw_since(arena, luke_text(\"" << esc(name)
            << "\"), (int64_t)atoll(" << se.code << ".ptr)));\n";
        else
          bc.fail(line, "WHO SAW SINCE needs a time number or \"last week\"");
      }
    }
    return;
  }
  /* SCRUB TO access OF ssn — rewind audit cursor to first access (breach moment). */
  if (startsWithCI(text, "SCRUB TO ACCESS OF ")) {
    auto name = stripThe(trim(text.substr(19)));
    if (name.empty()) {
      bc.fail(line, "SCRUB TO access OF needs a field — SCRUB TO access OF ssn");
      return;
    }
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
      name = name.substr(1, name.size() - 2);
    o << "  luke_speak_text(luke_auth_scrub_to_access(arena, luke_text(\"" << esc(name)
      << "\")));\n";
    return;
  }
  if (startsWithCI(text, "IF ")) {
    auto rest = trim(text.substr(3));
    stripDo(rest);
    auto cond = bc.expr(rest, line);
    if (cond.ty.k != K::Flag && cond.ty.k != K::Num && cond.ty.k != K::Int)
      bc.fail(line, "IF needs a FLAG (or NUMBER/INTEGER) condition — got " + tyName(cond.ty));
    o << "  if (" << cond.code << ") {\n";
    return;
  }
  if (toUpper(text) == "END IF" || toUpper(text) == "ENDIF") {
    o << "  }\n";
    return;
  }
  if (startsWithCI(text, "WHILE ")) {
    auto rest = trim(text.substr(6));
    stripDo(rest);
    auto cond = bc.expr(rest, line);
    if (cond.ty.k != K::Flag && cond.ty.k != K::Num && cond.ty.k != K::Int)
      bc.fail(line, "WHILE needs a FLAG (or NUMBER/INTEGER) condition — got " + tyName(cond.ty));
    o << "  while (" << cond.code << ") {\n";
    return;
  }
  if (toUpper(text) == "END WHILE" || toUpper(text) == "ENDWHILE") {
    o << "  }\n";
    return;
  }
  if (startsWithCI(text, "FOR EACH ")) {
    auto rest = trim(text.substr(9));
    stripDo(rest);
    auto U = toUpper(rest);
    auto inPos = findOutsideQuotes(rest, U, " IN ");
    if (inPos == std::string::npos) {
      bc.fail(line, "FOR EACH needs: FOR EACH item IN list DO");
      return;
    }
    auto varName = trim(rest.substr(0, inPos));
    auto listE = bc.expr(trim(rest.substr(inPos + 4)), line);
    bc.expectTy(line, listE.ty, Ty::list(), "FOR EACH … IN");
    if (varName.empty()) {
      bc.fail(line, "FOR EACH needs an item name");
      return;
    }
    int id = ++bc.forEachSeq;
    o << "  {\n";
    o << "    double _luke_fe_n" << id << " = luke_list_len(" << listE.code << ");\n";
    o << "    for (double _luke_fe_i" << id << " = 0; _luke_fe_i" << id << " < _luke_fe_n" << id
      << "; _luke_fe_i" << id << " += 1) {\n";
    o << "      LukeText " << cIdent(varName) << " = luke_list_get(" << listE.code << ", _luke_fe_i"
      << id << ");\n";
    bc.locals[varName] = Ty::text();
    bc.forEachVars.push_back(varName);
    return;
  }
  if (toUpper(text) == "END FOR" || toUpper(text) == "ENDFOR" || toUpper(text) == "END FOR EACH") {
    if (bc.forEachVars.empty()) {
      bc.fail(line, "END FOR without matching FOR EACH");
      return;
    }
    bc.locals.erase(bc.forEachVars.back());
    bc.forEachVars.pop_back();
    o << "    }\n  }\n";
    return;
  }
  if (startsWithCI(text, "IN ARENA") || toUpper(text) == "IN ARENA DO") {
    std::string rest = startsWithCI(text, "IN ARENA") ? trim(text.substr(8)) : "";
    stripDo(rest);
    std::string mark = "_luke_m" + std::to_string(++bc.arenaSeq);
    bc.arenaMarks.push_back(mark);
    o << "  {\n";
    o << "    LukeArenaMark " << mark << " = luke_arena_mark(arena);\n";
    return;
  }
  if (toUpper(text) == "END ARENA" || toUpper(text) == "ENDARENA") {
    if (bc.arenaMarks.empty()) {
      bc.fail(line, "END ARENA without matching IN ARENA");
      return;
    }
    std::string mark = bc.arenaMarks.back();
    bc.arenaMarks.pop_back();
    o << "    luke_arena_reset(arena, " << mark << ");\n";
    o << "  }\n";
    return;
  }

  /* Collections — conversational */
  if (startsWithCI(text, "ADD ")) {
    auto rest = trim(text.substr(4));
    auto U = toUpper(rest);
    auto to = U.find(" TO ");
    if (to != std::string::npos) {
      auto val = bc.expr(trim(rest.substr(0, to)), line);
      auto listName = stripThe(trim(rest.substr(to + 4)));
      auto v = bc.coerceText(val);
      if (bc.rxCells.count(listName) && bc.rxCellTy.count(listName) &&
          bc.rxCellTy[listName].k == K::List) {
        bc.usesRx = true;
        o << "  luke_rx_list_add(_luke_rx, _luke_rx_id_" << cIdent(listName) << ", " << v.code
          << ");\n";
        return;
      }
      if (!bc.locals.count(listName) || bc.locals[listName].k != K::List) {
        bc.fail(line, "ADD … TO needs a LIST — declare MY NAME IS " + listName +
                          " AS LIST (or REMEMBER " + listName + " AS LIST)");
        return;
      }
      o << "  luke_list_add(arena, " << cIdent(listName) << ", " << v.code << ");\n";
      return;
    }
  }
  if (startsWithCI(text, "PUT ")) {
    auto rest = trim(text.substr(4));
    auto U = toUpper(rest);
    auto to = U.find(" TO ");
    auto in = U.find(" IN ");
    if (to != std::string::npos && in != std::string::npos && in > to) {
      auto key = bc.expr(trim(rest.substr(0, to)), line);
      auto val = bc.expr(trim(rest.substr(to + 4, in - (to + 4))), line);
      auto mapName = stripThe(trim(rest.substr(in + 4)));
      auto k = bc.coerceText(key);
      auto v = bc.coerceText(val);
      if (bc.rxCells.count(mapName) && bc.rxCellTy.count(mapName) &&
          bc.rxCellTy[mapName].k == K::Map) {
        bc.usesRx = true;
        o << "  luke_rx_map_put(_luke_rx, _luke_rx_id_" << cIdent(mapName) << ", " << k.code
          << ", " << v.code << ");\n";
        return;
      }
      if (!bc.locals.count(mapName) || bc.locals[mapName].k != K::Map) {
        bc.fail(line, "PUT … IN needs a MAP — declare MY NAME IS " + mapName +
                          " AS MAP (or REMEMBER " + mapName + " AS MAP)");
        return;
      }
      o << "  luke_map_put(arena, " << cIdent(mapName) << ", " << k.code << ", " << v.code
        << ");\n";
      return;
    }
  }

  /* ATTEMPT / OTHERWISE / GIVE UP — conversational errors */
  if (toUpper(text) == "ATTEMPT" || toUpper(text) == "ATTEMPT DO" ||
      startsWithCI(text, "ATTEMPT ")) {
    int id = ++bc.attemptSeq;
    std::string lab = "luke_attempt_" + std::to_string(id);
    bc.attemptLabels.push_back(lab);
    bc.attemptHasOtherwise.push_back(false);
    o << "  luke_clear_problem();\n";
    o << "  {\n";
    return;
  }
  if (startsWithCI(text, "OTHERWISE") || toUpper(text) == "ELSE" || startsWithCI(text, "ELSE ")) {
    if (bc.attemptLabels.empty()) {
      /* IF … OTHERWISE / ELSE — close then-branch, open else-branch. */
      o << "  } else {\n";
      return;
    }
    if (bc.attemptHasOtherwise.back()) {
      bc.fail(line, "ATTEMPT already has OTHERWISE — one recovery path only");
      return;
    }
    bc.attemptHasOtherwise.back() = true;
    auto rest = trim(text);
    if (startsWithCI(rest, "OTHERWISE")) rest = trim(rest.substr(9));
    else if (startsWithCI(rest, "ELSE")) rest = trim(rest.substr(4));
    stripDo(rest);
    std::string bind = "problem";
    if (startsWithCI(rest, "WITH ")) {
      bind = trim(rest.substr(5));
      if (bind.empty()) bind = "problem";
    }
    std::string lab = bc.attemptLabels.back();
    o << "    goto " << lab << "_end;\n";
    o << "  " << lab << "_fail:\n";
    o << "    {\n";
    o << "      LukeText " << cIdent(bind) << " = luke_the_problem();\n";
    bc.locals[bind] = Ty::text();
    return;
  }
  if (toUpper(text) == "END ATTEMPT" || toUpper(text) == "ENDATTEMPT") {
    if (bc.attemptLabels.empty()) {
      bc.fail(line, "END ATTEMPT without matching ATTEMPT");
      return;
    }
    std::string lab = bc.attemptLabels.back();
    bool hasO = bc.attemptHasOtherwise.back();
    bc.attemptLabels.pop_back();
    bc.attemptHasOtherwise.pop_back();
    if (hasO) {
      o << "    }\n";
    } else {
      o << "    goto " << lab << "_end;\n";
      o << "  " << lab << "_fail: ;\n";
    }
    o << "  " << lab << "_end: ;\n";
    o << "  }\n";
    return;
  }
  if (startsWithCI(text, "GIVE UP")) {
    if (bc.attemptLabels.empty()) {
      bc.fail(line, "GIVE UP only works inside ATTEMPT … END ATTEMPT");
      return;
    }
    auto rest = trim(text.substr(7));
    Expr msg{"luke_text(\"gave up\")", Ty::text()};
    if (startsWithCI(rest, "WITH ")) {
      msg = bc.coerceText(bc.expr(trim(rest.substr(5)), line));
    } else if (!rest.empty()) {
      msg = bc.coerceText(bc.expr(rest, line));
    }
    std::string lab = bc.attemptLabels.back();
    o << "  luke_set_problem(" << msg.code << ");\n";
    o << "  goto " << lab << "_fail;\n";
    return;
  }

  /* TEST / MAKE SURE */
  if (startsWithCI(text, "TEST ")) {
    auto rest = trim(text.substr(5));
    stripDo(rest);
    auto name = bc.coerceText(bc.expr(rest, line));
    o << "  luke_speak_text(luke_text_concat(arena, luke_text(\"TEST \"), " << name.code
      << "));\n";
    o << "  {\n";
    return;
  }
  if (toUpper(text) == "END TEST" || toUpper(text) == "ENDTEST") {
    o << "  }\n";
    o << "  luke_speak_text(luke_text(\"  ok\"));\n";
    return;
  }
  if (startsWithCI(text, "MAKE SURE ")) {
    auto rest = trim(text.substr(10));
    auto U = toUpper(rest);
    auto eq = U.find(" EQUALS ");
    if (eq == std::string::npos) eq = U.find(" IS EQUAL TO ");
    if (eq == std::string::npos) {
      bc.fail(line, "MAKE SURE needs … EQUALS … — tell me what must match");
      return;
    }
    size_t kwLen = (U.compare(eq, 8, " EQUALS ") == 0) ? 8 : 13;
    auto L = bc.expr(trim(rest.substr(0, eq)), line);
    auto R = bc.expr(trim(rest.substr(eq + kwLen)), line);
    std::string cond;
    if (L.ty.k == K::Text || R.ty.k == K::Text) {
      auto Lt = bc.coerceText(L);
      auto Rt = bc.coerceText(R);
      cond = "luke_text_eq((" + Lt.code + "),(" + Rt.code + "))";
    } else if (bc.isNumeric(L.ty) && bc.isNumeric(R.ty)) {
      Expr Lc = L.ty.k == K::Int ? Expr{"((double)(" + L.code + "))", Ty::num()} : L;
      Expr Rc = R.ty.k == K::Int ? Expr{"((double)(" + R.code + "))", Ty::num()} : R;
      cond = "((" + Lc.code + ") == (" + Rc.code + "))";
    } else {
      bc.expectTy(line, L.ty, R.ty, "MAKE SURE");
      cond = "((" + L.code + ") == (" + R.code + "))";
    }
    o << "  if (!(" << cond << ")) {\n";
    o << "    luke_speak_text(luke_text(\"MAKE SURE failed on line " << line << "\"));\n";
    o << "    exit(1);\n";
    o << "  }\n";
    return;
  }

  /* Browser page ownership — conversational */
  if (startsWithCI(text, "BRING FONT FROM ")) {
    auto raw = trim(text.substr(16));
    auto href = bc.coerceText(bc.expr(raw, line));
    BrowserFont f;
    f.family = "LukeFont";
    f.hrefOrPath = unquoteText(raw);
    f.local = !(startsWithCI(f.hrefOrPath, "http://") || startsWithCI(f.hrefOrPath, "https://"));
    if (f.local) {
      auto slash = f.hrefOrPath.find_last_of("/\\");
      auto base = slash == std::string::npos ? f.hrefOrPath : f.hrefOrPath.substr(slash + 1);
      f.outRelPath = "fonts/" + base;
    }
    bc.pageFonts.push_back(f);
    bc.hasPage = true;
    if (f.local) {
      std::ostringstream face;
      face << "@font-face{font-family:\"" << esc(f.family) << "\";src:url(\"" << esc(f.outRelPath)
           << "\") format(\"woff2\");font-display:swap;}";
      o << "  luke_js_add_style(luke_text(\"" << esc(face.str()) << "\"));\n";
    } else {
      o << "  luke_js_load_font(" << href.code << ");\n";
    }
    return;
  }
  if (startsWithCI(text, "BRING FONT ")) {
    auto rest = trim(text.substr(11));
    auto fr = findOutsideQuotes(rest, toUpper(rest), " FROM ");
    if (fr != std::string::npos) {
      auto famRaw = trim(rest.substr(0, fr));
      auto pathRaw = trim(rest.substr(fr + 6));
      auto href = bc.coerceText(bc.expr(pathRaw, line));
      BrowserFont f;
      f.family = unquoteText(famRaw);
      f.hrefOrPath = unquoteText(pathRaw);
      f.local = !(startsWithCI(f.hrefOrPath, "http://") || startsWithCI(f.hrefOrPath, "https://"));
      bc.pageFonts.push_back(f);
      bc.hasPage = true;
      if (f.local) {
        auto slash = f.hrefOrPath.find_last_of("/\\");
        auto base =
            slash == std::string::npos ? f.hrefOrPath : f.hrefOrPath.substr(slash + 1);
        bc.pageFonts.back().outRelPath = "fonts/" + base;
        std::ostringstream face;
        face << "@font-face{font-family:\"" << esc(f.family) << "\";src:url(\""
             << esc(bc.pageFonts.back().outRelPath)
             << "\") format(\"woff2\");font-display:swap;}";
        o << "  luke_js_add_style(luke_text(\"" << esc(face.str()) << "\"));\n";
      } else {
        o << "  luke_js_load_font(" << href.code << ");\n";
      }
      return;
    }
  }
  if (startsWithCI(text, "WEAR STYLE ")) {
    auto raw = trim(text.substr(11));
    auto css = bc.coerceText(bc.expr(raw, line));
    auto cssText = unquoteText(raw);
    if (!cssText.empty()) {
      if (!bc.pageStyle.empty()) bc.pageStyle += "\n";
      bc.pageStyle += cssText;
      bc.hasPage = true;
    }
    o << "  luke_js_add_style(" << css.code << ");\n";
    return;
  }
  if (startsWithCI(text, "NAME THE PAGE ")) {
    auto raw = trim(text.substr(14));
    auto title = bc.coerceText(bc.expr(raw, line));
    bc.pageTitle = unquoteText(raw);
    bc.hasPage = true;
    o << "  luke_js_set_title(" << title.code << ");\n";
    return;
  }
  if (startsWithCI(text, "FILL ") && findOutsideQuotes(text, toUpper(text), " WITH ") != std::string::npos) {
    auto rest = trim(text.substr(5));
    auto w = findOutsideQuotes(rest, toUpper(rest), " WITH ");
    auto idRaw = trim(rest.substr(0, w));
    auto htmlRaw = trim(rest.substr(w + 6));
    auto id = bc.coerceText(bc.expr(idRaw, line));
    auto html = bc.coerceText(bc.expr(htmlRaw, line));
    if (unquoteText(idRaw) == "root") {
      bc.pageBody = unquoteText(htmlRaw);
      bc.hasPage = true;
    }
    o << "  luke_js_set_html(" << id.code << ", " << html.code << ");\n";
    return;
  }

  /* Production web — hash routing + async fetch */
  if (startsWithCI(text, "GO TO ")) {
    auto path = bc.coerceText(bc.expr(trim(text.substr(6)), line));
    o << "  luke_js_route_go(" << path.code << ");\n";
    return;
  }
  if (startsWithCI(text, "START FETCH ")) {
    auto rest = trim(text.substr(12));
    auto U = toUpper(rest);
    /* START FETCH "job" GET "url" [INTO body] [STATUS st] [READY ready]
     * START FETCH "job" POST "url" [WITH body] [INTO …] [STATUS …] [READY …] */
    auto getPos = findOutsideQuotes(rest, U, " GET ");
    auto postPos = findOutsideQuotes(rest, U, " POST ");
    bool isPost = postPos != std::string::npos && (getPos == std::string::npos || postPos < getPos);
    size_t methPos = isPost ? postPos : getPos;
    if (methPos == std::string::npos) {
      bc.fail(line, "START FETCH needs GET or POST — START FETCH \"job\" GET \"url\"");
      return;
    }
    auto idRaw = trim(rest.substr(0, methPos));
    auto idE = bc.coerceText(bc.expr(idRaw, line));
    auto after = trim(rest.substr(methPos + (isPost ? 6 : 5)));
    auto aU = toUpper(after);
    size_t withPos = isPost ? findOutsideQuotes(after, aU, " WITH ") : std::string::npos;
    size_t intoPos = findOutsideQuotes(after, aU, " INTO ");
    size_t statusPos = findOutsideQuotes(after, aU, " STATUS ");
    size_t readyPos = findOutsideQuotes(after, aU, " READY ");

    size_t firstClause = std::string::npos;
    auto consider = [&](size_t p) {
      if (p != std::string::npos && (firstClause == std::string::npos || p < firstClause))
        firstClause = p;
    };
    consider(withPos);
    consider(intoPos);
    consider(statusPos);
    consider(readyPos);

    auto urlPart = firstClause == std::string::npos ? after : trim(after.substr(0, firstClause));
    auto urlE = bc.coerceText(bc.expr(urlPart, line));
    Expr bodyE{"luke_text(\"\")", Ty::text()};
    if (withPos != std::string::npos) {
      size_t bEnd = after.size();
      auto limit = [&](size_t p) {
        if (p != std::string::npos && p > withPos && p < bEnd) bEnd = p;
      };
      limit(intoPos);
      limit(statusPos);
      limit(readyPos);
      bodyE = bc.coerceText(bc.expr(trim(after.substr(withPos + 6, bEnd - (withPos + 6))), line));
    }

    auto parseCellRef = [&](size_t pos, size_t keyLen, const char *clause,
                            bool wantText) -> std::string {
      if (pos == std::string::npos) return {};
      size_t start = pos + keyLen;
      size_t end = after.size();
      auto limit = [&](size_t p) {
        if (p != std::string::npos && p > pos && p < end) end = p;
      };
      limit(intoPos);
      limit(statusPos);
      limit(readyPos);
      limit(withPos);
      auto name = stripThe(trim(after.substr(start, end - start)));
      if (name.empty()) {
        bc.fail(line, std::string("START FETCH ") + clause + " needs a cell name");
        return {};
      }
      if (!bc.rxCells.count(name)) {
        bc.fail(line, std::string("START FETCH ") + clause + " '" + name +
                          "' — REMEMBER it first as a reactive cell");
        return {};
      }
      Ty ty = bc.rxCellTy.count(name) ? bc.rxCellTy[name] : Ty::num();
      if (wantText)
        bc.expectTy(line, ty, Ty::text(), std::string("START FETCH ") + clause);
      else if (ty.k != K::Num && ty.k != K::Flag && ty.k != K::Int)
        bc.fail(line, std::string("START FETCH ") + clause + " wants NUMBER/INTEGER/FLAG cell");
      return name;
    };

    std::string intoCell = parseCellRef(intoPos, 6, "INTO", true);
    if (bc.bad) return;
    std::string statusCell = parseCellRef(statusPos, 8, "STATUS", false);
    if (bc.bad) return;
    std::string readyCell = parseCellRef(readyPos, 7, "READY", false);
    if (bc.bad) return;

    if (!intoCell.empty() || !statusCell.empty() || !readyCell.empty()) {
      std::string jobLit = unquoteText(idRaw);
      if (jobLit.empty()) jobLit = idRaw;
      bc.rxFetchBinds.push_back({jobLit, intoCell, statusCell, readyCell, line});
      bc.usesRx = true;
    }

    o << "  luke_js_fetch_start(" << idE.code << ", luke_text(\"" << (isPost ? "POST" : "GET")
      << "\"), " << urlE.code << ", " << bodyE.code << ");\n";
    if (!readyCell.empty()) {
      Ty rty = bc.rxCellTy.count(readyCell) ? bc.rxCellTy[readyCell] : Ty::num();
      if (rty.k == K::Int)
        o << "  luke_rx_write_int(_luke_rx, _luke_rx_id_" << cIdent(readyCell) << ", 0);\n";
      else
        o << "  luke_rx_write_num(_luke_rx, _luke_rx_id_" << cIdent(readyCell) << ", 0);\n";
    }
    return;
  }

  /* Live Graph WATCH:
   *   Server: WATCH user FROM db AS "SELECT …" | WHERE "id = 1"
   *   Client: WATCH user FROM "http://…/watch"  [READY ready]
   * Client form is sugar over START SUBSCRIBE (job id = cell name). */
  if (startsWithCI(text, "WATCH ")) {
    auto rest = trim(text.substr(6));
    stripDo(rest);
    auto U = toUpper(rest);
    auto fromPos = findOutsideQuotes(rest, U, " FROM ");
    if (fromPos == std::string::npos) {
      bc.fail(line, "WATCH needs FROM — WATCH user FROM db WHERE \"id = 1\" "
                    "or WATCH user FROM \"http://…/watch\"");
      return;
    }
    auto cellName = stripThe(trim(rest.substr(0, fromPos)));
    if (cellName.empty()) {
      bc.fail(line, "WATCH needs a cell name — WATCH user FROM …");
      return;
    }
    auto after = trim(rest.substr(fromPos + 6));
    auto aU = toUpper(after);
    bool scopedToUser = false;
    {
      auto forUser = aU.rfind(" FOR CURRENT USER");
      if (forUser != std::string::npos) {
        scopedToUser = true;
        after = trim(after.substr(0, forUser));
        aU = toUpper(after);
      }
    }
    size_t asPos = findOutsideQuotes(after, aU, " AS ");
    size_t wherePos = findOutsideQuotes(after, aU, " WHERE ");
    size_t readyPos = findOutsideQuotes(after, aU, " READY ");

    /* First token after FROM — if unquoted identifier + AS/WHERE → server DB watch. */
    size_t dbEnd = after.size();
    auto limitDb = [&](size_t p) {
      if (p != std::string::npos && p < dbEnd) dbEnd = p;
    };
    limitDb(asPos);
    limitDb(wherePos);
    limitDb(readyPos);
    auto dbName = stripThe(trim(after.substr(0, dbEnd)));
    bool dbWatch = !dbName.empty() && dbName[0] != '"' &&
                   (asPos != std::string::npos || wherePos != std::string::npos);

    if (dbWatch) {
      if (!bc.locals.count(dbName) || bc.locals[dbName].k != K::Ptr ||
          bc.locals[dbName].klass != "__Db") {
        bc.fail(line, "WATCH FROM needs a DATABASE — MY NAME IS " + dbName + " AS DATABASE");
        return;
      }
      std::string sql;
      std::string baseTable;
      std::string baseTable2;
      std::string wherePred; /* only set for IVM-capable shapes */
      std::string ivmColExpr = "name";
      bool hasJoin = false;
      /* Join chain: hop[0] is FROM table; hop[i>0] carries ON equi linking prior hops. */
      struct LukeJoinHop {
        std::string table;
        std::string alias;
        std::string onLeftAlias, onLeftCol, onRightAlias, onRightCol;
      };
      std::vector<LukeJoinHop> joinHops;
      std::string joinA1, joinA2, joinOn; /* convenience for 2-table paths */
      if (asPos != std::string::npos) {
        auto sqlRaw = trim(after.substr(asPos + 4));
        sql = unquoteText(sqlRaw);
        if (sql.empty()) sql = sqlRaw;

        /* IVM extraction:
         *   SELECT <col> FROM <table> WHERE <pred>
         *   SELECT <col> FROM t1 a1 JOIN t2 a2 ON … [JOIN t3 a3 ON …] WHERE <pred>
         */
        auto sU = toUpper(sql);
        size_t selPos = sU.find("SELECT ");
        size_t fromPos2 = sU.find(" FROM ");
        size_t wherePos2 = sU.find(" WHERE ");
        if (selPos != std::string::npos && fromPos2 != std::string::npos &&
            wherePos2 != std::string::npos && fromPos2 > selPos && wherePos2 > fromPos2) {
          auto selExpr = trim(sql.substr(selPos + 7, fromPos2 - (selPos + 7)));
          auto predExpr = trim(sql.substr(wherePos2 + 7));
          if (!predExpr.empty() && predExpr.back() == ';') predExpr.pop_back();

          auto firstTok = [](const std::string &s) {
            size_t i = 0;
            while (i < s.size() && !isspace((unsigned char)s[i])) ++i;
            return trim(s.substr(0, i));
          };
          auto parseTblAlias = [&](const std::string &clause, std::string &tbl,
                                   std::string &alias) {
            auto t = firstTok(clause);
            if (t.empty() || t.find('.') != std::string::npos) return false;
            tbl = t;
            auto rest = trim(clause.substr(t.size()));
            if (startsWithCI(rest, "AS ")) rest = trim(rest.substr(3));
            alias = firstTok(rest);
            if (alias.empty() || toUpper(alias) == "ON" || toUpper(alias) == "JOIN" ||
                toUpper(alias) == "INNER")
              alias = tbl;
            return true;
          };
          auto splitEqDot = [](const std::string &onExpr, std::string &la, std::string &lc,
                               std::string &ra, std::string &rc) {
            size_t eq = onExpr.find('=');
            if (eq == std::string::npos) return false;
            auto L = trim(onExpr.substr(0, eq));
            auto R = trim(onExpr.substr(eq + 1));
            size_t d1 = L.find('.'), d2 = R.find('.');
            if (d1 == std::string::npos || d2 == std::string::npos) return false;
            la = trim(L.substr(0, d1));
            lc = trim(L.substr(d1 + 1));
            ra = trim(R.substr(0, d2));
            rc = trim(R.substr(d2 + 1));
            return !la.empty() && !lc.empty() && !ra.empty() && !rc.empty();
          };

          std::vector<size_t> joinStarts;
          std::vector<size_t> joinKwLens;
          for (size_t p = fromPos2; p < wherePos2;) {
            size_t j = sU.find(" JOIN ", p);
            size_t ij = sU.find(" INNER JOIN ", p);
            size_t best = std::string::npos;
            size_t kw = 6;
            if (j != std::string::npos && j < wherePos2) {
              best = j;
              kw = 6;
            }
            if (ij != std::string::npos && ij < wherePos2 &&
                (best == std::string::npos || ij < best)) {
              best = ij;
              kw = 12;
            }
            if (best == std::string::npos) break;
            joinStarts.push_back(best);
            joinKwLens.push_back(kw);
            p = best + kw;
          }

          if (!joinStarts.empty()) {
            auto fromClause = trim(sql.substr(fromPos2 + 6, joinStarts[0] - (fromPos2 + 6)));
            LukeJoinHop h0;
            bool ok = parseTblAlias(fromClause, h0.table, h0.alias);
            if (ok) {
              joinHops.push_back(h0);
              for (size_t ji = 0; ji < joinStarts.size(); ++ji) {
                size_t segStart = joinStarts[ji] + joinKwLens[ji];
                size_t segEnd = (ji + 1 < joinStarts.size()) ? joinStarts[ji + 1] : wherePos2;
                auto seg = trim(sql.substr(segStart, segEnd - segStart));
                auto onPos = toUpper(seg).find(" ON ");
                if (onPos == std::string::npos) {
                  ok = false;
                  break;
                }
                LukeJoinHop h;
                if (!parseTblAlias(trim(seg.substr(0, onPos)), h.table, h.alias)) {
                  ok = false;
                  break;
                }
                if (!splitEqDot(trim(seg.substr(onPos + 4)), h.onLeftAlias, h.onLeftCol,
                                h.onRightAlias, h.onRightCol)) {
                  ok = false;
                  break;
                }
                joinHops.push_back(h);
              }
            }
            if (!ok || joinHops.size() < 2) {
              joinHops.clear();
            } else {
              baseTable = joinHops[0].table;
              baseTable2 = joinHops[1].table;
              wherePred = predExpr;
              ivmColExpr = selExpr;
              hasJoin = true;
              joinA1 = joinHops[0].alias;
              joinA2 = joinHops[1].alias;
              joinOn = joinHops[1].onLeftAlias + "." + joinHops[1].onLeftCol + " = " +
                       joinHops[1].onRightAlias + "." + joinHops[1].onRightCol;
            }
          } else {
            auto tblExpr = trim(sql.substr(fromPos2 + 6, wherePos2 - (fromPos2 + 6)));
            bool baseOk = !tblExpr.empty() && tblExpr.find(' ') == std::string::npos &&
                           tblExpr.find('\t') == std::string::npos &&
                           tblExpr.find('.') == std::string::npos;
            if (baseOk) {
              baseTable = tblExpr;
              wherePred = predExpr;
              ivmColExpr = selExpr;
            }
          }
        }
      } else {
        /* WATCH user FROM db WHERE "id = 1" → SELECT name FROM users WHERE id = 1 */
        auto predRaw = trim(after.substr(wherePos + 7));
        wherePred = unquoteText(predRaw);
        if (wherePred.empty()) wherePred = predRaw;
        if (wherePred.empty()) {
          bc.fail(line, "WATCH WHERE needs a predicate — WHERE \"id = 1\"");
          return;
        }
        std::string table = cellName;
        if (table.empty() || table.back() != 's') table += "s";
        baseTable = table;
        ivmColExpr = "name";
        sql = "SELECT name FROM " + table + " WHERE " + wherePred;
      }
      if (sql.empty()) {
        bc.fail(line, "WATCH needs AS \"SELECT …\" or WHERE \"…\"");
        return;
      }
      std::string key = rxScopedCellName(bc.rxEntityStack, cellName);
      auto sanitizeSqlIdent = [&](const std::string &s) -> std::string {
        std::string out;
        for (char c : s) {
          if (isalnum((unsigned char)c) || c == '_')
            out.push_back(c);
          else
            out.push_back('_');
        }
        if (out.empty()) out = "x";
        return out;
      };

      std::string readSql = sql;
      std::string ivmTable;
      std::string ivmValueSql;
      std::string eventLog;
      if (scopedToUser) {
        /* Per-user Live Graph: no shared IVM cache (would leak across tenants).
         * SQL should use ? for user_id; we bind THE CURRENT USER at read/push time. */
        if (sql.find('?') == std::string::npos) {
          bc.fail(line, "WATCH … FOR CURRENT USER needs a ? bind for user_id — "
                        "AS \"SELECT … WHERE user_id = ?\"");
          return;
        }
        BC::RxQueryDef qd;
        qd.name = key;
        qd.dbLocal = dbName;
        qd.sql = sql;
        qd.readSql = sql;
        qd.hasIvm = false;
        qd.scopedToUser = true;
        qd.line = line;
        bc.rxQueryDefs.push_back(qd);

        bc.usesRx = true;
        bc.locals[cellName] = Ty::text();
        bc.rxCellTy[key] = Ty::text();
        if (!bc.rxCells.count(key)) bc.rxCellOrder.push_back(key);
        bc.rxCells[key] = true;
        /* Auth-as-types: FOR CURRENT USER unlocks SECRET binds for this cell. */
        bc.rxScopedSecretOk[key] = true;
        bc.rxScopedSecretOk[cellName] = true;
        emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(key),
                          "luke_rx_cell_text(_luke_rx, luke_text(\"\"))", key);
        o << "  {\n";
        o << "    LukeText _luke_uid = luke_auth_current_user();\n";
        o << "    if (_luke_uid.len) {\n";
        o << "      LukeList *_luke_ub = luke_list_new(arena);\n";
        o << "      luke_list_add(arena, _luke_ub, _luke_uid);\n";
        o << "      luke_rx_write_text(_luke_rx, _luke_rx_id_" << cIdent(key)
          << ", luke_db_query_bind_text(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(sql)
          << "\"), _luke_ub));\n";
        if (bc.rxSecretCells.count(key) || bc.rxSecretCells.count(cellName)) {
          o << "      luke_auth_mark_saw(arena, luke_text(\"" << esc(key) << "\"));\n";
        }
        o << "    }\n";
        o << "  }\n";
        return;
      }
      /* Auth-as-types: SECRET cells must be WATCH'd with FOR CURRENT USER. */
      if (bc.rxSecretCells.count(key) || bc.rxSecretCells.count(cellName)) {
        bc.fail(line, "SECRET '" + cellName + "' requires WATCH … FOR CURRENT USER — "
                      "unauthorized access is a compile error");
        return;
      }
      if (!baseTable.empty()) {
        /* Differential IVM cache + causal event log for SSE resume. */
        ivmTable = "luke_ivm_" + sanitizeSqlIdent(key);
        eventLog = "luke_ivm_log_" + sanitizeSqlIdent(key);
        readSql = "SELECT v FROM " + ivmTable + " WHERE k = 1";
        if (hasJoin) {
          /* Keep the original JOIN SELECT as the maintained scalar. */
          ivmValueSql = sql;
        } else {
          /* char(10) = newline — avoids C-string \\n vs SQL separator confusion. */
          ivmValueSql = "SELECT group_concat(" + ivmColExpr + ", char(10)) FROM " + baseTable +
                         " WHERE " + wherePred;
        }

        /* Prefer NEW/OLD differential triggers for simple single-table `id = N`. */
        bool simpleCol = !hasJoin && !ivmColExpr.empty() &&
                         ivmColExpr.find(' ') == std::string::npos &&
                         ivmColExpr.find('(') == std::string::npos &&
                         ivmColExpr.find('.') == std::string::npos;
        auto wU = toUpper(wherePred);
        bool idPred = wU.find("ID =") != std::string::npos || wU.find("ID=") != std::string::npos;
        bool hasDiff = simpleCol && idPred;

        /* Keyed equi-join differential: WHERE a.col = lit + ON a.x = b.y
         * → WHEN-gated triggers that probe the partner row via NEW, not full recompute. */
        bool hasJoinDiff = false;
        std::string jCol1, jCol2; /* join columns on a1 / a2 */
        std::string whereAlias, whereCol, whereLit;
        std::string whenT1, whenT2, whenT1Old, whenT2Old;
        std::string valSqlT1, valSqlT2;
        if (hasJoin && !joinOn.empty() && !joinA1.empty() && !joinA2.empty()) {
          auto splitEq = [](const std::string &s, std::string &L, std::string &R) {
            size_t eq = s.find('=');
            if (eq == std::string::npos) return false;
            L = trim(s.substr(0, eq));
            R = trim(s.substr(eq + 1));
            return !L.empty() && !R.empty();
          };
          auto splitDot = [](const std::string &s, std::string &a, std::string &c) {
            size_t d = s.find('.');
            if (d == std::string::npos) return false;
            a = trim(s.substr(0, d));
            c = trim(s.substr(d + 1));
            return !a.empty() && !c.empty();
          };
          std::string onL, onR, onLa, onLc, onRa, onRc;
          if (splitEq(joinOn, onL, onR) && splitDot(onL, onLa, onLc) &&
              splitDot(onR, onRa, onRc)) {
            if (toUpper(onLa) == toUpper(joinA1) && toUpper(onRa) == toUpper(joinA2)) {
              jCol1 = onLc;
              jCol2 = onRc;
            } else if (toUpper(onLa) == toUpper(joinA2) && toUpper(onRa) == toUpper(joinA1)) {
              jCol1 = onRc;
              jCol2 = onLc;
            }
          }
          std::string wL, wR;
          if (!jCol1.empty() && splitEq(wherePred, wL, wR)) {
            std::string wa, wc;
            if (splitDot(wL, wa, wc)) {
              whereAlias = wa;
              whereCol = wc;
              whereLit = wR;
            } else if (splitDot(wR, wa, wc)) {
              whereAlias = wa;
              whereCol = wc;
              whereLit = wL;
            }
          }
          auto rewriteAliasQual = [](const std::string &expr, const std::string &alias,
                                     const std::string &qual) {
            std::string out;
            std::string aU = toUpper(alias);
            for (size_t i = 0; i < expr.size();) {
              bool atBound =
                (i == 0 || (!isalnum((unsigned char)expr[i - 1]) && expr[i - 1] != '_'));
            if (atBound && i + alias.size() < expr.size() && expr[i + alias.size()] == '.') {
                bool match = true;
                for (size_t k = 0; k < alias.size(); ++k) {
                  if ((char)std::toupper((unsigned char)expr[i + k]) != aU[k]) {
                    match = false;
                    break;
                  }
                }
                if (match) {
                  out += qual;
                  i += alias.size() + 1; /* skip alias. */
                  continue;
                }
              }
              out.push_back(expr[i]);
              ++i;
            }
            return out;
          };
          if (!jCol1.empty() && !whereAlias.empty() && !whereLit.empty() &&
              joinHops.size() <= 2) {
            bool whereOn1 = toUpper(whereAlias) == toUpper(joinA1);
            bool whereOn2 = toUpper(whereAlias) == toUpper(joinA2);
            /* Filter must be on a join key so we can map WHEN to both sides. */
            bool keyOk = (whereOn1 && toUpper(whereCol) == toUpper(jCol1)) ||
                         (whereOn2 && toUpper(whereCol) == toUpper(jCol2));
            if (keyOk) {
              std::string lit = whereLit;
              if (whereOn1) {
                whenT1 = "NEW." + whereCol + " = " + lit;
                whenT1Old = "OLD." + whereCol + " = " + lit;
                whenT2 = "NEW." + jCol2 + " = " + lit;
                whenT2Old = "OLD." + jCol2 + " = " + lit;
              } else {
                whenT2 = "NEW." + whereCol + " = " + lit;
                whenT2Old = "OLD." + whereCol + " = " + lit;
                whenT1 = "NEW." + jCol1 + " = " + lit;
                whenT1Old = "OLD." + jCol1 + " = " + lit;
              }
              std::string selT1 = rewriteAliasQual(ivmColExpr, joinA1, "NEW.");
              std::string selT2 = rewriteAliasQual(ivmColExpr, joinA2, "NEW.");
              valSqlT1 = "SELECT " + selT1 + " FROM " + baseTable2 + " " + joinA2 + " WHERE " +
                         joinA2 + "." + jCol2 + " = NEW." + jCol1;
              valSqlT2 = "SELECT " + selT2 + " FROM " + baseTable + " " + joinA1 + " WHERE " +
                         joinA1 + "." + jCol1 + " = NEW." + jCol2;
              hasJoinDiff = true;
              hasDiff = true; /* headline: true differential, not recompute-both */
            }
          }
        }

        /* 3+ table equi-JOIN chain differential: propagate WHERE a.col = lit
         * through ON equalities so every table gets a WHEN NEW.<boundCol> = lit
         * (or EXISTS back-probe) and a NEW-pinned SELECT over the remaining joins. */
        bool hasJoinChainDiff = false;
        struct ChainTrig {
          std::string table;
          std::string suffix;
          std::string whenNew;
          std::string whenOld;
          std::string valSql;
        };
        std::vector<ChainTrig> chainTrigs;
        if (!hasJoinDiff && joinHops.size() >= 3 && !whereAlias.empty() && !whereLit.empty()) {
          struct BoundCol {
            std::string alias;
            std::string col;
          };
          std::vector<BoundCol> bound;
          bound.push_back({whereAlias, whereCol});
          auto same = [](const std::string &a, const std::string &b) {
            return toUpper(a) == toUpper(b);
          };
          /* Closure over ON equalities. Copy BoundCol by value — push_back must
           * not invalidate a reference held across the second side check. */
          bool grew = true;
          while (grew) {
            grew = false;
            for (size_t hi = 1; hi < joinHops.size(); ++hi) {
              auto &h = joinHops[hi];
              size_t nBound = bound.size();
              for (size_t bi = 0; bi < nBound; ++bi) {
                BoundCol b = bound[bi];
                if (same(b.alias, h.onLeftAlias) && same(b.col, h.onLeftCol)) {
                  bool have = false;
                  for (auto &x : bound)
                    if (same(x.alias, h.onRightAlias) && same(x.col, h.onRightCol)) have = true;
                  if (!have) {
                    bound.push_back({h.onRightAlias, h.onRightCol});
                    grew = true;
                  }
                }
                if (same(b.alias, h.onRightAlias) && same(b.col, h.onRightCol)) {
                  bool have = false;
                  for (auto &x : bound)
                    if (same(x.alias, h.onLeftAlias) && same(x.col, h.onLeftCol)) have = true;
                  if (!have) {
                    bound.push_back({h.onLeftAlias, h.onLeftCol});
                    grew = true;
                  }
                }
              }
            }
          }
          auto rewriteAliasQual = [](const std::string &expr, const std::string &alias,
                                     const std::string &qual) {
            std::string out;
            std::string aU = toUpper(alias);
            for (size_t i = 0; i < expr.size();) {
              bool atBound =
                  (i == 0 || (!isalnum((unsigned char)expr[i - 1]) && expr[i - 1] != '_'));
              if (atBound && i + alias.size() < expr.size() && expr[i + alias.size()] == '.') {
                bool match = true;
                for (size_t k = 0; k < alias.size(); ++k) {
                  if ((char)std::toupper((unsigned char)expr[i + k]) != aU[k]) {
                    match = false;
                    break;
                  }
                }
                if (match) {
                  out += qual;
                  i += alias.size() + 1;
                  continue;
                }
              }
              out.push_back(expr[i]);
              ++i;
            }
            return out;
          };
          auto boundFor = [&](const std::string &alias) -> const BoundCol * {
            for (auto &b : bound)
              if (same(b.alias, alias)) return &b;
            return nullptr;
          };
          bool allBound = true;
          for (auto &h : joinHops)
            if (!boundFor(h.alias)) allBound = false;
          if (allBound) {
            for (size_t hi = 0; hi < joinHops.size(); ++hi) {
              auto &h = joinHops[hi];
              auto *bc_ = boundFor(h.alias);
              ChainTrig ct;
              ct.table = h.table;
              ct.suffix = "_jd_t" + std::to_string(hi + 1);
              ct.whenNew = "NEW." + bc_->col + " = " + whereLit;
              ct.whenOld = "OLD." + bc_->col + " = " + whereLit;
              std::string sel = rewriteAliasQual(ivmColExpr, h.alias, "NEW.");
              /* FROM all tables except the firing hop; ON refs to the firing
               * alias become NEW.<col>. Pin every ON edge that touches NEW. */
              std::ostringstream fr;
              bool started = false;
              for (size_t oi = 0; oi < joinHops.size(); ++oi) {
                if (oi == hi) continue;
                if (!started) {
                  fr << joinHops[oi].table << " " << joinHops[oi].alias;
                  started = true;
                } else {
                  auto &oh = joinHops[oi];
                  std::string lref = same(oh.onLeftAlias, h.alias)
                                        ? ("NEW." + oh.onLeftCol)
                                        : (oh.onLeftAlias + "." + oh.onLeftCol);
                  std::string rref = same(oh.onRightAlias, h.alias)
                                        ? ("NEW." + oh.onRightCol)
                                        : (oh.onRightAlias + "." + oh.onRightCol);
                  fr << " JOIN " << oh.table << " " << oh.alias << " ON " << lref << " = "
                     << rref;
                }
              }
              std::string valFrom = fr.str();
              std::string whereSql;
              for (size_t oi = 1; oi < joinHops.size(); ++oi) {
                auto &oh = joinHops[oi];
                std::string pin;
                if (same(oh.onLeftAlias, h.alias))
                  pin = oh.onRightAlias + "." + oh.onRightCol + " = NEW." + oh.onLeftCol;
                else if (same(oh.onRightAlias, h.alias))
                  pin = oh.onLeftAlias + "." + oh.onLeftCol + " = NEW." + oh.onRightCol;
                if (pin.empty()) continue;
                /* Skip pins whose remaining alias was itself rewritten into ON
                 * and is not present as a table alias in FROM — still emit; the
                 * partner alias is always among the remaining hops. */
                if (!whereSql.empty()) whereSql += " AND ";
                whereSql += pin;
              }
              if (whereSql.empty())
                whereSql = std::string("NEW.") + bc_->col + " = " + whereLit;
              if (!same(whereAlias, h.alias))
                whereSql += " AND " + whereAlias + "." + whereCol + " = " + whereLit;
              else
                whereSql += " AND NEW." + whereCol + " = " + whereLit;
              ct.valSql = "SELECT " + sel + " FROM " + valFrom + " WHERE " + whereSql;
              chainTrigs.push_back(ct);
            }
            if (chainTrigs.size() == joinHops.size()) {
              hasJoinChainDiff = true;
              hasDiff = true;
            } else {
              chainTrigs.clear();
            }
          }
        }

        BC::RxQueryDef qd;
        qd.name = key;
        qd.dbLocal = dbName;
        qd.sql = sql;
        qd.readSql = readSql;
        qd.baseTable = baseTable;
        qd.baseTable2 = baseTable2;
        qd.ivmTable = ivmTable;
        qd.ivmValueSql = ivmValueSql;
        qd.eventLog = eventLog;
        qd.wherePred = wherePred;
        qd.ivmColExpr = ivmColExpr;
        qd.hasIvm = true;
        qd.hasDiffTrig = hasDiff;
        qd.hasJoin = hasJoin;
        qd.line = line;
        bc.rxQueryDefs.push_back(qd);

        std::string createTbl = "CREATE TABLE IF NOT EXISTS " + ivmTable +
                                 "(k INTEGER PRIMARY KEY, v TEXT)";
        std::string createLog = "CREATE TABLE IF NOT EXISTS " + eventLog +
                                 "(seq INTEGER PRIMARY KEY AUTOINCREMENT, v TEXT)";
        std::string initRow = "INSERT OR REPLACE INTO " + ivmTable +
                               "(k, v) VALUES(1, (" + ivmValueSql + "))";

        auto emitRecomputeTrigs = [&](const std::string &table, const std::string &suffix) {
          std::string ai = "CREATE TRIGGER IF NOT EXISTS " + ivmTable + suffix +
                           "_ai AFTER INSERT ON " + table + " BEGIN " +
                           "INSERT OR REPLACE INTO " + ivmTable +
                           "(k, v) VALUES(1, (" + ivmValueSql + ")); END";
          std::string au = "CREATE TRIGGER IF NOT EXISTS " + ivmTable + suffix +
                           "_au AFTER UPDATE ON " + table + " BEGIN " +
                           "INSERT OR REPLACE INTO " + ivmTable +
                           "(k, v) VALUES(1, (" + ivmValueSql + ")); END";
          std::string ad = "CREATE TRIGGER IF NOT EXISTS " + ivmTable + suffix +
                           "_ad AFTER DELETE ON " + table + " BEGIN " +
                           "INSERT OR REPLACE INTO " + ivmTable +
                           "(k, v) VALUES(1, (" + ivmValueSql + ")); END";
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(ai) << "\"));\n";
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(au) << "\"));\n";
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(ad) << "\"));\n";
        };

        auto emitJoinDiffTrigs = [&](const std::string &table, const std::string &suffix,
                                     const std::string &whenNew, const std::string &whenOld,
                                     const std::string &valSql) {
          std::string ai = "CREATE TRIGGER IF NOT EXISTS " + ivmTable + suffix +
                           "_ai AFTER INSERT ON " + table + " WHEN " + whenNew + " BEGIN " +
                           "INSERT OR REPLACE INTO " + ivmTable + "(k, v) VALUES(1, (" + valSql +
                           ")); END";
          std::string au = "CREATE TRIGGER IF NOT EXISTS " + ivmTable + suffix +
                           "_au AFTER UPDATE ON " + table + " WHEN " + whenNew + " BEGIN " +
                           "INSERT OR REPLACE INTO " + ivmTable + "(k, v) VALUES(1, (" + valSql +
                           ")); END";
          std::string ad = "CREATE TRIGGER IF NOT EXISTS " + ivmTable + suffix +
                           "_ad AFTER DELETE ON " + table + " WHEN " + whenOld + " BEGIN " +
                           "INSERT OR REPLACE INTO " + ivmTable + "(k, v) VALUES(1, (" +
                           ivmValueSql + ")); END";
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(ai) << "\"));\n";
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(au) << "\"));\n";
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(ad) << "\"));\n";
        };

        /* Qualify every bare column identifier with NEW./OLD. for trigger WHEN/bodies.
         * Handles equality, inequality, LIKE, BETWEEN, AND/OR, etc. Leaves alias.col,
         * SQL keywords, and string/numeric literals untouched. */
        auto qualifyBarePred = [](const std::string &pred, const char *qual) {
          static const char *kws[] = {"AND",   "OR",     "NOT",    "LIKE",  "BETWEEN", "IN",
                                      "IS",    "NULL",   "TRUE",   "FALSE", "GLOB",    "MATCH",
                                      "REGEXP","ESCAPE", "CASE",   "WHEN",  "THEN",    "ELSE",
                                      "END",   "COLLATE","ASC",    "DESC",  "IFNULL",  "COALESCE",
                                      "CAST",  "AS",     "NULL",   nullptr};
          std::string out;
          for (size_t i = 0; i < pred.size();) {
            unsigned char c = (unsigned char)pred[i];
            if (c == '\'' || c == '"') {
              char q = pred[i++];
              out.push_back(q);
              while (i < pred.size()) {
                out.push_back(pred[i]);
                if (pred[i] == q) {
                  ++i;
                  break;
                }
                ++i;
              }
              continue;
            }
            if (std::isalpha(c) || c == '_') {
              size_t s = i;
              while (i < pred.size() &&
                     (std::isalnum((unsigned char)pred[i]) || pred[i] == '_'))
                ++i;
              std::string tok = pred.substr(s, i - s);
              if (i < pred.size() && pred[i] == '.') {
                out += tok; /* alias.col — do not qualify the alias */
                continue;
              }
              /* Function call (identifier immediately followed by '(') is not a
               * column — qualifying it (NEW.substr(...)) yields invalid SQL whose
               * trigger silently never fires, leaving the cell stale. Leave it. */
              {
                size_t j = i;
                while (j < pred.size() && std::isspace((unsigned char)pred[j])) ++j;
                if (j < pred.size() && pred[j] == '(') {
                  out += tok;
                  continue;
                }
              }
              std::string tu = toUpper(tok);
              bool kw = false;
              for (int ki = 0; kws[ki]; ++ki)
                if (tu == kws[ki]) {
                  kw = true;
                  break;
                }
              if (kw)
                out += tok;
              else
                out += std::string(qual) + tok;
              continue;
            }
            out.push_back(pred[i++]);
          }
          return out;
        };

        /* Single-table bag: any filter whose columns can be NEW./OLD.-qualified.
         * (Previously only `col = lit` — inequalities fell back to full recompute.)
         * A subquery predicate (`… IN (SELECT …)`, correlated EXISTS) references
         * other rows, so a per-row NEW./OLD. rewrite is unsound — those fall back
         * to the correct full-recompute path instead of a silently-wrong bag. */
        bool predHasSubquery = toUpper(wherePred).find("SELECT") != std::string::npos;
        bool bagAgg = !hasJoin && simpleCol && !hasDiff && !ivmColExpr.empty() &&
                      !wherePred.empty() && !predHasSubquery;

        /* Multi-row equi-JOIN bag: non-point joins maintain group_concat of join
         * rows keyed by participating rowids — not a scalar subquery / full recompute. */
        bool joinBag = hasJoin && !hasJoinDiff && !hasJoinChainDiff && joinHops.size() >= 2 &&
                       !ivmColExpr.empty();

        o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(createTbl) << "\"));\n";
        o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(createLog) << "\"));\n";
        /* Bag / join-bag seed+publish own the initial IVM row. */
        if (!bagAgg && !joinBag) {
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(initRow) << "\"));\n";
        }

        if (hasJoinChainDiff) {
          for (auto &ct : chainTrigs)
            emitJoinDiffTrigs(ct.table, ct.suffix, ct.whenNew, ct.whenOld, ct.valSql);
        } else if (hasJoinDiff) {
          emitJoinDiffTrigs(baseTable, "_jd_t1", whenT1, whenT1Old, valSqlT1);
          emitJoinDiffTrigs(baseTable2, "_jd_t2", whenT2, whenT2Old, valSqlT2);
        } else if (hasDiff) {
          /* Rewrite bare `id` → NEW.id / OLD.id for trigger WHEN clauses. */
          auto rewriteId = [](const std::string &pred, const char *qual) {
            std::string out;
            for (size_t i = 0; i < pred.size();) {
              if ((i == 0 || !isalnum((unsigned char)pred[i - 1])) &&
                  (pred[i] == 'i' || pred[i] == 'I') && i + 1 < pred.size() &&
                  (pred[i + 1] == 'd' || pred[i + 1] == 'D') &&
                  (i + 2 >= pred.size() || !isalnum((unsigned char)pred[i + 2]))) {
                out += qual;
                out += "id";
                i += 2;
              } else {
                out.push_back(pred[i]);
                ++i;
              }
            }
            return out;
          };
          std::string whenNew = rewriteId(wherePred, "NEW.");
          std::string whenOld = rewriteId(wherePred, "OLD.");
          std::string aiTrig = "CREATE TRIGGER IF NOT EXISTS " + ivmTable +
                               "_ai AFTER INSERT ON " + baseTable + " WHEN " + whenNew +
                               " BEGIN INSERT OR REPLACE INTO " + ivmTable + "(k, v) VALUES(1, NEW." +
                               ivmColExpr + "); END";
          std::string auTrig = "CREATE TRIGGER IF NOT EXISTS " + ivmTable +
                               "_au AFTER UPDATE ON " + baseTable + " WHEN " + whenNew +
                               " BEGIN INSERT OR REPLACE INTO " + ivmTable + "(k, v) VALUES(1, NEW." +
                               ivmColExpr + "); END";
          std::string adTrig = "CREATE TRIGGER IF NOT EXISTS " + ivmTable +
                               "_ad AFTER DELETE ON " + baseTable + " WHEN " + whenOld +
                               " BEGIN INSERT OR REPLACE INTO " + ivmTable +
                               "(k, v) VALUES(1, ''); END";
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(aiTrig) << "\"));\n";
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(auTrig) << "\"));\n";
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(adTrig) << "\"));\n";
        } else if (hasJoin && joinBag) {
          /* Multi-row equi-JOIN bag: each result row keyed by hop rowids.
           * A write on hop hi deletes that hop's bag rows and re-inserts only
           * the join slice pinned to NEW — never a full-table recompute. */
          auto rewriteAliasQual = [](const std::string &expr, const std::string &alias,
                                     const std::string &qual) {
            std::string out;
            std::string aU = toUpper(alias);
            for (size_t i = 0; i < expr.size();) {
              bool atBound =
                  (i == 0 || (!isalnum((unsigned char)expr[i - 1]) && expr[i - 1] != '_'));
              if (atBound && i + alias.size() < expr.size() && expr[i + alias.size()] == '.') {
                bool match = true;
                for (size_t k = 0; k < alias.size(); ++k) {
                  if ((char)std::toupper((unsigned char)expr[i + k]) != aU[k]) {
                    match = false;
                    break;
                  }
                }
                if (match) {
                  out += qual;
                  i += alias.size() + 1;
                  continue;
                }
              }
              out.push_back(expr[i]);
              ++i;
            }
            return out;
          };
          auto sameA = [](const std::string &a, const std::string &b) {
            return toUpper(a) == toUpper(b);
          };
          std::string jbag = "luke_ivm_jbag_" + sanitizeSqlIdent(key);
          std::string colDefs = "k TEXT PRIMARY KEY, v TEXT";
          for (size_t hi = 0; hi < joinHops.size(); ++hi)
            colDefs += ", r" + std::to_string(hi) + " INTEGER NOT NULL";
          std::string createJbag =
              "CREATE TABLE IF NOT EXISTS " + jbag + "(" + colDefs + ")";
          std::string clearJbag = "DELETE FROM " + jbag;
          /* Seed: full join once at watch create. */
          std::string keyExpr = "printf('";
          std::string keyArgs;
          for (size_t hi = 0; hi < joinHops.size(); ++hi) {
            if (hi) keyExpr += ",";
            keyExpr += "%d";
            keyArgs += ", " + joinHops[hi].alias + ".rowid";
          }
          keyExpr += "'" + keyArgs + ")";
          std::string fromJoin = joinHops[0].table + " " + joinHops[0].alias;
          for (size_t hi = 1; hi < joinHops.size(); ++hi) {
            auto &h = joinHops[hi];
            fromJoin += " JOIN " + h.table + " " + h.alias + " ON " + h.onLeftAlias + "." +
                        h.onLeftCol + " = " + h.onRightAlias + "." + h.onRightCol;
          }
          std::string ridCols, ridSels;
          for (size_t hi = 0; hi < joinHops.size(); ++hi) {
            if (hi) {
              ridCols += ", ";
              ridSels += ", ";
            }
            ridCols += "r" + std::to_string(hi);
            ridSels += joinHops[hi].alias + ".rowid";
          }
          std::string seedJbag = "INSERT INTO " + jbag + "(k, v, " + ridCols + ") SELECT " +
                                 keyExpr + ", (" + ivmColExpr + "), " + ridSels + " FROM " +
                                 fromJoin + " WHERE " + wherePred;
          std::string publish = "INSERT OR REPLACE INTO " + ivmTable +
                                "(k, v) VALUES(1, (SELECT group_concat(v, char(10)) FROM "
                                "(SELECT v FROM " +
                                jbag + " ORDER BY " + ridCols + ")))";

          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(createJbag)
            << "\"));\n";
          for (size_t hi = 0; hi < joinHops.size(); ++hi) {
            std::string idx = "CREATE INDEX IF NOT EXISTS " + jbag + "_i" + std::to_string(hi) +
                              " ON " + jbag + "(r" + std::to_string(hi) + ")";
            o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(idx) << "\"));\n";
          }
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(clearJbag)
            << "\"));\n";
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(seedJbag)
            << "\"));\n";
          o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(publish)
            << "\"));\n";

          for (size_t hi = 0; hi < joinHops.size(); ++hi) {
            auto &h = joinHops[hi];
            std::string suffix = "_jb_t" + std::to_string(hi + 1);
            std::string rcol = "r" + std::to_string(hi);
            /* FROM remaining hops + ON, with firing alias rewritten to NEW. */
            std::ostringstream fr;
            bool started = false;
            for (size_t oi = 0; oi < joinHops.size(); ++oi) {
              if ((size_t)oi == hi) continue;
              if (!started) {
                fr << joinHops[oi].table << " " << joinHops[oi].alias;
                started = true;
              } else {
                auto &oh = joinHops[oi];
                std::string lref = sameA(oh.onLeftAlias, h.alias)
                                      ? ("NEW." + oh.onLeftCol)
                                      : (oh.onLeftAlias + "." + oh.onLeftCol);
                std::string rref = sameA(oh.onRightAlias, h.alias)
                                      ? ("NEW." + oh.onRightCol)
                                      : (oh.onRightAlias + "." + oh.onRightCol);
                fr << " JOIN " << oh.table << " " << oh.alias << " ON " << lref << " = " << rref;
              }
            }
            /* Pin every ON edge that touches the firing hop. */
            std::string pinSql;
            for (size_t oi = 1; oi < joinHops.size(); ++oi) {
              auto &oh = joinHops[oi];
              std::string pin;
              if (sameA(oh.onLeftAlias, h.alias))
                pin = oh.onRightAlias + "." + oh.onRightCol + " = NEW." + oh.onLeftCol;
              else if (sameA(oh.onRightAlias, h.alias))
                pin = oh.onLeftAlias + "." + oh.onLeftCol + " = NEW." + oh.onRightCol;
              if (pin.empty()) continue;
              if (!pinSql.empty()) pinSql += " AND ";
              pinSql += pin;
            }
            std::string whereNew = rewriteAliasQual(wherePred, h.alias, "NEW.");
            std::string whereIns = whereNew;
            if (!pinSql.empty()) {
              if (!whereIns.empty()) whereIns = pinSql + " AND (" + whereIns + ")";
              else whereIns = pinSql;
            }
            std::string sel = rewriteAliasQual(ivmColExpr, h.alias, "NEW.");
            /* Key / rid list with NEW.rowid in the firing slot. */
            std::string keyIns = "printf('";
            std::string keyInsArgs;
            std::string ridIns;
            for (size_t oi = 0; oi < joinHops.size(); ++oi) {
              if (oi) {
                keyIns += ",";
                keyInsArgs += ", ";
                ridIns += ", ";
              }
              keyIns += "%d";
              if (oi == hi) {
                keyInsArgs += "NEW.rowid";
                ridIns += "NEW.rowid";
              } else {
                keyInsArgs += joinHops[oi].alias + ".rowid";
                ridIns += joinHops[oi].alias + ".rowid";
              }
            }
            keyIns += "', " + keyInsArgs + ")";
            std::string fromIns = fr.str();
            std::string insertSel = "INSERT INTO " + jbag + "(k, v, " + ridCols + ") SELECT " +
                                    keyIns + ", (" + sel + "), " + ridIns;
            if (!fromIns.empty()) insertSel += " FROM " + fromIns;
            if (!whereIns.empty()) insertSel += " WHERE " + whereIns;

            /* When only the firing table exists in a 1-hop degenerate — not possible
             * for joinHops.size()>=2. If fromIns is empty (firing is sole hop),
             * INSERT…SELECT without FROM still works for NEW-only projection. */
            if (fromIns.empty()) {
              insertSel = "INSERT INTO " + jbag + "(k, v, " + ridCols + ") SELECT " + keyIns +
                          ", (" + sel + "), " + ridIns;
              if (!whereNew.empty()) insertSel += " WHERE " + whereNew;
            }

            std::string delOld =
                "DELETE FROM " + jbag + " WHERE " + rcol + " = OLD.rowid";
            std::string delNew =
                "DELETE FROM " + jbag + " WHERE " + rcol + " = NEW.rowid";

            std::string ai = "CREATE TRIGGER IF NOT EXISTS " + ivmTable + suffix +
                             "_ai AFTER INSERT ON " + h.table + " BEGIN " + delNew + "; " +
                             insertSel + "; " + publish + "; END";
            std::string au = "CREATE TRIGGER IF NOT EXISTS " + ivmTable + suffix +
                             "_au AFTER UPDATE ON " + h.table + " BEGIN " + delOld + "; " +
                             delNew + "; " + insertSel + "; " + publish + "; END";
            std::string ad = "CREATE TRIGGER IF NOT EXISTS " + ivmTable + suffix +
                             "_ad AFTER DELETE ON " + h.table + " BEGIN " + delOld + "; " +
                             publish + "; END";
            o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(ai) << "\"));\n";
            o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(au) << "\"));\n";
            o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(ad) << "\"));\n";
          }
        } else if (hasJoin) {
          /* Unreachable when joinBag covers all non-point equi-joins; keep
             recompute as last-resort safety if joinBag was declined. */
          emitRecomputeTrigs(baseTable, "_t1");
          if (!baseTable2.empty()) emitRecomputeTrigs(baseTable2, "_t2");
        } else {
          /* Single-table: bag-maintained group_concat for multi-row / non-id filters.
           * Simple id=N is handled above (hasDiff). Expression columns still recompute. */
          if (bagAgg) {
            std::string bagTable = "luke_ivm_bag_" + sanitizeSqlIdent(key);
            std::string createBag = "CREATE TABLE IF NOT EXISTS " + bagTable +
                                    "(rid INTEGER PRIMARY KEY, v TEXT)";
            std::string clearBag = "DELETE FROM " + bagTable;
            std::string seedBag = "INSERT INTO " + bagTable +
                                  "(rid, v) SELECT rowid, " + ivmColExpr + " FROM " + baseTable +
                                  " WHERE " + wherePred;
            /* ORDER BY must wrap the bag read — SQLite applies outer ORDER BY
             * after aggregation, which would not order group_concat inputs.
             * char(10) = newline separator (same as historical group_concat). */
            std::string publish = "INSERT OR REPLACE INTO " + ivmTable +
                                  "(k, v) VALUES(1, (SELECT group_concat(v, char(10)) FROM "
                                  "(SELECT v FROM " +
                                  bagTable + " ORDER BY rid)))";
            std::string whenNew = qualifyBarePred(wherePred, "NEW.");
            std::string whenOld = qualifyBarePred(wherePred, "OLD.");
            std::string ai = "CREATE TRIGGER IF NOT EXISTS " + ivmTable +
                             "_bag_ai AFTER INSERT ON " + baseTable + " WHEN " + whenNew +
                             " BEGIN INSERT OR REPLACE INTO " + bagTable +
                             "(rid, v) VALUES(NEW.rowid, NEW." + ivmColExpr + "); " + publish +
                             "; END";
            std::string au = "CREATE TRIGGER IF NOT EXISTS " + ivmTable +
                             "_bag_au AFTER UPDATE ON " + baseTable + " WHEN " + whenNew +
                             " OR " + whenOld + " BEGIN DELETE FROM " + bagTable +
                             " WHERE rid = OLD.rowid; INSERT INTO " + bagTable +
                             "(rid, v) SELECT NEW.rowid, NEW." + ivmColExpr + " WHERE " + whenNew +
                             "; " + publish + "; END";
            std::string ad = "CREATE TRIGGER IF NOT EXISTS " + ivmTable +
                             "_bag_ad AFTER DELETE ON " + baseTable + " WHEN " + whenOld +
                             " BEGIN DELETE FROM " + bagTable + " WHERE rid = OLD.rowid; " +
                             publish + "; END";
            o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(createBag)
              << "\"));\n";
            o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(clearBag)
              << "\"));\n";
            o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(seedBag)
              << "\"));\n";
            o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(publish)
              << "\"));\n";
            o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(ai) << "\"));\n";
            o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(au) << "\"));\n";
            o << "  dbExec(arena, " << cIdent(dbName) << ", luke_text(\"" << esc(ad) << "\"));\n";
          } else {
            emitRecomputeTrigs(baseTable, "");
          }
        }
      } else {
        BC::RxQueryDef qd;
        qd.name = key;
        qd.dbLocal = dbName;
        qd.sql = sql;
        qd.readSql = sql;
        qd.line = line;
        bc.rxQueryDefs.push_back(qd);
      }

      bc.usesRx = true;
      bc.locals[cellName] = Ty::text();
      bc.rxCellTy[key] = Ty::text();
      if (!bc.rxCells.count(key)) bc.rxCellOrder.push_back(key);
      bc.rxCells[key] = true;
      emitRxNamedAssign(o, "_luke_rx_id_" + cIdent(key),
                        "luke_rx_cell_text(_luke_rx, luke_text(\"\"))", key);
      o << "  luke_rx_query_refresh(_luke_rx, _luke_rx_id_" << cIdent(key) << ", "
        << cIdent(dbName) << ", luke_text(\"" << esc(readSql) << "\"));\n";
      return;
    }

    /* Client wire watch — cell must already be REMEMBER'd. */
    if (!bc.rxCells.count(cellName)) {
      bc.fail(line, "WATCH '" + cellName + "' — REMEMBER it first as a reactive TEXT cell");
      return;
    }
    Ty cty = bc.rxCellTy.count(cellName) ? bc.rxCellTy[cellName] : Ty::num();
    bc.expectTy(line, cty, Ty::text(), "WATCH");
    if (bc.bad) return;
    std::string urlPart =
        readyPos == std::string::npos ? after : trim(after.substr(0, readyPos));
    if (startsWithCI(urlPart, "GET ")) urlPart = trim(urlPart.substr(4));
    if (urlPart.empty()) {
      bc.fail(line, "WATCH needs a url — WATCH user FROM \"http://…/watch\"");
      return;
    }
    std::string synth = "START SUBSCRIBE \"" + cellName + "\" GET " + urlPart + " INTO " + cellName;
    if (readyPos != std::string::npos)
      synth += " READY " + trim(after.substr(readyPos + 7));
    stmt(bc, synth, line, o);
    return;
  }

  /* PUSH WATCH user ON req [FOR n BEATS] [EVERY ms MILLISECONDS]
   * Live Graph tier 2: CDC-poll the WATCH'd query cell and SSE-push on change. */
  if (startsWithCI(text, "PUSH WATCH ")) {
    auto rest = trim(text.substr(11));
    stripDo(rest);
    auto U = toUpper(rest);
    auto onPos = findOutsideQuotes(rest, U, " ON ");
    if (onPos == std::string::npos) {
      bc.fail(line, "PUSH WATCH needs ON — PUSH WATCH user ON req");
      return;
    }
    auto cellName =
        resolveRxCellName(bc.rxCells, bc.rxEntityStack, stripThe(trim(rest.substr(0, onPos))));
    auto afterOn = trim(rest.substr(onPos + 4));
    auto aU = toUpper(afterOn);
    size_t forPos = findOutsideQuotes(afterOn, aU, " FOR ");
    size_t everyPos = findOutsideQuotes(afterOn, aU, " EVERY ");
    size_t reqEnd = afterOn.size();
    if (forPos != std::string::npos && forPos < reqEnd) reqEnd = forPos;
    if (everyPos != std::string::npos && everyPos < reqEnd) reqEnd = everyPos;
    auto reqName = stripThe(trim(afterOn.substr(0, reqEnd)));
    if (cellName.empty() || reqName.empty()) {
      bc.fail(line, "PUSH WATCH needs cell and request — PUSH WATCH user ON req");
      return;
    }
    const BC::RxQueryDef *qd = nullptr;
    for (auto &q : bc.rxQueryDefs)
      if (q.name == cellName) {
        qd = &q;
        break;
      }
    if (!qd) {
      bc.fail(line, "PUSH WATCH '" + cellName + "' — WATCH it FROM a DATABASE first");
      return;
    }
    if ((bc.rxSecretCells.count(cellName) || bc.rxSecretCells.count(stripThe(trim(rest.substr(0, onPos))))) &&
        !qd->scopedToUser) {
      bc.fail(line, "PUSH WATCH of SECRET '" + cellName +
                        "' requires WATCH … FOR CURRENT USER — unauthorized stream = compile error");
      return;
    }
    if (!bc.locals.count(reqName) || bc.locals[reqName].k != K::Ptr ||
        bc.locals[reqName].klass != "__HttpReq") {
      bc.fail(line, "PUSH WATCH ON needs a REQUEST — MY NAME IS " + reqName + " AS REQUEST");
      return;
    }
    double beats = 50;
    double everyMs = 50;
    if (forPos != std::string::npos) {
      auto forClause = trim(afterOn.substr(forPos + 5));
      auto fU = toUpper(forClause);
      size_t beatWord = fU.find(" BEAT");
      auto nPart = beatWord == std::string::npos ? forClause : trim(forClause.substr(0, beatWord));
      /* Stop at EVERY if present in forClause remnant */
      auto nU = toUpper(nPart);
      size_t evIn = nU.find(" EVERY ");
      if (evIn != std::string::npos) nPart = trim(nPart.substr(0, evIn));
      auto nE = bc.expr(nPart, line);
      if (!bc.isNumeric(nE.ty)) {
        bc.fail(line, "PUSH WATCH FOR wants a NUMBER of beats");
        return;
      }
      /* Prefer literal when possible; otherwise emit runtime expression via temp — use 50 default
       * parse from literal text for CI simplicity. */
      try {
        beats = std::stod(nPart);
      } catch (...) {
        beats = 50;
      }
      if (beats < 1) beats = 1;
    }
    if (everyPos != std::string::npos) {
      auto everyClause = trim(afterOn.substr(everyPos + 7));
      auto eU = toUpper(everyClause);
      size_t msWord = eU.find(" MILLISECOND");
      auto msPart = msWord == std::string::npos ? everyClause : trim(everyClause.substr(0, msWord));
      try {
        everyMs = std::stod(msPart);
      } catch (...) {
        everyMs = 50;
      }
      if (everyMs < 1) everyMs = 1;
    }
    bc.usesRx = true;
    o << "  if (httpSseOpen(arena, " << cIdent(reqName) << ")) {\n";
    if (qd->scopedToUser) {
      /* Fail closed: scoped watch streams require an authenticated request user. */
      o << "    LukeText _luke_push_uid = luke_auth_current_user();\n";
      o << "    if (!_luke_push_uid.len) _luke_push_uid = " << cIdent(reqName) << "->user_id;\n";
      o << "    if (!_luke_push_uid.len) {\n";
      o << "      /* unauthorized scoped PUSH — close without streaming */\n";
      o << "    } else {\n";
    }
    o << "  {\n";
    o << "    LukeText _luke_watch_last = luke_text(\"\");\n";
    o << "    double _luke_watch_beats = 0;\n";
    o << "    int64_t _luke_watch_ver = -1;\n";
    o << "    int64_t _luke_watch_queries = 0;\n";
    o << "    int64_t _luke_watch_seq = 0;\n";
    o << "    int _luke_watch_alive = 1;\n";
    if (!qd->eventLog.empty()) {
      /* Distributed time-travel: resume from Last-Event-ID via causal event log. */
      o << "    {\n";
      o << "      LukeText _luke_lei = luke_http_last_event_id(" << cIdent(reqName) << ");\n";
      o << "      int64_t _luke_resume = 0;\n";
      o << "      if (_luke_lei.len > 0) {\n";
      o << "        char _luke_leib[64]; size_t _luke_lein = _luke_lei.len < 63 ? _luke_lei.len : 63;\n";
      o << "        memcpy(_luke_leib, _luke_lei.ptr, _luke_lein); _luke_leib[_luke_lein] = 0;\n";
      o << "        _luke_resume = (int64_t)atoll(_luke_leib);\n";
      o << "      }\n";
      o << "      char _luke_sqlbuf[256];\n";
      o << "      snprintf(_luke_sqlbuf, sizeof(_luke_sqlbuf),\n";
      o << "        \"SELECT seq, v FROM " << esc(qd->eventLog)
        << " WHERE seq > %lld ORDER BY seq\", (long long)_luke_resume);\n";
      o << "      sqlite3_stmt *_luke_st = NULL;\n";
      o << "      if (" << cIdent(qd->dbLocal) << " && " << cIdent(qd->dbLocal)
        << "->db &&\n";
      o << "          sqlite3_prepare_v2(" << cIdent(qd->dbLocal)
        << "->db, _luke_sqlbuf, -1, &_luke_st, NULL) == SQLITE_OK) {\n";
      o << "        while (sqlite3_step(_luke_st) == SQLITE_ROW) {\n";
      o << "          int64_t _luke_s = sqlite3_column_int64(_luke_st, 0);\n";
      o << "          const unsigned char *_luke_tv = sqlite3_column_text(_luke_st, 1);\n";
      o << "          const char *_luke_ts = _luke_tv ? (const char *)_luke_tv : \"\";\n";
      o << "          size_t _luke_tl = strlen(_luke_ts);\n";
      o << "          char *_luke_tp = (char *)luke_arena_alloc(arena, _luke_tl + 1, 1);\n";
      o << "          if (_luke_tl) memcpy(_luke_tp, _luke_ts, _luke_tl);\n";
      o << "          _luke_tp[_luke_tl] = 0;\n";
      o << "          LukeText _luke_v = luke_text_n(_luke_tp, _luke_tl);\n";
      o << "          _luke_watch_last = _luke_v;\n";
      o << "          _luke_watch_seq = _luke_s;\n";
      o << "          luke_rx_write_text(_luke_rx, _luke_rx_id_" << cIdent(cellName)
        << ", _luke_v);\n";
      o << "          if (!httpSseId(arena, " << cIdent(reqName)
        << ", luke_integer_to_text(arena, _luke_s)) ||\n";
      o << "              !httpSseData(arena, " << cIdent(reqName) << ", _luke_v)) {\n";
      o << "            _luke_watch_alive = 0;\n";
      o << "          } else {\n";
      o << "            luke_speak_text(luke_text_concat(arena, luke_text(\"replay=\"), _luke_v));\n";
      o << "          }\n";
      o << "          if (!_luke_watch_alive) break;\n";
      o << "        }\n";
      o << "        sqlite3_finalize(_luke_st);\n";
      o << "      }\n";
      o << "    }\n";
    }
    o << "    while (_luke_watch_alive && _luke_watch_beats < " << beats << ") {\n";
    o << "      _luke_watch_beats = _luke_watch_beats + 1;\n";
    o << "      int64_t _luke_watch_nowv = luke_db_data_version(" << cIdent(qd->dbLocal) << ");\n";
    o << "      if (_luke_watch_nowv != _luke_watch_ver) {\n";
    o << "        _luke_watch_ver = _luke_watch_nowv;\n";
    o << "        _luke_watch_queries = _luke_watch_queries + 1;\n";
    if (qd->scopedToUser) {
      o << "        LukeText _luke_watch_now = luke_text(\"\");\n";
      o << "        {\n";
      o << "          LukeText _luke_uid = luke_auth_current_user();\n";
      o << "          if (!_luke_uid.len) _luke_uid = " << cIdent(reqName) << "->user_id;\n";
      o << "          if (_luke_uid.len) {\n";
      o << "            LukeList *_luke_ub = luke_list_new(arena);\n";
      o << "            luke_list_add(arena, _luke_ub, _luke_uid);\n";
      o << "            _luke_watch_now = luke_db_query_bind_text(arena, " << cIdent(qd->dbLocal)
        << ", luke_text(\"" << esc(qd->sql) << "\"), _luke_ub);\n";
      o << "          }\n";
      o << "        }\n";
    } else {
      auto querySql = qd->hasIvm ? qd->readSql : qd->sql;
      o << "        LukeText _luke_watch_now = luke_db_query_text(arena, " << cIdent(qd->dbLocal)
        << ", luke_text(\"" << esc(querySql) << "\"));\n";
    }
    o << "        if (!luke_text_eq(_luke_watch_now, _luke_watch_last)) {\n";
    o << "          _luke_watch_last = _luke_watch_now;\n";
    o << "          luke_rx_write_text(_luke_rx, _luke_rx_id_" << cIdent(cellName)
      << ", _luke_watch_now);\n";
    if (!qd->eventLog.empty()) {
      o << "          _luke_watch_seq = luke_db_log_append(" << cIdent(qd->dbLocal)
        << ", luke_text(\"" << esc(qd->eventLog) << "\"), _luke_watch_now);\n";
    } else {
      o << "          _luke_watch_seq = _luke_watch_seq + 1;\n";
    }
    o << "          if (!httpSseId(arena, " << cIdent(reqName)
      << ", luke_integer_to_text(arena, _luke_watch_seq)) ||\n";
    o << "              !httpSseData(arena, " << cIdent(reqName) << ", _luke_watch_now)) {\n";
    o << "            _luke_watch_alive = 0;\n";
    o << "            break;\n";
    o << "          }\n";
    o << "          luke_speak_text(luke_text_concat(arena, luke_text(\"row=\"), _luke_watch_now));\n";
    o << "        }\n";
    o << "      }\n";
    o << "      if (!httpSseComment(arena, " << cIdent(reqName) << ", luke_text(\"cdc\"))) {\n";
    o << "        _luke_watch_alive = 0;\n";
    o << "        break;\n";
    o << "      }\n";
    o << "      double _luke_watch_t0 = argus_now_ms();\n";
    o << "      while (_luke_watch_alive && (argus_now_ms() - _luke_watch_t0) < " << everyMs
      << ") {\n";
    o << "      }\n";
    o << "    }\n";
    o << "    luke_speak_text(luke_text_concat(arena, luke_text(\"watch_queries=\"), "
      << "luke_integer_to_text(arena, _luke_watch_queries)));\n";
    o << "  }\n";
    if (qd->scopedToUser) {
      o << "    }\n"; /* end auth-ok branch */
    }
    o << "  }\n"; /* end httpSseOpen ok */
    o << "  httpClose(arena, " << cIdent(reqName) << ");\n";
    return;
  }

  /* START SUBSCRIBE "feed" GET "url" [INTO body] [READY ready] — SSE → cell (Spike A push) */
  if (startsWithCI(text, "START SUBSCRIBE ")) {
    auto rest = trim(text.substr(16));
    auto U = toUpper(rest);
    auto getPos = findOutsideQuotes(rest, U, " GET ");
    if (getPos == std::string::npos) {
      bc.fail(line, "START SUBSCRIBE needs GET — START SUBSCRIBE \"feed\" GET \"url\"");
      return;
    }
    auto idRaw = trim(rest.substr(0, getPos));
    auto idE = bc.coerceText(bc.expr(idRaw, line));
    auto after = trim(rest.substr(getPos + 5));
    auto aU = toUpper(after);
    size_t intoPos = findOutsideQuotes(after, aU, " INTO ");
    size_t readyPos = findOutsideQuotes(after, aU, " READY ");

    size_t firstClause = std::string::npos;
    auto consider = [&](size_t p) {
      if (p != std::string::npos && (firstClause == std::string::npos || p < firstClause))
        firstClause = p;
    };
    consider(intoPos);
    consider(readyPos);
    auto urlPart = firstClause == std::string::npos ? after : trim(after.substr(0, firstClause));
    auto urlE = bc.coerceText(bc.expr(urlPart, line));

    auto parseCellRef = [&](size_t pos, size_t keyLen, const char *clause,
                            bool wantText) -> std::string {
      if (pos == std::string::npos) return {};
      size_t start = pos + keyLen;
      size_t end = after.size();
      auto limit = [&](size_t p) {
        if (p != std::string::npos && p > pos && p < end) end = p;
      };
      limit(intoPos);
      limit(readyPos);
      auto name = stripThe(trim(after.substr(start, end - start)));
      if (name.empty()) {
        bc.fail(line, std::string("START SUBSCRIBE ") + clause + " needs a cell name");
        return {};
      }
      if (!bc.rxCells.count(name)) {
        bc.fail(line, std::string("START SUBSCRIBE ") + clause + " '" + name +
                          "' — REMEMBER it first as a reactive cell");
        return {};
      }
      Ty ty = bc.rxCellTy.count(name) ? bc.rxCellTy[name] : Ty::num();
      if (wantText)
        bc.expectTy(line, ty, Ty::text(), std::string("START SUBSCRIBE ") + clause);
      else if (ty.k != K::Num && ty.k != K::Flag && ty.k != K::Int)
        bc.fail(line, std::string("START SUBSCRIBE ") + clause + " wants NUMBER/INTEGER/FLAG cell");
      return name;
    };

    std::string intoCell = parseCellRef(intoPos, 6, "INTO", true);
    if (bc.bad) return;
    std::string readyCell = parseCellRef(readyPos, 7, "READY", false);
    if (bc.bad) return;

    if (!intoCell.empty() || !readyCell.empty()) {
      std::string jobLit = unquoteText(idRaw);
      if (jobLit.empty()) jobLit = idRaw;
      bc.rxSubscribeBinds.push_back({jobLit, intoCell, readyCell, line});
      bc.usesRx = true;
    }

    o << "  luke_js_subscribe_start(" << idE.code << ", " << urlE.code << ");\n";
    if (!readyCell.empty()) {
      Ty rty = bc.rxCellTy.count(readyCell) ? bc.rxCellTy[readyCell] : Ty::num();
      if (rty.k == K::Int)
        o << "  luke_rx_write_int(_luke_rx, _luke_rx_id_" << cIdent(readyCell) << ", 0);\n";
      else
        o << "  luke_rx_write_num(_luke_rx, _luke_rx_id_" << cIdent(readyCell) << ", 0);\n";
    }
    return;
  }

  /* Hanka — layout boxes → Argus frames */
  if (toUpper(text) == "LAY OUT THE SCREEN" || toUpper(text) == "LAYOUT THE SCREEN" ||
      toUpper(text) == "LAY OUT" || toUpper(text) == "LAYOUT") {
    if (bc.usesRxUi || bc.needsViewportRelayout) o << "  hanka_set_keep_roots(arena, 1);\n";
    o << "  hanka_layout(arena);\n";
    return;
  }
  if (startsWithCI(text, "BEGIN COLUMN") || startsWithCI(text, "BEGIN ROW") ||
      startsWithCI(text, "BEGIN STACK") || startsWithCI(text, "BEGIN GRID")) {
    std::string axis = startsWithCI(text, "BEGIN COLUMN") ? "COLUMN"
                       : startsWithCI(text, "BEGIN ROW")    ? "ROW"
                       : startsWithCI(text, "BEGIN STACK")  ? "STACK"
                                                            : "GRID";
    /* "BEGIN COLUMN"=12, "BEGIN ROW"=9, "BEGIN STACK"=11, "BEGIN GRID"=10 */
    auto rest = trim(text.substr(axis == "COLUMN"   ? 12
                                 : axis == "ROW"    ? 9
                                 : axis == "STACK"  ? 11
                                                    : 10));
    stripDo(rest);
    auto U = toUpper(rest);
    size_t atPos = std::string::npos;
    size_t atLen = 4;
    if (startsWithCI(rest, "AT ")) {
      atPos = 0;
      atLen = 3;
    } else {
      atPos = findOutsideQuotes(rest, U, " AT ");
    }
    auto sizePos = findOutsideQuotes(rest, U, " SIZE ");
    if (atPos == std::string::npos || sizePos == std::string::npos || atPos > sizePos) {
      bc.fail(line, "BEGIN " + axis +
                        " needs AT x, y SIZE w, h [PAD n] [GAP n] "
                        "[ALIGN START|CENTER|END | ALIGN MAIN … CROSS … | ALIGN main, cross] "
                        "[WRAP] [STACK BELOW n] [WRAP BELOW n] [SCROLL] [COLUMNS n] [WEAR \"…\"]");
      return;
    }
    auto atPart = trim(rest.substr(atPos + atLen, sizePos - (atPos + atLen)));
    auto afterSize = trim(rest.substr(sizePos + 6));
    auto asU = toUpper(afterSize);
    auto padPos = findOutsideQuotes(afterSize, asU, " PAD ");
    auto gapPos = findOutsideQuotes(afterSize, asU, " GAP ");
    auto alignPos = findOutsideQuotes(afterSize, asU, " ALIGN ");
    auto stackBelowPos = findOutsideQuotes(afterSize, asU, " STACK BELOW ");
    auto wrapBelowPos = findOutsideQuotes(afterSize, asU, " WRAP BELOW ");
    auto wrapPos = findOutsideQuotes(afterSize, asU, " WRAP");
    auto scrollPos = findOutsideQuotes(afterSize, asU, " SCROLL");
    size_t colsPos = findOutsideQuotes(afterSize, asU, " COLUMNS ");
    size_t colsTokLen = 9; /* " COLUMNS " */
    if (colsPos == std::string::npos) {
      colsPos = findOutsideQuotes(afterSize, asU, " COLS ");
      colsTokLen = 6; /* " COLS " */
    }
    auto wearPos = findOutsideQuotes(afterSize, asU, " WEAR ");
    /* Bare WRAP must not steal the WRAP in WRAP BELOW */
    bool bareWrap = wrapPos != std::string::npos &&
                    !(wrapBelowPos != std::string::npos && wrapPos == wrapBelowPos);
    std::string sizePart = afterSize;
    std::string padRaw = "0";
    std::string gapRaw = "0";
    std::string stackBelowRaw;
    std::string wrapBelowRaw;
    std::string colsRaw = "1";
    std::string wearRaw;
    int alignVal = 0; /* start */
    int crossAlignVal = 0;
    int wrapVal = 0;
    int scrollVal = 0;
    int hasCrossAlign = 0;
    auto earlier = [](size_t cut, size_t pos) {
      return pos != std::string::npos && pos < cut ? pos : cut;
    };
    size_t cut = afterSize.size();
    cut = earlier(cut, padPos);
    cut = earlier(cut, gapPos);
    cut = earlier(cut, alignPos);
    cut = earlier(cut, stackBelowPos);
    cut = earlier(cut, wrapBelowPos);
    if (bareWrap) cut = earlier(cut, wrapPos);
    cut = earlier(cut, scrollPos);
    cut = earlier(cut, colsPos);
    cut = earlier(cut, wearPos);
    sizePart = trim(afterSize.substr(0, cut));
    if (bareWrap) wrapVal = 1;
    if (scrollPos != std::string::npos) scrollVal = 1;
    auto nextCutAfter = [&](size_t from) {
      size_t end = afterSize.size();
      auto consider = [&](size_t pos) {
        if (pos != std::string::npos && pos > from && pos < end) end = pos;
      };
      consider(padPos);
      consider(gapPos);
      consider(alignPos);
      consider(stackBelowPos);
      consider(wrapBelowPos);
      if (bareWrap) consider(wrapPos);
      consider(scrollPos);
      consider(colsPos);
      consider(wearPos);
      return end;
    };
    if (padPos != std::string::npos) {
      size_t padEnd = nextCutAfter(padPos);
      padRaw = trim(afterSize.substr(padPos + 5, padEnd - (padPos + 5)));
    }
    if (gapPos != std::string::npos) {
      size_t gapEnd = nextCutAfter(gapPos);
      gapRaw = trim(afterSize.substr(gapPos + 5, gapEnd - (gapPos + 5)));
    }
    if (stackBelowPos != std::string::npos) {
      size_t sbEnd = nextCutAfter(stackBelowPos);
      stackBelowRaw = trim(afterSize.substr(stackBelowPos + 13, sbEnd - (stackBelowPos + 13)));
    }
    if (wrapBelowPos != std::string::npos) {
      size_t wbEnd = nextCutAfter(wrapBelowPos);
      wrapBelowRaw = trim(afterSize.substr(wrapBelowPos + 12, wbEnd - (wrapBelowPos + 12)));
    }
    if (colsPos != std::string::npos) {
      size_t cEnd = nextCutAfter(colsPos);
      colsRaw = trim(afterSize.substr(colsPos + colsTokLen, cEnd - (colsPos + colsTokLen)));
    }
    if (wearPos != std::string::npos) {
      size_t wEnd = nextCutAfter(wearPos);
      wearRaw = trim(afterSize.substr(wearPos + 6, wEnd - (wearPos + 6)));
    }
    auto parseAlignTok = [&](const std::string &tok, int *out) -> bool {
      auto t = toUpper(trim(tok));
      if (t == "CENTER" || t == "MIDDLE") {
        *out = 1;
        return true;
      }
      if (t == "END" || t == "RIGHT" || t == "BOTTOM") {
        *out = 2;
        return true;
      }
      if (t == "START" || t == "LEFT" || t == "TOP" || t.empty()) {
        *out = 0;
        return true;
      }
      return false;
    };
    if (alignPos != std::string::npos) {
      size_t alignEnd = nextCutAfter(alignPos);
      auto alignRaw = trim(afterSize.substr(alignPos + 7, alignEnd - (alignPos + 7)));
      auto aU = toUpper(alignRaw);
      /* ALIGN MAIN CENTER CROSS START */
      auto mainPos = aU.find("MAIN ");
      auto crossPos = aU.find(" CROSS ");
      if (mainPos == 0 && crossPos != std::string::npos) {
        auto mainTok = trim(alignRaw.substr(5, crossPos - 5));
        auto crossTok = trim(alignRaw.substr(crossPos + 7));
        if (!parseAlignTok(mainTok, &alignVal) || !parseAlignTok(crossTok, &crossAlignVal)) {
          bc.fail(line, "ALIGN MAIN/CROSS needs START, CENTER, or END");
          return;
        }
        hasCrossAlign = 1;
      } else {
        auto comma = findOutsideQuotes(alignRaw, aU, ",");
        if (comma != std::string::npos) {
          /* ALIGN CENTER, START — main, cross */
          if (!parseAlignTok(alignRaw.substr(0, comma), &alignVal) ||
              !parseAlignTok(alignRaw.substr(comma + 1), &crossAlignVal)) {
            bc.fail(line, "ALIGN main, cross needs START, CENTER, or END");
            return;
          }
          hasCrossAlign = 1;
        } else if (!parseAlignTok(alignRaw, &alignVal)) {
          bc.fail(line, "ALIGN needs START, CENTER, or END — got " + alignRaw);
          return;
        } else {
          crossAlignVal = alignVal;
        }
      }
    }
    auto commaAt = findOutsideQuotes(atPart, toUpper(atPart), ",");
    auto commaSz = findOutsideQuotes(sizePart, toUpper(sizePart), ",");
    if (commaAt == std::string::npos || commaSz == std::string::npos) {
      bc.fail(line, "BEGIN " + axis + " AT needs x, y and SIZE needs w, h (AUTO ok on flex children)");
      return;
    }
    auto x = bc.expr(trim(atPart.substr(0, commaAt)), line);
    auto y = bc.expr(trim(atPart.substr(commaAt + 1)), line);
    auto wRaw = trim(sizePart.substr(0, commaSz));
    auto hRaw = trim(sizePart.substr(commaSz + 1));
    /* Spike B part 2: SIZE AUTO → flex-grow on nested flow boxes */
    auto w = toUpper(wRaw) == "AUTO" ? Expr{"-1.0", Ty::num()} : bc.expr(wRaw, line);
    auto h = toUpper(hRaw) == "AUTO" ? Expr{"-1.0", Ty::num()} : bc.expr(hRaw, line);
    auto pad = bc.expr(padRaw, line);
    auto gap = bc.expr(gapRaw, line);
    bc.expectTy(line, x.ty, Ty::num(), "BEGIN AT x");
    bc.expectTy(line, y.ty, Ty::num(), "BEGIN AT y");
    bc.expectTy(line, w.ty, Ty::num(), "BEGIN SIZE w");
    bc.expectTy(line, h.ty, Ty::num(), "BEGIN SIZE h");
    bc.expectTy(line, pad.ty, Ty::num(), "BEGIN PAD");
    bc.expectTy(line, gap.ty, Ty::num(), "BEGIN GAP");
    if (axis == "GRID") {
      auto cols = bc.expr(colsRaw.empty() ? "1" : colsRaw, line);
      bc.expectTy(line, cols.ty, Ty::num(), "BEGIN GRID COLUMNS");
      o << "  hanka_begin_grid(arena, " << x.code << ", " << y.code << ", " << w.code << ", "
        << h.code << ", " << pad.code << ", " << gap.code << ", " << cols.code << ");\n";
    } else {
      const char *fn = axis == "COLUMN" ? "hanka_begin_column"
                       : axis == "ROW"  ? "hanka_begin_row"
                                        : "hanka_begin_stack";
      o << "  " << fn << "(arena, " << x.code << ", " << y.code << ", " << w.code << ", " << h.code
        << ", " << pad.code << ", " << gap.code << ");\n";
    }
    if (hasCrossAlign)
      o << "  hanka_set_align_axes(arena, " << alignVal << ", " << crossAlignVal << ");\n";
    else if (alignVal != 0)
      o << "  hanka_set_align(arena, " << alignVal << ");\n";
    if (wrapVal != 0) o << "  hanka_set_wrap(arena, 1);\n";
    if (!stackBelowRaw.empty()) {
      auto sb = bc.expr(stackBelowRaw, line);
      bc.expectTy(line, sb.ty, Ty::num(), "STACK BELOW");
      o << "  hanka_set_stack_below(arena, " << sb.code << ");\n";
      bc.needsViewportRelayout = true;
    }
    if (!wrapBelowRaw.empty()) {
      auto wb = bc.expr(wrapBelowRaw, line);
      bc.expectTy(line, wb.ty, Ty::num(), "WRAP BELOW");
      o << "  hanka_set_wrap_below(arena, " << wb.code << ");\n";
      bc.needsViewportRelayout = true;
    }
    if (scrollVal != 0) o << "  hanka_set_scroll(arena, 1);\n";
    if (!wearRaw.empty()) {
      auto cls = bc.coerceText(bc.expr(wearRaw, line));
      o << "  hanka_set_class(arena, " << cls.code << ");\n";
    }
    bc.hankaStack.push_back(axis);
    return;
  }
  if (toUpper(text) == "END COLUMN" || toUpper(text) == "END ROW" || toUpper(text) == "END STACK" ||
      toUpper(text) == "END GRID" || toUpper(text) == "END HANKA") {
    if (bc.hankaStack.empty()) {
      bc.fail(line, "END without matching BEGIN COLUMN|ROW|STACK|GRID");
      return;
    }
    std::string want = bc.hankaStack.back();
    std::string got = toUpper(text) == "END HANKA" ? want : trim(toUpper(text).substr(4));
    if (got != want) {
      bc.fail(line, "END " + got + " but open box is " + want);
      return;
    }
    bc.hankaStack.pop_back();
    o << "  hanka_end(arena);\n";
    return;
  }
  if (startsWithCI(text, "SLOT ")) {
    auto rest = trim(text.substr(5));
    auto U = toUpper(rest);
    auto kindEnd = rest.find(' ');
    if (kindEnd == std::string::npos) {
      bc.fail(line, "SLOT needs TEXT|BUTTON|IMAGE|BOX|INPUT|SELECT|TABLE|MODAL \"id\" …");
      return;
    }
    auto kindRaw = toUpper(trim(rest.substr(0, kindEnd)));
    rest = trim(rest.substr(kindEnd));
    U = toUpper(rest);
    auto atPos = findOutsideQuotes(rest, U, " AT ");
    auto sizePos = findOutsideQuotes(rest, U, " SIZE ");
    if (sizePos == std::string::npos) {
      bc.fail(line, "SLOT needs SIZE w, h (use AUTO for measured text width)");
      return;
    }
    std::string idPart;
    bool hasAt = atPos != std::string::npos && atPos < sizePos;
    if (hasAt) {
      idPart = trim(rest.substr(0, atPos));
    } else {
      idPart = trim(rest.substr(0, sizePos));
    }
    int inputType = 0;
    {
      auto idU = toUpper(idPart);
      if (startsWithCI(idPart, "AS ")) {
        /* SLOT INPUT AS CHECKBOX "id" */
        auto afterAs = trim(idPart.substr(3));
        auto sp = afterAs.find(' ');
        if (sp == std::string::npos) {
          bc.fail(line, "INPUT AS needs a type and id — SLOT INPUT AS CHECKBOX \"agree\" …");
          return;
        }
        auto ty = toUpper(trim(afterAs.substr(0, sp)));
        idPart = trim(afterAs.substr(sp));
        if (ty == "PASSWORD") inputType = 1;
        else if (ty == "EMAIL") inputType = 2;
        else if (ty == "TEXT") inputType = 0;
        else if (ty == "CHECKBOX" || ty == "CHECK") inputType = 3;
        else if (ty == "RADIO") inputType = 4;
        else {
          bc.fail(line, "INPUT AS needs TEXT, EMAIL, PASSWORD, CHECKBOX, or RADIO — got " + ty);
          return;
        }
      } else {
        auto asTy = findOutsideQuotes(idPart, idU, " AS ");
        if (asTy != std::string::npos) {
          auto ty = toUpper(trim(idPart.substr(asTy + 4)));
          idPart = trim(idPart.substr(0, asTy));
          if (ty == "PASSWORD") inputType = 1;
          else if (ty == "EMAIL") inputType = 2;
          else if (ty == "TEXT") inputType = 0;
          else if (ty == "CHECKBOX" || ty == "CHECK") inputType = 3;
          else if (ty == "RADIO") inputType = 4;
          else {
            bc.fail(line, "INPUT AS needs TEXT, EMAIL, PASSWORD, CHECKBOX, or RADIO — got " + ty);
            return;
          }
        }
      }
    }
    if (idPart.empty()) {
      bc.fail(line, "SLOT needs an element id after the kind");
      return;
    }
    auto idE = bc.coerceText(bc.expr(idPart, line));
    std::string atPart;
    if (hasAt) atPart = trim(rest.substr(atPos + 4, sizePos - (atPos + 4)));
    auto afterSize = trim(rest.substr(sizePos + 6));
    {
      auto aU = toUpper(afterSize);
      size_t p = findOutsideQuotes(afterSize, aU, " AS PASSWORD");
      if (p != std::string::npos) {
        inputType = 1;
        afterSize = trim(trim(afterSize.substr(0, p)) + " " + trim(afterSize.substr(p + 12)));
      } else if ((p = findOutsideQuotes(afterSize, aU, " AS EMAIL")) != std::string::npos) {
        inputType = 2;
        afterSize = trim(trim(afterSize.substr(0, p)) + " " + trim(afterSize.substr(p + 9)));
      } else if ((p = findOutsideQuotes(afterSize, aU, " AS CHECKBOX")) != std::string::npos) {
        inputType = 3;
        afterSize = trim(trim(afterSize.substr(0, p)) + " " + trim(afterSize.substr(p + 12)));
      } else if ((p = findOutsideQuotes(afterSize, aU, " AS RADIO")) != std::string::npos) {
        inputType = 4;
        afterSize = trim(trim(afterSize.substr(0, p)) + " " + trim(afterSize.substr(p + 9)));
      } else if ((p = findOutsideQuotes(afterSize, aU, " AS TEXT")) != std::string::npos) {
        inputType = 0;
        afterSize = trim(trim(afterSize.substr(0, p)) + " " + trim(afterSize.substr(p + 8)));
      }
    }
    auto sayPos = findOutsideQuotes(afterSize, toUpper(afterSize), " SAY ");
    auto fromPos = findOutsideQuotes(afterSize, toUpper(afterSize), " FROM ");
    std::string sizePart;
    std::string trail;
    if (sayPos != std::string::npos) {
      sizePart = trim(afterSize.substr(0, sayPos));
      trail = trim(afterSize.substr(sayPos + 5));
    } else if (fromPos != std::string::npos) {
      sizePart = trim(afterSize.substr(0, fromPos));
      trail = trim(afterSize.substr(fromPos + 6));
    } else {
      sizePart = afterSize;
    }
    int liveLevel = 0;
    std::string wearRaw;
    auto stripTrailMods = [&](std::string &t) {
      for (;;) {
        auto tU = toUpper(t);
        size_t p;
        if ((p = findOutsideQuotes(t, tU, " ANNOUNCE URGENT")) != std::string::npos) {
          liveLevel = 2;
          t = trim(t.substr(0, p) + " " + t.substr(p + 16));
          continue;
        }
        if ((p = findOutsideQuotes(t, tU, " ANNOUNCE")) != std::string::npos) {
          liveLevel = liveLevel ? liveLevel : 1;
          t = trim(t.substr(0, p) + " " + t.substr(p + 9));
          continue;
        }
        if ((p = findOutsideQuotes(t, tU, " WEAR ")) != std::string::npos) {
          wearRaw = trim(t.substr(p + 6));
          t = trim(t.substr(0, p));
          continue;
        }
        break;
      }
    };
    stripTrailMods(trail);
    if (trail.empty()) stripTrailMods(sizePart);
    auto commaSz = findOutsideQuotes(sizePart, toUpper(sizePart), ",");
    if (commaSz == std::string::npos) {
      bc.fail(line, "SLOT SIZE needs w, h");
      return;
    }
    auto wRaw = trim(sizePart.substr(0, commaSz));
    auto hRaw = trim(sizePart.substr(commaSz + 1));
    Expr w = toUpper(wRaw) == "AUTO" ? Expr{"-1.0", Ty::num()} : bc.expr(wRaw, line);
    Expr h = toUpper(hRaw) == "AUTO" ? Expr{"-1.0", Ty::num()} : bc.expr(hRaw, line);
    bc.expectTy(line, w.ty, Ty::num(), "SLOT SIZE w");
    bc.expectTy(line, h.ty, Ty::num(), "SLOT SIZE h");
    Expr ox{"0", Ty::num()}, oy{"0", Ty::num()};
    if (hasAt) {
      auto commaAt = findOutsideQuotes(atPart, toUpper(atPart), ",");
      if (commaAt == std::string::npos) {
        bc.fail(line, "SLOT AT needs ox, oy");
        return;
      }
      ox = bc.expr(trim(atPart.substr(0, commaAt)), line);
      oy = bc.expr(trim(atPart.substr(commaAt + 1)), line);
      bc.expectTy(line, ox.ty, Ty::num(), "SLOT AT ox");
      bc.expectTy(line, oy.ty, Ty::num(), "SLOT AT oy");
    }
    if (kindRaw == "TEXT") {
      auto t = bc.coerceText(bc.expr(trail, line));
      if (hasAt)
        o << "  hanka_slot_text_at(arena, " << idE.code << ", " << ox.code << ", " << oy.code << ", "
          << w.code << ", " << h.code << ", " << t.code << ");\n";
      else
        o << "  hanka_slot_text(arena, " << idE.code << ", " << w.code << ", " << h.code << ", "
          << t.code << ");\n";
    } else if (kindRaw == "BUTTON") {
      auto t = bc.coerceText(bc.expr(trail, line));
      if (hasAt)
        o << "  hanka_slot_button_at(arena, " << idE.code << ", " << ox.code << ", " << oy.code
          << ", " << w.code << ", " << h.code << ", " << t.code << ");\n";
      else
        o << "  hanka_slot_button(arena, " << idE.code << ", " << w.code << ", " << h.code << ", "
          << t.code << ");\n";
    } else if (kindRaw == "IMAGE") {
      auto t = bc.coerceText(bc.expr(trail, line));
      if (hasAt)
        o << "  hanka_slot_image_at(arena, " << idE.code << ", " << ox.code << ", " << oy.code
          << ", " << w.code << ", " << h.code << ", " << t.code << ");\n";
      else
        o << "  hanka_slot_image(arena, " << idE.code << ", " << w.code << ", " << h.code << ", "
          << t.code << ");\n";
    } else if (kindRaw == "BOX") {
      if (hasAt)
        o << "  hanka_slot_box_at(arena, " << idE.code << ", " << ox.code << ", " << oy.code << ", "
          << w.code << ", " << h.code << ");\n";
      else
        o << "  hanka_slot_box(arena, " << idE.code << ", " << w.code << ", " << h.code << ");\n";
    } else if (kindRaw == "INPUT") {
      auto t = bc.coerceText(bc.expr(trail.empty() ? "\"\"" : trail, line));
      if (hasAt)
        o << "  hanka_slot_input_at(arena, " << idE.code << ", " << ox.code << ", " << oy.code
          << ", " << w.code << ", " << h.code << ", " << t.code << ", " << inputType << ");\n";
      else
        o << "  hanka_slot_input(arena, " << idE.code << ", " << w.code << ", " << h.code << ", "
          << t.code << ", " << inputType << ");\n";
    } else if (kindRaw == "SELECT" || kindRaw == "DROPDOWN") {
      auto t = bc.coerceText(bc.expr(trail.empty() ? "\"\"" : trail, line));
      o << "  hanka_slot_select(arena, " << idE.code << ", " << w.code << ", " << h.code << ", "
        << t.code << ");\n";
    } else if (kindRaw == "TABLE") {
      auto t = bc.coerceText(bc.expr(trail.empty() ? "\"\"" : trail, line));
      o << "  hanka_slot_table(arena, " << idE.code << ", " << w.code << ", " << h.code << ", "
        << t.code << ");\n";
    } else if (kindRaw == "MODAL" || kindRaw == "DIALOG") {
      auto t = bc.coerceText(bc.expr(trail.empty() ? "\"\"" : trail, line));
      o << "  hanka_slot_modal(arena, " << idE.code << ", " << w.code << ", " << h.code << ", "
        << t.code << ");\n";
    } else {
      bc.fail(line, "SLOT needs TEXT, BUTTON, IMAGE, BOX, INPUT, SELECT, TABLE, or MODAL — got " +
                         kindRaw);
      return;
    }
    if (liveLevel) o << "  hanka_leaf_set_live(arena, " << liveLevel << ");\n";
    if (!wearRaw.empty()) {
      auto classes = bc.coerceText(bc.expr(wearRaw, line));
      o << "  hanka_leaf_set_class(arena, " << classes.code << ");\n";
    }
    return;
  }

  /* Argus — conversational scene presentment (DOM backend) */
  if (toUpper(text) == "PAINT THE SCREEN" || toUpper(text) == "PAINT SCREEN") {
    o << "  argus_paint(arena);\n";
    return;
  }
  if (toUpper(text) == "CLEAR THE SCREEN" || toUpper(text) == "CLEAR SCREEN") {
    o << "  hanka_clear(arena);\n";
    o << "  argus_clear(arena);\n";
    return;
  }
  if (toUpper(text) == "RESET THE BENCH" || toUpper(text) == "RESET BENCH" ||
      toUpper(text) == "RESET THE BENCHMARK") {
    o << "  luke_bench_reset();\n";
    return;
  }
  if (startsWithCI(text, "RECORD BENCH SAMPLE ") || startsWithCI(text, "RECORD BENCHMARK SAMPLE ")) {
    auto rest = startsWithCI(text, "RECORD BENCH SAMPLE ") ? trim(text.substr(20))
                                                           : trim(text.substr(25));
    auto e = bc.expr(rest, line);
    bc.expectTy(line, e.ty, Ty::num(), "RECORD BENCH SAMPLE");
    o << "  luke_bench_push(" << e.code << ");\n";
    return;
  }
  if (startsWithCI(text, "SET THE OPACITY OF ") || startsWithCI(text, "SET OPACITY OF ")) {
    auto rest = startsWithCI(text, "SET THE OPACITY OF ") ? trim(text.substr(18))
                                                          : trim(text.substr(14));
    auto U = toUpper(rest);
    auto toPos = findOutsideQuotes(rest, U, " TO ");
    if (toPos == std::string::npos) {
      bc.fail(line, "SET THE OPACITY OF needs: SET THE OPACITY OF \"id\" TO 0.5");
      return;
    }
    auto idE = bc.coerceText(bc.expr(trim(rest.substr(0, toPos)), line));
    auto opE = bc.expr(trim(rest.substr(toPos + 4)), line);
    bc.expectTy(line, opE.ty, Ty::num(), "SET THE OPACITY OF … TO");
    o << "  argus_set_opacity_id(arena, " << idE.code << ", " << opE.code << ");\n";
    return;
  }
  if (startsWithCI(text, "FADE ")) {
    auto rest = trim(text.substr(5));
    auto U = toUpper(rest);
    auto fromPos = findOutsideQuotes(rest, U, " FROM ");
    auto toPos = findOutsideQuotes(rest, U, " TO ");
    auto overPos = findOutsideQuotes(rest, U, " OVER ");
    if (toPos == std::string::npos) {
      bc.fail(line, "FADE needs: FADE \"id\" [FROM 0] TO 1 [OVER 300]");
      return;
    }
    std::string idRaw = trim(rest.substr(0, fromPos != std::string::npos && fromPos < toPos
                                                ? fromPos
                                                : toPos));
    auto idE = bc.coerceText(bc.expr(idRaw, line));
    Expr fromE{"-1.0", Ty::num()};
    if (fromPos != std::string::npos && fromPos < toPos) {
      fromE = bc.expr(trim(rest.substr(fromPos + 6, toPos - (fromPos + 6))), line);
      bc.expectTy(line, fromE.ty, Ty::num(), "FADE FROM");
    }
    std::string afterTo =
        overPos != std::string::npos && overPos > toPos
            ? trim(rest.substr(toPos + 4, overPos - (toPos + 4)))
            : trim(rest.substr(toPos + 4));
    auto toE = bc.expr(afterTo, line);
    bc.expectTy(line, toE.ty, Ty::num(), "FADE TO");
    Expr msE{"300.0", Ty::num()};
    if (overPos != std::string::npos && overPos > toPos) {
      auto overRest = trim(rest.substr(overPos + 6));
      auto ou = toUpper(overRest);
      if (ou.size() >= 3 && ou.compare(ou.size() - 3, 3, " MS") == 0)
        overRest = trim(overRest.substr(0, overRest.size() - 3));
      else if (ou.size() >= 13 && ou.compare(ou.size() - 13, 13, " MILLISECONDS") == 0)
        overRest = trim(overRest.substr(0, overRest.size() - 13));
      msE = bc.expr(overRest, line);
      bc.expectTy(line, msE.ty, Ty::num(), "FADE OVER");
    }
    o << "  argus_fade_to(arena, " << idE.code << ", " << fromE.code << ", " << toE.code << ", "
      << msE.code << ");\n";
    return;
  }
  /* a11y — focus trap / restore / live-region announce */
  if (startsWithCI(text, "TRAP FOCUS IN ") || startsWithCI(text, "TRAP FOCUS ON ")) {
    auto rest = startsWithCI(text, "TRAP FOCUS IN ") ? trim(text.substr(14)) : trim(text.substr(14));
    auto idE = bc.coerceText(bc.expr(rest, line));
    o << "  argus_js_focus_trap(" << idE.code << ");\n";
    return;
  }
  if (toUpper(text) == "RESTORE FOCUS" || toUpper(text) == "RELEASE FOCUS") {
    o << "  argus_js_focus_restore();\n";
    return;
  }
  if (startsWithCI(text, "ANNOUNCE ")) {
    auto rest = trim(text.substr(9));
    auto msg = bc.coerceText(bc.expr(rest, line));
    o << "  argus_js_announce(" << msg.code << ");\n";
    return;
  }
  /* Modal show/hide — trap on OPEN, not on mount */
  if (startsWithCI(text, "OPEN THE MODAL ") || startsWithCI(text, "SHOW THE MODAL ") ||
      startsWithCI(text, "OPEN MODAL ") || startsWithCI(text, "SHOW MODAL ")) {
    auto rest = startsWithCI(text, "OPEN THE MODAL ")   ? trim(text.substr(15))
                : startsWithCI(text, "SHOW THE MODAL ") ? trim(text.substr(15))
                : startsWithCI(text, "OPEN MODAL ")     ? trim(text.substr(11))
                                                        : trim(text.substr(11));
    auto idE = bc.coerceText(bc.expr(rest, line));
    o << "  argus_open_modal(arena, " << idE.code << ");\n";
    return;
  }
  if (startsWithCI(text, "CLOSE THE MODAL ") || startsWithCI(text, "HIDE THE MODAL ") ||
      startsWithCI(text, "CLOSE MODAL ") || startsWithCI(text, "HIDE MODAL ")) {
    auto rest = startsWithCI(text, "CLOSE THE MODAL ")  ? trim(text.substr(16))
                : startsWithCI(text, "HIDE THE MODAL ") ? trim(text.substr(15))
                : startsWithCI(text, "CLOSE MODAL ")    ? trim(text.substr(12))
                                                        : trim(text.substr(11));
    auto idE = bc.coerceText(bc.expr(rest, line));
    o << "  argus_close_modal(arena, " << idE.code << ");\n";
    return;
  }
  if (startsWithCI(text, "PLACE ")) {
    auto rest = trim(text.substr(6));
    auto U = toUpper(rest);
    auto asPos = findOutsideQuotes(rest, U, " AS ");
    auto atPos = findOutsideQuotes(rest, U, " AT ");
    auto sizePos = findOutsideQuotes(rest, U, " SIZE ");
    if (asPos == std::string::npos || atPos == std::string::npos || sizePos == std::string::npos ||
        !(asPos < atPos && atPos < sizePos)) {
      bc.fail(line,
              "PLACE needs: PLACE \"id\" AS TEXT|BUTTON|IMAGE|BOX|INPUT|SELECT|TABLE|MODAL AT x, y "
              "SIZE w, h …");
      return;
    }
    auto idE = bc.coerceText(bc.expr(trim(rest.substr(0, asPos)), line));
    auto kindRaw = toUpper(trim(rest.substr(asPos + 4, atPos - (asPos + 4))));
    auto atPart = trim(rest.substr(atPos + 4, sizePos - (atPos + 4)));
    auto afterSize = trim(rest.substr(sizePos + 6));
    int inputType = 0;
    {
      auto aU = toUpper(afterSize);
      size_t p = findOutsideQuotes(afterSize, aU, " AS PASSWORD");
      if (p != std::string::npos) {
        inputType = 1;
        afterSize = trim(trim(afterSize.substr(0, p)) + " " + trim(afterSize.substr(p + 12)));
      } else if ((p = findOutsideQuotes(afterSize, aU, " AS EMAIL")) != std::string::npos) {
        inputType = 2;
        afterSize = trim(trim(afterSize.substr(0, p)) + " " + trim(afterSize.substr(p + 9)));
      } else if ((p = findOutsideQuotes(afterSize, aU, " AS CHECKBOX")) != std::string::npos) {
        inputType = 3;
        afterSize = trim(trim(afterSize.substr(0, p)) + " " + trim(afterSize.substr(p + 12)));
      } else if ((p = findOutsideQuotes(afterSize, aU, " AS RADIO")) != std::string::npos) {
        inputType = 4;
        afterSize = trim(trim(afterSize.substr(0, p)) + " " + trim(afterSize.substr(p + 9)));
      } else if ((p = findOutsideQuotes(afterSize, aU, " AS TEXT")) != std::string::npos) {
        inputType = 0;
        afterSize = trim(trim(afterSize.substr(0, p)) + " " + trim(afterSize.substr(p + 8)));
      }
    }
    auto sayPos = findOutsideQuotes(afterSize, toUpper(afterSize), " SAY ");
    auto fromPos = findOutsideQuotes(afterSize, toUpper(afterSize), " FROM ");
    std::string sizePart;
    std::string trail;
    if (sayPos != std::string::npos) {
      sizePart = trim(afterSize.substr(0, sayPos));
      trail = trim(afterSize.substr(sayPos + 5));
    } else if (fromPos != std::string::npos) {
      sizePart = trim(afterSize.substr(0, fromPos));
      trail = trim(afterSize.substr(fromPos + 6));
    } else {
      sizePart = afterSize;
    }
    auto commaAt = findOutsideQuotes(atPart, toUpper(atPart), ",");
    auto commaSz = findOutsideQuotes(sizePart, toUpper(sizePart), ",");
    if (commaAt == std::string::npos || commaSz == std::string::npos) {
      bc.fail(line, "PLACE AT needs x, y and SIZE needs w, h");
      return;
    }
    auto x = bc.expr(trim(atPart.substr(0, commaAt)), line);
    auto y = bc.expr(trim(atPart.substr(commaAt + 1)), line);
    auto wRaw = trim(sizePart.substr(0, commaSz));
    auto hRaw = trim(sizePart.substr(commaSz + 1));
    Expr w = toUpper(wRaw) == "AUTO" ? Expr{"-1.0", Ty::num()} : bc.expr(wRaw, line);
    Expr h = toUpper(hRaw) == "AUTO" ? Expr{"-1.0", Ty::num()} : bc.expr(hRaw, line);
    if (toUpper(wRaw) == "AUTO" && (kindRaw == "TEXT" || kindRaw == "BUTTON" || kindRaw == "MODAL")) {
      /* resolve at place-time via measure when trail known — use sentinel; paint path measures */
    }
    bc.expectTy(line, x.ty, Ty::num(), "PLACE AT x");
    bc.expectTy(line, y.ty, Ty::num(), "PLACE AT y");
    bc.expectTy(line, w.ty, Ty::num(), "PLACE SIZE w");
    bc.expectTy(line, h.ty, Ty::num(), "PLACE SIZE h");
    if (kindRaw == "TEXT") {
      auto t = bc.coerceText(bc.expr(trail, line));
      if (toUpper(wRaw) == "AUTO")
        o << "  argus_place_text(arena, " << idE.code << ", " << x.code << ", " << y.code
          << ", argus_measure_text(" << t.code << "), " << h.code << ", " << t.code << ");\n";
      else
        o << "  argus_place_text(arena, " << idE.code << ", " << x.code << ", " << y.code << ", "
          << w.code << ", " << h.code << ", " << t.code << ");\n";
    } else if (kindRaw == "BUTTON") {
      auto t = bc.coerceText(bc.expr(trail, line));
      o << "  argus_place_button(arena, " << idE.code << ", " << x.code << ", " << y.code << ", "
        << w.code << ", " << h.code << ", " << t.code << ");\n";
    } else if (kindRaw == "IMAGE") {
      auto t = bc.coerceText(bc.expr(trail, line));
      o << "  argus_place_image(arena, " << idE.code << ", " << x.code << ", " << y.code << ", "
        << w.code << ", " << h.code << ", " << t.code << ");\n";
    } else if (kindRaw == "BOX") {
      o << "  argus_place_box(arena, " << idE.code << ", " << x.code << ", " << y.code << ", "
        << w.code << ", " << h.code << ");\n";
    } else if (kindRaw == "INPUT") {
      auto t = bc.coerceText(bc.expr(trail.empty() ? "\"\"" : trail, line));
      o << "  argus_place_input(arena, " << idE.code << ", " << x.code << ", " << y.code << ", "
        << w.code << ", " << h.code << ", " << t.code << ", " << inputType << ");\n";
    } else if (kindRaw == "SELECT" || kindRaw == "DROPDOWN") {
      auto t = bc.coerceText(bc.expr(trail.empty() ? "\"\"" : trail, line));
      o << "  argus_place_select(arena, " << idE.code << ", " << x.code << ", " << y.code << ", "
        << w.code << ", " << h.code << ", " << t.code << ");\n";
    } else if (kindRaw == "TABLE") {
      auto t = bc.coerceText(bc.expr(trail.empty() ? "\"\"" : trail, line));
      o << "  argus_place_table(arena, " << idE.code << ", " << x.code << ", " << y.code << ", "
        << w.code << ", " << h.code << ", " << t.code << ");\n";
    } else if (kindRaw == "MODAL" || kindRaw == "DIALOG") {
      auto t = bc.coerceText(bc.expr(trail.empty() ? "\"\"" : trail, line));
      o << "  argus_place_modal(arena, " << idE.code << ", " << x.code << ", " << y.code << ", "
        << w.code << ", " << h.code << ", " << t.code << ");\n";
    } else {
      bc.fail(line, "PLACE AS needs TEXT, BUTTON, IMAGE, BOX, INPUT, SELECT, TABLE, or MODAL — got " +
                         kindRaw);
    }
    return;
  }

  bc.fail(line, "Unsupported Build statement: " + text +
                    " — Build doesn't understand this yet (Play-only feature?)");
  bc.unsupportedHint = true;
}

bool parse(BC &bc, const std::string &source) {
  /* Triple-quoted TEXT: """ … """ may span lines → single "…" with \n */
  std::string src;
  {
    size_t i = 0;
    while (i < source.size()) {
      if (i + 2 < source.size() && source[i] == '"' && source[i + 1] == '"' && source[i + 2] == '"') {
        i += 3;
        std::string body;
        while (i + 2 < source.size() &&
               !(source[i] == '"' && source[i + 1] == '"' && source[i + 2] == '"')) {
          char c = source[i++];
          if (c == '\\') {
            body.push_back('\\');
            if (i < source.size()) body.push_back(source[i++]);
          } else if (c == '"') {
            body += "\\\"";
          } else if (c == '\n') {
            body += "\\n";
          } else if (c == '\r') {
            /* skip */
          } else {
            body.push_back(c);
          }
        }
        if (i + 2 < source.size()) i += 3;
        src.push_back('"');
        src += body;
        src.push_back('"');
        continue;
      }
      src.push_back(source[i++]);
    }
  }

  std::istringstream in(src);
  std::string raw;
  size_t lineNo = 0;
  enum Mode { Top, InFn, InBp, InMeth, InWhen, InRxWhen, InFlow, InRoutes, InForm, InSchema,
               InMigration };
  Mode mode = Top;
  Fn curFn;
  BP curBp;
  Method curM;
  BrowserWhen curWhen;
  BC::RxWhenDef curRxWhen;
  BC::FlowDef curFlow;
  BC::FormDef curForm;
  BC::SchemaDef curSchema;
  BC::MigrationDef curMigration;
  bool skipContract = false;

  while (std::getline(in, raw)) {
    ++lineNo;
    auto text = trim(raw);
    {
      std::string markerPath;
      size_t markerLine = 0;
      if (parseLukeFileMarker(text, &markerPath, &markerLine)) {
        bc.curFile = std::move(markerPath);
        lineNo = markerLine - 1; /* next getline ++lineNo → markerLine */
        continue;
      }
    }
    if (text.empty() || startsWithCI(text, "//")) continue;
    if (startsWithCI(text, "LET'S START") || startsWithCI(text, "LETS START") ||
        startsWithCI(text, "GET OUTTA HERE"))
      continue;
    if (skipContract) {
      if (toUpper(text) == "END CONTRACT" || toUpper(text) == "ENDCONTRACT") skipContract = false;
      continue;
    }
    if (startsWithCI(text, "CONTRACT ")) {
      skipContract = true;
      continue;
    }

    if (mode == Top) {
      if (startsWithCI(text, "FOREIGN FUNCTION ") || startsWithCI(text, "FOREIGN ")) {
        std::string rest =
            startsWithCI(text, "FOREIGN FUNCTION ") ? trim(text.substr(17)) : trim(text.substr(8));
        stripDo(rest);
        Fn f;
        f.foreign = true;
        auto U = toUpper(rest);
        Ty declaredRet = Ty::num();
        auto gb = U.find(" GIVES BACK ");
        if (gb == std::string::npos) gb = U.find(" RETURNS ");
        if (gb != std::string::npos) {
          size_t kwLen = (U.compare(gb, 12, " GIVES BACK ") == 0) ? 12 : 9;
          auto tyRaw = trim(rest.substr(gb + kwLen));
          declaredRet = bc.parseTy(tyRaw);
          if (declaredRet.k == K::Void) {
            bc.fail(lineNo, "Unknown FOREIGN return type '" + tyRaw + "'");
            return false;
          }
          rest = trim(rest.substr(0, gb));
          U = toUpper(rest);
        }
        f.ret = declaredRet;
        f.retDeclared = true;
        auto w = U.find(" WITH ");
        if (w == std::string::npos) f.name = rest;
        else {
          f.name = trim(rest.substr(0, w));
          for (auto &a : splitArgs(trim(rest.substr(w + 6)))) {
            auto p = bc.parseParam(a);
            if (p.ty.k == K::Void) {
              bc.fail(lineNo, "Unknown FOREIGN parameter type on '" + p.name + "'");
              return false;
            }
            f.params.push_back(p);
          }
        }
        if (f.name.empty()) {
          bc.fail(lineNo, "FOREIGN FUNCTION needs a name");
          return false;
        }
        bc.fns[f.name] = f;
        bc.fnOrder.push_back(f.name);
        continue;
      }
      if (startsWithCI(text, "THIS IS FUNCTION ") || startsWithCI(text, "MAKE FUNCTION ") ||
          startsWithCI(text, "RECIPE ")) {
        std::string rest;
        if (startsWithCI(text, "THIS IS FUNCTION ")) rest = trim(text.substr(17));
        else if (startsWithCI(text, "MAKE FUNCTION ")) rest = trim(text.substr(14));
        else rest = trim(text.substr(7));
        stripDo(rest);
        curFn = {};
        auto U = toUpper(rest);
        Ty declaredRet = Ty::vod();
        auto gb = U.find(" GIVES BACK ");
        if (gb == std::string::npos) gb = U.find(" RETURNS ");
        if (gb != std::string::npos) {
          size_t kwLen = (U.compare(gb, 12, " GIVES BACK ") == 0) ? 12 : 9;
          auto tyRaw = trim(rest.substr(gb + kwLen));
          declaredRet = bc.parseTy(tyRaw);
          if (declaredRet.k == K::Void) {
            bc.fail(lineNo, "Unknown return type '" + tyRaw +
                                "' — use NUMBER, TEXT, FLAG, JSON, or a blueprint");
            return false;
          }
          rest = trim(rest.substr(0, gb));
          U = toUpper(rest);
        }
        auto w = U.find(" WITH ");
        if (w == std::string::npos) curFn.name = rest;
        else {
          curFn.name = trim(rest.substr(0, w));
          for (auto &a : splitArgs(trim(rest.substr(w + 6)))) {
            auto p = bc.parseParam(a);
            if (p.ty.k == K::Void) {
              bc.fail(lineNo, "Unknown parameter type on '" + p.name +
                                  "' — use AS NUMBER/TEXT/FLAG/JSON or a blueprint");
              return false;
            }
            curFn.params.push_back(p);
          }
        }
        curFn.ret = declaredRet.k == K::Void ? Ty::num() : declaredRet;
        curFn.retDeclared = declaredRet.k != K::Void;
        mode = InFn;
        continue;
      }
      if (startsWithCI(text, "BLUEPRINT ") || startsWithCI(text, "CLASS ")) {
        std::string rest =
            startsWithCI(text, "BLUEPRINT ") ? trim(text.substr(10)) : trim(text.substr(6));
        stripDo(rest);
        auto impl = toUpper(rest).find(" IMPLEMENTS ");
        if (impl != std::string::npos) rest = trim(rest.substr(0, impl));
        curBp = {};
        auto U = toUpper(rest);
        auto fol = U.find(" FOLLOWS ");
        if (fol == std::string::npos) fol = U.find(" EXTENDS ");
        if (fol == std::string::npos) curBp.name = rest;
        else {
          curBp.name = trim(rest.substr(0, fol));
          auto parent = trim(rest.substr(fol));
          if (startsWithCI(parent, "FOLLOWS ")) parent = trim(parent.substr(8));
          else if (startsWithCI(parent, "EXTENDS ")) parent = trim(parent.substr(8));
          curBp.parent = parent;
        }
        mode = InBp;
        continue;
      }
      /* FLOW signup DO … END FLOW — auth state machine; VERIFY before DONE or compile error. */
      if (startsWithCI(text, "FLOW ")) {
        auto rest = trim(text.substr(5));
        stripDo(rest);
        /* Allow "FLOW signup:" */
        if (!rest.empty() && rest.back() == ':') rest.pop_back();
        rest = trim(rest);
        if (rest.empty()) {
          bc.fail(lineNo, "FLOW needs a name — FLOW signup DO");
          return false;
        }
        curFlow = {};
        curFlow.name = stripThe(rest);
        curFlow.line = lineNo;
        mode = InFlow;
        continue;
      }
      /* ROUTES DO … END ROUTES — declarative route table. */
      if (toUpper(text) == "ROUTES" || startsWithCI(text, "ROUTES ")) {
        stripDo(text);
        bc.hasRoutesBlock = true;
        mode = InRoutes;
        continue;
      }
      /* FORM login DO … END FORM */
      if (startsWithCI(text, "FORM ")) {
        auto rest = trim(text.substr(5));
        stripDo(rest);
        if (!rest.empty() && rest.back() == ':') rest.pop_back();
        rest = trim(rest);
        if (rest.empty()) {
          bc.fail(lineNo, "FORM needs a name — FORM login DO");
          return false;
        }
        curForm = {};
        curForm.name = stripThe(rest);
        curForm.line = lineNo;
        mode = InForm;
        continue;
      }
      /* SCHEMA notes DO … END SCHEMA */
      if (startsWithCI(text, "SCHEMA ")) {
        auto rest = trim(text.substr(7));
        stripDo(rest);
        if (!rest.empty() && rest.back() == ':') rest.pop_back();
        rest = trim(rest);
        if (rest.empty()) {
          bc.fail(lineNo, "SCHEMA needs a table name — SCHEMA notes DO");
          return false;
        }
        curSchema = {};
        curSchema.name = stripThe(rest);
        curSchema.line = lineNo;
        mode = InSchema;
        continue;
      }
      /* MIGRATION app DO … END MIGRATION — versioned UP/DOWN SQL. */
      if (startsWithCI(text, "MIGRATION ")) {
        auto rest = trim(text.substr(10));
        stripDo(rest);
        if (!rest.empty() && rest.back() == ':') rest.pop_back();
        rest = trim(rest);
        if (rest.empty()) {
          bc.fail(lineNo, "MIGRATION needs a name — MIGRATION app DO");
          return false;
        }
        curMigration = {};
        curMigration.name = stripThe(rest);
        curMigration.line = lineNo;
        mode = InMigration;
        continue;
      }
      if (startsWithCI(text, "WHEN BACKGROUND REACTIVE ") ||
          startsWithCI(text, "WHEN REACTIVE WEAK ") ||
          startsWithCI(text, "WHEN WEAK REACTIVE ") ||
          startsWithCI(text, "WHEN REACTIVE ")) {
        bool bg = startsWithCI(text, "WHEN BACKGROUND REACTIVE ");
        bool weak = startsWithCI(text, "WHEN REACTIVE WEAK ") ||
                    startsWithCI(text, "WHEN WEAK REACTIVE ");
        size_t prefix = bg ? 25 : (weak ? (startsWithCI(text, "WHEN REACTIVE WEAK ") ? 19 : 18) : 14);
        auto rest = trim(text.substr(prefix));
        stripDo(rest);
        auto U = toUpper(rest);
        auto chPos = U.find(" CHANGES");
        if (chPos == std::string::npos) {
          bc.fail(lineNo, "WHEN REACTIVE needs: WHEN REACTIVE cell CHANGES DO");
          return false;
        }
        auto cellName = stripThe(trim(rest.substr(0, chPos)));
        if (cellName.empty()) {
          bc.fail(lineNo, "WHEN REACTIVE needs a cell name — WHEN REACTIVE count CHANGES DO");
          return false;
        }
        curRxWhen = {};
        curRxWhen.cellName = cellName;
        curRxWhen.background = bg;
        curRxWhen.weak = weak;
        curRxWhen.line = lineNo;
        mode = InRxWhen;
        continue;
      }
      if (startsWithCI(text, "WHEN ")) {
        curWhen = {};
        auto rest = trim(text.substr(5));
        stripDo(rest);
        auto U = toUpper(rest);
        if (startsWithCI(rest, "THE ROUTE IS ")) {
          curWhen.event = "route";
          curWhen.elementId = unquoteText(trim(rest.substr(13)));
          if (curWhen.elementId.empty()) {
            bc.fail(lineNo, "WHEN THE ROUTE IS needs a name — WHEN THE ROUTE IS \"home\" DO");
            return false;
          }
        } else if (startsWithCI(rest, "THE VIEWPORT IS BELOW ") ||
                   startsWithCI(rest, "THE WINDOW IS BELOW ")) {
          auto raw = startsWithCI(rest, "THE VIEWPORT IS BELOW ") ? trim(rest.substr(22))
                                                                  : trim(rest.substr(20));
          auto ru = toUpper(raw);
          if (ru.size() >= 5 && ru.compare(ru.size() - 5, 5, " WIDE") == 0)
            raw = trim(raw.substr(0, raw.size() - 5));
          if (raw.empty() || !isdigit((unsigned char)raw[0])) {
            bc.fail(lineNo, "WHEN THE VIEWPORT IS BELOW needs a width number");
            return false;
          }
          curWhen.event = "viewport";
          curWhen.elementId = "below:" + raw;
        } else if (startsWithCI(rest, "THE VIEWPORT IS ABOVE ") ||
                   startsWithCI(rest, "THE WINDOW IS ABOVE ")) {
          auto raw = startsWithCI(rest, "THE VIEWPORT IS ABOVE ") ? trim(rest.substr(22))
                                                                  : trim(rest.substr(20));
          auto ru = toUpper(raw);
          if (ru.size() >= 5 && ru.compare(ru.size() - 5, 5, " WIDE") == 0)
            raw = trim(raw.substr(0, raw.size() - 5));
          if (raw.empty() || !isdigit((unsigned char)raw[0])) {
            bc.fail(lineNo, "WHEN THE VIEWPORT IS ABOVE needs a width number");
            return false;
          }
          curWhen.event = "viewport";
          curWhen.elementId = "above:" + raw;
        } else if (startsWithCI(rest, "THE VIEWPORT IS AT LEAST ") ||
                   startsWithCI(rest, "THE WINDOW IS AT LEAST ")) {
          /* Declarative breakpoint: WHEN THE VIEWPORT IS AT LEAST 800 [WIDE] DO */
          auto raw = startsWithCI(rest, "THE VIEWPORT IS AT LEAST ")
                         ? trim(rest.substr(25))
                         : trim(rest.substr(23));
          auto ru = toUpper(raw);
          if (ru.size() >= 5 && ru.compare(ru.size() - 5, 5, " WIDE") == 0)
            raw = trim(raw.substr(0, raw.size() - 5));
          if (raw.empty() || !isdigit((unsigned char)raw[0])) {
            bc.fail(lineNo, "WHEN THE VIEWPORT IS AT LEAST needs a width number");
            return false;
          }
          curWhen.event = "breakpoint";
          curWhen.elementId = "min:" + raw;
        } else if (startsWithCI(rest, "THE VIEWPORT IS UNDER ") ||
                   startsWithCI(rest, "THE WINDOW IS UNDER ")) {
          auto raw = startsWithCI(rest, "THE VIEWPORT IS UNDER ") ? trim(rest.substr(22))
                                                                : trim(rest.substr(20));
          auto ru = toUpper(raw);
          if (ru.size() >= 5 && ru.compare(ru.size() - 5, 5, " WIDE") == 0)
            raw = trim(raw.substr(0, raw.size() - 5));
          if (raw.empty() || !isdigit((unsigned char)raw[0])) {
            bc.fail(lineNo, "WHEN THE VIEWPORT IS UNDER needs a width number");
            return false;
          }
          curWhen.event = "breakpoint";
          curWhen.elementId = "max:" + raw;
        } else if (startsWithCI(rest, "THE VIEWPORT IS BETWEEN ") ||
                   startsWithCI(rest, "THE WINDOW IS BETWEEN ")) {
          auto raw = startsWithCI(rest, "THE VIEWPORT IS BETWEEN ") ? trim(rest.substr(24))
                                                                  : trim(rest.substr(22));
          auto ru = toUpper(raw);
          auto andPos = ru.find(" AND ");
          if (andPos == std::string::npos) {
            bc.fail(lineNo, "WHEN THE VIEWPORT IS BETWEEN needs: BETWEEN lo AND hi [WIDE]");
            return false;
          }
          auto lo = trim(raw.substr(0, andPos));
          auto hi = trim(raw.substr(andPos + 5));
          auto hu = toUpper(hi);
          if (hu.size() >= 5 && hu.compare(hu.size() - 5, 5, " WIDE") == 0)
            hi = trim(hi.substr(0, hi.size() - 5));
          if (lo.empty() || hi.empty() || !isdigit((unsigned char)lo[0]) ||
              !isdigit((unsigned char)hi[0])) {
            bc.fail(lineNo, "WHEN THE VIEWPORT IS BETWEEN needs numeric lo AND hi");
            return false;
          }
          curWhen.event = "breakpoint";
          curWhen.elementId = "min:" + lo + ":max:" + hi;
        } else if (startsWithCI(rest, "THE VIEWPORT CHANGES") ||
                   startsWithCI(rest, "THE WINDOW CHANGES") ||
                   toUpper(rest) == "THE VIEWPORT CHANGES" ||
                   startsWithCI(rest, "THE VIEWPORT IS CHANGED")) {
          curWhen.event = "viewport";
          curWhen.elementId = "";
        } else if (startsWithCI(rest, "TIMELINE ") && U.find(" IS FINISHED") != std::string::npos) {
          curWhen.event = "timeline";
          auto fin = U.find(" IS FINISHED");
          curWhen.elementId = unquoteText(trim(rest.substr(9, fin - 9)));
          if (curWhen.elementId.empty()) {
            bc.fail(lineNo, "WHEN TIMELINE … IS FINISHED needs a timeline id");
            return false;
          }
        } else if (startsWithCI(rest, "FETCH ") && U.find(" IS READY") != std::string::npos) {
          curWhen.event = "fetch";
          auto ready = U.find(" IS READY");
          curWhen.elementId = unquoteText(trim(rest.substr(6, ready - 6)));
          if (curWhen.elementId.empty()) {
            bc.fail(lineNo, "WHEN FETCH … IS READY needs a job id");
            return false;
          }
        } else if (startsWithCI(rest, "SUBSCRIBE ") && U.find(" IS READY") != std::string::npos) {
          curWhen.event = "subscribe";
          auto ready = U.find(" IS READY");
          curWhen.elementId = unquoteText(trim(rest.substr(10, ready - 10)));
          if (curWhen.elementId.empty()) {
            bc.fail(lineNo, "WHEN SUBSCRIBE … IS READY needs a job id");
            return false;
          }
        } else if (startsWithCI(rest, "WATCH ") && U.find(" IS READY") != std::string::npos) {
          /* Live Graph: WHEN WATCH cell IS READY → same continuation as SUBSCRIBE job=cell */
          curWhen.event = "subscribe";
          auto ready = U.find(" IS READY");
          curWhen.elementId = unquoteText(trim(rest.substr(6, ready - 6)));
          if (curWhen.elementId.empty()) {
            bc.fail(lineNo, "WHEN WATCH … IS READY needs a cell name");
            return false;
          }
        } else {
          size_t evPos = std::string::npos;
          if ((evPos = U.find(" IS CLICKED")) != std::string::npos)
            curWhen.event = "click";
          else if ((evPos = U.find(" IS CHANGED")) != std::string::npos)
            curWhen.event = "change";
          else if ((evPos = U.find(" IS SUBMITTED")) != std::string::npos)
            curWhen.event = "submit";
          else {
            bc.fail(lineNo, "WHEN needs IS CLICKED|CHANGED|SUBMITTED, THE ROUTE IS, "
                            "THE VIEWPORT CHANGES / IS BELOW|ABOVE|AT LEAST|UNDER|BETWEEN …, "
                            "TIMELINE … IS FINISHED, FETCH … IS READY, "
                            "SUBSCRIBE … IS READY, or WATCH … IS READY");
            return false;
          }
          curWhen.elementId = unquoteText(trim(rest.substr(0, evPos)));
          if (curWhen.elementId.empty()) {
            bc.fail(lineNo, "WHEN needs an element id — WHEN \"cta\" IS CLICKED DO");
            return false;
          }
        }
        if (curWhen.event.empty()) curWhen.event = "click";
        curWhen.exportName = "luke_when_" + std::to_string(bc.whenSeq++);
        mode = InWhen;
        continue;
      }
      pushTop(bc, lineNo, text);
      continue;
    }

    if (mode == InRxWhen) {
      if (toUpper(text) == "END WHEN REACTIVE" || toUpper(text) == "ENDWHEN REACTIVE") {
        curRxWhen.seq = ++bc.rxWhenSeq;
        bc.rxWhenDefs.push_back(curRxWhen);
        pushTop(bc, lineNo, "REACTIVE WATCH REGISTER " + std::to_string(curRxWhen.seq));
        mode = Top;
        continue;
      }
      curRxWhen.body.push_back(text);
      curRxWhen.lines.push_back(lineNo);
      curRxWhen.files.push_back(bc.curFile);
      continue;
    }

    if (mode == InWhen) {
      if (toUpper(text) == "END WHEN" || toUpper(text) == "ENDWHEN") {
        bc.pageWhens.push_back(curWhen);
        bc.hasPage = true;
        mode = Top;
        continue;
      }
      curWhen.body.push_back(text);
      curWhen.lines.push_back(lineNo);
      curWhen.files.push_back(bc.curFile);
      continue;
    }

    if (mode == InRoutes) {
      if (toUpper(text) == "END ROUTES" || toUpper(text) == "ENDROUTES") {
        for (auto &r : bc.routes) {
          if (r.touchesSecret && !r.requiresAuth) {
            bc.fail(r.line, "ROUTE " + r.method + " \"" + r.pattern +
                                "\" touches SECRET without REQUIRES AUTH — compile error");
            return false;
          }
        }
        if (bc.routes.empty()) {
          bc.fail(lineNo, "ROUTES table is empty — add GET/POST entries");
          return false;
        }
        /* Reactive current_route cell beachhead */
        pushTop(bc, lineNo, "REMEMBER current_route AS TEXT SET TO \"/\"");
        mode = Top;
        continue;
      }
      auto Uline = toUpper(text);
      (void)Uline;
      std::string method;
      std::string rest;
      if (startsWithCI(text, "GET ")) {
        method = "GET";
        rest = trim(text.substr(4));
      } else if (startsWithCI(text, "POST ")) {
        method = "POST";
        rest = trim(text.substr(5));
      } else if (startsWithCI(text, "PUT ")) {
        method = "PUT";
        rest = trim(text.substr(4));
      } else if (startsWithCI(text, "DELETE ")) {
        method = "DELETE";
        rest = trim(text.substr(7));
      } else {
        bc.fail(lineNo, "Inside ROUTES: GET/POST/PUT/DELETE \"/path\" [AS TYPE] [HANDLE name] "
                        "[REQUIRES AUTH] [TOUCHES SECRET]");
        return false;
      }
      std::string pattern;
      if (!rest.empty() && rest[0] == '"') {
        auto end = rest.find('"', 1);
        if (end == std::string::npos) {
          bc.fail(lineNo, "ROUTES path needs quotes — GET \"/user/:id\"");
          return false;
        }
        pattern = rest.substr(1, end - 1);
        rest = trim(rest.substr(end + 1));
      } else {
        auto sp = rest.find(' ');
        pattern = sp == std::string::npos ? rest : trim(rest.substr(0, sp));
        rest = sp == std::string::npos ? "" : trim(rest.substr(sp));
      }
      if (pattern.empty() || pattern[0] != '/') {
        bc.fail(lineNo, "ROUTES path must start with / — GET \"/health\"");
        return false;
      }
      BC::RouteDef rd;
      rd.method = method;
      rd.pattern = pattern;
      rd.line = lineNo;
      auto rU = toUpper(rest);
      auto asPos = rU.find(" AS ");
      if (asPos != std::string::npos) {
        auto after = trim(rest.substr(asPos + 4));
        auto aU = toUpper(after);
        size_t tyEnd = 0;
        while (tyEnd < aU.size() && (isalnum((unsigned char)aU[tyEnd]))) ++tyEnd;
        rd.paramTy = trim(aU.substr(0, tyEnd));
        rest = trim(after.substr(tyEnd));
        rU = toUpper(rest);
        auto colon = pattern.find(':');
        if (colon != std::string::npos) {
          size_t e = colon + 1;
          while (e < pattern.size() && (isalnum((unsigned char)pattern[e]) || pattern[e] == '_'))
            ++e;
          rd.paramName = pattern.substr(colon + 1, e - (colon + 1));
        }
        if (rd.paramTy != "INTEGER" && rd.paramTy != "TEXT" && rd.paramTy != "NUMBER" &&
            rd.paramTy != "FLAG") {
          bc.fail(lineNo, "ROUTE param type must be INTEGER, TEXT, NUMBER, or FLAG — got " +
                              rd.paramTy);
          return false;
        }
      }
      if (rU.find("REQUIRES AUTH") != std::string::npos ||
          rU.find("REQUIRE AUTH") != std::string::npos)
        rd.requiresAuth = true;
      if (rU.find("TOUCHES SECRET") != std::string::npos)
        rd.touchesSecret = true;
      /* HANDLE name — optional until SERVE ROUTES */
      auto hU = toUpper(rest);
      auto handPos = hU.find(" HANDLE ");
      if (handPos == std::string::npos && startsWithCI(rest, "HANDLE ")) handPos = 0;
      if (handPos != std::string::npos) {
        auto hrest = handPos == 0 ? trim(rest.substr(7)) : trim(rest.substr(handPos + 8));
        auto sp = hrest.find(' ');
        rd.handler = sp == std::string::npos ? hrest : trim(hrest.substr(0, sp));
        if (rd.handler.empty()) {
          bc.fail(lineNo, "HANDLE needs a function name — GET \"/ok\" HANDLE health");
          return false;
        }
      }
      bc.routes.push_back(rd);
      continue;
    }

    if (mode == InForm) {
      if (toUpper(text) == "END FORM" || toUpper(text) == "ENDFORM") {
        if (curForm.fields.empty()) {
          bc.fail(curForm.line, "FORM '" + curForm.name + "' needs at least one HAS field");
          return false;
        }
        bc.forms[curForm.name] = curForm;
        bc.formOrder.push_back(curForm.name);
        /* Reactive error cells — server validation → UI BIND without extra invention. */
        for (auto &f : curForm.fields) {
          std::string errCell = "form_" + curForm.name + "_" + f.name + "_error";
          pushTop(bc, curForm.line, "REMEMBER " + errCell + " AS TEXT SET TO \"\"");
        }
        pushTop(bc, curForm.line, "REMEMBER form_" + curForm.name + "_ok AS TEXT SET TO \"1\"");
        mode = Top;
        continue;
      }
      if (startsWithCI(text, "HAS ")) {
        auto rest = trim(text.substr(4));
        auto U = toUpper(rest);
        auto asPos = U.find(" AS ");
        if (asPos == std::string::npos) {
          bc.fail(lineNo, "FORM field needs — HAS email AS EMAIL");
          return false;
        }
        BC::FormField ff;
        ff.name = stripThe(trim(rest.substr(0, asPos)));
        auto after = trim(rest.substr(asPos + 4));
        auto aU = toUpper(after);
        /* INTEGER 0..150 */
        auto dots = aU.find("..");
        if (startsWithCI(after, "INTEGER") && dots != std::string::npos) {
          ff.ty = "INTEGER";
          auto range = trim(after.substr(7));
          auto d2 = range.find("..");
          ff.minV = atol(trim(range.substr(0, d2)).c_str());
          ff.maxV = atol(trim(range.substr(d2 + 2)).c_str());
          ff.hasRange = true;
        } else {
          auto sp = aU.find(' ');
          ff.ty = sp == std::string::npos ? aU : trim(aU.substr(0, sp));
        }
        if (ff.ty != "TEXT" && ff.ty != "EMAIL" && ff.ty != "INTEGER" && ff.ty != "PASSWORD" &&
            ff.ty != "NUMBER" && ff.ty != "FLAG") {
          bc.fail(lineNo, "FORM field type must be TEXT, EMAIL, INTEGER, PASSWORD — got " + ff.ty);
          return false;
        }
        curForm.fields.push_back(ff);
        continue;
      }
      bc.fail(lineNo, "Inside FORM: only HAS … / END FORM");
      return false;
    }

    if (mode == InSchema) {
      if (toUpper(text) == "END SCHEMA" || toUpper(text) == "ENDSCHEMA") {
        if (curSchema.fields.empty()) {
          bc.fail(curSchema.line, "SCHEMA '" + curSchema.name + "' needs at least one HAS field");
          return false;
        }
        bc.schemas[curSchema.name] = curSchema;
        bc.schemaOrder.push_back(curSchema.name);
        mode = Top;
        continue;
      }
      if (startsWithCI(text, "HAS ")) {
        auto rest = trim(text.substr(4));
        auto U = toUpper(rest);
        auto asPos = U.find(" AS ");
        if (asPos == std::string::npos) {
          bc.fail(lineNo, "SCHEMA field needs — HAS body AS TEXT");
          return false;
        }
        BC::SchemaField sf;
        sf.name = stripThe(trim(rest.substr(0, asPos)));
        auto ty = toUpper(trim(rest.substr(asPos + 4)));
        if (ty == "INTEGER" || ty == "INT")
          sf.sqlTy = "INTEGER";
        else if (ty == "TEXT" || ty == "STRING")
          sf.sqlTy = "TEXT";
        else if (ty == "NUMBER" || ty == "REAL" || ty == "FLOAT")
          sf.sqlTy = "REAL";
        else {
          bc.fail(lineNo, "SCHEMA type must be INTEGER, TEXT, or NUMBER — got " + ty +
                              " (breaking/unknown schema type = compile error)");
          return false;
        }
        curSchema.fields.push_back(sf);
        continue;
      }
      bc.fail(lineNo, "Inside SCHEMA: only HAS … / END SCHEMA");
      return false;
    }

    if (mode == InMigration) {
      if (toUpper(text) == "END MIGRATION" || toUpper(text) == "ENDMIGRATION") {
        if (curMigration.steps.empty()) {
          bc.fail(curMigration.line,
                  "MIGRATION '" + curMigration.name + "' needs at least one VERSION … UP … DOWN");
          return false;
        }
        if (bc.migrations.count(curMigration.name)) {
          bc.fail(curMigration.line, "MIGRATION '" + curMigration.name + "' already declared");
          return false;
        }
        bc.migrations[curMigration.name] = curMigration;
        bc.migrationOrder.push_back(curMigration.name);
        mode = Top;
        continue;
      }
      if (startsWithCI(text, "VERSION ")) {
        auto rest = trim(text.substr(8));
        auto U = toUpper(rest);
        int ver = 0;
        size_t i = 0;
        while (i < rest.size() && isdigit((unsigned char)rest[i])) {
          ver = ver * 10 + (rest[i] - '0');
          ++i;
        }
        if (ver <= 0) {
          bc.fail(lineNo, "VERSION needs a positive integer — VERSION 1 UP \"…\" DOWN \"…\"");
          return false;
        }
        rest = trim(rest.substr(i));
        U = toUpper(rest);
        size_t upPos = std::string::npos;
        size_t downPos = U.find(" DOWN ");
        if (startsWithCI(rest, "UP "))
          upPos = 0;
        else
          upPos = U.find(" UP ");
        if (upPos == std::string::npos || downPos == std::string::npos ||
            (upPos != 0 && downPos < upPos)) {
          bc.fail(lineNo, "VERSION needs — VERSION 1 UP \"CREATE…\" DOWN \"DROP…\"");
          return false;
        }
        std::string upRest;
        if (upPos == 0)
          upRest = trim(rest.substr(3, downPos - 3));
        else
          upRest = trim(rest.substr(upPos + 4, downPos - (upPos + 4)));
        auto downRest = trim(rest.substr(downPos + 6));
        auto takeQuoted = [&](const std::string &s, std::string &out) -> bool {
          auto t = trim(s);
          if (t.empty() || t[0] != '"') return false;
          auto end = t.find('"', 1);
          if (end == std::string::npos) return false;
          out = t.substr(1, end - 1);
          return true;
        };
        BC::MigrateStep st;
        st.version = ver;
        st.line = lineNo;
        if (!takeQuoted(upRest, st.upSql) || !takeQuoted(downRest, st.downSql)) {
          bc.fail(lineNo, "VERSION UP/DOWN SQL must be quoted strings");
          return false;
        }
        if (st.upSql.empty() || st.downSql.empty()) {
          bc.fail(lineNo, "VERSION UP and DOWN SQL cannot be empty");
          return false;
        }
        for (auto &ex : curMigration.steps) {
          if (ex.version == ver) {
            bc.fail(lineNo, "MIGRATION duplicate VERSION " + std::to_string(ver));
            return false;
          }
        }
        curMigration.steps.push_back(st);
        continue;
      }
      bc.fail(lineNo, "Inside MIGRATION: only VERSION n UP \"…\" DOWN \"…\" / END MIGRATION");
      return false;
    }

    if (mode == InFlow) {
      if (toUpper(text) == "END FLOW" || toUpper(text) == "ENDFLOW") {
        if (curFlow.hasDone && !curFlow.hasVerify) {
          bc.fail(curFlow.line, "FLOW '" + curFlow.name +
                                    "' — impossible auth state: DONE without VERIFY is a compile error");
          return false;
        }
        if (!curFlow.hasDone) {
          bc.fail(curFlow.line, "FLOW '" + curFlow.name +
                                    "' needs DONE → CREATE ACCOUNT / RESET PASSWORD / …");
          return false;
        }
        /* OAuth flows must VERIFY BY OAUTH (classic CSRF/state gaps → compile error beachhead). */
        auto actU = toUpper(curFlow.doneAction);
        if (actU.find("OAUTH") != std::string::npos && curFlow.verifyKind != "OAUTH") {
          bc.fail(curFlow.line, "FLOW '" + curFlow.name +
                                    "' OAuth DONE requires VERIFY … BY OAUTH — compile error");
          return false;
        }
        if (curFlow.steps.empty() || curFlow.steps[0] != "collect") {
          bc.fail(curFlow.line, "FLOW '" + curFlow.name + "' needs COLLECT before VERIFY/DONE");
          return false;
        }
        bc.flows[curFlow.name] = curFlow;
        bc.flowOrder.push_back(curFlow.name);
        /* Materialize reactive cells for the flow (resumable Live Graph beachhead). */
        std::string stepCell = curFlow.name + "_step";
        pushTop(bc, curFlow.line, "REMEMBER " + stepCell + " AS TEXT SET TO \"collect\"");
        for (auto &f : curFlow.collectFields) {
          auto Uf = toUpper(f);
          std::string cell = curFlow.name + "_" + f;
          if (Uf == "PASSWORD" || Uf == "PASS")
            pushTop(bc, curFlow.line, "SECRET REMEMBER " + cell + " AS TEXT");
          else
            pushTop(bc, curFlow.line, "REMEMBER " + cell + " AS TEXT");
        }
        mode = Top;
        continue;
      }
      if (startsWithCI(text, "COLLECT ")) {
        if (curFlow.hasVerify || curFlow.hasDone) {
          bc.fail(lineNo, "FLOW COLLECT must come before VERIFY/DONE");
          return false;
        }
        auto fields = trim(text.substr(8));
        std::string cur;
        auto flush = [&]() {
          auto n = stripThe(trim(cur));
          cur.clear();
          if (n.empty()) return;
          curFlow.collectFields.push_back(n);
        };
        for (char c : fields) {
          if (c == ',' || c == ' ') {
            flush();
          } else {
            cur.push_back(c);
          }
        }
        flush();
        if (curFlow.collectFields.empty()) {
          bc.fail(lineNo, "COLLECT needs field names — COLLECT email, password");
          return false;
        }
        if (curFlow.steps.empty() || curFlow.steps.back() != "collect")
          curFlow.steps.push_back("collect");
        continue;
      }
      if (startsWithCI(text, "VERIFY ")) {
        if (curFlow.hasDone) {
          bc.fail(lineNo, "FLOW VERIFY must come before DONE");
          return false;
        }
        auto vrest = trim(text.substr(7));
        auto vU = toUpper(vrest);
        auto byPos = vU.find(" BY ");
        if (byPos != std::string::npos) {
          auto kind = toUpper(trim(vrest.substr(byPos + 4)));
          auto sp = kind.find(' ');
          if (sp != std::string::npos) kind = trim(kind.substr(0, sp));
          if (kind != "CODE" && kind != "TOTP" && kind != "OAUTH" && kind != "EMAIL") {
            bc.fail(lineNo, "VERIFY BY needs CODE, TOTP, or OAUTH — got " + kind +
                                " (unknown verify kind = compile error)");
            return false;
          }
          if (kind == "EMAIL") kind = "CODE";
          curFlow.verifyKind = kind;
        } else {
          curFlow.verifyKind = "CODE";
        }
        curFlow.hasVerify = true;
        if (curFlow.steps.empty() || curFlow.steps.back() != "verify")
          curFlow.steps.push_back("verify");
        continue;
      }
      if (startsWithCI(text, "DONE")) {
        auto rest = trim(text.substr(4));
        for (;;) {
          if (startsWithCI(rest, "->")) {
            rest = trim(rest.substr(2));
            continue;
          }
          if (rest.size() >= 3 && (unsigned char)rest[0] == 0xe2 &&
              (unsigned char)rest[1] == 0x86 && (unsigned char)rest[2] == 0x92) {
            rest = trim(rest.substr(3));
            continue;
          }
          break;
        }
        auto U = toUpper(rest);
        auto cr = U.find("CREATE");
        if (cr != std::string::npos) rest = trim(rest.substr(cr));
        curFlow.hasDone = true;
        curFlow.doneAction = rest.empty() ? "CREATE ACCOUNT" : rest;
        if (curFlow.steps.empty() || curFlow.steps.back() != "done")
          curFlow.steps.push_back("done");
        continue;
      }
      bc.fail(lineNo, "Inside FLOW: only COLLECT / VERIFY / DONE / END FLOW — got: " + text);
      return false;
    }

    if (mode == InFn) {
      if (toUpper(text) == "END FUNCTION" || toUpper(text) == "ENDFUNCTION") {
        bc.fns[curFn.name] = curFn;
        bc.fnOrder.push_back(curFn.name);
        mode = Top;
        continue;
      }
      curFn.body.push_back(text);
      curFn.lines.push_back(lineNo);
      curFn.files.push_back(bc.curFile);
      continue;
    }

    if (mode == InBp) {
      if (toUpper(text) == "END CLASS" || toUpper(text) == "ENDCLASS" ||
          toUpper(text) == "END BLUEPRINT") {
        bc.bps[curBp.name] = curBp;
        bc.bpOrder.push_back(curBp.name);
        mode = Top;
        continue;
      }
      if (startsWithCI(text, "HAS ") || startsWithCI(text, "PRIVATE ") ||
          startsWithCI(text, "SECRET ")) {
        bool priv = startsWithCI(text, "PRIVATE ");
        bool secret = startsWithCI(text, "SECRET ");
        std::string rest = text;
        if (startsWithCI(rest, "PRIVATE ")) rest = trim(rest.substr(8));
        else if (startsWithCI(rest, "SECRET ")) rest = trim(rest.substr(7));
        if (startsWithCI(rest, "METHOD ")) {
          // method below
        } else {
          if (startsWithCI(rest, "HAS ")) rest = trim(rest.substr(4));
          Field f;
          f.priv = priv;
          f.secret = secret;
          f.owner = curBp.name;
          auto U = toUpper(rest);
          auto as = U.find(" AS ");
          auto set = U.find(" SET TO ");
          if (as != std::string::npos && (set == std::string::npos || as < set)) {
            f.name = trim(rest.substr(0, as));
            auto after = trim(rest.substr(as + 4));
            auto set2 = toUpper(after).find(" SET TO ");
            bool tySecret = false;
            if (set2 != std::string::npos) {
              auto tyPart = bc.stripSecretTy(trim(after.substr(0, set2)), &tySecret);
              f.ty = bc.parseTy(tyPart);
              f.defRaw = trim(after.substr(set2 + 8));
            } else {
              auto tyPart = bc.stripSecretTy(after, &tySecret);
              f.ty = bc.parseTy(tyPart);
            }
            if (tySecret) f.secret = true;
          } else if (set != std::string::npos) {
            f.name = trim(rest.substr(0, set));
            f.defRaw = trim(rest.substr(set + 8));
            if (f.defRaw.size() >= 2 && f.defRaw.front() == '"') f.ty = Ty::text();
            else if (toUpper(f.defRaw) == "TRUE" || toUpper(f.defRaw) == "FALSE")
              f.ty = Ty::flag();
            else {
              char *end = nullptr;
              std::strtod(f.defRaw.c_str(), &end);
              f.ty = (end && *end == '\0') ? Ty::num() : Ty::text();
            }
          } else {
            f.name = rest;
            f.ty = Ty::text();
          }
          if (f.ty.k == K::Void) f.ty = Ty::text();
          curBp.fields.push_back(f);
          continue;
        }
      }
      if (startsWithCI(text, "WHEN BORN") ||
          (startsWithCI(text, "BORN") && !startsWithCI(text, "BORNED"))) {
        curM = {};
        curM.ctor = true;
        curM.name = "born";
        std::string rest = startsWithCI(text, "WHEN BORN") ? trim(text.substr(9)) : trim(text.substr(4));
        stripDo(rest);
        if (startsWithCI(rest, "WITH "))
          for (auto &a : splitArgs(trim(rest.substr(5)))) curM.params.push_back(bc.parseParam(a));
        mode = InMeth;
        continue;
      }
      std::string ml = text;
      if (startsWithCI(ml, "PRIVATE METHOD ") || startsWithCI(ml, "SECRET METHOD "))
        ml = trim(ml.substr(ml.find("METHOD")));
      if (startsWithCI(ml, "METHOD ") || startsWithCI(ml, "ACTION ")) {
        curM = {};
        std::string rest = trim(ml.substr(7));
        stripDo(rest);
        auto U = toUpper(rest);
        auto w = U.find(" WITH ");
        if (w == std::string::npos) curM.name = rest;
        else {
          curM.name = trim(rest.substr(0, w));
          for (auto &a : splitArgs(trim(rest.substr(w + 6)))) curM.params.push_back(bc.parseParam(a));
        }
        mode = InMeth;
        continue;
      }
      bc.fail(lineNo, "Unknown blueprint member: " + text);
      return false;
    }

    if (mode == InMeth) {
      if (toUpper(text) == "END BORN" || toUpper(text) == "ENDBORN" ||
          toUpper(text) == "END METHOD" || toUpper(text) == "ENDMETHOD") {
        curBp.methods.push_back(curM);
        mode = InBp;
        continue;
      }
      curM.body.push_back(text);
      curM.lines.push_back(lineNo);
      curM.files.push_back(bc.curFile);
      continue;
    }
  }
  if (mode != Top) {
    bc.fail(lineNo, "Unclosed block");
    return false;
  }
  // Infer / validate function return types from GIVE BACK expressions.
  for (auto &name : bc.fnOrder) {
    auto &fn = bc.fns[name];
    BC probe = bc;
    probe.locals.clear();
    probe.curClass.clear();
    probe.bad = false;
    probe.err.clear();
    for (auto &p : fn.params) probe.locals[p.name] = p.ty;
    Ty inferred = Ty::vod();
    bool sawReturn = false;
    for (size_t i = 0; i < fn.body.size(); ++i) {
      auto &t = fn.body[i];
      if (startsWithCI(t, "GIVE BACK ") || startsWithCI(t, "SEND BACK ") ||
          startsWithCI(t, "HAND BACK ")) {
        auto U = toUpper(t);
        auto b = U.find(" BACK ");
        auto e = probe.expr(trim(t.substr(b + 6)), fn.lines[i]);
        if (probe.bad) {
          bc.fail(fn.lines[i], probe.err);
          return false;
        }
        if (!sawReturn) {
          inferred = e.ty;
          sawReturn = true;
        } else if (!typesEqual(inferred, e.ty)) {
          bc.fail(fn.lines[i], "Function '" + name + "' GIVE BACK types disagree — saw " +
                                   tyName(inferred) + " then " + tyName(e.ty));
          return false;
        }
      }
    }
    if (fn.retDeclared) {
      if (sawReturn && !typesEqual(inferred, fn.ret)) {
        bc.fail(fn.lines.empty() ? 1 : fn.lines[0],
                "Function '" + name + "' should GIVE BACK " + tyName(fn.ret) + " but returns " +
                    tyName(inferred));
        return false;
      }
    } else if (sawReturn) {
      fn.ret = inferred;
    } else {
      fn.ret = Ty::num();
    }
  }
  return !bc.bad;
}

std::string defInit(BC &bc, const Field &f) {
  if (f.defRaw.empty()) {
    if (f.ty.k == K::Text) return "luke_text(\"\")";
    if (f.ty.k == K::Flag) return "0";
    if (f.ty.k == K::Int) return "0LL";
    return "0.0";
  }
  return bc.expr(f.defRaw, 1).code;
}

std::string emit(BC &bc) {
  std::ostringstream o;
  o << "/* Generated by Luke Build — native, no GC */\n";
  if (bc.forBrowser) o << "#define LUKE_BROWSER 1\n";
  o << "#include \"luke_rt.h\"\n";
  o << "#include \"luke_std.h\"\n";
  o << "#include \"argus.h\"\n";
  o << "#include \"hanka.h\"\n";
  o << "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdint.h>\n\n";
  o << "static LukeText luke_number_to_text(LukeArena *arena, double n) {\n";
  o << "  char buf[64]; int k = snprintf(buf, sizeof(buf), \"%.10g\", n); if (k<0) k=0;\n";
  o << "  char *p=(char*)luke_arena_alloc(arena,(size_t)k+1,1); memcpy(p,buf,(size_t)k+1);\n";
  o << "  return luke_text_n(p,(size_t)k);\n}\n";
  o << "static int luke_text_eq(LukeText a, LukeText b) {\n";
  o << "  return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);\n}\n\n";

  for (auto &tl : bc.top) {
    if (startsWithCI(tl.text, "SERVE ROUTES ")) bc.serveRoutes = true;
  }
  for (auto &h : bc.httpServeHandlers) {
    o << "static void luke_http_wrap_" << cIdent(h)
      << "(LukeArena *arena, LukeHttpRequest *req);\n";
  }
  if (bc.serveRoutes)
    o << "static void luke_http_wrap___luke_routes(LukeArena *arena, LukeHttpRequest *req);\n";

  for (auto &n : bc.bpOrder) o << "typedef struct " << cIdent(n) << " " << cIdent(n) << ";\n";
  o << "\n";
  for (auto &n : bc.bpOrder) {
    o << "struct " << cIdent(n) << " {\n";
    auto fs = bc.flatFields(n);
    if (fs.empty()) o << "  char _pad;\n";
    for (auto &f : fs) o << "  " << cTy(f.ty) << " " << bc.fname(f) << ";\n";
    o << "};\n\n";
  }

  for (auto &n : bc.fnOrder) {
    auto &fn = bc.fns[n];
    if (fn.foreign) {
      o << "extern " << cTy(fn.ret) << " " << cIdent(n) << "(";
      for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) o << ", ";
        o << cTy(fn.params[i].ty) << " " << cIdent(fn.params[i].name);
      }
      o << ");\n";
    } else {
      o << "static " << cTy(fn.ret) << " " << cIdent(n) << "(LukeArena *arena";
      for (auto &p : fn.params) o << ", " << cTy(p.ty) << " " << cIdent(p.name);
      o << ");\n";
    }
  }
  for (auto &n : bc.bpOrder) {
    auto &bp = bc.bps[n];
    std::vector<Param> ctorP;
        for (auto &m : bp.methods)
      if (m.ctor || m.name == "born") ctorP = m.params;
    // Inherit constructor signature from nearest ancestor with WHEN BORN.
    if (ctorP.empty()) {
      for (std::string c = bp.parent; !c.empty(); c = bc.bps[c].parent) {
        for (auto &m : bc.bps[c].methods) {
          if (m.ctor || m.name == "born") {
            ctorP = m.params;
            break;
          }
        }
        if (!ctorP.empty()) break;
      }
    }
    o << "static " << cIdent(n) << " *" << cIdent(n) << "_new(LukeArena *arena";
    for (auto &p : ctorP) o << ", " << cTy(p.ty) << " " << cIdent(p.name);
    o << ");\n";
    for (auto &m : bp.methods) {
      if (m.ctor) continue;
      o << "static void " << cIdent(n) << "_" << cIdent(m.name) << "(LukeArena *arena, "
        << cIdent(n) << " *self";
      for (auto &p : m.params) o << ", " << cTy(p.ty) << " " << cIdent(p.name);
      o << ");\n";
    }
  }
  o << "\n";

  for (auto &n : bc.fnOrder) {
    auto &fn = bc.fns[n];
    if (fn.foreign) continue;
    bc.locals.clear();
    bc.curClass.clear();
    bc.curRet = fn.ret;
    bc.hasCurRet = true;
    for (auto &p : fn.params) bc.locals[p.name] = p.ty;
    std::ostringstream body;
    for (size_t i = 0; i < fn.body.size(); ++i) {
      std::string f = i < fn.files.size() ? fn.files[i] : bc.sourcePath;
      stmt(bc, fn.body[i], fn.lines[i], body, f);
    }
    if (bc.bad) return {};
    o << "static " << cTy(fn.ret) << " " << cIdent(n) << "(LukeArena *arena";
    for (auto &p : fn.params) o << ", " << cTy(p.ty) << " " << cIdent(p.name);
    o << ") {\n" << body.str();
    if (fn.ret.k == K::Num) o << "  return 0.0;\n";
    else if (fn.ret.k == K::Flag) o << "  return 0;\n";
    else if (fn.ret.k == K::Text) o << "  return luke_text(\"\");\n";
    else if (fn.ret.k == K::Json) o << "  return (LukeJson*)0;\n";
    else if (fn.ret.k == K::List) o << "  return luke_list_new(arena);\n";
    else if (fn.ret.k == K::Map) o << "  return luke_map_new(arena);\n";
    else if (fn.ret.k == K::Ptr) o << "  return (" << cTy(fn.ret) << ")0;\n";
    o << "}\n\n";
  }

  for (auto &n : bc.bpOrder) {
    auto &bp = bc.bps[n];
    bc.curClass = n;
    bc.hasCurRet = false;
    bc.curRet = Ty::vod();
    for (auto &m : bp.methods) {
      if (m.ctor) continue;
      bc.locals.clear();
      bc.locals["SELF"] = Ty::ptr(n);
      for (auto &p : m.params) bc.locals[p.name] = p.ty;
      std::ostringstream body;
      for (size_t i = 0; i < m.body.size(); ++i) {
        std::string f = i < m.files.size() ? m.files[i] : bc.sourcePath;
        stmt(bc, m.body[i], m.lines[i], body, f);
      }
      if (bc.bad) return {};
      o << "static void " << cIdent(n) << "_" << cIdent(m.name) << "(LukeArena *arena, "
        << cIdent(n) << " *self";
      for (auto &p : m.params) o << ", " << cTy(p.ty) << " " << cIdent(p.name);
      o << ") {\n" << body.str() << "}\n\n";
    }
    Method *ctor = nullptr;
    for (auto &m : bp.methods)
      if (m.ctor || m.name == "born") ctor = &m;
    // Nearest ancestor ctor if this blueprint has none.
    Method *inheritedCtor = ctor;
    std::string ctorOwner = n;
    if (!inheritedCtor) {
      for (std::string c = bp.parent; !c.empty(); c = bc.bps[c].parent) {
        for (auto &m : bc.bps[c].methods) {
          if (m.ctor || m.name == "born") {
            inheritedCtor = &m;
            ctorOwner = c;
            break;
          }
        }
        if (inheritedCtor) break;
      }
    }
    o << "static " << cIdent(n) << " *" << cIdent(n) << "_new(LukeArena *arena";
    if (inheritedCtor)
      for (auto &p : inheritedCtor->params) o << ", " << cTy(p.ty) << " " << cIdent(p.name);
    o << ") {\n";
    o << "  " << cIdent(n) << " *self = (" << cIdent(n) << "*)luke_arena_alloc(arena, sizeof("
      << cIdent(n) << "), sizeof(void*));\n";
    o << "  memset(self, 0, sizeof(*self));\n";
    bc.locals.clear();
    bc.locals["SELF"] = Ty::ptr(n);
    if (inheritedCtor)
      for (auto &p : inheritedCtor->params) bc.locals[p.name] = p.ty;
    for (auto &f : bc.flatFields(n)) {
      o << "  self->" << bc.fname(f) << " = " << defInit(bc, f) << ";\n";
      if (bc.bad) return {};
    }
    if (inheritedCtor) {
      // Run ctor body with self typed as defining class when inherited.
      std::string saved = bc.curClass;
      bc.curClass = ctorOwner;
      // For inherited ctor, methods use Parent* field names — same flattened names on child.
      bc.locals["SELF"] = Ty::ptr(n);  // still child pointer; field names match
      bc.curClass = n;                 // allow child private? use owner for private checks in ctor of parent fields
      if (ctorOwner != n) bc.curClass = ctorOwner;
      for (size_t i = 0; i < inheritedCtor->body.size(); ++i) {
        std::string f =
            i < inheritedCtor->files.size() ? inheritedCtor->files[i] : bc.sourcePath;
        stmt(bc, inheritedCtor->body[i], inheritedCtor->lines[i], o, f);
      }
      bc.curClass = saved;
      if (bc.bad) return {};
    }
    o << "  return self;\n}\n\n";
  }

  bc.locals.clear();
  bc.curClass.clear();
  bc.hasCurRet = false;
  bc.curRet = Ty::vod();

  if (bc.forBrowser || !bc.pageWhens.empty() || bc.needsViewportRelayout)
    o << "static LukeArena *luke_page_arena = NULL;\n\n";

  /* Pre-declare reactive names so THE a IS b … can forward-reference. */
  for (auto &tl : bc.top) {
    const std::string &text = tl.text;
    if (startsWithCI(text, "REMEMBER ")) {
      auto rest = trim(text.substr(9));
      auto U = toUpper(rest);
      auto asPos = U.find(" AS ");
      if (asPos == std::string::npos) continue;
      auto name = stripThe(trim(rest.substr(0, asPos)));
      if (name.empty()) continue;
      auto after = trim(rest.substr(asPos + 4));
      auto aU = toUpper(after);
      auto setPos = aU.find(" SET TO ");
      std::string tyTok = setPos == std::string::npos ? after : trim(after.substr(0, setPos));
      /* First token only for typed empty forms */
      auto sp = tyTok.find(' ');
      if (sp != std::string::npos) tyTok = trim(tyTok.substr(0, sp));
      Ty hint = bc.parseTy(tyTok);
      if (hint.k == K::Void) hint = Ty::num();
      bc.usesRx = true;
      bc.locals[name] = hint;
      if (!bc.rxCells.count(name)) bc.rxCellOrder.push_back(name);
      bc.rxCells[name] = true;
      if (!bc.rxCellTy.count(name)) bc.rxCellTy[name] = hint;
    } else if (startsWithCI(text, "THE ") && !startsWithCI(text, "THE VALUE OF ") &&
               !startsWithCI(text, "THE BODY OF ") && !startsWithCI(text, "THE STATUS OF ")) {
      auto rest = trim(text.substr(4));
      auto U = toUpper(rest);
      auto isPos = U.find(" IS ");
      if (isPos == std::string::npos) continue;
      auto name = stripThe(trim(rest.substr(0, isPos)));
      bool simple = !name.empty();
      for (char c : name)
        if (!(isalnum((unsigned char)c) || c == '_')) simple = false;
      if (!simple) continue;
      bc.usesRx = true;
      bc.locals[name] = Ty::num();
      if (!bc.rxCells.count(name)) bc.rxCellOrder.push_back(name);
      bc.rxCells[name] = true;
      bc.rxDerived[name] = true;
      if (!bc.rxCellTy.count(name)) bc.rxCellTy[name] = Ty::num();
    }
  }

  /* Emit top-level + WHEN bodies first so REMEMBER / THE x IS populate reactive metadata. */
  bc.locals.clear();
  for (auto &kv : bc.rxCells) {
    Ty ty = bc.rxCellTy.count(kv.first) ? bc.rxCellTy[kv.first] : Ty::num();
    bc.locals[kv.first] = ty;
  }
  std::ostringstream mainBody;
  for (auto &tl : bc.top) {
    stmt(bc, tl.text, tl.line, mainBody, tl.file);
    if (bc.bad) return {};
  }
  if (!bc.arenaMarks.empty()) {
    bc.fail(1, "Unclosed IN ARENA — missing END ARENA");
    return {};
  }
  if (!bc.hankaStack.empty()) {
    bc.fail(1, "Unclosed BEGIN " + bc.hankaStack.back() + " — missing END " + bc.hankaStack.back());
    return {};
  }
  if (!bc.forEachVars.empty()) {
    bc.fail(1, "Unclosed FOR EACH — missing END FOR");
    return {};
  }
  if (!bc.rxComponentStack.empty()) {
    bc.fail(1, "Unclosed COMPONENT " + bc.rxComponentStack.back() + " — missing END COMPONENT");
    return {};
  }
  if (!bc.rxBoundaryStack.empty()) {
    bc.fail(1, "Unclosed ERROR BOUNDARY " + bc.rxBoundaryStack.back() +
                   " — missing END ERROR BOUNDARY");
    return {};
  }

  for (auto &tl : bc.top) {
    if (startsWithCI(tl.text, "START TIMELINE ") || startsWithCI(tl.text, "RUN TIMELINE "))
      bc.usesTimeline = true;
  }
  for (auto &w : bc.pageWhens) {
    for (auto &line : w.body) {
      if (startsWithCI(line, "START TIMELINE ") || startsWithCI(line, "RUN TIMELINE "))
        bc.usesTimeline = true;
    }
  }

  /* Ensure reactive names are visible while compiling derived compute fns. */
  for (auto &kv : bc.rxCells) {
    Ty ty = bc.rxCellTy.count(kv.first) ? bc.rxCellTy[kv.first] : Ty::num();
    bc.locals[kv.first] = ty;
  }

  if (bc.usesRx) {
    o << "static LukeRxGraph *_luke_rx = NULL;\n";
    o << "static LukeRxGraph _luke_rx_storage;\n";
    for (auto &name : bc.rxCellOrder)
      o << "static LukeRxId _luke_rx_id_" << cIdent(name) << ";\n";
    for (auto &b : bc.rxBindDefs)
      o << "static LukeRxId _luke_rx_id_bind_" << b.seq << ";\n";
    for (auto &b : bc.rxListBindDefs)
      o << "static LukeRxId _luke_rx_id_bind_" << b.seq << ";\n";
    for (auto &b : bc.rxOpacityBindDefs)
      o << "static LukeRxId _luke_rx_id_bind_" << b.seq << ";\n";
    for (auto &w : bc.rxWhenDefs)
      o << "static LukeRxId _luke_rx_id_when_" << w.seq << ";\n";
    if (bc.usesTimeline || bc.forBrowser) o << "static LukeText _luke_active_timeline_id;\n";
    /* Global symbol for gdb / luke DEBUG --inspect / DAP Reactive scope. */
    o << "__attribute__((used,noinline)) const char *luke_debug_rx_inspect(void) {\n";
    o << "  return luke_rx_inspect_cstr(_luke_rx);\n";
    o << "}\n";
    o << "\n";
    for (auto &kv : bc.rxCells) {
      auto dot = kv.first.find('.');
      if (dot != std::string::npos) {
        Ty ty = bc.rxCellTy.count(kv.first) ? bc.rxCellTy[kv.first] : Ty::num();
        bc.locals[kv.first.substr(dot + 1)] = ty;
      }
    }
    for (auto &d : bc.rxDerivedDefs) {
      bc.rxGraphVar = "g";
      auto dot = d.name.find('.');
      if (dot != std::string::npos) bc.locals[d.name.substr(dot + 1)] = Ty::num();
      auto e = bc.expr(d.exprRaw, d.line);
      if (bc.bad) return {};
      bc.expectTy(d.line, e.ty, Ty::num(), "THE " + d.name + " IS");
      if (bc.bad) return {};
      o << "static double _luke_rx_fn_" << cIdent(d.name) << "(LukeRxGraph *g, void *ctx) {\n";
      o << "  (void)ctx;\n";
      o << "  return " << e.code << ";\n";
      o << "}\n\n";
    }
    for (auto &w : bc.rxWhenDefs) {
      bc.rxGraphVar = "g";
      o << "static void _luke_rx_when_" << w.seq << "(LukeRxGraph *g, void *ctx) {\n";
      o << "  (void)ctx;\n";
      o << "  LukeArena *arena = g->arena;\n";
      o << "  (void)arena;\n";
      auto watchCell = resolveRxCellName(bc.rxCells, bc.rxEntityStack, w.cellName);
      if (!bc.rxCells.count(watchCell)) {
        bc.fail(w.line, "WHEN REACTIVE needs a REMEMBER'd cell — not '" + w.cellName + "'");
        return {};
      }
      if (bc.rxCellTy.count(watchCell) && bc.rxCellTy[watchCell].k == K::Text)
        o << "  (void)luke_rx_read_text(g, _luke_rx_id_" << cIdent(watchCell) << ");\n";
      else
        o << "  (void)luke_rx_read_num(g, _luke_rx_id_" << cIdent(watchCell) << ");\n";
      bc.locals.clear();
      for (auto &kv : bc.rxCells) {
        Ty ty = bc.rxCellTy.count(kv.first) ? bc.rxCellTy[kv.first] : Ty::num();
        bc.locals[kv.first] = ty;
        auto dot = kv.first.find('.');
        if (dot != std::string::npos) bc.locals[kv.first.substr(dot + 1)] = ty;
      }
      std::ostringstream wb;
      for (size_t i = 0; i < w.body.size(); ++i) {
        std::string f = i < w.files.size() ? w.files[i] : bc.sourcePath;
        stmt(bc, w.body[i], w.lines[i], wb, f);
        if (bc.bad) return {};
      }
      o << wb.str();
      o << "}\n\n";
    }
    for (auto &b : bc.rxBindDefs) {
      bc.rxGraphVar = "g";
      auto te = bc.coerceText(bc.expr(b.exprRaw, b.line));
      if (bc.bad) return {};
      o << "static void _luke_rx_bind_" << b.seq << "(LukeRxGraph *g, void *ctx) {\n";
      o << "  (void)ctx;\n";
      o << "  LukeArena *arena = g->arena;\n";
      o << "  LukeText _luke_bind_t = " << te.code << ";\n";
      o << "  luke_rx_ui_set_text(g, " << b.argusId << ", _luke_bind_t);\n";
      o << "}\n\n";
    }
    for (auto &b : bc.rxListBindDefs) {
      bc.rxGraphVar = "g";
      auto pe = bc.coerceText(bc.expr(b.prefixRaw, b.line));
      if (bc.bad) return {};
      o << "static void _luke_rx_bind_list_" << b.seq << "(LukeRxGraph *g, void *ctx) {\n";
      o << "  (void)ctx;\n";
      o << "  LukeArena *arena = g->arena;\n";
      o << "  (void)arena;\n";
      o << "  luke_rx_ui_paint_list(g, _luke_rx_id_" << cIdent(b.listName) << ", " << pe.code
        << ");\n";
      o << "}\n\n";
    }
    for (auto &b : bc.rxOpacityBindDefs) {
      bc.rxGraphVar = "g";
      auto oe = bc.expr(b.exprRaw, b.line);
      if (bc.bad) return {};
      bc.expectTy(b.line, oe.ty, Ty::num(), "BIND OPACITY");
      o << "static void _luke_rx_bind_opacity_" << b.seq << "(LukeRxGraph *g, void *ctx) {\n";
      o << "  (void)ctx;\n";
      o << "  luke_rx_ui_set_opacity(g, " << b.argusId << ", " << oe.code << ");\n";
      o << "}\n\n";
    }
    if (bc.forBrowser && bc.usesRx) {
      o << "__attribute__((export_name(\"luke_timeline_progress\")))\n";
      o << "void luke_timeline_progress(double t) {\n";
      o << "  if (_luke_rx) luke_rx_timeline_progress(_luke_rx, _luke_active_timeline_id, t);\n";
      o << "}\n\n";
      o << "__attribute__((export_name(\"luke_timeline_finish_export\")))\n";
      o << "void luke_timeline_finish_export(void) {\n";
      o << "  if (_luke_rx) luke_rx_timeline_finish(_luke_rx, _luke_active_timeline_id);\n";
      o << "}\n\n";
    }
    bc.rxGraphVar = "_luke_rx";
  }

  /* Collect WHEN bodies; START FETCH inside handlers may register rxFetchBinds. */
  std::vector<std::string> whenBodies;
  if (!bc.pageWhens.empty()) {
    for (auto &w : bc.pageWhens) {
      bc.locals.clear();
      for (auto &kv : bc.rxCells) {
        Ty ty = bc.rxCellTy.count(kv.first) ? bc.rxCellTy[kv.first] : Ty::num();
        bc.locals[kv.first] = ty;
      }
      std::ostringstream wb;
      for (size_t i = 0; i < w.body.size(); ++i) {
        std::string f = i < w.files.size() ? w.files[i] : bc.sourcePath;
        stmt(bc, w.body[i], w.lines[i], wb, f);
        if (bc.bad) return {};
      }
      if (!bc.hankaStack.empty()) {
        bc.fail(1, "Unclosed BEGIN " + bc.hankaStack.back() + " in WHEN handler");
        return {};
      }
      whenBodies.push_back(wb.str());
    }
  }

  /* Phase 4: ensure every START FETCH … INTO has a FETCH READY continuation. */
  for (auto &fb : bc.rxFetchBinds) {
    bool found = false;
    for (auto &w : bc.pageWhens) {
      if (w.event == "fetch" && w.elementId == fb.jobId) {
        found = true;
        break;
      }
    }
    if (!found) {
      BrowserWhen w;
      w.event = "fetch";
      w.elementId = fb.jobId;
      w.exportName = "luke_when_" + std::to_string(bc.whenSeq++);
      bc.pageWhens.push_back(w);
      bc.hasPage = true;
      whenBodies.push_back("");
    }
  }

  /* Spike A push: ensure START SUBSCRIBE … INTO has a SUBSCRIBE READY continuation. */
  for (auto &sb : bc.rxSubscribeBinds) {
    bool found = false;
    for (auto &w : bc.pageWhens) {
      if (w.event == "subscribe" && w.elementId == sb.jobId) {
        found = true;
        break;
      }
    }
    if (!found) {
      BrowserWhen w;
      w.event = "subscribe";
      w.elementId = sb.jobId;
      w.exportName = "luke_when_" + std::to_string(bc.whenSeq++);
      bc.pageWhens.push_back(w);
      bc.hasPage = true;
      whenBodies.push_back("");
    }
  }

  /* Phase 6: ensure START TIMELINE has FINISHED continuation (browser). */
  if (bc.forBrowser || bc.hasPage) {
  for (auto &tb : bc.rxTimelineBinds) {
    bool found = false;
    for (auto &w : bc.pageWhens) {
      if (w.event == "timeline" && w.elementId == tb.jobId) {
        found = true;
        break;
      }
    }
    if (!found) {
      BrowserWhen w;
      w.event = "timeline";
      w.elementId = tb.jobId;
      w.exportName = "luke_when_" + std::to_string(bc.whenSeq++);
      bc.pageWhens.push_back(w);
      bc.hasPage = true;
      whenBodies.push_back("");
    }
  }
  }

  if (!bc.pageWhens.empty()) {
    for (size_t wi = 0; wi < bc.pageWhens.size(); ++wi) {
      auto &w = bc.pageWhens[wi];
      if (bc.forBrowser) o << "__attribute__((export_name(\"" << w.exportName << "\")))\n";
      o << "void " << w.exportName << "(void) {\n";
      o << "  LukeArena *arena = luke_page_arena;\n";
      o << "  if (!arena) return;\n";
      if (w.event == "fetch") {
        /* Continuation edge: result cells first (batched), then user body. */
        for (auto &fb : bc.rxFetchBinds) {
          if (fb.jobId != w.elementId) continue;
          o << "  if (_luke_rx) luke_rx_batch_begin(_luke_rx);\n";
          if (!fb.bodyCell.empty()) {
            o << "  luke_rx_write_text(_luke_rx, _luke_rx_id_" << cIdent(fb.bodyCell)
              << ", luke_js_fetch_body(arena, luke_text(\"" << esc(fb.jobId) << "\")));\n";
          }
          if (!fb.statusCell.empty()) {
            Ty sty = bc.rxCellTy.count(fb.statusCell) ? bc.rxCellTy[fb.statusCell] : Ty::num();
            if (sty.k == K::Int)
              o << "  luke_rx_write_int(_luke_rx, _luke_rx_id_" << cIdent(fb.statusCell)
                << ", (int64_t)luke_js_fetch_status(luke_text(\"" << esc(fb.jobId) << "\")));\n";
            else
              o << "  luke_rx_write_num(_luke_rx, _luke_rx_id_" << cIdent(fb.statusCell)
                << ", luke_js_fetch_status(luke_text(\"" << esc(fb.jobId) << "\")));\n";
          }
          if (!fb.readyCell.empty()) {
            Ty rty = bc.rxCellTy.count(fb.readyCell) ? bc.rxCellTy[fb.readyCell] : Ty::num();
            if (rty.k == K::Int)
              o << "  luke_rx_write_int(_luke_rx, _luke_rx_id_" << cIdent(fb.readyCell) << ", 1);\n";
            else
              o << "  luke_rx_write_num(_luke_rx, _luke_rx_id_" << cIdent(fb.readyCell) << ", 1);\n";
          }
          o << "  if (_luke_rx) luke_rx_batch_end(_luke_rx);\n";
        }
      }
      if (w.event == "subscribe") {
        for (auto &sb : bc.rxSubscribeBinds) {
          if (sb.jobId != w.elementId) continue;
          o << "  if (_luke_rx) luke_rx_batch_begin(_luke_rx);\n";
          if (!sb.bodyCell.empty()) {
            o << "  luke_rx_write_text(_luke_rx, _luke_rx_id_" << cIdent(sb.bodyCell)
              << ", luke_js_subscribe_body(arena, luke_text(\"" << esc(sb.jobId) << "\")));\n";
          }
          if (!sb.readyCell.empty()) {
            Ty rty = bc.rxCellTy.count(sb.readyCell) ? bc.rxCellTy[sb.readyCell] : Ty::num();
            if (rty.k == K::Int)
              o << "  luke_rx_write_int(_luke_rx, _luke_rx_id_" << cIdent(sb.readyCell) << ", 1);\n";
            else
              o << "  luke_rx_write_num(_luke_rx, _luke_rx_id_" << cIdent(sb.readyCell) << ", 1);\n";
          }
          o << "  if (_luke_rx) luke_rx_batch_end(_luke_rx);\n";
        }
      }
      o << whenBodies[wi];
      o << "}\n\n";
    }
  }

  if (bc.needsViewportRelayout) {
    if (bc.forBrowser) o << "__attribute__((export_name(\"luke_viewport_relayout\")))\n";
    o << "void luke_viewport_relayout(void) {\n";
    o << "  LukeArena *arena = luke_page_arena;\n";
    o << "  if (!arena) return;\n";
    o << "  hanka_set_keep_roots(arena, 1);\n";
    o << "  hanka_layout(arena);\n";
    o << "  argus_paint(arena);\n";
    o << "}\n\n";
  }

  for (auto &h : bc.httpServeHandlers) {
    o << "static void luke_http_wrap_" << cIdent(h)
      << "(LukeArena *arena, LukeHttpRequest *req) {\n";
    o << "  " << cIdent(h) << "(arena, req);\n";
    o << "}\n\n";
  }
  if (bc.serveRoutes) {
    o << "static void __luke_routes(LukeArena *arena, LukeHttpRequest *req) {\n";
    o << "  LukeText _luke_method = luke_http_method(req);\n";
    o << "  LukeText _luke_path = luke_http_path(req);\n";
    o << "  LukeMap *_luke_params = luke_map_new(arena);\n";
    o << "  int _luke_answered = 0;\n";
    for (auto &r : bc.routes) {
      o << "  if (!_luke_answered && luke_text_eq(_luke_method, luke_text(\"" << esc(r.method)
        << "\")) && luke_http_match(arena, _luke_path, luke_text(\"" << esc(r.pattern)
        << "\"), _luke_params)) {\n";
      o << "    int _luke_bad = 0;\n";
      if (!r.paramName.empty() && r.paramTy == "INTEGER") {
        o << "    {\n";
        o << "      LukeText _luke_pv = luke_map_get(_luke_params, luke_text(\"" << esc(r.paramName)
          << "\"));\n";
        o << "      int _luke_okp = _luke_pv.len > 0;\n";
        o << "      for (size_t _i = 0; _luke_okp && _i < _luke_pv.len; ++_i) {\n";
        o << "        char _c = _luke_pv.ptr[_i];\n";
        o << "        if (_c < '0' || _c > '9') _luke_okp = 0;\n";
        o << "      }\n";
        o << "      if (!_luke_okp) {\n";
        o << "        luke_http_reply(req, 400, luke_text(\"text/plain\"), luke_text(\"bad param\"));\n";
        o << "        _luke_answered = 1;\n";
        o << "        _luke_bad = 1;\n";
        o << "      }\n";
        o << "    }\n";
      }
      if (r.requiresAuth) {
        o << "    if (!_luke_bad) {\n";
        o << "      LukeText _luke_uid = luke_auth_current_user();\n";
        o << "      if (!_luke_uid.len) {\n";
        o << "        luke_http_reply(req, 401, luke_text(\"text/plain\"), luke_text(\"login required\"));\n";
        o << "        _luke_answered = 1;\n";
        o << "      } else {\n";
        o << "        if (_luke_rx) luke_rx_write_text(_luke_rx, _luke_rx_id_current_route, luke_text(\""
          << esc(r.pattern) << "\"));\n";
        o << "        " << cIdent(r.handler) << "(arena, req);\n";
        o << "        _luke_answered = 1;\n";
        o << "      }\n";
        o << "    }\n";
      } else {
        o << "    if (!_luke_bad) {\n";
        o << "      if (_luke_rx) luke_rx_write_text(_luke_rx, _luke_rx_id_current_route, luke_text(\""
          << esc(r.pattern) << "\"));\n";
        o << "      " << cIdent(r.handler) << "(arena, req);\n";
        o << "      _luke_answered = 1;\n";
        o << "    }\n";
      }
      o << "  }\n";
    }
    o << "  if (!_luke_answered) luke_http_reply(req, 404, luke_text(\"text/plain\"), luke_text(\"not found\"));\n";
    o << "}\n\n";
    o << "static void luke_http_wrap___luke_routes(LukeArena *arena, LukeHttpRequest *req) {\n";
    o << "  __luke_routes(arena, req);\n";
    o << "}\n\n";
  }

  o << "int main(int argc, char **argv) {\n";
  o << "  luke_runtime_set_args(argc, argv);\n";
  o << "  LukeArena arena_storage; LukeArena *arena = &arena_storage;\n";
  /* 1MiB start; luke_arena_alloc grows on demand. Argus nodes are malloc'd separately. */
  o << "  luke_arena_init(arena, 1u<<20);\n";
  if (bc.forBrowser || !bc.pageWhens.empty())
    o << "  luke_page_arena = arena;\n";
  if (bc.usesRx) {
    o << "  luke_rx_graph_init(&_luke_rx_storage, arena);\n";
    o << "  _luke_rx = &_luke_rx_storage;\n";
    if (bc.usesRxUi) o << "  luke_rx_ui_enable(_luke_rx);\n";
  }
  o << mainBody.str();
  if (bc.forBrowser || !bc.pageWhens.empty()) {
    /* Keep arena alive for WHEN handlers / page lifetime. */
    o << "  return 0;\n}\n";
  } else {
    o << "  luke_arena_free(arena);\n  return 0;\n}\n";
  }
  return o.str();
}

static void fillIrSummary(BuildResult &r, BC &bc) {
  if (bc.needsPthread) {
    bool has = false;
    for (auto &l : r.linkLibs)
      if (l == "pthread") has = true;
    if (!has) r.linkLibs.push_back("pthread");
  }
  std::ostringstream ir;
  ir << "luke-build-ir 1\n";
  ir << "imports " << r.importedFiles.size() << "\n";
  for (auto &p : r.importedFiles) ir << "  " << p << "\n";
  ir << "link " << r.linkLibs.size() << "\n";
  for (auto &l : r.linkLibs) ir << "  -l" << l << "\n";
  ir << "functions " << bc.fnOrder.size() << "\n";
  for (auto &n : bc.fnOrder) {
    auto &fn = bc.fns[n];
    ir << "  " << (fn.foreign ? "foreign " : "fn ") << n << " -> " << tyName(fn.ret) << "\n";
    if (fn.foreign) r.foreignFns.push_back(n);
  }
  ir << "blueprints " << bc.bpOrder.size() << "\n";
  for (auto &n : bc.bpOrder) ir << "  bp " << n << "\n";
  ir << "toplevel " << bc.top.size() << "\n";
  if (!r.astSummary.empty()) {
    ir << "--- ast ---\n";
    ir << r.astSummary;
  }
  r.irSummary = ir.str();
}

static std::string expandImpl(const std::string &source, const BuildOptions &options, BuildResult &r) {
  auto dirname = [](std::string p) -> std::string {
    auto slash = p.find_last_of("/\\");
    if (slash == std::string::npos) return ".";
    return p.substr(0, slash);
  };
  auto readFile = [](const std::string &path) -> std::string {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  };

  std::string stdlib = options.stdlibPath;
  if (stdlib.empty()) {
    if (std::ifstream("stdlib/files.luke")) stdlib = "stdlib";
    else if (std::ifstream("vm/stdlib/files.luke")) stdlib = "vm/stdlib";
    else if (std::ifstream("../stdlib/files.luke")) stdlib = "../stdlib";
  }

  std::vector<std::string> pkgRoots = options.packagePaths;
  auto addRoot = [&](const std::string &p) {
    if (p.empty()) return;
    for (auto &e : pkgRoots)
      if (e == p) return;
    pkgRoots.push_back(p);
  };
  addRoot("luke_modules");
  addRoot("../luke_modules");
  addRoot("vm/luke_modules");
  addRoot("registry/packages");
  addRoot("../registry/packages");
  if (const char *env = std::getenv("LUKE_PACKAGES")) {
    std::string e = env;
    size_t start = 0;
    while (start <= e.size()) {
      auto colon = e.find(':', start);
      if (colon == std::string::npos) {
        addRoot(e.substr(start));
        break;
      }
      addRoot(e.substr(start, colon - start));
      start = colon + 1;
    }
  }

  auto resolvePackage = [&](const std::string &name) -> std::string {
    for (auto &root : pkgRoots) {
      std::string dir = root + "/" + name;
      auto readPkgEntry = [&](const std::string &pkgFile) -> std::string {
        auto body = readFile(pkgFile);
        if (body.empty()) return {};
        std::istringstream in(body);
        std::string line;
        while (std::getline(in, line)) {
          auto t = trim(line);
          if (startsWithCI(t, "entry=")) return trim(t.substr(6));
          auto key = t.find("\"entry\"");
          if (key != std::string::npos) {
            auto colon = t.find(':', key);
            auto q1 = t.find('"', colon + 1);
            auto q2 = t.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos)
              return t.substr(q1 + 1, q2 - q1 - 1);
          }
        }
        return {};
      };
      if (std::ifstream(dir + "/luke.pkg")) {
        auto entry = readPkgEntry(dir + "/luke.pkg");
        if (entry.empty()) entry = "main.luke";
        std::string path = dir + "/" + entry;
        if (std::ifstream(path)) return path;
      }
      if (std::ifstream(dir + "/main.luke")) return dir + "/main.luke";
      if (std::ifstream(dir + "/" + name + ".luke")) return dir + "/" + name + ".luke";
    }
    return {};
  };

  std::set<std::string> seen;
  std::function<std::string(const std::string &, const std::string &, const std::string &)> expand;
  expand = [&](const std::string &src, const std::string &baseDir,
               const std::string &filePath) -> std::string {
    std::istringstream in(src);
    std::ostringstream out;
    std::string line;
    size_t localLine = 0;
    while (std::getline(in, line)) {
      ++localLine;
      auto t = trim(line);
      if (startsWithCI(t, "IMPORT ")) {
        auto spec = trim(t.substr(7));
        if (spec.size() >= 2 && ((spec.front() == '"' && spec.back() == '"') ||
                                 (spec.front() == '\'' && spec.back() == '\'')))
          spec = spec.substr(1, spec.size() - 2);

        if (startsWithCI(spec, "c:") || startsWithCI(spec, "C:") || startsWithCI(spec, "ffi:") ||
            startsWithCI(spec, "FFI:")) {
          if (!options.expandCImports) {
            out << "// skipped " << t << "\n";
            continue;
          }
          std::string lib = startsWithCI(spec, "ffi:") || startsWithCI(spec, "FFI:")
                                ? trim(spec.substr(4))
                                : trim(spec.substr(2));
          bool dup = false;
          for (auto &e : r.linkLibs)
            if (e == lib) dup = true;
          if (!dup) r.linkLibs.push_back(lib);
          out << "// IMPORT c:" << lib << "\n";
          continue;
        }

        std::string path;
        if (startsWithCI(spec, "std/") || startsWithCI(spec, "STD/")) {
          if (!options.expandStd) {
            out << "// skipped " << t << "\n";
            continue;
          }
          path = stdlib + "/" + spec.substr(4) + ".luke";
          auto mod = toUpper(spec.substr(4));
          if (mod == "SQLITE") {
            bool dup = false;
            for (auto &e : r.linkLibs)
              if (e == "sqlite3") dup = true;
            if (!dup) r.linkLibs.push_back("sqlite3");
          }
          if (mod == "PG") {
            bool dup = false;
            for (auto &e : r.linkLibs)
              if (e == "pq") dup = true;
            if (!dup) r.linkLibs.push_back("pq");
            dup = false;
            for (auto &e : r.linkLibs)
              if (e == "pthread") dup = true;
            if (!dup) r.linkLibs.push_back("pthread");
          }
          if (mod == "AUTH") {
            bool dup = false;
            for (auto &e : r.linkLibs)
              if (e == "sodium") dup = true;
            if (!dup) r.linkLibs.push_back("sodium");
          }
        } else if (startsWithCI(spec, "luke/") || startsWithCI(spec, "LUKE/") ||
                   startsWithCI(spec, "package:") || startsWithCI(spec, "PACKAGE:")) {
          std::string name;
          if (startsWithCI(spec, "luke/") || startsWithCI(spec, "LUKE/"))
            name = spec.substr(5);
          else
            name = spec.substr(8);
          while (!name.empty() && (name.back() == '/' || name.back() == ' ')) name.pop_back();
          path = resolvePackage(name);
          if (path.empty()) {
            r.ok = false;
            r.error = "Build error: package '" + name +
                      "' not found — luke PKG install " + name +
                      " or place it in luke_modules/" + name;
            return {};
          }
        } else {
          if (spec.size() < 5 || spec.substr(spec.size() - 5) != ".luke") spec += ".luke";
          path = baseDir + "/" + spec;
        }
        if (seen.count(path)) continue;
        seen.insert(path);
        auto body = readFile(path);
        if (body.empty()) {
          r.ok = false;
          r.error = "Build error: IMPORT could not open '" + path + "'";
          return {};
        }
        r.importedFiles.push_back(path);
        out << makeLukeFileMarker(path, 1) << "\n";
        out << expand(body, dirname(path), path);
        out << makeLukeFileMarker(filePath, localLine + 1) << "\n";
        continue;
      }
      out << line << "\n";
    }
    return out.str();
  };

  std::string base = options.sourcePath.empty() ? "." : dirname(options.sourcePath);
  addRoot(base + "/luke_modules");
  std::string mainPath = options.sourcePath.empty() ? "luke" : options.sourcePath;
  return makeLukeFileMarker(mainPath, 1) + "\n" + expand(source, base, mainPath);
}

static BuildResult compileExpanded(const std::string &expanded, const BuildOptions &options,
                                   BuildResult r) {
  if (!r.error.empty()) return r;
  r.expandedSource = expanded;
  BC bc;
  bc.forBrowser = options.forBrowser;
  bc.sourcePath = options.sourcePath.empty() ? "luke" : options.sourcePath;
  bc.curFile = bc.sourcePath;
  if (!parse(bc, expanded)) {
    r.ok = false;
    r.error = bc.err.empty() ? "Build parse failed" : bc.err;
    r.unsupportedForBuild = bc.unsupportedHint;
    fillIrSummary(r, bc);
    return r;
  }
  for (auto &n : bc.bpOrder) {
    if (!bc.bps[n].parent.empty() && !bc.bps.count(bc.bps[n].parent)) {
      r.ok = false;
      r.error = "Build error: unknown parent blueprint '" + bc.bps[n].parent + "'";
      return r;
    }
  }
  fillIrSummary(r, bc);
  auto c = emit(bc);
  if (bc.bad) {
    r.ok = false;
    r.error = bc.err;
    r.unsupportedForBuild = bc.unsupportedHint;
    return r;
  }
  if (options.forWasm || options.forBrowser) {
    c = std::string("/* luke target: ") + (options.forBrowser ? "browser" : "wasm/wasi") +
        " */\n" + c;
  }
  r.ok = true;
  r.cSource = std::move(c);
  r.hasPage = bc.hasPage;
  r.pageTitle = bc.pageTitle;
  r.pageStyle = bc.pageStyle;
  r.pageCssHref = bc.pageCssHref;
  r.pageBody = bc.pageBody;
  r.pageFonts = bc.pageFonts;
  r.pageWhens = bc.pageWhens;
  return r;
}

}  // namespace

BuildResult compileLukeToC(const std::string &source, const BuildOptions &options) {
  BuildResult r;
  std::string expanded = expandImpl(source, options, r);
  if (!r.error.empty()) return r;
  /* Production pipeline: expanded source → Program AST → flatten → emit.
   * Codegen still uses the BC lowering engine; the AST is the parse IR every
   * build goes through (LSP/FMT/IR share the same nodes). */
  Program prog = parseLuke(expanded);
  r.astSummary = dumpProgram(prog);
  std::string viaAst = flattenProgram(prog);
  if (viaAst.empty() && !expanded.empty()) {
    r.ok = false;
    r.error = "Build error: AST flatten produced empty program";
    return r;
  }
  return compileExpanded(viaAst.empty() ? expanded : viaAst, options, std::move(r));
}

std::string expandLukeImports(const std::string &source, const BuildOptions &options,
                              BuildResult *meta) {
  BuildResult local;
  BuildResult &r = meta ? *meta : local;
  std::string expanded = expandImpl(source, options, r);
  if (!r.error.empty()) return {};
  r.expandedSource = expanded;
  return expanded;
}

std::string softenBuildSurfaceForPlay(const std::string &expanded) {
  auto trimLine = [](std::string s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b])) ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
  };
  auto up = [](std::string s) {
    for (char &c : s) c = (char)toupper((unsigned char)c);
    return s;
  };
  auto starts = [&](const std::string &s, const std::string &p) {
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
      if (toupper((unsigned char)s[i]) != toupper((unsigned char)p[i])) return false;
    return true;
  };
  std::istringstream in(expanded);
  std::ostringstream out;
  std::string line;
  while (std::getline(in, line)) {
    auto t = trimLine(line);
    if (starts(t, "FOREIGN ") || starts(t, "IN ARENA") || up(t) == "END ARENA" ||
        starts(t, "IMPORT C:") || starts(t, "IMPORT FFI:"))
      continue;
    std::string s = line;
    for (;;) {
      auto U = up(s);
      auto gb = U.find(" GIVES BACK ");
      if (gb == std::string::npos) gb = U.find(" RETURNS ");
      if (gb == std::string::npos) break;
      size_t kwLen = (U.compare(gb, 12, " GIVES BACK ") == 0) ? 12 : 9;
      auto after = trimLine(s.substr(gb + kwLen));
      auto doPos = up(after).find(" DO");
      if (doPos != std::string::npos)
        s = trimLine(s.substr(0, gb)) + " DO" + after.substr(doPos + 3);
      else {
        auto sp = after.find(' ');
        s = trimLine(s.substr(0, gb)) + (sp == std::string::npos ? std::string() : after.substr(sp));
      }
    }
    for (;;) {
      auto U = up(s);
      auto as = U.find(" AS ");
      if (as == std::string::npos) break;
      auto after = trimLine(s.substr(as + 4));
      auto sp = after.find_first_of(" ,");
      std::string rest = sp == std::string::npos ? std::string() : after.substr(sp);
      s = trimLine(s.substr(0, as)) + rest;
    }
    out << s << "\n";
  }
  return out.str();
}

BuildResult analyzeLukeBuild(const std::string &source, const BuildOptions &options) {
  BuildResult r = compileLukeToC(source, options);
  r.cSource.clear();
  return r;
}

}  // namespace luke
