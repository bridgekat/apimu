#include "evaluator.hpp"
// TEMP CODE?
#include <fstream>
#include <iostream>

namespace Eval {

  using std::string;
  using std::vector;
  using std::pair;
  using std::optional, std::visit, std::get_if;
  using Core::Allocator;
  using Parsing::Token, Parsing::NFALexer, Parsing::EarleyParser;

  bool Evaluator::parseNextStatement() {
    return parser.nextSentence();
    // std::cout << parser.showStates(symbolNames) << std::endl;
  }

  Tree* Evaluator::evalParsedStatement() {
    auto e = resolve(); // std::cout << e->toString() << std::endl;
    e = expand(e);
    std::cout << e->toString() << std::endl;
    e = eval(globalEnv, e);
    return e;
  }

  vector<ParsingError> Evaluator::popParsingErrors() {
    vector<ParsingError> res;
    // See:
    // https://stackoverflow.com/questions/30448182/is-it-safe-to-use-a-c11-range-based-for-loop-with-an-rvalue-range-init
    for (auto const& e: lexer.popErrors()) {
      res.emplace_back("Parsing error, unexpected characters: " + e.lexeme, e.startPos, e.endPos);
    }
    for (auto const& e: parser.popErrors()) {
      string s = "Parsing error, expected one of: ";
      for (Parsing::Symbol sym: e.expected) s += "<" + symbolNames[sym] + ">, ";
      if (e.got) s += "got token <" + symbolNames[*e.got] + ">";
      else s += "but reached the end of file";
      res.emplace_back(s, e.startPos, e.endPos);
    }
    return res;
  }

  // Match an Tree against another Tree (pattern)
  // See: https://github.com/digama0/mm0/blob/master/mm0-hs/mm1.md#syntax-forms
  // (Continuation, `__k`, `and`, `or`, `not` and `pred?` patterns are not implemented)
  bool Evaluator::match(Tree* e, Tree* pat, Tree*& env, bool quoteMode) {
    if (auto const& sym = get_if<Symbol>(pat); sym && !quoteMode) {
      if (sym->val != "_") env = extend(env, sym->val, e);
      return true;
    }
    if (auto const& cons = get_if<Cons>(pat)) {
      auto const& [h, t] = *cons;
      if (auto const& sym = get_if<Symbol>(h)) {
        if (sym->val == "quote" && !quoteMode) return match(e, expect<Cons>(t).head, env, true);   // Enter quote mode
        if (sym->val == "unquote" && quoteMode) return match(e, expect<Cons>(t).head, env, false); // Leave quote mode
        if (sym->val == "...") return get_if<Nil>(e) || get_if<Cons>(e);
      }
      auto const& econs = get_if<Cons>(e);
      return econs && match(econs->head, h, env, quoteMode) && match(econs->tail, t, env, quoteMode);
    }
    return *e == *pat;
  }

  vector<unsigned int> stringToCharVec(string const& s) {
    vector<unsigned int> res;
    for (char c: s) res.push_back(static_cast<unsigned int>(c));
    return res;
  }

  NFALexer::NFA Evaluator::treePattern(Tree* e) {
    auto const& [tag, t] = expect<Cons>(e);
    string stag = expect<Symbol>(tag).val;
    if (stag == "empty") return lexer.empty();
    if (stag == "any") return lexer.any();
    if (stag == "utf8seg") return lexer.utf8segment();
    if (stag == "char") return lexer.chars(stringToCharVec(expect<String>(expect<Cons>(t).head).val));
    if (stag == "except") return lexer.except(stringToCharVec(expect<String>(expect<Cons>(t).head).val));
    if (stag == "range") {
      auto const& [lbound, u] = expect<Cons>(t);
      auto const& [ubound, _] = expect<Cons>(u);
      return lexer.range(
        static_cast<unsigned int>(expect<Nat64>(lbound).val),
        static_cast<unsigned int>(expect<Nat64>(ubound).val)
      );
    }
    if (stag == "word") return lexer.word(stringToCharVec(expect<String>(expect<Cons>(t).head).val));
    if (stag == "alt") return lexer.alt(listPatterns(t));
    if (stag == "concat") return lexer.concat(listPatterns(t));
    if (stag == "opt") return lexer.opt(treePattern(expect<Cons>(t).head));
    if (stag == "star") return lexer.star(treePattern(expect<Cons>(t).head));
    if (stag == "plus") return lexer.plus(treePattern(expect<Cons>(t).head));
    unimplemented;
  }

