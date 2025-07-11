// This file is part of AsmJit project <https://asmjit.com>
//
// See <asmjit/core.h> or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#ifndef ASMJIT_CORE_RAPASS_P_H_INCLUDED
#define ASMJIT_CORE_RAPASS_P_H_INCLUDED

#include "../core/api-config.h"
#ifndef ASMJIT_NO_COMPILER

#include "../core/compiler.h"
#include "../core/emithelper_p.h"
#include "../core/raassignment_p.h"
#include "../core/radefs_p.h"
#include "../core/rastack_p.h"
#include "../core/support.h"

ASMJIT_BEGIN_NAMESPACE

//! \cond INTERNAL
//! \addtogroup asmjit_ra
//! \{

//! Flags used by \ref RABlock.
enum class RABlockFlags : uint32_t {
  //! No flags.
  kNone = 0,

  //! Block has been constructed from nodes.
  kIsConstructed = 0x00000001u,
  //! Block is reachable (set by `buildCFGViews()`).
  kIsReachable = 0x00000002u,
  //! Block is a target (has an associated label or multiple labels).
  kIsTargetable = 0x00000004u,
  //! Block has been allocated.
  kIsAllocated = 0x00000008u,
  //! Block is a function-exit.
  kIsFuncExit = 0x00000010u,

  //! Block has a terminator (jump, conditional jump, ret).
  kHasTerminator = 0x00000100u,
  //! Block naturally flows to the next block.
  kHasConsecutive = 0x00000200u,
  //! Block has a jump to a jump-table at the end.
  kHasJumpTable = 0x00000400u,
  //! Block contains fixed registers (pre-colored).
  kHasFixedRegs = 0x00000800u,
  //! Block contains function calls.
  kHasFuncCalls = 0x00001000u
};
ASMJIT_DEFINE_ENUM_FLAGS(RABlockFlags)

//! Basic block used by register allocator pass.
class RABlock {
public:
  ASMJIT_NONCOPYABLE(RABlock)

  //! \name Types
  //! \{

  using PhysToWorkMap = RAAssignment::PhysToWorkMap;
  using WorkToPhysMap = RAAssignment::WorkToPhysMap;

  //! \}

  //! \name Constants
  //! \{

  //! Unassigned block id.
  static inline constexpr uint32_t kUnassignedId = 0xFFFFFFFFu;

  static inline constexpr uint32_t kLiveIn = 0;
  static inline constexpr uint32_t kLiveOut = 1;
  static inline constexpr uint32_t kLiveGen = 2;
  static inline constexpr uint32_t kLiveKill = 3;
  static inline constexpr uint32_t kLiveCount = 4;

  //! \}

  //! \name Members
  //! \{

  //! Register allocator pass.
  BaseRAPass* _ra;

  //! Block id (indexed from zero).
  uint32_t _blockId = kUnassignedId;
  //! Block flags, see `Flags`.
  RABlockFlags _flags = RABlockFlags::kNone;

  //! First `BaseNode` of this block (inclusive).
  BaseNode* _first = nullptr;
  //! Last `BaseNode` of this block (inclusive).
  BaseNode* _last = nullptr;

  //! Initial position of this block (inclusive).
  uint32_t _firstPosition = 0;
  //! End position of this block (exclusive).
  uint32_t _endPosition = 0;

  //! Weight of this block (default 0, each loop adds one).
  uint32_t _weight = 0;
  //! Post-order view order, used during POV construction.
  uint32_t _povOrder = 0;

  //! Basic statistics about registers.
  RARegsStats _regsStats = RARegsStats();
  //! Maximum live-count per register group.
  RALiveCount _maxLiveCount = RALiveCount();

  //! Timestamp (used by block visitors).
  mutable uint64_t _timestamp = 0;
  //! Immediate dominator of this block.
  RABlock* _idom = nullptr;

  //! Block predecessors.
  RABlocks _predecessors {};
  //! Block successors.
  RABlocks _successors {};

  //! Liveness in/out/use/kill.
  ZoneBitVector _liveBits[kLiveCount] {};

  //! Shared assignment it or `Globals::kInvalidId` if this block doesn't have shared assignment.
  //! See \ref RASharedAssignment for more details.
  uint32_t _sharedAssignmentId = Globals::kInvalidId;
  //! Scratch registers that cannot be allocated upon block entry.
  RegMask _entryScratchGpRegs = 0;
  //! Scratch registers used at exit, by a terminator instruction.
  RegMask _exitScratchGpRegs = 0;

  //! Register assignment on entry.
  PhysToWorkMap* _entryPhysToWorkMap = nullptr;

  //! \}

  //! \name Construction & Destruction
  //! \{

  ASMJIT_INLINE_NODEBUG RABlock(BaseRAPass* ra) noexcept
    : _ra(ra) {}

  //! \}

