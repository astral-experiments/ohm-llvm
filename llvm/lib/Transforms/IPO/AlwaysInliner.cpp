//===- AlwaysInliner.cpp - Code to inline always_inline functions ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a custom inliner that handles only functions that
// are marked as "always inline".
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/InlineAdvisor.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

#define DEBUG_TYPE "inline"

namespace {

bool AlwaysInlineImpl(
    Module &M, bool InsertLifetime, ProfileSummaryInfo &PSI,
    FunctionAnalysisManager *FAM,
    function_ref<AssumptionCache &(Function &)> GetAssumptionCache,
    function_ref<AAResults &(Function &)> GetAAR,
    function_ref<TargetTransformInfo &(Function &)> GetTTI,
    function_ref<const TargetLibraryInfo &(Function &)> GetTLI,
    unsigned MaxInstructions = 0) {
  auto IsSmall = [MaxInstructions](const Function &F) {
    if (!MaxInstructions || F.isInterposable() ||
        F.getInstructionCount() > MaxInstructions)
      return false;
    // Leave stack growth to the regular cost model. A single alloca can
    // substantially increase the size of every recursive caller frame.
    return none_of(F.args(),
                   [](const Argument &A) { return A.hasByValAttr(); }) &&
           none_of(instructions(F),
                   [](const Instruction &I) { return isa<AllocaInst>(I); });
  };
  DenseMap<Function *, unsigned> ExtraCalls;
  SmallSetVector<CallBase *, 16> Calls;
  bool Changed = false;
  SmallVector<Function *, 16> InlinedComdatFunctions;
  SmallVector<Function *, 4> NeedFlattening;

  auto TryInline = [&](CallBase &CB, Function &Callee,
                       OptimizationRemarkEmitter &ORE, const char *InlineReason,
                       SmallVectorImpl<CallBase *> *NewCallSites =
                           nullptr) -> bool {
    Function *Caller = CB.getCaller();
    DebugLoc DLoc = CB.getDebugLoc();
    BasicBlock *Block = CB.getParent();

    TargetTransformInfo &CalleeTTI = GetTTI(Callee);
    std::optional<InlineResult> CanInlineWithAttributes =
        getAttributeBasedInliningDecision(CB, &Callee, CalleeTTI, GetTLI);
    if (!CanInlineWithAttributes && IsSmall(Callee) && Caller->size() < 256 &&
        ExtraCalls.lookup(Caller) < MaxInstructions &&
        AttributeFuncs::areInlineCompatible(*Caller, Callee))
      CanInlineWithAttributes = InlineResult::success();
    if (!CanInlineWithAttributes || !CanInlineWithAttributes->isSuccess()) {
      ORE.emit([&]() {
        return OptimizationRemarkMissed(DEBUG_TYPE, "NotInlined", DLoc, Block)
               << "'" << ore::NV("Callee", &Callee) << ", is not inlined into"
               << ore::NV("Caller", Caller) << "': "
               << ore::NV("Reason",
                          CanInlineWithAttributes.has_value()
                              ? CanInlineWithAttributes->getFailureReason()
                              : "due to incompatible function attributes");
      });
      return false;
    }

    InlineFunctionInfo IFI(GetAssumptionCache, &PSI);
    InlineResult Res = InlineFunction(
        CB, IFI, /*MergeAttributes=*/true, &GetAAR(Callee), InsertLifetime,
        /*TrackInlineHistory=*/NewCallSites != nullptr);
    if (!Res.isSuccess()) {
      ORE.emit([&]() {
        return OptimizationRemarkMissed(DEBUG_TYPE, "NotInlined", DLoc, Block)
               << "'" << ore::NV("Callee", &Callee) << "' is not inlined into '"
               << ore::NV("Caller", Caller)
               << "': " << ore::NV("Reason", Res.getFailureReason());
      });
      return false;
    }

    // Later callees see the calls cloned by earlier inlines. Bound their
    // growth even when every layer fits in a single basic block.
    if (MaxInstructions && IFI.InlinedCallSites.size() > 1)
      ExtraCalls[Caller] += IFI.InlinedCallSites.size() - 1;

    emitInlinedIntoBasedOnCost(ORE, DLoc, Block, Callee, *Caller,
                               InlineCost::getAlways(InlineReason),
                               /*ForProfileContext=*/false, DEBUG_TYPE);
    if (FAM)
      FAM->invalidate(*Caller, PreservedAnalyses::none());
    if (NewCallSites)
      *NewCallSites = std::move(IFI.InlinedCallSites);
    return true;
  };

  for (Function &F : make_early_inc_range(M)) {
    if (F.hasFnAttribute(Attribute::Flatten))
      NeedFlattening.push_back(&F);

    if (F.isPresplitCoroutine())
      continue;

    if (F.isDeclaration() || !isInlineViable(F).isSuccess())
      continue;

    bool Small = IsSmall(F);
    Calls.clear();

    for (User *U : F.users())
      if (auto *CB = dyn_cast<CallBase>(U))
        if (CB->getCalledFunction() == &F &&
            (CB->hasFnAttr(Attribute::AlwaysInline) ||
             (Small && !CB->isMustTailCall())) &&
            !CB->getAttributes().hasFnAttr(Attribute::NoInline))
          Calls.insert(CB);

    for (CallBase *CB : Calls) {
      OptimizationRemarkEmitter ORE(CB->getCaller());
      Changed |= TryInline(*CB, F, ORE,
                           CB->hasFnAttr(Attribute::AlwaysInline)
                               ? "always inline attribute"
                               : "small callee");
    }

    F.removeDeadConstantUsers();
    if ((F.hasFnAttribute(Attribute::AlwaysInline) ||
         (Small && !F.hasFnAttribute(Attribute::Flatten))) &&
        F.isDefTriviallyDead()) {
      if (F.hasComdat()) {
        InlinedComdatFunctions.push_back(&F);
      } else {
        if (FAM)
          FAM->clear(F, F.getName());
        M.getFunctionList().erase(F);
        Changed = true;
      }
    }
  }

  // Flatten functions with the flatten attribute using a local worklist.
  for (Function *F : NeedFlattening) {
    SmallVector<std::pair<CallBase *, int>, 16> Worklist;
    SmallVector<std::pair<Function *, int>, 16> InlineHistory;
    SmallVector<CallBase *> NewCallSites;
    OptimizationRemarkEmitter ORE(F);

    // Collect initial calls.
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          Function *Callee = CB->getCalledFunction();
          if (!Callee || Callee->isDeclaration())
            continue;
          Worklist.push_back({CB, -1});
        }
      }
    }

    while (!Worklist.empty()) {
      auto Item = Worklist.pop_back_val();
      CallBase *CB = Item.first;
      int InlineHistoryID = Item.second;
      Function *Callee = CB->getCalledFunction();
      if (!Callee)
        continue;

      // Detect recursion.
      if (Callee == F) {
        ORE.emit([&]() {
          return OptimizationRemarkMissed("inline", "NotInlined",
                                          CB->getDebugLoc(), CB->getParent())
                 << "'" << ore::NV("Callee", Callee)
                 << "' is not inlined into '"
                 << ore::NV("Caller", CB->getCaller())
                 << "': recursive call during flattening";
        });
        continue;
      }

      // Use getAttributeBasedInliningDecision for all attribute-based checks
      // including TTI/TLI compatibility and isInlineViable.
      TargetTransformInfo &CalleeTTI = GetTTI(*Callee);
      auto Decision =
          getAttributeBasedInliningDecision(*CB, Callee, CalleeTTI, GetTLI);
      if (!Decision || !Decision->isSuccess())
        continue;

      if (!TryInline(*CB, *Callee, ORE, "flatten attribute", &NewCallSites))
        continue;

      Changed = true;

      // Add new call sites from the inlined function to the worklist.
      if (!NewCallSites.empty()) {
        int NewHistoryID = InlineHistory.size();
        InlineHistory.push_back({Callee, InlineHistoryID});
        for (CallBase *NewCB : NewCallSites) {
          Function *NewCallee = NewCB->getCalledFunction();
          if (NewCallee && !NewCallee->isDeclaration())
            Worklist.push_back({NewCB, NewHistoryID});
        }
      }
    }
  }

  if (!InlinedComdatFunctions.empty()) {
    // Now we just have the comdat functions. Filter out the ones whose comdats
    // are not actually dead.
    filterDeadComdatFunctions(InlinedComdatFunctions);
    // The remaining functions are actually dead.
    for (Function *F : InlinedComdatFunctions) {
      if (FAM)
        FAM->clear(*F, F->getName());
      M.getFunctionList().erase(F);
      Changed = true;
    }
  }

  return Changed;
}

