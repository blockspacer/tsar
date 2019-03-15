#include "tsar_array_subscript_delinearize.h"
#include "DelinearizeJSON.h"
#include "KnownFunctionTraits.h"
#include "MemoryAccessUtils.h"
#include "tsar_query.h"
#include "tsar_utility.h"
#include "tsar/Support/SCEVUtils.h"
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Transforms/Utils/Local.h>
#include <bcl/Json.h>
#include <utility>
#include <cmath>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "delinearize"

char DelinearizationPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(DelinearizationPass, "delinearize",
  "Array Access Delinearizer", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(DelinearizationPass, "delinearize",
  "Array Access Delinearizer", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

STATISTIC(NumDelinearizedSubscripts, "Number of delinearized subscripts");

namespace {
/// Traverse a SCEV and simplifies it to a binomial if possible. The result is
/// `Coef * Count + FreeTerm`, where `Count` is and induction variable for L.
/// `IsSafeCast` will be set to `false` if some unsafe casts are necessary for
/// simplification.
struct SCEVBionmialSearch : public SCEVVisitor<SCEVBionmialSearch, void> {
  ScalarEvolution *mSE = nullptr;
  const SCEV * Coef = nullptr;
  const SCEV * FreeTerm = nullptr;
  const Loop * L = nullptr;
  bool IsSafeCast = true;

  SCEVBionmialSearch(ScalarEvolution &SE) : mSE(&SE) {}

  void visitTruncateExpr(const SCEVTruncateExpr *S) {
    IsSafeCast = false;
    visit(S->getOperand());
    if (Coef)
      Coef = mSE->getTruncateExpr(Coef, S->getType());
    if (FreeTerm)
      FreeTerm = mSE->getTruncateExpr(FreeTerm, S->getType());
  }

  void visitSignExtendExpr(const SCEVSignExtendExpr *S) {
    IsSafeCast = false;
    visit(S->getOperand());
    if (Coef)
      Coef = mSE->getSignExtendExpr(Coef, S->getType());
    if (FreeTerm)
      FreeTerm = mSE->getSignExtendExpr(FreeTerm, S->getType());
  }

  void visitZeroExtendExpr(const SCEVZeroExtendExpr *S) {
    IsSafeCast = false;
    visit(S->getOperand());
    if (Coef)
      Coef = mSE->getZeroExtendExpr(Coef, S->getType());
    if (FreeTerm)
      FreeTerm = mSE->getZeroExtendExpr(FreeTerm, S->getType());
  }

  void visitAddRecExpr(const SCEVAddRecExpr *S) {
    L = S->getLoop();
    Coef = S->getStepRecurrence(*mSE);
    FreeTerm = S->getStart();
  }

  void visitMulExpr(const SCEVMulExpr *S) {
    assert(!L && "Loop must not be set yet!");
    auto OpI = S->op_begin(), OpEI = S->op_end();
    SmallVector<const SCEV *, 4> MulFreeTerm;
    for (; OpI != OpEI; ++OpI) {
      visit(*OpI);
      if (L)
        break;
      MulFreeTerm.push_back(*OpI);
    }
    if (L) {
      MulFreeTerm.append(++OpI, OpEI);
      auto MulCoef = MulFreeTerm;
      MulFreeTerm.push_back(FreeTerm);
      // Note, that getMulExpr() may change order of SCEVs in it's parameter.
      FreeTerm = mSE->getMulExpr(MulFreeTerm);
      MulCoef.push_back(Coef);
      Coef = mSE->getMulExpr(MulCoef);
    } else {
      FreeTerm = S;
    }
  }

  void visitAddExpr(const SCEVAddExpr *S) {
    assert(!L && "Loop must not be set yet!");
    auto OpI = S->op_begin(), OpEI = S->op_end();
    SmallVector<const SCEV *, 4> Terms;
    for (; OpI != OpEI; ++OpI) {
      visit(*OpI);
      if (L)
        break;
      Terms.push_back(*OpI);
    }
    if (L) {
      Terms.append(++OpI, OpEI);
      Terms.push_back(FreeTerm);
      FreeTerm = mSE->getAddExpr(Terms);
    } else {
      FreeTerm = S;
    }
  }

  void visitConstant(const SCEVConstant *S) { FreeTerm = S; }
  void visitUDivExpr(const SCEVUDivExpr *S) { FreeTerm = S; }
  void visitSMaxExpr(const SCEVSMaxExpr *S) { FreeTerm = S; }
  void visitUMaxExpr(const SCEVUMaxExpr *S) { FreeTerm = S; }
  void visitUnknown(const SCEVUnknown *S) { FreeTerm = S; }
  void visitCouldNotCompute(const SCEVCouldNotCompute *S) { FreeTerm = S; }
};
}

std::pair<const SCEV *, bool> tsar::computeSCEVAddRec(
    const SCEV *Expr, llvm::ScalarEvolution &SE) {
  SCEVBionmialSearch Search(SE);
  Search.visit(Expr);
  bool IsSafe = true;
  if (Search.L) {
    Expr = SE.getAddRecExpr(
      Search.FreeTerm, Search.Coef, Search.L, SCEV::FlagAnyWrap);
    IsSafe = Search.IsSafeCast;
  }
  return std::make_pair(Expr, IsSafe);
}

std::pair<const Array *, const Array::Element *>
DelinearizeInfo::findElement(const Value *ElementPtr) const {
  auto Itr = mElements.find(ElementPtr);
  if (Itr != mElements.end()) {
    auto *TargetArray = Itr->getArray();
    auto *TargetElement = TargetArray->getElement(Itr->getElementIdx());
    return std::make_pair(TargetArray, TargetElement);
  }
  return std::make_pair(nullptr, nullptr);
}

void DelinearizeInfo::fillElementsMap() {
  mElements.clear();
  for (auto &ArrayEntry : mArrays) {
    int Idx = 0;
    for (auto Itr = ArrayEntry->begin(), ItrE = ArrayEntry->end();
         Itr != ItrE; ++Itr, ++Idx)
      mElements.try_emplace(Itr->Ptr, ArrayEntry, Idx);
  }
}

namespace {
template<class GEPItrT>
void extractSubscriptsFromGEPs(
    const GEPItrT &GEPBeginItr, const GEPItrT &GEPEndItr,
    SmallVectorImpl<Value *> &Idxs) {
  for (auto *GEP : make_range(GEPBeginItr, GEPEndItr)) {
    unsigned NumOperands = GEP->getNumOperands();
    if (NumOperands == 2) {
      Idxs.push_back(GEP->getOperand(1));
    } else {
      if (auto *SecondOp = dyn_cast<Constant>(GEP->getOperand(1))) {
        if (!SecondOp->isZeroValue())
          Idxs.push_back(GEP->getOperand(1));
      } else {
        Idxs.push_back(GEP->getOperand(1));
      }
      for (unsigned I = 2; I < NumOperands; ++I) {
        Idxs.push_back(GEP->getOperand(I));
      }
    }
  }
}

void countPrimeNumbers(uint64_t Bound, std::vector<uint64_t> &Primes) {
  //Implements Sieve of Atkin with cache

  enum {
    PRIMES_CACHE_SIZE = 60
  };

  static uint64_t CachedPrimes[PRIMES_CACHE_SIZE] = {
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59,
    61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131,
    137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191, 193, 197,
    199, 211, 223, 227, 229, 233, 239, 241, 251, 257, 263, 269, 271,
    277, 281
  };

  Primes.clear();

  if (Bound <= CachedPrimes[PRIMES_CACHE_SIZE - 1]) {
    for (int i = 0; i < PRIMES_CACHE_SIZE; i++) {
      if (CachedPrimes[i] <= Bound) {
        Primes.push_back(CachedPrimes[i]);
      } else {
        break;
      }
    }
    return;
  }

  std::vector<bool> IsPrime;
  IsPrime.resize(Bound + 1);
  for (int i = 0; i <= Bound; i++) {
    IsPrime[i] = false;
  }
  IsPrime[2] = true;
  IsPrime[3] = true;
  uint64_t BoundSqrt = (int)sqrt(Bound);

  uint64_t x2 = 0, y2, n;
  for (int i = 1; i <= BoundSqrt; i++) {
    x2 += 2 * i - 1;
    y2 = 0;
    for (int j = 1; j <= BoundSqrt; j++) {
      y2 += 2 * j - 1;
      n = 4 * x2 + y2;
      if ((n <= Bound) && (n % 12 == 1 || n % 12 == 5)) {
        IsPrime[n] = !IsPrime[n];
      }

      n -= x2;
      if ((n <= Bound) && (n % 12 == 7)) {
        IsPrime[n] = !IsPrime[n];
      }

      n -= 2 * y2;
      if ((i > j) && (n <= Bound) && (n % 12 == 11)) {
        IsPrime[n] = !IsPrime[n];
      }
    }
  }

  for (int i = 5; i <= BoundSqrt; i++) {
    if (IsPrime[i]) {
      n = i * i;
      for (uint64_t j = n; j <= Bound; j += n) {
        IsPrime[j] = false;
      }
    }
  }

  Primes.push_back(2);
  Primes.push_back(3);
  Primes.push_back(5);
  for (int i = 6; i <= Bound; i++) {
    if ((IsPrime[i]) && (i % 3) && (i % 5)) {
      Primes.push_back(i);
    }
  }
}

void countConstantMultipliers(const SCEVConstant *Const,
  ScalarEvolution &SE, SmallVectorImpl<const SCEV *> &Multipliers) {
  uint64_t ConstValue = Const->getAPInt().getLimitedValue();
  assert(ConstValue && "Constant value is zero");

  if (ConstValue < 0) {
    Multipliers.push_back(SE.getConstant(Const->getType(), -1, true));
    ConstValue *= -1;
  }
  if (ConstValue == 1) {
    Multipliers.push_back(SE.getConstant(Const->getType(), 1, false));
    return;
  }
  std::vector<uint64_t> Primes;
  countPrimeNumbers(ConstValue, Primes);
  size_t i = Primes.size() - 1;
  LLVM_DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Constant Multipliers:\n";
  );
  while (ConstValue > 1) {
    if (ConstValue % Primes[i] == 0) {
      Multipliers.push_back(SE.getConstant(Const->getType(), Primes[i], false));
      LLVM_DEBUG(
        dbgs() << "\t";
        Multipliers[Multipliers.size() - 1]->dump();
      );
      ConstValue /= Primes[i];
    } else {
      i--;
    }
  }
}

const SCEV* findGCD(SmallVectorImpl<const SCEV *> &Expressions,
  ScalarEvolution &SE) {
  assert(!Expressions.empty() && "GCD Expressions size must not be zero");

  SmallVector<const SCEV *, 3> Terms;

  //Release AddRec Expressions, multipliers are in step and start expressions
  for (auto *Expr : Expressions) {
    switch (Expr->getSCEVType()) {
    case scTruncate:
    case scZeroExtend:
    case scSignExtend: {
      auto *CastExpr = cast<SCEVCastExpr>(Expr);
      auto *InnerOp = CastExpr->getOperand();
      switch (InnerOp->getSCEVType()) {
      case scAddRecExpr: {
        auto *AddRec = cast<SCEVAddRecExpr>(InnerOp);
        auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
        auto *AddRecStart = AddRec->getStart();
        if (Expr->getSCEVType() == scTruncate) {
          Terms.push_back(SE.getTruncateExpr(AddRecStepRecurrence, Expr->getType()));
          Terms.push_back(SE.getTruncateExpr(AddRecStart, Expr->getType()));
        }
        if (Expr->getSCEVType() == scSignExtend) {
          Terms.push_back(SE.getSignExtendExpr(AddRecStepRecurrence, Expr->getType()));
          Terms.push_back(SE.getSignExtendExpr(AddRecStart, Expr->getType()));
        }
        if (Expr->getSCEVType() == scZeroExtend) {
          Terms.push_back(SE.getZeroExtendExpr(AddRecStepRecurrence, Expr->getType()));
          Terms.push_back(SE.getZeroExtendExpr(AddRecStart, Expr->getType()));
        }
        break;
      }
      case scUnknown:
      case scAddExpr:
      case scMulExpr: {
        Terms.push_back(Expr);
        break;
      }
      }
      break;
    }
    case scConstant:
    case scUnknown:
    case scAddExpr: {
      Terms.push_back(Expr);
      break;
    }
    case scMulExpr: {
      auto *MulExpr = cast<SCEVMulExpr>(Expr);
      bool hasAddRec = false;
      SmallVector<const SCEV *, 3> StepMultipliers, StartMultipliers;
      for (int i = 0; i < MulExpr->getNumOperands(); ++i) {
        auto *Op = MulExpr->getOperand(i);
        switch (Op->getSCEVType()) {
        case scTruncate:
        case scZeroExtend:
        case scSignExtend: {
          auto *InnerOp = cast<SCEVCastExpr>(Op)->getOperand();

          if (auto *AddRec = dyn_cast<SCEVAddRecExpr>(InnerOp)) {
            hasAddRec = true;
            auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
            auto *AddRecStart = AddRec->getStart();
            if (Op->getSCEVType() == scTruncate) {
              StepMultipliers.push_back(SE.getTruncateExpr(AddRecStepRecurrence, Op->getType()));
              if (!AddRecStart->isZero()) {
                StartMultipliers.push_back(SE.getTruncateExpr(AddRecStart, Op->getType()));
              }
            }
            if (Op->getSCEVType() == scSignExtend) {
              StepMultipliers.push_back(SE.getSignExtendExpr(AddRecStepRecurrence, Op->getType()));
              if (!AddRecStart->isZero()) {
                StartMultipliers.push_back(SE.getSignExtendExpr(AddRecStart, Op->getType()));
              }
            }
            if (Op->getSCEVType() == scZeroExtend) {
              StepMultipliers.push_back(SE.getZeroExtendExpr(AddRecStepRecurrence, Op->getType()));
              if (!AddRecStart->isZero()) {
                StartMultipliers.push_back(SE.getZeroExtendExpr(AddRecStart, Op->getType()));
              }
            }
          } else if (auto *InnerMulExpr = dyn_cast<SCEVMulExpr>(InnerOp)) {
            StepMultipliers.push_back(Op);
            StartMultipliers.push_back(Op);
          } else if (InnerOp->getSCEVType() == scUnknown ||
            InnerOp->getSCEVType() == scAddExpr) {
            StepMultipliers.push_back(Op);
            StartMultipliers.push_back(Op);
          }
         break;
        }
        case scAddRecExpr: {
          auto *AddRec = cast<SCEVAddRecExpr>(Op);
          hasAddRec = true;
          auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
          auto *AddRecStart = AddRec->getStart();
          StepMultipliers.push_back(AddRecStepRecurrence);
          if (!AddRecStart->isZero()) {
            StartMultipliers.push_back(AddRecStart);
          }
          break;
        }
        case scUnknown:
        case scAddExpr:
        case scConstant: {
          StepMultipliers.push_back(Op);
          StartMultipliers.push_back(Op);
          break;
        }
        }
      }
      if (hasAddRec && !StartMultipliers.empty()) {
        SmallVector<const SCEV *, 2> AddRecInnerExpressions = {
          SE.getMulExpr(StartMultipliers),
          SE.getMulExpr(StepMultipliers)
        };
        Terms.push_back(findGCD(AddRecInnerExpressions, SE));
      } else if (!StepMultipliers.empty()) {
        Terms.push_back(SE.getMulExpr(StepMultipliers));
      }
     break;
    }
    case scAddRecExpr: {
      auto *AddRec = cast<SCEVAddRecExpr>(Expr);
      auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
      /*SCEVConstant *AddRecStartConstantMultiplier = nullptr;
      SCEVConstant *AddRecStepConstantMultiplier = nullptr;*/

      if (auto *MulExpr = dyn_cast<SCEVMulExpr>(AddRecStepRecurrence)) {
        SmallVector<const SCEV *, 2> Multipliers;
        for (int i = 0; i < MulExpr->getNumOperands(); ++i) {
          auto *Op = MulExpr->getOperand(i);
          if (Op->getSCEVType() == scUnknown ||
            Op->getSCEVType() == scTruncate ||
            Op->getSCEVType() == scSignExtend ||
            Op->getSCEVType() == scZeroExtend ||
            Op->getSCEVType() == scAddExpr ||
            Op->getSCEVType() == scConstant) {
            Multipliers.push_back(Op);
          }
          //if (auto *Const = dyn_cast<SCEVConstant>(Op)) {

          //}
        }
        AddRecStepRecurrence = SE.getMulExpr(Multipliers);
      }

      auto *AddRecStart = AddRec->getStart();
      if (auto *MulExpr = dyn_cast<SCEVMulExpr>(AddRecStart)) {
        SmallVector<const SCEV *, 2> Multipliers;
        for (int i = 0; i < MulExpr->getNumOperands(); ++i) {
          auto *Op = MulExpr->getOperand(i);
          if (Op->getSCEVType() == scUnknown ||
            Op->getSCEVType() == scTruncate ||
            Op->getSCEVType() == scSignExtend ||
            Op->getSCEVType() == scZeroExtend ||
            Op->getSCEVType() == scAddExpr ||
            Op->getSCEVType() == scConstant) {
            Multipliers.push_back(Op);
          }
        }
        AddRecStart = SE.getMulExpr(Multipliers);
      }
      SmallVector<const SCEV *, 2> AddRecInnerExpressions = {AddRecStart, AddRecStepRecurrence};
      //AddRecInnerExpressions.push_back();
      Terms.push_back(findGCD(AddRecInnerExpressions, SE));
      /*Terms.push_back(AddRecStepRecurrence);
      Terms.push_back(AddRecStart);*/
      break;
    }
    }
  }

  LLVM_DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] GCD Terms:\n";
    for (auto *Term : Terms) {
      dbgs() << "\t";
      Term->dump();
    });

  if (Terms.empty()) {
    return SE.getConstant(Expressions[0]->getType(), 1, true);
  }

  SmallVector<const SCEV *, 3> Dividers;

  const SCEV* OpeningSCEV = nullptr;

  //Finding not zero SCEV in Terms
  for (auto *Term : Terms) {
    if (!Term->isZero()) {
      OpeningSCEV = Term;
      break;
    }
  }
  if (!OpeningSCEV) {
    return SE.getConstant(Expressions[0]->getType(), 0, true);
  }

  //Start from multipliers of first SCEV, then exclude them step by step
  if (auto *Mul = dyn_cast<SCEVMulExpr>(OpeningSCEV)) {
    for (int i = 0; i < Mul->getNumOperands(); ++i) {
      if (auto *Const = dyn_cast<SCEVConstant>(Mul->getOperand(i))) {
        SmallVector<const SCEV *, 3> ConstMultipliers;
        countConstantMultipliers(Const, SE, ConstMultipliers);
        Dividers.append(ConstMultipliers.begin(), ConstMultipliers.end());
      } else {
        Dividers.push_back(Mul->getOperand(i));
      }
    }
  } else {
    if (auto *Const = dyn_cast<SCEVConstant>(OpeningSCEV)) {
      SmallVector<const SCEV *, 3> ConstMultipliers;
      countConstantMultipliers(Const, SE, ConstMultipliers);
      Dividers.append(ConstMultipliers.begin(), ConstMultipliers.end());
    } else {
      Dividers.push_back(OpeningSCEV);
    }
  }

  for (int i = 1; i < Terms.size(); ++i) {
    auto *CurrentTerm = Terms[i];
    SmallVector<const SCEV *, 3> ActualStepDividers;

    for (auto *Divider : Dividers) {
      auto Div = divide(SE, CurrentTerm, Divider, false);
      if (Div.Remainder->isZero()) {
        ActualStepDividers.push_back(Divider);
        CurrentTerm = Div.Quotient;
        if (ActualStepDividers.size() == Dividers.size()) {
          break;
        }
      }
    }

    Dividers = ActualStepDividers;
    if (Dividers.size() == 0) {
      return SE.getConstant(Expressions[0]->getType(), 1, true);
    }
  }

  if (Dividers.size() == 1) {
    return Dividers[0];
  } else {
    return SE.getMulExpr(Dividers);
  }

}

