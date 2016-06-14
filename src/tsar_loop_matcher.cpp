//===- tsar_loop_matcher.h - High and Low Level Loop Matcher-----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements pass to match loops.
//
//===----------------------------------------------------------------------===//

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Function.h>
#include "tsar_loop_matcher.h"
#include "tsar_transformation.h"
#include "tsar_utility.h"

using namespace clang;
using namespace llvm;
using namespace tsar;
using ::llvm::Module;

#undef DEBUG_TYPE
#define DEBUG_TYPE "loop-matcher"

STATISTIC(NumMatchLoop, "Number of matched loops");
STATISTIC(NumNonMatchIRLoop, "Number of non-matched IR loops");
STATISTIC(NumNonMatchASTLoop, "Number of non-matched AST loops");

char LoopMatcherPass::ID = 0;
INITIALIZE_PASS_BEGIN(LoopMatcherPass, "matcher",
  "High and Low Loop Matcher", true, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(LoopMatcherPass, "matcher",
  "High and Low Level Loop Matcher", true, true)

namespace {
/// \brief Implementation of a DenseMapInfo for DILocation *.
///
/// To generate hash value pair of line and column is used. It is possible to
/// use find_as() method with a parameter of type clang::PresumedLoc.
struct DILocationMapInfo {
  static inline DILocation * getEmptyKey() {
    return DenseMapInfo<DILocation *>::getEmptyKey();
  }
  static inline DILocation * getTombstoneKey() {
    return DenseMapInfo<DILocation *>::getTombstoneKey();
  }
  static unsigned getHashValue(const DILocation *Loc) {
    auto Line = Loc->getLine();
    auto Column = Loc->getColumn();
    auto Pair = std::make_pair(Line, Column);
    return DenseMapInfo<decltype(Pair)>::getHashValue(Pair);
  }
  static unsigned getHashValue(const PresumedLoc &PLoc) {
    auto Line = PLoc.getLine();
    auto Column = PLoc.getColumn();
    auto Pair = std::make_pair(Line, Column);
    return DenseMapInfo<decltype(Pair)>::getHashValue(Pair);
  }
  static bool isEqual(const DILocation *LHS, const DILocation *RHS) {
    return LHS == RHS;
  }
  static bool isEqual(const PresumedLoc &LHS, const DILocation *RHS) {
    return !isEqual(RHS, getTombstoneKey()) &&
      !isEqual(RHS, getEmptyKey()) &&
      LHS.getLine() == RHS->getLine() &&
      LHS.getColumn() == RHS->getColumn() &&
      LHS.getFilename() == RHS->getFilename();
  }
};

class MatchASTVisitor : public RecursiveASTVisitor<MatchASTVisitor> {
public:
  typedef DenseMap<DILocation *, Loop *, DILocationMapInfo> LocToLoopMap;

  MatchASTVisitor(LoopMatcherPass::LoopMatcher &LM,
      const LocToLoopMap &LocMap, SourceManager &SrcMgr) :
    mMatcher(&LM), mLocToLoop(&LocMap), mSrcMgr(&SrcMgr) {}

  bool VisitStmt(Stmt *S) {
    if (!isa<ForStmt>(S))
      return true;
    ForStmt *For = cast<ForStmt>(S);
    // To determine appropriate loop in LLVM IR it is necessary to use start
    // location of initialization instruction, if it is available.
    Stmt *Init = For->getInit();
    SourceLocation Loc = Init ? Init->getLocStart() : For->getLocStart();
    if (Loop *L = findLoopForLocation(Loc))
      mMatcher->emplace(For, L);
    else
      ++NumNonMatchASTLoop;
    return true;
  }

  bool VisitFunctionDecl(FunctionDecl *F) {
    return true;
  }

private:
  /// Finds low-level representation of a loop at the specified location.
  ///
  /// \return LLVM IR for a loop or `nullptr`.
  Loop * findLoopForLocation(SourceLocation Loc) {
    if (Loc.isInvalid())
      return nullptr;
    Loc = mSrcMgr->getExpansionLoc(Loc);
    if (Loc.isInvalid())
      return nullptr;
    PresumedLoc PLoc = mSrcMgr->getPresumedLoc(Loc, false);
    auto LocItr = mLocToLoop->find_as(PLoc);
    if (LocItr == mLocToLoop->end())
      return nullptr;
    return LocItr->second;
  }

  LoopMatcherPass::LoopMatcher *mMatcher;
  const LocToLoopMap *mLocToLoop;
  SourceManager *mSrcMgr;
};
}

bool LoopMatcherPass::runOnFunction(Function &F) {
  auto M = F.getParent();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  mFuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!mFuncDecl)
    return false;
  auto &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  MatchASTVisitor::LocToLoopMap LocToLoop;
  for_each(LpInfo, [&LocToLoop](Loop *L) {
    auto Loc = L->getStartLoc().get();
    if (!Loc)
      ++NumNonMatchIRLoop;
    else
      LocToLoop.insert(std::make_pair(Loc, L));
  });
  auto &SrcMgr = TfmCtx->getRewriter().getSourceMgr();
  MatchASTVisitor MatchVisitor(mMatcher, LocToLoop, SrcMgr);
  MatchVisitor.TraverseDecl(mFuncDecl);
  return false;
}

void LoopMatcherPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createLoopMatcherPass() {
  return new LoopMatcherPass();
}