struct AlwaysInlinerLegacyPass : public ModulePass {
  bool InsertLifetime;

  AlwaysInlinerLegacyPass()
      : AlwaysInlinerLegacyPass(/*InsertLifetime*/ true) {}

  AlwaysInlinerLegacyPass(bool InsertLifetime)
      : ModulePass(ID), InsertLifetime(InsertLifetime) {}

  /// Main run interface method.
  bool runOnModule(Module &M) override {

    auto &PSI = getAnalysis<ProfileSummaryInfoWrapperPass>().getPSI();
    auto GetAAR = [&](Function &F) -> AAResults & {
      return getAnalysis<AAResultsWrapperPass>(F).getAAResults();
    };
    auto GetAssumptionCache = [&](Function &F) -> AssumptionCache & {
      return getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
    };
    auto GetTTI = [&](Function &F) -> TargetTransformInfo & {
      return getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
    };
    auto GetTLI = [&](Function &F) -> const TargetLibraryInfo & {
      return getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
    };

    return AlwaysInlineImpl(M, InsertLifetime, PSI, /*FAM=*/nullptr,
                            GetAssumptionCache, GetAAR, GetTTI, GetTLI);
  }

  static char ID; // Pass identification, replacement for typeid

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AssumptionCacheTracker>();
    AU.addRequired<AAResultsWrapperPass>();
    AU.addRequired<ProfileSummaryInfoWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
  }
};

} // namespace