#ifdef LLVM_DEBUG
void delinearizationLog(const DelinearizeInfo &Info, ScalarEvolution &SE,
    raw_ostream  &OS) {
  for (auto &ArrayInfo : Info.getArrays()) {
    OS << "[DELINEARIZE]: results for array ";
    ArrayInfo->getBase()->print(OS, true);
    OS << "\n";
    OS << "  number of dimensions: " << ArrayInfo->getNumberOfDims() << "\n";
    for (std::size_t I = 0, EI = ArrayInfo->getNumberOfDims(); I < EI; ++I) {
      OS << "    " << I << ": ";
      ArrayInfo->getDimSize(I)->print(OS);
      OS << "\n";
    }
    OS << "  accesses:\n";
    for (auto &El : *ArrayInfo) {
      OS << "    address: ";
      El.Ptr->print(OS, true);
      OS << "\n";
      for (auto *S : El.Subscripts) {
        OS << "      SCEV: ";
        S->print(OS);
        OS << "\n";
        auto Info = computeSCEVAddRec(S, SE);
        const SCEV *Coef, *ConstTerm;
        if (auto AddRec = dyn_cast<SCEVAddRecExpr>(Info.first)) {
          Coef = AddRec->getStepRecurrence(SE);
          ConstTerm = AddRec->getStart();
        } else {
          Coef = SE.getZero(Info.first->getType());
          ConstTerm = Info.first;
        }
        OS << "      a: ";
        Coef->print(OS);
        OS << "\n";
        OS << "      b: ";
        ConstTerm->print(OS);
        OS << "\n";
        if (!Info.second)
          OS << "      with unsafe cast\n";
      }
    }
  }
}
#endif
}

