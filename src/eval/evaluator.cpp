#include "evaluator.hpp"

#include <iostream>
using std::cin, std::cout, std::endl;

namespace Eval {

  using std::string;
  using std::vector;
  using std::pair;
  using std::optional;
  using Core::Expr, Core::Allocator;
  using Parsing::Token, Parsing::NFALexer, Parsing::EarleyParser;


  Tree* Evaluator::evalNextStatement() {
    if (!parser.nextSentence()) return nullptr;
    // cout << parser.showStates(syncatNames) << endl;
    auto e = resolve(); // cout << e->toString() << endl;
    e = expand(e); cout << e->toString() << endl;
    e = eval(globalEnv, e);
    return e;
  }

  vector<ParsingError> Evaluator::popParsingErrors() {
    vector<ParsingError> res;
    // See: https://stackoverflow.com/questions/30448182/is-it-safe-to-use-a-c11-range-based-for-loop-with-an-rvalue-range-init
    for (const auto& e: lexer.popErrors()) {
      res.emplace_back("Parsing error, unexpected characters: " + e.lexeme, e.startPos, e.endPos);
    }
    for (const auto& e: parser.popErrors()) {
      string s = "Parsing error, expected one of:\n";
      bool first = true;
      for (Parsing::Symbol sym: e.expected) {
        string name = syncatNames[sym];
        if (name.empty()) name = "(" + std::to_string(sym) + ")";
        s += (first? "" : ", ") + name;
        first = false;
      }
      s += "\n";
      if (e.got) {
        string name = syncatNames[*e.got];
        if (name.empty()) name = "(" + std::to_string(*e.got) + ")";
        s += "got token " + name;
      } else {
        s += "but reached the end of file";
      }
      res.emplace_back(s, e.startPos, e.endPos);
    }
    return res;
  }

  // Throw this when you don't know about the parent expression,
  // or when you want the parent call to `eval()` to provide context.
  struct PartialEvalError: public EvalError {
    PartialEvalError(const std::string& s, const Tree* at): EvalError(s, at, at) {}
  };

  // A shortcut for `std::holds_alternative<T>(e->v)`
  template <typename T>
  bool holds(Tree* e) noexcept { return std::holds_alternative<T>(e->v); }

  // A shortcut for `std::get<T>(e->v)` (terminates on failure)
  template <typename T>
  T& get(Tree* e) noexcept { return std::get<T>(e->v); }

  // Convenient pattern-matching functions (throw customized exceptions on failure)
  template <typename T>
  T& expect(Tree*) { unreachable; }
  #define declareExpect(T, msg) \
    template <> \
    T& expect<T>(Tree* e) { \
      try { return std::get<T>(e->v); } \
      catch (std::bad_variant_access&) { throw PartialEvalError((msg ", got ") + e->toString(), e); } \
    }
  declareExpect(Nil,     "expected end-of-list")
  declareExpect(Cons,    "expected non-empty list")
  declareExpect(Symbol,  "expected symbol")
  declareExpect(Nat64,   "expected number")
  declareExpect(String,  "expected string")
  declareExpect(Bool,    "expected boolean")
  declareExpect(Closure, "expected function")
  declareExpect(Native,  "expected native type")
  #undef declareExpect

  template <typename T>
  T expectNative(Tree* e) {
    try { return std::any_cast<T>(expect<Native>(e).val); }
    catch (std::bad_any_cast& ex) { throw PartialEvalError(string("native type mismatch: ") + ex.what(), e); }
  }

  // Match an Tree against another Tree (pattern)
  // See: https://github.com/digama0/mm0/blob/master/mm0-hs/mm1.md#syntax-forms
  // (Continuation, `__k`, `and`, `or`, `not` and `pred?` patterns are not implemented)
  bool Evaluator::match(Tree* e, Tree* pat, Tree*& env, bool quoteMode) {
    if (holds<Symbol>(pat) && !quoteMode) {
      string sym = get<Symbol>(pat).val;
      if (sym != "_") env = extend(env, sym, e);
      return true;
    }
    if (holds<Cons>(pat)) {
      const auto& [h, t] = get<Cons>(pat);
      if (holds<Symbol>(h)) {
        string sym = get<Symbol>(h).val;
        if (sym == "quote" && !quoteMode) return match(e, expect<Cons>(t).head, env, true); // Enter quote mode
        if (sym == "unquote" && quoteMode) return match(e, expect<Cons>(t).head, env, false); // Leave quote mode
        if (sym == "...") return holds<Nil>(e) || holds<Cons>(e);
      }
      return holds<Cons>(e) &&
        match(get<Cons>(e).head, h, env, quoteMode) &&
        match(get<Cons>(e).tail, t, env, quoteMode);
    }
    return *e == *pat;
  }

  vector<char8_t> stringToCharVec(const string& s) {
    vector<char8_t> res;
    for (char c: s) res.push_back(static_cast<char8_t>(c));
    return res;
  }

