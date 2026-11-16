// RacerProbePass: inserts accelerator-preemption probe points at loop headers.
//
// A probe point loads the thread-local pointer `preempt_signal`, reads the
// byte it points to (the status byte of an accelerator completion record) and
// calls `do_yield()` when the byte is nonzero. The RACER runtime sets
// `preempt_signal` before it switches to a filler fiber; `do_yield()` switches
// back to the scheduler. These two symbol names are the names the RACER
// applications already define (src/*.cpp), so probed libraries link into them
// unchanged.
//
// The pass runs at the end of the optimization pipeline, in two phases:
//
//   1. Unroll phase. An innermost loop whose body has fewer IR instructions
//      than the probe interval is partially unrolled by
//      interval / body_instruction_count, so that one probe covers about
//      `racer-probe-interval` IR instructions. Each unrolled copy keeps its
//      exit test, so no runtime remainder loop is made.
//   2. Probe phase. Every loop header gets one probe point.
//
// With `-racer-probe-interval=0` the unroll phase is off and every loop
// iteration polls the signal.

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"

using namespace llvm;

#define DEBUG_TYPE "racer-probe"

static cl::opt<unsigned> ProbeIntervalInstructions(
    "racer-probe-interval", cl::init(200),
    cl::desc("Target number of IR instructions between two probe points. "
             "Innermost loops with smaller bodies are unrolled to reach it. "
             "0 disables unrolling: every loop iteration polls the signal."));

static constexpr const char *PreemptSignalName = "preempt_signal";
static constexpr const char *YieldFunctionName = "do_yield";
static constexpr const char *SkipAnnotation = "xmp_skip";

// The signal byte is almost always zero: the probe branch is cold.
static constexpr uint32_t YieldBranchWeight = 1;
static constexpr uint32_t NoYieldBranchWeight = 10000;

namespace {

class RacerProbePass : public PassInfoMixin<RacerProbePass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
  static bool isRequired() { return true; }

private:
  GlobalVariable *getOrDeclarePreemptSignal(Module &M);
  SmallPtrSet<Function *, 8> collectSkipAnnotatedFunctions(Module &M);
  bool shouldProbe(Function &F,
                   const SmallPtrSetImpl<Function *> &SkipAnnotated);

  bool unrollInnermostLoops(Function &F, FunctionAnalysisManager &FAM);
  bool insertProbesAtLoopHeaders(Function &F, FunctionAnalysisManager &FAM,
                                 GlobalVariable *PreemptSignal,
                                 FunctionCallee DoYield);
};

} // namespace

GlobalVariable *RacerProbePass::getOrDeclarePreemptSignal(Module &M) {
  if (GlobalVariable *Existing = M.getNamedGlobal(PreemptSignalName)) {
    if (!Existing->isThreadLocal())
      report_fatal_error(Twine("racer-probe: '") + PreemptSignalName +
                             "' exists but is not thread_local",
                         /*gen_crash_diag=*/false);
    if (Existing->hasLocalLinkage())
      report_fatal_error(Twine("racer-probe: '") + PreemptSignalName +
                             "' has local linkage; probes would read a private "
                             "copy that the runtime never sets",
                         /*gen_crash_diag=*/false);
    return Existing;
  }
  PointerType *PointerTy = PointerType::getUnqual(M.getContext());
  return new GlobalVariable(
      M, PointerTy, /*isConstant=*/false, GlobalValue::ExternalLinkage,
      /*Initializer=*/nullptr, PreemptSignalName,
      /*InsertBefore=*/nullptr, GlobalValue::InitialExecTLSModel);
}

// Functions marked __attribute__((annotate("xmp_skip"))) get no probes. The
// annotation name is the one the original RACER XMPPass used.
SmallPtrSet<Function *, 8>
RacerProbePass::collectSkipAnnotatedFunctions(Module &M) {
  SmallPtrSet<Function *, 8> SkipAnnotated;
  GlobalVariable *Annotations = M.getNamedGlobal("llvm.global.annotations");
  if (!Annotations || !Annotations->hasInitializer())
    return SkipAnnotated;
  auto *Entries = dyn_cast<ConstantArray>(Annotations->getInitializer());
  if (!Entries)
    return SkipAnnotated;
  for (Value *EntryValue : Entries->operands()) {
    auto *Entry = dyn_cast<ConstantStruct>(EntryValue);
    if (!Entry || Entry->getNumOperands() < 2)
      continue;
    auto *Annotated =
        dyn_cast<Function>(Entry->getOperand(0)->stripPointerCasts());
    auto *StringGlobal =
        dyn_cast<GlobalVariable>(Entry->getOperand(1)->stripPointerCasts());
    if (!Annotated || !StringGlobal || !StringGlobal->hasInitializer())
      continue;
    auto *Text = dyn_cast<ConstantDataArray>(StringGlobal->getInitializer());
    if (Text && Text->isCString() && Text->getAsCString() == SkipAnnotation)
      SkipAnnotated.insert(Annotated);
  }
  return SkipAnnotated;
}