void DelinearizationPass::cleanSubscripts(Array &ArrayInfo) {
  assert(ArrayInfo.isDelinearized() && "Array must be delinearized!");
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: simplify subscripts for "
                    << ArrayInfo.getBase()->getName() << "\n");
  auto LastConstDim = ArrayInfo.getNumberOfDims();
  for (LastConstDim; LastConstDim > 0; --LastConstDim)
    if (!isa<SCEVConstant>(ArrayInfo.getDimSize(LastConstDim - 1)))
      break;
  if (LastConstDim == 0)
    return;
  auto *PrevDimSizesProduct = mSE->getConstant(mIndexTy, 1);
  for (auto DimIdx = LastConstDim - 1; DimIdx > 0; --DimIdx) {
    assert(ArrayInfo.isKnownDimSize(DimIdx) &&
      "Non-first unknown dimension in delinearized array!");
    PrevDimSizesProduct = mSE->getMulExpr(PrevDimSizesProduct,
      mSE->getTruncateOrZeroExtend(ArrayInfo.getDimSize(DimIdx), mIndexTy));
    for (auto &Range: ArrayInfo) {
      auto *Subscript = Range.Subscripts[DimIdx - 1];
      auto Div = divide(*mSE, Subscript, PrevDimSizesProduct, false);
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: subscript " << DimIdx - 1 << " ";
        Subscript->dump();
        dbgs() << "[DELINEARIZE]: product of sizes of previous dimensions: ";
        PrevDimSizesProduct->dump();
        dbgs() << "[DELINEARIZE]: quotient "; Div.Quotient->dump();
        dbgs() << "[DELINEARIZE]: remainder "; Div.Remainder->dump());
      if (!Div.Remainder->isZero()) {
        Range.Traits &= ~Range.IsValid;
        break;
      }
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: set subscript to ";
        Div.Quotient->dump());
      Range.Subscripts[DimIdx - 1] = Div.Quotient;
    }
  }
}

