//===---- MachineOutliner.cpp - Outline instructions -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Replaces repeated sequences of instructions with function calls.
///
/// This works by placing every instruction from every basic block in a
/// suffix tree, and repeatedly querying that tree for repeated sequences of
/// instructions. If a sequence of instructions appears often, then it ought
/// to be beneficial to pull out into a function.
///
/// The MachineOutliner communicates with a given target using hooks defined in
/// TargetInstrInfo.h. The target supplies the outliner with information on how
/// a specific sequence of instructions should be outlined. This information
/// is used to deduce the number of instructions necessary to
///
/// * Create an outlined function
/// * Call that outlined function
///
/// Targets must implement
///   * getOutliningCandidateInfo
///   * buildOutlinedFrame
///   * insertOutlinedCall
///   * isFunctionSafeToOutlineFrom
///
/// in order to make use of the MachineOutliner.
///
/// This was originally presented at the 2016 LLVM Developers' Meeting in the
/// talk "Reducing Code Size Using Outlining". For a high-level overview of
/// how this pass works, the talk is available on YouTube at
///
/// https://www.youtube.com/watch?v=yorld-WSOeU
///
/// The slides for the talk are available at
///
/// http://www.llvm.org/devmtg/2016-11/Slides/Paquette-Outliner.pdf
///
/// The talk provides an overview of how the outliner finds candidates and
/// ultimately outlines them. It describes how the main data structure for this
/// pass, the suffix tree, is queried and purged for candidates. It also gives
/// a simplified suffix tree construction algorithm for suffix trees based off
/// of the algorithm actually used here, Ukkonen's algorithm.
///
/// For the original RFC for this pass, please see
///
/// http://lists.llvm.org/pipermail/llvm-dev/2016-August/104170.html
///
/// For more information on the suffix tree data structure, please see
/// https://www.cs.helsinki.fi/u/ukkonen/SuffixT1withFigs.pdf
///
//===----------------------------------------------------------------------===//
#include "llvm/CodeGen/MachineOutliner.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/GCMetadata.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineOptimizationRemarkEmitter.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/StackMapEncode.h"
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SuffixTree.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/MC/MCStreamer.h"
#include <functional>
#include <string>
#include <tuple>
#include <vector>

namespace llvm {
extern cl::opt<bool> CJPipeline;
} // namespace llvm

#define DEBUG_TYPE "machine-outliner"

using namespace llvm;
using namespace ore;
using namespace outliner;

static const char *CangjieOutlinedStatepointFPToSPOffsetAttr =
    "cangjie-outlined-statepoint-fp-sp-offset";

// Statistics for outlined functions.
STATISTIC(NumOutlined, "Number of candidates outlined");
STATISTIC(FunctionsCreated, "Number of functions created");

// Statistics for instruction mapping.
STATISTIC(NumLegalInUnsignedVec, "Number of legal instrs in unsigned vector");
STATISTIC(NumIllegalInUnsignedVec,
          "Number of illegal instrs in unsigned vector");
STATISTIC(NumInvisible, "Number of invisible instrs in unsigned vector");
STATISTIC(UnsignedVecSize, "Size of unsigned vector");

// Set to true if the user wants the outliner to run on linkonceodr linkage
// functions. This is false by default because the linker can dedupe linkonceodr
// functions. Since the outliner is confined to a single module (modulo LTO),
// this is off by default. It should, however, be the default behaviour in
// LTO.
static cl::opt<bool> EnableLinkOnceODROutlining(
    "enable-linkonceodr-outlining", cl::Hidden,
    cl::desc("Enable the machine outliner on linkonceodr functions"),
    cl::init(false));

/// Number of times to re-run the outliner. This is not the total number of runs
/// as the outliner will run at least one time. The default value is set to 0,
/// meaning the outliner will run one time and rerun zero times after that.
static cl::opt<unsigned> OutlinerReruns(
    "machine-outliner-reruns", cl::init(0), cl::Hidden,
    cl::desc(
        "Number of times to rerun the outliner after the initial outline"));

namespace {

/// Maps \p MachineInstrs to unsigned integers and stores the mappings.
struct InstructionMapper {

  /// The next available integer to assign to a \p MachineInstr that
  /// cannot be outlined.
  ///
  /// Set to -3 for compatability with \p DenseMapInfo<unsigned>.
  unsigned IllegalInstrNumber = -3;

  /// The next available integer to assign to a \p MachineInstr that can
  /// be outlined.
  unsigned LegalInstrNumber = 0;

  /// Correspondence from \p MachineInstrs to unsigned integers.
  DenseMap<MachineInstr *, unsigned, MachineInstrExpressionTrait>
      InstructionIntegerMap;

  /// Correspondence between \p MachineBasicBlocks and target-defined flags.
  DenseMap<MachineBasicBlock *, unsigned> MBBFlagsMap;

  /// The vector of unsigned integers that the module is mapped to.
  std::vector<unsigned> UnsignedVec;

  /// Stores the location of the instruction associated with the integer
  /// at index i in \p UnsignedVec for each index i.
  std::vector<MachineBasicBlock::iterator> InstrList;

  // Set if we added an illegal number in the previous step.
  // Since each illegal number is unique, we only need one of them between
  // each range of legal numbers. This lets us make sure we don't add more
  // than one illegal number per range.
  bool AddedIllegalLastTime = false;

  /// Maps \p *It to a legal integer.
  ///
  /// Updates \p CanOutlineWithPrevInstr, \p HaveLegalRange, \p InstrListForMBB,
  /// \p UnsignedVecForMBB, \p InstructionIntegerMap, and \p LegalInstrNumber.
  ///
  /// \returns The integer that \p *It was mapped to.
  unsigned mapToLegalUnsigned(
      MachineBasicBlock::iterator &It, bool &CanOutlineWithPrevInstr,
      bool &HaveLegalRange, unsigned &NumLegalInBlock,
      std::vector<unsigned> &UnsignedVecForMBB,
      std::vector<MachineBasicBlock::iterator> &InstrListForMBB) {
    // We added something legal, so we should unset the AddedLegalLastTime
    // flag.
    AddedIllegalLastTime = false;

    // If we have at least two adjacent legal instructions (which may have
    // invisible instructions in between), remember that.
    if (CanOutlineWithPrevInstr)
      HaveLegalRange = true;
    CanOutlineWithPrevInstr = true;

    // Keep track of the number of legal instructions we insert.
    NumLegalInBlock++;

    // Get the integer for this instruction or give it the current
    // LegalInstrNumber.
    InstrListForMBB.push_back(It);
    MachineInstr &MI = *It;
    bool WasInserted;
    DenseMap<MachineInstr *, unsigned, MachineInstrExpressionTrait>::iterator
        ResultIt;
    std::tie(ResultIt, WasInserted) =
        InstructionIntegerMap.insert(std::make_pair(&MI, LegalInstrNumber));
    unsigned MINumber = ResultIt->second;

    // There was an insertion.
    if (WasInserted)
      LegalInstrNumber++;

    UnsignedVecForMBB.push_back(MINumber);

    // Make sure we don't overflow or use any integers reserved by the DenseMap.
    if (LegalInstrNumber >= IllegalInstrNumber)
      report_fatal_error("Instruction mapping overflow!");

    assert(LegalInstrNumber != DenseMapInfo<unsigned>::getEmptyKey() &&
           "Tried to assign DenseMap tombstone or empty key to instruction.");
    assert(LegalInstrNumber != DenseMapInfo<unsigned>::getTombstoneKey() &&
           "Tried to assign DenseMap tombstone or empty key to instruction.");

    // Statistics.
    ++NumLegalInUnsignedVec;
    return MINumber;
  }

  /// Maps \p *It to an illegal integer.
  ///
  /// Updates \p InstrListForMBB, \p UnsignedVecForMBB, and \p
  /// IllegalInstrNumber.
  ///
  /// \returns The integer that \p *It was mapped to.
  unsigned mapToIllegalUnsigned(
      MachineBasicBlock::iterator &It, bool &CanOutlineWithPrevInstr,
      std::vector<unsigned> &UnsignedVecForMBB,
      std::vector<MachineBasicBlock::iterator> &InstrListForMBB) {
    // Can't outline an illegal instruction. Set the flag.
    CanOutlineWithPrevInstr = false;

    // Only add one illegal number per range of legal numbers.
    if (AddedIllegalLastTime)
      return IllegalInstrNumber;

    // Remember that we added an illegal number last time.
    AddedIllegalLastTime = true;
    unsigned MINumber = IllegalInstrNumber;

    InstrListForMBB.push_back(It);
    UnsignedVecForMBB.push_back(IllegalInstrNumber);
    IllegalInstrNumber--;
    // Statistics.
    ++NumIllegalInUnsignedVec;

    assert(LegalInstrNumber < IllegalInstrNumber &&
           "Instruction mapping overflow!");

    assert(IllegalInstrNumber != DenseMapInfo<unsigned>::getEmptyKey() &&
           "IllegalInstrNumber cannot be DenseMap tombstone or empty key!");

    assert(IllegalInstrNumber != DenseMapInfo<unsigned>::getTombstoneKey() &&
           "IllegalInstrNumber cannot be DenseMap tombstone or empty key!");

    return MINumber;
  }