char AlwaysInlinerLegacyPass::ID = 0;
INITIALIZE_PASS_BEGIN(AlwaysInlinerLegacyPass, "always-inline",
                      "Inliner for always_inline functions", false, false)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_END(AlwaysInlinerLegacyPass, "always-inline",
                    "Inliner for always_inline functions", false, false)

Pass *llvm::createAlwaysInlinerLegacyPass(bool InsertLifetime) {
  return new AlwaysInlinerLegacyPass(InsertLifetime);
}

PreservedAnalyses AlwaysInlinerPass::run(Module &M,
                                         ModuleAnalysisManager &MAM) {
  return runWithSmallCallees(M, MAM, /*MaxInstructions=*/0);
}

PreservedAnalyses
AlwaysInlinerPass::runWithSmallCallees(Module &M, ModuleAnalysisManager &MAM,
                                       unsigned MaxInstructions) {
  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  auto GetAssumptionCache = [&](Function &F) -> AssumptionCache & {
    return FAM.getResult<AssumptionAnalysis>(F);
  };
  auto GetAAR = [&](Function &F) -> AAResults & {
    return FAM.getResult<AAManager>(F);
  };
  auto GetTTI = [&](Function &F) -> TargetTransformInfo & {
    return FAM.getResult<TargetIRAnalysis>(F);
  };
  auto GetTLI = [&](Function &F) -> const TargetLibraryInfo & {
    return FAM.getResult<TargetLibraryAnalysis>(F);
  };
  auto &PSI = MAM.getResult<ProfileSummaryAnalysis>(M);

  bool Changed =
      AlwaysInlineImpl(M, InsertLifetime, PSI, &FAM, GetAssumptionCache, GetAAR,
                       GetTTI, GetTLI, MaxInstructions);
  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  // We have already invalidated all analyses on modified functions.
  PA.preserveSet<AllAnalysesOn<Function>>();
  return PA;
}
