//===-- StatepointLowering.cpp - SDAGBuilder's statepoint code -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file includes support code use by SelectionDAGBuilder when lowering a
// statepoint sequence in SelectionDAG IR.
//
//===----------------------------------------------------------------------===//

#include "StatepointLowering.h"
#include "SelectionDAGBuilder.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/FunctionLoweringInfo.h"
#include "llvm/CodeGen/GCMetadata.h"
#include "llvm/CodeGen/GCStrategy.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/StackMaps.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/Target/TargetLowering.h"
#include <algorithm>
using namespace llvm;

#define DEBUG_TYPE "statepoint-lowering"

STATISTIC(NumSlotsAllocatedForStatepoints,
          "Number of stack slots allocated for statepoints");
STATISTIC(NumOfStatepoints, "Number of statepoint nodes encountered");
STATISTIC(StatepointMaxSlotsRequired,
          "Maximum number of stack slots required for a singe statepoint");

void StatepointLoweringState::startNewStatepoint(SelectionDAGBuilder &Builder) {
  // Consistency check
  assert(PendingGCRelocateCalls.empty() &&
         "Trying to visit statepoint before finished processing previous one");
  Locations.clear();
  RelocLocations.clear();
  NextSlotToAllocate = 0;
  // Need to resize this on each safepoint - we need the two to stay in
  // sync and the clear patterns of a SelectionDAGBuilder have no relation
  // to FunctionLoweringInfo.
  AllocatedStackSlots.resize(Builder.FuncInfo.StatepointStackSlots.size());
  for (size_t i = 0; i < AllocatedStackSlots.size(); i++) {
    AllocatedStackSlots[i] = false;
  }
}
void StatepointLoweringState::clear() {
  Locations.clear();
  RelocLocations.clear();
  AllocatedStackSlots.clear();
  assert(PendingGCRelocateCalls.empty() &&
         "cleared before statepoint sequence completed");
}

SDValue
StatepointLoweringState::allocateStackSlot(EVT ValueType,
                                           SelectionDAGBuilder &Builder) {

  NumSlotsAllocatedForStatepoints++;

  // The basic scheme here is to first look for a previously created stack slot
  // which is not in use (accounting for the fact arbitrary slots may already
  // be reserved), or to create a new stack slot and use it.

  // If this doesn't succeed in 40000 iterations, something is seriously wrong
  for (int i = 0; i < 40000; i++) {
    assert(Builder.FuncInfo.StatepointStackSlots.size() ==
               AllocatedStackSlots.size() &&
           "broken invariant");
    const size_t NumSlots = AllocatedStackSlots.size();
    assert(NextSlotToAllocate <= NumSlots && "broken invariant");

    if (NextSlotToAllocate >= NumSlots) {
      assert(NextSlotToAllocate == NumSlots);
      // record stats
      if (NumSlots + 1 > StatepointMaxSlotsRequired) {
        StatepointMaxSlotsRequired = NumSlots + 1;
      }

      SDValue SpillSlot = Builder.DAG.CreateStackTemporary(ValueType);
      const unsigned FI = cast<FrameIndexSDNode>(SpillSlot)->getIndex();
      Builder.FuncInfo.StatepointStackSlots.push_back(FI);
      AllocatedStackSlots.push_back(true);
      return SpillSlot;
    }
    if (!AllocatedStackSlots[NextSlotToAllocate]) {
      const int FI = Builder.FuncInfo.StatepointStackSlots[NextSlotToAllocate];
      AllocatedStackSlots[NextSlotToAllocate] = true;
      return Builder.DAG.getFrameIndex(FI, ValueType);
    }
    // Note: We deliberately choose to advance this only on the failing path.
    // Doing so on the suceeding path involes a bit of complexity that caused a
    // minor bug previously.  Unless performance shows this matters, please
    // keep this code as simple as possible.
    NextSlotToAllocate++;
  }
  llvm_unreachable("infinite loop?");
}