  /// Transforms a \p MachineBasicBlock into a \p vector of \p unsigneds
  /// and appends it to \p UnsignedVec and \p InstrList.
  ///
  /// Two instructions are assigned the same integer if they are identical.
  /// If an instruction is deemed unsafe to outline, then it will be assigned an
  /// unique integer. The resulting mapping is placed into a suffix tree and
  /// queried for candidates.
  ///
  /// \param MBB The \p MachineBasicBlock to be translated into integers.
  /// \param TII \p TargetInstrInfo for the function.
  void convertToUnsignedVec(MachineBasicBlock &MBB,
                            const TargetInstrInfo &TII,
                            bool TreatStatepointAsLegal) {
    unsigned Flags = 0;

    // Don't even map in this case.
    if (!TII.isMBBSafeToOutlineFrom(MBB, Flags))
      return;

    // Store info for the MBB for later outlining.
    MBBFlagsMap[&MBB] = Flags;

    MachineBasicBlock::iterator It = MBB.begin();

    // The number of instructions in this block that will be considered for
    // outlining.
    unsigned NumLegalInBlock = 0;

    // True if we have at least two legal instructions which aren't separated
    // by an illegal instruction.
    bool HaveLegalRange = false;

    // True if we can perform outlining given the last mapped (non-invisible)
    // instruction. This lets us know if we have a legal range.
    bool CanOutlineWithPrevInstr = false;

    // FIXME: Should this all just be handled in the target, rather than using
    // repeated calls to getOutliningType?
    std::vector<unsigned> UnsignedVecForMBB;
    std::vector<MachineBasicBlock::iterator> InstrListForMBB;

    for (MachineBasicBlock::iterator Et = MBB.end(); It != Et; ++It) {
      // Keep track of where this instruction is in the module.
      InstrType OutliningType = TII.getOutliningType(It, Flags);

      // The dedicated multi-STATEPOINT outlining flow needs STATEPOINT to stay
      // inside a candidate instead of forcibly terminating it.
      if (TreatStatepointAsLegal &&
          It->getOpcode() == TargetOpcode::STATEPOINT)
        OutliningType = InstrType::Legal;

      if (TreatStatepointAsLegal &&
          TII.isIllegalForMultiStatepointOutlining(*It) &&
          It->getOpcode() != TargetOpcode::STATEPOINT) {
        OutliningType = InstrType::Illegal;
      }

      switch (OutliningType) {
      case InstrType::Illegal:
        mapToIllegalUnsigned(It, CanOutlineWithPrevInstr, UnsignedVecForMBB,
                             InstrListForMBB);
        break;

      case InstrType::Legal:
        mapToLegalUnsigned(It, CanOutlineWithPrevInstr, HaveLegalRange,
                           NumLegalInBlock, UnsignedVecForMBB, InstrListForMBB);
        break;

      case InstrType::LegalTerminator:
        mapToLegalUnsigned(It, CanOutlineWithPrevInstr, HaveLegalRange,
                           NumLegalInBlock, UnsignedVecForMBB, InstrListForMBB);
        // The instruction also acts as a terminator, so we have to record that
        // in the string.
        mapToIllegalUnsigned(It, CanOutlineWithPrevInstr, UnsignedVecForMBB,
                             InstrListForMBB);
        break;

      case InstrType::Invisible:
        // Normally this is set by mapTo(Blah)Unsigned, but we just want to
        // skip this instruction. So, unset the flag here.
        ++NumInvisible;
        AddedIllegalLastTime = false;
        break;
      }
    }

    // Are there enough legal instructions in the block for outlining to be
    // possible?
    if (HaveLegalRange) {
      // After we're done every insertion, uniquely terminate this part of the
      // "string". This makes sure we won't match across basic block or function
      // boundaries since the "end" is encoded uniquely and thus appears in no
      // repeated substring.
      mapToIllegalUnsigned(It, CanOutlineWithPrevInstr, UnsignedVecForMBB,
                           InstrListForMBB);
      llvm::append_range(InstrList, InstrListForMBB);
      llvm::append_range(UnsignedVec, UnsignedVecForMBB);
    }
  }

  InstructionMapper() {
    // Make sure that the implementation of DenseMapInfo<unsigned> hasn't
    // changed.
    assert(DenseMapInfo<unsigned>::getEmptyKey() == (unsigned)-1 &&
           "DenseMapInfo<unsigned>'s empty key isn't -1!");
    assert(DenseMapInfo<unsigned>::getTombstoneKey() == (unsigned)-2 &&
           "DenseMapInfo<unsigned>'s tombstone key isn't -2!");
  }
};

/// An interprocedural pass which finds repeated sequences of
/// instructions and replaces them with calls to functions.
///
/// Each instruction is mapped to an unsigned integer and placed in a string.
/// The resulting mapping is then placed in a \p SuffixTree. The \p SuffixTree
/// is then repeatedly queried for repeated sequences of instructions. Each
/// non-overlapping repeated sequence is then placed in its own
/// \p MachineFunction and each instance is then replaced with a call to that
/// function.
struct MachineOutliner : public ModulePass {

  static char ID;

  std::unique_ptr<AsmPrinter> ASM;

  StackMaps *SM = nullptr;

  DenseMap<const MachineInstr *, unsigned> StatepointToStackMapIdx;

  DenseMap<const MachineInstr *, std::string> StackMapValueMap;

  DenseMap<const MachineInstr *, bool> StatepointHasNonEmptyDerivedStartIdx;

  struct StatepointHashCallTargetGroup {
    std::string Hash;
    int64_t FPToSPOffset = 0;
    const MachineInstr *Representative = nullptr;
    std::vector<const MachineInstr *> Statepoints;
  };

  /// Set to true if the outliner should consider functions with
  /// linkonceodr linkage.
  bool OutlineFromLinkOnceODRs = false;

  /// The current repeat number of machine outlining.
  unsigned OutlineRepeatedNum = 0;

  /// Set to true if the outliner should run on all functions in the module
  /// considered safe for outlining.
  /// Set to true by default for compatibility with llc's -run-pass option.
  /// Set when the pass is constructed in TargetPassConfig.
  bool RunOnAllFunctions = true;

  StringRef getPassName() const override { return "Machine Outliner"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineModuleInfoWrapperPass>();
    AU.addRequired<GCModuleInfo>();
    AU.addPreserved<MachineModuleInfoWrapperPass>();
    AU.setPreservesAll();
    ModulePass::getAnalysisUsage(AU);
  }

  MachineOutliner() : ModulePass(ID) {
    initializeMachineOutlinerPass(*PassRegistry::getPassRegistry());
  }

  /// Remark output explaining that not outlining a set of candidates would be
  /// better than outlining that set.
  void emitNotOutliningCheaperRemark(
      unsigned StringLen, std::vector<Candidate> &CandidatesForRepeatedSeq,
      OutlinedFunction &OF);

  /// Remark output explaining that a function was outlined.
  void emitOutlinedFunctionRemark(OutlinedFunction &OF);

  /// Find repeated substrings with a dedicated suffix-tree traversal for the
  /// multi-STATEPOINT mode, then keep only groups whose STATEPOINT hash
  /// sequence is consistent inside each group.
  ///
  /// \param Mapper Contains outlining mapping information.
  /// \param[out] FunctionList Filled with a list of \p OutlinedFunctions
  /// each type of candidate.
  void findMultiStatepointHashCandidates(
      InstructionMapper &Mapper, std::vector<OutlinedFunction> &FunctionList);

  /// Find all repeated substrings that satisfy the outlining cost model by
  /// constructing a suffix tree.
  ///
  /// If a substring appears at least twice, then it must be represented by
  /// an internal node which appears in at least two suffixes. Each suffix
  /// is represented by a leaf node. To do this, we visit each internal node
  /// in the tree, using the leaf children of each internal node. If an
  /// internal node represents a beneficial substring, then we use each of
  /// its leaf children to find the locations of its substring.
  ///
  /// \param Mapper Contains outlining mapping information.
  /// \param[out] FunctionList Filled with a list of \p OutlinedFunctions
  /// each type of candidate.
  void findCandidates(InstructionMapper &Mapper,
                      std::vector<OutlinedFunction> &FunctionList);

  /// Replace the sequences of instructions represented by \p OutlinedFunctions
  /// with calls to functions.
  ///
  /// \param M The module we are outlining from.
  /// \param FunctionList A list of functions to be inserted into the module.
  /// \param Mapper Contains the instruction mappings for the module.
  bool outline(Module &M, std::vector<OutlinedFunction> &FunctionList,
               InstructionMapper &Mapper, unsigned &OutlinedFunctionNum);

  /// Creates a function for \p OF and inserts it into the module.
  MachineFunction *createOutlinedFunction(Module &M, OutlinedFunction &OF,
                                          InstructionMapper &Mapper,
                                          unsigned Name);

  /// Calls 'doOutline()' 1 + OutlinerReruns times.
  bool runOnModule(Module &M) override;

  /// Construct a suffix tree on the instructions in \p M and outline repeated
  /// strings from that tree.
  bool doOutline(Module &M, unsigned &OutlinedFunctionNum);

  /// Return the stackmap callsite info collected for a STATEPOINT.
  const StackMaps::CallsiteInfo *
  getStatepointCallsiteInfo(const MachineInstr &MI) const;

  /// Return the compressed stackmap data collected for a STATEPOINT.
  std::unique_ptr<CompressedInfo>
  createStatepointCompressedInfo(const MachineInstr &MI) const;

  std::string getStatepointCompressedInfoHash(const MachineInstr &MI) const;

  /// Collect STATEPOINT instructions with the same stack map hash and the same
  /// call target into groups.
  std::vector<StatepointHashCallTargetGroup>
  collectStatepointGroupsByHashAndCallTarget() const;

  /// Find single-STATEPOINT groups that can share one outlined STATEPOINT to
  /// reduce emitted stack map records.
  void findCangjieStatepointStackMapCandidates(
      InstructionMapper &Mapper, std::vector<OutlinedFunction> &FunctionList);

  /// Return a DISubprogram for OF if one exists, and null otherwise. Helper
  /// function for remark emission.
  DISubprogram *getSubprogramOrNull(const OutlinedFunction &OF) {
    for (const Candidate &C : OF.Candidates)
      if (MachineFunction *MF = C.getMF())
        if (DISubprogram *SP = MF->getFunction().getSubprogram())
          return SP;
    return nullptr;
  }