void DelinearizationPass::fillArrayDimensionsSizes(
    SmallVectorImpl<int64_t> &DimSizes, Array &ArrayInfo) {
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: compute sizes of dimensions for "
                    << ArrayInfo.getBase()->getName() << "\n");
  auto NumberOfDims = ArrayInfo.getNumberOfDims();
  auto LastUnknownDim = NumberOfDims;
  if (NumberOfDims == 0) {
    auto RangeItr = ArrayInfo.begin(), RangeItrE = ArrayInfo.end();
    for (; RangeItr != RangeItrE; ++RangeItr) {
      if (RangeItr->isElement() && RangeItr->isValid()) {
        NumberOfDims = RangeItr->Subscripts.size();
        break;
      }
    }
    if (RangeItr == RangeItrE) {
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: no valid element found\n";
        dbgs() << "[DELINEARIZE]: unable to determine number of"
        " dimensions for " << ArrayInfo.getBase()->getName() << "\n");
      return;
    }
    for (++RangeItr; RangeItr != RangeItrE; ++RangeItr)
      if (RangeItr->isElement() && RangeItr->isValid())
        if (NumberOfDims != RangeItr->Subscripts.size()) {
          LLVM_DEBUG(
            dbgs() << "[DELINEARIZE]: unable to determine number of"
            " dimensions for " << ArrayInfo.getBase()->getName() << "\n");
          return;
        }
    assert(NumberOfDims > 0 && "Scalar variable is treated as array?");
    DimSizes.resize(NumberOfDims, -1);
    ArrayInfo.setNumberOfDims(NumberOfDims);
    ArrayInfo.setDimSize(0, mSE->getCouldNotCompute());
    LastUnknownDim = NumberOfDims - 1;
    LLVM_DEBUG(
      dbgs() << "[DELINEARIZE]: extract number of dimensions from subscripts: "
             << NumberOfDims << "\n");
  } else {
    auto LastConstDim = DimSizes.size();
    for (auto I = DimSizes.size(); I > 0; --I) {
      if (DimSizes[I - 1] < 0)
        break;
      LastConstDim = I - 1;
      ArrayInfo.setDimSize(I - 1, mSE->getConstant(mIndexTy, DimSizes[I - 1]));
    }
    if (LastConstDim == 0) {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: all dimensions have constant sizes\n");
      return;
    }
    if (DimSizes[0] > 0)
      ArrayInfo.setDimSize(0, mSE->getConstant(mIndexTy, DimSizes[0]));
    else
      ArrayInfo.setDimSize(0, mSE->getCouldNotCompute());
    LastUnknownDim = LastConstDim - 1;
  }
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: compute non-constant dimension sizes\n");
  auto *PrevDimSizesProduct = mSE->getConstant(mIndexTy, 1);
  auto DimIdx = LastUnknownDim;
  for (; DimIdx > 0; --DimIdx) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: process dimension " << DimIdx << "\n");
    const SCEV *DimSize;
    if (DimSizes[DimIdx] > 0) {
      DimSize = mSE->getConstant(mIndexTy, DimSizes[DimIdx]);
    } else {
      SmallVector<const SCEV *, 3> Expressions;
      for (auto &Range: ArrayInfo) {
        if (!Range.isElement())
          continue;
        assert(Range.Subscripts.size() == NumberOfDims &&
          "Number of dimensions is inconsistent with number of subscripts!");
        for (auto J = DimIdx; J > 0; --J) {
          Expressions.push_back(Range.Subscripts[J - 1]);
          LLVM_DEBUG(dbgs() << "[DELINEARIZE]: use for GCD computation: ";
            Expressions.back()->dump());
        }
      }
      DimSize = findGCD(Expressions, *mSE);
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: GCD: "; DimSize->dump());
      auto Div = divide(*mSE, DimSize, PrevDimSizesProduct, false);
      DimSize = Div.Quotient;
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: product of sizes of previous dimensions: ";
        PrevDimSizesProduct->dump();
        dbgs() << "[DELINEARIZE]: quotient "; Div.Quotient->dump();
        dbgs() << "[DELINEARIZE]: remainder "; Div.Remainder->dump());
    }
    if (DimSize->isZero()) {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: could not compute dimension size\n");
      DimSize = mSE->getCouldNotCompute();
      ArrayInfo.setDimSize(DimIdx, DimSize);
      for (auto J : seq(decltype(DimIdx)(1), DimIdx)) {
        if (DimSizes[J] > 0)
          ArrayInfo.setDimSize(J, mSE->getConstant(mIndexTy, DimSizes[J]));
        else
          ArrayInfo.setDimSize(J, DimSize);
      }
      break;
    }
    ArrayInfo.setDimSize(DimIdx, DimSize);
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: dimension size is "; DimSize->dump());
    DimSize = mSE->getTruncateOrZeroExtend(DimSize, mIndexTy);
    PrevDimSizesProduct = mSE->getMulExpr(PrevDimSizesProduct, DimSize);
  }
  if (DimIdx == 0)
    ArrayInfo.setDelinearized();
}