  vector<NFALexer::NFA> Evaluator::listPatterns(Tree* e) {
    vector<NFALexer::NFA> res;
    for (auto it = get_if<Cons>(e); it; it = get_if<Cons>(it->tail)) { res.push_back(treePattern(it->head)); }
    return res;
  }

  vector<pair<Parsing::Symbol, Parsing::Precedence>> Evaluator::listSymbols(Tree* e) {
    vector<pair<Parsing::Symbol, Parsing::Precedence>> res;
    for (auto it = get_if<Cons>(e); it; it = get_if<Cons>(it->tail)) {
      auto const& [sym, t] = expect<Cons>(it->head);
      auto const& [pre, _] = expect<Cons>(t);
      res.emplace_back(getSymbol(expect<Symbol>(sym).val), expect<Nat64>(pre).val);
    }
    return res;
  }

  // TODO: handle exceptions, refactor
  void Evaluator::setSyntax(Tree* p, Tree* r) {
    symbolNames.clear();
    nameSymbols.clear();
    patternNames.clear();
    ruleNames.clear();
    lexer.clearPatterns();
    parser.clearPatterns();
    parser.clearRules();

    patterns = p;
    rules = r;

    // Add ignored and starting symbols
    symbolNames.push_back("_");
    parser.setIgnoredSymbol(IgnoredSymbol);
    symbolNames.push_back("_");
    parser.setStartSymbol(StartSymbol);

    // Add patterns
    for (auto it = get_if<Cons>(patterns); it; it = get_if<Cons>(it->tail)) {
      auto const& [name, t] = expect<Cons>(it->head);
      auto const& [lhs, u] = expect<Cons>(t);
      auto const& [rhs, _1] = expect<Cons>(u);
      auto const& [sym, v] = expect<Cons>(lhs);
      auto const& [pre, _2] = expect<Cons>(v);
      string sname = expect<Symbol>(sym).val;
      size_t sid = (sname == "_") ? IgnoredSymbol : getSymbol(sname);
      size_t pid = patternNames.size();
      patternNames.push_back(expect<Symbol>(name).val);
      if (lexer.addPattern(treePattern(rhs)) != pid) unreachable;
      if (parser.addPattern(sid, static_cast<Parsing::Precedence>(expect<Nat64>(pre).val)) != pid) unreachable;
    }

    // Add rules
    for (auto it = get_if<Cons>(rules); it; it = get_if<Cons>(it->tail)) {
      auto const& [name, t] = expect<Cons>(it->head);
      auto const& [lhs, u] = expect<Cons>(t);
      auto const& [rhs, _1] = expect<Cons>(u);
      auto const& [sym, v] = expect<Cons>(lhs);
      auto const& [pre, _2] = expect<Cons>(v);
      string sname = expect<Symbol>(sym).val;
      size_t sid = (sname == "_") ? StartSymbol : getSymbol(sname);
      size_t rid = ruleNames.size();
      ruleNames.push_back(expect<Symbol>(name).val);
      if (parser.addRule(sid, static_cast<Parsing::Precedence>(expect<Nat64>(pre).val), listSymbols(rhs)) != rid)
        unreachable;
    }
  }

#define cons       pool.emplace
#define nil        nil
#define sym(s)     pool.emplace(Symbol{s})
#define str(s)     pool.emplace(String{s})
#define nat(n)     pool.emplace(Nat64{n})
#define boolean(b) pool.emplace(Bool{b})
#define unit       unit
#define list(...)  makeList({__VA_ARGS__})

  Tree* Evaluator::makeList(std::initializer_list<Tree*> const& es) {
    Tree* res = nil;
    for (auto it = std::rbegin(es); it != std::rend(es); it++) res = cons(*it, res);
    return res;
  }