  /// Populate and \p InstructionMapper with instruction-to-integer mappings.
  /// These are used to construct a suffix tree.
  void populateMapper(InstructionMapper &Mapper, Module &M,
                      MachineModuleInfo &MMI,
                      bool TreatStatepointAsLegal = false,
                      const SmallPtrSetImpl<MachineFunction *> *AllowedMFs =
                          nullptr);

  /// Run the target AsmPrinter's STATEPOINT stack map collection against a
  /// null streamer so that the outliner has the stack map records the backend
  /// would eventually emit.
  void collectStatepointStackMaps(Module &M, MachineModuleInfo &MMI);

  /// Initialize information necessary to output a size remark.
  /// FIXME: This should be handled by the pass manager, not the outliner.
  /// FIXME: This is nearly identical to the initSizeRemarkInfo in the legacy
  /// pass manager.
  void initSizeRemarkInfo(const Module &M, const MachineModuleInfo &MMI,
                          StringMap<unsigned> &FunctionToInstrCount);

  /// Emit the remark.
  // FIXME: This should be handled by the pass manager, not the outliner.
  void
  emitInstrCountChangedRemark(const Module &M, const MachineModuleInfo &MMI,
                              const StringMap<unsigned> &FunctionToInstrCount);
};
} // Anonymous namespace.

char MachineOutliner::ID = 0;

namespace llvm {
ModulePass *createMachineOutlinerPass(bool RunOnAllFunctions) {
  MachineOutliner *OL = new MachineOutliner();
  OL->RunOnAllFunctions = RunOnAllFunctions || CJPipeline;
  return OL;
}

} // namespace llvm

INITIALIZE_PASS(MachineOutliner, DEBUG_TYPE, "Machine Function Outliner", false,
                false)

void MachineOutliner::emitNotOutliningCheaperRemark(
    unsigned StringLen, std::vector<Candidate> &CandidatesForRepeatedSeq,
    OutlinedFunction &OF) {
  // FIXME: Right now, we arbitrarily choose some Candidate from the
  // OutlinedFunction. This isn't necessarily fixed, nor does it have to be.
  // We should probably sort these by function name or something to make sure
  // the remarks are stable.
  Candidate &C = CandidatesForRepeatedSeq.front();
  MachineOptimizationRemarkEmitter MORE(*(C.getMF()), nullptr);
  MORE.emit([&]() {
    MachineOptimizationRemarkMissed R(DEBUG_TYPE, "NotOutliningCheaper",
                                      C.front()->getDebugLoc(), C.getMBB());
    R << "Did not outline " << NV("Length", StringLen) << " instructions"
      << " from " << NV("NumOccurrences", CandidatesForRepeatedSeq.size())
      << " locations."
      << " Bytes from outlining all occurrences ("
      << NV("OutliningCost", OF.getOutliningCost()) << ")"
      << " >= Unoutlined instruction bytes ("
      << NV("NotOutliningCost", OF.getNotOutlinedCost()) << ")"
      << " (Also found at: ";

    // Tell the user the other places the candidate was found.
    for (unsigned i = 1, e = CandidatesForRepeatedSeq.size(); i < e; i++) {
      R << NV((Twine("OtherStartLoc") + Twine(i)).str(),
              CandidatesForRepeatedSeq[i].front()->getDebugLoc());
      if (i != e - 1)
        R << ", ";
    }

    R << ")";
    return R;
  });
}

void MachineOutliner::emitOutlinedFunctionRemark(OutlinedFunction &OF) {
  MachineBasicBlock *MBB = &*OF.MF->begin();
  MachineOptimizationRemarkEmitter MORE(*OF.MF, nullptr);
  MachineOptimizationRemark R(DEBUG_TYPE, "OutlinedFunction",
                              MBB->findDebugLoc(MBB->begin()), MBB);
  R << "Saved " << NV("OutliningBenefit", OF.getBenefit()) << " bytes by "
    << "outlining " << NV("Length", OF.getNumInstrs()) << " instructions "
    << "from " << NV("NumOccurrences", OF.getOccurrenceCount())
    << " locations. "
    << "(Found at: ";

  // Tell the user the other places the candidate was found.
  for (size_t i = 0, e = OF.Candidates.size(); i < e; i++) {

    R << NV((Twine("StartLoc") + Twine(i)).str(),
            OF.Candidates[i].front()->getDebugLoc());
    if (i != e - 1)
      R << ", ";
  }

  R << ")";

  MORE.emit(R);
}

static bool hasStatePointBeforeEnd(MachineBasicBlock::iterator StartIt, MachineBasicBlock::iterator EndIt) {
  for (auto It = StartIt; It != EndIt; ++It) {
    if (It->getOpcode() == TargetOpcode::STATEPOINT)
      return true;
  }
  return false;
}

static bool isCangjieSpecificCallee(const MachineInstr *MI, const Function *Callee) {
  if (Callee == nullptr) {
    return false;
  }

  const auto &CallerFunc = MI->getParent()->getParent()->getFunction();

  StringRef FuncName = Callee->getName();

  return FuncName.isGetGCPhase() 
      || FuncName.isSetDebugLocation() 
      || FuncName.equals("CJ_MRT_PreInitializePackage")
      || FuncName.equals("CJ_MCC_ThrowException")
      || FuncName.equals("CJ_MCC_NewFinalizer")
      || FuncName.startswith("CJ_MCC_NewArray")
      || FuncName.equals("CJ_MCC_NewObjArray")
      || Callee->isGetCJThreadId()
      || Callee->isCangjieNativeStub(CallerFunc);
}

static bool isCangjieSpecificCall(const MachineInstr *MI) {
  if (!MI->isCall()) {
    return false;
  }
  unsigned Opcode = MI->getOpcode();
  if (Opcode == TargetOpcode::STATEPOINT) {
    StatepointOpers SO(MI);
    if (SO.getID() != Cangjie::CJStatepointID::Default &&
        SO.getID() != Cangjie::CJStatepointID::Safepoint)
      return true;

    const auto *Callee = SO.getCalledFunction();
    if (Callee && Callee->isCangjieSafePoint())
      return true;
    return isCangjieSpecificCallee(MI, Callee);
  }

  if (MI->getNumOperands() == 0) {
    return false;
  }

  const MachineOperand &MOSym = MI->getOperand(0);
  if (!MOSym.isGlobal()) {
    return false;
  }

  const auto *Callee = dyn_cast<const Function>(MOSym.getGlobal());
  return isCangjieSpecificCallee(MI, Callee);
}

static bool hasCangjieSpecificCallInRange(
    MachineBasicBlock::iterator StartIt, MachineBasicBlock::iterator EndIt) {
  for (auto It = StartIt;; ++It) {
    if (isCangjieSpecificCall(&*It))
      return true;
    if (It == EndIt)
      break;
  }
  return false;
}

static std::string getStatepointCallTargetKey(const MachineInstr &MI) {
  StatepointOpers SO(&MI);
  std::string Key;
  raw_string_ostream OS(Key);
  SO.getCallTarget().print(OS);
  return OS.str();
}

static int64_t getFunctionFPToSPOffset(const MachineFunction &MF) {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  return static_cast<int64_t>(MFI.getStackSize());
}

static bool isMachineOutlinerCreatedFunction(const MachineFunction &MF) {
  return MF.getName().startswith("OUTLINED_FUNCTION");
}

static bool hasNonEmptyDerivedStartIdx(const CompressedInfo &Data) {
  for (const auto &StackMapItem : Data.StackMapItem)
    if (StackMapItem.second.DerivedInfoStartIdx != 0)
      return true;
  return false;
}

static bool isCangjieThrowExceptionStatepoint(const MachineInstr &MI) {
  if (MI.getOpcode() != TargetOpcode::STATEPOINT)
    return false;

  StatepointOpers SO(&MI);
  const Function *Callee = SO.getCalledFunction();
  return Callee && Callee->isCangjieThrowException();
}

static bool isCangjieStackCheckStatepoint(const MachineInstr &MI) {
  if (MI.getOpcode() != TargetOpcode::STATEPOINT)
    return false;

  StatepointOpers SO(&MI);
  return SO.getID() == Cangjie::CJStatepointID::StackCheck;
}

static void copyStatepointAllocaObjectAllocations(const MachineInstr &MI,
                                                  MachineFunction &OutlinedMF) {
  if (MI.getOpcode() != TargetOpcode::STATEPOINT)
    return;

  StatepointOpers SO(&MI);
  unsigned NumAllocasIdx = SO.getNumAllocaIdx();
  unsigned NumAllocas =
      static_cast<unsigned>(MI.getOperand(NumAllocasIdx).getImm());
  if (!NumAllocas)
    return;

  int AllocaIdx = SO.getFirstAllocaIdx();
  assert(AllocaIdx != -1 && "expected first alloca operand");

  const MachineFrameInfo &OrigMFI = MI.getMF()->getFrameInfo();
  MachineFrameInfo &OutlinedMFI = OutlinedMF.getFrameInfo();
  while (NumAllocas--) {
    const MachineOperand &Kind = MI.getOperand(AllocaIdx);
    if (Kind.isImm() && Kind.getImm() == StackMaps::DirectMemRefOp) {
      int FI = static_cast<int>(MI.getOperand(AllocaIdx + 3).getImm());
      OutlinedMFI.setOutlinedObjectAllocation(
          FI, OrigMFI.getObjectAllocationIncludingOutlined(FI));
    }
    AllocaIdx = StackMaps::getNextMetaArgIdx(&MI, AllocaIdx);
  }
}

