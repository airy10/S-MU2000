// license:BSD-3-Clause
// copyright-holders:Juergen Buchmueller, R. Belmont
//
// MAME src/devices/cpu/sh/sh_dasm.h, written to match the sh_dasm.cpp that was
// already vendored in src/mame/cpu/.
//
// That .cpp came in with the SH-2 files but nothing included it: the build had
// no disassembler, and could not have one, because its header was not vendored
// and it includes MAME's emu.h, which this project replaces with the
// compatibility layer (doc/design.md:86). So the four things it wants from emu.h
// now live in compat/mamecompat.h - data_buffer, util::disasm_interface, the
// step flags and step_over_extra() - and this file declares the class the .cpp
// actually defines. **The 1199 lines of decode logic are untouched**, which is
// the point: the answer to "what does this instruction mean" should come from
// MAME, not from us.
//
// Why a disassembler at all: the PLG card speaks a Yamaha sysEx over SCI4
// (doc/plg-cards.md, section 4). The SCI4 trace says what went out on the wire;
// this says what the firmware did with what came back, which is the half that
// decides what a card has to answer.

#ifndef S_MU2000_MAME_CPU_SH_DASM_H
#define S_MU2000_MAME_CPU_SH_DASM_H

#pragma once

// S-MU2000: MAME's emu.h is replaced by the compatibility layer. data_buffer,
// util::disasm_interface and the step flags are in there.
//
// <ostream> because sh_dasm.cpp writes with `stream << "NOP"` and MAME's emu.h
// is what used to drag the operator in. The using-declarations are the same
// story: MAME's emu.h puts util's names in scope, and the .cpp uses them
// unqualified.
#include <ostream>

#include "../../compat/mamecompat.h"

using util::STEP_COND;
using util::STEP_OUT;
using util::STEP_OVER;
using util::step_over_extra;

// SUPPORTED is MAME's "this opcode decoded" flag, ORed into the return value.
// Nothing here reads it - sh2dis prints text and steps by pc - so it only has to
// exist.
#define SUPPORTED (1u << 4)

class sh_disassembler : public util::disasm_interface
{
public:
	sh_disassembler(bool is_sh34);
	virtual ~sh_disassembler() = default;

	virtual u32 opcode_alignment() const override;
	virtual offs_t disassemble(std::ostream &stream, offs_t pc, const data_buffer &opcodes, const data_buffer &params) override;

	offs_t dasm_one(std::ostream &stream, offs_t pc, u16 opcode);

private:
	static const char *const regname[16];
	uint32_t op0000(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op0001(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op0010(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op0011(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op0100(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op0101(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op0110(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op0111(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op1000(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op1001(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op1010(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op1011(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op1100(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op1101(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op1110(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op1111(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op0000_sh34(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op0100_sh34(std::ostream &stream, uint32_t pc, uint16_t opcode);
	uint32_t op1111_sh34(std::ostream &stream, uint32_t pc, uint16_t opcode);

	bool m_is_sh34;
};

#endif // S_MU2000_MAME_CPU_SH_DASM_H