  // Initialize with built-in patterns, rules, forms and procedures
  // See: https://github.com/digama0/mm0/blob/master/mm0-hs/mm1.md#syntax-forms
  // See: https://github.com/digama0/mm0/blob/master/mm0-hs/mm1.md#Prim-functions
  Evaluator::Evaluator():
    pool(),
    nil(pool.emplace(Nil{})),
    unit(pool.emplace(Unit{})),
    patterns(nil),
    rules(nil),
    symbolNames(),
    nameSymbols(),
    patternNames(),
    ruleNames(),
    lexer(),
    parser(lexer),
    globalEnv(nil),
    macros(),
    nameMacros(),
    prims(),
    namePrims() {

    // Commonly used constants
    // auto const& nzero  = pool.emplace(Nat64{ 0 });
    // auto const& sempty = pool.emplace(String{ "" });
    auto const& btrue = pool.emplace(Bool{true});
    auto const& bfalse = pool.emplace(Bool{false});

    // =========================
    // Default syntax and macros
    // =========================

#define symbol(s)               list(sym(s), nat(0))
#define pattern(name, lhs, pat) list(sym(name), lhs, pat)
#define rule(name, lhs, rhs)    list(sym(name), lhs, rhs)
#define empty                   list(sym("empty"))
#define any                     list(sym("any"))
#define utf8seg                 list(sym("utf8seg"))
#define chars(s)                list(sym("char"), str(s))
#define except(s)               list(sym("except"), str(s))
#define range(l, u)             list(sym("range"), nat(l), nat(u))
#define word(s)                 list(sym("word"), str(s))
#define alt(...)                list(sym("alt"), __VA_ARGS__)
#define concat(...)             list(sym("concat"), __VA_ARGS__)
#define opt(pat)                list(sym("opt"), pat)
#define star(pat)               list(sym("star"), pat)
#define plus(pat)               list(sym("plus"), pat)

    Tree* defaultPatterns = list(
      pattern("_", symbol("_"), star(chars(" \f\n\r\t\v"))),               // Blank
      pattern("_", symbol("_"), concat(word("//"), star(except("\n\r")))), // Line comment
      pattern(                                                             // Block comment
        "_",
        symbol("_"),
        concat(
          word("/*"),
          star(concat(star(except("*")), plus(chars("*")), except("/"))),
          star(except("*")),
          plus(chars("*")),
          chars("/")
        )
      ),
      pattern(
        "symbol'",
        symbol("tree"),
        concat(
          alt(range('a', 'z'), range('A', 'Z'), chars("_'"), utf8seg),
          star(alt(range('a', 'z'), range('A', 'Z'), range('0', '9'), chars("_'"), utf8seg))
        )
      ),
      pattern(
        "nat64'",
        symbol("tree"),
        alt(
          plus(range('0', '9')),
          concat(chars("0"), chars("xX"), plus(alt(range('0', '9'), range('a', 'f'), range('A', 'F'))))
        )
      ),
      pattern(
        "string'",
        symbol("tree"),
        concat(chars("\""), star(alt(except("\\\""), concat(chars("\\"), chars("\\\"abfnrtv")))), chars("\""))
      ),
      pattern("_", symbol("left_paren"), word("(")),
      pattern("_", symbol("right_paren"), word(")")),
      pattern("_", symbol("period"), word(".")),
      pattern("_", symbol("quote"), word("`")),
      pattern("_", symbol("comma"), word(","))
    );

    Tree* defaultRules = list(
      rule("nil'", symbol("list"), list()),
      rule("cons'", symbol("list"), list(symbol("tree"), symbol("list"))),
      rule("period'", symbol("list"), list(symbol("tree"), symbol("period"), symbol("tree"))),
      rule("quote'", symbol("tree"), list(symbol("quote"), symbol("tree"))),
      rule("unquote'", symbol("tree"), list(symbol("comma"), symbol("tree"))),
      rule("tree'", symbol("tree"), list(symbol("left_paren"), symbol("list"), symbol("right_paren"))),
      rule("id'", symbol("_"), list(symbol("tree")))
    );

    setSyntax(defaultPatterns, defaultRules);
    addMacro("symbol'", Closure{globalEnv, list(sym("s")), list(list(sym("string_symbol"), sym("s")))});
    addMacro("nat64'", Closure{globalEnv, list(sym("n")), list(list(sym("string_nat64"), sym("n")))});
    addMacro(
      "string'",
      Closure{
        globalEnv,
        list(sym("s")),
        list(list(
          sym("string_unescape"),
          list(sym("string_substr"), sym("s"), nat(1), list(sym("sub"), list(sym("string_length"), sym("s")), nat(2)))
        ))}
    );
    addMacro("nil'", Closure{globalEnv, list(), list(list(sym("nil")))});
    addMacro("cons'", Closure{globalEnv, list(sym("l"), sym("r")), list(list(sym("cons"), sym("l"), sym("r")))});
    addMacro("id'", Closure{globalEnv, list(sym("l")), list(sym("l"))});
    addMacro(
      "period'",
      Closure{globalEnv, list(sym("l"), sym("_"), sym("r")), list(list(sym("cons"), sym("l"), sym("r")))}
    );
    addMacro(
      "quote'",
      Closure{
        globalEnv,
        list(sym("_"), sym("l")),
        list(list(sym("list"), list(sym("string_symbol"), str("quote")), sym("l")))}
    );
    addMacro(
      "unquote'",
      Closure{
        globalEnv,
        list(sym("_"), sym("l")),
        list(list(sym("list"), list(sym("string_symbol"), str("unquote")), sym("l")))}
    );
    addMacro("tree'", Closure{globalEnv, list(sym("_"), sym("l"), sym("_")), list(sym("l"))});

#undef syncat
#undef pattern
#undef rule
#undef empty
#undef any
#undef utf8seg
#undef chars
#undef except
#undef range
#undef word
#undef alt
#undef concat
#undef opt
#undef star
#undef plus
#undef list

    // ===============
    // Primitive forms
    // ===============

    // [√] Introduction rule for `Closure`
    addPrimitive("lambda", false, [this](Tree* env, Tree* e) -> Result {
      auto const& [formal, es] = expect<Cons>(e);
      return pool.emplace(Closure{env, formal, es});
    });

    // [√] Elimination rule for `Bool`
    addPrimitive("cond", false, [this](Tree* env, Tree* e) -> Result {
      auto const& [test, t] = expect<Cons>(e);
      auto const& [iftrue, u] = expect<Cons>(t);
      auto const& iffalse = (get_if<Cons>(u) ? get_if<Cons>(u)->head : unit);
      bool result = expect<Bool>(eval(env, test)).val;
      return {env, result ? iftrue : iffalse};
    });

    // [√] Introduction rule for sealed `Tree`
    addPrimitive("quote", false, [this](Tree* env, Tree* e) -> Result {
      return quasiquote(env, expect<Cons>(e).head);
    });
    addPrimitive("unquote", false, [this](Tree* env, Tree* e) -> Result { return eval(env, expect<Cons>(e).head); });

    // [√] Elimination rule for sealed `Tree`
    addPrimitive("match", false, [this](Tree* env, Tree* e) -> Result {
      auto const& [head, t] = expect<Cons>(e);
      auto const& [clauses, _] = expect<Cons>(t);
      auto const& target = eval(env, head);
      for (auto it = get_if<Cons>(clauses); it; it = get_if<Cons>(it->tail)) {
        auto const& [pat, u] = expect<Cons>(it->head);
        Tree* newEnv = env;
        if (match(target, pat, newEnv)) {
          auto const& [expr, _] = expect<Cons>(u);
          return {newEnv, expr};
        }
      }
      // All failed, throw exception
      string msg = "nonexhaustive patterns: { ";
      bool first = true;
      for (auto it = get_if<Cons>(clauses); it; it = get_if<Cons>(it->tail)) {
        auto const& [pat, _] = expect<Cons>(it->head);
        if (!first) msg += ", ";
        first = false;
        msg += pat->toString();
      }
      msg += " } ?= " + target->toString();
      throw PartialEvalError(msg, clauses);
    });

    // [√] Currently we don't have a `let`, and this is just a synonym of `let*`...
    addPrimitive("let", false, [this](Tree* env, Tree* e) -> Result {
      auto const& [defs, es] = expect<Cons>(e);
      for (auto it = get_if<Cons>(defs); it; it = get_if<Cons>(it->tail)) {
        auto const& [lhs, t] = expect<Cons>(it->head);
        auto const& [rhs, _] = expect<Cons>(t);
        env = extend(env, expect<Symbol>(lhs).val, eval(env, rhs));
      }
      return {env, beginList(env, es)};
    });

    // [√] Currently we don't have a `letrec`, and this is just a synonym of `letrec*`...
    addPrimitive("letrec", false, [this](Tree* env, Tree* e) -> Result {
      auto const& [defs, es] = expect<Cons>(e);
      // Add #unit (placeholder) bindings
      vector<Tree**> refs;
      for (auto it = get_if<Cons>(defs); it; it = get_if<Cons>(it->tail)) {
        auto const& [lhs, _] = expect<Cons>(it->head);
        env = extend(env, expect<Symbol>(lhs).val, unit);
        // Will always succeed due to the recent `extend`.
        refs.push_back(&get_if<Cons>(get_if<Cons>(get_if<Cons>(env)->head)->tail)->head);
      }
      // Sequentially evaluate and assign
      size_t i = 0;
      for (auto it = get_if<Cons>(defs); it; it = get_if<Cons>(it->tail)) {
        auto const& [lhs, t] = expect<Cons>(it->head);
        auto const& [rhs, _] = expect<Cons>(t);
        *refs[i++] = eval(env, rhs);
      }
      return {env, beginList(env, es)};
    });

    // [√] Global definitions will become effective only on the curr statement...
    // For local definitions, use `letrec*`.
    addPrimitive("define", false, [this](Tree* env, Tree* e) -> Result {
      auto const& [lhs, t] = expect<Cons>(e);
      auto const& [rhs, _] = expect<Cons>(t);
      globalEnv = extend(globalEnv, expect<Symbol>(lhs).val, eval(env, rhs));
      return unit;
    });

    // [?]
    addPrimitive("define_macro", false, [this](Tree* env, Tree* e) -> Result {
      auto const& [lhs, t] = expect<Cons>(e);
      auto const& [rhs, _] = expect<Cons>(t);
      addMacro(expect<Symbol>(lhs).val, expect<Closure>(eval(env, rhs)));
      return unit;
    });

    // [√] This modifies nodes reachable through `env` (which prevents making `Tree*` into const pointers)
    // Ignores more arguments
    addPrimitive("set", false, [this](Tree* env, Tree* e) -> Result {
      auto const& [lhs, t] = expect<Cons>(e);
      auto const& [rhs, _] = expect<Cons>(t);
      auto const& val = eval(env, rhs);
      string s = expect<Symbol>(lhs).val;
      for (auto it = get_if<Cons>(env); it; it = get_if<Cons>(it->tail)) {
        if (auto const& curr = get_if<Cons>(it->head)) {
          auto& lhs = curr->head;
          if (auto const& t = get_if<Cons>(curr->tail)) {
            auto& rhs = t->head;
            if (auto const& sym = get_if<Symbol>(lhs); sym && sym->val == s) {
              rhs = val;
              return unit;
            }
          }
        }
      }
      // Not found, throw exception
      throw PartialEvalError("unbound symbol \"" + s + "\"", lhs);
    });

    // [√]
    addPrimitive("begin", false, [this](Tree* env, Tree* e) -> Result { return {env, beginList(env, e)}; });

    // ====================
    // Primitive procedures
    // ====================

    // [√]
    addPrimitive("eval", true, [](Tree* env, Tree* e) -> Result {
      auto const& [h, t] = expect<Cons>(e);
      if (auto const& tcons = get_if<Cons>(t)) env = tcons->head;
      return {env, h}; // Let it restart and evaluate
    });
    addPrimitive("env", true, [](Tree* env, Tree*) -> Result { return env; });
    addPrimitive("get_syntax", true, [this](Tree*, Tree*) -> Result { return cons(patterns, cons(rules, nil)); });
    addPrimitive("set_syntax", true, [this](Tree*, Tree* e) -> Result {
      auto const& [p, t] = expect<Cons>(e);
      auto const& [r, _] = expect<Cons>(t);
      setSyntax(p, r);
      return unit;
    });
    addPrimitive("get_global_env", true, [this](Tree*, Tree*) -> Result { return globalEnv; });
    addPrimitive("set_global_env", true, [this](Tree*, Tree* e) -> Result {
      globalEnv = expect<Cons>(e).head;
      return unit;
    });

    // [√] In principle these can be implemented using patterns and `quote`s,
    // but making them into primitives will make things run faster.
    addPrimitive("nil", true, [this](Tree*, Tree*) -> Result { return nil; });
    addPrimitive("cons", true, [this](Tree*, Tree* e) -> Result {
      auto const& [lhs, t] = expect<Cons>(e);
      auto const& [rhs, _] = expect<Cons>(t);
      return cons(lhs, rhs);
    });
    addPrimitive("list", true, [](Tree*, Tree* e) -> Result { return e; });
    addPrimitive("id", true, [](Tree*, Tree* e) -> Result { return expect<Cons>(e).head; });

    // [√] String functions
    addPrimitive("string_symbol", true, [this](Tree*, Tree* e) -> Result {
      return sym(expect<String>(expect<Cons>(e).head).val);
    });
    addPrimitive("string_nat64", true, [this](Tree*, Tree* e) -> Result {
      return nat(std::stoull(expect<String>(expect<Cons>(e).head).val, nullptr, 0));
    });
    addPrimitive("string_escape", true, [this](Tree*, Tree* e) -> Result {
      return str(Tree::escapeString(expect<String>(expect<Cons>(e).head).val));
    });
    addPrimitive("string_unescape", true, [this](Tree*, Tree* e) -> Result {
      return str(Tree::unescapeString(expect<String>(expect<Cons>(e).head).val));
    });
    addPrimitive("string_length", true, [this](Tree*, Tree* e) -> Result {
      return nat(expect<String>(expect<Cons>(e).head).val.size());
    });
    addPrimitive("string_char", true, [this](Tree*, Tree* e) -> Result {
      auto const& [lhs, t] = expect<Cons>(e);
      auto const& [rhs, _] = expect<Cons>(t);
      auto const& sv = expect<String>(lhs).val;
      size_t iv = expect<Nat64>(rhs).val;
      if (iv >= sv.size()) throw PartialEvalError("Index " + std::to_string(iv) + " out of range", rhs);
      return nat(static_cast<unsigned char>(sv[iv]));
    });
    addPrimitive("char_string", true, [this](Tree*, Tree* e) -> Result {
      auto const& [chr, _] = expect<Cons>(e);
      uint64_t cv = expect<Nat64>(chr).val;
      if (cv >= 256) throw PartialEvalError("Character code " + std::to_string(cv) + " out of range", chr);
      return str(string(1, static_cast<char>(cv)));
    });
    addPrimitive("string_concat", true, [this](Tree*, Tree* e) -> Result {
      auto const& [lhs, t] = expect<Cons>(e);
      auto const& [rhs, _] = expect<Cons>(t);
      return str(expect<String>(lhs).val + expect<String>(rhs).val);
    });
    addPrimitive("string_substr", true, [this](Tree*, Tree* e) -> Result {
      auto const& [s, t] = expect<Cons>(e);
      auto const& [pos, u] = expect<Cons>(t);
      auto const& [len, _] = expect<Cons>(u);
      auto const& sv = expect<String>(s).val;
      size_t posv = expect<Nat64>(pos).val;
      if (posv > sv.size()) posv = sv.size();
      return str(sv.substr(posv, expect<Nat64>(len).val));
    });
    addPrimitive("string_eq", true, [this](Tree*, Tree* e) -> Result {
      auto const& [lhs, t] = expect<Cons>(e);
      auto const& [rhs, _] = expect<Cons>(t);
      return boolean(expect<String>(lhs).val == expect<String>(rhs).val);
    });

// [√]
#define unary(T, name, op)                                    \
  addPrimitive(name, true, [this](Tree*, Tree* e) -> Result { \
    auto const& [lhs, _] = expect<Cons>(e);                   \
    return pool.emplace(T{op(expect<T>(lhs).val)});       \
  })
#define binary(T, name, op)                                               \
  addPrimitive(name, true, [this](Tree*, Tree* e) -> Result {             \
    auto const& [lhs, t] = expect<Cons>(e);                               \
    auto const& [rhs, _] = expect<Cons>(t);                               \
    return pool.emplace(T{expect<T>(lhs).val op expect<T>(rhs).val}); \
  })
#define binpred(T, name, op)                                            \
  addPrimitive(name, true, [btrue, bfalse](Tree*, Tree* e) -> Result {  \
    auto const& [lhs, t] = expect<Cons>(e);                             \
    auto const& [rhs, _] = expect<Cons>(t);                             \
    return (expect<T>(lhs).val op expect<T>(rhs).val) ? btrue : bfalse; \
  })

    unary(Nat64, "minus", -);
    binary(Nat64, "add", +);
    binary(Nat64, "sub", -);
    binary(Nat64, "mul", *);
    binary(Nat64, "div", /);
    binary(Nat64, "mod", %);
    binpred(Nat64, "le", <=);
    binpred(Nat64, "lt", <);
    binpred(Nat64, "ge", >=);
    binpred(Nat64, "gt", >);
    binpred(Nat64, "eq", ==);
    binpred(Nat64, "neq", !=);
    unary(Bool, "not", !);
    binary(Bool, "and", &&);
    binary(Bool, "or", ||);
    binary(Bool, "implies", <=);
    binary(Bool, "iff", ==);

#undef unary
#undef binary
#undef binpred

    // [√] For debugging?
    addPrimitive("print", true, [this](Tree*, Tree* e) -> Result {
      return pool.emplace(String{expect<Cons>(e).head->toString()});
    });

    // [?] TODO: output to ostream
    addPrimitive("display", true, [this](Tree*, Tree* e) -> Result {
      auto const& [head, tail] = expect<Cons>(e);
      std::cout << expect<String>(head).val << std::endl;
      return unit;
    });
    addPrimitive("debug_save_file", true, [this](Tree*, Tree* e) -> Result {
      auto const& [lhs, t] = expect<Cons>(e);
      auto const& [rhs, _] = expect<Cons>(t);
      std::ofstream out(expect<String>(lhs).val);
      if (!out.is_open()) throw PartialEvalError("Could not open file", lhs);
      out << expect<String>(rhs).val << std::endl;
      return unit;
    });
  }