static bool isMappedLegalInstruction(const InstructionMapper &Mapper,
                                     unsigned Idx) {
  assert(Idx < Mapper.UnsignedVec.size() && "Index out of bounds!");
  assert(Idx < Mapper.InstrList.size() && "Index out of bounds!");
  if (Mapper.UnsignedVec[Idx] >= Mapper.LegalInstrNumber)
    return false;

  MachineInstr *MI = &*Mapper.InstrList[Idx];
  auto It = Mapper.InstructionIntegerMap.find(MI);
  return It != Mapper.InstructionIntegerMap.end() &&
         It->second == Mapper.UnsignedVec[Idx];
}

static bool hasConsistentStatepointHash(
    const DenseMap<const MachineInstr *, std::string> &StackMapValueMap,
    MachineBasicBlock::iterator StartIt, MachineBasicBlock::iterator EndIt,
    std::string *CommonHash = nullptr) {
  unsigned StatepointNum = 0;
  Optional<StringRef> FirstHash = None;
  for (auto It = StartIt;; ++It) {
    if (It->getOpcode() == TargetOpcode::STATEPOINT) {
      ++StatepointNum;
      auto HashIt = StackMapValueMap.find(&*It);
      if (HashIt != StackMapValueMap.end()) {
        StringRef Hash = HashIt->second;
        if (!FirstHash)
          FirstHash = Hash;
        else if (Hash != *FirstHash)
          return false;
      }
    }

    if (It == EndIt)
      break;
  }

  if (StatepointNum < 2 || !FirstHash)
    return false;

  if (CommonHash)
    *CommonHash = FirstHash->str();
  return true;
}

static unsigned countStatepointsInRange(MachineBasicBlock::iterator StartIt,
                                        MachineBasicBlock::iterator EndIt) {
  unsigned StatepointNum = 0;
  for (auto It = StartIt;; ++It) {
    if (It->getOpcode() == TargetOpcode::STATEPOINT)
      ++StatepointNum;
    if (It == EndIt)
      break;
  }
  return StatepointNum;
}

static bool isMultiStatepointCandidate(Candidate &C) {
  return countStatepointsInRange(C.front(), C.back()) >= 2;
}

static std::vector<unsigned> getStatepointInstructionOffsets(Candidate &C) {
  std::vector<unsigned> Offsets;
  unsigned Offset = 0;
  for (auto It = C.front(), E = std::next(C.back()); It != E; ++It, ++Offset) {
    if (It->getOpcode() == TargetOpcode::STATEPOINT)
      Offsets.push_back(Offset);
  }
  return Offsets;
}

static bool canReplaceStatepointWithRegularCall(const MachineInstr &MI) {
  if (MI.getOpcode() != TargetOpcode::STATEPOINT)
    return true;

  StatepointOpers Statepoint(&MI);
  if (Statepoint.getNumPatchBytes() != 0)
    return false;

  switch (Statepoint.getCallTarget().getType()) {
  case MachineOperand::MO_GlobalAddress:
  case MachineOperand::MO_ExternalSymbol:
  case MachineOperand::MO_Immediate:
  case MachineOperand::MO_Register:
    return true;
  default:
    return false;
  }
}

static bool isSupportedMultiStatepointRange(
    MachineBasicBlock::iterator StartIt, MachineBasicBlock::iterator EndIt) {
  if (EndIt->isTerminator() ||
      (EndIt->isCall() && EndIt->getOpcode() != TargetOpcode::STATEPOINT))
    return false;

  for (auto It = StartIt;; ++It) {
    if (!canReplaceStatepointWithRegularCall(*It))
      return false;
    if (It == EndIt)
      break;
  }

  return !hasCangjieSpecificCallInRange(StartIt, EndIt);
}

static bool buildTrimmedConsistentCandidate(
    const DenseMap<const MachineInstr *, std::string> &StackMapValueMap,
    Candidate &Original, unsigned TrimStartOffset, unsigned TrimEndOffset,
    Candidate &Trimmed) {
  if (TrimStartOffset > TrimEndOffset || TrimEndOffset >= Original.getLength())
    return false;

  MachineBasicBlock::iterator StartIt =
      std::next(Original.front(), TrimStartOffset);
  MachineBasicBlock::iterator EndIt = std::next(Original.front(), TrimEndOffset);

  if (!isSupportedMultiStatepointRange(StartIt, EndIt))
    return false;
  if (!hasConsistentStatepointHash(StackMapValueMap, StartIt, EndIt))
    return false;

  unsigned NewStartIdx = Original.getStartIdx() + TrimStartOffset;
  unsigned NewLen = TrimEndOffset - TrimStartOffset + 1;
  Trimmed = Candidate(NewStartIdx, NewLen, StartIt, EndIt, Original.getMBB(),
                      Original.FunctionIdx, Original.Flags);
  return true;
}

static MachineInstr *findFirstStatepointInCandidate(Candidate &C) {
  for (auto It = C.front(), E = std::next(C.back()); It != E; ++It) {
    if (It->getOpcode() == TargetOpcode::STATEPOINT)
      return &*It;
  }
  return nullptr;
}

static bool shouldWrapOutlinedCallWithStatepoint(
    Candidate &C,
    const DenseMap<const MachineInstr *, std::string> &StackMapValueMap) {
  for (auto It = C.front(), E = std::next(C.back()); It != E; ++It) {
    if (It->getOpcode() != TargetOpcode::STATEPOINT)
      continue;

    auto HashIt = StackMapValueMap.find(&*It);
    if (HashIt == StackMapValueMap.end())
      continue;
    if (!StringRef(HashIt->second).equals("null"))
      return true;
  }
  return false;
}

static MachineBasicBlock::iterator replaceOutlinedCallWithStatepoint(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator CallIt,
    MachineFunction &OutlinedMF, const MachineInstr &PrototypeStatepoint) {
  if (!CallIt->isCall() || CallIt->isTerminator())
    return CallIt;

  MachineFunction &CallerMF = *MBB.getParent();
  MachineInstr *Wrapper = CallerMF.CloneMachineInstr(&PrototypeStatepoint);
  StatepointOpers Statepoint(Wrapper);
  unsigned NCallArgsIdx = Statepoint.getNCallArgsPos();
  unsigned CallTargetIdx = NCallArgsIdx + 1;
  unsigned NumCallArgs = Statepoint.getNumCallArgs();

  for (unsigned I = 0; I < NumCallArgs; ++I)
    Wrapper->removeOperand(CallTargetIdx + 1);
  Wrapper->getOperand(NCallArgsIdx).setImm(0);
  Wrapper->getOperand(CallTargetIdx).ChangeToGA(&OutlinedMF.getFunction(), 0);
  Wrapper->setDebugLoc(CallIt->getDebugLoc());

  MachineBasicBlock::iterator NewIt = MBB.insert(CallIt, Wrapper);
  MBB.erase(CallIt);
  return NewIt;
}

std::vector<MachineOutliner::StatepointHashCallTargetGroup>
MachineOutliner::collectStatepointGroupsByHashAndCallTarget() const {
  std::vector<StatepointHashCallTargetGroup> Groups;
  DenseMap<const MachineFunction *, int64_t> FunctionFPToSPOffsetMap;
  DenseMap<const MachineInstr *, std::string> ExtendedStackMapValueMap;
  StringMap<unsigned> GroupIndexMap;

  for (const auto &Entry : StackMapValueMap) {
    const MachineInstr *MI = Entry.first;
    if (!MI || MI->getOpcode() != TargetOpcode::STATEPOINT)
      continue;

    const MachineFunction *MF = MI->getMF();
    if (!MF)
      continue;

    FunctionFPToSPOffsetMap.try_emplace(MF, getFunctionFPToSPOffset(*MF));
  }

  for (const auto &Entry : StackMapValueMap) {
    const MachineInstr *MI = Entry.first;
    if (!MI || MI->getOpcode() != TargetOpcode::STATEPOINT)
      continue;

    const MachineFunction *MF = MI->getMF();
    if (!MF)
      continue;

    auto OffsetIt = FunctionFPToSPOffsetMap.find(MF);
    if (OffsetIt == FunctionFPToSPOffsetMap.end())
      continue;

    std::string ExtendedHash = Entry.second;
    ExtendedHash += "|fp-sp=" + std::to_string(OffsetIt->second);
    ExtendedStackMapValueMap[MI] = ExtendedHash;

    std::string GroupKey = ExtendedHash;
    GroupKey += "|call-target=" + getStatepointCallTargetKey(*MI);
    auto GroupIt = GroupIndexMap.find(GroupKey);
    if (GroupIt != GroupIndexMap.end()) {
      StatepointHashCallTargetGroup &Group = Groups[GroupIt->second];
      Group.Statepoints.push_back(MI);
      continue;
    }

    StatepointHashCallTargetGroup Group;
    Group.Hash = ExtendedStackMapValueMap[MI];
    Group.FPToSPOffset = OffsetIt->second;
    Group.Representative = MI;
    Group.Statepoints.push_back(MI);
    GroupIndexMap[GroupKey] = Groups.size();
    Groups.push_back(std::move(Group));
  }

  return Groups;
}