  NFALexer::NFA Evaluator::treePattern(Tree* e) {
    const auto& [tag, t] = expect<Cons>(e);
    string stag = expect<Symbol>(tag).val;
    if (stag == "empty")   return lexer.empty();
    if (stag == "any")     return lexer.any();
    if (stag == "utf8seg") return lexer.utf8segment();
    if (stag == "char")    return lexer.charsvec(stringToCharVec(expect<String>(expect<Cons>(t).head).val));
    if (stag == "except")  return lexer.exceptvec(stringToCharVec(expect<String>(expect<Cons>(t).head).val));
    if (stag == "range") {
      const auto& [lbound, u] = expect<Cons>(t);
      const auto& [ubound, _] = expect<Cons>(u);
      return lexer.range(
        static_cast<char8_t>(expect<Nat64>(lbound).val),
        static_cast<char8_t>(expect<Nat64>(ubound).val)
      );
    }
    if (stag == "word")    return lexer.word(stringToCharVec(expect<String>(expect<Cons>(t).head).val));
    if (stag == "alt")     return lexer.altvec(listPatterns(t));
    if (stag == "concat")  return lexer.concatvec(listPatterns(t));
    if (stag == "opt")     return lexer.opt(treePattern(expect<Cons>(t).head));
    if (stag == "star")    return lexer.star(treePattern(expect<Cons>(t).head));
    if (stag == "plus")    return lexer.plus(treePattern(expect<Cons>(t).head));
    notimplemented;
  }

  vector<NFALexer::NFA> Evaluator::listPatterns(Tree* e) {
    vector<NFALexer::NFA> res;
    for (auto it = e; holds<Cons>(it); it = get<Cons>(it).tail) {
      res.push_back(treePattern(get<Cons>(it).head));
    }
    return res;
  }

  vector<pair<Parsing::Symbol, Parsing::Prec>> Evaluator::listSyncats(Tree* e) {
    vector<pair<Parsing::Symbol, Parsing::Prec>> res;
    for (auto it = e; holds<Cons>(it); it = get<Cons>(it).tail) {
      const auto& [sym, t] = expect<Cons>(get<Cons>(it).head);
      const auto& [pre, _] = expect<Cons>(t);
      res.emplace_back(getSyncat(expect<Symbol>(sym).val), expect<Nat64>(pre).val);
    }
    return res;
  }

  // TODO: handle exceptions, refactor
  void Evaluator::setSyntax(Tree* p, Tree* r) {
    syncatNames.clear();
    nameSyncats.clear();
    patternNames.clear();
    ruleNames.clear();
    lexer.clearPatterns();
    parser.clearPatterns();
    parser.clearRules();

    patterns = p;
    rules = r;

    // Add ignored and starting syncats
    syncatNames.push_back("_"); parser.setIgnoredSymbol(IgnoredSyncat);
    syncatNames.push_back("_"); parser.setStartSymbol(StartSyncat);

    // Add patterns
    for (auto it = patterns; holds<Cons>(it); it = get<Cons>(it).tail) {
      const auto& [name, t] = expect<Cons>(get<Cons>(it).head);
      const auto& [lhs, u]  = expect<Cons>(t);
      const auto& [rhs, _1] = expect<Cons>(u);
      const auto& [sym, v]  = expect<Cons>(lhs);
      const auto& [pre, _2] = expect<Cons>(v);
      string sname = expect<Symbol>(sym).val;
      size_t sid = (sname == "_")? IgnoredSyncat : getSyncat(sname);
      size_t pid = patternNames.size();
      patternNames.push_back(expect<Symbol>(name).val);
      if (lexer.addPattern(treePattern(rhs)) != pid) unreachable;
      if (parser.addPattern(sid, expect<Nat64>(pre).val) != pid) unreachable;
    }

    // Add rules
    for (auto it = rules; holds<Cons>(it); it = get<Cons>(it).tail) {
      const auto& [name, t] = expect<Cons>(get<Cons>(it).head);
      const auto& [lhs, u]  = expect<Cons>(t);
      const auto& [rhs, _1] = expect<Cons>(u);
      const auto& [sym, v]  = expect<Cons>(lhs);
      const auto& [pre, _2] = expect<Cons>(v);
      string sname = expect<Symbol>(sym).val;
      size_t sid = (sname == "_")? StartSyncat : getSyncat(sname);
      size_t rid = ruleNames.size();
      ruleNames.push_back(expect<Symbol>(name).val);
      if (parser.addRule(sid, expect<Nat64>(pre).val, listSyncats(rhs)) != rid) unreachable;
    }
  }

  #define cons      pool.emplaceBack
  #define nil       nil
  #define sym(s)    pool.emplaceBack(Symbol{ s })
  #define str(s)    pool.emplaceBack(String{ s })
  #define nat(n)    pool.emplaceBack(Nat64{ n })
  #define unit      unit
  #define list(...) makeList({ __VA_ARGS__ })

  Tree* Evaluator::makeList(const std::initializer_list<Tree*>& es) {
    Tree* res = nil;
    for (auto it = std::rbegin(es); it != std::rend(es); it++) res = cons(*it, res);
    return res;
  }

