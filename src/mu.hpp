// :: Mu

#ifndef MU_HPP_
#define MU_HPP_

#include "parsing/language.hpp"
#include "core.hpp"


class Mu: public Parsing::Language {
public:
  struct AnalysisInfo {
    size_t startPos, endPos;
    std::string info;
    AnalysisInfo(const Parsing::ParseTree* x, const std::string& s): startPos(x->startPos), endPos(x->endPos), info(s) {}
    AnalysisInfo(size_t startPos, size_t endPos, const std::string& s): startPos(startPos), endPos(endPos), info(s) {}
  };

  struct AnalysisErrorException: public std::runtime_error {
    size_t startPos, endPos;
    AnalysisErrorException(const Parsing::ParseTree* x, const std::string& s): std::runtime_error(s), startPos(x->startPos), endPos(x->endPos) {}
    AnalysisErrorException(size_t startPos, size_t endPos, const std::string& s): std::runtime_error(s), startPos(startPos), endPos(endPos) {}
  };

  Mu();
  void analyze(const std::string& str);
  std::vector<AnalysisInfo> popAnalysisInfo();
  std::vector<AnalysisErrorException> popAnalysisErrors();

private:
  Core::Allocator<Core::Expr> exprs{};
  Core::Allocator<Core::Proof> proofs{};

  Core::Context ctx{};
  bool immediate{};
  std::vector<std::pair<size_t, size_t>> scopes{};
  std::vector<std::string> boundVars{};

  size_t lparenPattern{}, rparenPattern{}, parenRule{};
  // Word -> (pattern ID, ref count)
  std::unordered_map<std::string, std::pair<size_t, size_t>> wordlike{};
  // Name -> (rule ID, words involved)
  std::unordered_map<std::string, std::pair<size_t, std::vector<std::string>>> customParsingRules{};

  std::unordered_map<void*, std::pair<size_t, size_t>> sourceMap{};
  std::vector<AnalysisInfo> info{};
  std::vector<AnalysisErrorException> errors{};

  template <typename ...Ts>
  Core::Expr* makeExprLoc(const Parsing::ParseTree* x, const Ts&... args) {
    Core::Expr* res = exprs.pushBack(Core::Expr(args...));
    sourceMap[res] = { x->startPos, x->endPos };
    return res;
  }

  template <typename ...Ts>
  Core::Proof* makeProofLoc(const Parsing::ParseTree* x, const Ts&... args) {
    Core::Proof* res = proofs.pushBack(Core::Proof(args...));
    sourceMap[res] = { x->startPos, x->endPos };
    return res;
  }

  size_t wordlikePattern(const std::string& word);
  void removeWordlikePattern(const std::string& word);

  template <typename T>
  size_t wordlikePatternRule(const std::string& word, const T& res) {
    Parsing::Symbol wordsym = lexer.patternSymbol(wordlikePattern(word));
    return addRuleImpl(
      Parsing::SymbolName<T>::get(),
      getSymbol<T>(),
      std::vector<Parsing::Symbol>({ wordsym }),
      [res] (const Parsing::ParseTree*) { return res; });
  }

  Parsing::ParseTree* replaceVarsByExprs(const Parsing::ParseTree* x, const std::unordered_map<std::string, const Parsing::ParseTree*> mp);
};

#endif // MU_HPP_