void MachineOutliner::findCangjieStatepointStackMapCandidates(
    InstructionMapper &Mapper, std::vector<OutlinedFunction> &FunctionList) {
  FunctionList.clear();

  std::vector<StatepointHashCallTargetGroup> Groups =
      collectStatepointGroupsByHashAndCallTarget();
  for (StatepointHashCallTargetGroup &Group : Groups) {
    if (Group.Statepoints.size() < 2)
      continue;

    std::vector<Candidate> CandidatesForGroup;
    for (const MachineInstr *ConstMI : Group.Statepoints) {
      MachineInstr *MI = const_cast<MachineInstr *>(ConstMI);
      if (!MI || MI->getOpcode() != TargetOpcode::STATEPOINT)
        continue;
      if (StatepointHasNonEmptyDerivedStartIdx.lookup(MI))
        continue;

      if (isCangjieThrowExceptionStatepoint(*MI))
        continue;
      if (isCangjieStackCheckStatepoint(*MI))
        continue;

      MachineBasicBlock *MBB = MI->getParent();
      if (!MBB)
        continue;

      if (isMachineOutlinerCreatedFunction(*MBB->getParent()))
        continue;

      const TargetInstrInfo *TII =
          MBB->getParent()->getSubtarget().getInstrInfo();
      unsigned Flags = 0;
      if (!TII->isMBBSafeToOutlineFrom(*MBB, Flags))
        continue;

      unsigned StartIdx = Mapper.UnsignedVec.size();
      MachineBasicBlock::iterator It = MI->getIterator();
      Mapper.InstrList.push_back(It);
      Mapper.UnsignedVec.push_back(StartIdx);
      Mapper.MBBFlagsMap[MBB] = Flags;

      Candidate C(StartIdx, /*Len=*/1, It, It, MBB, FunctionList.size(),
                  Flags);
      C.IsCangjieStatepointStackMapCandidate = true;
      C.CangjieStatepointStackMapFPToSPOffset = Group.FPToSPOffset;
      CandidatesForGroup.push_back(C);
    }

    if (CandidatesForGroup.size() < 2)
      continue;

    const TargetInstrInfo *TII =
        CandidatesForGroup[0].getMF()->getSubtarget().getInstrInfo();
    OutlinedFunction OF = TII->getOutliningCandidateInfo(CandidatesForGroup);
    if (OF.Candidates.size() < 2)
      continue;

    if (OF.getBenefit() < 1) {
      emitNotOutliningCheaperRemark(/*StringLen=*/1, CandidatesForGroup, OF);
      continue;
    }

    FunctionList.push_back(std::move(OF));
  }
}

void MachineOutliner::findMultiStatepointHashCandidates(
    InstructionMapper &Mapper, std::vector<OutlinedFunction> &FunctionList) {
  FunctionList.clear();

  if (StackMapValueMap.empty() || Mapper.UnsignedVec.empty())
    return;

  SuffixTree MultiStatepointST(Mapper.UnsignedVec);
  std::vector<Candidate> CandidatesForRepeatedSeq;
  unsigned GroupIdx = 0;
  
  for (const SuffixTree::RepeatedSubstring &RS : MultiStatepointST) {
    CandidatesForRepeatedSeq.clear();
    unsigned StringLen = RS.Length;

    for (const unsigned &StartIdx : RS.StartIndices) {
      unsigned EndIdx = StartIdx + StringLen - 1;

      // Keep non-overlapping candidates only.
      if (!llvm::all_of(CandidatesForRepeatedSeq, [&StartIdx, &EndIdx](
                                                     const Candidate &C) {
            return (EndIdx < C.getStartIdx() || StartIdx > C.getEndIdx());
          }))
        continue;

      // The dedicated suffix-tree flow only considers legal mapped ranges.
      bool AllLegal = true;
      for (unsigned I = StartIdx; I <= EndIdx; ++I) {
        if (!isMappedLegalInstruction(Mapper, I)) {
          AllLegal = false;
          break;
        }
      }
      if (!AllLegal)
        continue;

      MachineBasicBlock::iterator StartIt = Mapper.InstrList[StartIdx];
      MachineBasicBlock::iterator EndIt = Mapper.InstrList[EndIdx];
      MachineBasicBlock *MBB = StartIt->getParent();
      if (MBB != EndIt->getParent())
        continue;

      // Multi-STATEPOINT outlining must not fall back to the target tail-call
      // or thunk path. Keep only candidates that can be emitted as a normal
      // call to the outlined helper.
      if (EndIt->isTerminator() ||
          (EndIt->isCall() && EndIt->getOpcode() != TargetOpcode::STATEPOINT))
        continue;

      bool AllStatepointsReplaceable = true;
      for (auto It = StartIt;; ++It) {
        if (!canReplaceStatepointWithRegularCall(*It)) {
          AllStatepointsReplaceable = false;
          break;
        }
        if (It == EndIt)
          break;
      }
      if (!AllStatepointsReplaceable)
        continue;

      if (hasCangjieSpecificCallInRange(StartIt, EndIt))
        continue;

      CandidatesForRepeatedSeq.emplace_back(StartIdx, StringLen, StartIt, EndIt,
                                            MBB, FunctionList.size(),
                                            Mapper.MBBFlagsMap[MBB]);
    }

    if (CandidatesForRepeatedSeq.size() < 2)
      continue;

    std::vector<unsigned> StatepointOffsets =
        getStatepointInstructionOffsets(CandidatesForRepeatedSeq.front());
    if (StatepointOffsets.size() < 2)
      continue;

    std::vector<Candidate> ConsistentStatepointCandidates;
    unsigned BestCandidateCount = 0;
    unsigned BestTrimmedLen = 0;
    unsigned BestStatepointCount = 0;

    for (unsigned BeginSP = 0, EndSPCount = StatepointOffsets.size();
         BeginSP + 1 < EndSPCount; ++BeginSP) {
      for (unsigned EndSP = BeginSP + 1; EndSP < EndSPCount; ++EndSP) {
        unsigned TrimStartOffset =
            (BeginSP == 0) ? 0 : StatepointOffsets[BeginSP - 1] + 1;
        unsigned MinTrimEndOffset = StatepointOffsets[EndSP];
        unsigned TrimEndOffset =
            (EndSP + 1 == EndSPCount) ? (StringLen - 1)
                                      : (StatepointOffsets[EndSP + 1] - 1);

        MachineBasicBlock::iterator FirstStartIt =
            std::next(CandidatesForRepeatedSeq.front().front(), TrimStartOffset);
        bool FoundSupportedEnd = false;
        while (TrimEndOffset >= MinTrimEndOffset) {
          MachineBasicBlock::iterator FirstEndIt =
              std::next(CandidatesForRepeatedSeq.front().front(),
                        TrimEndOffset);
          if (isSupportedMultiStatepointRange(FirstStartIt, FirstEndIt)) {
            FoundSupportedEnd = true;
            break;
          }
          if (TrimEndOffset == MinTrimEndOffset)
            break;
          --TrimEndOffset;
        }
        if (!FoundSupportedEnd)
          continue;

        std::vector<Candidate> TrimmedCandidates;
        for (Candidate &C : CandidatesForRepeatedSeq) {
          Candidate TrimmedCandidate;
          if (buildTrimmedConsistentCandidate(StackMapValueMap, C,
                                              TrimStartOffset, TrimEndOffset,
                                              TrimmedCandidate))
            TrimmedCandidates.push_back(TrimmedCandidate);
        }
        if (TrimmedCandidates.size() < 2)
          continue;

        unsigned TrimmedLen = TrimEndOffset - TrimStartOffset + 1;
        unsigned StatepointCount = EndSP - BeginSP + 1;
        if (TrimmedCandidates.size() > BestCandidateCount ||
            (TrimmedCandidates.size() == BestCandidateCount &&
             TrimmedLen > BestTrimmedLen) ||
            (TrimmedCandidates.size() == BestCandidateCount &&
             TrimmedLen == BestTrimmedLen &&
             StatepointCount > BestStatepointCount)) {
          BestCandidateCount = TrimmedCandidates.size();
          BestTrimmedLen = TrimmedLen;
          BestStatepointCount = StatepointCount;
          ConsistentStatepointCandidates = std::move(TrimmedCandidates);
        }
      }
    }

    unsigned CurrentGroupIdx = GroupIdx + 1;
    if (ConsistentStatepointCandidates.size() < 2) {
      (void)CurrentGroupIdx;
      continue;
    }

    GroupIdx = CurrentGroupIdx;

    for (Candidate &C : ConsistentStatepointCandidates)
      C.FunctionIdx = FunctionList.size();

    const TargetInstrInfo *TII =
        ConsistentStatepointCandidates[0].getMF()->getSubtarget().getInstrInfo();
    OutlinedFunction OF =
        TII->getOutliningCandidateInfo(ConsistentStatepointCandidates);

    if (OF.Candidates.size() < 2)
      continue;

    unsigned GroupStringLen = ConsistentStatepointCandidates[0].getLength();
    if (OF.getBenefit() < 1) {
      emitNotOutliningCheaperRemark(GroupStringLen,
                                    ConsistentStatepointCandidates, OF);
      continue;
    }

    FunctionList.push_back(std::move(OF));
  }
}