  // Initialize with built-in patterns, rules, forms and procedures
  // See: https://github.com/digama0/mm0/blob/master/mm0-hs/mm1.md#syntax-forms
  // See: https://github.com/digama0/mm0/blob/master/mm0-hs/mm1.md#Prim-functions
  Evaluator::Evaluator():
      pool(), nil(pool.emplaceBack(Nil{})), unit(pool.emplaceBack(Unit{})),
      patterns(nil), rules(nil), syncatNames(), nameSyncats(),
      patternNames(), ruleNames(), lexer(), parser(lexer),
      globalEnv(nil), macros(), nameMacros(), prims(), namePrims() {
    // BEGIN FLATBREAD CODE (开始摊大饼)

    // Commonly used constants
    // const auto nzero  = pool.emplaceBack(Nat64{ 0 });
    // const auto sempty = pool.emplaceBack(String{ "" });
    const auto btrue  = pool.emplaceBack(Bool{ true });
    const auto bfalse = pool.emplaceBack(Bool{ false });

    // ==============
    // Default syntax
    // ==============

    #define syncat(s) list(sym(s), nat(0))
    #define pattern(name, lhs, pat) list(sym(name), lhs, pat)
    #define rule(name, lhs, rhs) list(sym(name), lhs, rhs)
    #define empty       list(sym("empty"))
    #define any         list(sym("any"))
    #define utf8seg     list(sym("utf8seg"))
    #define chars(s)    list(sym("char"), str(s))
    #define except(s)   list(sym("except"), str(s))
    #define range(l, u) list(sym("range"), nat(l), nat(u))
    #define word(s)     list(sym("word"), str(s))
    #define alt(...)    list(sym("alt"), __VA_ARGS__)
    #define concat(...) list(sym("concat"), __VA_ARGS__)
    #define opt(pat)    list(sym("opt"), pat)
    #define star(pat)   list(sym("star"), pat)
    #define plus(pat)   list(sym("plus"), pat)

    Tree* defaultPatterns = list(
      pattern("_", syncat("_"), star(chars(" \f\n\r\t\v"))),                // Blank
      pattern("_", syncat("_"), concat(word("//"), star(except("\n\r")))),  // Line comment
      pattern("_", syncat("_"),                                             // Block comment
        concat(word("/*"),
               star(concat(star(except("*")),
                           plus(chars("*")),
                           except("/"))),
               star(except("*")),
               plus(chars("*")),
               chars("/"))),
      pattern("symbol'", syncat("tree"),
        concat(alt(range('a', 'z'), range('A', 'Z'), chars("_'"), utf8seg),
               star(alt(range('a', 'z'), range('A', 'Z'), range('0', '9'), chars("_'"), utf8seg)))),
      pattern("nat64'", syncat("tree"),
        alt(plus(range('0', '9')),
            concat(chars("0"), chars("xX"), plus(alt(range('0', '9'), range('a', 'f'), range('A', 'F')))))),
      pattern("string'", syncat("tree"),
        concat(chars("\""),
               star(alt(except("\\\""),
                        concat(chars("\\"), chars("\\\"abfnrtv")))),
               chars("\""))),
      pattern("_", syncat("left_paren"), word("(")),
      pattern("_", syncat("right_paren"), word(")")),
      pattern("_", syncat("period"), word("."))
    );

    Tree* defaultRules = list(
      rule("nil'",    syncat("list"), list()),
      rule("cons'",   syncat("list"), list(syncat("tree"), syncat("list"))),
      rule("period'", syncat("list"), list(syncat("tree"), syncat("period"), syncat("tree"))),
      rule("tree'",   syncat("tree"), list(syncat("left_paren"), syncat("list"), syncat("right_paren"))),
      rule("id'",     syncat("_"),    list(syncat("tree")))
    );

    setSyntax(defaultPatterns, defaultRules);

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
    addPrimitive("lambda", { false, [this] (Tree* env, Tree* e) -> Result {
      const auto& [formal, es] = expect<Cons>(e);
      return pool.emplaceBack(Closure{ env, formal, es });
    }});

    // [√] Elimination rule for `Bool`
    addPrimitive("cond", { false, [this] (Tree* env, Tree* e) -> Result {
      const auto& [test, t] = expect<Cons>(e);
      const auto& [iftrue, u] = expect<Cons>(t);
      const auto& iffalse = (holds<Cons>(u)? get<Cons>(u).head : unit);
      bool result = expect<Bool>(eval(env, test)).val;
      return { env, result? iftrue : iffalse };
    }});

    // [√] Introduction rule for sealed `Tree`
    addPrimitive("quote", { false, [this] (Tree* env, Tree* e) -> Result { return quasiquote(env, expect<Cons>(e).head); }});
    addPrimitive("unquote", { false, [this] (Tree* env, Tree* e) -> Result { return eval(env, expect<Cons>(e).head); }});

    // [√] Elimination rule for sealed `Tree`
    addPrimitive("match", { false, [this] (Tree* env, Tree* e) -> Result {
      const auto& [head, t] = expect<Cons>(e);
      const auto& [clauses, _] = expect<Cons>(t);
      const auto& target = eval(env, head);
      for (auto it = clauses; holds<Cons>(it); it = get<Cons>(it).tail) {
        const auto& [pat, u] = expect<Cons>(get<Cons>(it).head);
        Tree* newEnv = env;
        if (match(target, pat, newEnv)) {
          const auto& [expr, _] = expect<Cons>(u);
          return { newEnv, expr };
        }
      }
      // All failed, throw exception
      string msg = "nonexhaustive patterns: { ";
      bool first = true;
      for (auto it = clauses; holds<Cons>(it); it = get<Cons>(it).tail) {
        const auto& [pat, _] = expect<Cons>(get<Cons>(it).head);
        if (!first) msg += ", ";
        first = false;
        msg += pat->toString();
      }
      msg += " } ?= " + target->toString();
      throw PartialEvalError(msg, clauses);
    }});

    // [√] Currently we don't have a `let`, and this is just a synonym of `let*`...
    addPrimitive("let", { false, [this] (Tree* env, Tree* e) -> Result {
      const auto& [defs, es] = expect<Cons>(e);
      for (auto it = defs; holds<Cons>(it); it = get<Cons>(it).tail) {
        const auto& [lhs, t] = expect<Cons>(get<Cons>(it).head);
        const auto& [rhs, _] = expect<Cons>(t);
        env = extend(env, expect<Symbol>(lhs).val, eval(env, rhs));
      }
      return { env, beginList(env, es) };
    }});

    // [√] Currently we don't have a `letrec`, and this is just a synonym of `letrec*`...
    addPrimitive("letrec", { false, [this] (Tree* env, Tree* e) -> Result {
      const auto& [defs, es] = expect<Cons>(e);
      // Add #unit (placeholder) bindings
      vector<Tree**> refs;
      for (auto it = defs; holds<Cons>(it); it = get<Cons>(it).tail) {
        const auto& [lhs, _] = expect<Cons>(get<Cons>(it).head);
        env = extend(env, expect<Symbol>(lhs).val, unit);
        refs.push_back(&get<Cons>(get<Cons>(get<Cons>(env).head).tail).head); // Will always succeed due to the recent `extend`
      }
      // Sequentially evaluate and assign
      size_t i = 0;
      for (auto it = defs; holds<Cons>(it); it = get<Cons>(it).tail) {
        const auto& [lhs, t] = expect<Cons>(get<Cons>(it).head);
        const auto& [rhs, _] = expect<Cons>(t);
        *refs[i++] = eval(env, rhs);
      }
      return { env, beginList(env, es) };
    }});

    // [√] Global definitions will become effective only on the curr statement...
    // For local definitions, use `letrec*`.
    addPrimitive("define", { false, [this] (Tree* env, Tree* e) -> Result {
      const auto& [lhs, t] = expect<Cons>(e);
      const auto& [rhs, _] = expect<Cons>(t);
      globalEnv = extend(globalEnv, expect<Symbol>(lhs).val, eval(env, rhs));
      return unit;
    }});

    // [?]
    addPrimitive("define_macro", { false, [this] (Tree* env, Tree* e) -> Result {
      const auto& [lhs, t] = expect<Cons>(e);
      const auto& [rhs, _] = expect<Cons>(t);
      addMacro(expect<Symbol>(lhs).val, expect<Closure>(eval(env, rhs)));
      return unit;
    }});

    // [√] This modifies nodes reachable through `env` (which prevents making `Tree*` into const pointers)
    // Ignores more arguments
    addPrimitive("set", { false, [this] (Tree* env, Tree* e) -> Result {
      const auto& [lhs, t] = expect<Cons>(e);
      const auto& [rhs, _] = expect<Cons>(t);
      const auto& val = eval(env, rhs);
      string s = expect<Symbol>(lhs).val;
      for (auto it = env; holds<Cons>(it); it = get<Cons>(it).tail) {
        const auto& curr = get<Cons>(it).head;
        if (holds<Cons>(curr)) {
          auto& [lhs, t] = get<Cons>(curr);
          auto& [rhs, _] = get<Cons>(t);
          if (holds<Symbol>(lhs) && get<Symbol>(lhs).val == s) { rhs = val; return unit; }
        }
      }
      // Not found, throw exception
      throw PartialEvalError("unbound symbol \"" + s + "\"", lhs);
    }});

    // [√]
    addPrimitive("begin", { false, [this] (Tree* env, Tree* e) -> Result {
      return { env, beginList(env, e) };
    }});

    // ====================
    // Primitive procedures
    // ====================

    // [√]
    addPrimitive("eval", { true, [] (Tree* env, Tree* e) -> Result {
      const auto& [h, t] = expect<Cons>(e);
      if (holds<Cons>(t)) env = expect<Cons>(t).head;
      return { env, h }; // Let it restart and evaluate
    }});
    addPrimitive("env", { true, [] (Tree* env, Tree*) -> Result { return env; }});
    addPrimitive("get_syntax", { true, [this] (Tree*, Tree*) -> Result { return cons(patterns, cons(rules, nil)); }});
    addPrimitive("set_syntax", { true, [this] (Tree*, Tree* e) -> Result {
      const auto& [p, t] = expect<Cons>(e);
      const auto& [r, _] = expect<Cons>(t);
      setSyntax(p, r);
      return unit;
    }});
    addPrimitive("get_global_env", { true, [this] (Tree*, Tree*) -> Result { return globalEnv; }});
    addPrimitive("set_global_env", { true, [this] (Tree*, Tree* e) -> Result { globalEnv = expect<Cons>(e).head; return unit; }});

    // [?]
    #define unary(name, op) \
      addPrimitive(name, { true, [this] (Tree*, Tree* e) -> Result { \
        const auto& [lhs, _] = expect<Cons>(e); \
        return pool.emplaceBack(Nat64{ op(expect<Nat64>(lhs).val) }); \
      }})

    #define binary(name, op) \
      addPrimitive(name, { true, [this] (Tree*, Tree* e) -> Result { \
        const auto& [lhs, t] = expect<Cons>(e); \
        const auto& [rhs, _] = expect<Cons>(t); \
        return pool.emplaceBack(Nat64{ expect<Nat64>(lhs).val op expect<Nat64>(rhs).val }); \
      }})

    #define binpred(name, op) \
      addPrimitive(name, { true, [btrue, bfalse] (Tree*, Tree* e) -> Result { \
        const auto& [lhs, t] = expect<Cons>(e); \
        const auto& [rhs, _] = expect<Cons>(t); \
        return (expect<Nat64>(lhs).val op expect<Nat64>(rhs).val)? btrue : bfalse; \
      }})

    unary("minus", -);
    binary("add", +);
    binary("sub", -);
    binary("mul", *);
    binary("div", /);
    binpred("le", <=);
    binpred("lt", <);
    binpred("ge", >=);
    binpred("gt", >);
    binpred("eq", ==);

    #undef unary
    #undef binary
    #undef binpred

    // [√] For debugging?
    addPrimitive("print", { true, [this] (Tree*, Tree* e) -> Result {
      return pool.emplaceBack(String{ e->toString() });
    }});

    // [?] TODO: output to ostream
    addPrimitive("display", { true, [this] (Tree*, Tree* e) -> Result {
      const auto& [head, tail] = expect<Cons>(e);
      String res = expect<String>(head);
      return pool.emplaceBack(res);
    }});

    // =========================
    // ApiMu-specific procedures
    // =========================

    /*
    #define cons   pool.emplaceBack
    #define nil    nil
    #define sym(s) pool.emplaceBack(Symbol{ s })
    #define str(s) pool.emplaceBack(String{ s })
    #define num(n) pool.emplaceBack(Number{ n })
    #define obj(o) pool.emplaceBack(Native{ o })

    // [!] Conversions between lists and native objects (`Expr`, `Context`)
    namePrims["list->Expr"] = addPrim(true, [this] (Tree*, Tree* e) -> Result {
      return obj(TreeExpr(expect<Cons>(e).head, epool));
    });
    namePrims["Expr->list"] = addPrim(true, [this] (Tree*, Tree* e) -> Result {
      return ExprTree(expectNative<const Expr*>(expect<Cons>(e).head), pool);
    });
    // ...

    // [?] TEMP CODE
    namePrims["ContextSize"] = addPrim(true, [this] (Tree*, Tree*) -> Result {
      return num(static_cast<Number>(globalCtx.size()));
    });
    namePrims["ContextGet"] = addPrim(true, [this] (Tree*, Tree* e) -> Result {
      auto i = expect<Number>(expect<Cons>(e).head);
      if (i >= 0) {
        size_t index = static_cast<size_t>(i);
        if (index < globalCtx.size()) return cons(str(globalCtx.identifier(index)), cons(obj(globalCtx[index]), nil));
      }
      return unit;
    });
    namePrims["ContextPush"] = addPrim(true, [this] (Tree*, Tree* e) -> Result {
      const auto& [lhs, t] = expect<Cons>(expect<Cons>(e).head);
      const auto& [rhs, _] = expect<Cons>(t);
      string s = expect<String>(lhs);
      const Expr* expr = expectNative<const Expr*>(rhs);
      try { globalCtx.pushAssumption(s, expr); return unit; }
      catch (std::runtime_error& ex) { return str(ex.what()); }
    });
    namePrims["ContextPop"] = addPrim(true, [this] (Tree*, Tree*) -> Result {
      try { globalCtx.pop(); return unit; }
      catch (std::runtime_error& ex) { return str(ex.what()); }
    });
    namePrims["Expr.Print"] = addPrim(true, [this] (Tree*, Tree* e) -> Result {
      return str(expectNative<const Expr*>(expect<Cons>(e).head)->toString(globalCtx));
    });
    namePrims["Expr.PrintFOL"] = addPrim(true, [this] (Tree*, Tree* e) -> Result {
      return str(Core::FOLForm::fromExpr(expectNative<const Expr*>(expect<Cons>(e).head)).toString(globalCtx));
    });
    namePrims["Expr.CheckType"] = addPrim(true, [this] (Tree*, Tree* e) -> Result {
      try { return obj(expectNative<const Expr*>(expect<Cons>(e).head)->checkType(globalCtx, epool)); }
      catch (std::runtime_error& ex) { return str(ex.what()); }
    });

    // [?] TEMP CODE
    namePrims["Expr.MakeBound"] = addPrim(true, [this] (Tree*, Tree* e) -> Result {
      const auto& [h, t] = expect<Cons>(e);
      const auto& [h1, t1] = expect<Cons>(t);
      const auto id = static_cast<uint64_t>(expect<Number>(h1));
      const auto res = TreeExpr(h, epool)->makeBound(id, epool);
      return ExprTree(res, pool);
    });
    namePrims["Expr.MakeReplace"] = addPrim(true, [this] (Tree*, Tree* e) -> Result {
      const auto& [h, t] = expect<Cons>(e);
      const auto& [h1, t1] = expect<Cons>(t);
      const auto res = TreeExpr(h, epool)->makeReplace(TreeExpr(h1, epool), epool);
      return ExprTree(res, pool);
    });

    #undef cons
    #undef nil
    #undef sym
    #undef str
    #undef num
    */

    // END OF FLATBREAD CODE (摊大饼结束)
  }