void DelinearizationPass::findArrayDimesionsFromDbgInfo(
    Value *BasePtr, SmallVectorImpl<int64_t> &Dimensions) {
  assert(BasePtr && "RootArray must not be null");
  SmallVector<DIMemoryLocation, 1> DILocs;
  auto DIM = findMetadata(BasePtr, DILocs, mDT);
  if (!DIM)
    return;
  assert(DIM->isValid() && "Debug memory location must be valid!");
  if (!DIM->Var->getType())
    return;
  auto VarDbgTy = DIM->Var->getType().resolve();
  DINodeArray ArrayDims = nullptr;
  bool IsFirstDimPointer = false;
  if (VarDbgTy->getTag() == dwarf::DW_TAG_array_type) {
    ArrayDims = cast<DICompositeType>(VarDbgTy)->getElements();
  } else if (VarDbgTy->getTag() == dwarf::DW_TAG_pointer_type) {
    IsFirstDimPointer = true;
    auto BaseTy = cast<DIDerivedType>(VarDbgTy)->getBaseType();
    if (BaseTy && BaseTy.resolve()->getTag() == dwarf::DW_TAG_array_type)
      ArrayDims = cast<DICompositeType>(BaseTy)->getElements();
  }
  LLVM_DEBUG(
    dbgs() << "[DELINEARIZE]: number of array dimensions for "
           << BasePtr->getName() << " is ";
    dbgs() << (ArrayDims.size() + (IsFirstDimPointer ? 1 : 0)) << "\n");
  if (IsFirstDimPointer) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: first dimension is pointer\n");
    Dimensions.push_back(-1);
  }
  if (!ArrayDims)
    return;
  Dimensions.reserve(ArrayDims.size() + IsFirstDimPointer ? 1 : 0);
  for (unsigned int DimIdx = 0; DimIdx < ArrayDims.size(); ++DimIdx) {
    LLVM_DEBUG(dbgs() << "[DELINEARIZE]: size of " << DimIdx << " dimension is ");
    int64_t DimSize = -1;
    if (auto *DIDim = dyn_cast<DISubrange>(ArrayDims[DimIdx])) {
      auto DIDimCount = DIDim->getCount();
      if (DIDimCount.is<ConstantInt*>()) {
        auto Count = DIDimCount.get<ConstantInt *>()->getValue();
        if (Count.getMinSignedBits() <= 64)
          DimSize = Count.getSExtValue();
        LLVM_DEBUG(dbgs() << DimSize << "\n");
      } else if (DIDimCount.is<DIVariable *>()) {
        LLVM_DEBUG(dbgs() << DIDimCount.get<DIVariable *>()->getName() << "\n");
      } else {
        LLVM_DEBUG( dbgs() << "unknown\n");
      }
    }
    Dimensions.push_back(DimSize);
  }
}