void MachineOutliner::findCandidates(
    InstructionMapper &Mapper, std::vector<OutlinedFunction> &FunctionList) {
  FunctionList.clear();
  SuffixTree ST(Mapper.UnsignedVec);

  // First, find all of the repeated substrings in the tree of minimum length
  // 2.
  std::vector<Candidate> CandidatesForRepeatedSeq;
  for (const SuffixTree::RepeatedSubstring &RS : ST) {
    CandidatesForRepeatedSeq.clear();
    unsigned StringLen = RS.Length;
    for (const unsigned &StartIdx : RS.StartIndices) {
      unsigned EndIdx = StartIdx + StringLen - 1;
      // Trick: Discard some candidates that would be incompatible with the
      // ones we've already found for this sequence. This will save us some
      // work in candidate selection.
      //
      // If two candidates overlap, then we can't outline them both. This
      // happens when we have candidates that look like, say
      //
      // AA (where each "A" is an instruction).
      //
      // We might have some portion of the module that looks like this:
      // AAAAAA (6 A's)
      //
      // In this case, there are 5 different copies of "AA" in this range, but
      // at most 3 can be outlined. If only outlining 3 of these is going to
      // be unbeneficial, then we ought to not bother.
      //
      // Note that two things DON'T overlap when they look like this:
      // start1...end1 .... start2...end2
      // That is, one must either
      // * End before the other starts
      // * Start after the other ends
      if (llvm::all_of(CandidatesForRepeatedSeq, [&StartIdx,
                                                  &EndIdx](const Candidate &C) {
            return (EndIdx < C.getStartIdx() || StartIdx > C.getEndIdx());
          })) {
        // It doesn't overlap with anything, so we can outline it.
        // Each sequence is over [StartIt, EndIt].
        // Save the candidate and its location.

        MachineBasicBlock::iterator StartIt = Mapper.InstrList[StartIdx];
        MachineBasicBlock::iterator EndIt = Mapper.InstrList[EndIdx];
        MachineBasicBlock *MBB = StartIt->getParent();

        if (hasStatePointBeforeEnd(StartIt, EndIt) || isCangjieSpecificCall(&*EndIt))
           continue;

        CandidatesForRepeatedSeq.emplace_back(StartIdx, StringLen, StartIt,
                                              EndIt, MBB, FunctionList.size(),
                                              Mapper.MBBFlagsMap[MBB]);
      }
    }

    // We've found something we might want to outline.
    // Create an OutlinedFunction to store it and check if it'd be beneficial
    // to outline.
    if (CandidatesForRepeatedSeq.size() < 2)
      continue;

    // Arbitrarily choose a TII from the first candidate.
    // FIXME: Should getOutliningCandidateInfo move to TargetMachine?
    const TargetInstrInfo *TII =
        CandidatesForRepeatedSeq[0].getMF()->getSubtarget().getInstrInfo();

    OutlinedFunction OF =
        TII->getOutliningCandidateInfo(CandidatesForRepeatedSeq);

    // If we deleted too many candidates, then there's nothing worth outlining.
    // FIXME: This should take target-specified instruction sizes into account.
    if (OF.Candidates.size() < 2)
      continue;

    // Is it better to outline this candidate than not?
    if (OF.getBenefit() < 1) {
      emitNotOutliningCheaperRemark(StringLen, CandidatesForRepeatedSeq, OF);
      continue;
    }

    FunctionList.push_back(OF);
  }
}

MachineFunction *MachineOutliner::createOutlinedFunction(
    Module &M, OutlinedFunction &OF, InstructionMapper &Mapper, unsigned Name) {

  // Create the function name. This should be unique.
  // FIXME: We should have a better naming scheme. This should be stable,
  // regardless of changes to the outliner's cost model/traversal order.
  std::string FunctionName = "OUTLINED_FUNCTION_";
  if (OutlineRepeatedNum > 0)
    FunctionName += std::to_string(OutlineRepeatedNum + 1) + "_";
  FunctionName += std::to_string(Name);

  // Create the function using an IR-level function.
  LLVMContext &C = M.getContext();
  Function *F = Function::Create(FunctionType::get(Type::getVoidTy(C), false),
                                 Function::ExternalLinkage, FunctionName, M);

  Candidate &FirstCand = OF.Candidates.front();
  if (isMultiStatepointCandidate(FirstCand) ||
      FirstCand.IsCangjieStatepointStackMapCandidate) {
    F->setGC("cangjie");
    F->setCallingConv(CallingConv::CangjieGC);
    F->addFnAttr("cangjie-outlined-multistatepoint");
    if (FirstCand.IsCangjieStatepointStackMapCandidate)
      F->addFnAttr(
          CangjieOutlinedStatepointFPToSPOffsetAttr,
          std::to_string(FirstCand.CangjieStatepointStackMapFPToSPOffset));
  }

  // NOTE: If this is linkonceodr, then we can take advantage of linker deduping
  // which gives us better results when we outline from linkonceodr functions.
  F->setLinkage(GlobalValue::InternalLinkage);
  F->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

  // Set optsize/minsize, so we don't insert padding between outlined
  // functions.
  F->addFnAttr(Attribute::OptimizeForSize);
  F->addFnAttr(Attribute::MinSize);

  const TargetInstrInfo &TII =
      *FirstCand.getMF()->getSubtarget().getInstrInfo();

  TII.mergeOutliningCandidateAttributes(*F, OF.Candidates);

  // Set uwtable, so we generate eh_frame.
  UWTableKind UW = std::accumulate(
      OF.Candidates.cbegin(), OF.Candidates.cend(), UWTableKind::None,
      [](UWTableKind K, const outliner::Candidate &C) {
        return std::max(K, C.getMF()->getFunction().getUWTableKind());
      });
  if (UW != UWTableKind::None)
    F->setUWTableKind(UW);

  BasicBlock *EntryBB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> Builder(EntryBB);
  Builder.CreateRetVoid();

  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfoWrapperPass>().getMMI();
  MachineFunction &MF = MMI.getOrCreateMachineFunction(*F);
  MachineBasicBlock &MBB = *MF.CreateMachineBasicBlock();

  // Insert the new function into the module.
  MF.insert(MF.begin(), &MBB);

  MachineFunction *OriginalMF = FirstCand.front()->getMF();
  const std::vector<MCCFIInstruction> &Instrs =
      OriginalMF->getFrameInstructions();
  for (auto I = FirstCand.front(), E = std::next(FirstCand.back()); I != E;
       ++I) {
    if (I->isDebugInstr())
      continue;

    // Don't keep debug information for outlined instructions.
    auto DL = DebugLoc();
    if (I->isCFIInstruction()) {
      unsigned CFIIndex = I->getOperand(0).getCFIIndex();
      MCCFIInstruction CFI = Instrs[CFIIndex];
      BuildMI(MBB, MBB.end(), DL, TII.get(TargetOpcode::CFI_INSTRUCTION))
          .addCFIIndex(MF.addFrameInst(CFI));
    } else {
      MachineInstr *NewMI = MF.CloneMachineInstr(&*I);
      NewMI->dropMemRefs(MF);
      NewMI->setDebugLoc(DL);
      copyStatepointAllocaObjectAllocations(*I, MF);
      MBB.insert(MBB.end(), NewMI);
    }
  }

  // Set normal properties for a late MachineFunction.
  MF.getProperties().reset(MachineFunctionProperties::Property::IsSSA);
  MF.getProperties().set(MachineFunctionProperties::Property::NoPHIs);
  MF.getProperties().set(MachineFunctionProperties::Property::NoVRegs);
  MF.getProperties().set(MachineFunctionProperties::Property::TracksLiveness);
  MF.getRegInfo().freezeReservedRegs(MF);

  // Compute live-in set for outlined fn
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetRegisterInfo &TRI = *MRI.getTargetRegisterInfo();
  LivePhysRegs LiveIns(TRI);
  for (auto &Cand : OF.Candidates) {
    // Figure out live-ins at the first instruction.
    MachineBasicBlock &OutlineBB = *Cand.front()->getParent();
    LivePhysRegs CandLiveIns(TRI);
    CandLiveIns.addLiveOuts(OutlineBB);
    for (const MachineInstr &MI :
         reverse(make_range(Cand.front(), OutlineBB.end())))
      CandLiveIns.stepBackward(MI);

    // The live-in set for the outlined function is the union of the live-ins
    // from all the outlining points.
    for (MCPhysReg Reg : CandLiveIns)
      LiveIns.addReg(Reg);
  }
  addLiveIns(MBB, LiveIns);

  TII.buildOutlinedFrame(MBB, MF, OF);

  // If there's a DISubprogram associated with this outlined function, then
  // emit debug info for the outlined function.
  if (DISubprogram *SP = getSubprogramOrNull(OF)) {
    // We have a DISubprogram. Get its DICompileUnit.
    DICompileUnit *CU = SP->getUnit();
    DIBuilder DB(M, true, CU);
    DIFile *Unit = SP->getFile();
    Mangler Mg;
    // Get the mangled name of the function for the linkage name.
    std::string Dummy;
    llvm::raw_string_ostream MangledNameStream(Dummy);
    Mg.getNameWithPrefix(MangledNameStream, F, false);

    DISubprogram *OutlinedSP = DB.createFunction(
        Unit /* Context */, F->getName(), StringRef(MangledNameStream.str()),
        Unit /* File */,
        0 /* Line 0 is reserved for compiler-generated code. */,
        DB.createSubroutineType(DB.getOrCreateTypeArray(None)), /* void type */
        0, /* Line 0 is reserved for compiler-generated code. */
        DINode::DIFlags::FlagArtificial /* Compiler-generated code. */,
        /* Outlined code is optimized code by definition. */
        DISubprogram::SPFlagDefinition | DISubprogram::SPFlagOptimized);

    // Don't add any new variables to the subprogram.
    DB.finalizeSubprogram(OutlinedSP);

    // Attach subprogram to the function.
    F->setSubprogram(OutlinedSP);
    // We're done with the DIBuilder.
    DB.finalize();
  }

  return &MF;
}