/// Try to find existing copies of the incoming values in stack slots used for
/// statepoint spilling.  If we can find a spill slot for the incoming value,
/// mark that slot as allocated, and reuse the same slot for this safepoint.
/// This helps to avoid series of loads and stores that only serve to resuffle
/// values on the stack between calls.
static void reservePreviousStackSlotForValue(SDValue Incoming,
                                             SelectionDAGBuilder &Builder) {

  if (isa<ConstantSDNode>(Incoming) || isa<FrameIndexSDNode>(Incoming)) {
    // We won't need to spill this, so no need to check for previously
    // allocated stack slots
    return;
  }

  SDValue Loc = Builder.StatepointLowering.getLocation(Incoming);
  if (Loc.getNode()) {
    // duplicates in input
    return;
  }

  // Search back for the load from a stack slot pattern to find the original
  // slot we allocated for this value.  We could extend this to deal with
  // simple modification patterns, but simple dealing with trivial load/store
  // sequences helps a lot already.
  if (LoadSDNode *Load = dyn_cast<LoadSDNode>(Incoming)) {
    if (auto *FI = dyn_cast<FrameIndexSDNode>(Load->getBasePtr())) {
      const int Index = FI->getIndex();
      auto Itr = std::find(Builder.FuncInfo.StatepointStackSlots.begin(),
                           Builder.FuncInfo.StatepointStackSlots.end(), Index);
      if (Itr == Builder.FuncInfo.StatepointStackSlots.end()) {
        // not one of the lowering stack slots, can't reuse!
        // TODO: Actually, we probably could reuse the stack slot if the value
        // hasn't changed at all, but we'd need to look for intervening writes
        return;
      } else {
        // This is one of our dedicated lowering slots
        const int Offset =
            std::distance(Builder.FuncInfo.StatepointStackSlots.begin(), Itr);
        if (Builder.StatepointLowering.isStackSlotAllocated(Offset)) {
          // stack slot already assigned to someone else, can't use it!
          // TODO: currently we reserve space for gc arguments after doing
          // normal allocation for deopt arguments.  We should reserve for
          // _all_ deopt and gc arguments, then start allocating.  This
          // will prevent some moves being inserted when vm state changes,
          // but gc state doesn't between two calls.
          return;
        }
        // Reserve this stack slot
        Builder.StatepointLowering.reserveStackSlot(Offset);
      }

      // Cache this slot so we find it when going through the normal
      // assignment loop.
      SDValue Loc =
          Builder.DAG.getTargetFrameIndex(Index, Incoming.getValueType());

      Builder.StatepointLowering.setLocation(Incoming, Loc);
    }
  }

  // TODO: handle case where a reloaded value flows through a phi to
  // another safepoint.  e.g.
  // bb1:
  //  a' = relocated...
  // bb2: % pred: bb1, bb3, bb4, etc.
  //  a_phi = phi(a', ...)
  // statepoint ... a_phi
  // NOTE: This will require reasoning about cross basic block values.  This is
  // decidedly non trivial and this might not be the right place to do it.  We
  // don't really have the information we need here...

  // TODO: handle simple updates.  If a value is modified and the original
  // value is no longer live, it would be nice to put the modified value in the
  // same slot.  This allows folding of the memory accesses for some
  // instructions types (like an increment).
  // statepoint (i)
  // i1 = i+1
  // statepoint (i1)
}

