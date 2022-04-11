#include "procs.hpp"


namespace Elab::Procs {

  // Allow throwing in `noexcept` functions; we really intend to terminate with an error message
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wterminate"

  /*
  bool propValue(const Expr* e, const vector<bool>& fvmap) {
    using enum Expr::Tag;
    using enum Expr::VarTag;
    switch (e->tag) {
      case Var:
        if (e->var.tag != VFree) throw Unreachable();
        return e->var.id < fvmap.size() ? fvmap[e->var.id] : false;
      case App:
        return false;
      default:
        break;
    }
    throw NotImplemented();
  }

  Expr* toNNF(const Expr* e, const Context& ctx, Allocator<Expr>& pool, bool negated) {
    using enum Expr::Tag;
    using enum Expr::VarTag;
    switch (e->tag) {
      case Var:
        return negated ? Expr::make(pool, NOT, e->clone(pool)) : e->clone(pool);
      case TRUE:
        return Expr::make(pool, negated ? FALSE : TRUE);
      case FALSE:
        return Expr::make(pool, negated ? TRUE : FALSE);
      case NOT:
        return toNNF(e->conn.l, ctx, pool, !negated);
      case AND:
        return Expr::make(pool, negated ? OR : AND,
          toNNF(e->conn.l, ctx, pool, negated),
          toNNF(e->conn.r, ctx, pool, negated));
      case OR:
        return Expr::make(pool, negated ? AND : OR,
          toNNF(e->conn.l, ctx, pool, negated),
          toNNF(e->conn.r, ctx, pool, negated));
      case IMPLIES:  // (p implies q) seen as ((not p) or q)
        return Expr::make(pool, negated ? AND : OR,
          toNNF(e->conn.l, ctx, pool, !negated),
          toNNF(e->conn.r, ctx, pool, negated));
      case IFF: {    // (p iff q) seen as ((p implies q) and (q implies p))
        Expr mp(IMPLIES, e->conn.l, e->conn.r);
        Expr mpr(IMPLIES, e->conn.r, e->conn.l);
        return Expr::make(pool, negated ? OR : AND,
          toNNF(&mp, ctx, pool, negated),
          toNNF(&mpr, ctx, pool, negated));
      }
      case FORALL:
        return Expr::make(pool, negated ? EXISTS : FORALL,
          e->bv, e->binder.arity, e->binder.sort,
          toNNF(e->binder.r, ctx, pool, negated));
      case EXISTS:
        return Expr::make(pool, negated ? FORALL : EXISTS,
          e->bv, e->binder.arity, e->binder.sort,
          toNNF(e->binder.r, ctx, pool, negated));
      case UNIQUE: { // (unique x, p) seen as ((exists x, p) and (forall x, p implies (forall x', p implies x = x')))
        Expr exi(EXISTS, e->bv, e->binder.arity, e->binder.sort, e->binder.r);
        Expr x(BOUND, 1), x_(BOUND, 0);
        Expr eq(FREE, ctx.equals, { &x, &x_ });
        Expr d(IMPLIES, e->binder.r, &eq);
        Expr c(FORALL, e->bv + "'", e->binder.arity, e->binder.sort, &d);
        Expr b(IMPLIES, e->binder.r, &c);
        Expr a(FORALL, e->bv, e->binder.arity, e->binder.sort, &b);
        Expr no2 = a;
        return Expr::make(pool, negated ? OR : AND,
          toNNF(&exi, ctx, pool, negated),
          toNNF(&no2, ctx, pool, negated));
      }
      case FORALL2: case LAMBDA:
        throw Unreachable();
    }
    throw NotImplemented();
  }
  */

  #pragma GCC diagnostic pop

  string showSubs(const Subs& subs, const Context& ctx) {
    using enum Expr::VarTag;
    string res;
    for (size_t i = 0; i < subs.size(); i++) if (subs[i]) {
      res += Expr(VMeta, i).toString(ctx) + " => " + subs[i]->toString(ctx) + "\n";
    }
    return res;
  }