bool MachineOutliner::outline(Module &M,
                              std::vector<OutlinedFunction> &FunctionList,
                              InstructionMapper &Mapper,
                              unsigned &OutlinedFunctionNum) {

  bool OutlinedSomething = false;

  // Sort by benefit. The most beneficial functions should be outlined first.
  llvm::stable_sort(FunctionList, [](const OutlinedFunction &LHS,
                                     const OutlinedFunction &RHS) {
    return LHS.getBenefit() > RHS.getBenefit();
  });

  // Walk over each function, outlining them as we go along. Functions are
  // outlined greedily, based off the sort above.
  for (OutlinedFunction &OF : FunctionList) {
    // If we outlined something that overlapped with a candidate in a previous
    // step, then we can't outline from it.
    erase_if(OF.Candidates, [&Mapper](Candidate &C) {
      return std::any_of(
          Mapper.UnsignedVec.begin() + C.getStartIdx(),
          Mapper.UnsignedVec.begin() + C.getEndIdx() + 1,
          [](unsigned I) { return (I == static_cast<unsigned>(-1)); });
    });
    // If we made it unbeneficial to outline this function, skip it.
    if (OF.getBenefit() < 1)
      continue;

    // It's beneficial. Create the function and outline its sequence's
    // occurrences.
    OF.MF = createOutlinedFunction(M, OF, Mapper, OutlinedFunctionNum);
    emitOutlinedFunctionRemark(OF);
    FunctionsCreated++;
    OutlinedFunctionNum++; // Created a function, move to the next name.
    MachineFunction *MF = OF.MF;
    const TargetSubtargetInfo &STI = MF->getSubtarget();
    const TargetInstrInfo &TII = *STI.getInstrInfo();

    // Replace occurrences of the sequence with calls to the new function.
    for (Candidate &C : OF.Candidates) {
      MachineBasicBlock &MBB = *C.getMBB();
      MachineBasicBlock::iterator StartIt = C.front();
      MachineBasicBlock::iterator EndIt = C.back();
      MachineInstr *PrototypeStatepoint = findFirstStatepointInCandidate(C);
      bool NeedsStatepointWrapper =
          !C.IsCangjieStatepointStackMapCandidate && PrototypeStatepoint &&
          shouldWrapOutlinedCallWithStatepoint(C, StackMapValueMap);

      // Insert the call.
      auto CallInst = TII.insertOutlinedCall(M, MBB, StartIt, *MF, C);
      if (NeedsStatepointWrapper) {
        bool StartItPointsToInsertedCall = (StartIt == CallInst);
        CallInst = replaceOutlinedCallWithStatepoint(MBB, CallInst, *MF,
                                                     *PrototypeStatepoint);
        if (StartItPointsToInsertedCall)
          StartIt = CallInst;
      }

      // If the caller tracks liveness, then we need to make sure that
      // anything we outline doesn't break liveness assumptions. The outlined
      // functions themselves currently don't track liveness, but we should
      // make sure that the ranges we yank things out of aren't wrong.
      if (MBB.getParent()->getProperties().hasProperty(
              MachineFunctionProperties::Property::TracksLiveness)) {
        // The following code is to add implicit def operands to the call
        // instruction. It also updates call site information for moved
        // code.
        SmallSet<Register, 2> UseRegs, DefRegs;
        // Copy over the defs in the outlined range.
        // First inst in outlined range <-- Anything that's defined in this
        // ...                           .. range has to be added as an
        // implicit Last inst in outlined range  <-- def to the call
        // instruction. Also remove call site information for outlined block
        // of code. The exposed uses need to be copied in the outlined range.
        for (MachineBasicBlock::reverse_iterator
                 Iter = EndIt.getReverse(),
                 Last = std::next(CallInst.getReverse());
             Iter != Last; Iter++) {
          MachineInstr *MI = &*Iter;
          SmallSet<Register, 2> InstrUseRegs;
          for (MachineOperand &MOP : MI->operands()) {
            // Skip over anything that isn't a register.
            if (!MOP.isReg())
              continue;

            if (MOP.isDef()) {
              // Introduce DefRegs set to skip the redundant register.
              DefRegs.insert(MOP.getReg());
              if (UseRegs.count(MOP.getReg()) &&
                  !InstrUseRegs.count(MOP.getReg()))
                // Since the regiester is modeled as defined,
                // it is not necessary to be put in use register set.
                UseRegs.erase(MOP.getReg());
            } else if (!MOP.isUndef()) {
              // Any register which is not undefined should
              // be put in the use register set.
              UseRegs.insert(MOP.getReg());
              InstrUseRegs.insert(MOP.getReg());
            }
          }
          if (MI->isCandidateForCallSiteEntry())
            MI->getMF()->eraseCallSiteInfo(MI);
        }

        for (const Register &I : DefRegs)
          // If it's a def, add it to the call instruction.
          if (!CallInst->definesRegister(I))
            CallInst->addOperand(
                MachineOperand::CreateReg(I, true, /* isDef = true */
                                          true /* isImp = true */));

        for (const Register &I : UseRegs)
          // If it's a exposed use, add it to the call instruction.
          if (!CallInst->readsRegister(I))
            CallInst->addOperand(
                MachineOperand::CreateReg(I, false, /* isDef = false */
                                          true /* isImp = true */));
      }

      // Erase from the point after where the call was inserted up to, and
      // including, the final instruction in the sequence.
      // Erase needs one past the end, so we need std::next there too.
      MBB.erase(std::next(StartIt), std::next(EndIt));

      // Keep track of what we removed by marking them all as -1.
      for (unsigned &I :
           llvm::make_range(Mapper.UnsignedVec.begin() + C.getStartIdx(),
                            Mapper.UnsignedVec.begin() + C.getEndIdx() + 1))
        I = static_cast<unsigned>(-1);
      OutlinedSomething = true;

      // Statistics.
      NumOutlined++;
    }
  }

  LLVM_DEBUG(dbgs() << "OutlinedSomething = " << OutlinedSomething << "\n";);
  return OutlinedSomething;
}

void MachineOutliner::populateMapper(
    InstructionMapper &Mapper, Module &M, MachineModuleInfo &MMI,
    bool TreatStatepointAsLegal,
    const SmallPtrSetImpl<MachineFunction *> *AllowedMFs) {
  // Build instruction mappings for each function in the module. Start by
  // iterating over each Function in M.
  for (Function &F : M) {

    // If there's nothing in F, then there's no reason to try and outline from
    // it.
    if (F.empty())
      continue;

    // There's something in F. Check if it has a MachineFunction associated with
    // it.
    MachineFunction *MF = MMI.getMachineFunction(F);

    // If it doesn't, then there's nothing to outline from. Move to the next
    // Function.
    if (!MF)
      continue;
    if (isMachineOutlinerCreatedFunction(*MF))
      continue;
    if (AllowedMFs && !AllowedMFs->count(MF))
      continue;

    const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();

    if (!RunOnAllFunctions && !TII->shouldOutlineFromFunctionByDefault(*MF))
      continue;

    // We have a MachineFunction. Ask the target if it's suitable for outlining.
    // If it isn't, then move on to the next Function in the module.
    if (!TII->isFunctionSafeToOutlineFrom(*MF, OutlineFromLinkOnceODRs))
      continue;

    // We have a function suitable for outlining. Iterate over every
    // MachineBasicBlock in MF and try to map its instructions to a list of
    // unsigned integers.
    for (MachineBasicBlock &MBB : *MF) {
      // If there isn't anything in MBB, then there's no point in outlining from
      // it.
      // If there are fewer than 2 instructions in the MBB, then it can't ever
      // contain something worth outlining.
      // FIXME: This should be based off of the maximum size in B of an outlined
      // call versus the size in B of the MBB.
      if (MBB.empty() || MBB.size() < 2)
        continue;

      // Check if MBB could be the target of an indirect branch. If it is, then
      // we don't want to outline from it.
      if (MBB.hasAddressTaken())
        continue;

      // MBB is suitable for outlining. Map it to a list of unsigneds.
      Mapper.convertToUnsignedVec(MBB, *TII, TreatStatepointAsLegal);
    }

    // Statistics.
    UnsignedVecSize = Mapper.UnsignedVec.size();
  }
}

void MachineOutliner::collectStatepointStackMaps(Module &M,
                                                 MachineModuleInfo &MMI) {
  StatepointToStackMapIdx.clear();
  StackMapValueMap.clear();
  StatepointHasNonEmptyDerivedStartIdx.clear();
  if (!ASM || !SM)
    return;

  SM->reset();
  for (Function &F : M) {
    if (F.empty())
      continue;

    MachineFunction *MF = MMI.getMachineFunction(F);
    if (!MF)
      continue;
    if (isMachineOutlinerCreatedFunction(*MF))
      continue;

    const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();
    if (!RunOnAllFunctions && !TII->shouldOutlineFromFunctionByDefault(*MF))
      continue;
    if (!TII->isFunctionSafeToOutlineFrom(*MF, OutlineFromLinkOnceODRs))
      continue;
    ASM->emitStatepointStackMaps(*MF);
  }

  const auto &InstrToCallsiteIdx = SM->getInstrToCallsiteIndexMap();
  for (const auto &Entry : InstrToCallsiteIdx) {
    const MachineInstr *MI = Entry.first;
    if (MI->getOpcode() != TargetOpcode::STATEPOINT || Entry.second.empty())
      continue;

    assert(Entry.second.size() == 1 &&
           "expected a single stack map record per statepoint");
    StatepointToStackMapIdx[MI] = Entry.second.front();
  }

  for (Function &F : M) {
    if (F.empty())
      continue;

    MachineFunction *MF = MMI.getMachineFunction(F);
    if (!MF)
      continue;
    if (isMachineOutlinerCreatedFunction(*MF))
      continue;

    const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();
    if (!RunOnAllFunctions && !TII->shouldOutlineFromFunctionByDefault(*MF))
      continue;
    if (!TII->isFunctionSafeToOutlineFrom(*MF, OutlineFromLinkOnceODRs))
      continue;

    for (MachineBasicBlock &MBB : *MF) {
      for (MachineInstr &MI : MBB) {
        if (MI.getOpcode() != TargetOpcode::STATEPOINT)
          continue;

        auto It = StatepointToStackMapIdx.find(&MI);
        if (It == StatepointToStackMapIdx.end()) {
          continue;
        }

        assert(It->second < SM->getCSInfos().size() &&
               "invalid stack map index");

        std::unique_ptr<CompressedInfo> Data = createStatepointCompressedInfo(MI);
        if (!Data)
          continue;

        StatepointHasNonEmptyDerivedStartIdx[&MI] =
            hasNonEmptyDerivedStartIdx(*Data);
        StackMapValueMap[&MI] = SM->getCompressedInfoHash(*Data);
      }
    }
  }

  // The stack map records collected here are only used to compute stable
  // hashes for multi-STATEPOINT outlining. Do not let these analysis-only
  // records leak into the final stackmap section; final emission must only see
  // STATEPOINTs that still exist after outlining.
  SM->reset();
  StatepointToStackMapIdx.clear();
}

