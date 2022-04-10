#include "expr.hpp"
#include <type_traits>


namespace Core {

  using std::string;
  using std::vector;


  // Allow throwing in `noexcept` functions; we really intend to terminate with an error message
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wterminate"

  Expr* Expr::clone(Allocator<Expr>& pool) const {
    switch (tag) {
      case Sort: return make(pool, sort.tag);
      case Var: return make(pool, var.tag, var.id);
      case App: return make(pool, app.l? app.l->clone(pool) : nullptr, app.r? app.r->clone(pool) : nullptr);
      case Lam: return make(pool, LLam, lam.s, lam.t? lam.t->clone(pool) : nullptr, lam.r? lam.r->clone(pool) : nullptr);
      case Pi: return make(pool, PPi, pi.s, pi.t? pi.t->clone(pool) : nullptr, pi.r? pi.r->clone(pool) : nullptr);
    }
    throw NotImplemented();
  }

  bool Expr::operator==(const Expr& rhs) const noexcept {
    if (this == &rhs) return true;
    if (tag != rhs.tag) return false;
    // tag == rhs.tag
    #define nullorsame(x) ((!x && !rhs.x) || (x && rhs.x && *x == *rhs.x))
    switch (tag) {
      case Sort: return sort.tag == rhs.sort.tag;
      case Var: return var.tag == rhs.var.tag && var.id == rhs.var.id;
      case App: return nullorsame(app.l) && nullorsame(app.r);
      case Lam: return nullorsame(lam.t) && nullorsame(lam.r); // Ignore bound variable names
      case Pi: return nullorsame(pi.t) && nullorsame(pi.r);    // Ignore bound variable names
    }
    #undef nullorsame
    throw NotImplemented();
  }

  // Using: https://stackoverflow.com/questions/2590677/how-do-i-combine-hash-values-in-c0x
  template <class T>
  inline void hash_combine(size_t& seed, const T& v) noexcept {
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  }

  size_t Expr::hash() const noexcept {
    size_t res = static_cast<size_t>(tag);
    switch (tag) {
      case Sort:
        hash_combine(res, static_cast<std::underlying_type_t<SortTag>>(sort.tag));
        return res;
      case Var:
        hash_combine(res, static_cast<std::underlying_type_t<VarTag>>(var.tag));
        hash_combine(res, var.id);
        return res;
      case App:
        hash_combine(res, app.l? app.l->hash() : 0);
        hash_combine(res, app.r? app.r->hash() : 0);
        return res;
      case Lam:
        // Ignore bound variable names
        hash_combine(res, lam.t? lam.t->hash() : 0);
        hash_combine(res, lam.r? lam.r->hash() : 0);
        return res;
      case Pi:
        // Ignore bound variable names
        hash_combine(res, pi.t? pi.t->hash() : 0);
        hash_combine(res, pi.r? pi.r->hash() : 0);
        return res;
    }
    throw NotImplemented();
  }

  // Give unnamed bound variables a random name
  inline string newName(size_t i) {
    string res = "__";
    do {
      res.push_back('a' + i % 26);
      i /= 26;
    } while (i > 0);
    return res;
  }

  // Undefined variables and null pointers should be OK, as long as non-null pointers are valid.
  string Expr::toString(const Context& ctx, vector<string>& stk) const {
    switch (tag) {
      case Sort: return
        sort.tag == SProp ? "Prop" :
        sort.tag == SType ? "Type" :
        "@N";
      case Var: return
        var.tag == VBound ? (var.id < stk.size() ? stk[stk.size() - 1 - var.id] : "@B" + std::to_string(var.id)) :
        var.tag == VFree  ? (ctx.valid(var.id)   ? ctx.nameOf(var.id)           : "@F" + std::to_string(var.id)) :
        var.tag == VMeta  ? "@M" + std::to_string(var.id) :
        "@N";
      case App: return
        "(" + (app.l? app.l->toString(ctx, stk) : "@N") +
        " " + (app.r? app.r->toString(ctx, stk) : "@N") + ")";
      case Lam: {
        string res, name = lam.s.empty()? newName(stk.size()) : lam.s;
        res = "(\\" + name + ": " + (lam.t? lam.t->toString(ctx, stk) : "@N");
        stk.push_back(name);
        res += " => " + (lam.r? lam.r->toString(ctx, stk) : "@N") + ")";
        stk.pop_back();
        return res;
      }
      case Pi: {
        string res, name = pi.s.empty()? newName(stk.size()) : pi.s;
        res = "((" + name + ": " + (pi.t? pi.t->toString(ctx, stk) : "@N") + ")";
        stk.push_back(name);
        res += " -> " + (pi.r? pi.r->toString(ctx, stk) : "@N") + ")";
        stk.pop_back();
        return res;
      }
    }
    throw NotImplemented();
  }

  inline Expr::SortTag imax(Expr::SortTag s, Expr::SortTag t) {
    if (s == Expr::SProp || t == Expr::SProp) return Expr::SProp;
    // s == Expr::SType && t == Expr::SType
    return Expr::SType;
  }

