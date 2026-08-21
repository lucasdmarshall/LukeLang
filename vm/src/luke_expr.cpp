#include "luke_expr.hpp"

#include <cctype>
#include <sstream>

namespace luke {
namespace {

bool isIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool isIdent(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

std::string toUpperCopy(std::string s) {
  for (char &c : s) c = (char)std::toupper((unsigned char)c);
  return s;
}

/* Multi-word operators longest-first (uppercased needle → TokKind). */
struct OpLit {
  const char *u;
  TokKind k;
};
static const OpLit kOps[] = {
    {"IS LESS THAN OR EQUAL TO", TokKind::OpLe},
    {"IS GREATER THAN OR EQUAL TO", TokKind::OpGe},
    {"IS DIVISIBLE BY", TokKind::OpDivisible},
    {"IS LESS THAN", TokKind::OpLt},
    {"IS GREATER THAN", TokKind::OpGt},
    {"IS EQUAL TO", TokKind::OpEq},
    {"MULTIPLIED BY", TokKind::OpMul},
    {"MULTIPLY BY", TokKind::OpMul},
    {"DIVIDED BY", TokKind::OpDiv},
    {"SUBTRACT", TokKind::OpSub},
    {"MULTIPLY", TokKind::OpMul},
    {"DIVIDE", TokKind::OpDiv},
    {"EQUALS", TokKind::OpEq},
    {"ADD", TokKind::OpAdd},
    {"AND", TokKind::OpAnd},
    {"NOT", TokKind::OpNot},
};

bool startsWord(const std::string &U, size_t i, const char *word) {
  size_t n = std::char_traits<char>::length(word);
  if (i + n > U.size()) return false;
  if (U.compare(i, n, word) != 0) return false;
  if (i + n < U.size() && isIdent(U[i + n])) return false;
  if (i > 0 && isIdent(U[i - 1])) return false;
  return true;
}

int infixPrec(TokKind k) {
  switch (k) {
  case TokKind::OpAnd:
    return 10;
  case TokKind::OpEq:
  case TokKind::OpLt:
  case TokKind::OpGt:
  case TokKind::OpLe:
  case TokKind::OpGe:
  case TokKind::OpDivisible:
    return 20;
  case TokKind::OpAdd:
  case TokKind::OpSub:
    return 30;
  case TokKind::OpMul:
  case TokKind::OpDiv:
    return 40;
  default:
    return -1;
  }
}

const char *opC(TokKind k, bool asIntFn) {
  switch (k) {
  case TokKind::OpAdd:
    return asIntFn ? "luke_i64_add" : "+";
  case TokKind::OpSub:
    return asIntFn ? "luke_i64_sub" : "-";
  case TokKind::OpMul:
    return asIntFn ? "luke_i64_mul" : "*";
  case TokKind::OpDiv:
    return "/";
  case TokKind::OpEq:
    return "==";
  case TokKind::OpLt:
    return "<";
  case TokKind::OpGt:
    return ">";
  case TokKind::OpLe:
    return "<=";
  case TokKind::OpGe:
    return ">=";
  default:
    return "?";
  }
}

} // namespace

std::vector<Token> tokenizeExpr(const std::string &src) {
  std::vector<Token> out;
  std::string U = toUpperCopy(src);
  size_t i = 0, n = src.size();
  auto skipWs = [&] {
    while (i < n && std::isspace((unsigned char)src[i])) ++i;
  };
  while (true) {
    skipWs();
    if (i >= n) break;
    Token t;
    t.pos = i;
    char c = src[i];
    if (c == '(') {
      t.kind = TokKind::LParen;
      t.text = "(";
      ++i;
      out.push_back(t);
      continue;
    }
    if (c == ')') {
      t.kind = TokKind::RParen;
      t.text = ")";
      ++i;
      out.push_back(t);
      continue;
    }
    if (c == '"' || c == '\'') {
      char q = c;
      ++i;
      std::string lit;
      while (i < n && src[i] != q) {
        if (src[i] == '\\' && i + 1 < n) {
          char nch = src[++i];
          ++i;
          if (nch == 'n') lit.push_back('\n');
          else if (nch == 't') lit.push_back('\t');
          else if (nch == 'r') lit.push_back('\r');
          else lit.push_back(nch); /* \\ \" and unknown keep the escaped char */
          continue;
        }
        lit.push_back(src[i++]);
      }
      if (i < n) ++i;
      t.kind = TokKind::String;
      t.text = lit;
      out.push_back(t);
      continue;
    }
    if (std::isdigit((unsigned char)c) ||
        (c == '.' && i + 1 < n && std::isdigit((unsigned char)src[i + 1]))) {
      size_t s = i;
      while (i < n && (std::isdigit((unsigned char)src[i]) || src[i] == '.')) ++i;
      t.kind = TokKind::Number;
      t.text = src.substr(s, i - s);
      out.push_back(t);
      continue;
    }
    /* Multi-word ops */
    bool gotOp = false;
    for (auto &op : kOps) {
      if (startsWord(U, i, op.u)) {
        size_t len = std::char_traits<char>::length(op.u);
        t.kind = op.k;
        t.text = src.substr(i, len);
        i += len;
        out.push_back(t);
        gotOp = true;
        break;
      }
    }
    if (gotOp) continue;

    if (isIdentStart(c)) {
      size_t s = i;
      while (i < n && isIdent(src[i])) ++i;
      /* Glue "THE …" phrases as single Ident for resolve (THE CLOCK etc.) */
      std::string word = src.substr(s, i - s);
      if (toUpperCopy(word) == "THE") {
        while (i < n) {
          skipWs();
          if (i >= n || !isIdentStart(src[i])) break;
          /* Stop before an infix op word */
          bool stop = false;
          for (auto &op : kOps) {
            if (startsWord(U, i, op.u)) {
              stop = true;
              break;
            }
          }
          if (stop) break;
          size_t s2 = i;
          while (i < n && isIdent(src[i])) ++i;
          word.push_back(' ');
          word += src.substr(s2, i - s2);
        }
      }
      t.kind = TokKind::Ident;
      t.text = word;
      out.push_back(t);
      continue;
    }
    t.kind = TokKind::Unknown;
    t.text = std::string(1, c);
    ++i;
    out.push_back(t);
  }
  Token end;
  end.kind = TokKind::End;
  end.pos = i;
  out.push_back(end);
  return out;
}

namespace {

struct Parser {
  const std::vector<Token> &toks;
  size_t i = 0;
  size_t line = 0;
  const ExprLower *ctx = nullptr;