/// Remove any duplicate (as SDValues) from the derived pointer pairs.  This
/// is not required for correctness.  It's purpose is to reduce the size of
/// StackMap section.  It has no effect on the number of spill slots required
/// or the actual lowering.
static void removeDuplicatesGCPtrs(SmallVectorImpl<const Value *> &Bases,
                                   SmallVectorImpl<const Value *> &Ptrs,
                                   SmallVectorImpl<const Value *> &Relocs,
                                   SelectionDAGBuilder &Builder) {

  // This is horribly ineffecient, but I don't care right now
  SmallSet<SDValue, 64> Seen;

  SmallVector<const Value *, 64> NewBases, NewPtrs, NewRelocs;
  for (size_t i = 0; i < Ptrs.size(); i++) {
    SDValue SD = Builder.getValue(Ptrs[i]);
    // Only add non-duplicates
    if (Seen.count(SD) == 0) {
      NewBases.push_back(Bases[i]);
      NewPtrs.push_back(Ptrs[i]);
      NewRelocs.push_back(Relocs[i]);
    }
    Seen.insert(SD);
  }
  assert(Bases.size() >= NewBases.size());
  assert(Ptrs.size() >= NewPtrs.size());
  assert(Relocs.size() >= NewRelocs.size());
  Bases = NewBases;
  Ptrs = NewPtrs;
  Relocs = NewRelocs;
  assert(Ptrs.size() == Bases.size());
  assert(Ptrs.size() == Relocs.size());
}

/// Extract call from statepoint, lower it and return pointer to the
/// call node. Also update NodeMap so that getValue(statepoint) will
/// reference lowered call result
static SDNode *
lowerCallFromStatepoint(ImmutableStatepoint ISP, MachineBasicBlock *LandingPad,
                        SelectionDAGBuilder &Builder,
                        SmallVectorImpl<SDValue> &PendingExports) {

  ImmutableCallSite CS(ISP.getCallSite());

  SDValue ActualCallee = Builder.getValue(ISP.getActualCallee());

  // Handle immediate and symbolic callees.
  if (auto *ConstCallee = dyn_cast<ConstantSDNode>(ActualCallee.getNode()))
    ActualCallee = Builder.DAG.getIntPtrConstant(ConstCallee->getZExtValue(),
                                                 Builder.getCurSDLoc(),
                                                 /*isTarget=*/true);
  else if (auto *SymbolicCallee =
               dyn_cast<GlobalAddressSDNode>(ActualCallee.getNode()))
    ActualCallee = Builder.DAG.getTargetGlobalAddress(
        SymbolicCallee->getGlobal(), SDLoc(SymbolicCallee),
        SymbolicCallee->getValueType(0));

  assert(CS.getCallingConv() != CallingConv::AnyReg &&
         "anyregcc is not supported on statepoints!");

  Type *DefTy = ISP.getActualReturnType();
  bool HasDef = !DefTy->isVoidTy();

  SDValue ReturnValue, CallEndVal;
  std::tie(ReturnValue, CallEndVal) = Builder.lowerCallOperands(
      ISP.getCallSite(), ImmutableStatepoint::CallArgsBeginPos,
      ISP.getNumCallArgs(), ActualCallee, DefTy, LandingPad,
      false /* IsPatchPoint */);

  SDNode *CallEnd = CallEndVal.getNode();

  // Get a call instruction from the call sequence chain.  Tail calls are not
  // allowed.  The following code is essentially reverse engineering X86's
  // LowerCallTo.
  //
  // We are expecting DAG to have the following form:
  //
  // ch = eh_label (only in case of invoke statepoint)
  //   ch, glue = callseq_start ch
  //   ch, glue = X86::Call ch, glue
  //   ch, glue = callseq_end ch, glue
  //   get_return_value ch, glue
  //
  // get_return_value can either be a CopyFromReg to grab the return value from
  // %RAX, or it can be a LOAD to load a value returned by reference via a stack
  // slot.

  if (HasDef && (CallEnd->getOpcode() == ISD::CopyFromReg ||
                 CallEnd->getOpcode() == ISD::LOAD))
    CallEnd = CallEnd->getOperand(0).getNode();

  assert(CallEnd->getOpcode() == ISD::CALLSEQ_END && "expected!");

  if (HasDef) {
    if (CS.isInvoke()) {
      // Result value will be used in different basic block for invokes
      // so we need to export it now. But statepoint call has a different type
      // than the actuall call. It means that standart exporting mechanism will
      // create register of the wrong type. So instead we need to create
      // register with correct type and save value into it manually.
      // TODO: To eliminate this problem we can remove gc.result intrinsics
      //       completelly and make statepoint call to return a tuple.
      unsigned Reg = Builder.FuncInfo.CreateRegs(ISP.getActualReturnType());
      RegsForValue RFV(*Builder.DAG.getContext(),
                       Builder.DAG.getTargetLoweringInfo(), Reg,
                       ISP.getActualReturnType());
      SDValue Chain = Builder.DAG.getEntryNode();

      RFV.getCopyToRegs(ReturnValue, Builder.DAG, Builder.getCurSDLoc(), Chain,
                        nullptr);
      PendingExports.push_back(Chain);
      Builder.FuncInfo.ValueMap[CS.getInstruction()] = Reg;
    } else {
      // The value of the statepoint itself will be the value of call itself.
      // We'll replace the actually call node shortly.  gc_result will grab
      // this value.
      Builder.setValue(CS.getInstruction(), ReturnValue);
    }
  } else {
    // The token value is never used from here on, just generate a poison value
    Builder.setValue(CS.getInstruction(),
                     Builder.DAG.getIntPtrConstant(-1, Builder.getCurSDLoc()));
  }

  return CallEnd->getOperand(0).getNode();
}

