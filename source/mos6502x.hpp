#pragma once

// MOS 6502X: cycle-accurate 6502 as implemented by BizHawk's NesHawk core.
//
// TRANSLATED 1:1 from BizHawk (MIT license, original C) 2009-2025 BizHawk team):
//   src/BizHawk.Emulation.Cores/CPUs/MOS 6502X/{Execute.cs, MOS6502X.cs}
// by extern/nesHawk/tools/transliterate_cpu.py -- regenerate with that script; hand-edits go in
// the FIXUPS section of the script, not here. Structure (microcode table, stage methods,
// dispatch switch) intentionally mirrors the original so the two stay line-auditable.
//
// TLink must provide: uint8_t ReadMemory(uint16_t), uint8_t DummyReadMemory(uint16_t),
//                     uint8_t PeekMemory(uint16_t), void WriteMemory(uint16_t, uint8_t),
//                     void OnExecFetch(uint16_t)

#include <cstdint>

namespace nesHawk
{

#ifdef _NESHAWK_DETECT_BAD_ACCESS
// isOfficialOpcode[b] == 1 for the 151 documented NMOS 6502 opcodes, 0 for the other 105 (unofficial).
// Same verified table as the QuickerNES bad-access detector. A frame that fetches an unofficial
// opcode (or code from RAM) has derailed into data-as-code -- the U+X glitch signature.
inline constexpr uint8_t cpu_isOfficialOpcode[256] = {
  /*0x00*/ 1,1,0,0,0,1,1,0, 1,1,1,0,0,1,1,0,
  /*0x10*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0,
  /*0x20*/ 1,1,0,0,1,1,1,0, 1,1,1,0,1,1,1,0,
  /*0x30*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0,
  /*0x40*/ 1,1,0,0,0,1,1,0, 1,1,1,0,1,1,1,0,
  /*0x50*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0,
  /*0x60*/ 1,1,0,0,0,1,1,0, 1,1,1,0,1,1,1,0,
  /*0x70*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0,
  /*0x80*/ 0,1,0,0,1,1,1,0, 1,0,1,0,1,1,1,0,
  /*0x90*/ 1,1,0,0,1,1,1,0, 1,1,1,0,0,1,0,0,
  /*0xA0*/ 1,1,1,0,1,1,1,0, 1,1,1,0,1,1,1,0,
  /*0xB0*/ 1,1,0,0,1,1,1,0, 1,1,1,0,1,1,1,0,
  /*0xC0*/ 1,1,0,0,1,1,1,0, 1,1,1,0,1,1,1,0,
  /*0xD0*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0,
  /*0xE0*/ 1,1,0,0,1,1,1,0, 1,1,1,0,1,1,1,0,
  /*0xF0*/ 1,1,0,0,0,1,1,0, 1,1,0,0,0,1,1,0,
};
#endif

template <typename TLink>
class MOS6502X
{
public:
  TLink& _link;

  explicit MOS6502X(TLink& link) : _link(link) { Reset(); }

  enum class Uop : uint16_t
  {

    //sometimes i used this as a marker for unsupported instructions, but it is very inconsistent
    Unsupported,

    Fetch1, Fetch1_Real, Fetch2, Fetch3,
    //used by instructions with no second opcode byte (6502 fetches a byte anyway but won't increment PC for these)
    FetchDummy,

    NOP,

    JSR,
    IncPC, //from RTS

    //[absolute WRITE]
    Abs_WRITE_STA, Abs_WRITE_STX, Abs_WRITE_STY,
    Abs_WRITE_SAX, //unofficials
    //[absolute READ]
    Abs_READ_BIT, Abs_READ_LDA, Abs_READ_LDY, Abs_READ_ORA, Abs_READ_LDX, Abs_READ_CMP, Abs_READ_ADC, Abs_READ_CPX, Abs_READ_SBC, Abs_READ_AND, Abs_READ_EOR, Abs_READ_CPY, Abs_READ_NOP,
    Abs_READ_LAX, //unofficials
    //[absolute RMW]
    Abs_RMW_Stage4, Abs_RMW_Stage6,
    Abs_RMW_Stage5_INC, Abs_RMW_Stage5_DEC, Abs_RMW_Stage5_LSR, Abs_RMW_Stage5_ROL, Abs_RMW_Stage5_ASL, Abs_RMW_Stage5_ROR,
    Abs_RMW_Stage5_SLO, Abs_RMW_Stage5_RLA, Abs_RMW_Stage5_SRE, Abs_RMW_Stage5_RRA, Abs_RMW_Stage5_DCP, Abs_RMW_Stage5_ISC, //unofficials

    //[absolute JUMP]
    JMP_abs,

    //[zero page misc]
    ZpIdx_Stage3_X, ZpIdx_Stage3_Y,
    ZpIdx_RMW_Stage4, ZpIdx_RMW_Stage6,
    //[zero page WRITE]
    ZP_WRITE_STA, ZP_WRITE_STX, ZP_WRITE_STY, ZP_WRITE_SAX,
    //[zero page RMW]
    ZP_RMW_Stage3, ZP_RMW_Stage5,
    ZP_RMW_DEC, ZP_RMW_INC, ZP_RMW_ASL, ZP_RMW_LSR, ZP_RMW_ROR, ZP_RMW_ROL,
    ZP_RMW_SLO, ZP_RMW_RLA, ZP_RMW_SRE, ZP_RMW_RRA, ZP_RMW_DCP, ZP_RMW_ISC,
    //[zero page READ]
    ZP_READ_EOR, ZP_READ_BIT, ZP_READ_ORA, ZP_READ_LDA, ZP_READ_LDY, ZP_READ_LDX, ZP_READ_CPX, ZP_READ_SBC, ZP_READ_CPY, ZP_READ_NOP, ZP_READ_ADC, ZP_READ_AND, ZP_READ_CMP, ZP_READ_LAX,

    //[indexed indirect READ] (addr,X)
    //[indexed indirect WRITE] (addr,X)
    IdxInd_Stage3, IdxInd_Stage4, IdxInd_Stage5,
    IdxInd_Stage6_READ_ORA, IdxInd_Stage6_READ_SBC, IdxInd_Stage6_READ_LDA, IdxInd_Stage6_READ_EOR, IdxInd_Stage6_READ_CMP, IdxInd_Stage6_READ_ADC, IdxInd_Stage6_READ_AND,
    IdxInd_Stage6_READ_LAX,
    IdxInd_Stage6_WRITE_STA, IdxInd_Stage6_WRITE_SAX,
    IdxInd_Stage6_RMW, //work happens in stage 7
    IdxInd_Stage7_RMW_SLO, IdxInd_Stage7_RMW_RLA, IdxInd_Stage7_RMW_SRE, IdxInd_Stage7_RMW_RRA, IdxInd_Stage7_RMW_ISC, IdxInd_Stage7_RMW_DCP, //unofficials
    IdxInd_Stage8_RMW,

    //[absolute indexed]
    AbsIdx_Stage3_X, AbsIdx_Stage3_Y, AbsIdx_Stage4,
    //[absolute indexed WRITE]
    AbsIdx_WRITE_Stage5_STA,
    AbsIdx_WRITE_Stage5_SHY, AbsIdx_WRITE_Stage5_SHX, //unofficials
    AbsIdx_WRITE_Stage5_SHS,
    //[absolute indexed READ]
    AbsIdx_READ_Stage4,
    AbsIdx_READ_Stage5_LDA, AbsIdx_READ_Stage5_CMP, AbsIdx_READ_Stage5_SBC, AbsIdx_READ_Stage5_ADC, AbsIdx_READ_Stage5_EOR, AbsIdx_READ_Stage5_LDX, AbsIdx_READ_Stage5_AND, AbsIdx_READ_Stage5_ORA, AbsIdx_READ_Stage5_LDY, AbsIdx_READ_Stage5_NOP,
    AbsIdx_READ_Stage5_LAX, //unofficials
    AbsIdx_READ_Stage5_ERROR,
    //[absolute indexed RMW]
    AbsIdx_RMW_Stage5, AbsIdx_RMW_Stage7,
    AbsIdx_RMW_Stage6_ROR, AbsIdx_RMW_Stage6_DEC, AbsIdx_RMW_Stage6_INC, AbsIdx_RMW_Stage6_ASL, AbsIdx_RMW_Stage6_LSR, AbsIdx_RMW_Stage6_ROL,
    AbsIdx_RMW_Stage6_SLO, AbsIdx_RMW_Stage6_RLA, AbsIdx_RMW_Stage6_SRE, AbsIdx_RMW_Stage6_RRA, AbsIdx_RMW_Stage6_DCP, AbsIdx_RMW_Stage6_ISC, //unofficials

    IncS, DecS,
    PushPCL, PushPCH, PushP, PullP, PullPCL, PullPCH_NoInc, PushA, PullA_NoInc, PullP_NoInc,
    PushP_BRK, PushP_NMI, PushP_IRQ, PushP_Reset, PushDummy,
    FetchPCLVector, FetchPCHVector, //todo - may not need these ?? can reuse fetch2 and fetch3?

    //[implied] and [accumulator]
    Imp_ASL_A, Imp_ROL_A, Imp_ROR_A, Imp_LSR_A,
    Imp_SEC, Imp_CLI, Imp_SEI, Imp_CLD, Imp_CLC, Imp_CLV, Imp_SED,
    Imp_INY, Imp_DEY, Imp_INX, Imp_DEX,
    Imp_TSX, Imp_TXS, Imp_TAX, Imp_TAY, Imp_TYA, Imp_TXA,

    //[immediate]
    Imm_CMP, Imm_ADC, Imm_AND, Imm_SBC, Imm_ORA, Imm_EOR, Imm_CPY, Imm_CPX, Imm_ANC, Imm_ASR, Imm_ARR, Imm_LXA, Imm_AXS,
    Imm_LDA, Imm_LDX, Imm_LDY,
    Imm_Unsupported,

    //sub-ops
    NZ_X, NZ_Y, NZ_A,
    RelBranch_Stage2_BNE, RelBranch_Stage2_BPL, RelBranch_Stage2_BCC, RelBranch_Stage2_BCS, RelBranch_Stage2_BEQ, RelBranch_Stage2_BMI, RelBranch_Stage2_BVC, RelBranch_Stage2_BVS,
    RelBranch_Stage2, RelBranch_Stage3, RelBranch_Stage4,
    _Eor, _Bit, _Cpx, _Cpy, _Cmp, _Adc, _Sbc, _Ora, _And, _Anc, _Asr, _Arr, _Lxa, _Axs, //alu-related sub-ops

    //JMP (addr) 0x6C
    AbsInd_JMP_Stage4, AbsInd_JMP_Stage5,

    //[indirect indexed] (i.e. LDA (addr),Y	)
    IndIdx_Stage3, IndIdx_Stage4, IndIdx_READ_Stage5, IndIdx_WRITE_Stage5,
    IndIdx_WRITE_Stage6_STA, IndIdx_WRITE_Stage6_SHA,
    IndIdx_READ_Stage6_LDA, IndIdx_READ_Stage6_CMP, IndIdx_READ_Stage6_ORA, IndIdx_READ_Stage6_SBC, IndIdx_READ_Stage6_ADC, IndIdx_READ_Stage6_AND, IndIdx_READ_Stage6_EOR,
    IndIdx_READ_Stage6_LAX,
    IndIdx_RMW_Stage5,
    IndIdx_RMW_Stage6, //just reads from effective address
    IndIdx_RMW_Stage7_SLO, IndIdx_RMW_Stage7_RLA, IndIdx_RMW_Stage7_SRE, IndIdx_RMW_Stage7_RRA, IndIdx_RMW_Stage7_ISC, IndIdx_RMW_Stage7_DCP, //unofficials
    IndIdx_RMW_Stage8,

    End,
    End_ISpecial, //same as end, but preserves the iflag set by the instruction
    End_SuppressInterrupt,

    Jam,

    // More unofficial micro-ops
    Imm_ANE,
    AbsIdx_WRITE_Stage5_SHA,
    IndIdx_WRITE_Stage5_SHA,
    AbsIdx_Stage4_SHX,
    AbsIdx_Stage4_SHY,
    AbsIdx_Stage4_SHA,
    AbsIdx_Stage4_SHS,
  };