  // Evaluator entries are stored as lists of two elements.
  Tree* Evaluator::extend(Tree* env, std::string const& s, Tree* e) { return cons(cons(sym(s), cons(e, nil)), env); }

  Tree* Evaluator::lookup(Tree* env, std::string const& s) {
    for (auto it = get_if<Cons>(env); it; it = get_if<Cons>(it->tail)) {
      if (auto const& curr = get_if<Cons>(it->head)) {
        auto const& lhs = curr->head;
        if (auto const& t = get_if<Cons>(curr->tail)) {
          auto const& rhs = t->head;
          if (auto const& sym = get_if<Symbol>(lhs); sym && sym->val == s) return get_if<Unit>(rhs) ? nullptr : rhs;
        }
      }
    }
    return nullptr;
  }

  bool operator==(EarleyParser::Location const& a, EarleyParser::Location const& b) {
    return a.pos == b.pos && a.index == b.index;
  }

  vector<Tree*> Evaluator::resolve(EarleyParser::Location loc, vector<Tree*> const& right, size_t maxDepth) {
    if (maxDepth == 0) return {};
    auto const& [state, links] = parser.forest()[loc.pos][loc.index];
    vector<Tree*> res;

    if (state.progress == 0) {
      // Whole rule completed
      for (Tree* r: right) res.push_back(cons(sym(ruleNames[state.rule]), r));
      return res;
    }

    // One step to left
    for (auto const& [prevLink, childLink]: links) {
      vector<Tree*> child;
      if (childLink == EarleyParser::Leaf) {
        auto const& tok = parser.sentence()[loc.pos - 1];
        child = {cons(sym(patternNames[tok.pattern]), cons(str(tok.lexeme), nil))};
      } else {
        child = resolve(childLink, {nil}, maxDepth - 1);
      }
      vector<Tree*> curr;
      for (Tree* c: child)
        for (Tree* r: right) curr.push_back(cons(c, r));
      vector<Tree*> final = resolve(prevLink, curr, maxDepth);
      for (Tree* f: final) res.push_back(f);
    }

    return res;
  }