/// Callect all gc pointers coming into statepoint intrinsic, clean them up,
/// and return two arrays:
///   Bases - base pointers incoming to this statepoint
///   Ptrs - derived pointers incoming to this statepoint
///   Relocs - the gc_relocate corresponding to each base/ptr pair
/// Elements of this arrays should be in one-to-one correspondence with each
/// other i.e Bases[i], Ptrs[i] are from the same gcrelocate call
static void getIncomingStatepointGCValues(
    SmallVectorImpl<const Value *> &Bases, SmallVectorImpl<const Value *> &Ptrs,
    SmallVectorImpl<const Value *> &Relocs, ImmutableStatepoint StatepointSite,
    SelectionDAGBuilder &Builder) {
  for (GCRelocateOperands relocateOpers :
       StatepointSite.getRelocates(StatepointSite)) {
    Relocs.push_back(relocateOpers.getUnderlyingCallSite().getInstruction());
    Bases.push_back(relocateOpers.getBasePtr());
    Ptrs.push_back(relocateOpers.getDerivedPtr());
  }

  // Remove any redundant llvm::Values which map to the same SDValue as another
  // input.  Also has the effect of removing duplicates in the original
  // llvm::Value input list as well.  This is a useful optimization for
  // reducing the size of the StackMap section.  It has no other impact.
  removeDuplicatesGCPtrs(Bases, Ptrs, Relocs, Builder);

  assert(Bases.size() == Ptrs.size() && Ptrs.size() == Relocs.size());
}

/// Spill a value incoming to the statepoint. It might be either part of
/// vmstate
/// or gcstate. In both cases unconditionally spill it on the stack unless it
/// is a null constant. Return pair with first element being frame index
/// containing saved value and second element with outgoing chain from the
/// emitted store
static std::pair<SDValue, SDValue>
spillIncomingStatepointValue(SDValue Incoming, SDValue Chain,
                             SelectionDAGBuilder &Builder) {
  SDValue Loc = Builder.StatepointLowering.getLocation(Incoming);

  // Emit new store if we didn't do it for this ptr before
  if (!Loc.getNode()) {
    Loc = Builder.StatepointLowering.allocateStackSlot(Incoming.getValueType(),
                                                       Builder);
    assert(isa<FrameIndexSDNode>(Loc));
    int Index = cast<FrameIndexSDNode>(Loc)->getIndex();
    // We use TargetFrameIndex so that isel will not select it into LEA
    Loc = Builder.DAG.getTargetFrameIndex(Index, Incoming.getValueType());

    // TODO: We can create TokenFactor node instead of
    //       chaining stores one after another, this may allow
    //       a bit more optimal scheduling for them
    Chain = Builder.DAG.getStore(Chain, Builder.getCurSDLoc(), Incoming, Loc,
                                 MachinePointerInfo::getFixedStack(Index),
                                 false, false, 0);

    Builder.StatepointLowering.setLocation(Incoming, Loc);
  }

  assert(Loc.getNode());
  return std::make_pair(Loc, Chain);
}