  const Token &cur() const { return toks[i < toks.size() ? i : toks.size() - 1]; }
  void advance() {
    if (i + 1 < toks.size()) ++i;
  }

  Ast parse(int minPrec) {
    Ast left = nud();
    while (true) {
      const Token &t = cur();
      int p = infixPrec(t.kind);
      if (p < minPrec) break;
      TokKind op = t.kind;
      advance();
      Ast right = parse(p + 1); /* left-assoc */
      Ast bin;
      bin.kind = AstKind::Binary;
      bin.op = op;
      bin.line = line;
      bin.kids.push_back(std::move(left));
      bin.kids.push_back(std::move(right));
      left = std::move(bin);
    }
    return left;
  }

  Ast nud() {
    const Token &t = cur();
    if (t.kind == TokKind::OpNot) {
      advance();
      Ast inner = parse(50);
      Ast u;
      u.kind = AstKind::Unary;
      u.op = TokKind::OpNot;
      u.line = line;
      u.kids.push_back(std::move(inner));
      return u;
    }
    if (t.kind == TokKind::LParen) {
      advance();
      Ast inner = parse(0);
      if (cur().kind != TokKind::RParen) {
        if (ctx && ctx->fail) ctx->fail(line, "Expected ')' in expression");
      } else
        advance();
      Ast g;
      g.kind = AstKind::Group;
      g.line = line;
      g.kids.push_back(std::move(inner));
      return g;
    }
    if (t.kind == TokKind::Number) {
      Ast a;
      a.kind = AstKind::Number;
      a.text = t.text;
      a.line = line;
      advance();
      return a;
    }
    if (t.kind == TokKind::String) {
      Ast a;
      a.kind = AstKind::String;
      a.text = t.text;
      a.line = line;
      advance();
      return a;
    }
    if (t.kind == TokKind::Ident) {
      Ast a;
      a.kind = AstKind::Ident;
      a.text = t.text;
      a.line = line;
      advance();
      /* Multi-word primary: continue consuming Idents into one phrase for resolve
       * e.g. already handled THE … in lexer; single idents stay. */
      return a;
    }
    if (ctx && ctx->fail) ctx->fail(line, "Unexpected token in expression '" + t.text + "'");
    Ast err;
    err.kind = AstKind::Ident;
    err.text = "0";
    advance();
    return err;
  }
};

} // namespace

Ast parseExprAst(const std::vector<Token> &toks, size_t line, const ExprLower &ctx,
                 size_t *outConsumed) {
  Parser p{toks, 0, line, &ctx};
  if (toks.empty() || toks[0].kind == TokKind::End) {
    Ast z;
    z.kind = AstKind::Number;
    z.text = "0";
    if (outConsumed) *outConsumed = 0;
    return z;
  }
  Ast a = p.parse(0);
  if (outConsumed) *outConsumed = p.i;
  return a;
}

std::pair<std::string, std::string> lowerExprAst(const Ast &ast, size_t line, const ExprLower &ctx) {
  auto lower = [&](auto &&self, const Ast &n) -> std::pair<std::string, std::string> {
    switch (n.kind) {
    case AstKind::Number: {
      bool isInt = n.text.find('.') == std::string::npos;
      if (isInt) return {n.text + "LL", "int"};
      return {n.text, "num"};
    }
    case AstKind::String: {
      std::string esc;
      for (char c : n.text) {
        if (c == '\\') esc += "\\\\";
        else if (c == '"') esc += "\\\"";
        else if (c == '\n') esc += "\\n";
        else if (c == '\t') esc += "\\t";
        else if (c == '\r') esc += "\\r";
        else esc.push_back(c);
      }
      return {"luke_text(\"" + esc + "\")", "text"};
    }
    case AstKind::Ident: {
      if (ctx.resolve) return ctx.resolve(n.text, line);
      return {"0", "err"};
    }
    case AstKind::Group:
      if (!n.kids.empty()) return self(self, n.kids[0]);
      return {"0", "num"};
    case AstKind::Unary: {
      if (n.kids.empty()) return {"0", "flag"};
      auto x = self(self, n.kids[0]);
      return {"(!(" + x.first + "))", "flag"};
    }
    case AstKind::Empty:
      return {std::string("0"), std::string("err")};
    case AstKind::Call:
      return {std::string("0"), std::string("legacy")};
    case AstKind::Binary: {
      if (n.kids.size() < 2) return {"0", "err"};
      auto L = self(self, n.kids[0]);
      auto R = self(self, n.kids[1]);
      if (n.op == TokKind::OpAnd) {
        auto toText = [&](std::pair<std::string, std::string> v) {
          if (v.second == "text") return v.first;
          if (v.second == "int") return "luke_integer_to_text(" + ctx.arenaVar + ", " + v.first + ")";
          if (v.second == "flag") return "((" + v.first + ") ? luke_text(\"YES\") : luke_text(\"NO\"))";
          return "luke_number_to_text(" + ctx.arenaVar + ", " + v.first + ")";
        };
        return {"luke_text_concat(" + ctx.arenaVar + ",(" + toText(L) + "),(" + toText(R) + "))",
                "text"};
      }
      if (n.op == TokKind::OpDivisible) {
        if (L.second == "int" && R.second == "int")
          return {"luke_i64_divisible(" + L.first + ", " + R.first + ")", "flag"};
        auto toNum = [](std::pair<std::string, std::string> v) {
          if (v.second == "int") return std::string("((double)(") + v.first + "))";
          return v.first;
        };
        return {"luke_divisible(" + toNum(L) + ", " + toNum(R) + ")", "flag"};
      }
      bool cmp = n.op == TokKind::OpEq || n.op == TokKind::OpLt || n.op == TokKind::OpGt ||
                 n.op == TokKind::OpLe || n.op == TokKind::OpGe;
      if (cmp) {
        if (L.second == "text" && R.second == "text" && n.op == TokKind::OpEq)
          return {"(luke_text_eq((" + L.first + "),(" + R.first + ")))", "flag"};
        auto widen = [](std::pair<std::string, std::string> v) -> std::pair<std::string, std::string> {
          if (v.second == "int")
            return {std::string("((double)(") + v.first + "))", std::string("num")};
          return v;
        };
        auto Lw = widen(L), Rw = widen(R);
        return {std::string("(") + Lw.first + opC(n.op, false) + Rw.first + ")", std::string("flag")};
      }
      /* arithmetic */
      if (L.second == "int" && R.second == "int" && n.op != TokKind::OpDiv) {
        return {std::string(opC(n.op, true)) + "(" + L.first + ", " + R.first + ")",
                std::string("int")};
      }
      auto toNum = [](std::pair<std::string, std::string> v) {
        if (v.second == "int") return std::string("((double)(") + v.first + "))";
        return v.first;
      };
      return {std::string("(") + toNum(L) + opC(n.op, false) + toNum(R) + ")", std::string("num")};
    }
    default:
      return {std::string("0"), std::string("err")};
    }
  };
  return lower(lower, ast);
}

std::pair<std::string, std::string> compileExpr(const std::string &src, size_t line,
                                               const ExprLower &ctx) {
  (void)line;
  std::string U = toUpperCopy(src);
  if (U.find("ASK ") != std::string::npos || U.find(" IF ") == 0 || U.find("IF ") == 0 ||
      U.find(" ITEM ") != std::string::npos || U.find("ITEM ") == 0 ||
      U.find(" GET ") != std::string::npos || U.find("GET ") == 0 ||
      U.find("HOW MANY") != std::string::npos || U.find("HAS KEY") != std::string::npos ||
      U.find(" NEW ") != std::string::npos || U.find("NEW ") == 0 ||
      U.find(" THE TEXT WIDTH") != std::string::npos || U.find("THE MEASURED") != std::string::npos ||
      U.find("THE NODE ID") != std::string::npos || U.find("THE DEP COUNT") != std::string::npos ||
      U.find("THE SUB COUNT") != std::string::npos || U.find("THE WHY ") != std::string::npos ||
      U.find("THE TIMELINE") != std::string::npos || U.find("THE BOUNDARY") != std::string::npos ||
      U.find("READY OF") != std::string::npos || U.find("STATUS OF") != std::string::npos ||
      U.find("BODY OF") != std::string::npos || U.find("COUNT OF") != std::string::npos ||
      U.find("__LUKE_") != std::string::npos) {
    return {"", "legacy"};
  }
  auto toks = tokenizeExpr(src);
  for (size_t i = 0; i + 1 < toks.size(); ++i) {
    if (toks[i].kind == TokKind::Ident && toks[i + 1].kind == TokKind::LParen)
      return {"", "legacy"};
    if (toks[i].kind == TokKind::Unknown) return {"", "legacy"};
  }
  /* Prefix "ADD a AND b" / "SUBTRACT …" still legacy. */
  if (!toks.empty() &&
      (toks[0].kind == TokKind::OpAdd || toks[0].kind == TokKind::OpSub ||
       toks[0].kind == TokKind::OpMul || toks[0].kind == TokKind::OpDiv))
    return {"", "legacy"};
  /* Quiet parse — do not fail the compile on exploratory AST errors; fall back. */
  ExprLower quiet = ctx;
  quiet.fail = nullptr;
  size_t consumed = 0;
  Ast ast = parseExprAst(toks, line, quiet, &consumed);
  /* Incomplete consume (e.g. HOW MANY IN nums → Ident HOW + leftovers) → legacy. */
  if (consumed >= toks.size() || toks[consumed].kind != TokKind::End) return {"", "legacy"};
  auto out = lowerExprAst(ast, line, ctx);
  if (out.second == "err" || out.first.empty()) return {"", "legacy"};
  return out;
}

std::string formatExpr(const std::string &src) {
  auto toks = tokenizeExpr(src);
  std::ostringstream o;
  bool first = true;
  auto isComma = [](const Token &t) {
    return t.kind == TokKind::Unknown && t.text == ",";
  };
  auto isDot = [](const Token &t) {
    return t.kind == TokKind::Unknown && t.text == ".";
  };
  for (auto &t : toks) {
    if (t.kind == TokKind::End) break;
    /* Spacing: no space before ), ,, . — and never rewrite casing.
     * Operators keep the source spelling (add vs ADD); uppercasing broke
     * user-defined names that share spellings with keywords (ASK add → ASK ADD).
     * Dots stay tight (SELF.name), not SELF . name. */
    bool skipSpace = first || t.kind == TokKind::RParen || isComma(t) || isDot(t);
    if (!skipSpace) o << ' ';
    if (t.kind == TokKind::String) {
      o << '"';
      for (char c : t.text) {
        if (c == '\\') o << "\\\\";
        else if (c == '"') o << "\\\"";
        else if (c == '\n') o << "\\n";
        else if (c == '\t') o << "\\t";
        else if (c == '\r') o << "\\r";
        else o << c;
      }
      o << '"';
    } else {
      o << t.text; /* preserve original spelling for ops, idents, numbers, commas */
    }
    first = false;
    if (t.kind == TokKind::LParen || isDot(t)) first = true; /* no space after ( or . */
    /* After `,` keep first=false so the next token gets a single space: `2, 3`. */
  }
  return o.str();
}

} // namespace luke