  // Evaluator entries are stored as lists of two elements.
  Tree* Evaluator::extend(Tree* env, const std::string& s, Tree* e) {
    return cons(cons(sym(s), cons(e, nil)), env);
  }

  Tree* Evaluator::lookup(Tree* env, const std::string& s) {
    for (auto it = env; holds<Cons>(it); it = get<Cons>(it).tail) {
      const auto& curr = get<Cons>(it).head;  if (!holds<Cons>(curr)) continue;
      const auto& [lhs, t] = get<Cons>(curr); if (!holds<Cons>(t)) continue;
      const auto& [rhs, _] = get<Cons>(t);
      if (holds<Symbol>(lhs) && get<Symbol>(lhs).val == s)
        return (holds<Unit>(rhs))? nullptr : rhs;
    }
    return nullptr;
  }

  bool operator==(const EarleyParser::Location& a, const EarleyParser::Location& b) {
    return a.pos == b.pos && a.i == b.i;
  }

  vector<Tree*> Evaluator::resolve(EarleyParser::Location loc, const vector<Tree*>& right, size_t maxDepth) {
    if (maxDepth == 0) return {};
    const auto& [state, links] = parser.getForest()[loc.pos][loc.i];
    vector<Tree*> res;

    if (state.progress == 0) {
      // Whole rule completed
      for (Tree* r: right) res.push_back(cons(sym(ruleNames[state.rule]), r));
      return res;
    }

    // One step to left
    for (const auto& [prevLink, childLink]: links) {
      vector<Tree*> child;
      if (childLink == EarleyParser::Leaf) {
        const auto& tok = parser.getSentence()[loc.pos - 1];
        child = { cons(sym(patternNames[tok.pattern]), cons(str(tok.lexeme), nil)) };
      } else {
        child = resolve(childLink, { nil }, maxDepth - 1);
      }
      vector<Tree*> curr;
      for (Tree* c: child) for (Tree* r: right) curr.push_back(cons(c, r));
      vector<Tree*> final = resolve(prevLink, curr, maxDepth);
      for (Tree* f: final) res.push_back(f);
    }

    return res;
  }