/// Lower a single value incoming to a statepoint node.  This value can be
/// either a deopt value or a gc value, the handling is the same.  We special
/// case constants and allocas, then fall back to spilling if required.
static void lowerIncomingStatepointValue(SDValue Incoming,
                                         SmallVectorImpl<SDValue> &Ops,
                                         SelectionDAGBuilder &Builder) {
  SDValue Chain = Builder.getRoot();

  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Incoming)) {
    // If the original value was a constant, make sure it gets recorded as
    // such in the stackmap.  This is required so that the consumer can
    // parse any internal format to the deopt state.  It also handles null
    // pointers and other constant pointers in GC states
    Ops.push_back(Builder.DAG.getTargetConstant(StackMaps::ConstantOp,
                                                Builder.getCurSDLoc(),
                                                MVT::i64));
    Ops.push_back(Builder.DAG.getTargetConstant(C->getSExtValue(),
                                                Builder.getCurSDLoc(),
                                                MVT::i64));
  } else if (FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(Incoming)) {
    // This handles allocas as arguments to the statepoint (this is only
    // really meaningful for a deopt value.  For GC, we'd be trying to
    // relocate the address of the alloca itself?)
    Ops.push_back(Builder.DAG.getTargetFrameIndex(FI->getIndex(),
                                                  Incoming.getValueType()));
  } else {
    // Otherwise, locate a spill slot and explicitly spill it so it
    // can be found by the runtime later.  We currently do not support
    // tracking values through callee saved registers to their eventual
    // spill location.  This would be a useful optimization, but would
    // need to be optional since it requires a lot of complexity on the
    // runtime side which not all would support.
    std::pair<SDValue, SDValue> Res =
        spillIncomingStatepointValue(Incoming, Chain, Builder);
    Ops.push_back(Res.first);
    Chain = Res.second;
  }

  Builder.DAG.setRoot(Chain);
}