  Tree* Evaluator::resolve(size_t maxDepth) {
    auto const& pos = parser.sentence().size();
    auto const& forest = parser.forest();
    if (pos >= forest.size()) unreachable;
    // Mid: pos < forest.size()

    vector<Tree*> all;
    for (size_t i = 0; i < forest[pos].size(); i++) {
      auto const& [state, links] = forest[pos][i];
      auto const& [lhs, rhs] = parser.rule(state.rule);
      if (state.startPos == 0 && lhs.first == StartSymbol && state.progress == rhs.size()) {
        vector<Tree*> final = resolve({pos, i}, {nil}, maxDepth);
        for (Tree* f: final) all.push_back(f);
      }
    }

    if (all.empty()) {
      // Failed to resolve (possibly due to excessive depth or infinite expansion)
      unimplemented;
    }
    if (all.size() > 1) {
      // Ambiguous
      for (auto const& parse: all) std::cout << parse->toString() << std::endl;
      unimplemented;
    }
    // Mid: all.size() == 1
    return all[0];
  }

#undef cons
#undef nil
#undef sym
#undef str
#undef nat
#undef boolean
#undef unit

  // TODO: custom expansion order
  Tree* Evaluator::expand(Tree* e) {
    if (get_if<Cons>(e)) {
      // Non-empty lists: expand all macros, from inside out
      try {
        e = expandList(e);
        auto const& [head, tail] = expect<Cons>(e);
        if (auto const& sym = get_if<Symbol>(head)) {
          auto const& it = nameMacros.find(sym->val);
          if (it != nameMacros.end()) {
            auto const& cl = macros[it->second];
            auto env = cl.env;
            if (!match(tail, cl.formal, env))
              throw EvalError("pattern matching failed: " + cl.formal->toString() + " ?= " + tail->toString(), tail, e);
            return eval(env, beginList(env, cl.es));
          }
        }
        return e;

      } catch (PartialEvalError& ex) {
        // "Decorate" partial exceptions with more context, and rethrow a (complete) exception
        throw EvalError(ex.what(), ex.at, e);
      }

    } else {
      // Everything else: expands to itself
      return e;
    }
  }