  static constexpr Uop MC_0[] = { Uop::Fetch2, Uop::PushPCH, Uop::PushPCL, Uop::PushP_BRK, Uop::FetchPCLVector, Uop::FetchPCHVector, Uop::End_SuppressInterrupt }; /*BRK [implied]*/
  static constexpr Uop MC_1[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_READ_ORA, Uop::End }; /*ORA (addr,X) [indexed indirect READ]*/
  static constexpr Uop MC_2[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_3[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_RMW, Uop::IdxInd_Stage7_RMW_SLO, Uop::IdxInd_Stage8_RMW, Uop::End }; /*SLO* (addr,X) [indexed indirect RMW] [unofficial]*/
  static constexpr Uop MC_4[] = { Uop::Fetch2, Uop::ZP_READ_NOP, Uop::End }; /*NOP zp [zero page READ]*/
  static constexpr Uop MC_5[] = { Uop::Fetch2, Uop::ZP_READ_ORA, Uop::End }; /*ORA zp [zero page READ]*/
  static constexpr Uop MC_6[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_ASL, Uop::ZP_RMW_Stage5, Uop::End }; /*ASL zp [zero page RMW]*/
  static constexpr Uop MC_7[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_SLO, Uop::ZP_RMW_Stage5, Uop::End }; /*SLO* zp [zero page RMW] [unofficial]*/
  static constexpr Uop MC_8[] = { Uop::FetchDummy, Uop::PushP, Uop::End }; /*PHP [implied]*/
  static constexpr Uop MC_9[] = { Uop::Imm_ORA, Uop::End }; /*ORA #nn [immediate]*/
  static constexpr Uop MC_10[] = { Uop::Imp_ASL_A, Uop::End }; /*ASL A [accumulator]*/
  static constexpr Uop MC_11[] = { Uop::Imm_ANC, Uop::End }; /*ANC** [immediate] [unofficial]*/
  static constexpr Uop MC_12[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_NOP, Uop::End }; /*NOP addr [absolute READ]*/
  static constexpr Uop MC_13[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_ORA, Uop::End }; /*ORA addr [absolute READ]*/
  static constexpr Uop MC_14[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_ASL, Uop::Abs_RMW_Stage6, Uop::End }; /*ASL addr [absolute RMW]*/
  static constexpr Uop MC_15[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_SLO, Uop::Abs_RMW_Stage6, Uop::End }; /*SLO* addr [absolute RMW] [unofficial]*/
  static constexpr Uop MC_16[] = { Uop::RelBranch_Stage2_BPL, Uop::End }; /*BPL +/-rel*/
  static constexpr Uop MC_17[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_READ_Stage5, Uop::IndIdx_READ_Stage6_ORA, Uop::End }; /*ORA (addr),Y* [indirect indexed READ]*/
  static constexpr Uop MC_18[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_19[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_RMW_Stage5, Uop::IndIdx_RMW_Stage6, Uop::IndIdx_RMW_Stage7_SLO, Uop::IndIdx_RMW_Stage8, Uop::End }; /*SLO (addr),Y* [indirect indexed RMW] [unofficial] */
  static constexpr Uop MC_20[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_NOP, Uop::End }; /*NOP zp,X [zero page indexed READ]*/
  static constexpr Uop MC_21[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_ORA, Uop::End }; /*ORA zp,X [zero page indexed READ]*/
  static constexpr Uop MC_22[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_ASL, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*ASL zp,X [zero page indexed RMW]*/
  static constexpr Uop MC_23[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_SLO, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*SLO* zp,X [zero page indexed RMW] [unofficial]*/
  static constexpr Uop MC_24[] = { Uop::Imp_CLC, Uop::End }; /*CLC [implied]*/
  static constexpr Uop MC_25[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_ORA, Uop::End }; /*ORA addr,Y* [absolute indexed READ Y]*/
  static constexpr Uop MC_26[] = { Uop::FetchDummy, Uop::End }; /*NOP 1A*/
  static constexpr Uop MC_27[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_SLO, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*SLO* addr,Y [absolute indexed RMW Y] [unofficial]*/
  static constexpr Uop MC_28[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_NOP, Uop::End }; /*NOP addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_29[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_ORA, Uop::End }; /*ORA addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_30[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_ASL, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*ASL addr,X [absolute indexed RMW X]*/
  static constexpr Uop MC_31[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_SLO, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*SLO* addr,X [absolute indexed RMW X] [unofficial]*/
  static constexpr Uop MC_32[] = { Uop::Fetch2, Uop::NOP, Uop::PushPCH, Uop::PushPCL, Uop::JSR, Uop::End }; /*JSR*/
  static constexpr Uop MC_33[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_READ_AND, Uop::End }; /*AND (addr,X) [indexed indirect READ]*/
  static constexpr Uop MC_34[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_35[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_RMW, Uop::IdxInd_Stage7_RMW_RLA, Uop::IdxInd_Stage8_RMW, Uop::End }; /*RLA* (addr,X) [indexed indirect RMW] [unofficial]*/
  static constexpr Uop MC_36[] = { Uop::Fetch2, Uop::ZP_READ_BIT, Uop::End }; /*BIT zp [zero page READ]*/
  static constexpr Uop MC_37[] = { Uop::Fetch2, Uop::ZP_READ_AND, Uop::End }; /*AND zp [zero page READ]*/
  static constexpr Uop MC_38[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_ROL, Uop::ZP_RMW_Stage5, Uop::End }; /*ROL zp [zero page RMW]*/
  static constexpr Uop MC_39[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_RLA, Uop::ZP_RMW_Stage5, Uop::End }; /*RLA* zp [zero page RMW] [unofficial]*/
  static constexpr Uop MC_40[] = { Uop::FetchDummy,  Uop::IncS, Uop::PullP_NoInc, Uop::End_ISpecial }; /*PLP [implied] */
  static constexpr Uop MC_41[] = { Uop::Imm_AND, Uop::End }; /*AND #nn [immediate]*/
  static constexpr Uop MC_42[] = { Uop::Imp_ROL_A, Uop::End }; /*ROL A [accumulator]*/
  static constexpr Uop MC_43[] = { Uop::Imm_ANC, Uop::End }; /*ANC** [immediate] [unofficial]*/
  static constexpr Uop MC_44[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_BIT, Uop::End }; /*BIT addr [absolute]*/
  static constexpr Uop MC_45[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_AND, Uop::End }; /*AND addr [absolute READ]*/
  static constexpr Uop MC_46[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_ROL, Uop::Abs_RMW_Stage6, Uop::End }; /*ROL addr [absolute RMW]*/
  static constexpr Uop MC_47[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_RLA, Uop::Abs_RMW_Stage6, Uop::End }; /*RLA* addr [absolute RMW] [unofficial]*/
  static constexpr Uop MC_48[] = { Uop::RelBranch_Stage2_BMI, Uop::End }; /*BMI +/-rel [relative]*/
  static constexpr Uop MC_49[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_READ_Stage5, Uop::IndIdx_READ_Stage6_AND, Uop::End }; /*AND (addr),Y* [indirect indexed READ]*/
  static constexpr Uop MC_50[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_51[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_RMW_Stage5, Uop::IndIdx_RMW_Stage6, Uop::IndIdx_RMW_Stage7_RLA, Uop::IndIdx_RMW_Stage8, Uop::End }; /*RLA* (addr),Y* [indirect indexed RMW] [unofficial] */
  static constexpr Uop MC_52[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_NOP, Uop::End }; /*NOP zp,X [zero page indexed READ]*/
  static constexpr Uop MC_53[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_AND, Uop::End }; /*AND zp,X [zero page indexed READ]*/
  static constexpr Uop MC_54[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_ROL, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*ROL zp,X [zero page indexed RMW]*/
  static constexpr Uop MC_55[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_RLA, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*RLA* zp,X [zero page indexed RMW] [unofficial]*/
  static constexpr Uop MC_56[] = { Uop::Imp_SEC, Uop::End }; /*SEC [implied]*/
  static constexpr Uop MC_57[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_AND, Uop::End }; /*AND addr,Y* [absolute indexed READ Y]*/
  static constexpr Uop MC_58[] = { Uop::FetchDummy, Uop::End }; /*NOP 3A [implied]*/
  static constexpr Uop MC_59[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_RLA, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*RLA* addr,Y [absolute indexed RMW Y] [unofficial]*/
  static constexpr Uop MC_60[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_NOP, Uop::End }; /*NOP addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_61[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_AND, Uop::End }; /*AND addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_62[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_ROL, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*ROL addr,X [absolute indexed RMW X]*/
  static constexpr Uop MC_63[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_RLA, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*RLA* addr,X [absolute indexed RMW X] [unofficial]*/
  static constexpr Uop MC_64[] = { Uop::FetchDummy, Uop::IncS, Uop::PullP, Uop::PullPCL, Uop::PullPCH_NoInc, Uop::End }; /*RTI*/
  static constexpr Uop MC_65[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_READ_EOR, Uop::End }; /*EOR (addr,X) [indexed indirect READ]*/
  static constexpr Uop MC_66[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_67[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_RMW, Uop::IdxInd_Stage7_RMW_SRE, Uop::IdxInd_Stage8_RMW, Uop::End }; /*SRE* (addr,X) [indexed indirect RMW] [unofficial]*/
  static constexpr Uop MC_68[] = { Uop::Fetch2, Uop::ZP_READ_NOP, Uop::End }; /*NOP zp [zero page READ]*/
  static constexpr Uop MC_69[] = { Uop::Fetch2, Uop::ZP_READ_EOR, Uop::End }; /*EOR zp [zero page READ]*/
  static constexpr Uop MC_70[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_LSR, Uop::ZP_RMW_Stage5, Uop::End }; /*LSR zp [zero page RMW]*/
  static constexpr Uop MC_71[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_SRE, Uop::ZP_RMW_Stage5, Uop::End }; /*SRE* zp [zero page RMW] [unofficial]*/
  static constexpr Uop MC_72[] = { Uop::FetchDummy, Uop::PushA, Uop::End }; /*PHA [implied]*/
  static constexpr Uop MC_73[] = { Uop::Imm_EOR, Uop::End }; /*EOR #nn [immediate]*/
  static constexpr Uop MC_74[] = { Uop::Imp_LSR_A, Uop::End }; /*LSR A [accumulator]*/
  static constexpr Uop MC_75[] = { Uop::Imm_ASR, Uop::End }; /*ASR** [immediate] [unofficial]*/
  static constexpr Uop MC_76[] = { Uop::Fetch2, Uop::JMP_abs, Uop::End }; /*JMP addr [absolute]*/
  static constexpr Uop MC_77[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_EOR, Uop::End }; /*EOR addr [absolute READ]*/
  static constexpr Uop MC_78[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_LSR, Uop::Abs_RMW_Stage6, Uop::End }; /*LSR addr [absolute RMW]*/
  static constexpr Uop MC_79[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_SRE, Uop::Abs_RMW_Stage6, Uop::End }; /*SRE* addr [absolute RMW] [unofficial]*/
  static constexpr Uop MC_80[] = { Uop::RelBranch_Stage2_BVC, Uop::End }; /*BVC +/-rel [relative]*/
  static constexpr Uop MC_81[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_READ_Stage5, Uop::IndIdx_READ_Stage6_EOR, Uop::End }; /*EOR (addr),Y* [indirect indexed READ]*/
  static constexpr Uop MC_82[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_83[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_RMW_Stage5, Uop::IndIdx_RMW_Stage6, Uop::IndIdx_RMW_Stage7_SRE, Uop::IndIdx_RMW_Stage8, Uop::End }; /*SRE* (addr),Y* [indirect indexed RMW] [unofficial] */
  static constexpr Uop MC_84[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_NOP, Uop::End }; /*NOP zp,X [zero page indexed READ]*/
  static constexpr Uop MC_85[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_EOR, Uop::End }; /*EOR zp,X [zero page indexed READ]*/
  static constexpr Uop MC_86[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_LSR, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*LSR zp,X [zero page indexed RMW]*/
  static constexpr Uop MC_87[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_SRE, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*SRE* zp,X [zero page indexed RMW] [unofficial]*/
  static constexpr Uop MC_88[] = { Uop::Imp_CLI, Uop::End_ISpecial }; /*CLI [implied]*/
  static constexpr Uop MC_89[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_EOR, Uop::End }; /*EOR addr,Y* [absolute indexed READ Y]*/
  static constexpr Uop MC_90[] = { Uop::FetchDummy, Uop::End }; /*NOP 5A [implied]*/
  static constexpr Uop MC_91[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_SRE, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*SRE* addr,Y [absolute indexed RMW Y] [unofficial]*/
  static constexpr Uop MC_92[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_NOP, Uop::End }; /*NOP addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_93[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_EOR, Uop::End }; /*EOR addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_94[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_LSR, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*LSR addr,X [absolute indexed RMW X]*/
  static constexpr Uop MC_95[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_SRE, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*SRE* addr,X [absolute indexed RMW X] [unofficial]*/
  static constexpr Uop MC_96[] = { Uop::FetchDummy, Uop::IncS, Uop::PullPCL, Uop::PullPCH_NoInc, Uop::IncPC, Uop::End }; /*RTS*/
  static constexpr Uop MC_97[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_READ_ADC, Uop::End }; /*ADC (addr,X) [indexed indirect READ]*/
  static constexpr Uop MC_98[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_99[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_RMW, Uop::IdxInd_Stage7_RMW_RRA, Uop::IdxInd_Stage8_RMW, Uop::End }; /*RRA* (addr,X) [indexed indirect RMW] [unofficial]*/
  static constexpr Uop MC_100[] = { Uop::Fetch2, Uop::ZP_READ_NOP, Uop::End }; /*NOP zp [zero page READ]*/
  static constexpr Uop MC_101[] = { Uop::Fetch2, Uop::ZP_READ_ADC, Uop::End }; /*ADC zp [zero page READ]*/
  static constexpr Uop MC_102[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_ROR, Uop::ZP_RMW_Stage5, Uop::End }; /*ROR zp [zero page RMW]*/
  static constexpr Uop MC_103[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_RRA, Uop::ZP_RMW_Stage5, Uop::End }; /*RRA* zp [zero page RMW] [unofficial]*/
  static constexpr Uop MC_104[] = { Uop::FetchDummy, Uop::IncS, Uop::PullA_NoInc, Uop::End }; /*PLA [implied]*/
  static constexpr Uop MC_105[] = { Uop::Imm_ADC, Uop::End }; /*ADC #nn [immediate]*/
  static constexpr Uop MC_106[] = { Uop::Imp_ROR_A, Uop::End }; /*ROR A [accumulator]*/
  static constexpr Uop MC_107[] = { Uop::Imm_ARR, Uop::End }; /*ARR** [immediate] [unofficial]*/
  static constexpr Uop MC_108[] = { Uop::Fetch2, Uop::Fetch3, Uop::AbsInd_JMP_Stage4, Uop::AbsInd_JMP_Stage5, Uop::End }; /*JMP (addr) [absolute indirect JMP]*/
  static constexpr Uop MC_109[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_ADC, Uop::End }; /*ADC addr [absolute READ]*/
  static constexpr Uop MC_110[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_ROR, Uop::Abs_RMW_Stage6, Uop::End }; /*ROR addr [absolute RMW]*/
  static constexpr Uop MC_111[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_RRA, Uop::Abs_RMW_Stage6, Uop::End }; /*RRA* addr [absolute RMW] [unofficial]*/
  static constexpr Uop MC_112[] = { Uop::RelBranch_Stage2_BVS, Uop::End }; /*BVS +/-rel [relative]*/
  static constexpr Uop MC_113[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_READ_Stage5, Uop::IndIdx_READ_Stage6_ADC, Uop::End }; /*ADC (addr),Y [indirect indexed READ]*/
  static constexpr Uop MC_114[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_115[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_RMW_Stage5, Uop::IndIdx_RMW_Stage6, Uop::IndIdx_RMW_Stage7_RRA, Uop::IndIdx_RMW_Stage8, Uop::End }; /*RRA* (addr),Y [indirect indexed RMW Y] [unofficial] */
  static constexpr Uop MC_116[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_NOP, Uop::End }; /*NOP zp,X [zero page indexed READ]*/
  static constexpr Uop MC_117[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_ADC, Uop::End }; /*ADC zp,X [zero page indexed READ]*/
  static constexpr Uop MC_118[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_ROR, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*ROR zp,X [zero page indexed RMW]*/
  static constexpr Uop MC_119[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_RRA, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*RRA* zp,X [zero page indexed RMW] [unofficial]*/
  static constexpr Uop MC_120[] = { Uop::Imp_SEI, Uop::End_ISpecial }; /*SEI [implied]*/
  static constexpr Uop MC_121[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_ADC, Uop::End }; /*ADC addr,Y* [absolute indexed READ Y]*/
  static constexpr Uop MC_122[] = { Uop::FetchDummy, Uop::End }; /*NOP 7A [implied]*/
  static constexpr Uop MC_123[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_RRA, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*RRA* addr,Y [absolute indexed RMW Y] [unofficial]*/
  static constexpr Uop MC_124[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_NOP, Uop::End }; /*NOP addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_125[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_ADC, Uop::End }; /*ADC addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_126[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_ROR, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*ROR addr,X [absolute indexed RMW X]*/
  static constexpr Uop MC_127[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_RRA, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*RRA* addr,X [absolute indexed RMW X] [unofficial]*/
  static constexpr Uop MC_128[] = { Uop::Imm_Unsupported, Uop::End }; /*NOP #nn [immediate]*/
  static constexpr Uop MC_129[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_WRITE_STA, Uop::End }; /*STA (addr,X) [indexed indirect WRITE]*/
  static constexpr Uop MC_130[] = { Uop::Imm_Unsupported, Uop::End }; /*NOP #nn [immediate]*/
  static constexpr Uop MC_131[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_WRITE_SAX, Uop::End }; /*SAX* (addr,X) [indexed indirect WRITE] [unofficial]*/
  static constexpr Uop MC_132[] = { Uop::Fetch2, Uop::ZP_WRITE_STY, Uop::End }; /*STY zp [zero page WRITE]*/
  static constexpr Uop MC_133[] = { Uop::Fetch2, Uop::ZP_WRITE_STA, Uop::End }; /*STA zp [zero page WRITE]*/
  static constexpr Uop MC_134[] = { Uop::Fetch2, Uop::ZP_WRITE_STX, Uop::End }; /*STX zp [zero page WRITE]*/
  static constexpr Uop MC_135[] = { Uop::Fetch2, Uop::ZP_WRITE_SAX, Uop::End }; /*SAX* zp [zero page WRITE] [unofficial]*/
  static constexpr Uop MC_136[] = { Uop::Imp_DEY, Uop::End }; /*DEY [implied]*/
  static constexpr Uop MC_137[] = { Uop::Imm_Unsupported, Uop::End }; /*NOP #nn [immediate]*/
  static constexpr Uop MC_138[] = { Uop::Imp_TXA, Uop::End }; /*TXA [implied]*/
  static constexpr Uop MC_139[] = { Uop::Imm_ANE, Uop::End }; /*ANE** [immediate] [unofficial]*/
  static constexpr Uop MC_140[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_WRITE_STY, Uop::End }; /*STY addr [absolute WRITE]*/
  static constexpr Uop MC_141[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_WRITE_STA, Uop::End }; /*STA addr [absolute WRITE]*/
  static constexpr Uop MC_142[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_WRITE_STX, Uop::End }; /*STX addr [absolute WRITE]*/
  static constexpr Uop MC_143[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_WRITE_SAX, Uop::End }; /*SAX* addr [absolute WRITE] [unofficial]*/
  static constexpr Uop MC_144[] = { Uop::RelBranch_Stage2_BCC, Uop::End }; /*BCC +/-rel [relative]*/
  static constexpr Uop MC_145[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_WRITE_Stage5, Uop::IndIdx_WRITE_Stage6_STA, Uop::End }; /*STA (addr),Y [indirect indexed WRITE]*/
  static constexpr Uop MC_146[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_147[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_WRITE_Stage5_SHA, Uop::IndIdx_WRITE_Stage6_SHA, Uop::End }; /*SHA** [indirect indexed WRITE] [unofficial] [not tested by blargg's instruction tests]*/
  static constexpr Uop MC_148[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_WRITE_STY, Uop::End }; /*STY zp,X [zero page indexed WRITE X]*/
  static constexpr Uop MC_149[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_WRITE_STA, Uop::End }; /*STA zp,X [zero page indexed WRITE X]*/
  static constexpr Uop MC_150[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_Y, Uop::ZP_WRITE_STX, Uop::End }; /*STX zp,Y [zero page indexed WRITE Y]*/
  static constexpr Uop MC_151[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_Y, Uop::ZP_WRITE_SAX, Uop::End }; /*SAX* zp,Y [zero page indexed WRITE Y] [unofficial]*/
  static constexpr Uop MC_152[] = { Uop::Imp_TYA, Uop::End }; /*TYA [implied]*/
  static constexpr Uop MC_153[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_Stage4, Uop::AbsIdx_WRITE_Stage5_STA, Uop::End }; /*STA addr,Y [absolute indexed WRITE]*/
  static constexpr Uop MC_154[] = { Uop::Imp_TXS, Uop::End }; /*TXS [implied]*/
  static constexpr Uop MC_155[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_Stage4_SHS, Uop::AbsIdx_WRITE_Stage5_SHS, Uop::End }; /*SHS* addr,Y [absolute indexed WRITE Y] [unofficial] */
  static constexpr Uop MC_156[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_Stage4_SHY, Uop::AbsIdx_WRITE_Stage5_SHY, Uop::End }; /*SHY** [absolute indexed WRITE] [unofficial]*/
  static constexpr Uop MC_157[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_Stage4, Uop::AbsIdx_WRITE_Stage5_STA, Uop::End }; /*STA addr,X [absolute indexed WRITE]*/
  static constexpr Uop MC_158[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_Stage4_SHX, Uop::AbsIdx_WRITE_Stage5_SHX, Uop::End }; /*SHX* addr,Y [absolute indexed WRITE Y] [unofficial]*/
  static constexpr Uop MC_159[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_Stage4_SHA, Uop::AbsIdx_WRITE_Stage5_SHA, Uop::End }; /*SHA* addr,Y [absolute indexed WRITE Y] [unofficial]*/
  static constexpr Uop MC_160[] = { Uop::Imm_LDY, Uop::End }; /*LDY #nn [immediate]*/
  static constexpr Uop MC_161[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_READ_LDA, Uop::End }; /*LDA (addr,X) [indexed indirect READ]*/
  static constexpr Uop MC_162[] = { Uop::Imm_LDX, Uop::End }; /*LDX #nn [immediate]*/
  static constexpr Uop MC_163[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_READ_LAX, Uop::End }; /*LAX* (addr,X) [indexed indirect READ] [unofficial]*/
  static constexpr Uop MC_164[] = { Uop::Fetch2, Uop::ZP_READ_LDY, Uop::End }; /*LDY zp [zero page READ]*/
  static constexpr Uop MC_165[] = { Uop::Fetch2, Uop::ZP_READ_LDA, Uop::End }; /*LDA zp [zero page READ]*/
  static constexpr Uop MC_166[] = { Uop::Fetch2, Uop::ZP_READ_LDX, Uop::End }; /*LDX zp [zero page READ]*/
  static constexpr Uop MC_167[] = { Uop::Fetch2, Uop::ZP_READ_LAX, Uop::End }; /*LAX* zp [zero page READ] [unofficial]*/
  static constexpr Uop MC_168[] = { Uop::Imp_TAY, Uop::End }; /*TAY [implied]*/
  static constexpr Uop MC_169[] = { Uop::Imm_LDA, Uop::End }; /*LDA #nn [immediate]*/
  static constexpr Uop MC_170[] = { Uop::Imp_TAX, Uop::End }; /*TAX [implied]*/
  static constexpr Uop MC_171[] = { Uop::Imm_LXA, Uop::End }; /*LXA** (ATX) [immediate] [unofficial]*/
  static constexpr Uop MC_172[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_LDY, Uop::End }; /*LDY addr [absolute READ]*/
  static constexpr Uop MC_173[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_LDA, Uop::End }; /*LDA addr [absolute READ]*/
  static constexpr Uop MC_174[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_LDX, Uop::End }; /*LDX addr [absolute READ]*/
  static constexpr Uop MC_175[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_LAX, Uop::End }; /*LAX* addr [absolute READ] [unofficial]*/
  static constexpr Uop MC_176[] = { Uop::RelBranch_Stage2_BCS, Uop::End }; /*BCS +/-rel [relative]*/
  static constexpr Uop MC_177[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_READ_Stage5, Uop::IndIdx_READ_Stage6_LDA, Uop::End }; /*LDA (addr),Y* [indirect indexed READ]*/
  static constexpr Uop MC_178[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_179[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_READ_Stage5, Uop::IndIdx_READ_Stage6_LAX, Uop::End }; /*LAX* (addr),Y* [indirect indexed READ] [unofficial] */
  static constexpr Uop MC_180[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_LDY, Uop::End }; /*LDY zp,X [zero page indexed READ X]*/
  static constexpr Uop MC_181[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_LDA, Uop::End }; /*LDA zp,X [zero page indexed READ X]*/
  static constexpr Uop MC_182[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_Y, Uop::ZP_READ_LDX, Uop::End }; /*LDX zp,Y [zero page indexed READ Y]*/
  static constexpr Uop MC_183[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_Y, Uop::ZP_READ_LAX, Uop::End }; /*LAX* zp,Y [zero page indexed READ] [unofficial]*/
  static constexpr Uop MC_184[] = { Uop::Imp_CLV, Uop::End }; /*CLV [implied]*/
  static constexpr Uop MC_185[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_LDA, Uop::End }; /*LDA addr,Y* [absolute indexed READ Y]*/
  static constexpr Uop MC_186[] = { Uop::Imp_TSX, Uop::End }; /*TSX [implied]*/
  static constexpr Uop MC_187[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_ERROR, Uop::End }; /*LAS* addr,Y [absolute indexed READ Y] [unofficial]*/
  static constexpr Uop MC_188[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_LDY, Uop::End }; /*LDY addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_189[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_LDA, Uop::End }; /*LDA addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_190[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_LDX, Uop::End }; /*LDX addr,Y* [absolute indexed READ Y]*/
  static constexpr Uop MC_191[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_LAX, Uop::End }; /*LAX* addr,Y [absolute indexed READ Y] [unofficial]*/
  static constexpr Uop MC_192[] = { Uop::Imm_CPY, Uop::End }; /*CPY #nn [immediate]*/
  static constexpr Uop MC_193[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_READ_CMP, Uop::End }; /*CMP (addr,X) [indexed indirect READ]*/
  static constexpr Uop MC_194[] = { Uop::Imm_Unsupported, Uop::End }; /*NOP #nn [immediate]*/
  static constexpr Uop MC_195[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_RMW, Uop::IdxInd_Stage7_RMW_DCP, Uop::IdxInd_Stage8_RMW, Uop::End }; /*DCP* (addr,X) [indexed indirect RMW] [unofficial]*/
  static constexpr Uop MC_196[] = { Uop::Fetch2, Uop::ZP_READ_CPY, Uop::End }; /*CPY zp [zero page READ]*/
  static constexpr Uop MC_197[] = { Uop::Fetch2, Uop::ZP_READ_CMP, Uop::End }; /*CMP zp [zero page READ]*/
  static constexpr Uop MC_198[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_DEC, Uop::ZP_RMW_Stage5, Uop::End }; /*DEC zp [zero page RMW]*/
  static constexpr Uop MC_199[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_DCP, Uop::ZP_RMW_Stage5, Uop::End }; /*DCP* zp [zero page RMW] [unofficial]*/
  static constexpr Uop MC_200[] = { Uop::Imp_INY, Uop::End }; /*INY [implied]*/
  static constexpr Uop MC_201[] = { Uop::Imm_CMP, Uop::End }; /*CMP #nn [immediate]*/
  static constexpr Uop MC_202[] = { Uop::Imp_DEX, Uop::End }; /*DEX  [implied]*/
  static constexpr Uop MC_203[] = { Uop::Imm_AXS, Uop::End }; /*AXS** [immediate] [unofficial]*/
  static constexpr Uop MC_204[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_CPY, Uop::End }; /*CPY addr [absolute READ]*/
  static constexpr Uop MC_205[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_CMP, Uop::End }; /*CMP addr [absolute READ]*/
  static constexpr Uop MC_206[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_DEC, Uop::Abs_RMW_Stage6, Uop::End }; /*DEC addr [absolute RMW]*/
  static constexpr Uop MC_207[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_DCP, Uop::Abs_RMW_Stage6, Uop::End }; /*DCP* addr [absolute RMW] [unofficial]*/
  static constexpr Uop MC_208[] = { Uop::RelBranch_Stage2_BNE, Uop::End }; /*BNE +/-rel [relative]*/
  static constexpr Uop MC_209[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_READ_Stage5, Uop::IndIdx_READ_Stage6_CMP, Uop::End }; /*CMP (addr),Y* [indirect indexed READ]*/
  static constexpr Uop MC_210[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_211[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_RMW_Stage5, Uop::IndIdx_RMW_Stage6, Uop::IndIdx_RMW_Stage7_DCP, Uop::IndIdx_RMW_Stage8, Uop::End }; /*DCP* (addr),Y* [indirect indexed RMW Y] [unofficial] */
  static constexpr Uop MC_212[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_NOP, Uop::End }; /*NOP zp,X [zero page indexed READ]*/
  static constexpr Uop MC_213[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_CMP, Uop::End }; /*CMP zp,X [zero page indexed READ]*/
  static constexpr Uop MC_214[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_DEC, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*DEC zp,X [zero page indexed RMW X]*/
  static constexpr Uop MC_215[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_DCP, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*DCP* zp,X [zero page indexed RMW] [unofficial]*/
  static constexpr Uop MC_216[] = { Uop::Imp_CLD, Uop::End }; /*CLD [implied]*/
  static constexpr Uop MC_217[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_CMP, Uop::End }; /*CMP addr,Y* [absolute indexed READ Y]*/
  static constexpr Uop MC_218[] = { Uop::FetchDummy, Uop::End }; /*NOP DA [implied]*/
  static constexpr Uop MC_219[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_DCP, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*DCP* addr,Y [absolute indexed RMW Y] [unofficial]*/
  static constexpr Uop MC_220[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_NOP, Uop::End }; /*NOP addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_221[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_CMP, Uop::End }; /*CMP addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_222[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_DEC, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*DEC addr,X [absolute indexed RMW X]*/
  static constexpr Uop MC_223[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_DCP, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*DCP* addr,X [absolute indexed RMW X] [unofficial]*/
  static constexpr Uop MC_224[] = { Uop::Imm_CPX, Uop::End }; /*CPX #nn [immediate]*/
  static constexpr Uop MC_225[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_READ_SBC, Uop::End }; /*SBC (addr,X) [indirect indexed]*/
  static constexpr Uop MC_226[] = { Uop::Imm_Unsupported, Uop::End }; /*NOP #nn [immediate]*/
  static constexpr Uop MC_227[] = { Uop::Fetch2, Uop::IdxInd_Stage3, Uop::IdxInd_Stage4, Uop::IdxInd_Stage5, Uop::IdxInd_Stage6_RMW, Uop::IdxInd_Stage7_RMW_ISC, Uop::IdxInd_Stage8_RMW, Uop::End }; /*ISC* (addr,X) [indexed indirect RMW] [unofficial]*/
  static constexpr Uop MC_228[] = { Uop::Fetch2, Uop::ZP_READ_CPX, Uop::End }; /*CPX zp [zero page READ]*/
  static constexpr Uop MC_229[] = { Uop::Fetch2, Uop::ZP_READ_SBC, Uop::End }; /*SBC zp [zero page READ]*/
  static constexpr Uop MC_230[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_INC, Uop::ZP_RMW_Stage5, Uop::End }; /*INC zp [zero page RMW]*/
  static constexpr Uop MC_231[] = { Uop::Fetch2, Uop::ZP_RMW_Stage3, Uop::ZP_RMW_ISC, Uop::ZP_RMW_Stage5, Uop::End }; /*ISB* zp [zero page RMW] [unofficial]*/
  static constexpr Uop MC_232[] = { Uop::Imp_INX, Uop::End }; /*INX [implied]*/
  static constexpr Uop MC_233[] = { Uop::Imm_SBC, Uop::End }; /*SBC #nn [immediate READ]*/
  static constexpr Uop MC_234[] = { Uop::FetchDummy, Uop::End }; /*NOP EA [implied]*/
  static constexpr Uop MC_235[] = { Uop::Imm_SBC, Uop::End }; /*ISB #nn [immediate READ]*/
  static constexpr Uop MC_236[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_CPX, Uop::End }; /*CPX addr [absolute READ]*/
  static constexpr Uop MC_237[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_READ_SBC, Uop::End }; /*SBC addr [absolute READ]*/
  static constexpr Uop MC_238[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_INC, Uop::Abs_RMW_Stage6, Uop::End }; /*INC addr [absolute RMW]*/
  static constexpr Uop MC_239[] = { Uop::Fetch2, Uop::Fetch3, Uop::Abs_RMW_Stage4, Uop::Abs_RMW_Stage5_ISC, Uop::Abs_RMW_Stage6, Uop::End }; /*ISC* addr [absolute RMW] [unofficial]*/
  static constexpr Uop MC_240[] = { Uop::RelBranch_Stage2_BEQ, Uop::End }; /*BEQ +/-rel [relative]*/
  static constexpr Uop MC_241[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_READ_Stage5, Uop::IndIdx_READ_Stage6_SBC, Uop::End }; /*SBC (addr),Y* [indirect indexed READ]*/
  static constexpr Uop MC_242[] = { Uop::Jam }; /*JAM*/
  static constexpr Uop MC_243[] = { Uop::Fetch2, Uop::IndIdx_Stage3, Uop::IndIdx_Stage4, Uop::IndIdx_RMW_Stage5, Uop::IndIdx_RMW_Stage6, Uop::IndIdx_RMW_Stage7_ISC, Uop::IndIdx_RMW_Stage8, Uop::End }; /*ISC* (addr),Y* [indirect indexed RMW Y] [unofficial] */
  static constexpr Uop MC_244[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_NOP, Uop::End }; /*NOP zp,X [zero page indexed READ]*/
  static constexpr Uop MC_245[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZP_READ_SBC, Uop::End }; /*SBC zp,X [zero page indexed READ X]*/
  static constexpr Uop MC_246[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_INC, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*INC zp,X [zero page indexed RMW X]*/
  static constexpr Uop MC_247[] = { Uop::Fetch2, Uop::ZpIdx_Stage3_X, Uop::ZpIdx_RMW_Stage4, Uop::ZP_RMW_ISC, Uop::ZpIdx_RMW_Stage6, Uop::End }; /*ISC* zp,X [zero page indexed RMW] [unofficial]*/
  static constexpr Uop MC_248[] = { Uop::Imp_SED, Uop::End }; /*SED [implied]*/
  static constexpr Uop MC_249[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_SBC, Uop::End }; /*SBC addr,Y* [absolute indexed READ Y]*/
  static constexpr Uop MC_250[] = { Uop::FetchDummy, Uop::End }; /*NOP FA [implied]*/
  static constexpr Uop MC_251[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_Y,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_ISC, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*ISC* addr,Y [absolute indexed RMW Y] [unofficial]*/
  static constexpr Uop MC_252[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_NOP, Uop::End }; /*NOP addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_253[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X, Uop::AbsIdx_READ_Stage4, Uop::AbsIdx_READ_Stage5_SBC, Uop::End }; /*SBC addr,X* [absolute indexed READ X]*/
  static constexpr Uop MC_254[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_INC, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*INC addr,X [absolute indexed RMW X]*/
  static constexpr Uop MC_255[] = { Uop::Fetch2, Uop::AbsIdx_Stage3_X,  Uop::AbsIdx_Stage4, Uop::AbsIdx_RMW_Stage5, Uop::AbsIdx_RMW_Stage6_ISC, Uop::AbsIdx_RMW_Stage7, Uop::End }; /*ISC* addr,X [absolute indexed RMW X] [unofficial]*/
  static constexpr Uop MC_256[] = { Uop::Fetch1 }; /*VOP_Fetch1*/
  static constexpr Uop MC_257[] = { Uop::RelBranch_Stage3, Uop::End }; /*VOP_RelativeStuff*/
  static constexpr Uop MC_258[] = { Uop::RelBranch_Stage4, Uop::End }; /*VOP_RelativeStuff2*/
  static constexpr Uop MC_259[] = { Uop::End_SuppressInterrupt }; /*VOP_RelativeStuff3*/
  static constexpr Uop MC_260[] = { Uop::FetchDummy, Uop::FetchDummy, Uop::PushPCH, Uop::PushPCL, Uop::PushP_NMI, Uop::FetchPCLVector, Uop::FetchPCHVector, Uop::End_SuppressInterrupt }; /*VOP_NMI*/
  static constexpr Uop MC_261[] = { Uop::FetchDummy, Uop::FetchDummy, Uop::PushPCH, Uop::PushPCL, Uop::PushP_IRQ, Uop::FetchPCLVector, Uop::FetchPCHVector, Uop::End_SuppressInterrupt }; /*VOP_IRQ*/
  static constexpr Uop MC_262[] = { Uop::FetchDummy, /*Uop::FetchDummy,*/ Uop::FetchDummy, Uop::PushDummy, Uop::PushDummy, Uop::PushP_Reset, Uop::FetchPCLVector, Uop::FetchPCHVector, Uop::End_SuppressInterrupt }; /*VOP_RESET*/
  static constexpr Uop MC_263[] = { Uop::Fetch1_Real }; /*VOP_Fetch1_NoInterrupt*/

  static constexpr const Uop* Microcode[] =
  {
    MC_0,
    MC_1,
    MC_2,
    MC_3,
    MC_4,
    MC_5,
    MC_6,
    MC_7,
    MC_8,
    MC_9,
    MC_10,
    MC_11,
    MC_12,
    MC_13,
    MC_14,
    MC_15,
    MC_16,
    MC_17,
    MC_18,
    MC_19,
    MC_20,
    MC_21,
    MC_22,
    MC_23,
    MC_24,
    MC_25,
    MC_26,
    MC_27,
    MC_28,
    MC_29,
    MC_30,
    MC_31,
    MC_32,
    MC_33,
    MC_34,
    MC_35,
    MC_36,
    MC_37,
    MC_38,
    MC_39,
    MC_40,
    MC_41,
    MC_42,
    MC_43,
    MC_44,
    MC_45,
    MC_46,
    MC_47,
    MC_48,
    MC_49,
    MC_50,
    MC_51,
    MC_52,
    MC_53,
    MC_54,
    MC_55,
    MC_56,
    MC_57,
    MC_58,
    MC_59,
    MC_60,
    MC_61,
    MC_62,
    MC_63,
    MC_64,
    MC_65,
    MC_66,
    MC_67,
    MC_68,
    MC_69,
    MC_70,
    MC_71,
    MC_72,
    MC_73,
    MC_74,
    MC_75,
    MC_76,
    MC_77,
    MC_78,
    MC_79,
    MC_80,
    MC_81,
    MC_82,
    MC_83,
    MC_84,
    MC_85,
    MC_86,
    MC_87,
    MC_88,
    MC_89,
    MC_90,
    MC_91,
    MC_92,
    MC_93,
    MC_94,
    MC_95,
    MC_96,
    MC_97,
    MC_98,
    MC_99,
    MC_100,
    MC_101,
    MC_102,
    MC_103,
    MC_104,
    MC_105,
    MC_106,
    MC_107,
    MC_108,
    MC_109,
    MC_110,
    MC_111,
    MC_112,
    MC_113,
    MC_114,
    MC_115,
    MC_116,
    MC_117,
    MC_118,
    MC_119,
    MC_120,
    MC_121,
    MC_122,
    MC_123,
    MC_124,
    MC_125,
    MC_126,
    MC_127,
    MC_128,
    MC_129,
    MC_130,
    MC_131,
    MC_132,
    MC_133,
    MC_134,
    MC_135,
    MC_136,
    MC_137,
    MC_138,
    MC_139,
    MC_140,
    MC_141,
    MC_142,
    MC_143,
    MC_144,
    MC_145,
    MC_146,
    MC_147,
    MC_148,
    MC_149,
    MC_150,
    MC_151,
    MC_152,
    MC_153,
    MC_154,
    MC_155,
    MC_156,
    MC_157,
    MC_158,
    MC_159,
    MC_160,
    MC_161,
    MC_162,
    MC_163,
    MC_164,
    MC_165,
    MC_166,
    MC_167,
    MC_168,
    MC_169,
    MC_170,
    MC_171,
    MC_172,
    MC_173,
    MC_174,
    MC_175,
    MC_176,
    MC_177,
    MC_178,
    MC_179,
    MC_180,
    MC_181,
    MC_182,
    MC_183,
    MC_184,
    MC_185,
    MC_186,
    MC_187,
    MC_188,
    MC_189,
    MC_190,
    MC_191,
    MC_192,
    MC_193,
    MC_194,
    MC_195,
    MC_196,
    MC_197,
    MC_198,
    MC_199,
    MC_200,
    MC_201,
    MC_202,
    MC_203,
    MC_204,
    MC_205,
    MC_206,
    MC_207,
    MC_208,
    MC_209,
    MC_210,
    MC_211,
    MC_212,
    MC_213,
    MC_214,
    MC_215,
    MC_216,
    MC_217,
    MC_218,
    MC_219,
    MC_220,
    MC_221,
    MC_222,
    MC_223,
    MC_224,
    MC_225,
    MC_226,
    MC_227,
    MC_228,
    MC_229,
    MC_230,
    MC_231,
    MC_232,
    MC_233,
    MC_234,
    MC_235,
    MC_236,
    MC_237,
    MC_238,
    MC_239,
    MC_240,
    MC_241,
    MC_242,
    MC_243,
    MC_244,
    MC_245,
    MC_246,
    MC_247,
    MC_248,
    MC_249,
    MC_250,
    MC_251,
    MC_252,
    MC_253,
    MC_254,
    MC_255,
    MC_256,
    MC_257,
    MC_258,
    MC_259,
    MC_260,
    MC_261,
    MC_262,
    MC_263,
  };

  static_assert(sizeof(Microcode) / sizeof(Microcode[0]) == 264, "microcode row count");

  // ---- constants (from MOS6502X.cs) ----
  static constexpr uint16_t NMIVector   = 0xFFFA;
  static constexpr uint16_t ResetVector = 0xFFFC;
  static constexpr uint16_t BRKVector   = 0xFFFE;
  static constexpr uint16_t IRQVector   = 0xFFFE;

  // N/Z flag pair per value (bit1 = Z for 0, bit7 = N); from MOS6502X.cs TableNZ
  static constexpr uint8_t TableNZ[256] = {
    0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
  };

  bool debug = false; // debug-print hook in C#; unused here

  // ---- registers & pins (from MOS6502X.cs) ----
  uint8_t A = 0, X = 0, Y = 0, P = 0x20, S = 0;
  uint16_t PC = 0;
  bool IRQ = false, NMI = false, RDY = true;
  int32_t ext_ppu_cycle = 0;
  uint8_t AneConstant = 0xFF; // ANE/XAA magic constant (hardware-variable; BizHawk default)
  uint8_t LxaConstant = 0xFF; // LXA magic constant
  int64_t TotalExecutedCycles = 0;
  bool BCD_Enabled = true;    // NES core sets false

  // flag accessors (P bit fields), mirroring the C# properties
  bool GetFlagC() const { return (P & 0x01) != 0; }  void SetFlagC(bool v) { P = v ? (P | 0x01) : (P & ~0x01); }
  bool GetFlagZ() const { return (P & 0x02) != 0; }  void SetFlagZ(bool v) { P = v ? (P | 0x02) : (P & ~0x02); }
  bool GetFlagI() const { return (P & 0x04) != 0; }  void SetFlagI(bool v) { P = v ? (P | 0x04) : (P & ~0x04); }
  bool GetFlagD() const { return (P & 0x08) != 0; }  void SetFlagD(bool v) { P = v ? (P | 0x08) : (P & ~0x08); }
  bool GetFlagB() const { return (P & 0x10) != 0; }  void SetFlagB(bool v) { P = v ? (P | 0x10) : (P & ~0x10); }
  bool GetFlagT() const { return (P & 0x20) != 0; }  void SetFlagT(bool v) { P = v ? (P | 0x20) : (P & ~0x20); }
  bool GetFlagV() const { return (P & 0x40) != 0; }  void SetFlagV(bool v) { P = v ? (P | 0x40) : (P & ~0x40); }
  bool GetFlagN() const { return (P & 0x80) != 0; }  void SetFlagN(bool v) { P = v ? (P | 0x80) : (P & ~0x80); }

  void Reset()
  {
    A = 0; X = 0; Y = 0; P = 0x20; S = 0; PC = 0;
    TotalExecutedCycles = 0;
    mi = 0;
    opcode = VOP_RESET;
    iflag_pending = true;
    RDY = true;
  }

  void NESSoftReset()
  {
    opcode = VOP_RESET;
    mi = 0;
    iflag_pending = true;
    SetFlagI(true);
  }

void InitOpcodeHandlers()
  {
    //delegates arent faster than the switch. pretty sure. don't use it.
    //opcodeHandlers = new Action[] {
    //  Unsupported,Fetch1, Fetch1_Real, Fetch2, Fetch3,FetchDummy,
    //  NOP,JSR,IncPC,
    //  Abs_WRITE_STA, Abs_WRITE_STX, Abs_WRITE_STY,Abs_WRITE_SAX,Abs_READ_BIT, Abs_READ_LDA, Abs_READ_LDY, Abs_READ_ORA, Abs_READ_LDX, Abs_READ_CMP, Abs_READ_ADC, Abs_READ_CPX, Abs_READ_SBC, Abs_READ_AND, Abs_READ_EOR, Abs_READ_CPY, Abs_READ_NOP,
    //  Abs_READ_LAX,Abs_RMW_Stage4, Abs_RMW_Stage6,Abs_RMW_Stage5_INC, Abs_RMW_Stage5_DEC, Abs_RMW_Stage5_LSR, Abs_RMW_Stage5_ROL, Abs_RMW_Stage5_ASL, Abs_RMW_Stage5_ROR,Abs_RMW_Stage5_SLO, Abs_RMW_Stage5_RLA, Abs_RMW_Stage5_SRE, Abs_RMW_Stage5_RRA, Abs_RMW_Stage5_DCP, Abs_RMW_Stage5_ISC,
    //  JMP_abs,ZpIdx_Stage3_X, ZpIdx_Stage3_Y,ZpIdx_RMW_Stage4, ZpIdx_RMW_Stage6,ZP_WRITE_STA, ZP_WRITE_STX, ZP_WRITE_STY, ZP_WRITE_SAX,ZP_RMW_Stage3, ZP_RMW_Stage5,
    //  ZP_RMW_DEC, ZP_RMW_INC, ZP_RMW_ASL, ZP_RMW_LSR, ZP_RMW_ROR, ZP_RMW_ROL,ZP_RMW_SLO, ZP_RMW_RLA, ZP_RMW_SRE, ZP_RMW_RRA, ZP_RMW_DCP, ZP_RMW_ISC,
    //  ZP_READ_EOR, ZP_READ_BIT, ZP_READ_ORA, ZP_READ_LDA, ZP_READ_LDY, ZP_READ_LDX, ZP_READ_CPX, ZP_READ_SBC, ZP_READ_CPY, ZP_READ_NOP, ZP_READ_ADC, ZP_READ_AND, ZP_READ_CMP, ZP_READ_LAX,
    //  IdxInd_Stage3, IdxInd_Stage4, IdxInd_Stage5,IdxInd_Stage6_READ_ORA, IdxInd_Stage6_READ_SBC, IdxInd_Stage6_READ_LDA, IdxInd_Stage6_READ_EOR, IdxInd_Stage6_READ_CMP, IdxInd_Stage6_READ_ADC, IdxInd_Stage6_READ_AND,
    //  IdxInd_Stage6_READ_LAX,IdxInd_Stage6_WRITE_STA, IdxInd_Stage6_WRITE_SAX,IdxInd_Stage6_RMW,IdxInd_Stage7_RMW_SLO, IdxInd_Stage7_RMW_RLA, IdxInd_Stage7_RMW_SRE, IdxInd_Stage7_RMW_RRA, IdxInd_Stage7_RMW_ISC, IdxInd_Stage7_RMW_DCP,
    //  IdxInd_Stage8_RMW,AbsIdx_Stage3_X, AbsIdx_Stage3_Y, AbsIdx_Stage4,AbsIdx_WRITE_Stage5_STA,AbsIdx_WRITE_Stage5_SHY, AbsIdx_WRITE_Stage5_SHX,AbsIdx_WRITE_Stage5_ERROR,AbsIdx_READ_Stage4,
    //  AbsIdx_READ_Stage5_LDA, AbsIdx_READ_Stage5_CMP, AbsIdx_READ_Stage5_SBC, AbsIdx_READ_Stage5_ADC, AbsIdx_READ_Stage5_EOR, AbsIdx_READ_Stage5_LDX, AbsIdx_READ_Stage5_AND, AbsIdx_READ_Stage5_ORA, AbsIdx_READ_Stage5_LDY, AbsIdx_READ_Stage5_NOP,
    //  AbsIdx_READ_Stage5_LAX,AbsIdx_READ_Stage5_ERROR,AbsIdx_RMW_Stage5, AbsIdx_RMW_Stage7,AbsIdx_RMW_Stage6_ROR, AbsIdx_RMW_Stage6_DEC, AbsIdx_RMW_Stage6_INC, AbsIdx_RMW_Stage6_ASL, AbsIdx_RMW_Stage6_LSR, AbsIdx_RMW_Stage6_ROL,
    //  AbsIdx_RMW_Stage6_SLO, AbsIdx_RMW_Stage6_RLA, AbsIdx_RMW_Stage6_SRE, AbsIdx_RMW_Stage6_RRA, AbsIdx_RMW_Stage6_DCP, AbsIdx_RMW_Stage6_ISC,IncS, DecS,
    //  PushPCL, PushPCH, PushP, PullP, PullPCL, PullPCH_NoInc, PushA, PullA_NoInc, PullP_NoInc,PushP_BRK, PushP_NMI, PushP_IRQ, PushP_Reset, PushDummy,FetchPCLVector, FetchPCHVector,
    //  Imp_ASL_A, Imp_ROL_A, Imp_ROR_A, Imp_LSR_A,Imp_SEC, Imp_CLI, Imp_SEI, Imp_CLD, Imp_CLC, Imp_CLV, Imp_SED,Imp_INY, Imp_DEY, Imp_INX, Imp_DEX,Imp_TSX, Imp_TXS, Imp_TAX, Imp_TAY, Imp_TYA, Imp_TXA,
    //  Imm_CMP, Imm_ADC, Imm_AND, Imm_SBC, Imm_ORA, Imm_EOR, Imm_CPY, Imm_CPX, Imm_ANC, Imm_ASR, Imm_ARR, Imm_LXA, Imm_AXS,Imm_LDA, Imm_LDX, Imm_LDY,
    //  Imm_Unsupported,NZ_X, NZ_Y, NZ_A,RelBranch_Stage2_BNE, RelBranch_Stage2_BPL, RelBranch_Stage2_BCC, RelBranch_Stage2_BCS, RelBranch_Stage2_BEQ, RelBranch_Stage2_BMI, RelBranch_Stage2_BVC, RelBranch_Stage2_BVS,
    //  RelBranch_Stage2, RelBranch_Stage3, RelBranch_Stage4,_Eor, _Bit, _Cpx, _Cpy, _Cmp, _Adc, _Sbc, _Ora, _And, _Anc, _Asr, _Arr, _Lxa, _Axs,
    //  AbsInd_JMP_Stage4, AbsInd_JMP_Stage5,IndIdx_Stage3, IndIdx_Stage4, IndIdx_READ_Stage5, IndIdx_WRITE_Stage5,
    //  IndIdx_WRITE_Stage6_STA, IndIdx_WRITE_Stage6_SHA,IndIdx_READ_Stage6_LDA, IndIdx_READ_Stage6_CMP, IndIdx_READ_Stage6_ORA, IndIdx_READ_Stage6_SBC, IndIdx_READ_Stage6_ADC, IndIdx_READ_Stage6_AND, IndIdx_READ_Stage6_EOR,
    //  IndIdx_READ_Stage6_LAX,IndIdx_RMW_Stage5,IndIdx_RMW_Stage6, IndIdx_RMW_Stage7_SLO, IndIdx_RMW_Stage7_RLA, IndIdx_RMW_Stage7_SRE, IndIdx_RMW_Stage7_RRA, IndIdx_RMW_Stage7_ISC, IndIdx_RMW_Stage7_DCP,IndIdx_RMW_Stage8,
    //  End,End_ISpecial,End_SuppressInterrupt,
    //};
  }

  static constexpr int VOP_Fetch1 = 256;
  static constexpr int VOP_RelativeStuff = 257;
  static constexpr int VOP_RelativeStuff2 = 258;
  static constexpr int VOP_RelativeStuff3 = 259;
  static constexpr int VOP_NMI = 260;
  static constexpr int VOP_IRQ = 261;
  static constexpr int VOP_RESET = 262;
  static constexpr int VOP_Fetch1_NoInterrupt = 263;
  static constexpr int VOP_NUM = 264;

  //opcode bytes.. theoretically redundant with the temp variables? who knows.
  int32_t opcode;
  uint8_t opcode2, opcode3;

  int32_t ea, alu_temp; //cpu internal temp variables
  int32_t mi; //microcode index
  bool iflag_pending; //iflag must be stored after it is checked in some cases (CLI and SEI).
  bool rdy_freeze; //true if the CPU must be frozen
  uint8_t H; //internal temp variable used in the "unstable high uint8_t group" of unofficial instructions
  uint16_t address_bus; // The 16 bit address bus.

  //tracks whether an interrupt condition has popped up recently.
  //not sure if this is real or not but it helps with the branch_irq_hack
  bool interrupt_pending;
  bool branch_irq_hack; //see Uop::RelBranch_Stage3 for more details

#ifdef _NESHAWK_DETECT_BAD_ACCESS
  // Per-frame bad-access flag: set when Fetch1_Real fetches an unofficial opcode or an opcode from
  // RAM ($0000-$7FFF) -- i.e. control derailed into data-as-code (the U+X glitch signature). Reset
  // each frame by the NES loop, exposed to jaffar so run-3 can win on a REAL ground-truth derail
  // (a normal off-path room transition leaves this 0). See cpu_isOfficialOpcode below.
  uint8_t badAccessLatch = 0;
#endif

  bool Interrupted() { return NMI || (IRQ && !GetFlagI()); }

  void FetchDummy()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC);
    }
  }

  void Execute(int cycles)
  {
    for (int i = 0; i < cycles; i++)
    {
      ExecuteOne();
    }
  }

  uint8_t value8, temp8;
  uint16_t value16;
  bool branch_taken = false;
  bool my_iflag;
  bool booltemp;
  int32_t tempint;
  int32_t lo, hi;
  // [not translated] public Action<TraceInfo> TraceCallback;

  void Fetch1()
  {
    my_iflag = GetFlagI();
    SetFlagI(iflag_pending);
    if (!branch_irq_hack)
    {
      interrupt_pending = false;
      if (NMI)
      {
        // [not translated] TraceCallback?.Invoke(new(disassembly: "====NMI====", registerInfo: string.Empty));
        ea = NMIVector;
        opcode = VOP_NMI;
        NMI = false;
        mi = 0;
        ExecuteOneRetry();
        return;
      }

      if (IRQ && !my_iflag)
      {
        // [not translated] TraceCallback?.Invoke(new(disassembly: "====IRQ====", registerInfo: string.Empty));
        ea = IRQVector;
        opcode = VOP_IRQ;
        mi = 0;
        ExecuteOneRetry();
        return;
      }
    }

    mi = 0;
    opcode = VOP_Fetch1_NoInterrupt;

    Fetch1_Real();
  }

  void Fetch1_Real()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      // [not translated] if (debug) Console.WriteLine(State());
      branch_irq_hack = false;
      _link.OnExecFetch(PC);
      // [not translated] TraceCallback?.Invoke(State());
      opcode = _link.ReadMemory(PC++);
#ifdef _NESHAWK_DETECT_BAD_ACCESS
      // bad access = fetched an unofficial opcode, or fetched code from RAM/regs ($0000-$7FFF).
      // Fetch address is PC-1 (just post-incremented). opcode is a real byte here (0-255).
      if (badAccessLatch == 0 && ((uint16_t)(PC - 1) < 0x8000u || cpu_isOfficialOpcode[opcode & 0xFF] == 0)) badAccessLatch = 1;
#endif
      mi = -1;
    }
    address_bus = PC;
  }

  void Fetch2()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      opcode2 = _link.ReadMemory(PC++);
    }
  }

  void Fetch3()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      opcode3 = _link.ReadMemory(PC++);
      address_bus = (uint16_t) ((opcode3 << 8) | opcode2);
    }
  }

  void PushPCH()
  {
    _link.WriteMemory((uint16_t)(S-- + 0x100), (uint8_t)(PC >> 8));
  }

  void PushPCL()
  {
    _link.WriteMemory((uint16_t)(S-- + 0x100), (uint8_t)PC);
  }

  void PushP_BRK()
  {
    SetFlagB(true);
    _link.WriteMemory((uint16_t)(S-- + 0x100), P);
    SetFlagI(true);
    ea = BRKVector;
  }

  void PushP_IRQ()
  {
    SetFlagB(false);
    _link.WriteMemory((uint16_t)(S-- + 0x100), P);
    SetFlagI(true);
    ea = IRQVector;
  }

  void PushP_NMI()
  {
    SetFlagB(false);
    _link.WriteMemory((uint16_t)(S-- + 0x100), P);
    SetFlagI(true); //is this right?
    ea = NMIVector;
  }

  void PushP_Reset()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      ea = ResetVector;
      _link.DummyReadMemory((uint16_t)(S-- + 0x100));
      SetFlagI(true);
    }
  }

  void PushDummy()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory((uint16_t)(S-- + 0x100));
    }
  }

  void FetchPCLVector()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      if (ea == BRKVector && GetFlagB() && NMI)
      {
        NMI = false;
        ea = NMIVector;
      }
      if (ea == IRQVector && !GetFlagB() && NMI)
      {
        NMI = false;
        ea = NMIVector;
      }
      alu_temp = _link.ReadMemory((uint16_t)ea);
    }
  }

  void FetchPCHVector()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp += _link.ReadMemory((uint16_t)(ea + 1)) << 8;
      PC = (uint16_t)alu_temp;
    }
  }

  void Imp_INY()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); Y++; NZ_Y();
    }
  }

  void Imp_DEY()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); Y--; NZ_Y();
    }
  }

  void Imp_INX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); X++; NZ_X();
    }
  }

  void Imp_DEX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); X--; NZ_X();
    }
  }

  void NZ_A()
  {
    P = (uint8_t)((P & 0x7D) | TableNZ[A]);
  }

  void NZ_X()
  {
    P = (uint8_t)((P & 0x7D) | TableNZ[X]);
  }

  void NZ_Y()
  {
    P = (uint8_t)((P & 0x7D) | TableNZ[Y]);
  }

  void Imp_TSX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); X = S; NZ_X();
    }
  }

  void Imp_TXS()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); S = X;
    }
  }