/// Lower deopt state and gc pointer arguments of the statepoint.  The actual
/// lowering is described in lowerIncomingStatepointValue.  This function is
/// responsible for lowering everything in the right position and playing some
/// tricks to avoid redundant stack manipulation where possible.  On
/// completion, 'Ops' will contain ready to use operands for machine code
/// statepoint. The chain nodes will have already been created and the DAG root
/// will be set to the last value spilled (if any were).
static void lowerStatepointMetaArgs(SmallVectorImpl<SDValue> &Ops,
                                    ImmutableStatepoint StatepointSite,
                                    SelectionDAGBuilder &Builder) {

  // Lower the deopt and gc arguments for this statepoint.  Layout will
  // be: deopt argument length, deopt arguments.., gc arguments...

  SmallVector<const Value *, 64> Bases, Ptrs, Relocations;
  getIncomingStatepointGCValues(Bases, Ptrs, Relocations, StatepointSite,
                                Builder);

#ifndef NDEBUG
  // Check that each of the gc pointer and bases we've gotten out of the
  // safepoint is something the strategy thinks might be a pointer into the GC
  // heap.  This is basically just here to help catch errors during statepoint
  // insertion. TODO: This should actually be in the Verifier, but we can't get
  // to the GCStrategy from there (yet).
  GCStrategy &S = Builder.GFI->getStrategy();
  for (const Value *V : Bases) {
    auto Opt = S.isGCManagedPointer(V);
    if (Opt.hasValue()) {
      assert(Opt.getValue() &&
             "non gc managed base pointer found in statepoint");
    }
  }
  for (const Value *V : Ptrs) {
    auto Opt = S.isGCManagedPointer(V);
    if (Opt.hasValue()) {
      assert(Opt.getValue() &&
             "non gc managed derived pointer found in statepoint");
    }
  }
  for (const Value *V : Relocations) {
    auto Opt = S.isGCManagedPointer(V);
    if (Opt.hasValue()) {
      assert(Opt.getValue() && "non gc managed pointer relocated");
    }
  }
#endif

  // Before we actually start lowering (and allocating spill slots for values),
  // reserve any stack slots which we judge to be profitable to reuse for a
  // particular value.  This is purely an optimization over the code below and
  // doesn't change semantics at all.  It is important for performance that we
  // reserve slots for both deopt and gc values before lowering either.
  for (auto I = StatepointSite.vm_state_begin() + 1,
            E = StatepointSite.vm_state_end();
       I != E; ++I) {
    Value *V = *I;
    SDValue Incoming = Builder.getValue(V);
    reservePreviousStackSlotForValue(Incoming, Builder);
  }
  for (unsigned i = 0; i < Bases.size() * 2; ++i) {
    // Even elements will contain base, odd elements - derived ptr
    const Value *V = i % 2 ? Bases[i / 2] : Ptrs[i / 2];
    SDValue Incoming = Builder.getValue(V);
    reservePreviousStackSlotForValue(Incoming, Builder);
  }

  // First, prefix the list with the number of unique values to be
  // lowered.  Note that this is the number of *Values* not the
  // number of SDValues required to lower them.
  const int NumVMSArgs = StatepointSite.getNumTotalVMSArgs();
  Ops.push_back( Builder.DAG.getTargetConstant(StackMaps::ConstantOp,
                                               Builder.getCurSDLoc(),
                                               MVT::i64));
  Ops.push_back(Builder.DAG.getTargetConstant(NumVMSArgs, Builder.getCurSDLoc(),
                                              MVT::i64));

  assert(NumVMSArgs + 1 == std::distance(StatepointSite.vm_state_begin(),
                                         StatepointSite.vm_state_end()));

  // The vm state arguments are lowered in an opaque manner.  We do
  // not know what type of values are contained within.  We skip the
  // first one since that happens to be the total number we lowered
  // explicitly just above.  We could have left it in the loop and
  // not done it explicitly, but it's far easier to understand this
  // way.
  for (auto I = StatepointSite.vm_state_begin() + 1,
            E = StatepointSite.vm_state_end();
       I != E; ++I) {
    const Value *V = *I;
    SDValue Incoming = Builder.getValue(V);
    lowerIncomingStatepointValue(Incoming, Ops, Builder);
  }

  // Finally, go ahead and lower all the gc arguments.  There's no prefixed
  // length for this one.  After lowering, we'll have the base and pointer
  // arrays interwoven with each (lowered) base pointer immediately followed by
  // it's (lowered) derived pointer.  i.e
  // (base[0], ptr[0], base[1], ptr[1], ...)
  for (unsigned i = 0; i < Bases.size() * 2; ++i) {
    // Even elements will contain base, odd elements - derived ptr
    const Value *V = i % 2 ? Bases[i / 2] : Ptrs[i / 2];
    SDValue Incoming = Builder.getValue(V);
    lowerIncomingStatepointValue(Incoming, Ops, Builder);
  }

  // If there are any explicit spill slots passed to the statepoint, record
  // them, but otherwise do not do anything special.  These are user provided
  // allocas and give control over placement to the consumer.  In this case,
  // it is the contents of the slot which may get updated, not the pointer to
  // the alloca
  for (Value *V : StatepointSite.gc_args()) {
    SDValue Incoming = Builder.getValue(V);
    if (FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(Incoming)) {
      // This handles allocas as arguments to the statepoint
      Ops.push_back(Builder.DAG.getTargetFrameIndex(FI->getIndex(),
                                                    Incoming.getValueType()));
    }
  }
}

