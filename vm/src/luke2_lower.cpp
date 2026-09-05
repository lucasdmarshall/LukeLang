/* Syntax v2 lowerer — v2 AST → v1 conversational source (+ @luke-file markers).
 *
 * Why lower to v1 text instead of straight to C: codegen (build_c.cpp) consumes
 * conversational text, so lowering here reuses the whole existing backend and
 * leaves no-GC / typecheck / Live Graph guarantees untouched. See plan §3.
 *
 * The `+` operator needs type information: v1 spells numeric addition
 * `ADD a AND b` and text concatenation `a AND b`, and `ADD` rejects TEXT. So this
 * file carries a small type environment. Where a type cannot be established it
 * reports an error rather than guessing (spec §11.4).
 */

#include "luke2.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <iostream>

namespace luke2 {
namespace {

enum class Ty { Unknown, Int, Float, Str, Bool, Json, List, Map, Obj, Server, Request, Db, Void };

bool isNum(Ty t) { return t == Ty::Int || t == Ty::Float; }

Ty tyFromV2(const std::string &n) {
  if (n == "int") return Ty::Int;
  if (n == "float") return Ty::Float;
  if (n == "str") return Ty::Str;
  if (n == "bool") return Ty::Bool;
  if (n == "json") return Ty::Json;
  if (n == "list") return Ty::List;
  if (n == "map") return Ty::Map;
  if (n == "Server") return Ty::Server;
  if (n == "Request") return Ty::Request;
  if (n == "Db") return Ty::Db;
  if (n.empty()) return Ty::Unknown;
  return Ty::Obj;
}

/* v1 type word for a v2 annotation. Unknown annotations pass through (struct names). */
std::string v1Type(const std::string &n) {
  if (n == "int") return "INTEGER";
  if (n == "float") return "NUMBER";
  if (n == "str") return "TEXT";
  if (n == "bool") return "FLAG";
  if (n == "json") return "JSON";
  if (n == "list") return "LIST";
  if (n == "map") return "MAP";
  if (n == "Server") return "SERVER";
  if (n == "Request") return "REQUEST";
  if (n == "Db") return "DATABASE";
  return n;
}

Ty tyFromV1Word(const std::string &w) {
  if (w == "INTEGER") return Ty::Int;
  if (w == "NUMBER") return Ty::Float;
  if (w == "TEXT") return Ty::Str;
  if (w == "FLAG") return Ty::Bool;
  if (w == "JSON") return Ty::Json;
  if (w == "LIST") return Ty::List;
  if (w == "MAP") return Ty::Map;
  if (w == "SERVER") return Ty::Server;
  if (w == "REQUEST") return Ty::Request;
  if (w == "DATABASE") return Ty::Db;
  return Ty::Unknown;
}

Ty tyFromV2Word(const std::string &w) {
  if (w == "int") return Ty::Int;
  if (w == "float") return Ty::Float;
  if (w == "str") return Ty::Str;
  if (w == "bool") return Ty::Bool;
  if (w == "json") return Ty::Json;
  if (w == "list") return Ty::List;
  if (w == "map") return Ty::Map;
  if (w == "Server") return Ty::Server;
  if (w == "Request") return Ty::Request;
  if (w == "Db") return Ty::Db;
  return Ty::Unknown;
}

struct Lower {
  LowerOptions opt;
  std::ostringstream out;
  std::string err;
  size_t errLine = 0;

  std::vector<std::map<std::string, Ty>> scopes;
  std::map<std::string, Ty> fnRet;      /* function name -> return type */
  std::set<std::string> structs;        /* declared struct names */
  std::set<std::string> signals;        /* reactive cells: assignment -> CHANGE */
  size_t lastMarkerLine = 0;

  Lower() { scopes.emplace_back(); }

  void fail(size_t line, const std::string &m) {
    if (err.empty()) {
      err = m;
      errLine = line;
    }
  }

  void push() { scopes.emplace_back(); }
  void pop() {
    if (scopes.size() > 1) scopes.pop_back();
  }
  void bind(const std::string &n, Ty t) { scopes.back()[n] = t; }
  Ty lookup(const std::string &n) const {
    for (size_t i = scopes.size(); i-- > 0;) {
      auto it = scopes[i].find(n);
      if (it != scopes[i].end()) return it->second;
    }
    return Ty::Unknown;
  }
  bool known(const std::string &n) const {
    for (size_t i = scopes.size(); i-- > 0;) {
      if (scopes[i].count(n)) return true;
    }
    return false;
  }