  void Imp_TAX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); X = A; NZ_X();
    }
  }

  void Imp_TAY()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); Y = A; NZ_Y();
    }
  }

  void Imp_TYA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); A = Y; NZ_A();
    }
  }

  void Imp_TXA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); A = X; NZ_A();
    }
  }

  void Imp_SEI()
  {
    // not affected by RDY
    iflag_pending = true;

    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC);
    }
  }

  void Imp_CLI()
  {
    // not affected by RDY
    iflag_pending = false;

    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC);
    }
  }

  void Imp_SEC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); SetFlagC(true);
    }
  }

  void Imp_CLC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); SetFlagC(false);
    }
  }

  void Imp_SED()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); SetFlagD(true);
    }
  }

  void Imp_CLD()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); SetFlagD(false);
    }
  }

  void Imp_CLV()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC); SetFlagV(false);
    }
  }

  void Abs_WRITE_STA()
  {
    _link.WriteMemory((uint16_t)((opcode3 << 8) + opcode2), A);
  }

  void Abs_WRITE_STX()
  {
    _link.WriteMemory((uint16_t)((opcode3 << 8) + opcode2), X);
  }

  void Abs_WRITE_STY()
  {
    _link.WriteMemory((uint16_t)((opcode3 << 8) + opcode2), Y);
  }

  void Abs_WRITE_SAX()
  {
    _link.WriteMemory((uint16_t)((opcode3 << 8) + opcode2), (uint8_t)(X & A));
  }

  void ZP_WRITE_STA()
  {
    _link.WriteMemory(opcode2, A);
  }

  void ZP_WRITE_STY()
  {
    _link.WriteMemory(opcode2, Y);
  }

  void ZP_WRITE_STX()
  {
    _link.WriteMemory(opcode2, X);
  }

  void ZP_WRITE_SAX()
  {
    _link.WriteMemory(opcode2, (uint8_t)(X & A));
  }

  void IndIdx_Stage3()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      ea = _link.ReadMemory(opcode2);
    }
  }

  void IndIdx_Stage4()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = ea + Y;
      ea = (_link.ReadMemory((uint8_t)(opcode2 + 1)) << 8)
        | ((alu_temp & 0xFF));
      address_bus = (uint16_t) ea;
      H = 0; // In preparation for SHA (indirect, X), set H to 0.
    }
  }

  void IndIdx_WRITE_Stage5()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.ReadMemory((uint16_t)ea);
      ea += (alu_temp >> 8) << 8;
    }
  }

  void IndIdx_WRITE_Stage5_SHA()
  {
    rdy_freeze = !RDY;

    if (RDY)
    {
      H |= (uint8_t)((ea >> 8) + 1);
      _link.ReadMemory((uint16_t) ea);

      if ((((alu_temp) >> 8 & 1) != 0))
      {
        ea = (uint16_t) (ea & 0xFF | ((ea + 0x100) & 0xFF00 & ((A & X) << 8)));
      }
    }
    else
    {
      H = 0xFF; //If the RDY line is low here, the SHA instruction omits the bitwise AND with H
    }
  }

  void IndIdx_READ_Stage5()
  {
    if (!(((alu_temp) >> 8 & 1) != 0))
    {
      mi++;
      ExecuteOneRetry();
      return;
    }
    else
    {
      rdy_freeze = !RDY;
      if (RDY)
      {
        _link.ReadMemory((uint16_t) ea);
        ea = (uint16_t) (ea + 0x100);
      }
    }
  }

  void IndIdx_RMW_Stage5()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.ReadMemory((uint16_t)ea);
      if ((((alu_temp) >> 8 & 1) != 0))
        ea = (uint16_t)(ea + 0x100);
    }
  }

  void IndIdx_WRITE_Stage6_STA()
  {
    _link.WriteMemory((uint16_t)ea, A);
  }

  void IndIdx_WRITE_Stage6_SHA()
  {
    alu_temp = A & X & H;
    _link.WriteMemory((uint16_t) ea, ((uint8_t)alu_temp));
  }

  void IndIdx_READ_Stage6_LDA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      A = _link.ReadMemory((uint16_t)ea);
      NZ_A();
    }
  }

  void IndIdx_READ_Stage6_CMP()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Cmp();
    }
  }

  void IndIdx_READ_Stage6_AND()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _And();
    }
  }

  void IndIdx_READ_Stage6_EOR()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Eor();
    }
  }

  void IndIdx_READ_Stage6_LAX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      A = X = _link.ReadMemory((uint16_t)ea);
      NZ_A();
    }
  }

  void IndIdx_READ_Stage6_ADC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Adc();
    }
  }

  void IndIdx_READ_Stage6_SBC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Sbc();
    }
  }

  void IndIdx_READ_Stage6_ORA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Ora();
    }
  }

  void IndIdx_RMW_Stage6()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
    }
  }

  void IndIdx_RMW_Stage7_SLO()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 0x80) != 0);
    alu_temp = value8 = (uint8_t)((value8 << 1));
    A |= value8;
    NZ_A();
  }

  void IndIdx_RMW_Stage7_SRE()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 1) != 0);
    alu_temp = value8 = (uint8_t)(value8 >> 1);
    A ^= value8;
    NZ_A();
  }

  void IndIdx_RMW_Stage7_RRA()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 >> 1) | ((P & 1) << 7));
    SetFlagC((temp8 & 1) != 0);
    _Adc();
  }

  void IndIdx_RMW_Stage7_ISC()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)(value8 + 1);
    _Sbc();
  }

  void IndIdx_RMW_Stage7_DCP()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)(value8 - 1);
    SetFlagC((temp8 & 1) != 0);
    _Cmp();
  }

  void IndIdx_RMW_Stage7_RLA()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 << 1) | (P & 1));
    SetFlagC((temp8 & 0x80) != 0);
    A &= value8;
    NZ_A();
  }

  void IndIdx_RMW_Stage8()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
  }

  void RelBranch_Stage2_BVS()
  {
    branch_taken = GetFlagV();
    RelBranch_Stage2();
  }

  void RelBranch_Stage2_BVC()
  {
    branch_taken = !GetFlagV();
    RelBranch_Stage2();
  }

  void RelBranch_Stage2_BMI()
  {
    branch_taken = GetFlagN();
    RelBranch_Stage2();
  }

  void RelBranch_Stage2_BPL()
  {
    branch_taken = !GetFlagN();
    RelBranch_Stage2();
  }

  void RelBranch_Stage2_BCS()
  {
    branch_taken = GetFlagC();
    RelBranch_Stage2();
  }

  void RelBranch_Stage2_BCC()
  {
    branch_taken = !GetFlagC();
    RelBranch_Stage2();
  }

  void RelBranch_Stage2_BEQ()
  {
    branch_taken = GetFlagZ();
    RelBranch_Stage2();
  }

  void RelBranch_Stage2_BNE()
  {
    branch_taken = !GetFlagZ();
    RelBranch_Stage2();
  }

  void RelBranch_Stage2()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      opcode2 = _link.ReadMemory(PC++);
      if (branch_taken)
      {
        branch_taken = false;
        //if the branch is taken, we enter a different bit of microcode to calculate the PC and complete the branch
        opcode = VOP_RelativeStuff;
        mi = -1;
      }
    }
  }

  void RelBranch_Stage3()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC);
      alu_temp = (uint8_t)PC + (int8_t)opcode2;
      PC &= 0xFF00;
      PC |= (uint16_t)(alu_temp & 0xFF);

      if ((((alu_temp) >> 8 & 1) != 0))
      {
        //we need to carry the add, and then we'll be ready to fetch the next instruction
        opcode = VOP_RelativeStuff2;
        mi = -1;
      }
      else
      {
        //to pass cpu_interrupts_v2/5-branch_delays_irq we need to handle a quirk here
        //if we decide to interrupt in the next cycle, this condition will cause it to get deferred by one instruction
        if (!interrupt_pending)
          branch_irq_hack = true;
      }
    }
  }

  void RelBranch_Stage4()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC);
      if ((((alu_temp) >> 31 & 1) != 0))
        PC = (uint16_t)(PC - 0x100);
      else PC = (uint16_t)(PC + 0x100);
    }
  }

  void NOP()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC);
    }
  }

  void DecS()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory((uint16_t) (0x100 | --S));
    }
  }

  void IncS()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory((uint16_t) (0x100 | S++));
    }
  }

  void JSR()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      PC = (uint16_t)((_link.ReadMemory(PC) << 8) + opcode2);
    }
  }

  void PullP()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      P = _link.ReadMemory((uint16_t)(S++ + 0x100));
      SetFlagT(true); //force T always to remain true
    }
  }

  void PullPCL()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      PC &= 0xFF00;
      PC |= _link.ReadMemory((uint16_t)(S++ + 0x100));
    }
  }

  void PullPCH_NoInc()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      PC &= 0xFF;
      PC |= (uint16_t)(_link.ReadMemory((uint16_t)(S + 0x100)) << 8);
    }
  }

  void Abs_READ_LDA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      A = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      NZ_A();
    }
  }

  void Abs_READ_LDY()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      Y = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      NZ_Y();
    }
  }

  void Abs_READ_LDX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      X = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      NZ_X();
    }
  }

  void Abs_READ_BIT()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      _Bit();
    }
  }

  void Abs_READ_LAX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      A = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      X = A;
      NZ_A();
    }
  }

  void Abs_READ_AND()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      _And();
    }
  }

  void Abs_READ_EOR()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      _Eor();
    }
  }

  void Abs_READ_ORA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      _Ora();
    }
  }

  void Abs_READ_ADC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      _Adc();
    }
  }

  void Abs_READ_CMP()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      _Cmp();
    }
  }

  void Abs_READ_CPY()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      _Cpy();
    }
  }

  void Abs_READ_NOP()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
    }
  }

  void Abs_READ_CPX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      _Cpx();
    }
  }

  void Abs_READ_SBC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)((opcode3 << 8) + opcode2));
      _Sbc();
    }
  }

  void ZpIdx_Stage3_X()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.ReadMemory(opcode2);
      opcode2 = (uint8_t)(opcode2 + X); //a bit sneaky to shove this into opcode2... but we can reuse all the zero page uops if we do that
    }
  }

  void ZpIdx_Stage3_Y()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.ReadMemory(opcode2);
      opcode2 = (uint8_t)(opcode2 + Y); //a bit sneaky to shove this into opcode2... but we can reuse all the zero page uops if we do that
    }
  }

  void ZpIdx_RMW_Stage4()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(opcode2);
    }
  }

  void ZpIdx_RMW_Stage6()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
  }

  void ZP_READ_EOR()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(opcode2);
      _Eor();
    }
  }

  void ZP_READ_BIT()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(opcode2);
      _Bit();
    }
  }

  void ZP_READ_LDA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      A = _link.ReadMemory(opcode2);
      NZ_A();
    }
  }

  void ZP_READ_LDY()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      Y = _link.ReadMemory(opcode2);
      NZ_Y();
    }
  }

  void ZP_READ_LDX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      X = _link.ReadMemory(opcode2);
      NZ_X();
    }
  }

  void ZP_READ_LAX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      //?? is this right??
      X = _link.ReadMemory(opcode2);
      A = X;
      NZ_A();
    }
  }

  void ZP_READ_CPY()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(opcode2);
      _Cpy();
    }
  }

  void ZP_READ_CMP()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(opcode2);
      _Cmp();
    }
  }

  void ZP_READ_CPX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(opcode2);
      _Cpx();
    }
  }

  void ZP_READ_ORA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(opcode2);
      _Ora();
    }
  }

  void ZP_READ_NOP()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.ReadMemory(opcode2); //just a dummy
    }
  }

  void ZP_READ_SBC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(opcode2);
      _Sbc();
    }
  }

  void ZP_READ_ADC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(opcode2);
      _Adc();
    }
  }

  void ZP_READ_AND()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(opcode2);
      _And();
    }
  }

  void _Cpx()
  {
    value8 = (uint8_t)alu_temp;
    value16 = (uint16_t)(X - value8);
    SetFlagC((X >= value8));
    P = (uint8_t)((P & 0x7D) | TableNZ[(uint8_t)value16]);
  }

  void _Cpy()
  {
    value8 = (uint8_t)alu_temp;
    value16 = (uint16_t)(Y - value8);
    SetFlagC((Y >= value8));
    P = (uint8_t)((P & 0x7D) | TableNZ[(uint8_t)value16]);
  }

  void _Cmp()
  {
    value8 = (uint8_t)alu_temp;
    value16 = (uint16_t)(A - value8);
    SetFlagC((A >= value8));
    P = (uint8_t)((P & 0x7D) | TableNZ[(uint8_t)value16]);
  }

  void _Bit()
  {
    SetFlagN((alu_temp & 0x80) != 0);
    SetFlagV((alu_temp & 0x40) != 0);
    SetFlagZ((A & alu_temp) == 0);
  }

  void _Eor()
  {
    A ^= (uint8_t)alu_temp;
    NZ_A();
  }

  void _And()
  {
    A &= (uint8_t)alu_temp;
    NZ_A();
  }

  void _Ora()
  {
    A |= (uint8_t)alu_temp;
    NZ_A();
  }

  void _Anc()
  {
    A &= (uint8_t)alu_temp;
    SetFlagC((((A) >> 7 & 1) != 0));
    NZ_A();
  }

  void _Asr()
  {
    A &= (uint8_t)alu_temp;
    SetFlagC((((A) >> 0 & 1) != 0));
    A >>= 1;
    NZ_A();
  }

  void _Axs()
  {
    X &= A;
    alu_temp = X - (uint8_t)alu_temp;
    X = (uint8_t)alu_temp;
    SetFlagC(!(((alu_temp) >> 8 & 1) != 0));
    NZ_X();
  }

  void _Arr()
  {
    A &= ((uint8_t)alu_temp);

    if (GetFlagD() && BCD_Enabled)
    {
      // Shift logic
      auto next = (A >> 1) | (GetFlagC() ? 0x80 : 0x00);
      SetFlagV(((A ^ next) & 0x40) != 0);
      SetFlagN(GetFlagC());
      SetFlagZ((next & 0xFF) == 0);

      // BCD fixup
      if ((A & 0x0F) + (A & 0x01) > 0x05)
      {
        next = (next & 0xF0) | ((next + 0x06) & 0x0F);
      }
      if ((A & 0xF0) + (A & 0x10) > 0x50)
      {
        next = (next & 0x0F) | ((next + 0x60) & 0xF0);
        SetFlagC(true);
      }
      else
      {
        SetFlagC(false);
      }

      A = ((uint8_t)next);
    }
    else
    {
      booltemp = (((A) >> 0 & 1) != 0);
      A = (uint8_t)((A >> 1) | (GetFlagC() ? 0x80 : 0x00));
      SetFlagC(booltemp);
      if ((((A) >> 5 & 1) != 0))
        if ((((A) >> 6 & 1) != 0))
        { SetFlagC(true); SetFlagV(false); }
        else { SetFlagV(true); SetFlagC(false); }
      else if ((((A) >> 6 & 1) != 0))
      { SetFlagV(true); SetFlagC(true); }
      else { SetFlagV(false); SetFlagC(false); }
      NZ_A();
    }
  }

  void _Ane()
  {
    // Many varied reports on what this should be.
    // A safe value is 0xFF. Commodore 64 needs 0xEF.
    A |= AneConstant;
    A &= ((uint8_t)(X & alu_temp));
    NZ_A();
  }

  void _Lxa()
  {
    //there is some debate about what this should be. it may depend on the 6502 variant.
    //this is suggested by qeed's doc for the nes and passes blargg's instruction test
    A |= LxaConstant;
    A &= (uint8_t)alu_temp;
    X = A;
    NZ_A();
  }

  void _Sbc()
  {
    value8 = ((uint8_t)alu_temp);
    tempint = A - value8 - (GetFlagC() ? 0 : 1);
    if (GetFlagD() && BCD_Enabled)
    {
      lo = (A & 0x0F) - (value8 & 0x0F) - (GetFlagC() ? 0 : 1);
      hi = (A & 0xF0) - (value8 & 0xF0);
      if ((lo & 0xF0) != 0) lo -= 0x06;
      if ((lo & 0x80) != 0) hi -= 0x10;
      if ((hi & 0x0F00) != 0) hi -= 0x60;
      SetFlagV(((A ^ value8) & (A ^ tempint) & 0x80) != 0);
      SetFlagZ((tempint & 0xFF) == 0);
      SetFlagN((tempint & 0x80) != 0);
      SetFlagC((hi & 0xFF00) == 0);
      A = ((uint8_t)((lo & 0x0F) | (hi & 0xF0)));
    }
    else
    {
      SetFlagV(((A ^ value8) & (A ^ tempint) & 0x80) != 0);
      SetFlagC(tempint >= 0);
      A = ((uint8_t)tempint);
      NZ_A();
    }
  }

  void _Adc()
  {
    //TODO - an extra cycle penalty on 65C02 only
    value8 = (uint8_t)alu_temp;
    if (GetFlagD() && BCD_Enabled)
    {
      tempint = (A & 0x0F) + (value8 & 0x0F) + (GetFlagC() ? 0x01 : 0x00);
      if (tempint > 0x09)
        tempint += 0x06;
      tempint = (tempint & 0x0F) + (A & 0xF0) + (value8 & 0xF0) + (tempint > 0x0F ? 0x10 : 0x00);
      SetFlagV((~(A ^ value8) & (A ^ tempint) & 0x80) != 0);
      SetFlagZ(((A + value8 + (GetFlagC() ? 1 : 0)) & 0xFF) == 0);
      SetFlagN((tempint & 0x80) != 0);
      if ((tempint & 0x1F0) > 0x090)
        tempint += 0x060;
      SetFlagC(tempint > 0xFF);
      A = (uint8_t)(tempint & 0xFF);
    }
    else
    {
      tempint = value8 + A + (GetFlagC() ? 1 : 0);
      SetFlagV((~(A ^ value8) & (A ^ tempint) & 0x80) != 0);
      SetFlagC(tempint > 0xFF);
      A = (uint8_t)tempint;
      NZ_A();
    }
  }

  void Unsupported() {}

  void Imm_EOR()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Eor();
    }
  }

  void Imm_ANC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Anc();
    }
  }

  void Imm_ASR()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Asr();
    }
  }

  void Imm_AXS()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Axs();
    }
  }

  void Imm_ARR()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Arr();
    }
  }

  void Imm_LXA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Lxa();
    }
  }

  void Imm_ORA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Ora();
    }
  }

  void Imm_CPY()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Cpy();
    }
  }

  void Imm_CPX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Cpx();
    }
  }

  void Imm_CMP()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Cmp();
    }
  }

  void Imm_SBC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Sbc();
    }
  }

  void Imm_AND()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _And();
    }
  }

  void Imm_ADC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Adc();
    }
  }

  void Imm_LDA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      A = _link.ReadMemory(PC++);
      NZ_A();
    }
  }

  void Imm_LDX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      X = _link.ReadMemory(PC++);
      NZ_X();
    }
  }

  void Imm_LDY()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      Y = _link.ReadMemory(PC++);
      NZ_Y();
    }
  }

  void Imm_ANE()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(PC++);
      _Ane();
    }
  }

  void Imm_Unsupported()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.ReadMemory(PC++);
    }
  }

  void IdxInd_Stage3()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(opcode2);
      alu_temp = (opcode2 + X) & 0xFF;
    }
  }

  void IdxInd_Stage4()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      ea = _link.ReadMemory((uint16_t)alu_temp);
      address_bus = (uint16_t) ea;
    }
  }

  void IdxInd_Stage5()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      ea += (_link.ReadMemory((uint8_t)(alu_temp + 1)) << 8);
    }
  }

  void IdxInd_Stage6_READ_LDA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      //TODO make uniform with others
      A = _link.ReadMemory((uint16_t)ea);
      NZ_A();
    }
  }

  void IdxInd_Stage6_READ_ORA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Ora();
    }
  }

  void IdxInd_Stage6_READ_LAX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      A = X = _link.ReadMemory((uint16_t)ea);
      NZ_A();
    }
  }

  void IdxInd_Stage6_READ_CMP()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Cmp();
    }
  }

  void IdxInd_Stage6_READ_ADC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Adc();
    }
  }

  void IdxInd_Stage6_READ_AND()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _And();
    }
  }

  void IdxInd_Stage6_READ_EOR()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Eor();
    }
  }

  void IdxInd_Stage6_READ_SBC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Sbc();
    }
  }

  void IdxInd_Stage6_WRITE_STA()
  {
    _link.WriteMemory((uint16_t)ea, A);
  }

  void IdxInd_Stage6_WRITE_SAX()
  {
    alu_temp = A & X;
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    //flag writing skipped on purpose
  }

  void IdxInd_Stage6_RMW()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
    }
  }

  void IdxInd_Stage7_RMW_SLO()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 0x80) != 0);
    alu_temp = value8 = (uint8_t)((value8 << 1));
    A |= value8;
    NZ_A();
  }

  void IdxInd_Stage7_RMW_ISC()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)(value8 + 1);
    _Sbc();
  }

  void IdxInd_Stage7_RMW_DCP()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)(value8 - 1);
    SetFlagC((temp8 & 1) != 0);
    _Cmp();
  }

  void IdxInd_Stage7_RMW_SRE()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 1) != 0);
    alu_temp = value8 = (uint8_t)(value8 >> 1);
    A ^= value8;
    NZ_A();
  }

  void IdxInd_Stage7_RMW_RRA()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 >> 1) | ((P & 1) << 7));
    SetFlagC((temp8 & 1) != 0);
    _Adc();
  }

  void IdxInd_Stage7_RMW_RLA()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 << 1) | (P & 1));
    SetFlagC((temp8 & 0x80) != 0);
    A &= value8;
    NZ_A();
  }

  void IdxInd_Stage8_RMW()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
  }

  void PushP()
  {
    SetFlagB(true);
    _link.WriteMemory((uint16_t)(S-- + 0x100), P);
  }

  void PushA()
  {
    _link.WriteMemory((uint16_t)(S-- + 0x100), A);
  }

  void PullA_NoInc()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      A = _link.ReadMemory((uint16_t)(S + 0x100));
      NZ_A();
    }
  }

  void PullP_NoInc()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      my_iflag = GetFlagI();
      P = _link.ReadMemory((uint16_t)(S + 0x100));
      iflag_pending = GetFlagI();
      SetFlagI(my_iflag);
      SetFlagT(true); //force T always to remain true
    }
  }

  void Imp_ASL_A()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC);
      SetFlagC((A & 0x80) != 0);
      A = (uint8_t)(A << 1);
      NZ_A();
    }
  }

  void Imp_ROL_A()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC);
      temp8 = A;
      A = (uint8_t)((A << 1) | (P & 1));
      SetFlagC((temp8 & 0x80) != 0);
      NZ_A();
    }
  }

  void Imp_ROR_A()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC);
      temp8 = A;
      A = (uint8_t)((A >> 1) | ((P & 1) << 7));
      SetFlagC((temp8 & 1) != 0);
      NZ_A();
    }
  }

  void Imp_LSR_A()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.DummyReadMemory(PC);
      SetFlagC((A & 1) != 0);
      A = (uint8_t)(A >> 1);
      NZ_A();
    }
  }

  void JMP_abs()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      PC = (uint16_t)((_link.ReadMemory(PC) << 8) + opcode2);
    }
  }

  void IncPC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      _link.ReadMemory(PC);
      PC++;
    }
  }

  void ZP_RMW_Stage3()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory(opcode2);
    }
  }

  void ZP_RMW_Stage5()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
  }

  void ZP_RMW_INC()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    alu_temp = (uint8_t)((alu_temp + 1) & 0xFF);
    P = (uint8_t)((P & 0x7D) | TableNZ[alu_temp]);
  }

  void ZP_RMW_DEC()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    alu_temp = (uint8_t)((alu_temp - 1) & 0xFF);
    P = (uint8_t)((P & 0x7D) | TableNZ[alu_temp]);
  }

  void ZP_RMW_ASL()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 0x80) != 0);
    alu_temp = value8 = (uint8_t)(value8 << 1);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void ZP_RMW_SRE()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 1) != 0);
    alu_temp = value8 = (uint8_t)(value8 >> 1);
    A ^= value8;
    NZ_A();
  }

  void ZP_RMW_RRA()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 >> 1) | ((P & 1) << 7));
    SetFlagC((temp8 & 1) != 0);
    _Adc();
  }

  void ZP_RMW_DCP()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)(value8 - 1);
    SetFlagC((temp8 & 1) != 0);
    _Cmp();
  }

  void ZP_RMW_LSR()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 1) != 0);
    alu_temp = value8 = (uint8_t)(value8 >> 1);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void ZP_RMW_ROR()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 >> 1) | ((P & 1) << 7));
    SetFlagC((temp8 & 1) != 0);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void ZP_RMW_ROL()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 << 1) | (P & 1));
    SetFlagC((temp8 & 0x80) != 0);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void ZP_RMW_SLO()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 0x80) != 0);
    alu_temp = value8 = (uint8_t)((value8 << 1));
    A |= value8;
    NZ_A();
  }

  void ZP_RMW_ISC()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)(value8 + 1);
    _Sbc();
  }

  void ZP_RMW_RLA()
  {
    _link.WriteMemory(opcode2, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 << 1) | (P & 1));
    SetFlagC((temp8 & 0x80) != 0);
    A &= value8;
    NZ_A();
  }

  void AbsIdx_Stage3_Y()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      opcode3 = _link.ReadMemory(PC++);
      alu_temp = opcode2 + Y;
      ea = (opcode3 << 8) + (alu_temp & 0xFF);
      address_bus = (uint16_t) ea;
      H = 0; // In preparation for SHA, SHS, and SHX, set H to 0.
      //new Uop[] { Uop::Fetch2, Uop::AbsIdx_Stage3_Y, Uop::AbsIdx_Stage4, Uop::AbsIdx_WRITE_Stage5_STA, Uop::End },
    }
  }

  void AbsIdx_Stage3_X()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      opcode3 = _link.ReadMemory(PC++);
      alu_temp = opcode2 + X;
      ea = (opcode3 << 8) + (alu_temp & 0xFF);
      address_bus = (uint16_t) ea;
      H = 0; // In preparation for SHY, set H to 0.
    }
  }

  void AbsIdx_READ_Stage4()
  {
    if (!(((alu_temp) >> 8 & 1) != 0))
    {
      mi++;
      ExecuteOneRetry();
    }
    else
    {
      rdy_freeze = !RDY;
      if (RDY)
      {
        alu_temp = _link.ReadMemory((uint16_t) ea);
        ea = (uint16_t) (ea + 0x100);
      }
    }
  }

  void AbsIdx_Stage4()
  {
    rdy_freeze = !RDY;

    if (RDY)
    {
      auto adjust = (((alu_temp) >> 8 & 1) != 0);
      alu_temp = _link.ReadMemory((uint16_t) ea);

      if (adjust)
      {
        ea = (uint16_t)(ea + 0x100);
      }
    }
  }

  void AbsIdx_Stage4_SHX()
  {
    rdy_freeze = !RDY;

    if (RDY)
    {
      H |= (uint8_t)((ea >> 8) + 1);
      auto adjust = (((alu_temp) >> 8 & 1) != 0);
      alu_temp = _link.ReadMemory((uint16_t) ea);

      if (adjust)
      {
        ea = (uint16_t) (ea & 0xFF | ((ea + 0x100) & 0xFF00 & (X << 8)));
      }
    }
    else
    {
      H = 0xFF; //If the RDY line is low here, the SHX instruction omits the bitwise AND with H
    }
  }

  void AbsIdx_Stage4_SHY()
  {
    rdy_freeze = !RDY;

    if (RDY)
    {
      H |= (uint8_t)((ea >> 8) + 1);
      auto adjust = (((alu_temp) >> 8 & 1) != 0);
      alu_temp = _link.ReadMemory((uint16_t) ea);

      if (adjust)
      {
        ea = (uint16_t) (ea & 0xFF | ((ea + 0x100) & 0xFF00 & (Y << 8)));
      }
    }
    else
    {
      H = 0xFF; //If the RDY line is low here, the SHY instruction omits the bitwise AND with H
    }
  }

  void AbsIdx_Stage4_SHA()
  {
    rdy_freeze = !RDY;

    if (RDY)
    {
      H |= (uint8_t)((ea >> 8) + 1);
      auto adjust = (((alu_temp) >> 8 & 1) != 0);
      alu_temp = _link.ReadMemory((uint16_t) ea);

      if (adjust)
      {
        ea = (uint16_t) ((ea & 0xFF) | ((ea + 0x100) & 0xFF00 & ((A & X) << 8)));
      }
    }
    else
    {
      H = 0xFF; //If the RDY line is low here, the SHA instruction omits the bitwise AND with H
    }
  }

  void AbsIdx_Stage4_SHS()
  {
    rdy_freeze = !RDY;

    if (RDY)
    {
      H |= (uint8_t)((ea >> 8) + 1);
      auto adjust = (((alu_temp) >> 8 & 1) != 0);
      alu_temp = _link.ReadMemory((uint16_t) ea);

      if (adjust)
      {
        ea = (uint16_t) (ea & 0xFF | ((ea + 0x100) & 0xFF00 & ((A & X) << 8)));
      }
    }
    else
    {
      H = 0xFF; //If the RDY line is low here, the SHS instruction omits the bitwise AND with H
    }
  }

  void AbsIdx_WRITE_Stage5_STA()
  {
    _link.WriteMemory((uint16_t)ea, A);
  }

  void AbsIdx_WRITE_Stage5_SHY()
  {
    alu_temp = Y & H;
    _link.WriteMemory((uint16_t) ea, (uint8_t)alu_temp);
  }

  void AbsIdx_WRITE_Stage5_SHX()
  {
    alu_temp = X & H;
    _link.WriteMemory((uint16_t) ea, (uint8_t)alu_temp);
  }

  void AbsIdx_WRITE_Stage5_SHA()
  {
    alu_temp = A & X & H;
    _link.WriteMemory((uint16_t) ea, (uint8_t)alu_temp);
  }

  void AbsIdx_WRITE_Stage5_SHS()
  {
    S = (uint8_t)(X & A);
    _link.WriteMemory((uint16_t) ea, (uint8_t)(S & H));
  }

  void AbsIdx_RMW_Stage5()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
    }
  }

  void AbsIdx_RMW_Stage7()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
  }

  void AbsIdx_RMW_Stage6_DEC()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    alu_temp = value8 = (uint8_t)(alu_temp - 1);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void AbsIdx_RMW_Stage6_DCP()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    alu_temp = value8 = (uint8_t)(alu_temp - 1);
    _Cmp();
  }

  void AbsIdx_RMW_Stage6_ISC()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    alu_temp = value8 = (uint8_t)(alu_temp + 1);
    _Sbc();
  }

  void AbsIdx_RMW_Stage6_INC()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    alu_temp = value8 = (uint8_t)(alu_temp + 1);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void AbsIdx_RMW_Stage6_ROL()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 << 1) | (P & 1));
    SetFlagC((temp8 & 0x80) != 0);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void AbsIdx_RMW_Stage6_LSR()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 1) != 0);
    alu_temp = value8 = (uint8_t)(value8 >> 1);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void AbsIdx_RMW_Stage6_SLO()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 0x80) != 0);
    alu_temp = value8 = (uint8_t)(value8 << 1);
    A |= value8;
    NZ_A();
  }

  void AbsIdx_RMW_Stage6_SRE()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 1) != 0);
    alu_temp = value8 = (uint8_t)(value8 >> 1);
    A ^= value8;
    NZ_A();
  }

  void AbsIdx_RMW_Stage6_RRA()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 >> 1) | ((P & 1) << 7));
    SetFlagC((temp8 & 1) != 0);
    _Adc();
  }

  void AbsIdx_RMW_Stage6_RLA()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 << 1) | (P & 1));
    SetFlagC((temp8 & 0x80) != 0);
    A &= value8;
    NZ_A();
  }

  void AbsIdx_RMW_Stage6_ASL()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 0x80) != 0);
    alu_temp = value8 = (uint8_t)(value8 << 1);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void AbsIdx_RMW_Stage6_ROR()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 >> 1) | ((P & 1) << 7));
    SetFlagC((temp8 & 1) != 0);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void AbsIdx_READ_Stage5_LDA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      A = _link.ReadMemory((uint16_t)ea);
      NZ_A();
    }
  }

  void AbsIdx_READ_Stage5_LDX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      X = _link.ReadMemory((uint16_t)ea);
      NZ_X();
    }
  }

  void AbsIdx_READ_Stage5_LAX()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      A = _link.ReadMemory((uint16_t)ea);
      X = A;
      NZ_A();
    }
  }

  void AbsIdx_READ_Stage5_LDY()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      Y = _link.ReadMemory((uint16_t)ea);
      NZ_Y();
    }
  }

  void AbsIdx_READ_Stage5_ORA()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Ora();
    }
  }

  void AbsIdx_READ_Stage5_NOP()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
    }
  }

  void AbsIdx_READ_Stage5_CMP()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Cmp();
    }
  }

  void AbsIdx_READ_Stage5_SBC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Sbc();
    }
  }

  void AbsIdx_READ_Stage5_ADC()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Adc();
    }
  }

  void AbsIdx_READ_Stage5_EOR()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _Eor();
    }
  }

  void AbsIdx_READ_Stage5_AND()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      _And();
    }
  }

  void AbsIdx_READ_Stage5_ERROR()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      alu_temp = _link.ReadMemory((uint16_t)ea);
      S &= (uint8_t)alu_temp;
      X = S;
      A = S;
      P = (uint8_t)((P & 0x7D) | TableNZ[S]);
    }
  }

  void AbsInd_JMP_Stage4()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      ea = (opcode3 << 8) + opcode2;
      alu_temp = _link.ReadMemory((uint16_t)ea);
    }
  }

  void AbsInd_JMP_Stage5()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      ea = (opcode3 << 8) + (uint8_t)(opcode2 + 1);
      alu_temp += _link.ReadMemory((uint16_t)ea) << 8;
      PC = (uint16_t)alu_temp;
    }
  }

  void Abs_RMW_Stage4()
  {
    rdy_freeze = !RDY;
    if (RDY)
    {
      ea = (opcode3 << 8) + opcode2;
      alu_temp = _link.ReadMemory((uint16_t)ea);
    }
  }

  void Abs_RMW_Stage5_INC()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)(alu_temp + 1);
    alu_temp = value8;
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void Abs_RMW_Stage5_DEC()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)(alu_temp - 1);
    alu_temp = value8;
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void Abs_RMW_Stage5_DCP()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)(alu_temp - 1);
    alu_temp = value8;
    _Cmp();
  }

  void Abs_RMW_Stage5_ISC()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)(alu_temp + 1);
    alu_temp = value8;
    _Sbc();
  }

  void Abs_RMW_Stage5_ASL()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 0x80) != 0);
    alu_temp = value8 = (uint8_t)(value8 << 1);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void Abs_RMW_Stage5_ROR()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 >> 1) | ((P & 1) << 7));
    SetFlagC((temp8 & 1) != 0);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void Abs_RMW_Stage5_SLO()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 0x80) != 0);
    alu_temp = value8 = (uint8_t)(value8 << 1);
    A |= value8;
    NZ_A();
  }

  void Abs_RMW_Stage5_RLA()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 << 1) | (P & 1));
    SetFlagC((temp8 & 0x80) != 0);
    A &= value8;
    NZ_A();
  }

  void Abs_RMW_Stage5_SRE()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 1) != 0);
    alu_temp = value8 = (uint8_t)(value8 >> 1);
    A ^= value8;
    NZ_A();
  }

  void Abs_RMW_Stage5_RRA()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 >> 1) | ((P & 1) << 7));
    SetFlagC((temp8 & 1) != 0);
    _Adc();
  }

  void Abs_RMW_Stage5_ROL()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = temp8 = (uint8_t)alu_temp;
    alu_temp = value8 = (uint8_t)((value8 << 1) | (P & 1));
    SetFlagC((temp8 & 0x80) != 0);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void Abs_RMW_Stage5_LSR()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
    value8 = (uint8_t)alu_temp;
    SetFlagC((value8 & 1) != 0);
    alu_temp = value8 = (uint8_t)(value8 >> 1);
    P = (uint8_t)((P & 0x7D) | TableNZ[value8]);
  }

  void Abs_RMW_Stage6()
  {
    _link.WriteMemory((uint16_t)ea, (uint8_t)alu_temp);
  }

  void End_ISpecial()
  {
    opcode = VOP_Fetch1;
    mi = 0;
    ExecuteOneRetry();
    return;
  }

  void End_SuppressInterrupt()
  {
    opcode = VOP_Fetch1_NoInterrupt;
    mi = 0;
    ExecuteOneRetry();
    return;
  }

  void End()
  {
    opcode = VOP_Fetch1;
    mi = 0;
    iflag_pending = GetFlagI();
    ExecuteOneRetry();
  }

  void End_BranchSpecial()
  {
    End();
  }

  void Jam()
  {
    rdy_freeze = true;
  }

  void ExecuteOneRetry()
  {
    //don't know whether this system is any faster. hard to get benchmarks someone else try it?
    //Uop uop = (Uop)CompiledMicrocode[MicrocodeIndex[opcode] + mi];
    Uop uop = Microcode[opcode][mi];
    switch (uop)
    {
      default: __builtin_trap();
      case Uop::Fetch1: Fetch1(); break;
      case Uop::Fetch1_Real: Fetch1_Real(); break;
      case Uop::Fetch2: Fetch2(); break;
      case Uop::Fetch3: Fetch3(); break;
      case Uop::FetchDummy: FetchDummy(); break;
      case Uop::PushPCH: PushPCH(); break;
      case Uop::PushPCL: PushPCL(); break;
      case Uop::PushP_BRK: PushP_BRK(); break;
      case Uop::PushP_IRQ: PushP_IRQ(); break;
      case Uop::PushP_NMI: PushP_NMI(); break;
      case Uop::PushP_Reset: PushP_Reset(); break;
      case Uop::PushDummy: PushDummy(); break;
      case Uop::FetchPCLVector: FetchPCLVector(); break;
      case Uop::FetchPCHVector: FetchPCHVector(); break;
      case Uop::Imp_INY: Imp_INY(); break;
      case Uop::Imp_DEY: Imp_DEY(); break;
      case Uop::Imp_INX: Imp_INX(); break;
      case Uop::Imp_DEX: Imp_DEX(); break;
      case Uop::NZ_A: NZ_A(); break;
      case Uop::NZ_X: NZ_X(); break;
      case Uop::NZ_Y: NZ_Y(); break;
      case Uop::Imp_TSX: Imp_TSX(); break;
      case Uop::Imp_TXS: Imp_TXS(); break;
      case Uop::Imp_TAX: Imp_TAX(); break;
      case Uop::Imp_TAY: Imp_TAY(); break;
      case Uop::Imp_TYA: Imp_TYA(); break;
      case Uop::Imp_TXA: Imp_TXA(); break;
      case Uop::Imp_SEI: Imp_SEI(); break;
      case Uop::Imp_CLI: Imp_CLI(); break;
      case Uop::Imp_SEC: Imp_SEC(); break;
      case Uop::Imp_CLC: Imp_CLC(); break;
      case Uop::Imp_SED: Imp_SED(); break;
      case Uop::Imp_CLD: Imp_CLD(); break;
      case Uop::Imp_CLV: Imp_CLV(); break;
      case Uop::Abs_WRITE_STA: Abs_WRITE_STA(); break;
      case Uop::Abs_WRITE_STX: Abs_WRITE_STX(); break;
      case Uop::Abs_WRITE_STY: Abs_WRITE_STY(); break;
      case Uop::Abs_WRITE_SAX: Abs_WRITE_SAX(); break;
      case Uop::ZP_WRITE_STA: ZP_WRITE_STA(); break;
      case Uop::ZP_WRITE_STY: ZP_WRITE_STY(); break;
      case Uop::ZP_WRITE_STX: ZP_WRITE_STX(); break;
      case Uop::ZP_WRITE_SAX: ZP_WRITE_SAX(); break;
      case Uop::IndIdx_Stage3: IndIdx_Stage3(); break;
      case Uop::IndIdx_Stage4: IndIdx_Stage4(); break;
      case Uop::IndIdx_WRITE_Stage5: IndIdx_WRITE_Stage5(); break;
      case Uop::IndIdx_WRITE_Stage5_SHA: IndIdx_WRITE_Stage5_SHA(); break;
      case Uop::IndIdx_READ_Stage5: IndIdx_READ_Stage5(); break;
      case Uop::IndIdx_RMW_Stage5: IndIdx_RMW_Stage5(); break;
      case Uop::IndIdx_WRITE_Stage6_STA: IndIdx_WRITE_Stage6_STA(); break;
      case Uop::IndIdx_WRITE_Stage6_SHA: IndIdx_WRITE_Stage6_SHA(); break;
      case Uop::IndIdx_READ_Stage6_LDA: IndIdx_READ_Stage6_LDA(); break;
      case Uop::IndIdx_READ_Stage6_CMP: IndIdx_READ_Stage6_CMP(); break;
      case Uop::IndIdx_READ_Stage6_AND: IndIdx_READ_Stage6_AND(); break;
      case Uop::IndIdx_READ_Stage6_EOR: IndIdx_READ_Stage6_EOR(); break;
      case Uop::IndIdx_READ_Stage6_LAX: IndIdx_READ_Stage6_LAX(); break;
      case Uop::IndIdx_READ_Stage6_ADC: IndIdx_READ_Stage6_ADC(); break;
      case Uop::IndIdx_READ_Stage6_SBC: IndIdx_READ_Stage6_SBC(); break;
      case Uop::IndIdx_READ_Stage6_ORA: IndIdx_READ_Stage6_ORA(); break;
      case Uop::IndIdx_RMW_Stage6: IndIdx_RMW_Stage6(); break;
      case Uop::IndIdx_RMW_Stage7_SLO: IndIdx_RMW_Stage7_SLO(); break;
      case Uop::IndIdx_RMW_Stage7_SRE: IndIdx_RMW_Stage7_SRE(); break;
      case Uop::IndIdx_RMW_Stage7_RRA: IndIdx_RMW_Stage7_RRA(); break;
      case Uop::IndIdx_RMW_Stage7_ISC: IndIdx_RMW_Stage7_ISC(); break;
      case Uop::IndIdx_RMW_Stage7_DCP: IndIdx_RMW_Stage7_DCP(); break;
      case Uop::IndIdx_RMW_Stage7_RLA: IndIdx_RMW_Stage7_RLA(); break;
      case Uop::IndIdx_RMW_Stage8: IndIdx_RMW_Stage8(); break;
      case Uop::RelBranch_Stage2_BVS: RelBranch_Stage2_BVS(); break;
      case Uop::RelBranch_Stage2_BVC: RelBranch_Stage2_BVC(); break;
      case Uop::RelBranch_Stage2_BMI: RelBranch_Stage2_BMI(); break;
      case Uop::RelBranch_Stage2_BPL: RelBranch_Stage2_BPL(); break;
      case Uop::RelBranch_Stage2_BCS: RelBranch_Stage2_BCS(); break;
      case Uop::RelBranch_Stage2_BCC: RelBranch_Stage2_BCC(); break;
      case Uop::RelBranch_Stage2_BEQ: RelBranch_Stage2_BEQ(); break;
      case Uop::RelBranch_Stage2_BNE: RelBranch_Stage2_BNE(); break;
      case Uop::RelBranch_Stage2: RelBranch_Stage2(); break;
      case Uop::RelBranch_Stage3: RelBranch_Stage3(); break;
      case Uop::RelBranch_Stage4: RelBranch_Stage4(); break;
      case Uop::NOP: NOP(); break;
      case Uop::DecS: DecS(); break;
      case Uop::IncS: IncS(); break;
      case Uop::JSR: JSR(); break;
      case Uop::PullP: PullP(); break;
      case Uop::PullPCL: PullPCL(); break;
      case Uop::PullPCH_NoInc: PullPCH_NoInc(); break;
      case Uop::Abs_READ_LDA: Abs_READ_LDA(); break;
      case Uop::Abs_READ_LDY: Abs_READ_LDY(); break;
      case Uop::Abs_READ_LDX: Abs_READ_LDX(); break;
      case Uop::Abs_READ_BIT: Abs_READ_BIT(); break;
      case Uop::Abs_READ_LAX: Abs_READ_LAX(); break;
      case Uop::Abs_READ_AND: Abs_READ_AND(); break;
      case Uop::Abs_READ_EOR: Abs_READ_EOR(); break;
      case Uop::Abs_READ_ORA: Abs_READ_ORA(); break;
      case Uop::Abs_READ_ADC: Abs_READ_ADC(); break;
      case Uop::Abs_READ_CMP: Abs_READ_CMP(); break;
      case Uop::Abs_READ_CPY: Abs_READ_CPY(); break;
      case Uop::Abs_READ_NOP: Abs_READ_NOP(); break;
      case Uop::Abs_READ_CPX: Abs_READ_CPX(); break;
      case Uop::Abs_READ_SBC: Abs_READ_SBC(); break;
      case Uop::ZpIdx_Stage3_X: ZpIdx_Stage3_X(); break;
      case Uop::ZpIdx_Stage3_Y: ZpIdx_Stage3_Y(); break;
      case Uop::ZpIdx_RMW_Stage4: ZpIdx_RMW_Stage4(); break;
      case Uop::ZpIdx_RMW_Stage6: ZpIdx_RMW_Stage6(); break;
      case Uop::ZP_READ_EOR: ZP_READ_EOR(); break;
      case Uop::ZP_READ_BIT: ZP_READ_BIT(); break;
      case Uop::ZP_READ_LDA: ZP_READ_LDA(); break;
      case Uop::ZP_READ_LDY: ZP_READ_LDY(); break;
      case Uop::ZP_READ_LDX: ZP_READ_LDX(); break;
      case Uop::ZP_READ_LAX: ZP_READ_LAX(); break;
      case Uop::ZP_READ_CPY: ZP_READ_CPY(); break;
      case Uop::ZP_READ_CMP: ZP_READ_CMP(); break;
      case Uop::ZP_READ_CPX: ZP_READ_CPX(); break;
      case Uop::ZP_READ_ORA: ZP_READ_ORA(); break;
      case Uop::ZP_READ_NOP: ZP_READ_NOP(); break;
      case Uop::ZP_READ_SBC: ZP_READ_SBC(); break;
      case Uop::ZP_READ_ADC: ZP_READ_ADC(); break;
      case Uop::ZP_READ_AND: ZP_READ_AND(); break;
      case Uop::_Cpx: _Cpx(); break;
      case Uop::_Cpy: _Cpy(); break;
      case Uop::_Cmp: _Cmp(); break;
      case Uop::_Eor: _Eor(); break;
      case Uop::_And: _And(); break;
      case Uop::_Ora: _Ora(); break;
      case Uop::_Anc: _Anc(); break;
      case Uop::_Asr: _Asr(); break;
      case Uop::_Axs: _Axs(); break;
      case Uop::_Arr: _Arr(); break;
      case Uop::_Lxa: _Lxa(); break;
      case Uop::_Sbc: _Sbc(); break;
      case Uop::_Adc: _Adc(); break;
      case Uop::Unsupported: Unsupported(); break;
      case Uop::Imm_EOR: Imm_EOR(); break;
      case Uop::Imm_ANC: Imm_ANC(); break;
      case Uop::Imm_ASR: Imm_ASR(); break;
      case Uop::Imm_AXS: Imm_AXS(); break;
      case Uop::Imm_ARR: Imm_ARR(); break;
      case Uop::Imm_LXA: Imm_LXA(); break;
      case Uop::Imm_ORA: Imm_ORA(); break;
      case Uop::Imm_CPY: Imm_CPY(); break;
      case Uop::Imm_CPX: Imm_CPX(); break;
      case Uop::Imm_CMP: Imm_CMP(); break;
      case Uop::Imm_SBC: Imm_SBC(); break;
      case Uop::Imm_AND: Imm_AND(); break;
      case Uop::Imm_ADC: Imm_ADC(); break;
      case Uop::Imm_LDA: Imm_LDA(); break;
      case Uop::Imm_LDX: Imm_LDX(); break;
      case Uop::Imm_LDY: Imm_LDY(); break;
      case Uop::Imm_ANE: Imm_ANE(); break;
      case Uop::Imm_Unsupported: Imm_Unsupported(); break;
      case Uop::IdxInd_Stage3: IdxInd_Stage3(); break;
      case Uop::IdxInd_Stage4: IdxInd_Stage4(); break;
      case Uop::IdxInd_Stage5: IdxInd_Stage5(); break;
      case Uop::IdxInd_Stage6_READ_LDA: IdxInd_Stage6_READ_LDA(); break;
      case Uop::IdxInd_Stage6_READ_ORA: IdxInd_Stage6_READ_ORA(); break;
      case Uop::IdxInd_Stage6_READ_LAX: IdxInd_Stage6_READ_LAX(); break;
      case Uop::IdxInd_Stage6_READ_CMP: IdxInd_Stage6_READ_CMP(); break;
      case Uop::IdxInd_Stage6_READ_ADC: IdxInd_Stage6_READ_ADC(); break;
      case Uop::IdxInd_Stage6_READ_AND: IdxInd_Stage6_READ_AND(); break;
      case Uop::IdxInd_Stage6_READ_EOR: IdxInd_Stage6_READ_EOR(); break;
      case Uop::IdxInd_Stage6_READ_SBC: IdxInd_Stage6_READ_SBC(); break;
      case Uop::IdxInd_Stage6_WRITE_STA: IdxInd_Stage6_WRITE_STA(); break;
      case Uop::IdxInd_Stage6_WRITE_SAX: IdxInd_Stage6_WRITE_SAX(); break;
      case Uop::IdxInd_Stage6_RMW: IdxInd_Stage6_RMW(); break;
      case Uop::IdxInd_Stage7_RMW_SLO: IdxInd_Stage7_RMW_SLO(); break;
      case Uop::IdxInd_Stage7_RMW_ISC: IdxInd_Stage7_RMW_ISC(); break;
      case Uop::IdxInd_Stage7_RMW_DCP: IdxInd_Stage7_RMW_DCP(); break;
      case Uop::IdxInd_Stage7_RMW_SRE: IdxInd_Stage7_RMW_SRE(); break;
      case Uop::IdxInd_Stage7_RMW_RRA: IdxInd_Stage7_RMW_RRA(); break;
      case Uop::IdxInd_Stage7_RMW_RLA: IdxInd_Stage7_RMW_RLA(); break;
      case Uop::IdxInd_Stage8_RMW: IdxInd_Stage8_RMW(); break;
      case Uop::PushP: PushP(); break;
      case Uop::PushA: PushA(); break;
      case Uop::PullA_NoInc: PullA_NoInc(); break;
      case Uop::PullP_NoInc: PullP_NoInc(); break;
      case Uop::Imp_ASL_A: Imp_ASL_A(); break;
      case Uop::Imp_ROL_A: Imp_ROL_A(); break;
      case Uop::Imp_ROR_A: Imp_ROR_A(); break;
      case Uop::Imp_LSR_A: Imp_LSR_A(); break;
      case Uop::JMP_abs: JMP_abs(); break;
      case Uop::IncPC: IncPC(); break;
      case Uop::ZP_RMW_Stage3: ZP_RMW_Stage3(); break;
      case Uop::ZP_RMW_Stage5: ZP_RMW_Stage5(); break;
      case Uop::ZP_RMW_INC: ZP_RMW_INC(); break;
      case Uop::ZP_RMW_DEC: ZP_RMW_DEC(); break;
      case Uop::ZP_RMW_ASL: ZP_RMW_ASL(); break;
      case Uop::ZP_RMW_SRE: ZP_RMW_SRE(); break;
      case Uop::ZP_RMW_RRA: ZP_RMW_RRA(); break;
      case Uop::ZP_RMW_DCP: ZP_RMW_DCP(); break;
      case Uop::ZP_RMW_LSR: ZP_RMW_LSR(); break;
      case Uop::ZP_RMW_ROR: ZP_RMW_ROR(); break;
      case Uop::ZP_RMW_ROL: ZP_RMW_ROL(); break;
      case Uop::ZP_RMW_SLO: ZP_RMW_SLO(); break;
      case Uop::ZP_RMW_ISC: ZP_RMW_ISC(); break;
      case Uop::ZP_RMW_RLA: ZP_RMW_RLA(); break;
      case Uop::AbsIdx_Stage3_Y: AbsIdx_Stage3_Y(); break;
      case Uop::AbsIdx_Stage3_X: AbsIdx_Stage3_X(); break;
      case Uop::AbsIdx_READ_Stage4: AbsIdx_READ_Stage4(); break;
      case Uop::AbsIdx_Stage4: AbsIdx_Stage4(); break;
      case Uop::AbsIdx_Stage4_SHX: AbsIdx_Stage4_SHX(); break;
      case Uop::AbsIdx_Stage4_SHY: AbsIdx_Stage4_SHY(); break;
      case Uop::AbsIdx_Stage4_SHA: AbsIdx_Stage4_SHA(); break;
      case Uop::AbsIdx_Stage4_SHS: AbsIdx_Stage4_SHS(); break;
      case Uop::AbsIdx_WRITE_Stage5_STA: AbsIdx_WRITE_Stage5_STA(); break;
      case Uop::AbsIdx_WRITE_Stage5_SHY: AbsIdx_WRITE_Stage5_SHY(); break;
      case Uop::AbsIdx_WRITE_Stage5_SHX: AbsIdx_WRITE_Stage5_SHX(); break;
      case Uop::AbsIdx_WRITE_Stage5_SHA: AbsIdx_WRITE_Stage5_SHA(); break;
      case Uop::AbsIdx_WRITE_Stage5_SHS: AbsIdx_WRITE_Stage5_SHS(); break;
      case Uop::AbsIdx_RMW_Stage5: AbsIdx_RMW_Stage5(); break;
      case Uop::AbsIdx_RMW_Stage7: AbsIdx_RMW_Stage7(); break;
      case Uop::AbsIdx_RMW_Stage6_DEC: AbsIdx_RMW_Stage6_DEC(); break;
      case Uop::AbsIdx_RMW_Stage6_DCP: AbsIdx_RMW_Stage6_DCP(); break;
      case Uop::AbsIdx_RMW_Stage6_ISC: AbsIdx_RMW_Stage6_ISC(); break;
      case Uop::AbsIdx_RMW_Stage6_INC: AbsIdx_RMW_Stage6_INC(); break;
      case Uop::AbsIdx_RMW_Stage6_ROL: AbsIdx_RMW_Stage6_ROL(); break;
      case Uop::AbsIdx_RMW_Stage6_LSR: AbsIdx_RMW_Stage6_LSR(); break;
      case Uop::AbsIdx_RMW_Stage6_SLO: AbsIdx_RMW_Stage6_SLO(); break;
      case Uop::AbsIdx_RMW_Stage6_SRE: AbsIdx_RMW_Stage6_SRE(); break;
      case Uop::AbsIdx_RMW_Stage6_RRA: AbsIdx_RMW_Stage6_RRA(); break;
      case Uop::AbsIdx_RMW_Stage6_RLA: AbsIdx_RMW_Stage6_RLA(); break;
      case Uop::AbsIdx_RMW_Stage6_ASL: AbsIdx_RMW_Stage6_ASL(); break;
      case Uop::AbsIdx_RMW_Stage6_ROR: AbsIdx_RMW_Stage6_ROR(); break;
      case Uop::AbsIdx_READ_Stage5_LDA: AbsIdx_READ_Stage5_LDA(); break;
      case Uop::AbsIdx_READ_Stage5_LDX: AbsIdx_READ_Stage5_LDX(); break;
      case Uop::AbsIdx_READ_Stage5_LAX: AbsIdx_READ_Stage5_LAX(); break;
      case Uop::AbsIdx_READ_Stage5_LDY: AbsIdx_READ_Stage5_LDY(); break;
      case Uop::AbsIdx_READ_Stage5_ORA: AbsIdx_READ_Stage5_ORA(); break;
      case Uop::AbsIdx_READ_Stage5_NOP: AbsIdx_READ_Stage5_NOP(); break;
      case Uop::AbsIdx_READ_Stage5_CMP: AbsIdx_READ_Stage5_CMP(); break;
      case Uop::AbsIdx_READ_Stage5_SBC: AbsIdx_READ_Stage5_SBC(); break;
      case Uop::AbsIdx_READ_Stage5_ADC: AbsIdx_READ_Stage5_ADC(); break;
      case Uop::AbsIdx_READ_Stage5_EOR: AbsIdx_READ_Stage5_EOR(); break;
      case Uop::AbsIdx_READ_Stage5_AND: AbsIdx_READ_Stage5_AND(); break;
      case Uop::AbsIdx_READ_Stage5_ERROR: AbsIdx_READ_Stage5_ERROR(); break;
      case Uop::AbsInd_JMP_Stage4: AbsInd_JMP_Stage4(); break;
      case Uop::AbsInd_JMP_Stage5: AbsInd_JMP_Stage5(); break;
      case Uop::Abs_RMW_Stage4: Abs_RMW_Stage4(); break;
      case Uop::Abs_RMW_Stage5_INC: Abs_RMW_Stage5_INC(); break;
      case Uop::Abs_RMW_Stage5_DEC: Abs_RMW_Stage5_DEC(); break;
      case Uop::Abs_RMW_Stage5_DCP: Abs_RMW_Stage5_DCP(); break;
      case Uop::Abs_RMW_Stage5_ISC: Abs_RMW_Stage5_ISC(); break;
      case Uop::Abs_RMW_Stage5_ASL: Abs_RMW_Stage5_ASL(); break;
      case Uop::Abs_RMW_Stage5_ROR: Abs_RMW_Stage5_ROR(); break;
      case Uop::Abs_RMW_Stage5_SLO: Abs_RMW_Stage5_SLO(); break;
      case Uop::Abs_RMW_Stage5_RLA: Abs_RMW_Stage5_RLA(); break;
      case Uop::Abs_RMW_Stage5_SRE: Abs_RMW_Stage5_SRE(); break;
      case Uop::Abs_RMW_Stage5_RRA: Abs_RMW_Stage5_RRA(); break;
      case Uop::Abs_RMW_Stage5_ROL: Abs_RMW_Stage5_ROL(); break;
      case Uop::Abs_RMW_Stage5_LSR: Abs_RMW_Stage5_LSR(); break;
      case Uop::Abs_RMW_Stage6: Abs_RMW_Stage6(); break;
      case Uop::End_ISpecial: End_ISpecial(); break;
      case Uop::End_SuppressInterrupt: End_SuppressInterrupt(); break;
      case Uop::End: End(); break;
      case Uop::Jam: Jam(); break;
    }
  }

  void ExecuteOne()
  {
    // total cycles now increments every time a cycle is called to accurately count during RDY
    TotalExecutedCycles++;
    interrupt_pending |= Interrupted();
    rdy_freeze = false;

    //i tried making ExecuteOneRetry not re-entrant by having it set a flag instead, then exit from the call below, check the flag, and GOTO if it was flagged, but it wasnt faster
    ExecuteOneRetry();

    if (!rdy_freeze)
      mi++;

    if (Microcode[opcode][mi] == Uop::End)
    {
      address_bus = PC; // If the next cycle is the start of a new instruction, the address bus needs to be set to the PC now, so a DMC DMA's halt cycles don't use the wrong address bus value.
    }
  }

  

  bool AtInstructionStart() const { return Microcode[opcode][mi] >= Uop::End; }
};

} // namespace nesHawk