void SelectionDAGBuilder::visitStatepoint(const CallInst &CI) {
  // Check some preconditions for sanity
  assert(isStatepoint(&CI) &&
         "function called must be the statepoint function");

  LowerStatepoint(ImmutableStatepoint(&CI));
}

void SelectionDAGBuilder::LowerStatepoint(
    ImmutableStatepoint ISP, MachineBasicBlock *LandingPad /*=nullptr*/) {
  // The basic scheme here is that information about both the original call and
  // the safepoint is encoded in the CallInst.  We create a temporary call and
  // lower it, then reverse engineer the calling sequence.

  NumOfStatepoints++;
  // Clear state
  StatepointLowering.startNewStatepoint(*this);

  ImmutableCallSite CS(ISP.getCallSite());

#ifndef NDEBUG
  // Consistency check
  for (const User *U : CS->users()) {
    const CallInst *Call = cast<CallInst>(U);
    if (isGCRelocate(Call))
      StatepointLowering.scheduleRelocCall(*Call);
  }
#endif

#ifndef NDEBUG
  // If this is a malformed statepoint, report it early to simplify debugging.
  // This should catch any IR level mistake that's made when constructing or
  // transforming statepoints.
  ISP.verify();

  // Check that the associated GCStrategy expects to encounter statepoints.
  // TODO: This if should become an assert.  For now, we allow the GCStrategy
  // to be optional for backwards compatibility.  This will only last a short
  // period (i.e. a couple of weeks).
  assert(GFI->getStrategy().useStatepoints() &&
         "GCStrategy does not expect to encounter statepoints");
#endif

  // Lower statepoint vmstate and gcstate arguments
  SmallVector<SDValue, 10> LoweredMetaArgs;
  lowerStatepointMetaArgs(LoweredMetaArgs, ISP, *this);

  // Get call node, we will replace it later with statepoint
  SDNode *CallNode =
      lowerCallFromStatepoint(ISP, LandingPad, *this, PendingExports);

  // Construct the actual STATEPOINT node with all the appropriate arguments
  // and return values.

  // TODO: Currently, all of these operands are being marked as read/write in
  // PrologEpilougeInserter.cpp, we should special case the VMState arguments
  // and flags to be read-only.
  SmallVector<SDValue, 40> Ops;

  // Calculate and push starting position of vmstate arguments
  // Call Node: Chain, Target, {Args}, RegMask, [Glue]
  SDValue Glue;
  if (CallNode->getGluedNode()) {
    // Glue is always last operand
    Glue = CallNode->getOperand(CallNode->getNumOperands() - 1);
  }
  // Get number of arguments incoming directly into call node
  unsigned NumCallRegArgs =
      CallNode->getNumOperands() - (Glue.getNode() ? 4 : 3);
  Ops.push_back(DAG.getTargetConstant(NumCallRegArgs, getCurSDLoc(), MVT::i32));

  // Add call target
  SDValue CallTarget = SDValue(CallNode->getOperand(1).getNode(), 0);
  Ops.push_back(CallTarget);

  // Add call arguments
  // Get position of register mask in the call
  SDNode::op_iterator RegMaskIt;
  if (Glue.getNode())
    RegMaskIt = CallNode->op_end() - 2;
  else
    RegMaskIt = CallNode->op_end() - 1;
  Ops.insert(Ops.end(), CallNode->op_begin() + 2, RegMaskIt);

  // Add a leading constant argument with the Flags and the calling convention
  // masked together
  CallingConv::ID CallConv = CS.getCallingConv();
  int Flags = cast<ConstantInt>(CS.getArgument(2))->getZExtValue();
  assert(Flags == 0 && "not expected to be used");
  Ops.push_back(DAG.getTargetConstant(StackMaps::ConstantOp, getCurSDLoc(),
                                      MVT::i64));
  Ops.push_back(DAG.getTargetConstant(Flags | ((unsigned)CallConv << 1),
                                      getCurSDLoc(), MVT::i64));

  // Insert all vmstate and gcstate arguments
  Ops.insert(Ops.end(), LoweredMetaArgs.begin(), LoweredMetaArgs.end());

  // Add register mask from call node
  Ops.push_back(*RegMaskIt);

  // Add chain
  Ops.push_back(CallNode->getOperand(0));

  // Same for the glue, but we add it only if original call had it
  if (Glue.getNode())
    Ops.push_back(Glue);

  // Compute return values.  Provide a glue output since we consume one as
  // input.  This allows someone else to chain off us as needed.
  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);

  SDNode *StatepointMCNode =
      DAG.getMachineNode(TargetOpcode::STATEPOINT, getCurSDLoc(), NodeTys, Ops);

  // Replace original call
  DAG.ReplaceAllUsesWith(CallNode, StatepointMCNode); // This may update Root
  // Remove originall call node
  DAG.DeleteNode(CallNode);

  // DON'T set the root - under the assumption that it's already set past the
  // inserted node we created.

  // TODO: A better future implementation would be to emit a single variable
  // argument, variable return value STATEPOINT node here and then hookup the
  // return value of each gc.relocate to the respective output of the
  // previously emitted STATEPOINT value.  Unfortunately, this doesn't appear
  // to actually be possible today.
}