  // Expands elements in a list
  Tree* Evaluator::expandList(Tree* e) {
    if (get_if<Nil>(e)) return e;
    else if (auto const& econs = get_if<Cons>(e)) {
      auto const& [head, tail] = *econs;
      auto const& ehead = expand(head);
      auto const& etail = expandList(tail);
      return (ehead == head && etail == tail) ? e : pool.emplace(ehead, etail);
    }
    return expand(e);
  }

  Tree* Evaluator::eval(Tree* env, Tree* e) {
    while (true) {
      // Evaluate current `e` under current `env`

      if (auto const& sym = get_if<Symbol>(e)) {
        // Symbols: evaluate to their bound values
        if (auto const& val = lookup(env, sym->val)) return val;
        if (auto const& it = namePrims.find(sym->val); it != namePrims.end()) return pool.emplace(Prim{it->second});
        throw PartialEvalError("unbound symbol \"" + sym->val + "\"", e);

      } else if (auto const& econs = get_if<Cons>(e)) {
        // Non-empty lists: evaluate as function application
        try {
          auto const& [head, tail] = *econs;
          auto const& ehead = eval(env, head);

          if (auto const& prim = get_if<Prim>(ehead)) {
            // Primitive function application
            auto const& [evalParams, f] = prims[prim->id];
            auto const& res = f(env, evalParams ? evalList(env, tail) : tail);
            // No tail call, return
            if (!res.env) return res.e;
            // Tail call
            env = res.env;
            e = res.e;
            continue;
          }

          if (auto const& cl = get_if<Closure>(ehead)) {
            // Lambda function application
            auto const& params = evalList(env, tail);
            // Evaluate body as tail call
            env = cl->env;
            if (!match(params, cl->formal, env))
              throw EvalError(
                "pattern matching failed: " + cl->formal->toString() + " ?= " + params->toString(),
                tail,
                e
              );
            e = beginList(env, cl->es);
            continue;
          }

          throw EvalError("head element " + ehead->toString() + " is not a function", head, e);

        } catch (PartialEvalError& ex) {
          // "Decorate" partial exceptions with more context, and rethrow a (complete) exception
          throw EvalError(ex.what(), ex.at, e);
        }

      } else {
        // Everything else: evaluates to itself
        return e;
      }
    }
  }