  /* ---------- stdlib signatures ---------- */

  /* v1: `THIS IS FUNCTION … GIVES BACK TYPE`. v2: `fn name(...) -> type`. */
  void loadStdlibModule(const std::string &spec) {
    if (opt.stdlibDir.empty()) return;
    const std::string pre = "std/";
    if (spec.compare(0, pre.size(), pre) != 0) return;
    std::string mod = spec.substr(pre.size());
    std::ifstream in(opt.stdlibDir + "/" + mod + ".lk");
    if (!in) in.open(opt.stdlibDir + "/" + mod + ".luke");
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
      {
        std::string t = line;
        size_t i = 0;
        while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
        if (t.compare(i, 3, "fn ") == 0) {
          i += 3;
          while (i < t.size() && std::isspace((unsigned char)t[i])) ++i;
          size_t ns = i;
          while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '_')) ++i;
          std::string name = t.substr(ns, i - ns);
          auto arrow = t.find("->", i);
          if (!name.empty() && arrow != std::string::npos) {
            size_t ts = arrow + 2;
            while (ts < t.size() && std::isspace((unsigned char)t[ts])) ++ts;
            size_t te = ts;
            while (te < t.size() && (std::isalnum((unsigned char)t[te]) || t[te] == '_')) ++te;
            Ty ret = tyFromV2Word(t.substr(ts, te - ts));
            if (ret != Ty::Unknown) fnRet[name] = ret;
          }
          continue;
        }
      }
      std::string up;
      up.reserve(line.size());
      for (char c : line) up.push_back((char)toupper((unsigned char)c));
      auto fp = up.find("THIS IS FUNCTION ");
      if (fp == std::string::npos) continue;
      size_t ns = fp + 17;
      size_t ne = ns;
      while (ne < line.size() && (std::isalnum((unsigned char)line[ne]) || line[ne] == '_')) ++ne;
      std::string name = line.substr(ns, ne - ns);
      if (name.empty()) continue;
      Ty ret = Ty::Unknown;
      auto gb = up.find("GIVES BACK ");
      if (gb != std::string::npos) {
        size_t ts = gb + 11;
        size_t te = ts;
        while (te < up.size() && std::isalpha((unsigned char)up[te])) ++te;
        ret = tyFromV1Word(up.substr(ts, te - ts));
      }
      fnRet[name] = ret;
    }
  }

  /* ---------- type inference ---------- */

  Ty infer(const Ex &e) {
    switch (e.k) {
      case Ek::Int: return Ty::Int;
      case Ek::Float: return Ty::Float;
      case Ek::Str: return Ty::Str;
      case Ek::Bool: return Ty::Bool;
      case Ek::ListLit: return Ty::List;
      case Ek::MapLit: return Ty::Map;
      case Ek::Ident: {
        if (e.s == "current_user") return Ty::Str;
        return lookup(e.s);
      }
      case Ek::Unary: return e.s == "!" ? Ty::Bool : infer(e.kids[0]);
      case Ek::Binary: {
        const std::string &op = e.s;
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=" ||
            op == "&&" || op == "||")
          return Ty::Bool;
        Ty l = infer(e.kids[0]), r = infer(e.kids[1]);
        if (op == "+") {
          if (l == Ty::Str || r == Ty::Str) return Ty::Str;
          if (l == Ty::Float || r == Ty::Float) return Ty::Float;
          if (isNum(l) && isNum(r)) return Ty::Int;
          return Ty::Unknown;
        }
        if (op == "/") return Ty::Float;
        if (l == Ty::Float || r == Ty::Float) return Ty::Float;
        if (isNum(l) && isNum(r)) return Ty::Int;
        return Ty::Unknown;
      }
      case Ek::Call: {
        if (structs.count(e.s)) return Ty::Obj;
        auto it = fnRet.find(e.s);
        return it == fnRet.end() ? Ty::Unknown : it->second;
      }
      case Ek::Method: {
        if (e.s == "len") return Ty::Int;
        if (e.s == "has") return Ty::Bool;
        if (e.s == "last") return Ty::Str;
        auto it = fnRet.find(e.s);
        return it == fnRet.end() ? Ty::Unknown : it->second;
      }
      case Ek::Index: {
        Ty recv = infer(e.kids[0]);
        if (recv == Ty::Map || recv == Ty::List) return Ty::Str;
        return Ty::Unknown;
      }
      case Ek::Field: return Ty::Unknown;
    }
    return Ty::Unknown;
  }

  /* ---------- expression emission ---------- */

  std::string ident(const std::string &n) {
    if (n == "self") return "SELF";
    if (n == "current_user") return "THE CURRENT USER";
    if (n == "clock") return "THE CLOCK";
    if (n == "granular_paint_count") return "THE GRANULAR PAINT COUNT";
    if (n == "region_paint_count") return "THE REGION PAINT COUNT";
    if (n == "reactive_error_count") return "THE REACTIVE ERROR COUNT";
    if (n == "error_isolation_count") return "THE ERROR ISOLATION COUNT";
    if (n == "last_error_node") return "THE LAST ERROR NODE";
    if (n == "async_failure_count") return "THE ASYNC FAILURE COUNT";
    if (n == "flush_count") return "THE FLUSH COUNT";
    if (n == "dirty_count") return "THE DIRTY COUNT";
    if (n == "derived_count") return "THE DERIVED COUNT";
    if (n == "alive_count") return "THE ALIVE COUNT";
    if (n == "stale_count") return "THE STALE COUNT";
    if (n == "subtree_count") return "THE SUBTREE COUNT";
    if (n == "weak_count") return "THE WEAK COUNT";
    if (n == "leak_count") return "THE LEAK COUNT";
    if (n == "scope_count") return "THE SCOPE COUNT";
    if (n == "scheduler_count") return "THE SCHEDULER COUNT";
    if (n == "bench_median") return "THE BENCH MEDIAN";
    if (n == "bench_min") return "THE BENCH MIN";
    if (n == "bench_max") return "THE BENCH MAX";
    if (n == "bench_sample_count") return "THE BENCH SAMPLE COUNT";
    return n;
  }

  /* Parenthesise when the child could bind loosely in v1's word-operator grammar. */
  static bool needsParens(const Ex &e) {
    return e.k == Ek::Binary || e.k == Ek::Unary;
  }

  std::string sub(const Ex &e) {
    std::string s = expr(e);
    if (needsParens(e)) return "(" + s + ")";
    return s;
  }

  std::string expr(const Ex &e) {
    switch (e.k) {
      case Ek::Int:
      case Ek::Float: return e.s;
      case Ek::Str: return e.s;
      case Ek::Bool: return e.s == "true" ? "TRUE" : "FALSE";
      case Ek::Ident: return ident(e.s);

      case Ek::Field: {
        /* self.f -> SELF.f */
        return sub(e.kids[0]) + "." + e.s;
      }

      case Ek::Unary: {
        if (e.s == "!") return "NOT " + sub(e.kids[0]);
        return "0 SUBTRACT " + sub(e.kids[0]); /* unary minus */
      }

      case Ek::Binary: return binary(e);

      case Ek::Index: {
        Ty recv = infer(e.kids[0]);
        if (recv == Ty::Map) return "GET " + sub(e.kids[1]) + " FROM " + sub(e.kids[0]);
        if (recv == Ty::List) return "ITEM " + sub(e.kids[1]) + " OF " + sub(e.kids[0]);
        fail(e.line,
             "cannot tell whether '" + expr(e.kids[0]) +
                 "' is a list or a map — annotate it (`: list` or `: map`)");
        return "";
      }

      case Ek::Method: return method(e);

      case Ek::Call: return call(e);

      case Ek::ListLit:
      case Ek::MapLit:
        /* Empty literals are handled at the declaration site (AS LIST / AS MAP). */
        if (e.kids.empty()) return "";
        fail(e.line, "non-empty list/map literals are not supported yet — build them with push/index");
        return "";
    }
    return "";
  }

  std::string binary(const Ex &e) {
    const std::string &op = e.s;
    const Ex &L = e.kids[0];
    const Ex &R = e.kids[1];

    if (op == "+") {
      Ty l = infer(L), r = infer(R);
      /* String AND is associative and Play's SPEAK parser rejects parenthesised
         AND-chains — emit a flat chain (`a AND b AND c`) instead of nested
         `(a AND b) AND c`. Arithmetic still uses sub() so - / stay correct. */
      if (l == Ty::Str || r == Ty::Str) return expr(L) + " AND " + expr(R);
      if (isNum(l) && isNum(r)) return "ADD " + sub(L) + " AND " + sub(R);
      fail(e.line,
           "cannot tell whether '+' is numeric addition or text concatenation here — "
           "annotate the operands (`: int`, `: float` or `: str`)");
      return "";
    }
    if (op == "-") return sub(L) + " SUBTRACT " + sub(R);
    /* Play understands prefix MULTIPLY/DIVIDE … AND …; infix MULTIPLIED BY /
       DIVIDED BY are Build-only. Prefer the shared form. */
    if (op == "*") return "MULTIPLY " + sub(L) + " AND " + sub(R);
    if (op == "/") return "DIVIDE " + sub(L) + " AND " + sub(R);
    if (op == "%") {
      fail(e.line, "'%' has no Build-mode equivalent yet");
      return "";
    }
    if (op == "==") return sub(L) + " EQUALS " + sub(R);
    if (op == "!=") return sub(L) + " IS NOT " + sub(R);
    if (op == "<") return sub(L) + " IS LESS THAN " + sub(R);
    if (op == ">") return sub(L) + " IS GREATER THAN " + sub(R);
    if (op == "<=") return sub(L) + " IS LESS THAN OR EQUAL TO " + sub(R);
    if (op == ">=") return sub(L) + " IS GREATER THAN OR EQUAL TO " + sub(R);
    if (op == "&&") return sub(L) + " AND " + sub(R);
    if (op == "||") return sub(L) + " OR " + sub(R);
    fail(e.line, "unsupported operator '" + op + "'");
    return "";
  }

  std::string args(const std::vector<Ex> &kids, size_t from) {
    std::string s;
    for (size_t i = from; i < kids.size(); ++i) {
      if (i > from) s += ", ";
      s += expr(kids[i]);
    }
    return s;
  }

  std::string method(const Ex &e) {
    const Ex &recv = e.kids[0];
    /* super.m(args) -> CALL PARENT m */
    if (recv.k == Ek::Ident && recv.s == "super") {
      std::string s = "CALL PARENT " + e.s;
      if (e.kids.size() > 1) s += " WITH " + args(e.kids, 1);
      return s;
    }
    if (e.s == "len") return "HOW MANY IN " + sub(recv);
    if (e.s == "last") return "LAST OF " + sub(recv);
    if (e.s == "has" && e.kids.size() > 1) return "HAS KEY " + expr(e.kids[1]) + " IN " + sub(recv);
    if (e.s == "remove" && e.kids.size() > 1)
      return "DELETE " + expr(e.kids[1]) + " FROM " + sub(recv);
    /* generic: ASK obj TO m [WITH args] */
    std::string s = "ASK " + sub(recv) + " TO " + e.s;
    if (e.kids.size() > 1) s += " WITH " + args(e.kids, 1);
    return s;
  }

  std::string call(const Ex &e) {
    if (structs.count(e.s)) {
      std::string s = "NEW " + e.s;
      if (!e.kids.empty()) s += " WITH " + args(e.kids, 0);
      return s;
    }
    /* Runtime / UI intrinsics in stdlib: keep C-call spelling, not ASK. */
    if (e.s.rfind("__", 0) == 0) {
      std::string s = e.s + "(";
      for (size_t i = 0; i < e.kids.size(); ++i) {
        if (i) s += ", ";
        s += expr(e.kids[i]);
      }
      s += ")";
      return s;
    }
    std::string s = "ASK " + e.s;
    if (!e.kids.empty()) s += " WITH " + args(e.kids, 0);
    return s;
  }

  /* ---------- statement emission ---------- */

  void marker(size_t line) {
    if (!opt.emitMarkers || line == 0 || opt.sourcePath.empty()) return;
    out << "// @luke-file \"" << opt.sourcePath << "\" " << line << "\n";
    lastMarkerLine = line;
  }

  void emitBlock(const std::vector<St> &body, const std::string &ind) {
    push();
    for (const auto &s : body) {
      stmt(s, ind);
      if (!err.empty()) return;
    }
    pop();
  }

  std::string paramList(const std::vector<std::pair<std::string, std::string>> &ps) {
    std::string s;
    for (size_t i = 0; i < ps.size(); ++i) {
      if (i) s += ", ";
      s += ps[i].first;
      if (!ps[i].second.empty()) s += " AS " + v1Type(ps[i].second);
    }
    return s;
  }

  void bindParams(const std::vector<std::pair<std::string, std::string>> &ps) {
    for (const auto &p : ps) bind(p.first, tyFromV2(p.second));
  }

  void stmt(const St &s, const std::string &ind) {
    if (!err.empty()) return;
    marker(s.line);

    switch (s.k) {
      case Sk::Import: {
        out << ind << "IMPORT " << s.aux << "\n";
        loadStdlibModule(s.aux);
        break;
      }

      case Sk::Let: {
        Ty t = s.type.empty() ? infer(s.e) : tyFromV2(s.type);
        /* Empty list/map literal: v1 declares the container by type. */
        bool emptyContainer =
            (s.e.k == Ek::ListLit || s.e.k == Ek::MapLit) && s.e.kids.empty();
        if (emptyContainer) {
          Ty ct = s.e.k == Ek::ListLit ? Ty::List : Ty::Map;
          if (!s.type.empty()) ct = tyFromV2(s.type);
          bind(s.name, ct);
          out << ind << "MY NAME IS " << s.name << " AS "
              << (ct == Ty::Map ? "MAP" : "LIST") << "\n";
          break;
        }
        bind(s.name, t);
        out << ind << "MY NAME IS " << s.name;
        if (!s.type.empty()) out << " AS " << v1Type(s.type);
        /*
         * dbQuery() is a normal SQLite stdlib function, but the Build backend
         * already has a native intrinsic for it:
         *
         *   __luke_db_query(db, sql)
         *
         * Keep this special case limited to dbQuery().
         */
        if (s.e.k == Ek::Call && 
          s.e.s == "dbQuery" && 
          s.e.kids.size() == 2) {
            std::string db = expr(s.e.kids[0]);
            if (!err.empty()) return;
            std::string sql = expr(s.e.kids[1]);
            if (!err.empty()) return;
            out << " SET TO __luke_db_query("
                << db << ", " << sql << ")";
        } else if (s.e.k != Ek::Ident || !s.e.s.empty()) {
          std::string v = expr(s.e);
          if (!err.empty()) return;
          if (!v.empty()) out << " SET TO " << v;
        }
        out << "\n";
        break;
      }

      case Sk::Signal: {
        signals.insert(s.name);
        Ty t = s.type.empty() ? infer(s.e) : tyFromV2(s.type);
        bind(s.name, t);
        bool emptyLit =
            (s.e.k == Ek::ListLit || s.e.k == Ek::MapLit) && s.e.kids.empty();
        bool noInit = (s.e.k == Ek::Ident && s.e.s.empty() && s.e.kids.empty());
        /* REMEMBER x AS LIST|MAP|TEXT|… — typed empty reactive cell */
        if ((noInit || emptyLit) && !s.type.empty()) {
          out << ind << (s.secret ? "SECRET REMEMBER " : "REMEMBER ") << s.name
              << " AS " << v1Type(s.type) << "\n";
          break;
        }
        std::string v = expr(s.e);
        if (!err.empty()) return;
        if (v.empty() && !s.type.empty()) {
          out << ind << (s.secret ? "SECRET REMEMBER " : "REMEMBER ") << s.name
              << " AS " << v1Type(s.type) << "\n";
          break;
        }
        /* Keep the type word when present so TIMELINE INTO / fractional cells stay NUMBER. */
        if (!s.type.empty()) {
          out << ind << (s.secret ? "SECRET REMEMBER " : "REMEMBER ") << s.name << " AS "
              << v1Type(s.type) << " SET TO " << v << "\n";
          break;
        }
        out << ind << (s.secret ? "SECRET REMEMBER " : "REMEMBER ") << s.name << " AS " << v
            << "\n";
        break;
      }

      case Sk::Derived: {
        bind(s.name, infer(s.e));
        std::string v = expr(s.e);
        if (!err.empty()) return;
        out << ind << "THE " << s.name << " IS " << v << "\n";
        break;
      }

      case Sk::Effect: {
        if (s.name.empty()) {
          fail(s.line, "`effect` needs the cell it reacts to: `effect on <cell> { … }`");
          return;
        }
        std::string kw = "WHEN REACTIVE";
        if (s.aux2 == "background") kw = "WHEN BACKGROUND REACTIVE";
        else if (s.aux2 == "weak") kw = "WHEN REACTIVE WEAK";
        out << ind << kw << " " << s.name << " CHANGES DO\n";
        emitBlock(s.body, ind + "  ");
        if (!err.empty()) return;
        out << ind << "END WHEN REACTIVE\n";
        break;
      }

      case Sk::Batch: {
        out << ind << "BEGIN REACTIVE BATCH\n";
        emitBlock(s.body, ind + "  ");
        if (!err.empty()) return;
        out << ind << "END REACTIVE BATCH\n";
        break;
      }

      case Sk::Watch: {
        out << ind << "WATCH " << s.name;
        if (!s.aux.empty()) out << " FROM " << s.aux;
        /* aux2: "WHERE …" | "AS …" | either plus " FOR CURRENT USER" */
        if (!s.aux2.empty()) {
          if (s.aux2.rfind("AS ", 0) == 0 || s.aux2.rfind("WHERE ", 0) == 0)
            out << " " << s.aux2;
          else
            out << " WHERE " << s.aux2;
        }
        out << "\n";
        bind(s.name, Ty::Str);
        break;
      }

      case Sk::PushWatch: {
        out << ind << "PUSH WATCH " << s.name;
        if (!s.aux.empty()) out << " ON " << s.aux;
        out << s.aux2 << "\n";
        break;
      }

      case Sk::Assign: {
        const Ex &t = s.e;
        std::string v = expr(s.e2);
        if (!err.empty()) return;
        if (t.k == Ek::Ident) {
          if (signals.count(t.s)) out << ind << "CHANGE " << t.s << " TO " << v << "\n";
          else out << ind << "SET " << ident(t.s) << " TO " << v << "\n";
          break;
        }
        if (t.k == Ek::Field) {
          out << ind << "SET " << expr(t) << " TO " << v << "\n";
          break;
        }
        if (t.k == Ek::Index) {
          Ty recv = infer(t.kids[0]);
          if (recv == Ty::Map) {
            out << ind << "PUT " << expr(t.kids[1]) << " TO " << v << " IN " << expr(t.kids[0])
                << "\n";
            break;
          }
          if (recv == Ty::List) {
            out << ind << "SET ITEM " << expr(t.kids[1]) << " OF " << expr(t.kids[0]) << " TO "
                << v << "\n";
            break;
          }
          fail(s.line, "indexed assignment needs a list or map receiver");
          return;
        }
        fail(s.line, "unsupported assignment target");
        return;
      }

      case Sk::OpAssign: {
        if (s.e.k != Ek::Ident) {
          fail(s.line, "compound assignment needs a plain variable on the left");
          return;
        }
        std::string v = expr(s.e2);
        if (!err.empty()) return;
        if (s.aux == "+=") out << ind << "INCREASE " << s.e.s << " BY " << v << "\n";
        else if (s.aux == "-=") out << ind << "DECREASE " << s.e.s << " BY " << v << "\n";
        else {
          fail(s.line, "only '+=' and '-=' are supported");
          return;
        }
        break;
      }

      case Sk::ExprStmt: {
        if (s.name == "__raw__") {
          out << ind << s.aux << "\n";
          break;
        }
        /* print(x) -> SPEAK x */
        if (s.e.k == Ek::Call && s.e.s == "print") {
          if (s.e.kids.size() != 1) {
            fail(s.line, "print() takes exactly one argument");
            return;
          }
          std::string v = expr(s.e.kids[0]);
          if (!err.empty()) return;
          out << ind << "SPEAK " << v << "\n";
          break;
        }
        /* fill("id", body) -> FILL "id" WITH body */
        if (s.e.k == Ek::Call && s.e.s == "fill" && s.e.kids.size() == 2) {
          out << ind << "FILL " << expr(s.e.kids[0]) << " WITH " << expr(s.e.kids[1]) << "\n";
          break;
        }
        /* page.title("t") / page.style("""…""") */
        if (s.e.k == Ek::Method && s.e.kids.size() >= 1 && s.e.kids[0].k == Ek::Ident &&
            s.e.kids[0].s == "page") {
          if (s.e.s == "title" && s.e.kids.size() == 2) {
            out << ind << "NAME THE PAGE " << expr(s.e.kids[1]) << "\n";
            break;
          }
          if (s.e.s == "style" && s.e.kids.size() == 2) {
            out << ind << "WEAR STYLE " << expr(s.e.kids[1]) << "\n";
            break;
          }
          if (s.e.s == "font" && s.e.kids.size() == 3) {
            out << ind << "BRING FONT " << expr(s.e.kids[1]) << " FROM " << expr(s.e.kids[2])
                << "\n";
            break;
          }
        }
        /* bind.list(xs, "prefix") / bind("id", cell) */
        if (s.e.k == Ek::Method && s.e.kids.size() >= 1 && s.e.kids[0].k == Ek::Ident &&
            s.e.kids[0].s == "bind") {
          if (s.e.s == "list" && s.e.kids.size() == 3) {
            out << ind << "BIND LIST " << expr(s.e.kids[1]) << " AS " << expr(s.e.kids[2])
                << "\n";
            break;
          }
        }
        if (s.e.k == Ek::Call && s.e.s == "bind" && s.e.kids.size() == 2) {
          out << ind << "BIND " << expr(s.e.kids[0]) << " TO " << expr(s.e.kids[1]) << "\n";
          break;
        }
        /* q.refresh() -> REFRESH QUERY q */
        if (s.e.k == Ek::Method && s.e.s == "refresh" && s.e.kids.size() == 1) {
          out << ind << "REFRESH QUERY " << expr(s.e.kids[0]) << "\n";
          break;
        }
        /* paint() / layout() */
        if (s.e.k == Ek::Call && s.e.s == "paint" && s.e.kids.empty()) {
          out << ind << "PAINT THE SCREEN\n";
          break;
        }
        if (s.e.k == Ek::Call && s.e.s == "layout" && s.e.kids.empty()) {
          out << ind << "LAY OUT THE SCREEN\n";
          break;
        }
        /* xs.push(e) -> ADD e TO xs */
        if (s.e.k == Ek::Method && s.e.s == "push" && s.e.kids.size() == 2) {
          std::string v = expr(s.e.kids[1]);
          if (!err.empty()) return;
          out << ind << "ADD " << v << " TO " << expr(s.e.kids[0]) << "\n";
          break;
        }
        std::string v = expr(s.e);
        if (!err.empty()) return;
        out << ind << v << "\n";
        break;
      }

      case Sk::If: {
        std::string c = expr(s.e);
        if (!err.empty()) return;
        out << ind << "IF " << c << " DO\n";
        emitBlock(s.body, ind + "  ");
        if (!err.empty()) return;
        if (!s.body2.empty()) {
          out << ind << "ELSE\n";
          emitBlock(s.body2, ind + "  ");
          if (!err.empty()) return;
        }
        out << ind << "END IF\n";
        break;
      }

      case Sk::While: {
        std::string c = expr(s.e);
        if (!err.empty()) return;
        out << ind << "WHILE " << c << " DO\n";
        emitBlock(s.body, ind + "  ");
        if (!err.empty()) return;
        out << ind << "END WHILE\n";
        break;
      }

      case Sk::For: {
        if (s.e2.k != Ek::Ident || !s.e2.s.empty() || !s.e2.kids.empty()) {
          fail(s.line, "range `for` is not supported yet — use `while`");
          return;
        }
        out << ind << "FOR EACH " << s.name << " IN " << expr(s.e) << " DO\n";
        push();
        bind(s.name, Ty::Unknown);
        emitBlock(s.body, ind + "  ");
        pop();
        if (!err.empty()) return;
        out << ind << "END FOR EACH\n";
        break;
      }

      case Sk::Fn: {
        fnRet[s.name] = tyFromV2(s.type);
        out << ind << "THIS IS FUNCTION " << s.name;
        if (!s.params.empty()) out << " WITH " << paramList(s.params);
        if (!s.type.empty()) out << " GIVES BACK " << v1Type(s.type);
        out << " DO\n";
        push();
        bindParams(s.params);
        for (const auto &b : s.body) {
          stmt(b, ind + "  ");
          if (!err.empty()) return;
        }
        pop();
        out << ind << "END FUNCTION\n";
        break;
      }

      case Sk::Return: {
        bool empty = s.e.k == Ek::Ident && s.e.s.empty() && s.e.kids.empty();
        if (empty) {
          out << ind << "GIVE BACK 0\n";
          break;
        }
        std::string v = expr(s.e);
        if (!err.empty()) return;
        out << ind << "GIVE BACK " << v << "\n";
        break;
      }

      case Sk::Struct: {
        structs.insert(s.name);
        out << ind << "BLUEPRINT " << s.name;
        if (!s.aux.empty()) out << " FOLLOWS " << s.aux;
        out << " DO\n";
        push();
        for (const auto &m : s.body) {
          member(m, ind + "  ");
          if (!err.empty()) return;
        }
        pop();
        out << ind << "END BLUEPRINT\n";
        break;
      }

      case Sk::Try: {
        out << ind << "ATTEMPT DO\n";
        emitBlock(s.body, ind + "  ");
        if (!err.empty()) return;
        if (!s.body2.empty() || !s.aux.empty()) {
          out << ind << "OTHERWISE WITH " << (s.aux.empty() ? "err" : s.aux) << " DO\n";
          push();
          bind(s.aux.empty() ? "err" : s.aux, Ty::Str);
          for (const auto &b : s.body2) {
            stmt(b, ind + "  ");
            if (!err.empty()) return;
          }
          pop();
        }
        out << ind << "END ATTEMPT\n";
        break;
      }

      case Sk::Throw: {
        std::string v = expr(s.e);
        if (!err.empty()) return;
        out << ind << "GIVE UP WITH " << v << "\n";
        break;
      }

      case Sk::Test: {
        out << ind << "TEST " << s.aux << " DO\n";
        emitBlock(s.body, ind + "  ");
        if (!err.empty()) return;
        out << ind << "END TEST\n";
        break;
      }

      case Sk::Assert: {
        std::string v = expr(s.e);
        if (!err.empty()) return;
        out << ind << "MAKE SURE " << v << "\n";
        break;
      }

      case Sk::Arena: {
        out << ind << "IN ARENA DO\n";
        emitBlock(s.body, ind + "  ");
        if (!err.empty()) return;
        out << ind << "END ARENA\n";
        break;
      }

      case Sk::Break: out << ind << "GET OUTTA HERE\n"; break;

      default: fail(s.line, "unsupported statement in v2 lowering"); return;
    }
  }

  void member(const St &m, const std::string &ind) {
    marker(m.line);
    switch (m.k) {
      case Sk::FieldDecl: {
        out << ind << "HAS " << m.name;
        if (!m.type.empty()) out << " AS " << v1Type(m.type);
        bool hasInit = !(m.e.k == Ek::Ident && m.e.s.empty() && m.e.kids.empty());
        if (hasInit) {
          std::string v = expr(m.e);
          if (!err.empty()) return;
          out << " SET TO " << v;
        }
        out << "\n";
        bind(m.name, m.type.empty() ? infer(m.e) : tyFromV2(m.type));
        break;
      }
      case Sk::Init: {
        out << ind << "WHEN BORN";
        if (!m.params.empty()) out << " WITH " << paramList(m.params);
        out << " DO\n";
        push();
        bindParams(m.params);
        for (const auto &b : m.body) {
          stmt(b, ind + "  ");
          if (!err.empty()) return;
        }
        pop();
        out << ind << "END BORN\n";
        break;
      }
      case Sk::Method: {
        fnRet[m.name] = tyFromV2(m.type);
        out << ind << (m.isPrivate ? "PRIVATE METHOD " : "METHOD ") << m.name;
        if (!m.params.empty()) out << " WITH " << paramList(m.params);
        if (!m.type.empty()) out << " GIVES BACK " << v1Type(m.type);
        out << " DO\n";
        push();
        bindParams(m.params);
        for (const auto &b : m.body) {
          stmt(b, ind + "  ");
          if (!err.empty()) return;
        }
        pop();
        out << ind << "END METHOD\n";
        break;
      }
      default: fail(m.line, "unsupported struct member"); return;
    }
  }

  /* Hoist declarations so forward references resolve (v1 programs call handlers
   * declared above `httpServe`, but struct/fn order in v2 should not matter). */
  void predeclare(const std::vector<St> &stmts) {
    for (const auto &s : stmts) {
      if (s.k == Sk::Struct) structs.insert(s.name);
      else if (s.k == Sk::Fn) fnRet[s.name] = tyFromV2(s.type);
      else if (s.k == Sk::Import) loadStdlibModule(s.aux);
    }
  }
};

}  // namespace

