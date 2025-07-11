// This file is part of AsmJit project <https://asmjit.com>
//
// See <asmjit/core.h> or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#include "../core/api-build_p.h"
#if !defined(ASMJIT_NO_X86)

#include "../core/assembler.h"
#include "../core/codewriter_p.h"
#include "../core/cpuinfo.h"
#include "../core/emitterutils_p.h"
#include "../core/formatter.h"
#include "../core/logger.h"
#include "../core/misc_p.h"
#include "../core/support.h"
#include "../x86/x86assembler.h"
#include "../x86/x86emithelper_p.h"
#include "../x86/x86instapi_p.h"
#include "../x86/x86instdb_p.h"
#include "../x86/x86formatter_p.h"
#include "../x86/x86opcode_p.h"

ASMJIT_BEGIN_SUB_NAMESPACE(x86)

using FastUInt8 = Support::FastUInt8;

// x86::Assembler - Constants
// ==========================

//! X86 bytes used to encode important prefixes.
enum X86Byte : uint32_t {
  //! 1-byte REX prefix mask.
  kX86ByteRex = 0x40,

  //! 1-byte REX.W component.
  kX86ByteRexW = 0x08,

  kX86ByteInvalidRex = 0x80,

  //! 2-byte VEX prefix:
  //!   - `[0]` - `0xC5`.
  //!   - `[1]` - `RvvvvLpp`.
  kX86ByteVex2 = 0xC5,

  //! 3-byte VEX prefix:
  //!   - `[0]` - `0xC4`.
  //!   - `[1]` - `RXBmmmmm`.
  //!   - `[2]` - `WvvvvLpp`.
  kX86ByteVex3 = 0xC4,

  //! 3-byte XOP prefix:
  //!   - `[0]` - `0x8F`.
  //!   - `[1]` - `RXBmmmmm`.
  //!   - `[2]` - `WvvvvLpp`.
  kX86ByteXop3 = 0x8F,

  //! 4-byte EVEX prefix:
  //!   - `[0]` - `0x62`.
  //!   - `[1]` - Payload0 or `P[ 7: 0]` - `[R  X  B  R' 0  m  m  m]`.
  //!   - `[2]` - Payload1 or `P[15: 8]` - `[W  v  v  v  v  1  p  p]`.
  //!   - `[3]` - Payload2 or `P[23:16]` - `[z  L' L  b  V' a  a  a]`.
  //!
  //! Payload:
  //!   - `P[ 2: 0]` - OPCODE: EVEX.mmmmm, only lowest 3 bits [2:0] used.
  //!   - `P[    3]` - ______: Must be 0.
  //!   - `P[    4]` - REG-ID: EVEX.R' - 5th bit of 'RRRRR'.
  //!   - `P[    5]` - REG-ID: EVEX.B  - 4th bit of 'BBBBB'.
  //!   - `P[    6]` - REG-ID: EVEX.X  - 5th bit of 'BBBBB' or 4th bit of 'XXXX' (with SIB).
  //!   - `P[    7]` - REG-ID: EVEX.R  - 4th bit of 'RRRRR'.
  //!   - `P[ 9: 8]` - OPCODE: EVEX.pp.
  //!   - `P[   10]` - ______: Must be 1.
  //!   - `P[14:11]` - REG-ID: 4 bits of 'VVVV'.
  //!   - `P[   15]` - OPCODE: EVEX.W.
  //!   - `P[18:16]` - REG-ID: K register k0...k7 (Merging/Zeroing Vector Ops).
  //!   - `P[   19]` - REG-ID: 5th bit of 'VVVVV'.
  //!   - `P[   20]` - OPCODE: Broadcast/Rounding Control/SAE bit.
  //!   - `P[22.21]` - OPCODE: Vector Length (L' and  L) / Rounding Control.
  //!   - `P[   23]` - OPCODE: Zeroing/Merging.
  kX86ByteEvex = 0x62
};

// AsmJit specific (used to encode VVVVV field in XOP/VEX/EVEX).
enum VexVVVVV : uint32_t {
  kVexVVVVVShift = 7,
  kVexVVVVVMask = 0x1F << kVexVVVVVShift
};

//! Instruction 2-byte/3-byte opcode prefix definition.
struct X86OpcodeMM {
  uint8_t size;
  uint8_t data[3];
};

//! Mandatory prefixes used to encode legacy [66, F3, F2] or [9B] byte.
static const uint8_t x86OpcodePP[8] = { 0x00, 0x66, 0xF3, 0xF2, 0x00, 0x00, 0x00, 0x9B };

//! Instruction 2-byte/3-byte opcode prefix data.
static const X86OpcodeMM x86OpcodeMM[] = {
  { 0, { 0x00, 0x00, 0 } }, // #00 (0b0000).
  { 1, { 0x0F, 0x00, 0 } }, // #01 (0b0001).
  { 2, { 0x0F, 0x38, 0 } }, // #02 (0b0010).
  { 2, { 0x0F, 0x3A, 0 } }, // #03 (0b0011).
  { 2, { 0x0F, 0x01, 0 } }, // #04 (0b0100).
  { 0, { 0x00, 0x00, 0 } }, // #05 (0b0101).
  { 0, { 0x00, 0x00, 0 } }, // #06 (0b0110).
  { 0, { 0x00, 0x00, 0 } }, // #07 (0b0111).
  { 0, { 0x00, 0x00, 0 } }, // #08 (0b1000).
  { 0, { 0x00, 0x00, 0 } }, // #09 (0b1001).
  { 0, { 0x00, 0x00, 0 } }, // #0A (0b1010).
  { 0, { 0x00, 0x00, 0 } }, // #0B (0b1011).
  { 0, { 0x00, 0x00, 0 } }, // #0C (0b1100).
  { 0, { 0x00, 0x00, 0 } }, // #0D (0b1101).
  { 0, { 0x00, 0x00, 0 } }, // #0E (0b1110).
  { 0, { 0x00, 0x00, 0 } }  // #0F (0b1111).
};

static const uint8_t x86SegmentPrefix[8] = {
  0x00, // None.
  0x26, // ES.
  0x2E, // CS.
  0x36, // SS.
  0x3E, // DS.
  0x64, // FS.
  0x65  // GS.
};

static const uint32_t x86OpcodePushSReg[8] = {
  Opcode::k000000 | 0x00, // None.
  Opcode::k000000 | 0x06, // Push ES.
  Opcode::k000000 | 0x0E, // Push CS.
  Opcode::k000000 | 0x16, // Push SS.
  Opcode::k000000 | 0x1E, // Push DS.
  Opcode::k000F00 | 0xA0, // Push FS.
  Opcode::k000F00 | 0xA8  // Push GS.
};

static const uint32_t x86OpcodePopSReg[8]  = {
  Opcode::k000000 | 0x00, // None.
  Opcode::k000000 | 0x07, // Pop ES.
  Opcode::k000000 | 0x00, // Pop CS.
  Opcode::k000000 | 0x17, // Pop SS.
  Opcode::k000000 | 0x1F, // Pop DS.
  Opcode::k000F00 | 0xA1, // Pop FS.
  Opcode::k000F00 | 0xA9  // Pop GS.
};

// x86::Assembler - X86MemInfo | X86VEXPrefix | X86LLByRegType | X86CDisp8Table
// ============================================================================

//! Memory operand's info bits.
//!
//! A lookup table that contains various information based on the BASE and INDEX information of a memory operand. This
//! is much better and safer than playing with IFs in the code and can check for errors must faster and better.
enum X86MemInfo_Enum {
  kX86MemInfo_0         = 0x00,

  kX86MemInfo_BaseGp    = 0x01, //!< Has BASE reg, REX.B can be 1, compatible with REX.B byte.
  kX86MemInfo_Index     = 0x02, //!< Has INDEX reg, REX.X can be 1, compatible with REX.X byte.

  kX86MemInfo_BaseLabel = 0x10, //!< Base is Label.
  kX86MemInfo_BaseRip   = 0x20, //!< Base is RIP.

  kX86MemInfo_67H_X86   = 0x40, //!< Address-size override in 32-bit mode.
  kX86MemInfo_67H_X64   = 0x80, //!< Address-size override in 64-bit mode.
  kX86MemInfo_67H_Mask  = 0xC0  //!< Contains all address-size override bits.
};

template<uint32_t X>
struct X86MemInfo_T {
  static inline constexpr uint32_t B = (X     ) & 0x1F;
  static inline constexpr uint32_t I = (X >> 5) & 0x1F;

  static inline constexpr uint32_t kBase =
    (B >= uint32_t(RegType::kGp16)     && B <= uint32_t(RegType::kGp64)   ) ? kX86MemInfo_BaseGp    :
    (B == uint32_t(RegType::kPC)                                          ) ? kX86MemInfo_BaseRip   :
    (B == uint32_t(RegType::kLabelTag)                                    ) ? kX86MemInfo_BaseLabel : 0;

  static inline constexpr uint32_t kIndex =
    (I >= uint32_t(RegType::kGp16)     && I <= uint32_t(RegType::kGp64)   ) ? kX86MemInfo_Index     :
    (I >= uint32_t(RegType::kVec128)   && I <= uint32_t(RegType::kVec512) ) ? kX86MemInfo_Index     : 0;

  static inline constexpr uint32_t k67H =
    (B == uint32_t(RegType::kGp16)     && I == uint32_t(RegType::kNone)   ) ? kX86MemInfo_67H_X86   :
    (B == uint32_t(RegType::kGp32)     && I == uint32_t(RegType::kNone)   ) ? kX86MemInfo_67H_X64   :
    (B == uint32_t(RegType::kNone)     && I == uint32_t(RegType::kGp16)   ) ? kX86MemInfo_67H_X86   :
    (B == uint32_t(RegType::kNone)     && I == uint32_t(RegType::kGp32)   ) ? kX86MemInfo_67H_X64   :
    (B == uint32_t(RegType::kGp16)     && I == uint32_t(RegType::kGp16)   ) ? kX86MemInfo_67H_X86   :
    (B == uint32_t(RegType::kGp32)     && I == uint32_t(RegType::kGp32)   ) ? kX86MemInfo_67H_X64   :
    (B == uint32_t(RegType::kGp16)     && I == uint32_t(RegType::kVec128) ) ? kX86MemInfo_67H_X86   :
    (B == uint32_t(RegType::kGp32)     && I == uint32_t(RegType::kVec128) ) ? kX86MemInfo_67H_X64   :
    (B == uint32_t(RegType::kGp16)     && I == uint32_t(RegType::kVec256) ) ? kX86MemInfo_67H_X86   :
    (B == uint32_t(RegType::kGp32)     && I == uint32_t(RegType::kVec256) ) ? kX86MemInfo_67H_X64   :
    (B == uint32_t(RegType::kGp16)     && I == uint32_t(RegType::kVec512) ) ? kX86MemInfo_67H_X86   :
    (B == uint32_t(RegType::kGp32)     && I == uint32_t(RegType::kVec512) ) ? kX86MemInfo_67H_X64   :
    (B == uint32_t(RegType::kLabelTag) && I == uint32_t(RegType::kGp16)   ) ? kX86MemInfo_67H_X86   :
    (B == uint32_t(RegType::kLabelTag) && I == uint32_t(RegType::kGp32)   ) ? kX86MemInfo_67H_X64   : 0;

  static inline constexpr uint32_t kValue = kBase | kIndex | k67H | 0x04u | 0x08u;
};

// The result stored in the LUT is a combination of
//   - 67H - Address override prefix - depends on BASE+INDEX register types and the target architecture.
//   - REX - A possible combination of REX.[B|X|R|W] bits in REX prefix where REX.B and REX.X are possibly
//           masked out, but REX.R and REX.W are kept as is.
#define VALUE(x) X86MemInfo_T<x>::kValue
static const uint8_t x86MemInfo[] = { ASMJIT_LOOKUP_TABLE_1024(VALUE, 0) };
#undef VALUE

// VEX3 or XOP xor bits applied to the opcode before emitted. The index to this table is 'mmmmm' value, which
// contains all we need. This is only used by a 3 BYTE VEX and XOP prefixes, 2 BYTE VEX prefix is handled differently.
// The idea is to minimize the difference between VEX3 vs XOP when encoding VEX or XOP instruction. This should
// minimize the code required to emit such instructions and should also make it faster as we don't need any branch to
// decide between VEX3 vs XOP.
//            ____    ___
// [_OPCODE_|WvvvvLpp|RXBmmmmm|VEX3_XOP]
#define VALUE(x) ((x & 0x08) ? kX86ByteXop3 : kX86ByteVex3) | (0xF << 19) | (0x7 << 13)
static const uint32_t x86VEXPrefix[] = { ASMJIT_LOOKUP_TABLE_16(VALUE, 0) };
#undef VALUE

// Table that contains LL opcode field addressed by a register size / 16. It's used to propagate L.256 or L.512 when
// YMM or ZMM registers are used, respectively.
#define VALUE(x) (x & (64 >> 4)) ? Opcode::kLL_2 : \
                 (x & (32 >> 4)) ? Opcode::kLL_1 : Opcode::kLL_0
static const uint32_t x86LLBySizeDiv16[] = { ASMJIT_LOOKUP_TABLE_16(VALUE, 0) };
#undef VALUE

// Table that contains LL opcode field addressed by a register size / 16. It's used to propagate L.256 or L.512 when
// YMM or ZMM registers are used, respectively.
#define VALUE(x) x == uint32_t(RegType::kVec512) ? Opcode::kLL_2 : \
                 x == uint32_t(RegType::kVec256) ? Opcode::kLL_1 : Opcode::kLL_0
static const uint32_t x86LLByRegType[] = { ASMJIT_LOOKUP_TABLE_16(VALUE, 0) };
#undef VALUE

// Table that contains a scale (shift left) based on 'TTWLL' field and the instruction's tuple-type (TT) field. The
// scale is then applied to the BASE-N stored in each opcode to calculate the final compressed displacement used by
// all EVEX encoded instructions.
template<uint32_t X>
struct X86CDisp8SHL_T {
  static inline constexpr uint32_t TT = (X >> 3) << Opcode::kCDTT_Shift;
  static inline constexpr uint32_t LL = (X >> 0) & 0x3;
  static inline constexpr uint32_t W  = (X >> 2) & 0x1;

  static inline constexpr uint32_t kValue = (
    TT == Opcode::kCDTT_None ? ((LL==0) ? 0 : (LL==1) ? 0   : 0  ) :
    TT == Opcode::kCDTT_ByLL ? ((LL==0) ? 0 : (LL==1) ? 1   : 2  ) :
    TT == Opcode::kCDTT_T1W  ? ((LL==0) ? W : (LL==1) ? 1+W : 2+W) :
    TT == Opcode::kCDTT_DUP  ? ((LL==0) ? 0 : (LL==1) ? 2   : 3  ) : 0) << Opcode::kCDSHL_Shift;
};

#define VALUE(x) X86CDisp8SHL_T<x>::kValue
static const uint32_t x86CDisp8SHL[] = { ASMJIT_LOOKUP_TABLE_32(VALUE, 0) };
#undef VALUE

// Table that contains MOD byte of a 16-bit [BASE + disp] address.
//   0xFF == Invalid.
static const uint8_t x86Mod16BaseTable[8] = {
  0xFF, // AX -> N/A.
  0xFF, // CX -> N/A.
  0xFF, // DX -> N/A.
  0x07, // BX -> 111.
  0xFF, // SP -> N/A.
  0x06, // BP -> 110.
  0x04, // SI -> 100.
  0x05  // DI -> 101.
};

// Table that contains MOD byte of a 16-bit [BASE + INDEX + disp] combination.
//   0xFF == Invalid.
template<uint32_t X>
struct X86Mod16BaseIndexTable_T {
  static inline constexpr uint32_t B = X >> 3;
  static inline constexpr uint32_t I = X & 0x7u;

  static inline constexpr uint32_t kValue =
    ((B == Gp::kIdBx && I == Gp::kIdSi) || (B == Gp::kIdSi && I == Gp::kIdBx)) ? 0x00u :
    ((B == Gp::kIdBx && I == Gp::kIdDi) || (B == Gp::kIdDi && I == Gp::kIdBx)) ? 0x01u :
    ((B == Gp::kIdBp && I == Gp::kIdSi) || (B == Gp::kIdSi && I == Gp::kIdBp)) ? 0x02u :
    ((B == Gp::kIdBp && I == Gp::kIdDi) || (B == Gp::kIdDi && I == Gp::kIdBp)) ? 0x03u : 0xFFu;
};

#define VALUE(x) X86Mod16BaseIndexTable_T<x>::kValue
static const uint8_t x86Mod16BaseIndexTable[] = { ASMJIT_LOOKUP_TABLE_64(VALUE, 0) };
#undef VALUE

// x86::Assembler - Helpers
// ========================

static ASMJIT_INLINE bool x86IsJmpOrCall(InstId instId) noexcept {
  return instId == Inst::kIdJmp || instId == Inst::kIdCall;
}

static ASMJIT_INLINE bool x86IsImplicitMem(const Operand_& op, uint32_t base) noexcept {
  return op.isMem() && op.as<Mem>().baseId() == base && !op.as<Mem>().hasOffset();
}

//! Combine `regId` and `vvvvvId` into a single value (used by AVX and AVX-512).
static ASMJIT_INLINE uint32_t x86PackRegAndVvvvv(uint32_t regId, uint32_t vvvvvId) noexcept {
  return regId + (vvvvvId << kVexVVVVVShift);
}

static ASMJIT_INLINE uint32_t x86OpcodeLByVMem(const Operand_& op) noexcept {
  return x86LLByRegType[size_t(op.as<Mem>().indexType())];
}

static ASMJIT_INLINE uint32_t x86OpcodeLBySize(uint32_t size) noexcept {
  return x86LLBySizeDiv16[size / 16];
}

//! Encode MOD byte.
static ASMJIT_INLINE uint32_t x86EncodeMod(uint32_t m, uint32_t o, uint32_t rm) noexcept {
  ASMJIT_ASSERT(m <= 3);
  ASMJIT_ASSERT(o <= 7);
  ASMJIT_ASSERT(rm <= 7);
  return (m << 6) + (o << 3) + rm;
}

//! Encode SIB byte.
static ASMJIT_INLINE uint32_t x86EncodeSib(uint32_t s, uint32_t i, uint32_t b) noexcept {
  ASMJIT_ASSERT(s <= 3);
  ASMJIT_ASSERT(i <= 7);
  ASMJIT_ASSERT(b <= 7);
  return (s << 6) + (i << 3) + b;
}

static ASMJIT_INLINE bool x86IsRexInvalid(uint32_t rex) noexcept {
  // Validates the following possibilities:
  //   REX == 0x00      -> OKAY (X86_32 / X86_64).
  //   REX == 0x40-0x4F -> OKAY (X86_64).
  //   REX == 0x80      -> OKAY (X86_32 mode, rex prefix not used).
  //   REX == 0x81-0xCF -> BAD  (X86_32 mode, rex prefix used).
  return rex > kX86ByteInvalidRex;
}

static ASMJIT_INLINE uint32_t x86GetForceEvex3MaskInLastBit(InstOptions options) noexcept {
  constexpr uint32_t kVex3Bit = Support::ConstCTZ<uint32_t(InstOptions::kX86_Vex3)>::value;
  return uint32_t(options & InstOptions::kX86_Vex3) << (31 - kVex3Bit);
}

template<typename T>
static ASMJIT_INLINE_CONSTEXPR T x86SignExtendI32(T imm) noexcept { return T(int64_t(int32_t(imm & T(0xFFFFFFFF)))); }

static ASMJIT_INLINE uint32_t x86AltOpcodeOf(const InstDB::InstInfo* info) noexcept {
  return InstDB::_altOpcodeTable[info->_altOpcodeIndex];
}

static ASMJIT_INLINE bool x86IsMmxOrXmm(const Reg& reg) noexcept {
  return reg.regType() == RegType::kX86_Mm || reg.regType() == RegType::kVec128;
}

// x86::Assembler - X86BufferWriter
// ================================

class X86BufferWriter : public CodeWriter {
public:
  ASMJIT_INLINE explicit X86BufferWriter(Assembler* a) noexcept
    : CodeWriter(a) {}

  ASMJIT_INLINE void emitPP(uint32_t opcode) noexcept {
    uint32_t ppIndex = (opcode              >> Opcode::kPP_Shift) &
                       (Opcode::kPP_FPUMask >> Opcode::kPP_Shift) ;
    emit8If(x86OpcodePP[ppIndex], ppIndex != 0);
  }

  ASMJIT_INLINE void emitMMAndOpcode(uint32_t opcode) noexcept {
    uint32_t mmIndex = (opcode & Opcode::kMM_Mask) >> Opcode::kMM_Shift;
    const X86OpcodeMM& mmCode = x86OpcodeMM[mmIndex];

    emit8If(mmCode.data[0], mmCode.size > 0);
    emit8If(mmCode.data[1], mmCode.size > 1);
    emit8(opcode);
  }

  ASMJIT_INLINE void emitSegmentOverride(uint32_t segmentId) noexcept {
    ASMJIT_ASSERT(segmentId < ASMJIT_ARRAY_SIZE(x86SegmentPrefix));

    FastUInt8 prefix = x86SegmentPrefix[segmentId];
    emit8If(prefix, prefix != 0);
  }