void DelinearizationPass::collectArrays(Function &F, DimensionMap &DimsCache) {
  for (auto &I : instructions(F)) {
    auto processMemory = [this, &DimsCache](Instruction &I, MemoryLocation Loc,
        unsigned,  AccessInfo, AccessInfo) {
      if (auto II = dyn_cast<IntrinsicInst>(&I)) {
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
            isDbgInfoIntrinsic(II->getIntrinsicID()))
          return;
      }
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: process instruction "; I.dump());
      auto &DL = I.getModule()->getDataLayout();
      auto *BasePtr = const_cast<Value *>(Loc.Ptr);
      BasePtr = GetUnderlyingObjectWithMetadata(BasePtr, DL);
      if (auto *LI = dyn_cast<LoadInst>(BasePtr))
        BasePtr = LI->getPointerOperand();
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: strip to base " << *BasePtr << "\n");
      auto DimsInfo = DimsCache.try_emplace(BasePtr);
      auto DimsItr = DimsInfo.first;
      if (DimsInfo.second)
        findArrayDimesionsFromDbgInfo(BasePtr, DimsItr->second);
      auto NumberOfDims = DimsItr->second.size();
      SmallVector<GEPOperator *, 3> GEPs;
      auto *GEP = dyn_cast<GEPOperator>(const_cast<Value *>(Loc.Ptr));
      while (GEP && (NumberOfDims == 0 || GEPs.size() < NumberOfDims)) {
        GEPs.push_back(GEP);
        GEP = dyn_cast<GEPOperator>(GEP->getPointerOperand());
      }
      SmallVector<Value *, 3> SubscriptValues;
      extractSubscriptsFromGEPs(GEPs.rbegin(), GEPs.rend(), SubscriptValues);
      auto ArrayItr = mDelinearizeInfo.getArrays().find_as(BasePtr);
      if (ArrayItr == mDelinearizeInfo.getArrays().end()) {
        ArrayItr = mDelinearizeInfo.getArrays().insert(new Array(BasePtr)).first;
        (*ArrayItr)->setNumberOfDims(NumberOfDims);
      }
      assert((*ArrayItr)->getNumberOfDims() == NumberOfDims &&
        "Inconsistent number of dimensions!");
      auto &El = (*ArrayItr)->addElement(
        GEPs.empty() ? const_cast<Value *>(Loc.Ptr) : GEPs.back());
      // In some cases zero subscript is dropping out by optimization passes.
      // So, we add extra zero subscripts at the beginning of subscript list.
      // We add subscripts for instructions which access a single element,
      // for example, in case of call it is possible to pass a whole array
      // as a parameter (without GEPs).
      // TODO (kaniandr@gmail.com): we could add subscripts not only at the
      // beginning of the list, try to implement smart extra subscript insertion.
      if (isa<LoadInst>(I) || isa<StoreInst>(I) ||
          isa<AtomicRMWInst>(I) || isa<AtomicCmpXchgInst>(I)) {
        El.Traits |= Array::Element::IsElement;
        if (SubscriptValues.size() < NumberOfDims) {
          (*ArrayItr)->setRangeRef();
          for (std::size_t Idx = 0, IdxE = NumberOfDims - SubscriptValues.size();
               Idx < IdxE; Idx++) {
            El.Subscripts.push_back(mSE->getZero(mIndexTy));
            LLVM_DEBUG(dbgs() << "[DELINEARIZE]: add extra zero subscript\n");
          }
        } else {
          El.Traits |= Array::Element::IsValid;
        }
      } else {
        El.Traits |= Array::Element::IsValid;
      }
      if (!SubscriptValues.empty()) {
        (*ArrayItr)->setRangeRef();
        for (auto *V : SubscriptValues)
          El.Subscripts.push_back(mSE->getSCEV(V));
      }
      LLVM_DEBUG(
        dbgs() << "[DELINEARIZE]: number of dimensions "
               << NumberOfDims << "\n";
        dbgs() << "[DELINEARIZE]: number of subscripts "
               << El.Subscripts.size() << "\n";
        dbgs() << "[DELINEARIZE]: element is "
               << (El.IsValid ? "valid" : "invalid") << "\n";
        dbgs() << "[DELINEARIZE]: subscripts: \n";
        for (auto *Subscript : El.Subscripts) {
          dbgs() << "  "; Subscript->dump();
        }
      );
    };
    for_each_memory(I, *mTLI, processMemory,
      [](Instruction &, AccessInfo, AccessInfo) {});
  }
  // Now, we remove all object which is not arrays.
  for (auto Itr = mDelinearizeInfo.getArrays().begin(),
       ItrE = mDelinearizeInfo.getArrays().end(); Itr != ItrE;) {
    auto CurrItr = Itr++;
    if ((*CurrItr)->getNumberOfDims() == 0 && !(*CurrItr)->hasRangeRef()) {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: not an array "
                        << (*CurrItr)->getBase()->getName() << "\n");
      DimsCache.erase((*CurrItr)->getBase());
      mDelinearizeInfo.getArrays().erase(CurrItr);
    }
  }
}

