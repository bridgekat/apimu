// Elab :: Tableau

#ifndef TABLEAU_HPP_
#define TABLEAU_HPP_

#include <vector>
#include <utility>
#include <unordered_set>
#include <core.hpp>
#include "procs.hpp"


namespace Elab {

  using std::vector;
  using std::pair, std::make_pair;
  using std::unordered_set;
  using namespace Core;


  // "Expression with hash" (a wrapper for `const Expr*` that overloads the `==` operator)
  struct ExprHash {
    const Expr* e;
    size_t hash;

    // `*e` should not be changed after this construction
    explicit ExprHash(const Expr* e) noexcept: e(e), hash(e->hash()) {}
    bool operator==(const ExprHash& r) const noexcept { return hash == r.hash && *e == *(r.e); }
    bool operator!=(const ExprHash& r) const noexcept { return hash != r.hash || *e != *(r.e); }

    struct GetHash {
      size_t operator()(const ExprHash& eh) const noexcept { return eh.hash; }
    };
  };

  // Method of analytic tableaux (aka. sequent calculus) for classical logic
  // (LOTS OF TEMPORARY CODE NOW!)

  // For an introduction, see:
  // - https://en.wikipedia.org/wiki/Method_of_analytic_tableaux
  // - https://en.wikipedia.org/wiki/Sequent_calculus#The_system_LK

  // For implementation-related things, see:
  // - https://www21.in.tum.de/teaching/sar/SS20/2.pdf
  // - https://moodle.risc.jku.at/pluginfile.php/10562/mod_resource/content/12/07-fol3.pdf
  // - https://www.wolfgangschwarz.net/trees/
  // - https://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.216.388&rep=rep1&type=pdf
  //   (Also contains several completeness proofs)

  // For translation of LK (tableaux) to NK (natural deduction), see:
  // - http://ceur-ws.org/Vol-2162/paper-03.pdf

  class Tableau {
  public:
    // Antecedents in `cedents[...][L]` and `hashset[L]`
    // Succedents in `cedents[...][R]` and `hashset[R]`
    // Cedents are classified as either "ι" (atomic), "α" (non-branching), "β" (branching), "γ" (universal) or "δ" (existential).
    // (TODO: "ε" (equational) and "φ" (second-order universal))
    enum Position: unsigned int { L, R };
    enum Type: unsigned int { ι, α, β, γ, δ, N }; // Tweak parameters here (1/3)

    Tableau(const Context& ctx) noexcept:
      pool(), ctx(ctx), cedents(), indices{}, hashset(), numUniversal{}, numSkolem{}, subs() {}

    void addAntecedent(const Expr* e) {
      auto it = hashset[L].insert(ExprHash(e));
      if (it.second) cedents[classify(L, e)][L].push_back(e);
    }

    void addSuccedent(const Expr* e) {
      auto it = hashset[R].insert(ExprHash(e));
      if (it.second) cedents[classify(R, e)][R].push_back(e);
    }

    // TODO: initial check (the given proof state may already be closed)

    void clear() noexcept {
      pool.clear();
      for (unsigned int i = 0; i < N; i++) {
        cedents[i][L].clear();
        cedents[i][R].clear();
        indices[i][L] = 0;
        indices[i][R] = 0;
      }
      hashset[L].clear();
      hashset[R].clear();
    }

    bool search(int maxDepth);
    string printStats();

  private:
    Allocator<Expr> pool;
    const Context& ctx;                                    // For offset and `eq`
    vector<const Expr*> cedents[N][2];                     // For queue-like structures
    size_t indices[N][2];                                  // Head index of queues
    unordered_set<ExprHash, ExprHash::GetHash> hashset[2]; // For fast membership testing

    // Ephemeral states
    // Number of new variables (for allocating new variable IDs)
    size_t numUniversal, numSkolem;
    Procs::Subs subs;

    // Statistics
    int maxDepthReached = 0;
    size_t invocations = 0, branches = 0, closed = 0;

    template <Position LR, Position RL>
    friend class WithCedent;

    static Type classify(Position antesucc, const Expr* e) noexcept;
    bool dfs(int maxDepth);
  };

}

#endif // TABLEAU_HPP_