Result lower(const Program &p, const LowerOptions &opt) {
  Result r;
  if (!p.ok()) {
    r.error = p.error;
    r.line = p.errorLine;
    return r;
  }
  Lower L;
  L.opt = opt;
  L.predeclare(p.stmts);
  for (const auto &s : p.stmts) {
    L.stmt(s, "");
    if (!L.err.empty()) break;
  }
  if (!L.err.empty()) {
    r.error = L.err;
    r.line = L.errLine;
    return r;
  }
  r.ok = true;
  r.v1 = L.out.str();
  return r;
}

Result lowerSource(const std::string &src, const LowerOptions &opt) {
  std::string lexErr;
  size_t lexLine = 0;
  auto toks = lex(src, &lexErr, &lexLine);
  if (!lexErr.empty()) {
    Result r;
    r.error = lexErr;
    r.line = lexLine;
    return r;
  }
  return lower(parse(toks), opt);
}

std::string maybeLowerSource(const std::string &path, const std::string &raw, SyntaxMode mode,
                             const std::string &stdlibDir, bool *ok, std::string *errOut,
                             size_t *errLine) {
  if (ok) *ok = true;
  if (!wantsV2(path, mode)) return raw;
  LowerOptions lo;
  lo.sourcePath = path;
  lo.stdlibDir = stdlibDir;
  auto res = lowerSource(raw, lo);
  if (!res.ok) {
    if (ok) *ok = false;
    if (errOut) *errOut = res.error;
    if (errLine) *errLine = res.line;
    return {};
  }
  return res.v1;
}

}  // namespace luke2