  //! \name Accessors
  //! \{

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG BaseRAPass* pass() const noexcept { return _ra; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG ZoneAllocator* allocator() const noexcept;

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t blockId() const noexcept { return _blockId; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RABlockFlags flags() const noexcept { return _flags; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasFlag(RABlockFlags flag) const noexcept { return Support::test(_flags, flag); }

  ASMJIT_INLINE_NODEBUG void addFlags(RABlockFlags flags) noexcept { _flags |= flags; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool isAssigned() const noexcept { return _blockId != kUnassignedId; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool isConstructed() const noexcept { return hasFlag(RABlockFlags::kIsConstructed); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool isReachable() const noexcept { return hasFlag(RABlockFlags::kIsReachable); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool isTargetable() const noexcept { return hasFlag(RABlockFlags::kIsTargetable); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool isAllocated() const noexcept { return hasFlag(RABlockFlags::kIsAllocated); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool isFuncExit() const noexcept { return hasFlag(RABlockFlags::kIsFuncExit); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasTerminator() const noexcept { return hasFlag(RABlockFlags::kHasTerminator); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasConsecutive() const noexcept { return hasFlag(RABlockFlags::kHasConsecutive); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasJumpTable() const noexcept { return hasFlag(RABlockFlags::kHasJumpTable); }

  ASMJIT_INLINE_NODEBUG void makeConstructed(const RARegsStats& regStats) noexcept {
    _flags |= RABlockFlags::kIsConstructed;
    _regsStats.combineWith(regStats);
  }

  ASMJIT_INLINE_NODEBUG void makeReachable() noexcept { _flags |= RABlockFlags::kIsReachable; }
  ASMJIT_INLINE_NODEBUG void makeTargetable() noexcept { _flags |= RABlockFlags::kIsTargetable; }
  ASMJIT_INLINE_NODEBUG void makeAllocated() noexcept { _flags |= RABlockFlags::kIsAllocated; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RARegsStats& regsStats() const noexcept { return _regsStats; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasPredecessors() const noexcept { return !_predecessors.empty(); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasSuccessors() const noexcept { return !_successors.empty(); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasSuccessor(RABlock* block) noexcept {
    if (block->_predecessors.size() < _successors.size())
      return block->_predecessors.contains(this);
    else
      return _successors.contains(block);
  }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RABlocks& predecessors() const noexcept { return _predecessors; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RABlocks& successors() const noexcept { return _successors; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG BaseNode* first() const noexcept { return _first; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG BaseNode* last() const noexcept { return _last; }

  ASMJIT_INLINE_NODEBUG void setFirst(BaseNode* node) noexcept { _first = node; }
  ASMJIT_INLINE_NODEBUG void setLast(BaseNode* node) noexcept { _last = node; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t firstPosition() const noexcept { return _firstPosition; }

  ASMJIT_INLINE_NODEBUG void setFirstPosition(uint32_t position) noexcept { _firstPosition = position; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t endPosition() const noexcept { return _endPosition; }

  ASMJIT_INLINE_NODEBUG void setEndPosition(uint32_t position) noexcept { _endPosition = position; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t povOrder() const noexcept { return _povOrder; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RegMask entryScratchGpRegs() const noexcept;

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RegMask exitScratchGpRegs() const noexcept { return _exitScratchGpRegs; }

  ASMJIT_INLINE_NODEBUG void addEntryScratchGpRegs(RegMask regMask) noexcept { _entryScratchGpRegs |= regMask; }
  ASMJIT_INLINE_NODEBUG void addExitScratchGpRegs(RegMask regMask) noexcept { _exitScratchGpRegs |= regMask; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasSharedAssignmentId() const noexcept { return _sharedAssignmentId != Globals::kInvalidId; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t sharedAssignmentId() const noexcept { return _sharedAssignmentId; }

  ASMJIT_INLINE_NODEBUG void setSharedAssignmentId(uint32_t id) noexcept { _sharedAssignmentId = id; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint64_t timestamp() const noexcept { return _timestamp; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasTimestamp(uint64_t ts) const noexcept { return _timestamp == ts; }

  ASMJIT_INLINE_NODEBUG void setTimestamp(uint64_t ts) const noexcept { _timestamp = ts; }

  ASMJIT_INLINE_NODEBUG void resetTimestamp() const noexcept { _timestamp = 0; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RABlock* consecutive() const noexcept { return hasConsecutive() ? _successors[0] : nullptr; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RABlock* iDom() noexcept { return _idom; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RABlock* iDom() const noexcept { return _idom; }

  ASMJIT_INLINE_NODEBUG void setIDom(RABlock* block) noexcept { _idom = block; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG ZoneBitVector& liveIn() noexcept { return _liveBits[kLiveIn]; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const ZoneBitVector& liveIn() const noexcept { return _liveBits[kLiveIn]; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG ZoneBitVector& liveOut() noexcept { return _liveBits[kLiveOut]; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const ZoneBitVector& liveOut() const noexcept { return _liveBits[kLiveOut]; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG ZoneBitVector& gen() noexcept { return _liveBits[kLiveGen]; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const ZoneBitVector& gen() const noexcept { return _liveBits[kLiveGen]; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG ZoneBitVector& kill() noexcept { return _liveBits[kLiveKill]; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const ZoneBitVector& kill() const noexcept { return _liveBits[kLiveKill]; }

  [[nodiscard]]
  inline Error resizeLiveBits(uint32_t size) noexcept {
    ASMJIT_PROPAGATE(_liveBits[kLiveIn  ].resize(allocator(), size));
    ASMJIT_PROPAGATE(_liveBits[kLiveOut ].resize(allocator(), size));
    ASMJIT_PROPAGATE(_liveBits[kLiveGen ].resize(allocator(), size));
    ASMJIT_PROPAGATE(_liveBits[kLiveKill].resize(allocator(), size));
    return kErrorOk;
  }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasEntryAssignment() const noexcept { return _entryPhysToWorkMap != nullptr; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG PhysToWorkMap* entryPhysToWorkMap() const noexcept { return _entryPhysToWorkMap; }

  ASMJIT_INLINE_NODEBUG void setEntryAssignment(PhysToWorkMap* physToWorkMap) noexcept { _entryPhysToWorkMap = physToWorkMap; }

  //! \}

  //! \name Utilities
  //! \{

  //! Adds a successor to this block, and predecessor to `successor`, making connection on both sides.
  //!
  //! This API must be used to manage successors and predecessors, never manage it manually.
  [[nodiscard]]
  Error appendSuccessor(RABlock* successor) noexcept;

  //! Similar to `appendSuccessor()`, but does prepend instead append.
  //!
  //! This function is used to add a natural flow (always first) to the block.
  [[nodiscard]]
  Error prependSuccessor(RABlock* successor) noexcept;

  //! \}
};

//! Register allocator's data associated with each `InstNode`.
class RAInst {
public:
  ASMJIT_NONCOPYABLE(RAInst)

  //! \name Members
  //! \{

  //! Parent block.
  RABlock* _block;
  //! Instruction RW flags.
  InstRWFlags _instRWFlags;
  //! Aggregated RATiedFlags from all operands & instruction specific flags.
  RATiedFlags _flags;
  //! Total count of RATiedReg's.
  uint32_t _tiedTotal;
  //! Index of RATiedReg's per register group.
  RARegIndex _tiedIndex;
  //! Count of RATiedReg's per register group.
  RARegCount _tiedCount;
  //! Number of live, and thus interfering VirtReg's at this point.
  RALiveCount _liveCount;
  //! Fixed physical registers used.
  RARegMask _usedRegs;
  //! Clobbered registers (by a function call).
  RARegMask _clobberedRegs;
  //! Tied registers.
  RATiedReg _tiedRegs[1];

  //! \}

  //! \name Construction & Destruction
  //! \{

  inline RAInst(RABlock* block, InstRWFlags instRWFlags, RATiedFlags tiedFlags, uint32_t tiedTotal, const RARegMask& clobberedRegs) noexcept {
    _block = block;
    _instRWFlags = instRWFlags;
    _flags = tiedFlags;
    _tiedTotal = tiedTotal;
    _tiedIndex.reset();
    _tiedCount.reset();
    _liveCount.reset();
    _usedRegs.reset();
    _clobberedRegs = clobberedRegs;
  }

  //! \}

  //! \name Accessors
  //! \{

  //! Returns instruction RW flags.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG InstRWFlags instRWFlags() const noexcept { return _instRWFlags; };

  //! Tests whether the given `flag` is present in instruction RW flags.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasInstRWFlag(InstRWFlags flag) const noexcept { return Support::test(_instRWFlags, flag); }

  //! Adds `flags` to instruction RW flags.
  ASMJIT_INLINE_NODEBUG void addInstRWFlags(InstRWFlags flags) noexcept { _instRWFlags |= flags; }

  //! Returns the instruction flags.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RATiedFlags flags() const noexcept { return _flags; }

  //! Tests whether the instruction has flag `flag`.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasFlag(RATiedFlags flag) const noexcept { return Support::test(_flags, flag); }

  //! Replaces the existing instruction flags with `flags`.
  ASMJIT_INLINE_NODEBUG void setFlags(RATiedFlags flags) noexcept { _flags = flags; }

  //! Adds instruction `flags` to this RAInst.
  ASMJIT_INLINE_NODEBUG void addFlags(RATiedFlags flags) noexcept { _flags |= flags; }

  //! Clears instruction `flags` from  this RAInst.
  ASMJIT_INLINE_NODEBUG void clearFlags(RATiedFlags flags) noexcept { _flags &= ~flags; }

  //! Tests whether one operand of this instruction has been patched from Reg to Mem.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool isRegToMemPatched() const noexcept { return hasFlag(RATiedFlags::kInst_RegToMemPatched); }

  //! Tests whether this instruction can be transformed to another instruction if necessary.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool isTransformable() const noexcept { return hasFlag(RATiedFlags::kInst_IsTransformable); }

  //! Returns the associated block with this RAInst.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RABlock* block() const noexcept { return _block; }

  //! Returns tied registers (all).
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RATiedReg* tiedRegs() const noexcept { return const_cast<RATiedReg*>(_tiedRegs); }

  //! Returns tied registers for a given `group`.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RATiedReg* tiedRegs(RegGroup group) const noexcept { return const_cast<RATiedReg*>(_tiedRegs) + _tiedIndex.get(group); }

  //! Returns count of all tied registers.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t tiedCount() const noexcept { return _tiedTotal; }

  //! Returns count of tied registers of a given `group`.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t tiedCount(RegGroup group) const noexcept { return _tiedCount[group]; }

  //! Returns `RATiedReg` at the given `index`.
  [[nodiscard]]
  inline RATiedReg* tiedAt(uint32_t index) const noexcept {
    ASMJIT_ASSERT(index < _tiedTotal);
    return tiedRegs() + index;
  }

  //! Returns `RATiedReg` at the given `index` of the given register `group`.
  [[nodiscard]]
  inline RATiedReg* tiedOf(RegGroup group, uint32_t index) const noexcept {
    ASMJIT_ASSERT(index < _tiedCount.get(group));
    return tiedRegs(group) + index;
  }

  [[nodiscard]]
  inline const RATiedReg* tiedRegForWorkReg(RegGroup group, uint32_t workId) const noexcept {
    const RATiedReg* array = tiedRegs(group);
    size_t count = tiedCount(group);

    for (size_t i = 0; i < count; i++) {
      const RATiedReg* tiedReg = &array[i];
      if (tiedReg->workId() == workId) {
        return tiedReg;
      }
    }

    return nullptr;
  }

  inline void setTiedAt(uint32_t index, RATiedReg& tied) noexcept {
    ASMJIT_ASSERT(index < _tiedTotal);
    _tiedRegs[index] = tied;
  }

  //! \name Static Functions
  //! \{

  [[nodiscard]]
  static ASMJIT_INLINE_NODEBUG size_t sizeOf(uint32_t tiedRegCount) noexcept {
    return sizeof(RAInst) - sizeof(RATiedReg) + tiedRegCount * sizeof(RATiedReg);
  }

  //! \}
};

//! A helper class that is used to build an array of RATiedReg items that are then copied to `RAInst`.
class RAInstBuilder {
public:
  ASMJIT_NONCOPYABLE(RAInstBuilder)

  //! \name Members
  //! \{

  //! Basic block id.
  uint32_t _basicBlockId;
  //! Instruction RW flags.
  InstRWFlags _instRWFlags;

  //! Flags combined from all RATiedReg's.
  RATiedFlags _aggregatedFlags;
  //! Flags that will be cleared before storing the aggregated flags to `RAInst`.
  RATiedFlags _forbiddenFlags;
  RARegCount _count;
  RARegsStats _stats;

  RARegMask _used;
  RARegMask _clobbered;

  //! Current tied register in `_tiedRegs`.
  RATiedReg* _cur;
  //! Array of temporary tied registers.
  RATiedReg _tiedRegs[128];

  //! \}

  //! \name Construction & Destruction
  //! \{

  ASMJIT_INLINE_NODEBUG explicit RAInstBuilder(uint32_t blockId = Globals::kInvalidId) noexcept { reset(blockId); }

  ASMJIT_INLINE_NODEBUG void init(uint32_t blockId) noexcept { reset(blockId); }
  ASMJIT_INLINE_NODEBUG void reset(uint32_t blockId) noexcept {
    _basicBlockId = blockId;
    _instRWFlags = InstRWFlags::kNone;
    _aggregatedFlags = RATiedFlags::kNone;
    _forbiddenFlags = RATiedFlags::kNone;
    _count.reset();
    _stats.reset();
    _used.reset();
    _clobbered.reset();
    _cur = _tiedRegs;
  }

  //! \}

  //! \name Accessors
  //! \{

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG InstRWFlags instRWFlags() const noexcept { return _instRWFlags; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasInstRWFlag(InstRWFlags flag) const noexcept { return Support::test(_instRWFlags, flag); }

  ASMJIT_INLINE_NODEBUG void addInstRWFlags(InstRWFlags flags) noexcept { _instRWFlags |= flags; }

  ASMJIT_INLINE_NODEBUG void clearInstRWFlags(InstRWFlags flags) noexcept { _instRWFlags &= ~flags; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RATiedFlags aggregatedFlags() const noexcept { return _aggregatedFlags; }

  ASMJIT_INLINE_NODEBUG void addAggregatedFlags(RATiedFlags flags) noexcept { _aggregatedFlags |= flags; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RATiedFlags forbiddenFlags() const noexcept { return _forbiddenFlags; }

  ASMJIT_INLINE_NODEBUG void addForbiddenFlags(RATiedFlags flags) noexcept { _forbiddenFlags |= flags; }

  //! Returns the number of tied registers added to the builder.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t tiedRegCount() const noexcept { return uint32_t((size_t)(_cur - _tiedRegs)); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RATiedReg* begin() noexcept { return _tiedRegs; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RATiedReg* end() noexcept { return _cur; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RATiedReg* begin() const noexcept { return _tiedRegs; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RATiedReg* end() const noexcept { return _cur; }

  //! Returns `RATiedReg` at the given `index`.
  [[nodiscard]]
  inline RATiedReg* operator[](size_t index) noexcept {
    ASMJIT_ASSERT(index < tiedRegCount());
    return &_tiedRegs[index];
  }

  //! Returns `RATiedReg` at the given `index`. (const).
  [[nodiscard]]
  inline const RATiedReg* operator[](size_t index) const noexcept {
    ASMJIT_ASSERT(index < tiedRegCount());
    return &_tiedRegs[index];
  }

  //! \}

  //! \name Utilities
  //! \{

  [[nodiscard]]
  Error add(
    RAWorkReg* workReg,
    RATiedFlags flags,
    RegMask useRegMask, uint32_t useId, uint32_t useRewriteMask,
    RegMask outRegMask, uint32_t outId, uint32_t outRewriteMask,
    uint32_t rmSize = 0,
    uint32_t consecutiveParent = Globals::kInvalidId
  ) noexcept {
    RegGroup group = workReg->group();
    RATiedReg* tiedReg = workReg->tiedReg();

    if (useId != Reg::kIdBad) {
      _stats.makeFixed(group);
      _used[group] |= Support::bitMask(useId);
      flags |= RATiedFlags::kUseFixed;
    }

    if (outId != Reg::kIdBad) {
      _clobbered[group] |= Support::bitMask(outId);
      flags |= RATiedFlags::kOutFixed;
    }

    _aggregatedFlags |= flags;
    _stats.makeUsed(group);

    if (!tiedReg) {
      // Would happen when the builder is not reset properly after each instruction - so catch that!
      ASMJIT_ASSERT(tiedRegCount() < ASMJIT_ARRAY_SIZE(_tiedRegs));

      tiedReg = _cur++;
      tiedReg->init(workReg->workId(), flags, useRegMask, useId, useRewriteMask, outRegMask, outId, outRewriteMask, rmSize, consecutiveParent);

      workReg->setTiedReg(tiedReg);
      workReg->assignBasicBlock(_basicBlockId);

      _count.add(group);
      return kErrorOk;
    }
    else {
      if (consecutiveParent != tiedReg->consecutiveParent()) {
        if (tiedReg->consecutiveParent() != Globals::kInvalidId) {
          return DebugUtils::errored(kErrorInvalidState);
        }
        tiedReg->_consecutiveParent = consecutiveParent;
      }

      if (useId != Reg::kIdBad) {
        if (ASMJIT_UNLIKELY(tiedReg->hasUseId())) {
          return DebugUtils::errored(kErrorOverlappedRegs);
        }
        tiedReg->setUseId(useId);
      }

      if (outId != Reg::kIdBad) {
        if (ASMJIT_UNLIKELY(tiedReg->hasOutId())) {
          return DebugUtils::errored(kErrorOverlappedRegs);
        }
        tiedReg->setOutId(outId);
      }

      tiedReg->addRefCount();
      tiedReg->addFlags(flags);
      tiedReg->_useRegMask &= useRegMask;
      tiedReg->_useRewriteMask |= useRewriteMask;
      tiedReg->_outRegMask &= outRegMask;
      tiedReg->_outRewriteMask |= outRewriteMask;
      tiedReg->_rmSize = uint8_t(Support::max<uint32_t>(tiedReg->rmSize(), rmSize));
      return kErrorOk;
    }
  }

  [[nodiscard]]
  Error addCallArg(RAWorkReg* workReg, uint32_t useId) noexcept {
    ASMJIT_ASSERT(useId != Reg::kIdBad);

    RATiedFlags flags = RATiedFlags::kUse | RATiedFlags::kRead | RATiedFlags::kUseFixed;
    RegGroup group = workReg->group();
    RegMask allocable = Support::bitMask(useId);

    _aggregatedFlags |= flags;
    _used[group] |= allocable;
    _stats.makeFixed(group);
    _stats.makeUsed(group);

    RATiedReg* tiedReg = workReg->tiedReg();
    if (!tiedReg) {
      // Could happen when the builder is not reset properly after each instruction.
      ASMJIT_ASSERT(tiedRegCount() < ASMJIT_ARRAY_SIZE(_tiedRegs));

      tiedReg = _cur++;
      tiedReg->init(workReg->workId(), flags, allocable, useId, 0, allocable, Reg::kIdBad, 0);

      workReg->setTiedReg(tiedReg);
      workReg->assignBasicBlock(_basicBlockId);

      _count.add(group);
      return kErrorOk;
    }
    else {
      if (tiedReg->hasUseId()) {
        flags |= RATiedFlags::kDuplicate;
        tiedReg->_useRegMask |= allocable;
      }
      else {
        tiedReg->setUseId(useId);
        tiedReg->_useRegMask &= allocable;
      }

      tiedReg->addRefCount();
      tiedReg->addFlags(flags);
      return kErrorOk;
    }
  }

  [[nodiscard]]
  Error addCallRet(RAWorkReg* workReg, uint32_t outId) noexcept {
    ASMJIT_ASSERT(outId != Reg::kIdBad);

    RATiedFlags flags = RATiedFlags::kOut | RATiedFlags::kWrite | RATiedFlags::kOutFixed;
    RegGroup group = workReg->group();
    RegMask outRegs = Support::bitMask(outId);

    _aggregatedFlags |= flags;
    _used[group] |= outRegs;
    _stats.makeFixed(group);
    _stats.makeUsed(group);

    RATiedReg* tiedReg = workReg->tiedReg();
    if (!tiedReg) {
      // Could happen when the builder is not reset properly after each instruction.
      ASMJIT_ASSERT(tiedRegCount() < ASMJIT_ARRAY_SIZE(_tiedRegs));

      tiedReg = _cur++;
      tiedReg->init(workReg->workId(), flags, Support::allOnes<RegMask>(), Reg::kIdBad, 0, outRegs, outId, 0);

      workReg->setTiedReg(tiedReg);
      workReg->assignBasicBlock(_basicBlockId);

      _count.add(group);
      return kErrorOk;
    }
    else {
      if (tiedReg->hasOutId()) {
        return DebugUtils::errored(kErrorOverlappedRegs);
      }

      tiedReg->addRefCount();
      tiedReg->addFlags(flags);
      tiedReg->setOutId(outId);
      return kErrorOk;
    }
  }

  //! \}
};

//! Intersection of multiple register assignments.
//!
//! See \ref RAAssignment for more information about register assignments.
class RASharedAssignment {
public:
  //! \name Types
  //! \{

  using PhysToWorkMap = RAAssignment::PhysToWorkMap;
  using WorkToPhysMap = RAAssignment::WorkToPhysMap;

  //! \}

  //! \name Members
  //! \{

  //! Bit-mask of registers that cannot be used upon a block entry, for each block that has this shared assignment.
  //! Scratch registers can come from ISA limits (like jecx/loop instructions on x86) or because the registers are
  //! used by jump/branch instruction that uses registers to perform an indirect jump.
  RegMask _entryScratchGpRegs = 0;
  //! Union of all live-in registers.
  ZoneBitVector _liveIn {};
  //! Register assignment (PhysToWork).
  PhysToWorkMap* _physToWorkMap = nullptr;

  //! \}

  //! \name Accessors
  //! \{

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool empty() const noexcept { return _physToWorkMap == nullptr; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RegMask entryScratchGpRegs() const noexcept { return _entryScratchGpRegs; }

  ASMJIT_INLINE_NODEBUG void addEntryScratchGpRegs(RegMask mask) noexcept { _entryScratchGpRegs |= mask; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const ZoneBitVector& liveIn() const noexcept { return _liveIn; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG PhysToWorkMap* physToWorkMap() const noexcept { return _physToWorkMap; }

  ASMJIT_INLINE_NODEBUG void assignPhysToWorkMap(PhysToWorkMap* physToWorkMap) noexcept { _physToWorkMap = physToWorkMap; }

  //! \}
};

//! Register allocation pass used by `BaseCompiler`.
class BaseRAPass : public FuncPass {
public:
  ASMJIT_NONCOPYABLE(BaseRAPass)
  using Base = FuncPass;

  //! \name Constants
  //! \{

  static inline constexpr uint32_t kCallArgWeight = 80;

  //! \}

  //! \name Types
  //! \{

  using PhysToWorkMap = RAAssignment::PhysToWorkMap;
  using WorkToPhysMap = RAAssignment::WorkToPhysMap;

  //! \}

  //! \name Members
  //! \{

  //! Allocator that uses zone passed to `runOnFunction()`.
  ZoneAllocator _allocator {};
  //! Emit helper.
  BaseEmitHelper* _iEmitHelper = nullptr;

  //! Logger, disabled if null.
  Logger* _logger = nullptr;
  //! Format options, copied from Logger, or zeroed if there is no logger.
  FormatOptions _formatOptions {};
  //! Diagnostic options, copied from Emitter, or zeroed if there is no logger.
  DiagnosticOptions _diagnosticOptions {};

  //! Function being processed.
  FuncNode* _func = nullptr;
  //! Stop node.
  BaseNode* _stop = nullptr;
  //! Node that is used to insert extra code after the function body.
  BaseNode* _extraBlock = nullptr;

  //! Blocks (first block is the entry, always exists).
  RABlocks _blocks {};
  //! Function exit blocks (usually one, but can contain more).
  RABlocks _exits {};
  //! Post order view (POV).
  RABlocks _pov {};

  //! Number of instruction nodes.
  uint32_t _instructionCount = 0;
  //! Number of created blocks (internal).
  uint32_t _createdBlockCount = 0;

  //! Shared assignment blocks.
  ZoneVector<RASharedAssignment> _sharedAssignments {};

  //! Timestamp generator (incremental).
  mutable uint64_t _lastTimestamp = 0;

  //! Architecture traits.
  const ArchTraits* _archTraits = nullptr;
  //! Index to physical registers in `RAAssignment::PhysToWorkMap`.
  RARegIndex _physRegIndex = RARegIndex();
  //! Count of physical registers in `RAAssignment::PhysToWorkMap`.
  RARegCount _physRegCount = RARegCount();
  //! Total number of physical registers.
  uint32_t _physRegTotal = 0;
  //! Indexes of a possible scratch registers that can be selected if necessary.
  Support::Array<uint8_t, 2> _scratchRegIndexes {};

  //! Registers available for allocation.
  RARegMask _availableRegs = RARegMask();
  //! Registers clobbered by the function.
  RARegMask _clobberedRegs = RARegMask();

  //! Work registers (registers used by the function).
  RAWorkRegs _workRegs;
  //! Work registers per register group.
  Support::Array<RAWorkRegs, Globals::kNumVirtGroups> _workRegsOfGroup;

  //! Register allocation strategy per register group.
  Support::Array<RAStrategy, Globals::kNumVirtGroups> _strategy;
  //! Global max live-count (from all blocks) per register group.
  RALiveCount _globalMaxLiveCount = RALiveCount();
  //! Global live spans per register group.
  Support::Array<LiveRegSpans*, Globals::kNumVirtGroups> _globalLiveSpans {};
  //! Temporary stack slot.
  Operand _temporaryMem = Operand();

  //! Stack pointer.
  Reg _sp = Reg();
  //! Frame pointer.
  Reg _fp = Reg();
  //! Stack manager.
  RAStackAllocator _stackAllocator {};
  //! Function arguments assignment.
  FuncArgsAssignment _argsAssignment {};
  //! Some StackArgs have to be assigned to StackSlots.
  uint32_t _numStackArgsToStackSlots = 0;

  //! Maximum name-size computed from all WorkRegs.
  uint32_t _maxWorkRegNameSize = 0;
  //! Temporary string builder used to format comments.
  StringTmp<80> _tmpString;

  //! \}

  //! \name Construction & Destruction
  //! \{

  BaseRAPass() noexcept;
  ~BaseRAPass() noexcept override;

  //! \}

  //! \name Accessors
  //! \{

  //! Returns \ref Logger passed to \ref runOnFunction().
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG Logger* logger() const noexcept { return _logger; }

  //! Returns either a valid logger if the given `option` is set and logging is enabled, or nullptr.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG Logger* getLoggerIf(DiagnosticOptions option) const noexcept { return Support::test(_diagnosticOptions, option) ? _logger : nullptr; }

  //! Returns whether the diagnostic `option` is enabled.
  //!
  //! \note Returns false if there is no logger (as diagnostics without logging make no sense).
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasDiagnosticOption(DiagnosticOptions option) const noexcept { return Support::test(_diagnosticOptions, option); }

  //! Returns \ref Zone passed to \ref runOnFunction().
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG Zone* zone() const noexcept { return _allocator.zone(); }

  //! Returns \ref ZoneAllocator used by the register allocator.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG ZoneAllocator* allocator() const noexcept { return const_cast<ZoneAllocator*>(&_allocator); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const ZoneVector<RASharedAssignment>& sharedAssignments() const { return _sharedAssignments; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t sharedAssignmentCount() const noexcept { return _sharedAssignments.size(); }

  //! Returns the current function node.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG FuncNode* func() const noexcept { return _func; }

  //! Returns the stop of the current function.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG BaseNode* stop() const noexcept { return _stop; }

  //! Returns an extra block used by the current function being processed.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG BaseNode* extraBlock() const noexcept { return _extraBlock; }

  //! Sets an extra block, see `extraBlock()`.
  ASMJIT_INLINE_NODEBUG void setExtraBlock(BaseNode* node) noexcept { _extraBlock = node; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t endPosition() const noexcept { return _instructionCount * 2; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RARegMask& availableRegs() const noexcept { return _availableRegs; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RARegMask& clobberedRegs() const noexcept { return _clobberedRegs; }

  //! \}

  //! \name Utilities
  //! \{

  inline void makeUnavailable(RegGroup group, uint32_t regId) noexcept {
    _availableRegs[group] &= ~Support::bitMask(regId);
  }

  inline void makeUnavailable(const RARegMask::RegMasks& regs) noexcept {
    _availableRegs.clear(regs);
  }

  //! Runs the register allocator for the given `func`.
  Error runOnFunction(Zone* zone, Logger* logger, FuncNode* func) override;

  //! Performs all allocation steps sequentially, called by `runOnFunction()`.
  Error onPerformAllSteps() noexcept;

  //! \}

  //! \name Events
  //! \{

  //! Called by \ref runOnFunction() before the register allocation to initialize
  //! architecture-specific data and constraints.
  virtual void onInit() noexcept;

  //! Called by \ref runOnFunction(` after register allocation to clean everything
  //! up. Called even if the register allocation failed.
  virtual void onDone() noexcept;

  //! \}

  //! \name CFG - Basic-Block Management
  //! \{

  //! Returns the function's entry block.
  [[nodiscard]]
  inline RABlock* entryBlock() noexcept {
    ASMJIT_ASSERT(!_blocks.empty());
    return _blocks[0];
  }

  //! \overload
  [[nodiscard]]
  inline const RABlock* entryBlock() const noexcept {
    ASMJIT_ASSERT(!_blocks.empty());
    return _blocks[0];
  }

  //! Returns all basic blocks of this function.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RABlocks& blocks() noexcept { return _blocks; }

  //! \overload
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RABlocks& blocks() const noexcept { return _blocks; }

  //! Returns the count of basic blocks (returns size of `_blocks` array).
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t blockCount() const noexcept { return _blocks.size(); }

  //! Returns the count of reachable basic blocks (returns size of `_pov` array).
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t reachableBlockCount() const noexcept { return _pov.size(); }

  //! Tests whether the CFG has dangling blocks - these were created by `newBlock()`, but not added to CFG through
  //! `addBlocks()`. If `true` is returned and the  CFG is constructed it means that something is missing and it's
  //! incomplete.
  //!
  //! \note This is only used to check if the number of created blocks matches the number of added blocks.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool hasDanglingBlocks() const noexcept { return _createdBlockCount != blockCount(); }

  //! Gest a next timestamp to be used to mark CFG blocks.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint64_t nextTimestamp() const noexcept { return ++_lastTimestamp; }

  //! Creates a new `RABlock` instance.
  //!
  //! \note New blocks don't have ID assigned until they are added to the block array by calling `addBlock()`.
  [[nodiscard]]
  RABlock* newBlock(BaseNode* initialNode = nullptr) noexcept;

  //! Tries to find a neighboring LabelNode (without going through code) that is already connected with `RABlock`.
  //! If no label is found then a new RABlock is created and assigned to all possible labels in a backward direction.
  [[nodiscard]]
  RABlock* newBlockOrExistingAt(LabelNode* cbLabel, BaseNode** stoppedAt = nullptr) noexcept;

  //! Adds the given `block` to the block list and assign it a unique block id.
  [[nodiscard]]
  Error addBlock(RABlock* block) noexcept;

  [[nodiscard]]
  inline Error addExitBlock(RABlock* block) noexcept {
    block->addFlags(RABlockFlags::kIsFuncExit);
    return _exits.append(allocator(), block);
  }

  [[nodiscard]]
  ASMJIT_INLINE RAInst* newRAInst(RABlock* block, InstRWFlags instRWFlags, RATiedFlags flags, uint32_t tiedRegCount, const RARegMask& clobberedRegs) noexcept {
    void* p = zone()->alloc(RAInst::sizeOf(tiedRegCount));
    if (ASMJIT_UNLIKELY(!p)) {
      return nullptr;
    }
    return new(Support::PlacementNew{p}) RAInst(block, instRWFlags, flags, tiedRegCount, clobberedRegs);
  }

  [[nodiscard]]
  ASMJIT_INLINE Error assignRAInst(BaseNode* node, RABlock* block, RAInstBuilder& ib) noexcept {
    uint32_t tiedRegCount = ib.tiedRegCount();
    RAInst* raInst = newRAInst(block, ib.instRWFlags(), ib.aggregatedFlags(), tiedRegCount, ib._clobbered);

    if (ASMJIT_UNLIKELY(!raInst)) {
      return DebugUtils::errored(kErrorOutOfMemory);
    }

    RARegIndex index;
    RATiedFlags flagsFilter = ~ib.forbiddenFlags();

    index.buildIndexes(ib._count);
    raInst->_tiedIndex = index;
    raInst->_tiedCount = ib._count;

    for (uint32_t i = 0; i < tiedRegCount; i++) {
      RATiedReg* tiedReg = ib[i];
      RAWorkReg* workReg = workRegById(tiedReg->workId());

      workReg->resetTiedReg();
      RegGroup group = workReg->group();

      if (tiedReg->hasUseId()) {
        block->addFlags(RABlockFlags::kHasFixedRegs);
        raInst->_usedRegs[group] |= Support::bitMask(tiedReg->useId());
      }

      if (tiedReg->hasOutId()) {
        block->addFlags(RABlockFlags::kHasFixedRegs);
      }

      RATiedReg& dst = raInst->_tiedRegs[index[group]++];
      dst = *tiedReg;
      dst._flags &= flagsFilter;

      if (!tiedReg->isDuplicate()) {
        dst._useRegMask &= ~ib._used[group];
      }
    }

    node->setPassData<RAInst>(raInst);
    return kErrorOk;
  }

  //! \}

  //! \name CFG - Build CFG
  //! \{

  //! Traverse the whole function and do the following:
  //!
  //!   1. Construct CFG (represented by `RABlock`) by populating `_blocks` and `_exits`. Blocks describe the control
  //!      flow of the function and contain some additional information that is used by the register allocator.
  //!
  //!   2. Remove unreachable code immediately. This is not strictly necessary for BaseCompiler itself as the register
  //!      allocator cannot reach such nodes, but keeping instructions that use virtual registers would fail during
  //!      instruction encoding phase (Assembler).
  //!
  //!   3. `RAInst` is created for each `InstNode` or compatible. It contains information that is essential for further
  //!      analysis and register allocation.
  //!
  //! Use `RACFGBuilderT` template that provides the necessary boilerplate.
  [[nodiscard]]
  virtual Error buildCFG() noexcept;

  //! Called after the CFG is built.
  [[nodiscard]]
  Error initSharedAssignments(const ZoneVector<uint32_t>& sharedAssignmentsMap) noexcept;

  //! \}

  //! \name CFG - Views Order
  //! \{

  //! Constructs CFG views (only POV at the moment).
  [[nodiscard]]
  Error buildCFGViews() noexcept;

  //! \}

  //! \name CFG - Dominators
  //! \{

  // Terminology:
  //   - A node `X` dominates a node `Z` if any path from the entry point to `Z` has to go through `X`.
  //   - A node `Z` post-dominates a node `X` if any path from `X` to the end of the graph has to go through `Z`.

  //! Constructs a dominator-tree from CFG.
  [[nodiscard]]
  Error buildCFGDominators() noexcept;

  [[nodiscard]]
  bool _strictlyDominates(const RABlock* a, const RABlock* b) const noexcept;

  [[nodiscard]]
  const RABlock* _nearestCommonDominator(const RABlock* a, const RABlock* b) const noexcept;

  //! Tests whether the basic block `a` dominates `b` - non-strict, returns true when `a == b`.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool dominates(const RABlock* a, const RABlock* b) const noexcept { return a == b ? true : _strictlyDominates(a, b); }

  //! Tests whether the basic block `a` dominates `b` - strict dominance check, returns false when `a == b`.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG bool strictlyDominates(const RABlock* a, const RABlock* b) const noexcept { return a == b ? false : _strictlyDominates(a, b); }

  //! Returns a nearest common dominator of `a` and `b`.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RABlock* nearestCommonDominator(RABlock* a, RABlock* b) const noexcept { return const_cast<RABlock*>(_nearestCommonDominator(a, b)); }

  //! Returns a nearest common dominator of `a` and `b` (const).
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RABlock* nearestCommonDominator(const RABlock* a, const RABlock* b) const noexcept { return _nearestCommonDominator(a, b); }

  //! \}

  //! \name CFG - Utilities
  //! \{

  [[nodiscard]]
  Error removeUnreachableCode() noexcept;

  //! Returns `node` or some node after that is ideal for beginning a new block. This function is mostly used after
  //! a conditional or unconditional jump to select the successor node. In some cases the next node could be a label,
  //! which means it could have assigned some block already.
  [[nodiscard]]
  BaseNode* findSuccessorStartingAt(BaseNode* node) noexcept;

  //! Returns `true` of the `node` can flow to `target` without reaching code nor data. It's used to eliminate jumps
  //! to labels that are next right to them.
  [[nodiscard]]
  bool isNextTo(BaseNode* node, BaseNode* target) noexcept;

  //! \}

  //! \name Virtual Register Management
  //! \{

  //! Returns a native size of the general-purpose register of the target architecture.
  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t registerSize() const noexcept { return _sp.size(); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RAWorkReg* workRegById(uint32_t workId) const noexcept { return _workRegs[workId]; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RAWorkRegs& workRegs() noexcept { return _workRegs; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG RAWorkRegs& workRegs(RegGroup group) noexcept { return _workRegsOfGroup[group]; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RAWorkRegs& workRegs() const noexcept { return _workRegs; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG const RAWorkRegs& workRegs(RegGroup group) const noexcept { return _workRegsOfGroup[group]; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t workRegCount() const noexcept { return _workRegs.size(); }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t workRegCount(RegGroup group) const noexcept { return _workRegsOfGroup[group].size(); }

  inline void _buildPhysIndex() noexcept {
    _physRegIndex.buildIndexes(_physRegCount);
    _physRegTotal = uint32_t(_physRegIndex[RegGroup::kMaxVirt]) +
                    uint32_t(_physRegCount[RegGroup::kMaxVirt]) ;
  }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t physRegIndex(RegGroup group) const noexcept { return _physRegIndex[group]; }

  [[nodiscard]]
  ASMJIT_INLINE_NODEBUG uint32_t physRegTotal() const noexcept { return _physRegTotal; }

  [[nodiscard]]
  Error _asWorkReg(VirtReg* vReg, RAWorkReg** out) noexcept;

  //! Creates `RAWorkReg` data for the given `vReg`. The function does nothing
  //! if `vReg` already contains link to `RAWorkReg`. Called by `constructBlocks()`.
  [[nodiscard]]
  inline Error asWorkReg(VirtReg* vReg, RAWorkReg** out) noexcept {
    *out = vReg->workReg();
    return *out ? kErrorOk : _asWorkReg(vReg, out);
  }

  [[nodiscard]]
  ASMJIT_INLINE Error virtIndexAsWorkReg(uint32_t vIndex, RAWorkReg** out) noexcept {
    const ZoneVector<VirtReg*>& virtRegs = cc()->virtRegs();
    if (ASMJIT_UNLIKELY(vIndex >= virtRegs.size()))
      return DebugUtils::errored(kErrorInvalidVirtId);
    return asWorkReg(virtRegs[vIndex], out);
  }

  [[nodiscard]]
  inline RAStackSlot* getOrCreateStackSlot(RAWorkReg* workReg) noexcept {
    RAStackSlot* slot = workReg->stackSlot();

    if (slot) {
      return slot;
    }

    slot = _stackAllocator.newSlot(_sp.id(), workReg->virtReg()->virtSize(), workReg->virtReg()->alignment(), RAStackSlot::kFlagRegHome);
    workReg->_stackSlot = slot;
    workReg->markStackUsed();
    return slot;
  }

  [[nodiscard]]
  inline BaseMem workRegAsMem(RAWorkReg* workReg) noexcept {
    (void)getOrCreateStackSlot(workReg);
    return BaseMem(OperandSignature::fromOpType(OperandType::kMem) |
                   OperandSignature::fromMemBaseType(_sp.regType()) |
                   OperandSignature::fromBits(OperandSignature::kMemRegHomeFlag),
                   workReg->virtId(), 0, 0);
  }

  [[nodiscard]]
  WorkToPhysMap* newWorkToPhysMap() noexcept;

  [[nodiscard]]
  PhysToWorkMap* newPhysToWorkMap() noexcept;

  [[nodiscard]]
  inline PhysToWorkMap* clonePhysToWorkMap(const PhysToWorkMap* map) noexcept {
    return static_cast<PhysToWorkMap*>(zone()->dup(map, PhysToWorkMap::sizeOf(_physRegTotal)));
  }

  //! \name Liveness Analysis & Statistics
  //! \{

  //! 1. Calculates GEN/KILL/IN/OUT of each block.
  //! 2. Calculates live spans and basic statistics of each work register.
  [[nodiscard]]
  Error buildLiveness() noexcept;

  //! Assigns argIndex to WorkRegs. Must be called after the liveness analysis
  //! finishes as it checks whether the argument is live upon entry.
  [[nodiscard]]
  Error assignArgIndexToWorkRegs() noexcept;

  //! \}

  //! \name Register Allocation - Global
  //! \{

  //! Runs a global register allocator.
  [[nodiscard]]
  Error runGlobalAllocator() noexcept;

  //! Initializes data structures used for global live spans.
  [[nodiscard]]
  Error initGlobalLiveSpans() noexcept;

  [[nodiscard]]
  Error binPack(RegGroup group) noexcept;

  //! \}

  //! \name Register Allocation - Local
  //! \{

  //! Runs a local register allocator.
  [[nodiscard]]
  Error runLocalAllocator() noexcept;

  [[nodiscard]]
  Error setBlockEntryAssignment(RABlock* block, const RABlock* fromBlock, const RAAssignment& fromAssignment) noexcept;

  [[nodiscard]]
  Error setSharedAssignment(uint32_t sharedAssignmentId, const RAAssignment& fromAssignment) noexcept;

  //! Called after the RA assignment has been assigned to a block.
  //!
  //! This cannot change the assignment, but can examine it.
  [[nodiscard]]
  Error blockEntryAssigned(const PhysToWorkMap* physToWorkMap) noexcept;

  //! \}

  //! \name Register Allocation Utilities
  //! \{

  [[nodiscard]]
  Error useTemporaryMem(BaseMem& out, uint32_t size, uint32_t alignment) noexcept;

  //! \}

  //! \name Function Prolog & Epilog
  //! \{

  [[nodiscard]]
  virtual Error updateStackFrame() noexcept;

  [[nodiscard]]
  Error _markStackArgsToKeep() noexcept;

  [[nodiscard]]
  Error _updateStackArgs() noexcept;

  [[nodiscard]]
  Error insertPrologEpilog() noexcept;

  //! \}

  //! \name Instruction Rewriter
  //! \{

  [[nodiscard]]
  Error rewrite() noexcept;

  [[nodiscard]]
  virtual Error _rewrite(BaseNode* first, BaseNode* stop) noexcept;

  //! \}

#ifndef ASMJIT_NO_LOGGING
  //! \name Logging
  //! \{

  Error annotateCode() noexcept;
  Error _dumpBlockIds(String& sb, const RABlocks& blocks) noexcept;
  Error _dumpBlockLiveness(String& sb, const RABlock* block) noexcept;
  Error _dumpLiveSpans(String& sb) noexcept;

  //! \}
#endif

  //! \name Emit
  //! \{

  [[nodiscard]]
  virtual Error emitMove(uint32_t workId, uint32_t dstPhysId, uint32_t srcPhysId) noexcept;

  [[nodiscard]]
  virtual Error emitSwap(uint32_t aWorkId, uint32_t aPhysId, uint32_t bWorkId, uint32_t bPhysId) noexcept;

  [[nodiscard]]
  virtual Error emitLoad(uint32_t workId, uint32_t dstPhysId) noexcept;

  [[nodiscard]]
  virtual Error emitSave(uint32_t workId, uint32_t srcPhysId) noexcept;

  [[nodiscard]]
  virtual Error emitJump(const Label& label) noexcept;

  [[nodiscard]]
  virtual Error emitPreCall(InvokeNode* invokeNode) noexcept;

  //! \}
};

inline ZoneAllocator* RABlock::allocator() const noexcept { return _ra->allocator(); }

inline RegMask RABlock::entryScratchGpRegs() const noexcept {
  RegMask regs = _entryScratchGpRegs;
  if (hasSharedAssignmentId()) {
    regs = _ra->_sharedAssignments[_sharedAssignmentId].entryScratchGpRegs();
  }
  return regs;
}

//! \}
//! \endcond

ASMJIT_END_NAMESPACE

#endif // !ASMJIT_NO_COMPILER
#endif // ASMJIT_CORE_RAPASS_P_H_INCLUDED