  /*
  // A simple anti-unification algorithm.
  // See: https://en.wikipedia.org/wiki/Anti-unification_(computer_science)#First-order_syntactical_anti-unification
  class Antiunifier {
  public:
    Allocator<Expr>* pool;
    Subs ls, rs;

    Antiunifier(): pool{}, ls(), rs() {}
    Antiunifier(const Antiunifier&) = delete;
    Antiunifier& operator=(const Antiunifier&) = delete;

    Expr* dfs(const Expr* lhs, const Expr* rhs) {
      using enum Expr::Tag;
      using enum Expr::VarTag;

      // If roots are different, return this
      auto different = [this, lhs, rhs] () {
        unsigned int id = ls.size();
        ls.push_back(lhs);
        rs.push_back(rhs);
        return Expr::make(*pool, UNDETERMINED, id);
      };
 
      if (lhs->tag != rhs->tag) return different();
      // lhs->tag == rhs->tag
      switch (lhs->tag) {
        case VAR: {
          if (lhs->var.vartag != rhs->var.vartag || lhs->var.id != rhs->var.id) {
            return different();
          }
          Expr* res = Expr::make(*pool, lhs->var.vartag, lhs->var.id), * last = nullptr;
          const Expr* plhs = lhs->var.c, * prhs = rhs->var.c;
          for (; plhs && prhs; plhs = plhs->s, prhs = prhs->s) {
            Expr* q = dfs(plhs, prhs);
            (last? last->s : res->var.c) = q;
            last = q;
          }
          (last? last->s : res->var.c) = nullptr;
          if (plhs || prhs) throw Unreachable();
          return res;
        }
        case TRUE: case FALSE:
          return Expr::make(*pool, lhs->tag);
        case NOT:
          return Expr::make(*pool, lhs->tag, dfs(lhs->conn.l, rhs->conn.l));
        case AND: case OR: case IMPLIES: case IFF:
          return Expr::make(*pool, lhs->tag, dfs(lhs->conn.l, rhs->conn.l), dfs(lhs->conn.r, rhs->conn.r));
        case FORALL: case EXISTS: case UNIQUE: case FORALL2: case LAMBDA:
          if (lhs->binder.arity != rhs->binder.arity || lhs->binder.sort != rhs->binder.sort) {
            return different();
          }
          return Expr::make(*pool, lhs->tag,
            lhs->bv, lhs->binder.arity, lhs->binder.sort,
            dfs(lhs->binder.r, rhs->binder.r));
      }
      throw NotImplemented();
    }

    tuple<Expr*, Subs, Subs> operator()(Allocator<Expr>* pool, const Expr* lhs, const Expr* rhs) {
      this->pool = pool;
      ls.clear(); rs.clear();
      Expr* c = dfs(lhs, rhs);
      return { c, ls, rs };
    }
  };

  tuple<Expr*, Subs, Subs> antiunify(const Expr* lhs, const Expr* rhs, Allocator<Expr>& pool) {
    return Antiunifier()(&pool, lhs, rhs);
  }

  // The Robinson's unification algorithm (could take exponential time for certain cases.)
  // See: https://en.wikipedia.org/wiki/Unification_(computer_science)#A_unification_algorithm
  optional<Subs> unify(vector<pair<const Expr*, const Expr*>> a, Allocator<Expr>& pool) {
    using enum Expr::Tag;
    using enum Expr::VarTag;
    Subs res;

    // Add a new substitution to `res`, then update the rest of `a` to eliminate the variable with id `id`.
    auto putsubs = [&res, &pool, &a] (unsigned int id, const Expr* e, size_t i0) {
      // Make enough space
      while (id >= res.size()) res.push_back(nullptr);
      // id < res.size()
      res[id] = e;
      // Update the rest of `a`
      auto f = [id, e, &pool] (unsigned int, Expr* x) {
        if (x->var.vartag == UNDETERMINED && x->var.id == id) return e->clone(pool);
        return x;
      };
      for (size_t i = i0; i < a.size(); i++) {
        a[i].first = a[i].first->updateVars(0, pool, f);
        a[i].second = a[i].second->updateVars(0, pool, f);
      }
    };

    // Each step transforms `a` into an equivalent set of equations
    // (in `a` and `res`; the latter contains equations in triangular/solved form.)
    for (size_t i = 0; i < a.size(); i++) {
      const Expr* lhs = a[i].first, * rhs = a[i].second;

      if (lhs->tag == VAR && lhs->var.vartag == UNDETERMINED) {
        // Variable elimination on the left.
        if (*lhs == *rhs);
        else if (rhs->occurs(UNDETERMINED, lhs->var.id)) return nullopt;
        else putsubs(lhs->var.id, rhs, i + 1);
        continue;

      } else if (rhs->tag == VAR && rhs->var.vartag == UNDETERMINED) {
        // Variable elimination on the right.
        if (*lhs == *rhs);
        else if (lhs->occurs(UNDETERMINED, rhs->var.id)) return nullopt;
        else putsubs(rhs->var.id, lhs, i + 1);
        continue;

      } else {
        // Try term reduction.
        if (lhs->tag != rhs->tag) return nullopt;
        // lhs->tag == rhs->tag
        switch (lhs->tag) {
          case VAR: {
            if (lhs->var.vartag != rhs->var.vartag || lhs->var.id != rhs->var.id) return nullopt;
            const Expr* plhs = lhs->var.c, * prhs = rhs->var.c;
            for (; plhs && prhs; plhs = plhs->s, prhs = prhs->s) {
              a.emplace_back(plhs, prhs);
            }
            if (plhs || prhs) throw Unreachable();
            break;
          }
          case TRUE: case FALSE:
            break;
          case NOT:
            a.emplace_back(lhs->conn.l, rhs->conn.l);
            break;
          case AND: case OR: case IMPLIES: case IFF:
            a.emplace_back(lhs->conn.l, rhs->conn.l);
            a.emplace_back(lhs->conn.r, rhs->conn.r);
            break;
          case FORALL: case EXISTS: case UNIQUE: case FORALL2: case LAMBDA:
            if (lhs->binder.arity != rhs->binder.arity || lhs->binder.sort != rhs->binder.sort) {
              return nullopt;
            }
            a.emplace_back(lhs->binder.r, rhs->binder.r);
            break;
        }
        continue;
      }
    }

    return res;
  }
  */

}