bool RacerProbePass::shouldProbe(
    Function &F, const SmallPtrSetImpl<Function *> &SkipAnnotated) {
  if (F.isDeclaration())
    return false;
  if (F.getName() == YieldFunctionName)
    return false;
  if (F.hasFnAttribute(Attribute::Naked))
    return false;
  return !SkipAnnotated.contains(&F);
}

// A call counts as one IR instruction but can run for any length of time.
// Unrolling a loop with such a call would stretch the time between probes, so
// these loops keep one probe per iteration.
static bool hasCallOfUnknownCost(const Loop &L) {
  for (BasicBlock *Block : L.blocks()) {
    for (Instruction &I : *Block) {
      auto *Call = dyn_cast<CallBase>(&I);
      if (!Call)
        continue;
      if (Call->isInlineAsm() || Call->isConvergent())
        return true;
      if (auto *MemoryIntrinsic = dyn_cast<MemIntrinsic>(Call)) {
        if (!isa<ConstantInt>(MemoryIntrinsic->getLength()))
          return true;
        continue;
      }
      if (!isa<IntrinsicInst>(Call))
        return true;
    }
  }
  return false;
}

static unsigned countBodyInstructions(const Loop &L) {
  unsigned Count = 0;
  for (BasicBlock *Block : L.blocks())
    for (Instruction &I : *Block)
      if (!isa<PHINode>(I) && !I.isDebugOrPseudoInst())
        ++Count;
  return Count;
}

bool RacerProbePass::unrollInnermostLoops(Function &F,
                                          FunctionAnalysisManager &FAM) {
  if (ProbeIntervalInstructions == 0 || F.hasOptNone())
    return false;

  auto &LoopInfoResult = FAM.getResult<LoopAnalysis>(F);
  auto &DomTree = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &ScalarEvolutionResult = FAM.getResult<ScalarEvolutionAnalysis>(F);
  auto &Assumptions = FAM.getResult<AssumptionAnalysis>(F);
  auto &TargetInfo = FAM.getResult<TargetIRAnalysis>(F);
  auto &RemarkEmitter = FAM.getResult<OptimizationRemarkEmitterAnalysis>(F);

  SmallVector<Loop *, 16> InnermostLoops;
  for (Loop *L : LoopInfoResult.getLoopsInPreorder())
    if (L->isInnermost())
      InnermostLoops.push_back(L);

  bool Changed = false;
  for (Loop *L : InnermostLoops) {
    if (hasCallOfUnknownCost(*L) || !L->isSafeToClone())
      continue;

    unsigned BodyInstructions = countBodyInstructions(*L);
    unsigned UnrollCount = ProbeIntervalInstructions / BodyInstructions;
    unsigned MaxTripCount =
        ScalarEvolutionResult.getSmallConstantMaxTripCount(L);
    if (MaxTripCount != 0 && UnrollCount > MaxTripCount)
      UnrollCount = MaxTripCount;
    if (UnrollCount < 2)
      continue;

    Changed |= simplifyLoop(L, &DomTree, &LoopInfoResult,
                            &ScalarEvolutionResult, &Assumptions,
                            /*MSSAU=*/nullptr, /*PreserveLCSSA=*/false);
    Changed |= formLCSSARecursively(*L, DomTree, &LoopInfoResult,
                                    &ScalarEvolutionResult);
    if (!L->isLoopSimplifyForm())
      continue;

    DebugLoc LoopLocation = L->getStartLoc();
    BasicBlock *LoopHeader = L->getHeader();

    UnrollLoopOptions Options;
    Options.Count = UnrollCount;
    Options.Force = false;
    Options.Runtime = false; // keep the exit test in each copy: no remainder
    Options.AllowExpensiveTripCount = true;
    Options.UnrollRemainder = false;
    Options.ForgetAllSCEV = false;
    LoopUnrollResult Result =
        UnrollLoop(L, Options, &LoopInfoResult, &ScalarEvolutionResult,
                   &DomTree, &Assumptions, &TargetInfo, &RemarkEmitter,
                   /*PreserveLCSSA=*/true);
    if (Result == LoopUnrollResult::Unmodified)
      continue;
    Changed = true;

    RemarkEmitter.emit([&] {
      return OptimizationRemark(DEBUG_TYPE, "Unroll", LoopLocation, LoopHeader)
             << "unrolled loop of "
             << ore::NV("BodyInstructions", BodyInstructions)
             << " IR instructions by " << ore::NV("UnrollCount", UnrollCount)
             << (Result == LoopUnrollResult::FullyUnrolled ? " (fully)" : "");
    });
  }
  return Changed;
}