const StackMaps::CallsiteInfo *
MachineOutliner::getStatepointCallsiteInfo(const MachineInstr &MI) const {
  if (!SM || MI.getOpcode() != TargetOpcode::STATEPOINT)
    return nullptr;

  auto It = StatepointToStackMapIdx.find(&MI);
  if (It == StatepointToStackMapIdx.end())
    return nullptr;

  const auto &CSInfos = SM->getCSInfos();
  if (It->second >= CSInfos.size())
    return nullptr;

  return &CSInfos[It->second];
}

std::unique_ptr<CompressedInfo>
MachineOutliner::createStatepointCompressedInfo(const MachineInstr &MI) const {
  if (!SM || MI.getOpcode() != TargetOpcode::STATEPOINT)
    return nullptr;

  auto It = StatepointToStackMapIdx.find(&MI);
  if (It == StatepointToStackMapIdx.end())
    return nullptr;

  return SM->createCompressedInfoForCallsite(It->second);
}

std::string MachineOutliner::getStatepointCompressedInfoHash(
    const MachineInstr &MI) const {
  if (!SM || MI.getOpcode() != TargetOpcode::STATEPOINT)
    return "";

  auto It = StatepointToStackMapIdx.find(&MI);
  if (It == StatepointToStackMapIdx.end())
    return "";

  return SM->getCompressedInfoHashForCallsite(It->second);
}

void MachineOutliner::initSizeRemarkInfo(
    const Module &M, const MachineModuleInfo &MMI,
    StringMap<unsigned> &FunctionToInstrCount) {
  // Collect instruction counts for every function. We'll use this to emit
  // per-function size remarks later.
  for (const Function &F : M) {
    MachineFunction *MF = MMI.getMachineFunction(F);

    // We only care about MI counts here. If there's no MachineFunction at this
    // point, then there won't be after the outliner runs, so let's move on.
    if (!MF)
      continue;
    FunctionToInstrCount[F.getName().str()] = MF->getInstructionCount();
  }
}

void MachineOutliner::emitInstrCountChangedRemark(
    const Module &M, const MachineModuleInfo &MMI,
    const StringMap<unsigned> &FunctionToInstrCount) {
  // Iterate over each function in the module and emit remarks.
  // Note that we won't miss anything by doing this, because the outliner never
  // deletes functions.
  for (const Function &F : M) {
    MachineFunction *MF = MMI.getMachineFunction(F);

    // The outliner never deletes functions. If we don't have a MF here, then we
    // didn't have one prior to outlining either.
    if (!MF)
      continue;

    std::string Fname = std::string(F.getName());
    unsigned FnCountAfter = MF->getInstructionCount();
    unsigned FnCountBefore = 0;

    // Check if the function was recorded before.
    auto It = FunctionToInstrCount.find(Fname);

    // Did we have a previously-recorded size? If yes, then set FnCountBefore
    // to that.
    if (It != FunctionToInstrCount.end())
      FnCountBefore = It->second;

    // Compute the delta and emit a remark if there was a change.
    int64_t FnDelta = static_cast<int64_t>(FnCountAfter) -
                      static_cast<int64_t>(FnCountBefore);
    if (FnDelta == 0)
      continue;

    MachineOptimizationRemarkEmitter MORE(*MF, nullptr);
    MORE.emit([&]() {
      MachineOptimizationRemarkAnalysis R("size-info", "FunctionMISizeChange",
                                          DiagnosticLocation(), &MF->front());
      R << DiagnosticInfoOptimizationBase::Argument("Pass", "Machine Outliner")
        << ": Function: "
        << DiagnosticInfoOptimizationBase::Argument("Function", F.getName())
        << ": MI instruction count changed from "
        << DiagnosticInfoOptimizationBase::Argument("MIInstrsBefore",
                                                    FnCountBefore)
        << " to "
        << DiagnosticInfoOptimizationBase::Argument("MIInstrsAfter",
                                                    FnCountAfter)
        << "; Delta: "
        << DiagnosticInfoOptimizationBase::Argument("Delta", FnDelta);
      return R;
    });
  }
}

bool MachineOutliner::runOnModule(Module &M) {
  // Check if there's anything in the module. If it's empty, then there's
  // nothing to outline.
  if (M.empty())
    return false;

  // Number to append to the current outlined function.
  unsigned OutlinedFunctionNum = 0;
  bool Changed = false;

  OutlineRepeatedNum = 0;
  Changed = doOutline(M, OutlinedFunctionNum);

  for (unsigned I = 0; Changed && I < OutlinerReruns; ++I) {
    OutlinedFunctionNum = 0;
    OutlineRepeatedNum++;
    if (!doOutline(M, OutlinedFunctionNum)) {
      LLVM_DEBUG({
        dbgs() << "Did not outline on iteration " << I + 2 << " out of "
               << OutlinerReruns + 1 << "\n";
      });
      break;
    }
  }

  return Changed;
}

bool MachineOutliner::doOutline(Module &M, unsigned &OutlinedFunctionNum) {
  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfoWrapperPass>().getMMI();

  if (!ASM) {
    auto &Target = MMI.getTarget().getTarget();
    auto &TM = const_cast<LLVMTargetMachine &>(MMI.getTarget());
    std::unique_ptr<MCStreamer> MCS(Target.createNullStreamer(MMI.getContext()));
    ASM =
        std::unique_ptr<AsmPrinter>(Target.createAsmPrinter(TM, std::move(MCS)));
    if (ASM)
      ASM->setResolver(getResolver());
    SM = ASM ? ASM->getStackMaps() : nullptr;
  }

  // If the user passed -enable-machine-outliner=always or
  // -enable-machine-outliner, the pass will run on all functions in the module.
  // Otherwise, if the target supports default outlining, it will run on all
  // functions deemed by the target to be worth outlining from by default. Tell
  // the user how the outliner is running.
  LLVM_DEBUG({
    dbgs() << "Machine Outliner: Running on ";
    if (RunOnAllFunctions)
      dbgs() << "all functions";
    else
      dbgs() << "target-default functions";
    dbgs() << "\n";
  });

  // If the user specifies that they want to outline from linkonceodrs, set
  // it here.
  OutlineFromLinkOnceODRs = EnableLinkOnceODROutlining;
  collectStatepointStackMaps(M, MMI);

  SmallPtrSet<MachineFunction *, 32> OriginalMFs;
  for (Function &F : M) {
    if (MachineFunction *MF = MMI.getMachineFunction(F))
      OriginalMFs.insert(MF);
  }

  std::vector<OutlinedFunction> StatepointStackMapFunctionList;
  std::vector<OutlinedFunction> MultiStatepointFunctionList;
  std::vector<OutlinedFunction> FunctionList;

  // If we've requested size remarks, then collect the MI counts of every
  // function before outlining, and the MI counts after outlining.
  // FIXME: This shouldn't be in the outliner at all; it should ultimately be
  // the pass manager's responsibility.
  // This could pretty easily be placed in outline instead, but because we
  // really ultimately *don't* want this here, it's done like this for now
  // instead.

  // Check if we want size remarks.
  bool ShouldEmitSizeRemarks = M.shouldEmitInstrCountChangedRemark();
  StringMap<unsigned> FunctionToInstrCount;
  if (ShouldEmitSizeRemarks)
    initSizeRemarkInfo(M, MMI, FunctionToInstrCount);

  InstructionMapper StatepointStackMapMapper;
  findCangjieStatepointStackMapCandidates(StatepointStackMapMapper,
                                          StatepointStackMapFunctionList);

  bool OutlinedSomething = false;
  OutlinedSomething |= outline(M, StatepointStackMapFunctionList,
                               StatepointStackMapMapper, OutlinedFunctionNum);

  InstructionMapper MultiStatepointMapper;

  // Build a dedicated mapper for the multi-STATEPOINT flow where STATEPOINT
  // stays legal inside repeated substrings.
  populateMapper(MultiStatepointMapper, M, MMI,
                 /*TreatStatepointAsLegal=*/true, &OriginalMFs);

  // Find and prioritize the dedicated multi-STATEPOINT candidate groups.
  findMultiStatepointHashCandidates(MultiStatepointMapper,
                                    MultiStatepointFunctionList);

  /*OutlinedSomething |= outline(M, MultiStatepointFunctionList,
                               MultiStatepointMapper, OutlinedFunctionNum);*/

  InstructionMapper RegularMapper;

  // Rebuild the regular mapper after the multi-STATEPOINT pass so the normal
  // outliner sees the updated code, while still skipping newly created
  // outlined functions from this same iteration.
  populateMapper(RegularMapper, M, MMI,
                 /*TreatStatepointAsLegal=*/false, &OriginalMFs);

  // Find all of the regular outlining candidates.
  findCandidates(RegularMapper, FunctionList);
  /*OutlinedSomething |=
      outline(M, FunctionList, RegularMapper, OutlinedFunctionNum);*/

  // If we outlined something, we definitely changed the MI count of the
  // module. If we've asked for size remarks, then output them.
  // FIXME: This should be in the pass manager.
  if (ShouldEmitSizeRemarks && OutlinedSomething)
    emitInstrCountChangedRemark(M, MMI, FunctionToInstrCount);


  LLVM_DEBUG({
    if (!OutlinedSomething)
      dbgs() << "Stopped outlining at iteration " << OutlineRepeatedNum
             << " because no changes were found.\n";
  });

  if (ASM) 
    ASM->setResolver(NULL);
  return OutlinedSomething;
}