  Tree* Evaluator::resolve(size_t maxDepth) {
    const size_t pos = parser.getSentence().size();
    const auto& forest = parser.getForest();
    if (pos >= forest.size()) unreachable;
    // Mid: pos < forest.size()

    vector<Tree*> all;
    for (size_t i = 0; i < forest[pos].size(); i++) {
      const auto& [state, links] = forest[pos][i];
      const auto& [lhs, rhs] = parser.getRule(state.rule);
      if (state.startPos == 0 && lhs.first == StartSyncat && state.progress == rhs.size()) {
        vector<Tree*> final = resolve({ pos, i }, { nil }, maxDepth);
        for (Tree* f: final) all.push_back(f);
      }
    }

    if (all.empty()) {
      // Failed to resolve (possibly due to excessive depth or infinite expansion)
      notimplemented;
    }
    if (all.size() > 1) {
      // Ambiguous
      for (const auto& parse: all) cout << parse->toString() << endl;
      notimplemented;
    }
    // Mid: all.size() == 1
    return all[0];
  }

  #undef cons
  #undef nil
  #undef sym
  #undef str
  #undef nat
  #undef unit

  Tree* Evaluator::expand(Tree* e) {
    while (true) {
      // Expand current `e`

      if (holds<Cons>(e)) {
        // Non-empty lists: expand all macros, from inside out
        try {
          e = expandList(e);
          const auto& [head, tail] = get<Cons>(e);

          if (holds<Symbol>(head)) {
            // TEMP CODE
            if (get<Symbol>(head).val == "symbol'") {
              string s = expect<String>(expect<Cons>(tail).head).val;
              e = pool.emplaceBack(Symbol{ s });
              return e; // TEMP CODE
              // continue;
            }
            if (get<Symbol>(head).val == "nat64'") {
              string s = expect<String>(expect<Cons>(tail).head).val;
              return pool.emplaceBack(Nat64{ std::stoull(s) });
            }
            if (get<Symbol>(head).val == "string'") {
              string s = expect<String>(expect<Cons>(tail).head).val;
              s = Tree::unescapeString(s.substr(1, s.size() - 2));
              return pool.emplaceBack(String{ s });
            }
            if (get<Symbol>(head).val == "nil'") {
              return nil;
            }
            if (get<Symbol>(head).val == "cons'") {
              const auto& [lhs, u] = expect<Cons>(tail);
              const auto& [rhs, _] = expect<Cons>(u);
              e = pool.emplaceBack(lhs, rhs);
              return e; // TEMP CODE
              // continue;
            }
            if (get<Symbol>(head).val == "period'") {
              const auto& [lhs, u] = expect<Cons>(tail);
              const auto& [mid, v] = expect<Cons>(u);
              const auto& [rhs, _] = expect<Cons>(v);
              e = pool.emplaceBack(lhs, rhs);
              return e; // TEMP CODE
              // continue;
            }
            if (get<Symbol>(head).val == "tree'") {
              const auto& [left, u] = expect<Cons>(tail);
              const auto& [mid,  _] = expect<Cons>(u);
              e = mid;
              return e; // TEMP CODE
              // continue;
            }
            if (get<Symbol>(head).val == "id'") {
              e = expect<Cons>(tail).head;
              return e; // TEMP CODE
              // continue;
            }
            // END TEMP CODE

            const auto it = nameMacros.find(get<Symbol>(head).val);
            if (it != nameMacros.end()) {
              const auto& cl = macros[it->second];
              auto env = cl.env;
              bool matched = match(tail, cl.formal, env);
              if (!matched) throw EvalError("pattern matching failed: " + cl.formal->toString() + " ?= " + tail->toString(), tail, e);
              e = eval(env, beginList(env, cl.es));
              // Make sure the new tree is also fully expanded
              return e; // TEMP CODE
              // continue;
            }
          }

          // Expansion complete
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
  }

  // Expands elements in a list
  Tree* Evaluator::expandList(Tree* e) {
    if (holds<Nil>(e)) return e;
    else if (holds<Cons>(e)) {
      const auto& [head, tail] = get<Cons>(e);
      const auto ehead = expand(head);
      const auto etail = expandList(tail);
      return (ehead == head && etail == tail)? e : pool.emplaceBack(ehead, etail);
    }
    return expand(e);
  }

  Tree* Evaluator::eval(Tree* env, Tree* e) {
    while (true) {
      // Evaluate current `e` under current `env`

      if (holds<Symbol>(e)) {
        // Symbols: evaluate to their bound values
        string sym = get<Symbol>(e).val;
        const auto val = lookup(env, sym);
        if (val) return val;
        const auto it = namePrims.find(sym);
        if (it != namePrims.end()) return pool.emplaceBack(Prim{ it->second });
        throw PartialEvalError("unbound symbol \"" + sym + "\"", e);

      } else if (holds<Cons>(e)) {
        // Non-empty lists: evaluate as function application
        try {
          const auto& [head, tail] = get<Cons>(e);
          const auto ehead = eval(env, head);

          if (holds<Prim>(ehead)) {
            // Primitive function application
            const auto& [evalParams, f] = prims[get<Prim>(ehead).id];
            const auto res = f(env, evalParams? evalList(env, tail) : tail);
            // No tail call, return
            if (!res.env) return res.e;
            // Tail call
            env = res.env;
            e = res.e;
            continue;
          }

          if (holds<Closure>(ehead)) {
            // Lambda function application
            const auto& cl = get<Closure>(ehead);
            const auto params = evalList(env, tail);
            // Evaluate body as tail call
            env = cl.env;
            bool matched = match(params, cl.formal, env);
            if (!matched) throw EvalError("pattern matching failed: " + cl.formal->toString() + " ?= " + params->toString(), tail, e);
            e = beginList(env, cl.es);
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
    if (holds<Nil>(e)) return e;
    else if (holds<Cons>(e)) {
      const auto& [head, tail] = get<Cons>(e);
      const auto ehead = eval(env, head);
      const auto etail = evalList(env, tail);
      return (ehead == head && etail == tail)? e : pool.emplaceBack(ehead, etail);
    }
    return eval(env, e);
  }

  // Executes elements in a list, except the last one (for tail call optimization)
  // Returns the last one unevaluated, or `#unit` if list is empty
  Tree* Evaluator::beginList(Tree* env, Tree* e) {
    for (auto it = e; holds<Cons>(it); it = get<Cons>(it).tail) {
      const auto& [head, tail] = get<Cons>(it);
      if (!holds<Cons>(tail)) {
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
    if (holds<Cons>(e)) {
      const auto& [head, tail] = get<Cons>(e);
      if (*head == Tree(Symbol{ "unquote" })) return eval(env, expect<Cons>(tail).head);
      const auto ehead = quasiquote(env, head);
      const auto etail = quasiquote(env, tail);
      return (ehead == head && etail == tail)? e : pool.emplaceBack(ehead, etail);
    }
    return e;
  }

  /*
  Tree* ExprTree(const Expr* e, Allocator<Tree>& pool) {
    using enum Expr::Tag;
    using enum Expr::SortTag;
    using enum Expr::VarTag;
    #define cons   pool.emplaceBack
    #define nil    pool.emplaceBack(Nil{})
    #define sym(s) pool.emplaceBack(Symbol{ s })
    #define str(s) pool.emplaceBack(String{ s })
    #define num(n) pool.emplaceBack(Number{ n })
    switch (e->tag) {
      case Sort:
        switch (e->sort.tag) {
          case SProp: return cons(sym("Sort"), cons(sym("Prop"), nil));
          case SType: return cons(sym("Sort"), cons(sym("Type"), nil));
          case SKind: return cons(sym("Sort"), cons(sym("Kind"), nil));
        }
        break;
      case Var:
        switch (e->var.tag) {
          case VBound: return cons(sym("Var"), cons(sym("Bound"), cons(num(static_cast<Number>(e->var.id)), nil)));
          case VFree:  return cons(sym("Var"), cons(sym("Free"),  cons(num(static_cast<Number>(e->var.id)), nil)));
          case VMeta:  return cons(sym("Var"), cons(sym("Meta"),  cons(num(static_cast<Number>(e->var.id)), nil)));
        }
        break;
      case App: return cons(sym("App"),                     cons(ExprTree(e->app.l, pool), cons(ExprTree(e->app.r, pool), nil)));
      case Lam: return cons(sym("Lam"), cons(str(e->lam.s), cons(ExprTree(e->lam.t, pool), cons(ExprTree(e->lam.r, pool), nil))));
      case Pi:  return cons(sym("Pi"),  cons(str(e->pi.s),  cons(ExprTree(e->pi.t,  pool), cons(ExprTree(e->pi.r,  pool), nil))));
    } exhaustive;
    #undef cons
    #undef nil
    #undef sym
    #undef str
    #undef num
  }

  const Expr* TreeExpr(Tree* e, Allocator<Expr>& pool) {
    using enum Expr::Tag;
    #define expr pool.emplaceBack
    const auto& [h, t] = expect<Cons>(e);
    string sym = expect<Symbol>(h).s;
    if (sym == "Sort") {
      const auto& [h1, t1] = expect<Cons>(t);
      expect<Nil>(t1);
      string tag = expect<Symbol>(h1).s;
      if      (tag == "Prop") return expr(Expr::SProp);
      else if (tag == "Type") return expr(Expr::SType);
      else if (tag == "Kind") return expr(Expr::SKind);
      else throw PartialEvalError(R"(tag must be "Prop", "Type" or "Kind")", h1);
    } else if (sym == "Var") {
      const auto& [h1, t1] = expect<Cons>(t);
      const auto& [h2, t2] = expect<Cons>(t1);
      expect<Nil>(t2);
      string tag = expect<Symbol>(h1).s;
      Number id = expect<Number>(h2);
      if (id < 0) throw PartialEvalError(R"(variable id must be nonnegative)", h2);
      if      (tag == "Bound") return expr(Expr::VBound, static_cast<uint64_t>(id));
      else if (tag == "Free")  return expr(Expr::VFree,  static_cast<uint64_t>(id));
      else if (tag == "Meta")  return expr(Expr::VMeta,  static_cast<uint64_t>(id));
      else throw PartialEvalError(R"(tag must be "Bound", "Free" or "Meta")", h1);
    } else if (sym == "App") {
      const auto& [h1, t1] = expect<Cons>(t);
      const auto& [h2, t2] = expect<Cons>(t1);
      expect<Nil>(t2);
      return expr(TreeExpr(h1, pool), TreeExpr(h2, pool));
    } else if (sym == "Lam") {
      const auto& [h1, t1] = expect<Cons>(t);
      const auto& [h2, t2] = expect<Cons>(t1);
      const auto& [h3, t3] = expect<Cons>(t2);
      expect<Nil>(t3);
      return expr(Expr::LLam, expect<String>(h1), TreeExpr(h2, pool), TreeExpr(h3, pool));
    } else if (sym == "Pi") {
      const auto& [h1, t1] = expect<Cons>(t);
      const auto& [h2, t2] = expect<Cons>(t1);
      const auto& [h3, t3] = expect<Cons>(t2);
      expect<Nil>(t3);
      return expr(Expr::PPi, expect<String>(h1), TreeExpr(h2, pool), TreeExpr(h3, pool));
    } else throw PartialEvalError(R"(symbol must be "Sort", "Var", "App", "Lam" or "Pi")", h);
    #undef expr
  }
  */

}