  template<typename CondT>
  ASMJIT_INLINE void emitAddressOverride(CondT condition) noexcept {
    emit8If(0x67, condition);
  }

  ASMJIT_INLINE void emitImmByteOrDWord(uint64_t immValue, FastUInt8 immSize) noexcept {
    if (!immSize) {
      return;
    }

    ASMJIT_ASSERT(immSize == 1 || immSize == 4);

#if ASMJIT_ARCH_BITS >= 64
    uint64_t imm = uint64_t(immValue);
#else
    uint32_t imm = uint32_t(immValue & 0xFFFFFFFFu);
#endif

    // Many instructions just use a single byte immediate, so make it fast.
    emit8(imm & 0xFFu);
    if (immSize == 1) return;

    imm >>= 8;
    emit8(imm & 0xFFu);
    imm >>= 8;
    emit8(imm & 0xFFu);
    imm >>= 8;
    emit8(imm & 0xFFu);
  }

  ASMJIT_INLINE void emitImmediate(uint64_t immValue, FastUInt8 immSize) noexcept {
#if ASMJIT_ARCH_BITS >= 64
    uint64_t imm = immValue;
    if (immSize >= 4) {
      emit32uLE(imm & 0xFFFFFFFFu);
      imm >>= 32;
      immSize = FastUInt8(immSize - 4u);
    }
#else
    uint32_t imm = uint32_t(immValue & 0xFFFFFFFFu);
    if (immSize >= 4) {
      emit32uLE(imm);
      imm = uint32_t(immValue >> 32);
      immSize = FastUInt8(immSize - 4u);
    }
#endif

    if (!immSize) {
      return;
    }
    emit8(imm & 0xFFu);
    imm >>= 8;

    if (--immSize == 0) {
      return;
    }
    emit8(imm & 0xFFu);
    imm >>= 8;

    if (--immSize == 0) {
      return;
    }
    emit8(imm & 0xFFu);
    imm >>= 8;

    if (--immSize == 0) {
      return;
    }
    emit8(imm & 0xFFu);
  }
};

// If the operand is BPL|SPL|SIL|DIL|R8B-15B
//   - Force REX prefix
// If the operand is AH|BH|CH|DH
//   - patch its index from 0..3 to 4..7 as encoded by X86.
//   - Disallow REX prefix.
#define FIXUP_GPB(REG_OP, REG_ID)                                \
  do {                                                           \
    if (!static_cast<const Gp&>(REG_OP).isGp8Hi()) {             \
      options |= (REG_ID) >= 4 ? InstOptions::kX86_Rex           \
                               : InstOptions::kNone;             \
    }                                                            \
    else {                                                       \
      options |= InstOptions::kX86_InvalidRex;                   \
      REG_ID += 4;                                               \
    }                                                            \
  } while (0)