  // Evaluates elements in a list (often used as parameters)
  Tree* Evaluator::evalList(Tree* env, Tree* e) {
    if (get_if<Nil>(e)) return e;
    else if (auto const& econs = get_if<Cons>(e)) {
      auto const& [head, tail] = *econs;
      auto const& ehead = eval(env, head);
      auto const& etail = evalList(env, tail);
      return (ehead == head && etail == tail) ? e : pool.emplace(ehead, etail);
    }
    return eval(env, e);
  }

  // Executes elements in a list, except the last one (for tail call optimization)
  // Returns the last one unevaluated, or `#unit` if list is empty
  Tree* Evaluator::beginList(Tree* env, Tree* e) {
    for (auto it = get_if<Cons>(e); it; it = get_if<Cons>(it->tail)) {
      auto const& [head, tail] = *it;
      if (!get_if<Cons>(tail)) {
        expect<Nil>(tail);
        return head;
      }
      eval(env, head);
    }
    expect<Nil>(e);
    return unit;
  }

  // Evaluates a quasiquoted list
  Tree* Evaluator::quasiquote(Tree* env, Tree* e) {
    if (auto const& econs = get_if<Cons>(e)) {
      auto const& [head, tail] = *econs;
      if (*head == Tree(Symbol{"unquote"})) return eval(env, expect<Cons>(tail).head);
      auto const& ehead = quasiquote(env, head);
      auto const& etail = quasiquote(env, tail);
      return (ehead == head && etail == tail) ? e : pool.emplace(ehead, etail);
    }
    return e;
  }
}