void DelinearizationPass::findSubscripts(Function &F) {
  DimensionMap DimsCache;
  collectArrays(F, DimsCache);
  for (auto *ArrayInfo : mDelinearizeInfo.getArrays()) {
    auto DimsItr = DimsCache.find(ArrayInfo->getBase());
    assert(DimsItr != DimsCache.end() &&
      "Cache of dimension sizes must be constructed!");
    fillArrayDimensionsSizes(DimsItr->second, *ArrayInfo);
    if (ArrayInfo->isDelinearized()) {
      cleanSubscripts(*ArrayInfo);
    } else {
      LLVM_DEBUG(dbgs() << "[DELINEARIZE]: unable to delinearize "
                        << DimsItr->first->getName() << "\n");
    }
  }
}

bool DelinearizationPass::runOnFunction(Function &F) {
  LLVM_DEBUG(
    dbgs() << "[DELINEARIZE]: process function " << F.getName() << "\n");
  releaseMemory();
  mDT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  mSE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  mTLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  auto &DL = F.getParent()->getDataLayout();
  mIndexTy = DL.getIndexType(Type::getInt8PtrTy(F.getContext()));
  LLVM_DEBUG(dbgs() << "[DELINEARIZE]: index type is "; mIndexTy->dump());
  findSubscripts(F);
  mDelinearizeInfo.fillElementsMap();
  LLVM_DEBUG(delinearizationLog(mDelinearizeInfo, *mSE, dbgs()));
  return false;
}

void DelinearizationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.setPreservesAll();
}

void DelinearizationPass::print(raw_ostream &OS, const Module *M) const {
  auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  RawDelinearizeInfo Info = tsar::toJSON(mDelinearizeInfo, SE);
  OS << json::Parser<RawDelinearizeInfo>::unparse(Info) << '\n';
}

FunctionPass * createDelinearizationPass() { return new DelinearizationPass; }

RawDelinearizeInfo tsar::toJSON(const DelinearizeInfo &Info, ScalarEvolution &SE) {
  RawDelinearizeInfo RawInfo;
  for (auto &ArrayInfo : Info.getArrays()) {
    std::string NameStr;
    raw_string_ostream NameOS(NameStr);
    NameOS.flush();
    ArrayInfo->getBase()->print(NameOS);
    std::vector<std::string> DimSizes(ArrayInfo->getNumberOfDims());
    for (std::size_t I = 0, EI = DimSizes.size(); I < EI; ++I) {
      raw_string_ostream DimSizeOS(DimSizes[I]);
      ArrayInfo->getDimSize(I)->print(DimSizeOS);
      DimSizeOS.flush();
    }
    std::vector<std::vector<std::vector<std::string>>> Accesses;
    for (auto &El : *ArrayInfo) {
      std::vector<std::vector<std::string>> Subscripts;
      for (auto *S : El.Subscripts) {
        Subscripts.emplace_back(2);
        auto &CoefStr = Subscripts.back().front();
        auto &ConstTermStr = Subscripts.back().back();
        auto Info = computeSCEVAddRec(S, SE);
        const SCEV *Coef, *ConstTerm;
        if (auto AddRec = dyn_cast<SCEVAddRecExpr>(Info.first)) {
          Coef = AddRec->getStepRecurrence(SE);
          ConstTerm = AddRec->getStart();
        } else {
          Coef = SE.getZero(Info.first->getType());
          ConstTerm = Info.first;
        }
        raw_string_ostream CoefOS(CoefStr);
        Coef->print(CoefOS);
        CoefOS.flush();
        raw_string_ostream ConstTermOS(ConstTermStr);
        ConstTerm->print(ConstTermOS);
        ConstTermOS.flush();
      }
      Accesses.push_back(std::move(Subscripts));
    }
    RawInfo[RawDelinearizeInfo::Sizes].emplace(NameStr, std::move(DimSizes));
    RawInfo[RawDelinearizeInfo::Accesses].emplace(
      std::move(NameStr), std::move(Accesses));
  }
  return RawInfo;
}