bool RacerProbePass::insertProbesAtLoopHeaders(Function &F,
                                               FunctionAnalysisManager &FAM,
                                               GlobalVariable *PreemptSignal,
                                               FunctionCallee DoYield) {
  auto &LoopInfoResult = FAM.getResult<LoopAnalysis>(F);
  auto &DomTree = FAM.getResult<DominatorTreeAnalysis>(F);
  auto &RemarkEmitter = FAM.getResult<OptimizationRemarkEmitterAnalysis>(F);
  DomTreeUpdater Updater(DomTree, DomTreeUpdater::UpdateStrategy::Eager);

  LLVMContext &Context = F.getContext();
  Type *PointerTy = PointerType::getUnqual(Context);
  Type *ByteTy = Type::getInt8Ty(Context);
  MDNode *ColdYieldWeights = MDBuilder(Context).createBranchWeights(
      YieldBranchWeight, NoYieldBranchWeight);

  // Collect first: splitting a header adds blocks to LoopInfo.
  SmallVector<Loop *, 16> Loops(LoopInfoResult.getLoopsInPreorder());
  bool Changed = false;
  for (Loop *L : Loops) {
    BasicBlock *Header = L->getHeader();
    BasicBlock::iterator InsertPoint = Header->getFirstInsertionPt();
    if (InsertPoint == Header->end())
      continue; // EH pad header (catchswitch): nothing may go here
    Instruction *FirstOriginalInstruction = &*InsertPoint;

    IRBuilder<> Builder(FirstOriginalInstruction);
    Value *SignalSlot = Builder.CreateThreadLocalAddress(PreemptSignal);
    Value *StatusByteAddress =
        Builder.CreateLoad(PointerTy, SignalSlot, "racer.signal");
    // The accelerator writes the byte asynchronously; a non-volatile load
    // could be hoisted out of the loop or merged with an earlier one.
    Value *StatusByte = Builder.CreateLoad(ByteTy, StatusByteAddress,
                                           /*isVolatile=*/true, "racer.status");
    Value *AcceleratorDone = Builder.CreateICmpNE(
        StatusByte, ConstantInt::get(ByteTy, 0), "racer.done");

    Instruction *YieldBranchEnd = SplitBlockAndInsertIfThen(
        AcceleratorDone, FirstOriginalInstruction, /*Unreachable=*/false,
        ColdYieldWeights, &Updater, &LoopInfoResult);
    YieldBranchEnd->getParent()->setName("racer.yield");
    CallInst *YieldCall = CallInst::Create(DoYield, "", YieldBranchEnd);
    YieldCall->setDoesNotThrow();
    YieldCall->setDebugLoc(Header->getTerminator()->getDebugLoc());
    Changed = true;

    RemarkEmitter.emit([&] {
      return OptimizationRemark(DEBUG_TYPE, "Probe", L->getStartLoc(), Header)
             << "probe point at loop header in "
             << ore::NV("Function", F.getName());
    });
  }
  return Changed;
}

PreservedAnalyses RacerProbePass::run(Module &M, ModuleAnalysisManager &MAM) {
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  SmallPtrSet<Function *, 8> SkipAnnotated = collectSkipAnnotatedFunctions(M);

  SmallVector<Function *, 64> FunctionsToProbe;
  for (Function &F : M)
    if (shouldProbe(F, SkipAnnotated))
      FunctionsToProbe.push_back(&F);
  if (FunctionsToProbe.empty())
    return PreservedAnalyses::all();

  GlobalVariable *PreemptSignal = nullptr;
  FunctionCallee DoYield;
  bool ModuleChanged = false;
  for (Function *F : FunctionsToProbe) {
    if (unrollInnermostLoops(*F, FAM)) {
      FAM.invalidate(*F, PreservedAnalyses::none());
      ModuleChanged = true;
    }

    if (FAM.getResult<LoopAnalysis>(*F).empty())
      continue;
    if (!PreemptSignal) {
      PreemptSignal = getOrDeclarePreemptSignal(M);
      DoYield = M.getOrInsertFunction(YieldFunctionName,
                                      Type::getVoidTy(M.getContext()));
    }
    insertProbesAtLoopHeaders(*F, FAM, PreemptSignal, DoYield);
    FAM.invalidate(*F, PreservedAnalyses::none());
    ModuleChanged = true;
  }
  return ModuleChanged ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RacerProbe", LLVM_VERSION_STRING,
          [](PassBuilder &Builder) {
            // clang -fpass-plugin: probe after all other optimizations, so
            // that no later pass moves, merges or deletes probe points.
            Builder.registerOptimizerLastEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(RacerProbePass());
                });
            // opt -passes=racer-probe
            Builder.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name != DEBUG_TYPE)
                    return false;
                  MPM.addPass(RacerProbePass());
                  return true;
                });
          }};
}