  // Check if the subtree is a well-formed term (1), type (2) or proof (3).
  // (1) Returns a well-formed, beta-reduced expression of type `Type`, representing the type of the term;
  // (2) Returns `Type` itself;
  // (3) Returns a well-formed, beta-reduced expression of type `Prop`, representing the proposition it proves.
  Expr* Expr::checkType(const Context& ctx, Allocator<Expr>& pool, vector<const Expr*>& stk, vector<string>& names) const {
    switch (tag) {
      case Sort: {
        switch (sort.tag) {
          case SProp: return pool.emplaceBack(SType);
          case SType: throw InvalidExpr("\"Type\" does not have a type", ctx, this);
        }
        throw NotImplemented();
      }
      case Var: {
        const Expr* t =
          var.tag == VBound ? (var.id < stk.size() ? stk[stk.size() - 1 - var.id] : nullptr) :
          var.tag == VFree  ? (ctx.valid(var.id)   ? ctx[var.id]                  : nullptr) :
          nullptr;
        if (!t)
          throw InvalidExpr(
            var.tag == VBound ? "de Bruijn index overflow" :
            var.tag == VFree  ? "free variable not in context" :
            var.tag == VMeta  ? "unexpected metavariable" :
            "unknown variable tag: " + static_cast<std::underlying_type_t<VarTag>>(var.tag), ctx, this);
        return t->reduce(pool);
      }
      case App: { // Π-elimination
        if (!app.l || !app.r) throw InvalidExpr("unexpected null pointer", ctx, this);
        Expr* tl = app.l->checkType(ctx, pool, stk, names);
        Expr* tr = app.r->checkType(ctx, pool, stk, names);
        // By postcondition of checkType(), returned type expression is arity-correct (no null pointers)
        if (tl->tag != Pi) throw InvalidExpr("expected function, term has type " + tl->toString(ctx, names), ctx, app.l);
        if (*tl->pi.t != *tr) throw InvalidExpr("argument type mismatch, expected " + tl->pi.t->toString(ctx, names) + ", got " + tr->toString(ctx, names), ctx, app.r);
        return tl->pi.r->makeReplace(app.r, pool)->reduce(pool);
      }
      case Lam: { // Π-introduction
        if (!lam.t || !lam.r) throw InvalidExpr("unexpected null pointer", ctx, this);
        Expr* tt = lam.t->checkType(ctx, pool, stk, names);
        if (tt->tag != Sort) throw InvalidExpr("expected proposition or type, got" + tt->toString(ctx, names), ctx, lam.t);
        names.push_back(lam.s);
        stk.push_back(lam.t);
        Expr* tr = lam.r->checkType(ctx, pool, stk, names);
        names.pop_back();
        stk.pop_back();
        return pool.emplaceBack(PPi, lam.s, lam.t->reduce(pool), tr);
      }
      case Pi: { // Π-formation
        if (!pi.t || !pi.r) throw InvalidExpr("unexpected null pointer", ctx, this);
        Expr* tt = pi.t->checkType(ctx, pool, stk, names);
        if (tt->tag != Sort) throw InvalidExpr("expected proposition or type, got" + tt->toString(ctx, names), ctx, pi.t);
        names.push_back(pi.s);
        stk.push_back(pi.t);
        Expr* tr = pi.r->checkType(ctx, pool, stk, names);
        names.pop_back();
        stk.pop_back();
        if (tr->tag != Sort) throw InvalidExpr("expected proposition or type, got" + tr->toString(ctx, names), ctx, pi.r);
        return pool.emplaceBack(imax(tt->sort.tag, tr->sort.tag));
      }
    }
    throw NotImplemented();
  }

  Expr* Expr::reduce(Allocator<Expr>& pool) const {
    switch (tag) {
      case Sort: return make(pool, sort.tag);
      case Var: return make(pool, var.tag, var.id);
      case App: {
        Expr* l = app.l? app.l->reduce(pool) : nullptr;
        Expr* r = app.r? app.r->reduce(pool) : nullptr;
        if (l && r && l->tag == Lam) return l->lam.r->makeReplace(r, pool)->reduce(pool);
        return make(pool, l, r);
      }
      case Lam: return make(pool, LLam, lam.s, lam.t? lam.t->reduce(pool) : nullptr, lam.r? lam.r->reduce(pool) : nullptr);
      case Pi: return make(pool, PPi, pi.s, pi.t? pi.t->reduce(pool) : nullptr, pi.r? pi.r->reduce(pool) : nullptr);
    }
    throw NotImplemented();
  }

  size_t Expr::size() const noexcept {
    switch (tag) {
      case Sort: return 1;
      case Var: return 1;
      case App: return 1 + (app.l? app.l->size() : 0) + (app.r? app.r->size() : 0);
      case Lam: return 1 + (lam.t? lam.t->size() : 0) + (lam.r? lam.r->size() : 0);
      case Pi: return 1 + (pi.t? pi.t->size() : 0) + (pi.r? pi.r->size() : 0);
    }
    throw NotImplemented();
  }

  bool Expr::occurs(VarTag vartag, uint64_t id) const noexcept {
    switch (tag) {
      case Sort: return false;
      case Var: return var.tag == vartag && var.id == id;
      case App: return (app.l && app.l->occurs(vartag, id)) || (app.r && app.r->occurs(vartag, id));
      case Lam: return (lam.t && lam.t->occurs(vartag, id)) || (lam.r && lam.r->occurs(vartag, id));
      case Pi: return (pi.t && pi.t->occurs(vartag, id)) || (pi.r && pi.r->occurs(vartag, id));
    }
    throw NotImplemented();
  }

  size_t Expr::numUndetermined() const noexcept {
    switch (tag) {
      case Sort: return 0;
      case Var: return var.tag == VMeta? (var.id + 1) : 0;
      case App: return std::max(app.l? app.l->numUndetermined() : 0, app.r? app.r->numUndetermined() : 0);
      case Lam: return std::max(lam.t? lam.t->numUndetermined() : 0, lam.r? lam.r->numUndetermined() : 0);
      case Pi: return std::max(pi.t? pi.t->numUndetermined() : 0, pi.r? pi.r->numUndetermined() : 0);
    }
    throw NotImplemented();
  }

  #pragma GCC diagnostic pop

}