void SelectionDAGBuilder::visitGCResult(const CallInst &CI) {
  // The result value of the gc_result is simply the result of the actual
  // call.  We've already emitted this, so just grab the value.
  Instruction *I = cast<Instruction>(CI.getArgOperand(0));
  assert(isStatepoint(I) && "first argument must be a statepoint token");

  if (isa<InvokeInst>(I)) {
    // For invokes we should have stored call result in a virtual register.
    // We can not use default getValue() functionality to copy value from this
    // register because statepoint and actuall call return types can be
    // different, and getValue() will use CopyFromReg of the wrong type,
    // which is always i32 in our case.
    PointerType *CalleeType =
        cast<PointerType>(ImmutableStatepoint(I).getActualCallee()->getType());
    Type *RetTy =
        cast<FunctionType>(CalleeType->getElementType())->getReturnType();
    SDValue CopyFromReg = getCopyFromRegs(I, RetTy);

    assert(CopyFromReg.getNode());
    setValue(&CI, CopyFromReg);
  } else {
    setValue(&CI, getValue(I));
  }
}

void SelectionDAGBuilder::visitGCRelocate(const CallInst &CI) {
#ifndef NDEBUG
  // Consistency check
  StatepointLowering.relocCallVisited(CI);
#endif

  GCRelocateOperands relocateOpers(&CI);
  SDValue SD = getValue(relocateOpers.getDerivedPtr());

  if (isa<ConstantSDNode>(SD) || isa<FrameIndexSDNode>(SD)) {
    // We didn't need to spill these special cases (constants and allocas).
    // See the handling in spillIncomingValueForStatepoint for detail.
    setValue(&CI, SD);
    return;
  }

  SDValue Loc = StatepointLowering.getRelocLocation(SD);
  // Emit new load if we did not emit it before
  if (!Loc.getNode()) {
    SDValue SpillSlot = StatepointLowering.getLocation(SD);
    int FI = cast<FrameIndexSDNode>(SpillSlot)->getIndex();

    // Be conservative: flush all pending loads
    // TODO: Probably we can be less restrictive on this,
    // it may allow more scheduling opprtunities
    SDValue Chain = getRoot();

    Loc = DAG.getLoad(SpillSlot.getValueType(), getCurSDLoc(), Chain, SpillSlot,
                      MachinePointerInfo::getFixedStack(FI), false, false,
                      false, 0);

    StatepointLowering.setRelocLocation(SD, Loc);

    // Again, be conservative, don't emit pending loads
    DAG.setRoot(Loc.getValue(1));
  }

  assert(Loc.getNode());
  setValue(&CI, Loc);
}