#define ENC_OPS1(OP0) \
  (uint32_t(OperandType::k##OP0))

#define ENC_OPS2(OP0, OP1) \
  (uint32_t(OperandType::k##OP0) + \
  (uint32_t(OperandType::k##OP1) << 3))

#define ENC_OPS3(OP0, OP1, OP2) \
  (uint32_t(OperandType::k##OP0) + \
  (uint32_t(OperandType::k##OP1) << 3) + \
  (uint32_t(OperandType::k##OP2) << 6))

#define ENC_OPS4(OP0, OP1, OP2, OP3) \
  (uint32_t(OperandType::k##OP0) + \
  (uint32_t(OperandType::k##OP1) << 3) + \
  (uint32_t(OperandType::k##OP2) << 6) + \
  (uint32_t(OperandType::k##OP3) << 9))

// x86::Assembler - Movabs Heuristics
// ==================================

static ASMJIT_INLINE uint32_t x86GetMovAbsInstSize64Bit(uint32_t regSize, InstOptions options, const Mem& rmRel) noexcept {
  uint32_t segmentPrefixSize = rmRel.segmentId() != 0;
  uint32_t _66hPrefixSize = regSize == 2;
  uint32_t rexPrefixSize = regSize == 8 || Support::test(options, InstOptions::kX86_Rex);
  uint32_t opCodeByteSize = 1;
  uint32_t immediateSize = 8;

  return segmentPrefixSize + _66hPrefixSize + rexPrefixSize + opCodeByteSize + immediateSize;
}

static ASMJIT_INLINE bool x86ShouldUseMovabs(Assembler* self, X86BufferWriter& writer, uint32_t regSize, InstOptions options, const Mem& rmRel) noexcept {
  if (self->is32Bit()) {
    // There is no relative addressing, just decide whether to use MOV encoded with MOD R/M or absolute.
    return !Support::test(options, InstOptions::kX86_ModMR | InstOptions::kX86_ModRM);
  }
  else {
    // If the addressing type is REL or MOD R/M was specified then absolute mov won't be used.
    if (rmRel.addrType() == Mem::AddrType::kRel || Support::test(options, InstOptions::kX86_ModMR | InstOptions::kX86_ModRM)) {
      return false;
    }

    int64_t addrValue = rmRel.offset();
    uint64_t baseAddress = self->code()->baseAddress();

    // If the address type is default, it means to basically check whether relative addressing is possible. However,
    // this is only possible when the base address is known - relative encoding uses RIP+N it has to be calculated.
    if (rmRel.addrType() == Mem::AddrType::kDefault && baseAddress != Globals::kNoBaseAddress && !rmRel.hasSegment()) {
      uint32_t instructionSize = x86GetMovAbsInstSize64Bit(regSize, options, rmRel);
      uint64_t virtualOffset = uint64_t(writer.offsetFrom(self->_bufferData));
      uint64_t rip64 = baseAddress + self->_section->offset() + virtualOffset + instructionSize;
      uint64_t rel64 = uint64_t(addrValue) - rip64;

      if (Support::isInt32(int64_t(rel64))) {
        return false;
      }
    }
    else {
      if (Support::isInt32(addrValue)) {
        return false;
      }
    }

    return uint64_t(addrValue) > 0xFFFFFFFFu;
  }
}

// x86::Assembler - Construction & Destruction
// ===========================================

Assembler::Assembler(CodeHolder* code) noexcept : BaseAssembler() {
  _archMask = (uint64_t(1) << uint32_t(Arch::kX86)) |
              (uint64_t(1) << uint32_t(Arch::kX64)) ;
  initEmitterFuncs(this);

  if (code) {
    code->attach(this);
  }
}
Assembler::~Assembler() noexcept {}

// x86::Assembler - Emit (Low-Level)
// =================================

ASMJIT_FAVOR_SPEED Error Assembler::_emit(InstId instId, const Operand_& o0, const Operand_& o1, const Operand_& o2, const Operand_* opExt) {
  constexpr uint32_t kVSHR_W     = Opcode::kW_Shift  - 23;
  constexpr uint32_t kVSHR_PP    = Opcode::kPP_Shift - 16;
  constexpr uint32_t kVSHR_PP_EW = Opcode::kPP_Shift - 16;

  constexpr InstOptions kRequiresSpecialHandling =
    InstOptions::kReserved     |   // Logging/Validation/Error.
    InstOptions::kX86_Rep      |   // REP/REPE prefix.
    InstOptions::kX86_Repne    |   // REPNE prefix.
    InstOptions::kX86_Lock     |   // LOCK prefix.
    InstOptions::kX86_XAcquire |   // XACQUIRE prefix.
    InstOptions::kX86_XRelease ;   // XRELEASE prefix.

  Error err;

  Opcode opcode;                   // Instruction opcode.
  InstOptions options;             // Instruction options.
  uint32_t isign3;                 // A combined signature of first 3 operands.

  const Operand_* rmRel;           // Memory operand or operand that holds Label|Imm.
  uint32_t rmInfo;                 // Memory operand's info based on x86MemInfo.
  uint32_t rbReg = 0;              // Memory base or modRM register.
  uint32_t rxReg;                  // Memory index register.
  uint32_t opReg;                  // ModR/M opcode or register id.

  LabelEntry* label;               // Label entry.
  RelocEntry* re = nullptr;        // Relocation entry.
  int32_t relOffset;               // Relative offset
  FastUInt8 relSize = 0;           // Relative size.
  uint8_t* memOpAOMark = nullptr;  // Marker that points before 'address-override prefix' is emitted.

  int64_t immValue = 0;            // Immediate value (must be 64-bit).
  FastUInt8 immSize = 0;           // Immediate size.

  X86BufferWriter writer(this);

  if (instId >= Inst::_kIdCount) {
    instId = 0;
  }

  const InstDB::InstInfo* instInfo = &InstDB::_instInfoTable[instId];
  const InstDB::CommonInfo* commonInfo = &instInfo->commonInfo();

  // Signature of the first 3 operands.
  isign3 = (uint32_t(o0.opType())     ) +
           (uint32_t(o1.opType()) << 3) +
           (uint32_t(o2.opType()) << 6);

  // Combine all instruction options and also check whether the instruction is valid. All options
  // that require special handling (including invalid instruction) are handled by the next branch.
  options = InstOptions((instId == 0) | ((size_t)(_bufferEnd - writer.cursor()) < 16)) | instOptions() | forcedInstOptions();

  // Handle failure and rare cases first.
  if (ASMJIT_UNLIKELY(Support::test(options, kRequiresSpecialHandling))) {
    if (ASMJIT_UNLIKELY(!_code)) {
      return reportError(DebugUtils::errored(kErrorNotInitialized));
    }

    // Unknown instruction.
    if (ASMJIT_UNLIKELY(instId == 0)) {
      goto InvalidInstruction;
    }

    // Grow request, happens rarely.
    err = writer.ensureSpace(this, 16);
    if (ASMJIT_UNLIKELY(err)) {
      goto Failed;
    }

#ifndef ASMJIT_NO_VALIDATION
    // Strict validation.
    if (hasDiagnosticOption(DiagnosticOptions::kValidateAssembler)) {
      Operand_ opArray[Globals::kMaxOpCount];
      EmitterUtils::opArrayFromEmitArgs(opArray, o0, o1, o2, opExt);

      err = _funcs.validate(BaseInst(instId, options, _extraReg), opArray, Globals::kMaxOpCount, ValidationFlags::kNone);
      if (ASMJIT_UNLIKELY(err)) {
        goto Failed;
      }
    }
#endif

    InstDB::InstFlags iFlags = instInfo->flags();

    // LOCK, XACQUIRE, and XRELEASE prefixes.
    if (Support::test(options, InstOptions::kX86_Lock)) {
      bool xAcqRel = Support::test(options, InstOptions::kX86_XAcquire | InstOptions::kX86_XRelease);

      if (ASMJIT_UNLIKELY(!Support::test(iFlags, InstDB::InstFlags::kLock) && !xAcqRel)) {
        goto InvalidLockPrefix;
      }

      if (xAcqRel) {
        if (ASMJIT_UNLIKELY(Support::test(options, InstOptions::kX86_XAcquire) && !Support::test(iFlags, InstDB::InstFlags::kXAcquire))) {
          goto InvalidXAcquirePrefix;
        }

        if (ASMJIT_UNLIKELY(Support::test(options, InstOptions::kX86_XRelease) && !Support::test(iFlags, InstDB::InstFlags::kXRelease))) {
          goto InvalidXReleasePrefix;
        }

        writer.emit8(Support::test(options, InstOptions::kX86_XAcquire) ? 0xF2 : 0xF3);
      }

      writer.emit8(0xF0);
    }

    // REP and REPNE prefixes.
    if (Support::test(options, InstOptions::kX86_Rep | InstOptions::kX86_Repne)) {
      if (ASMJIT_UNLIKELY(!Support::test(iFlags, InstDB::InstFlags::kRep))) {
        goto InvalidRepPrefix;
      }

      if (ASMJIT_UNLIKELY(_extraReg.isReg() && (_extraReg.group() != RegGroup::kGp || _extraReg.id() != Gp::kIdCx))) {
        goto InvalidRepPrefix;
      }

      writer.emit8(Support::test(options, InstOptions::kX86_Repne) ? 0xF2 : 0xF3);
    }
  }

  // This sequence seems to be the fastest.
  opcode = InstDB::_mainOpcodeTable[instInfo->_mainOpcodeIndex];
  opReg = opcode.extractModO();
  opcode |= instInfo->_mainOpcodeValue;

  // Encoding Scope
  // --------------

  // How it works? Each case here represents a unique encoding of a group of instructions, which is handled
  // separately. The handlers check instruction signature, possibly register types, etc, and process this
  // information by writing some bits to opcode, opReg/rbReg, immValue/immSize, etc, and then at the end of
  // the sequence it uses goto to jump into a lower level handler, that actually encodes the instruction.

  switch (instInfo->_encoding) {
    case InstDB::kEncodingNone:
      goto EmitDone;

    // Base Instructions
    // -----------------

    case InstDB::kEncodingX86Op:
      goto EmitX86Op;

    case InstDB::kEncodingX86Op_Mod11RM:
      rbReg = opcode.extractModRM();
      goto EmitX86R;

    case InstDB::kEncodingX86Op_Mod11RM_I8:
      // The first operand must be immediate, we don't care of other operands as they could be implicit.
      if (!o0.isImm())
        goto InvalidInstruction;

      rbReg = opcode.extractModRM();
      immValue = o0.as<Imm>().valueAs<uint8_t>();
      immSize = 1;
      goto EmitX86R;

    case InstDB::kEncodingX86Op_xAddr:
      if (ASMJIT_UNLIKELY(!o0.isReg()))
        goto InvalidInstruction;

      rmInfo = x86MemInfo[size_t(o0.as<Reg>().regType())];
      writer.emitAddressOverride((rmInfo & _addressOverrideMask()) != 0);
      goto EmitX86Op;

    case InstDB::kEncodingX86Op_xAX:
      if (isign3 == 0)
        goto EmitX86Op;

      if (isign3 == ENC_OPS1(Reg) && o0.id() == Gp::kIdAx)
        goto EmitX86Op;
      break;

    case InstDB::kEncodingX86Op_xDX_xAX:
      if (isign3 == 0)
        goto EmitX86Op;

      if (isign3 == ENC_OPS2(Reg, Reg) && o0.id() == Gp::kIdDx && o1.id() == Gp::kIdAx)
        goto EmitX86Op;
      break;

    case InstDB::kEncodingX86Op_MemZAX:
      if (isign3 == 0)
        goto EmitX86Op;

      rmRel = &o0;
      if (isign3 == ENC_OPS1(Mem) && x86IsImplicitMem(o0, Gp::kIdAx))
        goto EmitX86OpImplicitMem;

      break;

    case InstDB::kEncodingX86I_xAX:
      // Implicit form.
      if (isign3 == ENC_OPS1(Imm)) {
        immValue = o0.as<Imm>().valueAs<uint8_t>();
        immSize = 1;
        goto EmitX86Op;
      }

      // Explicit form.
      if (isign3 == ENC_OPS2(Reg, Imm) && o0.id() == Gp::kIdAx) {
        immValue = o1.as<Imm>().valueAs<uint8_t>();
        immSize = 1;
        goto EmitX86Op;
      }
      break;

    case InstDB::kEncodingX86M_NoMemSize:
      if (o0.isReg())
        opcode.addPrefixBySize(o0.x86RmSize());
      goto CaseX86M_NoSize;

    case InstDB::kEncodingX86M:
      opcode.addPrefixBySize(o0.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingX86M_NoSize:
CaseX86M_NoSize:
      rbReg = o0.id();
      if (isign3 == ENC_OPS1(Reg))
        goto EmitX86R;

      rmRel = &o0;
      if (isign3 == ENC_OPS1(Mem))
        goto EmitX86M;
      break;

    case InstDB::kEncodingX86M_GPB_MulDiv:
CaseX86M_GPB_MulDiv:
      // Explicit form?
      if (isign3 > 0x7) {
        // [AX] <- [AX] div|mul r8.
        if (isign3 == ENC_OPS2(Reg, Reg)) {
          if (ASMJIT_UNLIKELY(!o0.isGp16(Gp::kIdAx) || !o1.isGp8()))
            goto InvalidInstruction;

          rbReg = o1.id();
          FIXUP_GPB(o1, rbReg);
          goto EmitX86R;
        }

        // [AX] <- [AX] div|mul m8.
        if (isign3 == ENC_OPS2(Reg, Mem)) {
          if (ASMJIT_UNLIKELY(!o0.isGp16(Gp::kIdAx)))
            goto InvalidInstruction;

          rmRel = &o1;
          goto EmitX86M;
        }

        // [?DX:?AX] <- [?DX:?AX] div|mul r16|r32|r64
        if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
          if (ASMJIT_UNLIKELY(o0.x86RmSize() != o1.x86RmSize()))
            goto InvalidInstruction;

          opcode.addArithBySize(o0.x86RmSize());
          rbReg = o2.id();
          goto EmitX86R;
        }

        // [?DX:?AX] <- [?DX:?AX] div|mul m16|m32|m64
        if (isign3 == ENC_OPS3(Reg, Reg, Mem)) {
          if (ASMJIT_UNLIKELY(o0.x86RmSize() != o1.x86RmSize()))
            goto InvalidInstruction;

          opcode.addArithBySize(o0.x86RmSize());
          rmRel = &o2;
          goto EmitX86M;
        }

        goto InvalidInstruction;
      }

      [[fallthrough]];

    case InstDB::kEncodingX86M_GPB:
      if (isign3 == ENC_OPS1(Reg)) {
        opcode.addArithBySize(o0.x86RmSize());
        rbReg = o0.id();

        if (o0.x86RmSize() != 1)
          goto EmitX86R;

        FIXUP_GPB(o0, rbReg);
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS1(Mem)) {
        if (ASMJIT_UNLIKELY(o0.x86RmSize() == 0))
          goto AmbiguousOperandSize;

        opcode.addArithBySize(o0.x86RmSize());
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86M_Only_EDX_EAX:
      if (isign3 == ENC_OPS3(Mem, Reg, Reg) && o1.isGp32(Gp::kIdDx) && o2.isGp32(Gp::kIdAx)) {
        rmRel = &o0;
        goto EmitX86M;
      }
      [[fallthrough]];

    case InstDB::kEncodingX86M_Only:
      if (isign3 == ENC_OPS1(Mem)) {
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86M_Nop:
      if (isign3 == ENC_OPS1(None))
        goto EmitX86Op;

      // Single operand NOP instruction "0F 1F /0".
      opcode = Opcode::k000F00 | 0x1F;
      opReg = 0;

      if (isign3 == ENC_OPS1(Reg)) {
        opcode.addPrefixBySize(o0.x86RmSize());
        rbReg = o0.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS1(Mem)) {
        opcode.addPrefixBySize(o0.x86RmSize());
        rmRel = &o0;
        goto EmitX86M;
      }

      // Two operand NOP instruction "0F 1F /r".
      opReg = o1.id();
      opcode.addPrefixBySize(o1.x86RmSize());

      if (isign3 == ENC_OPS2(Reg, Reg)) {
        rbReg = o0.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86R_FromM:
      if (isign3 == ENC_OPS1(Mem)) {
        rmRel = &o0;
        rbReg = o0.id();
        goto EmitX86RFromM;
      }
      break;

    case InstDB::kEncodingX86R32_EDX_EAX:
      // Explicit form: R32, EDX, EAX.
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        if (!o1.isGp32(Gp::kIdDx) || !o2.isGp32(Gp::kIdAx))
          goto InvalidInstruction;
        rbReg = o0.id();
        goto EmitX86R;
      }

      // Implicit form: R32.
      if (isign3 == ENC_OPS1(Reg)) {
        if (!o0.isGp32())
          goto InvalidInstruction;
        rbReg = o0.id();
        goto EmitX86R;
      }
      break;

    case InstDB::kEncodingX86R_Native:
      if (isign3 == ENC_OPS1(Reg)) {
        rbReg = o0.id();
        goto EmitX86R;
      }
      break;

    case InstDB::kEncodingX86Rm:
      opcode.addPrefixBySize(o0.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingX86Rm_NoSize:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Rm_Raw66H:
      // We normally emit either [66|F2|F3], this instruction requires 66+[F2|F3].
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();

        if (o0.x86RmSize() == 2)
          writer.emit8(0x66);
        else
          opcode.addWBySize(o0.x86RmSize());
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;

        if (o0.x86RmSize() == 2)
          writer.emit8(0x66);
        else
          opcode.addWBySize(o0.x86RmSize());
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Mr:
      opcode.addPrefixBySize(o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingX86Mr_NoSize:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        rbReg = o0.id();
        opReg = o1.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        rmRel = &o0;
        opReg = o1.id();
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Arith:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opcode.addArithBySize(o0.x86RmSize());

        if (o0.x86RmSize() != o1.x86RmSize())
          goto OperandSizeMismatch;

        rbReg = o0.id();
        opReg = o1.id();

        if (o0.x86RmSize() == 1) {
          FIXUP_GPB(o0, rbReg);
          FIXUP_GPB(o1, opReg);
        }

        // MOD/MR: The default encoding used if not instructed otherwise..
        if (!Support::test(options, InstOptions::kX86_ModRM))
          goto EmitX86R;

        // MOD/RM: Alternative encoding selected via instruction options.
        opcode += 2u;
        std::swap(opReg, rbReg);
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opcode += 2u;
        opcode.addArithBySize(o0.x86RmSize());

        opReg = o0.id();
        rmRel = &o1;

        if (o0.x86RmSize() != 1)
          goto EmitX86M;

        FIXUP_GPB(o0, opReg);
        goto EmitX86M;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opcode.addArithBySize(o1.x86RmSize());
        opReg = o1.id();
        rmRel = &o0;

        if (o1.x86RmSize() != 1)
          goto EmitX86M;

        FIXUP_GPB(o1, opReg);
        goto EmitX86M;
      }

      // The remaining instructions use 0x80 opcode.
      opcode = 0x80;

      if (isign3 == ENC_OPS2(Reg, Imm)) {
        uint32_t size = o0.x86RmSize();

        rbReg = o0.id();
        immValue = o1.as<Imm>().value();

        if (size == 1) {
          FIXUP_GPB(o0, rbReg);
          immSize = 1;
        }
        else {
          if (size == 2) {
            opcode |= Opcode::kPP_66;
          }
          else if (size == 4) {
            // Sign extend so isInt8 returns the right result.
            immValue = x86SignExtendI32<int64_t>(immValue);
          }
          else if (size == 8) {
            bool canTransformTo32Bit = instId == Inst::kIdAnd && Support::isUInt32(immValue);

            if (!Support::isInt32(immValue)) {
              // We would do this by default when `kOptionOptimizedForSize` is
              // enabled, however, in this case we just force this as otherwise
              // we would have to fail.
              if (canTransformTo32Bit)
                size = 4;
              else
                goto InvalidImmediate;
            }
            else if (canTransformTo32Bit && hasEncodingOption(EncodingOptions::kOptimizeForSize)) {
              size = 4;
            }

            opcode.addWBySize(size);
          }

          immSize = FastUInt8(Support::min<uint32_t>(size, 4));
          if (Support::isInt8(immValue) && !Support::test(options, InstOptions::kLongForm))
            immSize = 1;
        }

        // Short form - AL, AX, EAX, RAX.
        if (rbReg == 0 && (size == 1 || immSize != 1) && !Support::test(options, InstOptions::kLongForm)) {
          opcode &= Opcode::kPP_66 | Opcode::kW;
          opcode |= ((opReg << 3) | (0x04 + (size != 1)));
          immSize = FastUInt8(Support::min<uint32_t>(size, 4));
          goto EmitX86Op;
        }

        opcode += size != 1 ? (immSize != 1 ? 1u : 3u) : 0u;
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Mem, Imm)) {
        uint32_t memSize = o0.x86RmSize();

        if (ASMJIT_UNLIKELY(memSize == 0))
          goto AmbiguousOperandSize;

        immValue = o1.as<Imm>().value();
        immSize = FastUInt8(Support::min<uint32_t>(memSize, 4));

        // Sign extend so isInt8 returns the right result.
        if (memSize == 4)
          immValue = x86SignExtendI32<int64_t>(immValue);

        if (Support::isInt8(immValue) && !Support::test(options, InstOptions::kLongForm))
          immSize = 1;

        opcode += memSize != 1 ? (immSize != 1 ? 1u : 3u) : 0u;
        opcode.addPrefixBySize(memSize);

        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Bswap:
      if (isign3 == ENC_OPS1(Reg)) {
        if (ASMJIT_UNLIKELY(o0.x86RmSize() == 1))
          goto InvalidInstruction;

        opReg = o0.id();
        opcode.addPrefixBySize(o0.x86RmSize());
        goto EmitX86OpReg;
      }
      break;

    case InstDB::kEncodingX86Bt:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opcode.addPrefixBySize(o1.x86RmSize());
        opReg = o1.id();
        rbReg = o0.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opcode.addPrefixBySize(o1.x86RmSize());
        opReg = o1.id();
        rmRel = &o0;
        goto EmitX86M;
      }

      // The remaining instructions use the secondary opcode/r.
      immValue = o1.as<Imm>().value();
      immSize = 1;

      opcode = x86AltOpcodeOf(instInfo);
      opcode.addPrefixBySize(o0.x86RmSize());
      opReg = opcode.extractModO();

      if (isign3 == ENC_OPS2(Reg, Imm)) {
        rbReg = o0.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Mem, Imm)) {
        if (ASMJIT_UNLIKELY(o0.x86RmSize() == 0))
          goto AmbiguousOperandSize;

        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Call:
      if (isign3 == ENC_OPS1(Reg)) {
        rbReg = o0.id();
        goto EmitX86R;
      }

      rmRel = &o0;
      if (isign3 == ENC_OPS1(Mem))
        goto EmitX86M;

      // Call with 32-bit displacement use 0xE8 opcode. Call with 8-bit displacement is not encodable so the
      // alternative opcode field in X86DB must be zero.
      opcode = 0xE8;
      opReg = 0;
      goto EmitJmpCall;

    case InstDB::kEncodingX86Cmpxchg: {
      // Convert explicit to implicit.
      if (isign3 & (0x7 << 6)) {
        if (!o2.isGp() || o2.id() != Gp::kIdAx) {
          goto InvalidInstruction;
        }
        isign3 &= 0x3F;
      }

      if (isign3 == ENC_OPS2(Reg, Reg)) {
        if (o0.x86RmSize() != o1.x86RmSize())
          goto OperandSizeMismatch;

        opcode.addArithBySize(o0.x86RmSize());
        rbReg = o0.id();
        opReg = o1.id();

        if (o0.x86RmSize() != 1)
          goto EmitX86R;

        FIXUP_GPB(o0, rbReg);
        FIXUP_GPB(o1, opReg);
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opcode.addArithBySize(o1.x86RmSize());
        opReg = o1.id();
        rmRel = &o0;

        if (o1.x86RmSize() != 1)
          goto EmitX86M;

        FIXUP_GPB(o1, opReg);
        goto EmitX86M;
      }
      break;
    }

    case InstDB::kEncodingX86Cmpxchg8b_16b: {
      const Operand_& o3 = opExt[EmitterUtils::kOp3];
      const Operand_& o4 = opExt[EmitterUtils::kOp4];

      if (isign3 == ENC_OPS3(Mem, Reg, Reg)) {
        if (o3.isReg() && o4.isReg()) {
          rmRel = &o0;
          goto EmitX86M;
        }
      }

      if (isign3 == ENC_OPS1(Mem)) {
        rmRel = &o0;
        goto EmitX86M;
      }
      break;
    }

    case InstDB::kEncodingX86Crc:
      opReg = o0.id();
      opcode.addWBySize(o0.x86RmSize());

      if (isign3 == ENC_OPS2(Reg, Reg)) {
        rbReg = o1.id();

        if (o1.x86RmSize() == 1) {
          FIXUP_GPB(o1, rbReg);
          goto EmitX86R;
        }
        else {
          // This seems to be the only exception of encoding '66F2' prefix.
          if (o1.x86RmSize() == 2) writer.emit8(0x66);

          opcode.add(1);
          goto EmitX86R;
        }
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        rmRel = &o1;
        if (o1.x86RmSize() == 0)
          goto AmbiguousOperandSize;

        // This seems to be the only exception of encoding '66F2' prefix.
        if (o1.x86RmSize() == 2) writer.emit8(0x66);

        opcode += uint32_t(o1.x86RmSize() != 1u);
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Enter:
      if (isign3 == ENC_OPS2(Imm, Imm)) {
        uint32_t iw = o0.as<Imm>().valueAs<uint16_t>();
        uint32_t ib = o1.as<Imm>().valueAs<uint8_t>();

        immValue = iw | (ib << 16);
        immSize = 3;
        goto EmitX86Op;
      }
      break;

    case InstDB::kEncodingX86Imul:
      // First process all forms distinct of `kEncodingX86M_OptB_MulDiv`.
      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opcode = 0x6B;
        opcode.addPrefixBySize(o0.x86RmSize());

        immValue = o2.as<Imm>().value();
        immSize = 1;

        if (!Support::isInt8(immValue) || Support::test(options, InstOptions::kLongForm)) {
          opcode -= 2;
          immSize = o0.x86RmSize() == 2 ? 2 : 4;
        }

        opReg = o0.id();
        rbReg = o1.id();

        goto EmitX86R;
      }

      if (isign3 == ENC_OPS3(Reg, Mem, Imm)) {
        opcode = 0x6B;
        opcode.addPrefixBySize(o0.x86RmSize());

        immValue = o2.as<Imm>().value();
        immSize = 1;

        // Sign extend so isInt8 returns the right result.
        if (o0.x86RmSize() == 4)
          immValue = x86SignExtendI32<int64_t>(immValue);

        if (!Support::isInt8(immValue) || Support::test(options, InstOptions::kLongForm)) {
          opcode -= 2;
          immSize = o0.x86RmSize() == 2 ? 2 : 4;
        }

        opReg = o0.id();
        rmRel = &o1;

        goto EmitX86M;
      }

      if (isign3 == ENC_OPS2(Reg, Reg)) {
        // Must be explicit 'ax, r8' form.
        if (o1.x86RmSize() == 1)
          goto CaseX86M_GPB_MulDiv;

        if (o0.x86RmSize() != o1.x86RmSize())
          goto OperandSizeMismatch;

        opReg = o0.id();
        rbReg = o1.id();

        opcode = Opcode::k000F00 | 0xAF;
        opcode.addPrefixBySize(o0.x86RmSize());
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        // Must be explicit 'ax, m8' form.
        if (o1.x86RmSize() == 1)
          goto CaseX86M_GPB_MulDiv;

        opReg = o0.id();
        rmRel = &o1;

        opcode = Opcode::k000F00 | 0xAF;
        opcode.addPrefixBySize(o0.x86RmSize());
        goto EmitX86M;
      }

      // Shorthand to imul 'reg, reg, imm'.
      if (isign3 == ENC_OPS2(Reg, Imm)) {
        opcode = 0x6B;
        opcode.addPrefixBySize(o0.x86RmSize());

        immValue = o1.as<Imm>().value();
        immSize = 1;

        // Sign extend so isInt8 returns the right result.
        if (o0.x86RmSize() == 4)
          immValue = x86SignExtendI32<int64_t>(immValue);

        if (!Support::isInt8(immValue) || Support::test(options, InstOptions::kLongForm)) {
          opcode -= 2;
          immSize = o0.x86RmSize() == 2 ? 2 : 4;
        }

        opReg = rbReg = o0.id();
        goto EmitX86R;
      }

      // Try implicit form.
      goto CaseX86M_GPB_MulDiv;

    case InstDB::kEncodingX86In:
      if (isign3 == ENC_OPS2(Reg, Imm)) {
        if (ASMJIT_UNLIKELY(o0.id() != Gp::kIdAx))
          goto InvalidInstruction;

        immValue = o1.as<Imm>().valueAs<uint8_t>();
        immSize = 1;

        opcode = x86AltOpcodeOf(instInfo) + (o0.x86RmSize() != 1);
        opcode.add66hBySize(o0.x86RmSize());
        goto EmitX86Op;
      }

      if (isign3 == ENC_OPS2(Reg, Reg)) {
        if (ASMJIT_UNLIKELY(o0.id() != Gp::kIdAx || o1.id() != Gp::kIdDx))
          goto InvalidInstruction;

        opcode += uint32_t(o0.x86RmSize() != 1u);
        opcode.add66hBySize(o0.x86RmSize());
        goto EmitX86Op;
      }
      break;

    case InstDB::kEncodingX86Ins:
      if (isign3 == ENC_OPS2(Mem, Reg)) {
        if (ASMJIT_UNLIKELY(!x86IsImplicitMem(o0, Gp::kIdDi) || o1.id() != Gp::kIdDx))
          goto InvalidInstruction;

        uint32_t size = o0.x86RmSize();
        if (ASMJIT_UNLIKELY(size == 0))
          goto AmbiguousOperandSize;

        rmRel = &o0;
        opcode += uint32_t(size != 1u);

        opcode.add66hBySize(size);
        goto EmitX86OpImplicitMem;
      }
      break;

    case InstDB::kEncodingX86IncDec:
      if (isign3 == ENC_OPS1(Reg)) {
        rbReg = o0.id();

        if (o0.x86RmSize() == 1) {
          FIXUP_GPB(o0, rbReg);
          goto EmitX86R;
        }

        if (is32Bit()) {
          // INC r16|r32 is only encodable in 32-bit mode (collides with REX).
          opcode = x86AltOpcodeOf(instInfo) + (rbReg & 0x07);
          opcode.add66hBySize(o0.x86RmSize());
          goto EmitX86Op;
        }
        else {
          opcode.addArithBySize(o0.x86RmSize());
          goto EmitX86R;
        }
      }

      if (isign3 == ENC_OPS1(Mem)) {
        if (!o0.x86RmSize())
          goto AmbiguousOperandSize;
        opcode.addArithBySize(o0.x86RmSize());
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Int:
      if (isign3 == ENC_OPS1(Imm)) {
        immValue = o0.as<Imm>().value();
        immSize = 1;
        goto EmitX86Op;
      }
      break;

    case InstDB::kEncodingX86Jcc:
      if (Support::test(options, InstOptions::kTaken | InstOptions::kNotTaken) && hasEncodingOption(EncodingOptions::kPredictedJumps)) {
        uint8_t prefix = Support::test(options, InstOptions::kTaken) ? uint8_t(0x3E) : uint8_t(0x2E);
        writer.emit8(prefix);
      }

      rmRel = &o0;
      opReg = 0;
      goto EmitJmpCall;

    case InstDB::kEncodingX86JecxzLoop:
      rmRel = &o0;
      // Explicit jecxz|loop [r|e]cx, dst
      if (o0.isReg()) {
        if (ASMJIT_UNLIKELY(!o0.isGp(Gp::kIdCx))) {
          goto InvalidInstruction;
        }

        writer.emitAddressOverride((is32Bit() && o0.x86RmSize() == 2) || (is64Bit() && o0.x86RmSize() == 4));
        rmRel = &o1;
      }

      opReg = 0;
      goto EmitJmpCall;

    case InstDB::kEncodingX86Jmp:
      if (isign3 == ENC_OPS1(Reg)) {
        rbReg = o0.id();
        goto EmitX86R;
      }

      rmRel = &o0;
      if (isign3 == ENC_OPS1(Mem))
        goto EmitX86M;

      // Jump encoded with 32-bit displacement use 0xE9 opcode. Jump encoded with 8-bit displacement's opcode is
      // stored as an alternative opcode.
      opcode = 0xE9;
      opReg = 0;
      goto EmitJmpCall;

    case InstDB::kEncodingX86JmpRel:
      rmRel = &o0;
      goto EmitJmpCall;

    case InstDB::kEncodingX86LcallLjmp:
      if (isign3 == ENC_OPS1(Mem)) {
        rmRel = &o0;
        uint32_t mSize = rmRel->as<Mem>().size();
        if (mSize == 0) {
          mSize = registerSize();
        }
        else {
          mSize -= 2;
          if (mSize != 2 && mSize != 4 && mSize != registerSize())
            goto InvalidAddress;
        }
        opcode.addPrefixBySize(mSize);
        goto EmitX86M;
      }

      if (isign3 == ENC_OPS2(Imm, Imm)) {
        if (!is32Bit())
          goto InvalidInstruction;

        const Imm& imm0 = o0.as<Imm>();
        const Imm& imm1 = o1.as<Imm>();

        if (imm0.value() > 0xFFFFu || imm1.value() > 0xFFFFFFFFu)
          goto InvalidImmediate;

        opcode = x86AltOpcodeOf(instInfo);
        immValue = imm1.value() | (imm0.value() << 32);
        immSize = 6;
        goto EmitX86Op;
      }
      break;

    case InstDB::kEncodingX86Lea:
      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opcode.addPrefixBySize(o0.x86RmSize());
        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Mov:
      // Reg <- Reg
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        // Asmjit uses segment registers indexed from 1 to 6, leaving zero as "no segment register used". We have to
        // fix this (decrement the index of the register) when emitting MOV instructions which move to/from a segment
        // register. The segment register is always `opReg`, because the MOV instruction uses either RM or MR encoding.

        // GP <- ??
        if (o0.as<Reg>().isGp()) {
          rbReg = o0.id();
          opReg = o1.id();

          // GP <- GP
          if (o1.as<Reg>().isGp()) {
            uint32_t opSize = o0.x86RmSize();
            if (opSize != o1.x86RmSize())
              goto InvalidInstruction;

            if (opSize == 1) {
              FIXUP_GPB(o0, rbReg);
              FIXUP_GPB(o1, opReg);
              opcode = 0x88;

              if (!Support::test(options, InstOptions::kX86_ModRM))
                goto EmitX86R;

              opcode += 2u;
              std::swap(opReg, rbReg);
              goto EmitX86R;
            }
            else {
              opcode = 0x89;
              opcode.addPrefixBySize(opSize);

              if (!Support::test(options, InstOptions::kX86_ModRM))
                goto EmitX86R;

              opcode += 2u;
              std::swap(opReg, rbReg);
              goto EmitX86R;
            }
          }

          // GP <- SReg
          if (o1.isSegmentReg()) {
            opcode = 0x8C;
            opcode.addPrefixBySize(o0.x86RmSize());
            opReg--;
            goto EmitX86R;
          }

          // GP <- CReg
          if (o1.isControlReg()) {
            opcode = Opcode::k000F00 | 0x20;

            // Use `LOCK MOV` in 32-bit mode if CR8+ register is accessed (AMD extension).
            if ((opReg & 0x8) && is32Bit()) {
              writer.emit8(0xF0);
              opReg &= 0x7;
            }
            goto EmitX86R;
          }

          // GP <- DReg
          if (o1.isDebugReg()) {
            opcode = Opcode::k000F00 | 0x21;
            goto EmitX86R;
          }
        }
        else {
          opReg = o0.id();
          rbReg = o1.id();

          // ?? <- GP
          if (!o1.as<Reg>().isGp())
            goto InvalidInstruction;

          // SReg <- GP
          if (o0.isSegmentReg()) {
            opcode = 0x8E;
            opcode.addPrefixBySize(o1.x86RmSize());
            opReg--;
            goto EmitX86R;
          }

          // CReg <- GP
          if (o0.isControlReg()) {
            opcode = Opcode::k000F00 | 0x22;

            // Use `LOCK MOV` in 32-bit mode if CR8+ register is accessed (AMD extension).
            if ((opReg & 0x8) && is32Bit()) {
              writer.emit8(0xF0);
              opReg &= 0x7;
            }
            goto EmitX86R;
          }

          // DReg <- GP
          if (o0.isDebugReg()) {
            opcode = Opcode::k000F00 | 0x23;
            goto EmitX86R;
          }
        }

        goto InvalidInstruction;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;

        // SReg <- Mem
        if (o0.isSegmentReg()) {
          opcode = 0x8E;
          opcode.addPrefixBySize(o1.x86RmSize());
          opReg--;
          goto EmitX86M;
        }
        // Reg <- Mem
        else {
          opcode = 0;
          opcode.addArithBySize(o0.x86RmSize());

          // Handle a special form of `mov al|ax|eax|rax, [ptr64]` that doesn't use MOD.
          if (opReg == Gp::kIdAx && !rmRel->as<Mem>().hasBaseOrIndex()) {
            if (x86ShouldUseMovabs(this, writer, o0.x86RmSize(), options, rmRel->as<Mem>())) {
              opcode += 0xA0u;
              immValue = rmRel->as<Mem>().offset();
              goto EmitX86OpMovAbs;
            }
          }

          if (o0.x86RmSize() == 1)
            FIXUP_GPB(o0, opReg);

          opcode += 0x8Au;
          goto EmitX86M;
        }
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opReg = o1.id();
        rmRel = &o0;

        // Mem <- SReg
        if (o1.isSegmentReg()) {
          opcode = 0x8C;
          opcode.addPrefixBySize(o0.x86RmSize());
          opReg--;
          goto EmitX86M;
        }
        // Mem <- Reg
        else {
          opcode = 0;
          opcode.addArithBySize(o1.x86RmSize());

          // Handle a special form of `mov [ptr64], al|ax|eax|rax` that doesn't use MOD.
          if (opReg == Gp::kIdAx && !rmRel->as<Mem>().hasBaseOrIndex()) {
            if (x86ShouldUseMovabs(this, writer, o1.x86RmSize(), options, rmRel->as<Mem>())) {
              opcode += 0xA2u;
              immValue = rmRel->as<Mem>().offset();
              goto EmitX86OpMovAbs;
            }
          }

          if (o1.x86RmSize() == 1)
            FIXUP_GPB(o1, opReg);

          opcode += 0x88u;
          goto EmitX86M;
        }
      }

      if (isign3 == ENC_OPS2(Reg, Imm)) {
        opReg = o0.id();
        immSize = FastUInt8(o0.x86RmSize());

        if (immSize == 1) {
          FIXUP_GPB(o0, opReg);

          opcode = 0xB0;
          immValue = o1.as<Imm>().valueAs<uint8_t>();
          goto EmitX86OpReg;
        }
        else {
          // 64-bit immediate in 64-bit mode is allowed.
          immValue = o1.as<Imm>().value();

          // Optimize the instruction size by using a 32-bit immediate if possible.
          if (immSize == 8 && !Support::test(options, InstOptions::kLongForm)) {
            if (Support::isUInt32(immValue) && hasEncodingOption(EncodingOptions::kOptimizeForSize)) {
              // Zero-extend by using a 32-bit GPD destination instead of a 64-bit GPQ.
              immSize = 4;
            }
            else if (Support::isInt32(immValue)) {
              // Sign-extend, uses 'C7 /0' opcode.
              rbReg = opReg;

              opcode = Opcode::kW | 0xC7;
              opReg = 0;

              immSize = 4;
              goto EmitX86R;
            }
          }

          opcode = 0xB8;
          opcode.addPrefixBySize(immSize);
          goto EmitX86OpReg;
        }
      }

      if (isign3 == ENC_OPS2(Mem, Imm)) {
        uint32_t memSize = o0.x86RmSize();
        if (ASMJIT_UNLIKELY(memSize == 0))
          goto AmbiguousOperandSize;

        opcode = 0xC6 + (memSize != 1);
        opcode.addPrefixBySize(memSize);
        opReg = 0;
        rmRel = &o0;

        immValue = o1.as<Imm>().value();
        immSize = FastUInt8(Support::min<uint32_t>(memSize, 4));
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Movabs:
      // Reg <- Mem
      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;

        opcode = 0xA0;
        opcode.addArithBySize(o0.x86RmSize());

        if (ASMJIT_UNLIKELY(!o0.as<Reg>().isGp()) || opReg != Gp::kIdAx)
          goto InvalidInstruction;

        if (ASMJIT_UNLIKELY(rmRel->as<Mem>().hasBaseOrIndex()))
          goto InvalidAddress;

        if (ASMJIT_UNLIKELY(rmRel->as<Mem>().addrType() == Mem::AddrType::kRel))
          goto InvalidAddress;

        immValue = rmRel->as<Mem>().offset();
        goto EmitX86OpMovAbs;
      }

      // Mem <- Reg
      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opReg = o1.id();
        rmRel = &o0;

        opcode = 0xA2;
        opcode.addArithBySize(o1.x86RmSize());

        if (ASMJIT_UNLIKELY(!o1.as<Reg>().isGp()) || opReg != Gp::kIdAx)
          goto InvalidInstruction;

        if (ASMJIT_UNLIKELY(rmRel->as<Mem>().hasBaseOrIndex()))
          goto InvalidAddress;

        immValue = rmRel->as<Mem>().offset();
        goto EmitX86OpMovAbs;
      }

      // Reg <- Imm.
      if (isign3 == ENC_OPS2(Reg, Imm)) {
        if (ASMJIT_UNLIKELY(!o0.isGp64()))
          goto InvalidInstruction;

        opReg = o0.id();
        opcode = 0xB8;

        immSize = 8;
        immValue = o1.as<Imm>().value();

        opcode.addPrefixBySize(8);
        goto EmitX86OpReg;
      }
      break;

    case InstDB::kEncodingX86MovsxMovzx:
      opcode.add(o1.x86RmSize() != 1);
      opcode.addPrefixBySize(o0.x86RmSize());

      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();

        if (o1.x86RmSize() != 1)
          goto EmitX86R;

        FIXUP_GPB(o1, rbReg);
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86MovntiMovdiri:
      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opcode.addWIf(o1.isGp64());

        opReg = o1.id();
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86EnqcmdMovdir64b:
      if (isign3 == ENC_OPS2(Mem, Mem)) {
        const Mem& m0 = o0.as<Mem>();
        // This is the only required validation, the rest is handled afterwards.
        if (ASMJIT_UNLIKELY(m0.baseType() != o1.as<Mem>().baseType() ||
                            m0.hasIndex() ||
                            m0.hasOffset() ||
                            (m0.hasSegment() && m0.segmentId() != SReg::kIdEs)))
          goto InvalidInstruction;

        // The first memory operand is passed via register, the second memory operand is RM.
        opReg = o0.as<Mem>().baseId();
        rmRel = &o1;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Out:
      if (isign3 == ENC_OPS2(Imm, Reg)) {
        if (ASMJIT_UNLIKELY(o1.id() != Gp::kIdAx))
          goto InvalidInstruction;

        opcode = x86AltOpcodeOf(instInfo) + (o1.x86RmSize() != 1);
        opcode.add66hBySize(o1.x86RmSize());

        immValue = o0.as<Imm>().valueAs<uint8_t>();
        immSize = 1;
        goto EmitX86Op;
      }

      if (isign3 == ENC_OPS2(Reg, Reg)) {
        if (ASMJIT_UNLIKELY(o0.id() != Gp::kIdDx || o1.id() != Gp::kIdAx))
          goto InvalidInstruction;

        opcode.add(o1.x86RmSize() != 1);
        opcode.add66hBySize(o1.x86RmSize());
        goto EmitX86Op;
      }
      break;

    case InstDB::kEncodingX86Outs:
      if (isign3 == ENC_OPS2(Reg, Mem)) {
        if (ASMJIT_UNLIKELY(o0.id() != Gp::kIdDx || !x86IsImplicitMem(o1, Gp::kIdSi)))
          goto InvalidInstruction;

        uint32_t size = o1.x86RmSize();
        if (ASMJIT_UNLIKELY(size == 0))
          goto AmbiguousOperandSize;

        rmRel = &o1;
        opcode.add(size != 1);
        opcode.add66hBySize(size);
        goto EmitX86OpImplicitMem;
      }
      break;

    case InstDB::kEncodingX86Pushw:
      if (isign3 == ENC_OPS1(Imm)) {
        immValue = o0.as<Imm>().value();
        immSize = 2;
        opcode = 0x68u | Opcode::kPP_66;
        goto EmitX86Op;
      }
      break;

    case InstDB::kEncodingX86Push:
      if (isign3 == ENC_OPS1(Reg)) {
        if (o0.isSegmentReg()) {
          uint32_t segment = o0.id();
          if (ASMJIT_UNLIKELY(segment >= SReg::kIdCount))
            goto InvalidSegment;

          opcode = x86OpcodePushSReg[segment];
          goto EmitX86Op;
        }
        else {
          goto CaseX86PushPop_Gp;
        }
      }

      if (isign3 == ENC_OPS1(Imm)) {
        immValue = o0.as<Imm>().value();
        immSize = 4;

        if (Support::isInt8(immValue) && !Support::test(options, InstOptions::kLongForm))
          immSize = 1;

        opcode = immSize == 1 ? 0x6A : 0x68;
        goto EmitX86Op;
      }
      [[fallthrough]];

    case InstDB::kEncodingX86Pop:
      if (isign3 == ENC_OPS1(Reg)) {
        if (o0.isSegmentReg()) {
          uint32_t segment = o0.id();
          if (ASMJIT_UNLIKELY(segment == SReg::kIdCs || segment >= SReg::kIdCount))
            goto InvalidSegment;

          opcode = x86OpcodePopSReg[segment];
          goto EmitX86Op;
        }
        else {
CaseX86PushPop_Gp:
          // We allow 2 byte, 4 byte, and 8 byte register sizes, although PUSH and POP only allow 2 bytes or
          // native size. On 64-bit we simply PUSH/POP 64-bit register even if 32-bit register was given.
          if (ASMJIT_UNLIKELY(o0.x86RmSize() < 2))
            goto InvalidInstruction;

          opcode = x86AltOpcodeOf(instInfo);
          opcode.add66hBySize(o0.x86RmSize());
          opReg = o0.id();
          goto EmitX86OpReg;
        }
      }

      if (isign3 == ENC_OPS1(Mem)) {
        if (ASMJIT_UNLIKELY(o0.x86RmSize() == 0))
          goto AmbiguousOperandSize;

        if (ASMJIT_UNLIKELY(o0.x86RmSize() != 2 && o0.x86RmSize() != registerSize()))
          goto InvalidInstruction;

        opcode.add66hBySize(o0.x86RmSize());
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Ret:
      if (isign3 == 0) {
        // 'ret' without immediate, change C2 to C3.
        opcode.add(1);
        goto EmitX86Op;
      }

      if (isign3 == ENC_OPS1(Imm)) {
        immValue = o0.as<Imm>().value();
        if (immValue == 0 && !Support::test(options, InstOptions::kLongForm)) {
          // 'ret' without immediate, change C2 to C3.
          opcode.add(1);
          goto EmitX86Op;
        }
        else {
          immSize = 2;
          goto EmitX86Op;
        }
      }
      break;

    case InstDB::kEncodingX86Rot:
      if (o0.isReg()) {
        opcode.addArithBySize(o0.x86RmSize());
        rbReg = o0.id();

        if (o0.x86RmSize() == 1)
          FIXUP_GPB(o0, rbReg);

        if (isign3 == ENC_OPS2(Reg, Reg)) {
          if (ASMJIT_UNLIKELY(o1.id() != Gp::kIdCx))
            goto InvalidInstruction;

          opcode += 2u;
          goto EmitX86R;
        }

        if (isign3 == ENC_OPS2(Reg, Imm)) {
          immValue = o1.as<Imm>().value() & 0xFF;
          immSize = 0;

          if (immValue == 1 && !Support::test(options, InstOptions::kLongForm))
            goto EmitX86R;

          opcode -= 0x10;
          immSize = 1;
          goto EmitX86R;
        }
      }
      else {
        if (ASMJIT_UNLIKELY(o0.x86RmSize() == 0))
          goto AmbiguousOperandSize;
        opcode.addArithBySize(o0.x86RmSize());

        if (isign3 == ENC_OPS2(Mem, Reg)) {
          if (ASMJIT_UNLIKELY(o1.id() != Gp::kIdCx))
            goto InvalidInstruction;

          opcode += 2u;
          rmRel = &o0;
          goto EmitX86M;
        }

        if (isign3 == ENC_OPS2(Mem, Imm)) {
          rmRel = &o0;
          immValue = o1.as<Imm>().value() & 0xFF;
          immSize = 0;

          if (immValue == 1 && !Support::test(options, InstOptions::kLongForm))
            goto EmitX86M;

          opcode -= 0x10;
          immSize = 1;
          goto EmitX86M;
        }
      }
      break;

    case InstDB::kEncodingX86Set:
      if (isign3 == ENC_OPS1(Reg)) {
        rbReg = o0.id();
        FIXUP_GPB(o0, rbReg);
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS1(Mem)) {
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86ShldShrd:
      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opcode.addPrefixBySize(o0.x86RmSize());
        opReg = o1.id();
        rbReg = o0.id();

        immValue = o2.as<Imm>().value();
        immSize = 1;
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS3(Mem, Reg, Imm)) {
        opcode.addPrefixBySize(o1.x86RmSize());
        opReg = o1.id();
        rmRel = &o0;

        immValue = o2.as<Imm>().value();
        immSize = 1;
        goto EmitX86M;
      }

      // The following instructions use opcode + 1.
      opcode.add(1);

      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        if (ASMJIT_UNLIKELY(o2.id() != Gp::kIdCx))
          goto InvalidInstruction;

        opcode.addPrefixBySize(o0.x86RmSize());
        opReg = o1.id();
        rbReg = o0.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS3(Mem, Reg, Reg)) {
        if (ASMJIT_UNLIKELY(o2.id() != Gp::kIdCx))
          goto InvalidInstruction;

        opcode.addPrefixBySize(o1.x86RmSize());
        opReg = o1.id();
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86StrRm:
      if (isign3 == ENC_OPS2(Reg, Mem)) {
        rmRel = &o1;
        if (ASMJIT_UNLIKELY(rmRel->as<Mem>().offsetLo32() || !o0.as<Reg>().isGp(Gp::kIdAx)))
          goto InvalidInstruction;

        uint32_t size = o0.x86RmSize();
        if (o1.x86RmSize() != 0u && ASMJIT_UNLIKELY(o1.x86RmSize() != size))
          goto OperandSizeMismatch;

        opcode.addArithBySize(size);
        goto EmitX86OpImplicitMem;
      }
      break;

    case InstDB::kEncodingX86StrMr:
      if (isign3 == ENC_OPS2(Mem, Reg)) {
        rmRel = &o0;
        if (ASMJIT_UNLIKELY(rmRel->as<Mem>().offsetLo32() || !o1.isGp(Gp::kIdAx)))
          goto InvalidInstruction;

        uint32_t size = o1.x86RmSize();
        if (o0.x86RmSize() != 0u && ASMJIT_UNLIKELY(o0.x86RmSize() != size))
          goto OperandSizeMismatch;

        opcode.addArithBySize(size);
        goto EmitX86OpImplicitMem;
      }
      break;

    case InstDB::kEncodingX86StrMm:
      if (isign3 == ENC_OPS2(Mem, Mem)) {
        if (ASMJIT_UNLIKELY(o0.as<Mem>().baseAndIndexTypes() !=
                            o1.as<Mem>().baseAndIndexTypes()))
          goto InvalidInstruction;

        rmRel = &o1;
        if (ASMJIT_UNLIKELY(o0.as<Mem>().hasOffset()))
          goto InvalidInstruction;

        uint32_t size = o1.x86RmSize();
        if (ASMJIT_UNLIKELY(size == 0))
          goto AmbiguousOperandSize;

        if (ASMJIT_UNLIKELY(o0.x86RmSize() != size))
          goto OperandSizeMismatch;

        opcode.addArithBySize(size);
        goto EmitX86OpImplicitMem;
      }
      break;

    case InstDB::kEncodingX86Test:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        if (o0.x86RmSize() != o1.x86RmSize())
          goto OperandSizeMismatch;

        opcode.addArithBySize(o0.x86RmSize());
        rbReg = o0.id();
        opReg = o1.id();

        if (o0.x86RmSize() != 1)
          goto EmitX86R;

        FIXUP_GPB(o0, rbReg);
        FIXUP_GPB(o1, opReg);
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opcode.addArithBySize(o1.x86RmSize());
        opReg = o1.id();
        rmRel = &o0;

        if (o1.x86RmSize() != 1)
          goto EmitX86M;

        FIXUP_GPB(o1, opReg);
        goto EmitX86M;
      }

      // The following instructions use the secondary opcode.
      opcode = x86AltOpcodeOf(instInfo);
      opReg = opcode.extractModO();

      if (isign3 == ENC_OPS2(Reg, Imm)) {
        opcode.addArithBySize(o0.x86RmSize());
        rbReg = o0.id();

        if (o0.x86RmSize() == 1) {
          FIXUP_GPB(o0, rbReg);
          immValue = o1.as<Imm>().valueAs<uint8_t>();
          immSize = 1;
        }
        else {
          immValue = o1.as<Imm>().value();
          immSize = FastUInt8(Support::min<uint32_t>(o0.x86RmSize(), 4));
        }

        // Short form - AL, AX, EAX, RAX.
        if (rbReg == 0 && !Support::test(options, InstOptions::kLongForm)) {
          opcode &= Opcode::kPP_66 | Opcode::kW;
          opcode |= 0xA8 + (o0.x86RmSize() != 1);
          goto EmitX86Op;
        }

        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Mem, Imm)) {
        if (ASMJIT_UNLIKELY(o0.x86RmSize() == 0))
          goto AmbiguousOperandSize;

        opcode.addArithBySize(o0.x86RmSize());
        rmRel = &o0;

        immValue = o1.as<Imm>().value();
        immSize = FastUInt8(Support::min<uint32_t>(o0.x86RmSize(), 4));
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Xchg:
      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opcode.addArithBySize(o0.x86RmSize());
        opReg = o0.id();
        rmRel = &o1;

        if (o0.x86RmSize() != 1)
          goto EmitX86M;

        FIXUP_GPB(o0, opReg);
        goto EmitX86M;
      }
      [[fallthrough]];

    case InstDB::kEncodingX86Xadd:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        rbReg = o0.id();
        opReg = o1.id();

        uint32_t opSize = o0.x86RmSize();
        if (opSize != o1.x86RmSize())
          goto OperandSizeMismatch;

        if (opSize == 1) {
          FIXUP_GPB(o0, rbReg);
          FIXUP_GPB(o1, opReg);
          goto EmitX86R;
        }

        // Special cases for 'xchg ?ax, reg'.
        if (instId == Inst::kIdXchg && (opReg == 0 || rbReg == 0)) {
          if (is64Bit() && opReg == rbReg && opSize >= 4) {
            if (opSize == 8) {
              // Encode 'xchg rax, rax' as '90' (REX and other prefixes are optional).
              opcode &= Opcode::kW;
              opcode |= 0x90;
              goto EmitX86OpReg;
            }
            else {
              // Encode 'xchg eax, eax' by using a generic path.
            }
          }
          else if (!Support::test(options, InstOptions::kLongForm)) {
            // The special encoding encodes only one register, which is non-zero.
            opReg += rbReg;

            opcode.addArithBySize(opSize);
            opcode &= Opcode::kW | Opcode::kPP_66;
            opcode |= 0x90;
            goto EmitX86OpReg;
          }
        }

        opcode.addArithBySize(opSize);
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opcode.addArithBySize(o1.x86RmSize());
        opReg = o1.id();
        rmRel = &o0;

        if (o1.x86RmSize() == 1) {
          FIXUP_GPB(o1, opReg);
        }

        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingX86Fence:
      rbReg = 0;
      goto EmitX86R;

    case InstDB::kEncodingX86Bndmov:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();

        // ModRM encoding:
        if (!Support::test(options, InstOptions::kX86_ModMR))
          goto EmitX86R;

        // ModMR encoding:
        opcode = x86AltOpcodeOf(instInfo);
        std::swap(opReg, rbReg);
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opcode = x86AltOpcodeOf(instInfo);

        rmRel = &o0;
        opReg = o1.id();
        goto EmitX86M;
      }
      break;

    // FPU Instructions
    // ----------------

    case InstDB::kEncodingFpuOp:
      goto EmitFpuOp;

    case InstDB::kEncodingFpuArith:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();

        // We switch to the alternative opcode if the first operand is zero.
        if (opReg == 0) {
CaseFpuArith_Reg:
          opcode = ((0xD8   << Opcode::kFPU_2B_Shift)       ) +
                   ((opcode >> Opcode::kFPU_2B_Shift) & 0xFF) + rbReg;
          goto EmitFpuOp;
        }
        else if (rbReg == 0) {
          rbReg = opReg;
          opcode = ((0xDC   << Opcode::kFPU_2B_Shift)       ) +
                   ((opcode                         ) & 0xFF) + rbReg;
          goto EmitFpuOp;
        }
        else {
          goto InvalidInstruction;
        }
      }

      if (isign3 == ENC_OPS1(Mem)) {
CaseFpuArith_Mem:
        // 0xD8/0xDC, depends on the size of the memory operand; opReg is valid.
        opcode = (o0.x86RmSize() == 4) ? 0xD8 : 0xDC;
        // Clear compressed displacement before going to EmitX86M.
        opcode &= ~uint32_t(Opcode::kCDSHL_Mask);

        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingFpuCom:
      if (isign3 == 0) {
        rbReg = 1;
        goto CaseFpuArith_Reg;
      }

      if (isign3 == ENC_OPS1(Reg)) {
        rbReg = o0.id();
        goto CaseFpuArith_Reg;
      }

      if (isign3 == ENC_OPS1(Mem)) {
        goto CaseFpuArith_Mem;
      }
      break;

    case InstDB::kEncodingFpuFldFst:
      if (isign3 == ENC_OPS1(Mem)) {
        rmRel = &o0;

        if (o0.x86RmSize() == 4 && commonInfo->hasFlag(InstDB::InstFlags::kFpuM32)) {
          goto EmitX86M;
        }

        if (o0.x86RmSize() == 8 && commonInfo->hasFlag(InstDB::InstFlags::kFpuM64)) {
          opcode += 4u;
          goto EmitX86M;
        }

        if (o0.x86RmSize() == 10 && commonInfo->hasFlag(InstDB::InstFlags::kFpuM80)) {
          opcode = x86AltOpcodeOf(instInfo);
          opReg  = opcode.extractModO();
          goto EmitX86M;
        }
      }

      if (isign3 == ENC_OPS1(Reg)) {
        if (instId == Inst::kIdFld ) { opcode = (0xD9 << Opcode::kFPU_2B_Shift) + 0xC0 + o0.id(); goto EmitFpuOp; }
        if (instId == Inst::kIdFst ) { opcode = (0xDD << Opcode::kFPU_2B_Shift) + 0xD0 + o0.id(); goto EmitFpuOp; }
        if (instId == Inst::kIdFstp) { opcode = (0xDD << Opcode::kFPU_2B_Shift) + 0xD8 + o0.id(); goto EmitFpuOp; }
      }
      break;

    case InstDB::kEncodingFpuM:
      if (isign3 == ENC_OPS1(Mem)) {
        // Clear compressed displacement before going to EmitX86M.
        opcode &= ~uint32_t(Opcode::kCDSHL_Mask);

        rmRel = &o0;
        if (o0.x86RmSize() == 2 && commonInfo->hasFlag(InstDB::InstFlags::kFpuM16)) {
          opcode += 4u;
          goto EmitX86M;
        }

        if (o0.x86RmSize() == 4 && commonInfo->hasFlag(InstDB::InstFlags::kFpuM32)) {
          goto EmitX86M;
        }

        if (o0.x86RmSize() == 8 && commonInfo->hasFlag(InstDB::InstFlags::kFpuM64)) {
          opcode = x86AltOpcodeOf(instInfo) & ~uint32_t(Opcode::kCDSHL_Mask);
          opReg  = opcode.extractModO();
          goto EmitX86M;
        }
      }
      break;

    case InstDB::kEncodingFpuRDef:
      if (isign3 == 0) {
        opcode += 1u;
        goto EmitFpuOp;
      }
      [[fallthrough]];

    case InstDB::kEncodingFpuR:
      if (isign3 == ENC_OPS1(Reg)) {
        opcode += o0.id();
        goto EmitFpuOp;
      }
      break;

    case InstDB::kEncodingFpuStsw:
      if (isign3 == ENC_OPS1(Reg)) {
        if (ASMJIT_UNLIKELY(o0.id() != Gp::kIdAx))
          goto InvalidInstruction;

        opcode = x86AltOpcodeOf(instInfo);
        goto EmitFpuOp;
      }

      if (isign3 == ENC_OPS1(Mem)) {
        // Clear compressed displacement before going to EmitX86M.
        opcode &= ~uint32_t(Opcode::kCDSHL_Mask);

        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    // Ext Instructions (Legacy Extensions)
    // ------------------------------------

    case InstDB::kEncodingExtPextrw:
      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opcode.add66hIf(o1.isVec128());

        immValue = o2.as<Imm>().value();
        immSize = 1;

        opReg = o0.id();
        rbReg = o1.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS3(Mem, Reg, Imm)) {
        // Secondary opcode of 'pextrw' instruction (SSE4.1).
        opcode = x86AltOpcodeOf(instInfo);
        opcode.add66hIf(o1.isVec128());

        immValue = o2.as<Imm>().value();
        immSize = 1;

        opReg = o1.id();
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingExtExtract:
      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opcode.add66hIf(o1.isVec128());

        immValue = o2.as<Imm>().value();
        immSize = 1;

        opReg = o1.id();
        rbReg = o0.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS3(Mem, Reg, Imm)) {
        opcode.add66hIf(o1.isVec128());

        immValue = o2.as<Imm>().value();
        immSize = 1;

        opReg = o1.id();
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingExtMov:
      // GP|MM|XMM <- GP|MM|XMM
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();

        if (!Support::test(options, InstOptions::kX86_ModMR) || !instInfo->_altOpcodeIndex)
          goto EmitX86R;

        opcode = x86AltOpcodeOf(instInfo);
        std::swap(opReg, rbReg);
        goto EmitX86R;
      }

      // GP|MM|XMM <- Mem
      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }

      // The following instruction uses opcode[1].
      opcode = x86AltOpcodeOf(instInfo);

      // Mem <- GP|MM|XMM
      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opReg = o1.id();
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingExtMovbe:
      if (isign3 == ENC_OPS2(Reg, Mem)) {
        if (o0.x86RmSize() == 1)
          goto InvalidInstruction;

        opcode.addPrefixBySize(o0.x86RmSize());
        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }

      // The following instruction uses the secondary opcode.
      opcode = x86AltOpcodeOf(instInfo);

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        if (o1.x86RmSize() == 1)
          goto InvalidInstruction;

        opcode.addPrefixBySize(o1.x86RmSize());
        opReg = o1.id();
        rmRel = &o0;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingExtMovd:
CaseExtMovd:
      if (x86IsMmxOrXmm(o0.as<Reg>())) {
        opReg = o0.id();
        opcode.add66hIf(o0.isVec128());

        // MM/XMM <- Gp
        if (isign3 == ENC_OPS2(Reg, Reg) && o1.as<Reg>().isGp()) {
          rbReg = o1.id();
          goto EmitX86R;
        }

        // MM/XMM <- Mem
        if (isign3 == ENC_OPS2(Reg, Mem)) {
          rmRel = &o1;
          goto EmitX86M;
        }
      }

      // The following instructions use the secondary opcode.
      if (x86IsMmxOrXmm(o1.as<Reg>())) {
        opcode &= Opcode::kW;
        opcode |= x86AltOpcodeOf(instInfo);
        opReg = o1.id();
        opcode.add66hIf(o1.isVec128());

        // GP <- MM/XMM
        if (isign3 == ENC_OPS2(Reg, Reg) && o0.as<Reg>().isGp()) {
          rbReg = o0.id();
          goto EmitX86R;
        }

        // Mem <- MM/XMM
        if (isign3 == ENC_OPS2(Mem, Reg)) {
          rmRel = &o0;
          goto EmitX86M;
        }
      }
      break;

    case InstDB::kEncodingExtMovq:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();

        // MM <- MM
        if (o0.isMmReg() && o1.isMmReg()) {
          opcode = Opcode::k000F00 | 0x6F;

          if (!Support::test(options, InstOptions::kX86_ModMR))
            goto EmitX86R;

          opcode += 0x10u;
          std::swap(opReg, rbReg);
          goto EmitX86R;
        }

        // XMM <- XMM
        if (o0.isVec128() && o1.isVec128()) {
          opcode = Opcode::kF30F00 | 0x7E;

          if (!Support::test(options, InstOptions::kX86_ModMR))
            goto EmitX86R;

          opcode = Opcode::k660F00 | 0xD6;
          std::swap(opReg, rbReg);
          goto EmitX86R;
        }
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;

        // MM <- Mem
        if (o0.isMmReg()) {
          opcode = Opcode::k000F00 | 0x6F;
          goto EmitX86M;
        }

        // XMM <- Mem
        if (o0.isVec128()) {
          opcode = Opcode::kF30F00 | 0x7E;
          goto EmitX86M;
        }
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opReg = o1.id();
        rmRel = &o0;

        // Mem <- MM
        if (o1.isMmReg()) {
          opcode = Opcode::k000F00 | 0x7F;
          goto EmitX86M;
        }

        // Mem <- XMM
        if (o1.isVec128()) {
          opcode = Opcode::k660F00 | 0xD6;
          goto EmitX86M;
        }
      }

      // MOVQ in other case is simply a MOVD instruction promoted to 64-bit.
      opcode |= Opcode::kW;
      goto CaseExtMovd;

    case InstDB::kEncodingExtRm_XMM0:
      if (ASMJIT_UNLIKELY(!o2.isNone() && !o2.isVec128(0)))
        goto InvalidInstruction;

      isign3 &= 0x3F;
      goto CaseExtRm;

    case InstDB::kEncodingExtRm_ZDI:
      if (ASMJIT_UNLIKELY(!o2.isNone() && !x86IsImplicitMem(o2, Gp::kIdDi)))
        goto InvalidInstruction;

      isign3 &= 0x3F;
      goto CaseExtRm;

    case InstDB::kEncodingExtRm_Wx:
      opcode.addWIf(o1.x86RmSize() == 8);
      [[fallthrough]];

    case InstDB::kEncodingExtRm_Wx_GpqOnly:
      opcode.addWIf(o0.isGp64());
      [[fallthrough]];

    case InstDB::kEncodingExtRm:
CaseExtRm:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingExtRm_P:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opcode.add66hIf(Support::bool_or(o0.isVec128(), o1.isVec128()));

        opReg = o0.id();
        rbReg = o1.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opcode.add66hIf(o0.isVec128());

        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingExtRmRi:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }

      // The following instruction uses the secondary opcode.
      opcode = x86AltOpcodeOf(instInfo);
      opReg  = opcode.extractModO();

      if (isign3 == ENC_OPS2(Reg, Imm)) {
        immValue = o1.as<Imm>().value();
        immSize = 1;

        rbReg = o0.id();
        goto EmitX86R;
      }
      break;

    case InstDB::kEncodingExtRmRi_P:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opcode.add66hIf(Support::bool_or(o0.isVec128(), o1.isVec128()));

        opReg = o0.id();
        rbReg = o1.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opcode.add66hIf(o0.isVec128());

        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }

      // The following instruction uses the secondary opcode.
      opcode = x86AltOpcodeOf(instInfo);
      opReg  = opcode.extractModO();

      if (isign3 == ENC_OPS2(Reg, Imm)) {
        opcode.add66hIf(o0.isVec128());

        immValue = o1.as<Imm>().value();
        immSize = 1;

        rbReg = o0.id();
        goto EmitX86R;
      }
      break;

    case InstDB::kEncodingExtRmi:
      immValue = o2.as<Imm>().value();
      immSize = 1;

      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opReg = o0.id();
        rbReg = o1.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS3(Reg, Mem, Imm)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }
      break;

    case InstDB::kEncodingExtRmi_P:
      immValue = o2.as<Imm>().value();
      immSize = 1;

      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opcode.add66hIf(Support::bool_or(o0.isVec128(), o1.isVec128()));

        opReg = o0.id();
        rbReg = o1.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS3(Reg, Mem, Imm)) {
        opcode.add66hIf(o0.isVec128());

        opReg = o0.id();
        rmRel = &o1;
        goto EmitX86M;
      }
      break;

    // Extrq & Insertq (SSE4A)
    // -----------------------

    case InstDB::kEncodingExtExtrq:
      opReg = o0.id();
      rbReg = o1.id();

      if (isign3 == ENC_OPS2(Reg, Reg))
        goto EmitX86R;

      if (isign3 == ENC_OPS3(Reg, Imm, Imm)) {
        // This variant of the instruction uses the secondary opcode.
        opcode = x86AltOpcodeOf(instInfo);
        rbReg = opReg;
        opReg = opcode.extractModO();

        immValue = (uint32_t(o1.as<Imm>().valueAs<uint8_t>())     ) +
                   (uint32_t(o2.as<Imm>().valueAs<uint8_t>()) << 8) ;
        immSize = 2;
        goto EmitX86R;
      }
      break;

    case InstDB::kEncodingExtInsertq: {
      const Operand_& o3 = opExt[EmitterUtils::kOp3];
      const uint32_t isign4 = isign3 + (uint32_t(o3.opType()) << 9);

      opReg = o0.id();
      rbReg = o1.id();

      if (isign4 == ENC_OPS2(Reg, Reg))
        goto EmitX86R;

      if (isign4 == ENC_OPS4(Reg, Reg, Imm, Imm)) {
        // This variant of the instruction uses the secondary opcode.
        opcode = x86AltOpcodeOf(instInfo);

        immValue = (uint32_t(o2.as<Imm>().valueAs<uint8_t>())     ) +
                   (uint32_t(o3.as<Imm>().valueAs<uint8_t>()) << 8) ;
        immSize = 2;
        goto EmitX86R;
      }
      break;
    }

    // 3DNOW Instructions
    // ------------------

    case InstDB::kEncodingExt3dNow:
      // Every 3dNow instruction starts with 0x0F0F and the actual opcode is
      // stored as 8-bit immediate.
      immValue = opcode.v & 0xFFu;
      immSize = 1;

      opcode = Opcode::k000F00 | 0x0F;
      opReg = o0.id();

      if (isign3 == ENC_OPS2(Reg, Reg)) {
        rbReg = o1.id();
        goto EmitX86R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        rmRel = &o1;
        goto EmitX86M;
      }
      break;

    // VEX/EVEX Instructions
    // ---------------------

    case InstDB::kEncodingVexOp:
      goto EmitVexOp;

    case InstDB::kEncodingVexOpMod:
      rbReg = 0;
      goto EmitVexEvexR;

    case InstDB::kEncodingVexKmov:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();

        // Form 'k, reg'.
        if (o1.as<Reg>().isGp()) {
          opcode = x86AltOpcodeOf(instInfo);
          goto EmitVexEvexR;
        }

        // Form 'reg, k'.
        if (o0.as<Reg>().isGp()) {
          opcode = x86AltOpcodeOf(instInfo) + 1;
          goto EmitVexEvexR;
        }

        // Form 'k, k'.
        if (!Support::test(options, InstOptions::kX86_ModMR))
          goto EmitVexEvexR;

        opcode.add(1);
        std::swap(opReg, rbReg);
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;

        goto EmitVexEvexM;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opcode.add(1);
        opReg = o1.id();
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexR_Wx:
      if (isign3 == ENC_OPS1(Reg)) {
        rbReg = o0.id();
        opcode.addWIf(o0.isGp64());
        goto EmitVexEvexR;
      }
      break;

    case InstDB::kEncodingVexM:
      if (isign3 == ENC_OPS1(Mem)) {
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexMr_Lx:
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());

      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o1.id();
        rbReg = o0.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opReg = o1.id();
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexMr_VM:
      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opcode |= Support::max(x86OpcodeLByVMem(o0), x86OpcodeLBySize(o1.x86RmSize()));

        opReg = o1.id();
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexMri_Vpextrw:
      // Use 'vpextrw reg, xmm1, i8' when possible.
      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opcode = Opcode::k660F00 | 0xC5;

        opReg = o0.id();
        rbReg = o1.id();

        immValue = o2.as<Imm>().value();
        immSize = 1;
        goto EmitVexEvexR;
      }

      goto CaseVexMri;

    case InstDB::kEncodingVexMvr_Wx:
      if (isign3 == ENC_OPS3(Mem, Reg, Reg)) {
        opcode.addWIf(o1.isGp64());
        opReg = x86PackRegAndVvvvv(o1.id(), o2.id());
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexMri_Lx:
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingVexMri:
CaseVexMri:
      immValue = o2.as<Imm>().value();
      immSize = 1;

      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opReg = o1.id();
        rbReg = o0.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Mem, Reg, Imm)) {
        opReg = o1.id();
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRm_ZDI:
      if (ASMJIT_UNLIKELY(!o2.isNone() && !x86IsImplicitMem(o2, Gp::kIdDi)))
        goto InvalidInstruction;

      isign3 &= 0x3F;
      goto CaseVexRm;

    case InstDB::kEncodingVexRm_Wx:
      opcode.addWIf(Support::bool_or(o0.isGp64(), o1.isGp64()));
      goto CaseVexRm;

    case InstDB::kEncodingVexRm_Lx_Narrow:
      if (o1.x86RmSize())
        opcode |= x86OpcodeLBySize(o1.x86RmSize());
      else if (o0.x86RmSize() == 32)
        opcode |= Opcode::kLL_2;
      goto CaseVexRm;

    case InstDB::kEncodingVexRm_Lx_Bcst:
      if (isign3 == ENC_OPS2(Reg, Reg) && o1.as<Reg>().isGp()) {
        opcode = x86AltOpcodeOf(instInfo) | x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
        opReg = o0.id();
        rbReg = o1.id();
        goto EmitVexEvexR;
      }
      [[fallthrough]];

    case InstDB::kEncodingVexRm_Lx:
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingVexRm:
CaseVexRm:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRm_VM:
      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opcode |= Support::max(x86OpcodeLByVMem(o1), x86OpcodeLBySize(o0.x86RmSize()));
        opReg = o0.id();
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRmi_Wx:
      opcode.addWIf(Support::bool_or(o0.isGp64(), o1.isGp64()));
      goto CaseVexRmi;

    case InstDB::kEncodingVexRmi_Lx:
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingVexRmi:
CaseVexRmi:
      immValue = o2.as<Imm>().value();
      immSize = 1;

      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opReg = o0.id();
        rbReg = o1.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Mem, Imm)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRvm:
CaseVexRvm:
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
CaseVexRvm_R:
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rbReg = o2.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Reg, Mem)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRvm_ZDX_Wx: {
      const Operand_& o3 = opExt[EmitterUtils::kOp3];
      if (ASMJIT_UNLIKELY(!o3.isNone() && !o3.isGp(Gp::kIdDx)))
        goto InvalidInstruction;
      [[fallthrough]];
    }

    case InstDB::kEncodingVexRvm_Wx: {
      opcode.addWIf(Support::bool_or(o0.isGp64(), o2.x86RmSize() == 8));
      goto CaseVexRvm;
    }

    case InstDB::kEncodingVexRvm_Lx_KEvex: {
      opcode.forceEvexIf(o0.isMaskReg());
      [[fallthrough]];
    }

    case InstDB::kEncodingVexRvm_Lx: {
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      goto CaseVexRvm;
    }

    case InstDB::kEncodingVexRvm_Lx_2xK: {
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        // Two registers are encoded as a single register.
        //   - First K register must be even.
        //   - Second K register must be first+1.
        if ((o0.id() & 1) != 0 || o0.id() + 1 != o1.id())
          goto InvalidPhysId;

        const Operand_& o3 = opExt[EmitterUtils::kOp3];

        opcode |= x86OpcodeLBySize(o2.x86RmSize());
        opReg = x86PackRegAndVvvvv(o0.id(), o2.id());

        if (o3.isReg()) {
          rbReg = o3.id();
          goto EmitVexEvexR;
        }

        if (o3.isMem()) {
          rmRel = &o3;
          goto EmitVexEvexM;
        }
      }
      break;
    }

    case InstDB::kEncodingVexRvmr_Lx: {
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];
    }

    case InstDB::kEncodingVexRvmr: {
      const Operand_& o3 = opExt[EmitterUtils::kOp3];
      const uint32_t isign4 = isign3 + (uint32_t(o3.opType()) << 9);

      immValue = o3.id() << 4;
      immSize = 1;

      if (isign4 == ENC_OPS4(Reg, Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rbReg = o2.id();
        goto EmitVexEvexR;
      }

      if (isign4 == ENC_OPS4(Reg, Reg, Mem, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;
        goto EmitVexEvexM;
      }
      break;
    }

    case InstDB::kEncodingVexRvmi_KEvex:
      opcode.forceEvexIf(o0.isMaskReg());
      goto VexRvmi;

    case InstDB::kEncodingVexRvmi_Lx_KEvex:
      opcode.forceEvexIf(o0.isMaskReg());
      [[fallthrough]];

    case InstDB::kEncodingVexRvmi_Lx:
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingVexRvmi:
VexRvmi:
    {
      const Operand_& o3 = opExt[EmitterUtils::kOp3];
      const uint32_t isign4 = isign3 + (uint32_t(o3.opType()) << 9);

      immValue = o3.as<Imm>().value();
      immSize = 1;

      if (isign4 == ENC_OPS4(Reg, Reg, Reg, Imm)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rbReg = o2.id();
        goto EmitVexEvexR;
      }

      if (isign4 == ENC_OPS4(Reg, Reg, Mem, Imm)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;
        goto EmitVexEvexM;
      }
      break;
    }

    case InstDB::kEncodingVexRmv_Wx:
      opcode.addWIf(Support::bool_or(o0.isGp64(), o2.isGp64()));
      [[fallthrough]];

    case InstDB::kEncodingVexRmv:
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o2.id());
        rbReg = o1.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Mem, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o2.id());
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRmvRm_VM:
      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opcode  = x86AltOpcodeOf(instInfo);
        opcode |= Support::max(x86OpcodeLByVMem(o1), x86OpcodeLBySize(o0.x86RmSize()));

        opReg = o0.id();
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      [[fallthrough]];

    case InstDB::kEncodingVexRmv_VM:
      if (isign3 == ENC_OPS3(Reg, Mem, Reg)) {
        opcode |= Support::max(x86OpcodeLByVMem(o1), x86OpcodeLBySize(o0.x86RmSize() | o2.x86RmSize()));

        opReg = x86PackRegAndVvvvv(o0.id(), o2.id());
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;


    case InstDB::kEncodingVexRmvi: {
      const Operand_& o3 = opExt[EmitterUtils::kOp3];
      const uint32_t isign4 = isign3 + (uint32_t(o3.opType()) << 9);

      immValue = o3.as<Imm>().value();
      immSize = 1;

      if (isign4 == ENC_OPS4(Reg, Reg, Reg, Imm)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o2.id());
        rbReg = o1.id();
        goto EmitVexEvexR;
      }

      if (isign4 == ENC_OPS4(Reg, Mem, Reg, Imm)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o2.id());
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;
    }

    case InstDB::kEncodingVexMovdMovq:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        if (o0.as<Reg>().isGp()) {
          opcode = x86AltOpcodeOf(instInfo);
          opcode.addWBySize(o0.x86RmSize());
          opReg = o1.id();
          rbReg = o0.id();
          goto EmitVexEvexR;
        }

        if (o1.as<Reg>().isGp()) {
          opcode.addWBySize(o1.x86RmSize());
          opReg = o0.id();
          rbReg = o1.id();
          goto EmitVexEvexR;
        }

        // If this is a 'W' version (movq) then allow also vmovq 'xmm|xmm' form.
        if (opcode & Opcode::kEvex_W_1) {
          opcode &= ~(Opcode::kPP_VEXMask | Opcode::kMM_Mask | 0xFF);
          opcode |=  (Opcode::kF30F00 | 0x7E);

          opReg = o0.id();
          rbReg = o1.id();
          goto EmitVexEvexR;
        }
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        if (opcode & Opcode::kEvex_W_1) {
          opcode &= ~(Opcode::kPP_VEXMask | Opcode::kMM_Mask | 0xFF);
          opcode |=  (Opcode::kF30F00 | 0x7E);
        }

        opReg = o0.id();
        rmRel = &o1;
        goto EmitVexEvexM;
      }

      // The following instruction uses the secondary opcode.
      opcode = x86AltOpcodeOf(instInfo);

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        if (opcode & Opcode::kEvex_W_1) {
          opcode &= ~(Opcode::kPP_VEXMask | Opcode::kMM_Mask | 0xFF);
          opcode |=  (Opcode::k660F00 | 0xD6);
        }

        opReg = o1.id();
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRmMr_Lx:
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingVexRmMr:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o0.id();
        rbReg = o1.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitVexEvexM;
      }

      // The following instruction uses the secondary opcode.
      opcode &= Opcode::kLL_Mask;
      opcode |= x86AltOpcodeOf(instInfo);

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opReg = o1.id();
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRvmRmv:
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o2.id());
        rbReg = o1.id();

        if (!Support::test(options, InstOptions::kX86_ModMR))
          goto EmitVexEvexR;

        opcode.addW();
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rbReg = o2.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Mem, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o2.id());
        rmRel = &o1;
        goto EmitVexEvexM;
      }

      if (isign3 == ENC_OPS3(Reg, Reg, Mem)) {
        opcode.addW();
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRvmRmi_Lx:
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingVexRvmRmi:
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rbReg = o2.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Reg, Mem)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;
        goto EmitVexEvexM;
      }

      // The following instructions use the secondary opcode.
      opcode &= Opcode::kLL_Mask;
      opcode |= x86AltOpcodeOf(instInfo);

      immValue = o2.as<Imm>().value();
      immSize = 1;

      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opReg = o0.id();
        rbReg = o1.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Mem, Imm)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRvmRmvRmi:
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o2.id());
        rbReg = o1.id();

        if (!Support::test(options, InstOptions::kX86_ModMR))
          goto EmitVexEvexR;

        opcode.addW();
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rbReg = o2.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Mem, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o2.id());
        rmRel = &o1;
        goto EmitVexEvexM;
      }

      if (isign3 == ENC_OPS3(Reg, Reg, Mem)) {
        opcode.addW();
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;
        goto EmitVexEvexM;
      }

      // The following instructions use the secondary opcode.
      opcode = x86AltOpcodeOf(instInfo);

      immValue = o2.as<Imm>().value();
      immSize = 1;

      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opReg = o0.id();
        rbReg = o1.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Mem, Imm)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRvmMr:
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rbReg = o2.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Reg, Mem)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;
        goto EmitVexEvexM;
      }

      // The following instructions use the secondary opcode.
      opcode = x86AltOpcodeOf(instInfo);

      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = o1.id();
        rbReg = o0.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opReg = o1.id();
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRvmMvr_Lx:
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingVexRvmMvr:
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rbReg = o2.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Reg, Mem)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;
        goto EmitVexEvexM;
      }

      // The following instruction uses the secondary opcode.
      opcode &= Opcode::kLL_Mask;
      opcode |= x86AltOpcodeOf(instInfo);

      if (isign3 == ENC_OPS3(Mem, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o2.id(), o1.id());
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexRvmVmi_Lx_MEvex:
      opcode.forceEvexIf(o1.isMem());
      [[fallthrough]];

    case InstDB::kEncodingVexRvmVmi_Lx:
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingVexRvmVmi:
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rbReg = o2.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Reg, Mem)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;
        goto EmitVexEvexM;
      }

      // The following instruction uses the secondary opcode.
      opcode &= Opcode::kLL_Mask | Opcode::kMM_ForceEvex;
      opcode |= x86AltOpcodeOf(instInfo);
      opReg = opcode.extractModO();

      immValue = o2.as<Imm>().value();
      immSize = 1;

      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opReg = x86PackRegAndVvvvv(opReg, o0.id());
        rbReg = o1.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Mem, Imm)) {
        opReg = x86PackRegAndVvvvv(opReg, o0.id());
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexVm_Wx:
      opcode.addWIf(Support::bool_or(o0.isGp64(), o1.isGp64()));
      [[fallthrough]];

    case InstDB::kEncodingVexVm:
      if (isign3 == ENC_OPS2(Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(opReg, o0.id());
        rbReg = o1.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = x86PackRegAndVvvvv(opReg, o0.id());
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexVmi_Lx_MEvex:
      if (isign3 == ENC_OPS3(Reg, Mem, Imm))
        opcode.forceEvex();
      [[fallthrough]];

    case InstDB::kEncodingVexVmi_Lx:
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingVexVmi:
      immValue = o2.as<Imm>().value();
      immSize = 1;

CaseVexVmi_AfterImm:
      if (isign3 == ENC_OPS3(Reg, Reg, Imm)) {
        opReg = x86PackRegAndVvvvv(opReg, o0.id());
        rbReg = o1.id();
        goto EmitVexEvexR;
      }

      if (isign3 == ENC_OPS3(Reg, Mem, Imm)) {
        opReg = x86PackRegAndVvvvv(opReg, o0.id());
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingVexVmi4_Wx:
      opcode.addWIf(Support::bool_or(o0.isGp64(), o1.x86RmSize() == 8));
      immValue = o2.as<Imm>().value();
      immSize = 4;
      goto CaseVexVmi_AfterImm;

    case InstDB::kEncodingVexRvrmRvmr_Lx:
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingVexRvrmRvmr: {
      const Operand_& o3 = opExt[EmitterUtils::kOp3];
      const uint32_t isign4 = isign3 + (uint32_t(o3.opType()) << 9);

      if (isign4 == ENC_OPS4(Reg, Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rbReg = o2.id();

        immValue = o3.id() << 4;
        immSize = 1;
        goto EmitVexEvexR;
      }

      if (isign4 == ENC_OPS4(Reg, Reg, Reg, Mem)) {
        opcode.addW();
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o3;

        immValue = o2.id() << 4;
        immSize = 1;
        goto EmitVexEvexM;
      }

      if (isign4 == ENC_OPS4(Reg, Reg, Mem, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;

        immValue = o3.id() << 4;
        immSize = 1;
        goto EmitVexEvexM;
      }
      break;
    }

    case InstDB::kEncodingVexRvrmiRvmri_Lx: {
      const Operand_& o3 = opExt[EmitterUtils::kOp3];
      const Operand_& o4 = opExt[EmitterUtils::kOp4];

      if (ASMJIT_UNLIKELY(!o4.isImm()))
        goto InvalidInstruction;

      const uint32_t isign4 = isign3 + (uint32_t(o3.opType()) << 9);
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize() | o2.x86RmSize() | o3.x86RmSize());

      immValue = o4.as<Imm>().valueAs<uint8_t>() & 0x0F;
      immSize = 1;

      if (isign4 == ENC_OPS4(Reg, Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rbReg = o2.id();

        immValue |= o3.id() << 4;
        goto EmitVexEvexR;
      }

      if (isign4 == ENC_OPS4(Reg, Reg, Reg, Mem)) {
        opcode.addW();
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o3;

        immValue |= o2.id() << 4;
        goto EmitVexEvexM;
      }

      if (isign4 == ENC_OPS4(Reg, Reg, Mem, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;

        immValue |= o3.id() << 4;
        goto EmitVexEvexM;
      }
      break;
    }

    case InstDB::kEncodingVexMovssMovsd:
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        goto CaseVexRvm_R;
      }

      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitVexEvexM;
      }

      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opcode = x86AltOpcodeOf(instInfo);
        opReg = o1.id();
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    // FMA4 Instructions
    // -----------------

    case InstDB::kEncodingFma4_Lx:
      // It's fine to just check the first operand, second is just for sanity.
      opcode |= x86OpcodeLBySize(o0.x86RmSize() | o1.x86RmSize());
      [[fallthrough]];

    case InstDB::kEncodingFma4: {
      const Operand_& o3 = opExt[EmitterUtils::kOp3];
      const uint32_t isign4 = isign3 + (uint32_t(o3.opType()) << 9);

      if (isign4 == ENC_OPS4(Reg, Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());

        if (!Support::test(options, InstOptions::kX86_ModMR)) {
          // MOD/RM - Encoding preferred by LLVM.
          opcode.addW();
          rbReg = o3.id();

          immValue = o2.id() << 4;
          immSize = 1;
          goto EmitVexEvexR;
        }
        else {
          // MOD/MR - Alternative encoding.
          rbReg = o2.id();

          immValue = o3.id() << 4;
          immSize = 1;
          goto EmitVexEvexR;
        }
      }

      if (isign4 == ENC_OPS4(Reg, Reg, Reg, Mem)) {
        opcode.addW();
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o3;

        immValue = o2.id() << 4;
        immSize = 1;
        goto EmitVexEvexM;
      }

      if (isign4 == ENC_OPS4(Reg, Reg, Mem, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o1.id());
        rmRel = &o2;

        immValue = o3.id() << 4;
        immSize = 1;
        goto EmitVexEvexM;
      }
      break;
    }

    // AMX Instructions
    // ----------------

    case InstDB::kEncodingAmxCfg:
      if (isign3 == ENC_OPS1(Mem)) {
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingAmxR:
      if (isign3 == ENC_OPS1(Reg)) {
        opReg = o0.id();
        rbReg = 0;
        goto EmitVexEvexR;
      }
      break;

    case InstDB::kEncodingAmxRm:
      if (isign3 == ENC_OPS2(Reg, Mem)) {
        opReg = o0.id();
        rmRel = &o1;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingAmxMr:
      if (isign3 == ENC_OPS2(Mem, Reg)) {
        opReg = o1.id();
        rmRel = &o0;
        goto EmitVexEvexM;
      }
      break;

    case InstDB::kEncodingAmxRmv:
      if (isign3 == ENC_OPS3(Reg, Reg, Reg)) {
        opReg = x86PackRegAndVvvvv(o0.id(), o2.id());
        rbReg = o1.id();
        goto EmitVexEvexR;
      }
      break;
  }

  goto InvalidInstruction;

  // Emit - X86 Opcode
  // -----------------

EmitX86OpMovAbs:
  immSize = FastUInt8(registerSize());
  writer.emitSegmentOverride(rmRel->as<Mem>().segmentId());

EmitX86Op:
  // Emit mandatory instruction prefix.
  writer.emitPP(opcode.v);

  // Emit REX prefix (64-bit only).
  {
    uint32_t rex = opcode.extractRex(options);
    if (ASMJIT_UNLIKELY(x86IsRexInvalid(rex)))
      goto InvalidRexPrefix;
    rex &= ~kX86ByteInvalidRex & 0xFF;
    writer.emit8If(rex | kX86ByteRex, rex != 0);
  }

  // Emit instruction opcodes.
  writer.emitMMAndOpcode(opcode.v);
  writer.emitImmediate(uint64_t(immValue), immSize);
  goto EmitDone;

  // Emit - X86 - Opcode + Reg
  // -------------------------

EmitX86OpReg:
  // Emit mandatory instruction prefix.
  writer.emitPP(opcode.v);

  // Emit REX prefix (64-bit only).
  {
    uint32_t rex = opcode.extractRex(options) | (opReg >> 3); // Rex.B (0x01).
    if (ASMJIT_UNLIKELY(x86IsRexInvalid(rex)))
      goto InvalidRexPrefix;
    rex &= ~kX86ByteInvalidRex & 0xFF;
    writer.emit8If(rex | kX86ByteRex, rex != 0);

    opReg &= 0x7;
  }

  // Emit instruction opcodes.
  opcode += opReg;
  writer.emitMMAndOpcode(opcode.v);
  writer.emitImmediate(uint64_t(immValue), immSize);
  goto EmitDone;

  // Emit - X86 - Opcode with Implicit <mem> Operand
  // -----------------------------------------------

EmitX86OpImplicitMem:
  rmInfo = x86MemInfo[rmRel->as<Mem>().baseAndIndexTypes()];
  if (ASMJIT_UNLIKELY(rmRel->as<Mem>().hasOffset() || (rmInfo & kX86MemInfo_Index)))
    goto InvalidInstruction;

  // Emit mandatory instruction prefix.
  writer.emitPP(opcode.v);

  // Emit REX prefix (64-bit only).
  {
    uint32_t rex = opcode.extractRex(options);
    if (ASMJIT_UNLIKELY(x86IsRexInvalid(rex)))
      goto InvalidRexPrefix;
    rex &= ~kX86ByteInvalidRex & 0xFF;
    writer.emit8If(rex | kX86ByteRex, rex != 0);
  }

  // Emit override prefixes.
  writer.emitSegmentOverride(rmRel->as<Mem>().segmentId());
  writer.emitAddressOverride((rmInfo & _addressOverrideMask()) != 0);

  // Emit instruction opcodes.
  writer.emitMMAndOpcode(opcode.v);

  // Emit immediate value.
  writer.emitImmediate(uint64_t(immValue), immSize);
  goto EmitDone;

  // Emit - X86 - Opcode /r - Register
  // ---------------------------------

EmitX86R:
  // Mandatory instruction prefix.
  writer.emitPP(opcode.v);

  // Emit REX prefix (64-bit only).
  {
    uint32_t rex = opcode.extractRex(options) |
                   ((opReg & 0x08) >> 1) | // REX.R (0x04).
                   ((rbReg & 0x08) >> 3) ; // REX.B (0x01).

    if (ASMJIT_UNLIKELY(x86IsRexInvalid(rex)))
      goto InvalidRexPrefix;
    rex &= ~kX86ByteInvalidRex & 0xFF;
    writer.emit8If(rex | kX86ByteRex, rex != 0);

    opReg &= 0x07;
    rbReg &= 0x07;
  }

  // Emit instruction opcodes.
  writer.emitMMAndOpcode(opcode.v);

  // Emit ModR.
  writer.emit8(x86EncodeMod(3, opReg, rbReg));

  // Emit immediate value.
  writer.emitImmediate(uint64_t(immValue), immSize);
  goto EmitDone;

  // Emit - X86 - Opcode /r - Memory Base
  // ------------------------------------

EmitX86RFromM:
  rmInfo = x86MemInfo[rmRel->as<Mem>().baseAndIndexTypes()];
  if (ASMJIT_UNLIKELY(rmRel->as<Mem>().hasOffset() || (rmInfo & kX86MemInfo_Index)))
    goto InvalidInstruction;

  // Emit mandatory instruction prefix.
  writer.emitPP(opcode.v);

  // Emit REX prefix (64-bit only).
  {
    uint32_t rex = opcode.extractRex(options) |
                   ((opReg & 0x08) >> 1) | // REX.R (0x04).
                   ((rbReg       ) >> 3) ; // REX.B (0x01).

    if (ASMJIT_UNLIKELY(x86IsRexInvalid(rex)))
      goto InvalidRexPrefix;
    rex &= ~kX86ByteInvalidRex & 0xFF;
    writer.emit8If(rex | kX86ByteRex, rex != 0);

    opReg &= 0x07;
    rbReg &= 0x07;
  }

  // Emit override prefixes.
  writer.emitSegmentOverride(rmRel->as<Mem>().segmentId());
  writer.emitAddressOverride((rmInfo & _addressOverrideMask()) != 0);

  // Emit instruction opcodes.
  writer.emitMMAndOpcode(opcode.v);

  // Emit ModR/M.
  writer.emit8(x86EncodeMod(3, opReg, rbReg));

  // Emit immediate value.
  writer.emitImmediate(uint64_t(immValue), immSize);
  goto EmitDone;

  // Emit - X86 - Opcode /r - memory Operand
  // ---------------------------------------

EmitX86M:
  // `rmRel` operand must be memory.
  ASMJIT_ASSERT(rmRel != nullptr);
  ASMJIT_ASSERT(rmRel->opType() == OperandType::kMem);
  ASMJIT_ASSERT((opcode & Opcode::kCDSHL_Mask) == 0);

  // Emit override prefixes.
  rmInfo = x86MemInfo[rmRel->as<Mem>().baseAndIndexTypes()];
  writer.emitSegmentOverride(rmRel->as<Mem>().segmentId());

  memOpAOMark = writer.cursor();
  writer.emitAddressOverride((rmInfo & _addressOverrideMask()) != 0);

  // Emit mandatory instruction prefix.
  writer.emitPP(opcode.v);

  // Emit REX prefix (64-bit only).
  rbReg = rmRel->as<Mem>().baseId();
  rxReg = rmRel->as<Mem>().indexId();
  {
    uint32_t rex;

    rex  = (rbReg >> 3) & 0x01; // REX.B (0x01).
    rex |= (rxReg >> 2) & 0x02; // REX.X (0x02).
    rex |= (opReg >> 1) & 0x04; // REX.R (0x04).

    rex &= rmInfo;
    rex |= opcode.extractRex(options);

    if (ASMJIT_UNLIKELY(x86IsRexInvalid(rex)))
      goto InvalidRexPrefix;
    rex &= ~kX86ByteInvalidRex & 0xFF;
    writer.emit8If(rex | kX86ByteRex, rex != 0);

    opReg &= 0x07;
  }

  // Emit instruction opcodes.
  writer.emitMMAndOpcode(opcode.v);

  // ... Fall through ...

  // Emit - MOD/SIB
  // --------------

EmitModSib:
  if (!(rmInfo & (kX86MemInfo_Index | kX86MemInfo_67H_X86))) {
    // ==========|> [BASE + DISP8|DISP32].
    if (rmInfo & kX86MemInfo_BaseGp) {
      rbReg &= 0x7;
      relOffset = rmRel->as<Mem>().offsetLo32();

      uint32_t mod = x86EncodeMod(0, opReg, rbReg);
      bool forceSIB = commonInfo->isTsibOp();

      if (rbReg == Gp::kIdSp || forceSIB) {
        // TSIB or [XSP|R12].
        mod = (mod & 0xF8u) | 0x04u;
        if (rbReg != Gp::kIdBp && relOffset == 0) {
          writer.emit8(mod);
          writer.emit8(x86EncodeSib(0, 4, rbReg));
        }
        // TSIB or [XSP|R12 + DISP8|DISP32].
        else {
          uint32_t cdShift = (opcode & Opcode::kCDSHL_Mask) >> Opcode::kCDSHL_Shift;
          int32_t cdOffset = relOffset >> cdShift;

          if (Support::isInt8(cdOffset) && relOffset == int32_t(uint32_t(cdOffset) << cdShift)) {
            writer.emit8(mod + 0x40); // <- MOD(1, opReg, rbReg).
            writer.emit8(x86EncodeSib(0, 4, rbReg));
            writer.emit8(cdOffset & 0xFF);
          }
          else {
            writer.emit8(mod + 0x80); // <- MOD(2, opReg, rbReg).
            writer.emit8(x86EncodeSib(0, 4, rbReg));
            writer.emit32uLE(uint32_t(relOffset));
          }
        }
      }
      else if (rbReg != Gp::kIdBp && relOffset == 0) {
        // [BASE].
        writer.emit8(mod);
      }
      else {
        // [BASE + DISP8|DISP32].
        uint32_t cdShift = (opcode & Opcode::kCDSHL_Mask) >> Opcode::kCDSHL_Shift;
        int32_t cdOffset = relOffset >> cdShift;

        if (Support::isInt8(cdOffset) && relOffset == int32_t(uint32_t(cdOffset) << cdShift)) {
          writer.emit8(mod + 0x40);
          writer.emit8(cdOffset & 0xFF);
        }
        else {
          writer.emit8(mod + 0x80);
          writer.emit32uLE(uint32_t(relOffset));
        }
      }
    }
    // ==========|> [ABSOLUTE | DISP32].
    else if (!(rmInfo & (kX86MemInfo_BaseLabel | kX86MemInfo_BaseRip))) {
      Mem::AddrType addrType = rmRel->as<Mem>().addrType();
      relOffset = rmRel->as<Mem>().offsetLo32();

      if (is32Bit()) {
        // Explicit relative addressing doesn't work in 32-bit mode.
        if (ASMJIT_UNLIKELY(addrType == Mem::AddrType::kRel))
          goto InvalidAddress;

        writer.emit8(x86EncodeMod(0, opReg, 5));
        writer.emit32uLE(uint32_t(relOffset));
      }
      else {
        bool isOffsetI32 = rmRel->as<Mem>().offsetHi32() == (relOffset >> 31);
        bool isOffsetU32 = rmRel->as<Mem>().offsetHi32() == 0;
        uint64_t baseAddress = code()->baseAddress();

        // If relative addressing was not explicitly set then we can try to guess. By guessing we check some
        // properties of the memory operand and try to base the decision on the segment prefix and the address type.
        if (addrType == Mem::AddrType::kDefault) {
          if (baseAddress == Globals::kNoBaseAddress) {
            // Prefer absolute addressing mode if the offset is 32-bit.
            addrType = isOffsetI32 || isOffsetU32 ? Mem::AddrType::kAbs
                                                  : Mem::AddrType::kRel;
          }
          else {
            // Prefer absolute addressing mode if FS|GS segment override is present.
            bool hasFsGs = rmRel->as<Mem>().segmentId() >= SReg::kIdFs;
            // Prefer absolute addressing mode if this is LEA with 32-bit immediate.
            bool isLea32 = (instId == Inst::kIdLea) && (isOffsetI32 || isOffsetU32);

            addrType = hasFsGs || isLea32 ? Mem::AddrType::kAbs
                                          : Mem::AddrType::kRel;
          }
        }

        if (addrType == Mem::AddrType::kRel) {
          uint32_t kModRel32Size = 5;
          uint64_t virtualOffset = uint64_t(writer.offsetFrom(_bufferData)) + immSize + kModRel32Size;

          if (baseAddress == Globals::kNoBaseAddress || _section->sectionId() != 0) {
            // Create a new RelocEntry as we cannot calculate the offset right now.
            err = _code->newRelocEntry(&re, RelocType::kAbsToRel);
            if (ASMJIT_UNLIKELY(err))
              goto Failed;

            writer.emit8(x86EncodeMod(0, opReg, 5));

            re->_sourceSectionId = _section->sectionId();
            re->_sourceOffset = offset();
            re->_format.resetToSimpleValue(OffsetType::kSignedOffset, 4);
            re->_format.setLeadingAndTrailingSize(writer.offsetFrom(_bufferPtr), immSize);
            re->_payload = uint64_t(rmRel->as<Mem>().offset());

            writer.emit32uLE(0);
            writer.emitImmediate(uint64_t(immValue), immSize);
            goto EmitDone;
          }
          else {
            uint64_t rip64 = baseAddress + _section->offset() + virtualOffset;
            uint64_t rel64 = uint64_t(rmRel->as<Mem>().offset()) - rip64;

            if (Support::isInt32(int64_t(rel64))) {
              writer.emit8(x86EncodeMod(0, opReg, 5));
              writer.emit32uLE(uint32_t(rel64 & 0xFFFFFFFFu));
              writer.emitImmediate(uint64_t(immValue), immSize);
              goto EmitDone;
            }
            else {
              // We must check the original address type as we have modified
              // `addrType`. We failed if the original address type is 'rel'.
              if (ASMJIT_UNLIKELY(rmRel->as<Mem>().isRel()))
                goto InvalidAddress;
            }
          }
        }

        // Handle unsigned 32-bit address that doesn't work with sign extension. Consider the following instructions:
        //
        //   1. lea rax, [-1]         - Sign extended to 0xFFFFFFFFFFFFFFFF
        //   2. lea rax, [0xFFFFFFFF] - Zero extended to 0x00000000FFFFFFFF
        //   3. add rax, [-1]         - Sign extended to 0xFFFFFFFFFFFFFFFF
        //   4. add rax, [0xFFFFFFFF] - Zero extended to 0x00000000FFFFFFFF
        //
        // Sign extension is naturally performed by the CPU so we don't have to bother, however, zero extension
        // requires address-size override prefix, which we probably don't have at this moment. So to make the address
        // valid we need to insert it at `memOpAOMark` if it's not already there.
        //
        // If this is 'lea' instruction then it's possible to remove REX.W part from REX prefix (if it's there), which
        // would be one-byte shorter than inserting address-size override.
        //
        // NOTE: If we don't do this then these instructions are unencodable.
        if (!isOffsetI32) {
          // 64-bit absolute address is unencodable.
          if (ASMJIT_UNLIKELY(!isOffsetU32))
            goto InvalidAddress64Bit;

          // We only patch the existing code if we don't have address-size override.
          if (*memOpAOMark != 0x67) {
            if (instId == Inst::kIdLea) {
              // LEA: Remove REX.W, if present. This is easy as we know that 'lea' doesn't use any PP prefix so if REX
              // prefix was emitted it would be at `memOpAOMark`.
              uint32_t rex = *memOpAOMark;
              if (rex & kX86ByteRex) {
                rex &= (~kX86ByteRexW) & 0xFF;
                *memOpAOMark = uint8_t(rex);

                // We can remove the REX prefix completely if it was not forced.
                if (rex == kX86ByteRex && !Support::test(options, InstOptions::kX86_Rex))
                  writer.remove8(memOpAOMark);
              }
            }
            else {
              // Any other instruction: Insert address-size override prefix.
              writer.insert8(memOpAOMark, 0x67);
            }
          }
        }

        // Emit 32-bit absolute address.
        writer.emit8(x86EncodeMod(0, opReg, 4));
        writer.emit8(x86EncodeSib(0, 4, 5));
        writer.emit32uLE(uint32_t(relOffset));
      }
    }
    // ==========|> [LABEL|RIP + DISP32]
    else {
      writer.emit8(x86EncodeMod(0, opReg, 5));

      if (is32Bit()) {
EmitModSib_LabelRip_X86:
        if (ASMJIT_UNLIKELY(_code->_relocations.willGrow(_code->allocator()) != kErrorOk))
          goto OutOfMemory;

        relOffset = rmRel->as<Mem>().offsetLo32();
        if (rmInfo & kX86MemInfo_BaseLabel) {
          // [LABEL->ABS].
          uint32_t baseLabelId = rmRel->as<Mem>().baseId();
          if (ASMJIT_UNLIKELY(!_code->isLabelValid(baseLabelId))) {
          }

          label = &_code->labelEntry(baseLabelId);
          err = _code->newRelocEntry(&re, RelocType::kRelToAbs);

          if (ASMJIT_UNLIKELY(err)) {
            goto Failed;
          }

          re->_sourceSectionId = _section->sectionId();
          re->_sourceOffset = offset();
          re->_format.resetToSimpleValue(OffsetType::kUnsignedOffset, 4);
          re->_format.setLeadingAndTrailingSize(writer.offsetFrom(_bufferPtr), immSize);
          re->_payload = uint64_t(int64_t(relOffset));

          if (label->isBound()) {
            // Label bound to the current section.
            re->_payload += label->offset();
            re->_targetSectionId = label->sectionId();
            writer.emit32uLE(0);
          }
          else {
            // Non-bound label or label bound to a different section.
            relOffset = -4 - int32_t(immSize);
            relSize = 4;
            goto EmitRel;
          }
        }
        else {
          // [RIP->ABS].
          err = _code->newRelocEntry(&re, RelocType::kRelToAbs);
          if (ASMJIT_UNLIKELY(err))
            goto Failed;

          re->_sourceSectionId = _section->sectionId();
          re->_targetSectionId = _section->sectionId();
          re->_format.resetToSimpleValue(OffsetType::kUnsignedOffset, 4);
          re->_format.setLeadingAndTrailingSize(writer.offsetFrom(_bufferPtr), immSize);
          re->_sourceOffset = offset();
          re->_payload = re->_sourceOffset + re->_format.regionSize() + uint64_t(int64_t(relOffset));

          writer.emit32uLE(0);
        }
      }
      else {
        relOffset = rmRel->as<Mem>().offsetLo32();
        if (rmInfo & kX86MemInfo_BaseLabel) {
          // [RIP].
          uint32_t baseLabelId = rmRel->as<Mem>().baseId();
          if (ASMJIT_UNLIKELY(!_code->isLabelValid(baseLabelId))) {
            goto InvalidLabel;
          }

          label = &_code->labelEntry(baseLabelId);
          relOffset -= (4 + immSize);

          if (label->isBoundTo(_section)) {
            // Label bound to the current section.
            relOffset += int32_t(label->offset() - writer.offsetFrom(_bufferData));
            writer.emit32uLE(uint32_t(relOffset));
          }
          else {
            // Non-bound label or label bound to a different section.
            relSize = 4;
            goto EmitRel;
          }
        }
        else {
          // [RIP].
          writer.emit32uLE(uint32_t(relOffset));
        }
      }
    }
  }
  else if (!(rmInfo & kX86MemInfo_67H_X86)) {
    // ESP|RSP can't be used as INDEX in pure SIB mode, however, VSIB mode allows XMM4|YMM4|ZMM4 (that's why the
    // check is before the label).
    if (ASMJIT_UNLIKELY(rxReg == Gp::kIdSp))
      goto InvalidAddressIndex;

EmitModVSib:
    rxReg &= 0x7;

    // ==========|> [BASE + INDEX + DISP8|DISP32].
    if (rmInfo & kX86MemInfo_BaseGp) {
      rbReg &= 0x7;
      relOffset = rmRel->as<Mem>().offsetLo32();

      uint32_t mod = x86EncodeMod(0, opReg, 4);
      uint32_t sib = x86EncodeSib(rmRel->as<Mem>().shift(), rxReg, rbReg);

      if (relOffset == 0 && rbReg != Gp::kIdBp) {
        // [BASE + INDEX << SHIFT].
        writer.emit8(mod);
        writer.emit8(sib);
      }
      else {
        uint32_t cdShift = (opcode & Opcode::kCDSHL_Mask) >> Opcode::kCDSHL_Shift;
        int32_t cdOffset = relOffset >> cdShift;

        if (Support::isInt8(cdOffset) && relOffset == int32_t(uint32_t(cdOffset) << cdShift)) {
          // [BASE + INDEX << SHIFT + DISP8].
          writer.emit8(mod + 0x40); // <- MOD(1, opReg, 4).
          writer.emit8(sib);
          writer.emit8(uint32_t(cdOffset));
        }
        else {
          // [BASE + INDEX << SHIFT + DISP32].
          writer.emit8(mod + 0x80); // <- MOD(2, opReg, 4).
          writer.emit8(sib);
          writer.emit32uLE(uint32_t(relOffset));
        }
      }
    }
    // ==========|> [INDEX + DISP32].
    else if (!(rmInfo & (kX86MemInfo_BaseLabel | kX86MemInfo_BaseRip))) {
      // [INDEX << SHIFT + DISP32].
      writer.emit8(x86EncodeMod(0, opReg, 4));
      writer.emit8(x86EncodeSib(rmRel->as<Mem>().shift(), rxReg, 5));

      relOffset = rmRel->as<Mem>().offsetLo32();
      writer.emit32uLE(uint32_t(relOffset));
    }
    // ==========|> [LABEL|RIP + INDEX + DISP32].
    else {
      if (is32Bit()) {
        writer.emit8(x86EncodeMod(0, opReg, 4));
        writer.emit8(x86EncodeSib(rmRel->as<Mem>().shift(), rxReg, 5));
        goto EmitModSib_LabelRip_X86;
      }
      else {
        // NOTE: This also handles VSIB+RIP, which is not allowed in 64-bit mode.
        goto InvalidAddress;
      }
    }
  }
  else {
    // 16-bit address mode (32-bit mode with 67 override prefix).
    relOffset = (int32_t(rmRel->as<Mem>().offsetLo32()) << 16) >> 16;

    // NOTE: 16-bit addresses don't use SIB byte and their encoding differs. We use a table-based approach to
    // calculate the proper MOD byte as it's easier. Also, not all BASE [+ INDEX] combinations are supported
    // in 16-bit mode, so this may fail.
    const uint32_t kBaseGpIdx = (kX86MemInfo_BaseGp | kX86MemInfo_Index);

    if (rmInfo & kBaseGpIdx) {
      // ==========|> [BASE + INDEX + DISP16].
      uint32_t mod;

      rbReg &= 0x7;
      rxReg &= 0x7;

      if ((rmInfo & kBaseGpIdx) == kBaseGpIdx) {
        uint32_t shf = rmRel->as<Mem>().shift();
        if (ASMJIT_UNLIKELY(shf != 0))
          goto InvalidAddress;
        mod = x86Mod16BaseIndexTable[(rbReg << 3) + rxReg];
      }
      else {
        if (rmInfo & kX86MemInfo_Index)
          rbReg = rxReg;
        mod = x86Mod16BaseTable[rbReg];
      }

      if (ASMJIT_UNLIKELY(mod == 0xFF))
        goto InvalidAddress;

      mod += opReg << 3;
      if (relOffset == 0 && mod != 0x06) {
        writer.emit8(mod);
      }
      else if (Support::isInt8(relOffset)) {
        writer.emit8(mod + 0x40);
        writer.emit8(uint32_t(relOffset));
      }
      else {
        writer.emit8(mod + 0x80);
        writer.emit16uLE(uint32_t(relOffset));
      }
    }
    else {
      // Not supported in 16-bit addresses.
      if (rmInfo & (kX86MemInfo_BaseRip | kX86MemInfo_BaseLabel))
        goto InvalidAddress;

      // ==========|> [DISP16].
      writer.emit8(opReg | 0x06);
      writer.emit16uLE(uint32_t(relOffset));
    }
  }

  writer.emitImmediate(uint64_t(immValue), immSize);
  goto EmitDone;

  // Emit - FPU
  // ----------

EmitFpuOp:
  // Mandatory instruction prefix.
  writer.emitPP(opcode.v);

  // FPU instructions consist of two opcodes.
  writer.emit8(opcode.v >> Opcode::kFPU_2B_Shift);
  writer.emit8(opcode.v);
  goto EmitDone;

  // Emit - VEX Opcode
  // -----------------

EmitVexOp:
  {
    // These don't use immediate.
    ASMJIT_ASSERT(immSize == 0);

    // Only 'vzeroall' and 'vzeroupper' instructions use this encoding, they don't define 'W' to be '1' so we can
    // just check the 'mmmmm' field. Both functions can encode by using VEX2 prefix so VEX3 is basically only used
    // when specified as instruction option.
    ASMJIT_ASSERT((opcode & Opcode::kW) == 0);

    uint32_t x = (uint32_t(opcode  & Opcode::kMM_Mask      ) >> (Opcode::kMM_Shift     )) |
                 (uint32_t(opcode  & Opcode::kLL_Mask      ) >> (Opcode::kLL_Shift - 10)) |
                 (uint32_t(opcode  & Opcode::kPP_VEXMask   ) >> (Opcode::kPP_Shift -  8)) ;

    if (Support::test(options, InstOptions::kX86_Vex3)) {
      x  = (x & 0xFFFF) << 8;                               // [00000000|00000Lpp|000mmmmm|00000000].
      x ^= (kX86ByteVex3) |                                 // [........|00000Lpp|000mmmmm|__VEX3__].
           (0x07u  << 13) |                                 // [........|00000Lpp|111mmmmm|__VEX3__].
           (0x0Fu  << 19) |                                 // [........|01111Lpp|111mmmmm|__VEX3__].
           (opcode << 24) ;                                 // [_OPCODE_|01111Lpp|111mmmmm|__VEX3__].

      writer.emit32uLE(x);
      goto EmitDone;
    }
    else {
      x = ((x >> 8) ^ x) ^ 0xF9;
      writer.emit8(kX86ByteVex2);
      writer.emit8(x);
      writer.emit8(opcode.v);
      goto EmitDone;
    }
  }

  // Emit - VEX|EVEX - /r - Register
  // -------------------------------

EmitVexEvexR:
  {
    // Construct `x` - a complete EVEX|VEX prefix.
    uint32_t x = ((opReg << 4) & 0xF980u) |                 // [........|........|Vvvvv..R|R.......].
                 ((rbReg << 2) & 0x0060u) |                 // [........|........|........|.BB.....].
                 (opcode.extractLLMMMMM(options)) |         // [........|.LL.....|Vvvvv..R|RBBmmmmm].
                 (_extraReg.id() << 16);                    // [........|.LL..aaa|Vvvvv..R|RBBmmmmm].
    opReg &= 0x7;

    // Handle AVX512 options by a single branch.
    const InstOptions kAvx512Options = InstOptions::kX86_ZMask | InstOptions::kX86_ER | InstOptions::kX86_SAE;
    if (Support::test(options, kAvx512Options)) {
      static constexpr uint32_t kBcstMask = 0x1 << 20;
      static constexpr uint32_t kLLMask10 = 0x2 << 21;
      static constexpr uint32_t kLLMask11 = 0x3 << 21;

      // Designed to be easily encodable so the position must be exact. The {rz-sae} is encoded as {11},
      // so it should match the mask.
      static_assert(uint32_t(InstOptions::kX86_RZ_SAE) == kLLMask11,
                    "This code requires InstOptions::X86_RZ_SAE to match kLLMask11 to work properly");

      x |= uint32_t(options & InstOptions::kX86_ZMask);     // [........|zLLb.aaa|Vvvvv..R|RBBmmmmm].

      // Support embedded-rounding {er} and suppress-all-exceptions {sae}.
      if (Support::test(options, InstOptions::kX86_ER | InstOptions::kX86_SAE)) {
        // Embedded rounding is only encodable if the instruction is either scalar or it's a 512-bit
        // operation as the {er} rounding predicate collides with LL part of the instruction.
        if ((x & kLLMask11) != kLLMask10) {
          // Ok, so LL is not 10, thus the instruction must be scalar. Scalar instructions don't
          // support broadcast so if this instruction supports it {er} nor {sae} would be encodable.
          if (ASMJIT_UNLIKELY(commonInfo->hasAvx512B()))
            goto InvalidEROrSAE;
        }

        if (Support::test(options, InstOptions::kX86_ER)) {
          if (ASMJIT_UNLIKELY(!commonInfo->hasAvx512ER()))
            goto InvalidEROrSAE;

          x &=~kLLMask11;                                   // [........|.00..aaa|Vvvvv..R|RBBmmmmm].
          x |= kBcstMask | (uint32_t(options) & kLLMask11); // [........|.LLb.aaa|Vvvvv..R|RBBmmmmm].
        }
        else {
          if (ASMJIT_UNLIKELY(!commonInfo->hasAvx512SAE()))
            goto InvalidEROrSAE;

          x &=~kLLMask11;                                   // [........|.00..aaa|Vvvvv..R|RBBmmmmm].
          x |= kBcstMask;                                   // [........|.00b.aaa|Vvvvv..R|RBBmmmmm].
        }
      }
    }

    // These bits would force EVEX prefix.
    constexpr uint32_t kEvexForce = 0x00000010u;            // [........|........|........|...x....].
    constexpr uint32_t kEvexBits = 0x00D78150u;             // [........|xx.x.xxx|x......x|.x.x....].

    // Force EVEX prefix even in case the instruction has VEX encoding, because EVEX encoding is preferred. At the
    // moment this is only required by AVX_VNNI instructions, which were added after AVX512_VNNI instructions. If
    // such instruction doesn't specify prefix, EVEX (AVX512_VNNI) is selected by default.
    if (commonInfo->preferEvex()) {
      if ((x & kEvexBits) == 0 && !Support::test(options, InstOptions::kX86_Vex | InstOptions::kX86_Vex3)) {
        x |= kEvexForce;
      }
    }

    // Check if EVEX is required by checking bits in `x` :     [........|xx.x.xxx|x......x|.x.x....].
    if (x & kEvexBits) {
      uint32_t y = ((x << 4) & 0x00080000u) |               // [........|...bV...|........|........].
                   ((x >> 4) & 0x00000010u) ;               // [........|...bV...|........|...R....].
      x  = (x & 0x00FF78EFu) | y;                           // [........|zLLbVaaa|0vvvv000|RBBRmmmm].
      x  = x << 8;                                          // [zLLbVaaa|0vvvv000|RBBRmmmm|00000000].
      x |= (opcode >> kVSHR_W    ) & 0x00800000u;           // [zLLbVaaa|Wvvvv000|RBBRmmmm|00000000].
      x |= (opcode >> kVSHR_PP_EW) & 0x00830000u;           // [zLLbVaaa|Wvvvv0pp|RBBRmmmm|00000000] (added PP and EVEX.W).
                                                            //      _     ____    ____
      x ^= 0x087CF000u | kX86ByteEvex;                      // [zLLbVaaa|Wvvvv1pp|RBBRmmmm|01100010].

      writer.emit32uLE(x);
      writer.emit8(opcode.v);

      rbReg &= 0x7;
      writer.emit8(x86EncodeMod(3, opReg, rbReg));
      writer.emitImmByteOrDWord(uint64_t(immValue), immSize);
      goto EmitDone;
    }

    // Not EVEX, prepare `x` for VEX2 or VEX3:             x = [........|00L00000|0vvvv000|R0Bmmmmm].
    x |= ((opcode >> (kVSHR_W  + 8)) & 0x8000u) |           // [00000000|00L00000|Wvvvv000|R0Bmmmmm].
         ((opcode >> (kVSHR_PP + 8)) & 0x0300u) |           // [00000000|00L00000|0vvvv0pp|R0Bmmmmm].
         ((x      >> 11            ) & 0x0400u) ;           // [00000000|00L00000|WvvvvLpp|R0Bmmmmm].
    x |= x86GetForceEvex3MaskInLastBit(options);            // [x0000000|00L00000|WvvvvLpp|R0Bmmmmm].

    // Check if VEX3 is required / forced:                     [x.......|........|x.......|..xxxxx.].
    if (x & 0x8000803Eu) {
      uint32_t xorMsk = x86VEXPrefix[x & 0xF] | (opcode << 24);

      // Clear all high bits.
      x  = (x & 0xFFFF) << 8;                               // [00000000|WvvvvLpp|R0Bmmmmm|00000000].
                                                            //            ____    _ _
      x ^= xorMsk;                                          // [_OPCODE_|WvvvvLpp|R1Bmmmmm|VEX3|XOP].
      writer.emit32uLE(x);

      rbReg &= 0x7;
      writer.emit8(x86EncodeMod(3, opReg, rbReg));
      writer.emitImmByteOrDWord(uint64_t(immValue), immSize);
      goto EmitDone;
    }
    else {
      // 'mmmmm' must be '00001'.
      ASMJIT_ASSERT((x & 0x1F) == 0x01);

      x = ((x >> 8) ^ x) ^ 0xF9;
      writer.emit8(kX86ByteVex2);
      writer.emit8(x);
      writer.emit8(opcode.v);

      rbReg &= 0x7;
      writer.emit8(x86EncodeMod(3, opReg, rbReg));
      writer.emitImmByteOrDWord(uint64_t(immValue), immSize);
      goto EmitDone;
    }
  }

  // Emit - VEX|EVEX - /r - Memory
  // -----------------------------

EmitVexEvexM:
  ASMJIT_ASSERT(rmRel != nullptr);
  ASMJIT_ASSERT(rmRel->opType() == OperandType::kMem);

  rmInfo = x86MemInfo[rmRel->as<Mem>().baseAndIndexTypes()];
  writer.emitSegmentOverride(rmRel->as<Mem>().segmentId());

  memOpAOMark = writer.cursor();
  writer.emitAddressOverride((rmInfo & _addressOverrideMask()) != 0);

  rbReg = rmRel->as<Mem>().hasBaseReg()  ? rmRel->as<Mem>().baseId()  : uint32_t(0);
  rxReg = rmRel->as<Mem>().hasIndexReg() ? rmRel->as<Mem>().indexId() : uint32_t(0);

  {
    uint32_t broadcastBit = uint32_t(rmRel->as<Mem>().hasBroadcast());

    // Construct `x` - a complete EVEX|VEX prefix.
    uint32_t x = ((opReg <<  4) & 0x0000F980u)  |           // [........|........|Vvvvv..R|R.......].
                 ((rxReg <<  3) & 0x00000040u)  |           // [........|........|........|.X......].
                 ((rxReg << 15) & 0x00080000u)  |           // [........|....X...|........|........].
                 ((rbReg <<  2) & 0x00000020u)  |           // [........|........|........|..B.....].
                 opcode.extractLLMMMMM(options) |           // [........|.LL.X...|Vvvvv..R|RXBmmmmm].
                 (_extraReg.id()    << 16)      |           // [........|.LL.Xaaa|Vvvvv..R|RXBmmmmm].
                 (broadcastBit      << 20)      ;           // [........|.LLbXaaa|Vvvvv..R|RXBmmmmm].
    opReg &= 0x07u;

    // Mark invalid VEX (force EVEX) case:                  // [@.......|.LLbXaaa|Vvvvv..R|RXBmmmmm].
    x |= uint32_t(~commonInfo->flags() & InstDB::InstFlags::kVex) << (31 - Support::ConstCTZ<uint32_t(InstDB::InstFlags::kVex)>::value);

    // Handle AVX512 options by a single branch.
    const InstOptions kAvx512Options = InstOptions::kX86_ZMask   |
                                       InstOptions::kX86_ER      |
                                       InstOptions::kX86_SAE     ;
    if (Support::test(options, kAvx512Options)) {
      // {er} and {sae} are both invalid if memory operand is used.
      if (ASMJIT_UNLIKELY(Support::test(options, InstOptions::kX86_ER | InstOptions::kX86_SAE)))
        goto InvalidEROrSAE;

      x |= uint32_t(options & InstOptions::kX86_ZMask);     // [@.......|zLLbXaaa|Vvvvv..R|RXBmmmmm].
    }

    // If these bits are used then EVEX prefix is required.
    constexpr uint32_t kEvexForce = 0x00000010u;            // [........|........|........|...x....].
    constexpr uint32_t kEvexBits = 0x80DF8110u;             // [@.......|xx.xxxxx|x......x|...x....].

    // Force EVEX prefix even in case the instruction has VEX encoding, because EVEX encoding is preferred. At the
    // moment this is only required for AVX_VNNI instructions, which were added after AVX512_VNNI instructions. If
    // such instruction doesn't specify prefix, EVEX (AVX512_VNNI) would be used by default,
    if (commonInfo->preferEvex()) {
      if ((x & kEvexBits) == 0 && !Support::test(options, InstOptions::kX86_Vex | InstOptions::kX86_Vex3)) {
        x |= kEvexForce;
      }
    }

    // Check if EVEX is required by checking bits in `x` :     [@.......|xx.xxxxx|x......x|...x....].
    if (x & kEvexBits) {
      uint32_t y = ((x << 4) & 0x00080000u) |               // [@.......|....V...|........|........].
                   ((x >> 4) & 0x00000010u) ;               // [@.......|....V...|........|...R....].
      x  = (x & 0x00FF78EFu) | y;                           // [........|zLLbVaaa|0vvvv000|RXBRmmmm].
      x  = x << 8;                                          // [zLLbVaaa|0vvvv000|RBBRmmmm|00000000].
      x |= (opcode >> kVSHR_W    ) & 0x00800000u;           // [zLLbVaaa|Wvvvv000|RBBRmmmm|00000000].
      x |= (opcode >> kVSHR_PP_EW) & 0x00830000u;           // [zLLbVaaa|Wvvvv0pp|RBBRmmmm|00000000] (added PP and EVEX.W).
                                                            //      _     ____    ____
      x ^= 0x087CF000u | kX86ByteEvex;                      // [zLLbVaaa|Wvvvv1pp|RBBRmmmm|01100010].

      if (x & 0x10000000u) {
        // Broadcast support.
        //
        // 1. Verify our LL field is correct as broadcast changes the "size" of the source operand. For example if
        //    a broadcasted operand is qword_ptr[X] {1to8} the source size becomes 64 and not 8 as the memory operand
        //    would report.
        //
        // 2. Change the compressed displacement scale to either x2 (SHL1), x4 (SHL 2), or x8 (SHL 3) depending on
        //    the broadcast unit/element size.
        uint32_t broadcastUnitSize = commonInfo->broadcastSize();
        uint32_t broadcastVectorSize = broadcastUnitSize << uint32_t(rmRel->as<Mem>().getBroadcast());

        if (ASMJIT_UNLIKELY(broadcastUnitSize == 0))
          goto InvalidBroadcast;

        // LL was already shifted 8 bits right.
        constexpr uint32_t kLLShift = 21 + 8;

        uint32_t currentLL = x & (0x3u << kLLShift);
        uint32_t broadcastLL = (Support::max<uint32_t>(Support::ctz(broadcastVectorSize), 4) - 4) << kLLShift;

        if (broadcastLL > (2u << kLLShift))
          goto InvalidBroadcast;

        uint32_t newLL = Support::max(currentLL, broadcastLL);
        x = (x & ~(uint32_t(0x3) << kLLShift)) | newLL;

        opcode &=~uint32_t(Opcode::kCDSHL_Mask);
        opcode |= Support::ctz(broadcastUnitSize) << Opcode::kCDSHL_Shift;
      }
      else {
        // Add the compressed displacement 'SHF' to the opcode based on 'TTWLL'.
        // The index to `x86CDisp8SHL` is composed as `CDTT[4:3] | W[2] | LL[1:0]`.
        uint32_t TTWLL = ((opcode >> (Opcode::kCDTT_Shift - 3)) & 0x18) +
                         ((opcode >> (Opcode::kW_Shift    - 2)) & 0x04) +
                         ((x >> 29) & 0x3);
        opcode += x86CDisp8SHL[TTWLL];
      }

      writer.emit32uLE(x);
      writer.emit8(opcode.v);
    }
    else {
      // Not EVEX, prepare `x` for VEX2 or VEX3:           x = [........|00L00000|0vvvv000|RXBmmmmm].
      x |= ((opcode >> (kVSHR_W  + 8)) & 0x8000u) |         // [00000000|00L00000|Wvvvv000|RXBmmmmm].
           ((opcode >> (kVSHR_PP + 8)) & 0x0300u) |         // [00000000|00L00000|Wvvvv0pp|RXBmmmmm].
           ((x      >> 11            ) & 0x0400u) ;         // [00000000|00L00000|WvvvvLpp|RXBmmmmm].
      x |= x86GetForceEvex3MaskInLastBit(options);          // [x0000000|00L00000|WvvvvLpp|RXBmmmmm].

      // Clear a possible CDisp specified by EVEX.
      opcode &= ~Opcode::kCDSHL_Mask;

      // Check if VEX3 is required / forced:                   [x.......|........|x.......|.xxxxxx.].
      if (x & 0x8000807Eu) {
        uint32_t xorMsk = x86VEXPrefix[x & 0xF] | (opcode << 24);

        // Clear all high bits.
        x  = (x & 0xFFFF) << 8;                             // [00000000|WvvvvLpp|RXBmmmmm|00000000].
                                                            //            ____    ___
        x ^= xorMsk;                                        // [_OPCODE_|WvvvvLpp|RXBmmmmm|VEX3_XOP].
        writer.emit32uLE(x);
      }
      else {
        // 'mmmmm' must be '00001'.
        ASMJIT_ASSERT((x & 0x1F) == 0x01);

        x = ((x >> 8) ^ x) ^ 0xF9;
        writer.emit8(kX86ByteVex2);
        writer.emit8(x);
        writer.emit8(opcode.v);
      }
    }
  }

  // MOD|SIB address.
  if (!commonInfo->hasFlag(InstDB::InstFlags::kVsib))
    goto EmitModSib;

  // MOD|VSIB address without INDEX is invalid.
  if (rmInfo & kX86MemInfo_Index)
    goto EmitModVSib;
  goto InvalidInstruction;

  // Emit - Jmp/Jcc/Call
  // -------------------

EmitJmpCall:
  {
    // Emit REX prefix if asked for (64-bit only).
    uint32_t rex = opcode.extractRex(options);
    if (ASMJIT_UNLIKELY(x86IsRexInvalid(rex)))
      goto InvalidRexPrefix;
    rex &= ~kX86ByteInvalidRex & 0xFF;
    writer.emit8If(rex | kX86ByteRex, rex != 0);

    uint64_t ip = uint64_t(writer.offsetFrom(_bufferData));
    uint32_t rel32 = 0;
    uint32_t opCode8 = x86AltOpcodeOf(instInfo);

    uint32_t inst8Size  = 1 + 1; //          OPCODE + REL8 .
    uint32_t inst32Size = 1 + 4; // [PREFIX] OPCODE + REL32.

    // Jcc instructions with 32-bit displacement use 0x0F prefix,
    // other instructions don't. No other prefixes are used by X86.
    ASMJIT_ASSERT((opCode8 & Opcode::kMM_Mask) == 0);
    ASMJIT_ASSERT((opcode  & Opcode::kMM_Mask) == 0 ||
                  (opcode  & Opcode::kMM_Mask) == Opcode::kMM_0F);

    // Only one of these should be used at the same time.
    inst32Size += uint32_t(opReg != 0);
    inst32Size += uint32_t((opcode & Opcode::kMM_Mask) == Opcode::kMM_0F);

    if (rmRel->isLabel()) {
      uint32_t labelId = rmRel->as<Label>().id();
      if (ASMJIT_UNLIKELY(!_code->isLabelValid(labelId))) {
        goto InvalidLabel;
      }

      label = &_code->labelEntry(labelId);
      if (label->isBoundTo(_section)) {
        // Label bound to the current section.
        rel32 = uint32_t((label->offset() - ip - inst32Size) & 0xFFFFFFFFu);
        goto EmitJmpCallRel;
      }
      else {
        // Non-bound label or label bound to a different section.
        if (opCode8 && (!opcode.v || Support::test(options, InstOptions::kShortForm))) {
          writer.emit8(opCode8);

          // Record DISP8 (non-bound label).
          relOffset = -1;
          relSize = 1;
          goto EmitRel;
        }
        else {
          // Refuse also 'short' prefix, if specified.
          if (ASMJIT_UNLIKELY(!opcode.v || Support::test(options, InstOptions::kShortForm)))
            goto InvalidDisplacement;

          writer.emit8If(0x0F, (opcode & Opcode::kMM_Mask) != 0);// Emit 0F prefix.
          writer.emit8(opcode.v);                                // Emit opcode.
          writer.emit8If(x86EncodeMod(3, opReg, 0), opReg != 0); // Emit MOD.

          // Record DISP32 (non-bound label).
          relOffset = -4;
          relSize = 4;
          goto EmitRel;
        }
      }
    }

    if (rmRel->isImm()) {
      uint64_t baseAddress = code()->baseAddress();
      uint64_t jumpAddress = rmRel->as<Imm>().valueAs<uint64_t>();

      // If the base-address is known calculate a relative displacement and check if it fits in 32 bits (which is
      // always true in 32-bit mode). Emit relative displacement as it was a bound label if all checks are ok.
      if (baseAddress != Globals::kNoBaseAddress) {
        uint64_t rel64 = jumpAddress - (ip + baseAddress) - inst32Size;
        if (Environment::is32Bit(arch()) || Support::isInt32(int64_t(rel64))) {
          rel32 = uint32_t(rel64 & 0xFFFFFFFFu);
          goto EmitJmpCallRel;
        }
        else {
          // Relative displacement exceeds 32-bits - relocator can only insert trampoline for jmp/call, but not
          // for jcc/jecxz.
          if (ASMJIT_UNLIKELY(!x86IsJmpOrCall(instId)))
            goto InvalidDisplacement;
        }
      }

      err = _code->newRelocEntry(&re, RelocType::kAbsToRel);
      if (ASMJIT_UNLIKELY(err))
        goto Failed;

      re->_sourceOffset = offset();
      re->_sourceSectionId = _section->sectionId();
      re->_payload = jumpAddress;

      if (ASMJIT_LIKELY(opcode.v)) {
        // 64-bit: Emit REX prefix so the instruction can be patched later. REX prefix does nothing if not patched,
        // but allows to patch the instruction to use MOD/M and to point to a memory where the final 64-bit address
        // is stored.
        if (Environment::is64Bit(arch()) && x86IsJmpOrCall(instId)) {
          if (!rex)
            writer.emit8(kX86ByteRex);

          err = _code->addAddressToAddressTable(jumpAddress);
          if (ASMJIT_UNLIKELY(err))
            goto Failed;

          re->_relocType = RelocType::kX64AddressEntry;
        }

        writer.emit8If(0x0F, (opcode & Opcode::kMM_Mask) != 0);  // Emit 0F prefix.
        writer.emit8(opcode.v);                                  // Emit opcode.
        writer.emit8If(x86EncodeMod(3, opReg, 0), opReg != 0);   // Emit MOD.
        re->_format.resetToSimpleValue(OffsetType::kSignedOffset, 4);
        re->_format.setLeadingAndTrailingSize(writer.offsetFrom(_bufferPtr), immSize);
        writer.emit32uLE(0);                                     // Emit DISP32.
      }
      else {
        writer.emit8(opCode8);                                   // Emit opcode.
        re->_format.resetToSimpleValue(OffsetType::kSignedOffset, 1);
        re->_format.setLeadingAndTrailingSize(writer.offsetFrom(_bufferPtr), immSize);
        writer.emit8(0);                                         // Emit DISP8 (zero).
      }
      goto EmitDone;
    }

    // Not Label|Imm -> Invalid.
    goto InvalidInstruction;

    // Emit jmp/call with relative displacement known at assembly-time. Decide between 8-bit and 32-bit displacement
    // encoding. Some instructions only allow either 8-bit or 32-bit encoding, others allow both encodings.
EmitJmpCallRel:
    if (Support::isInt8(int32_t(rel32 + inst32Size - inst8Size)) && opCode8 && !Support::test(options, InstOptions::kLongForm)) {
      options |= InstOptions::kShortForm;
      writer.emit8(opCode8);                                     // Emit opcode
      writer.emit8(rel32 + inst32Size - inst8Size);              // Emit DISP8.
      goto EmitDone;
    }
    else {
      if (ASMJIT_UNLIKELY(!opcode.v || Support::test(options, InstOptions::kShortForm)))
        goto InvalidDisplacement;

      options &= ~InstOptions::kShortForm;
      writer.emit8If(0x0F, (opcode & Opcode::kMM_Mask) != 0);    // Emit 0x0F prefix.
      writer.emit8(opcode.v);                                    // Emit Opcode.
      writer.emit8If(x86EncodeMod(3, opReg, 0), opReg != 0);     // Emit MOD.
      writer.emit32uLE(rel32);                                   // Emit DISP32.
      goto EmitDone;
    }
  }

  // Emit - Relative
  // ---------------

EmitRel:
  {
    ASMJIT_ASSERT(relSize == 1 || relSize == 4);

    // Chain with label.
    size_t offset = size_t(writer.offsetFrom(_bufferData));
    OffsetFormat of;
    of.resetToSimpleValue(OffsetType::kSignedOffset, relSize);

    Fixup* fixup = _code->newFixup(*label, _section->sectionId(), offset, relOffset, of);
    if (ASMJIT_UNLIKELY(!fixup)) {
      goto OutOfMemory;
    }

    if (re) {
      fixup->labelOrRelocId = re->id();
    }

    // Emit dummy zeros, must be patched later when the reference becomes known.
    writer.emitZeros(relSize);
  }
  writer.emitImmediate(uint64_t(immValue), immSize);

  // Emit - Done
  // -----------

EmitDone:
  if (Support::test(options, InstOptions::kReserved)) {
#ifndef ASMJIT_NO_LOGGING
    if (_logger)
      EmitterUtils::logInstructionEmitted(this, instId, options, o0, o1, o2, opExt, relSize, immSize, writer.cursor());
#endif
  }

  resetState();
  writer.done(this);
  return kErrorOk;

  // Error Handler
  // -------------

#define ERROR_HANDLER(ERR) ERR: err = DebugUtils::errored(kError##ERR); goto Failed;
  ERROR_HANDLER(OutOfMemory)
  ERROR_HANDLER(InvalidLabel)
  ERROR_HANDLER(InvalidInstruction)
  ERROR_HANDLER(InvalidLockPrefix)
  ERROR_HANDLER(InvalidXAcquirePrefix)
  ERROR_HANDLER(InvalidXReleasePrefix)
  ERROR_HANDLER(InvalidRepPrefix)
  ERROR_HANDLER(InvalidRexPrefix)
  ERROR_HANDLER(InvalidEROrSAE)
  ERROR_HANDLER(InvalidAddress)
  ERROR_HANDLER(InvalidAddressIndex)
  ERROR_HANDLER(InvalidAddress64Bit)
  ERROR_HANDLER(InvalidDisplacement)
  ERROR_HANDLER(InvalidPhysId)
  ERROR_HANDLER(InvalidSegment)
  ERROR_HANDLER(InvalidImmediate)
  ERROR_HANDLER(InvalidBroadcast)
  ERROR_HANDLER(OperandSizeMismatch)
  ERROR_HANDLER(AmbiguousOperandSize)
#undef ERROR_HANDLER

Failed:
#ifndef ASMJIT_NO_LOGGING
  return EmitterUtils::logInstructionFailed(this, err, instId, options, o0, o1, o2, opExt);
#else
  resetState();
  return reportError(err);
#endif
}

//x86::Assembler - Align
// =====================

Error Assembler::align(AlignMode alignMode, uint32_t alignment) {
  if (ASMJIT_UNLIKELY(!_code))
    return reportError(DebugUtils::errored(kErrorNotInitialized));

  if (ASMJIT_UNLIKELY(uint32_t(alignMode) > uint32_t(AlignMode::kMaxValue)))
    return reportError(DebugUtils::errored(kErrorInvalidArgument));

  if (alignment <= 1)
    return kErrorOk;

  if (ASMJIT_UNLIKELY(!Support::isPowerOf2UpTo(alignment, Globals::kMaxAlignment)))
    return reportError(DebugUtils::errored(kErrorInvalidArgument));

  uint32_t i = uint32_t(Support::alignUpDiff<size_t>(offset(), alignment));
  if (i > 0) {
    CodeWriter writer(this);
    ASMJIT_PROPAGATE(writer.ensureSpace(this, i));

    uint8_t pattern = 0x00;
    switch (alignMode) {
      case AlignMode::kCode: {
        if (hasEncodingOption(EncodingOptions::kOptimizedAlign)) {
          // Intel 64 and IA-32 Architectures Software Developer's Manual - Volume 2B (NOP).
          static constexpr uint32_t kMaxNopSize = 9;

          static const uint8_t nopData[kMaxNopSize][kMaxNopSize] = {
            { 0x90 },
            { 0x66, 0x90 },
            { 0x0F, 0x1F, 0x00 },
            { 0x0F, 0x1F, 0x40, 0x00 },
            { 0x0F, 0x1F, 0x44, 0x00, 0x00 },
            { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 },
            { 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 },
            { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
            { 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 }
          };

          do {
            uint32_t n = Support::min<uint32_t>(i, kMaxNopSize);
            const uint8_t* src = nopData[n - 1];

            i -= n;
            do {
              writer.emit8(*src++);
            } while (--n);
          } while (i);
        }

        pattern = 0x90;
        break;
      }

      case AlignMode::kData:
        pattern = 0xCC;
        break;

      case AlignMode::kZero:
        // Pattern already set to zero.
        break;
    }

    while (i) {
      writer.emit8(pattern);
      i--;
    }

    writer.done(this);
  }

#ifndef ASMJIT_NO_LOGGING
  if (_logger) {
    StringTmp<128> sb;
    sb.appendChars(' ', _logger->indentation(FormatIndentationGroup::kCode));
    sb.appendFormat("align %u\n", alignment);
    _logger->log(sb);
  }
#endif

  return kErrorOk;
}

// x86::Assembler - Events
// =======================

Error Assembler::onAttach(CodeHolder& code) noexcept {
  Arch arch = code.arch();
  ASMJIT_PROPAGATE(Base::onAttach(code));

  _instructionAlignment = uint8_t(1);
  updateEmitterFuncs(this);

  if (Environment::is32Bit(arch)) {
    // 32 bit architecture - X86.
    _forcedInstOptions |= InstOptions::kX86_InvalidRex;
    _setAddressOverrideMask(kX86MemInfo_67H_X86);
  }
  else {
    // 64 bit architecture - X64.
    _forcedInstOptions &= ~InstOptions::kX86_InvalidRex;
    _setAddressOverrideMask(kX86MemInfo_67H_X64);
  }

  return kErrorOk;
}

Error Assembler::onDetach(CodeHolder& code) noexcept {
  _forcedInstOptions &= ~InstOptions::kX86_InvalidRex;
  _setAddressOverrideMask(0);
  return Base::onDetach(code);
}

ASMJIT_END_SUB_NAMESPACE

#endif // !ASMJIT_NO_X86
