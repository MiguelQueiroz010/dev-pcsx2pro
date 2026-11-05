/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"

#include "Common.h"
#include "iR5900.h"
#include "R5900OpcodeTables.h"

#include "DebugTools/DebugInterface.h"
#include "DebugTools/MipsStackWalk.h"
#include "DebugTools/SymbolMap.h"

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

using namespace x86Emitter;

namespace R5900 {
namespace Dynarec {

// R5900 branch helper!
// Recompiles code for a branch test and/or skip, complete with delay slot
// handling.  Note, for "likely" branches use iDoBranchImm_Likely instead, which
// handles delay slots differently.
// Parameters:
//   jmpSkip - This parameter is the result of the appropriate J32 instruction
//   (usually JZ32 or JNZ32).
void recDoBranchImm(u32* jmpSkip, bool isLikely)
{
	// All R5900 branches use this format:
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;

	// First up is the Branch Taken Path : Save the recompiler's state, compile the
	// DelaySlot, and issue a BranchTest insertion.  The state is reloaded below for
	// the "did not branch" path (maintains consts, register allocations, and other optimizations).

	SaveBranchState();
	recompileNextInstruction(1);
	SetBranchImm(branchTo);

	// Jump target when the branch is *not* taken, skips the branchtest code
	// insertion above.
	x86SetJ32(jmpSkip);

	// if it's a likely branch then we'll need to skip the delay slot here, since
	// MIPS cancels the delay slot instruction when branches aren't taken.
	LoadBranchState();
	if (!isLikely)
	{
		pc -= 4; // instruction rewinder for delay slot, if non-likely.
		recompileNextInstruction(1);
	}
	SetBranchImm(pc); // start a new recompiled block.
}

void recDoBranchImm_Likely(u32* jmpSkip)
{
	recDoBranchImm(jmpSkip, true);
}

namespace OpcodeImpl {

////////////////////////////////////////////////////
//static void recCACHE() {
//	xMOV(ptr32[&cpuRegs.code], cpuRegs.code );
//	xMOV(ptr32[&cpuRegs.pc], pc );
//	iFlushCall(FLUSH_EVERYTHING);
//	xFastCall((void*)(uptr)CACHE );
//	//branch = 2;
//
//	xCMP(ptr32[(u32*)((int)&cpuRegs.pc)], pc);
//	j8Ptr[0] = JE8(0);
//	xRET();
//	x86SetJ8(j8Ptr[0]);
//}


void recPREF()
{
}

void recSYNC()
{
}

void recMFSA()
{
	int mmreg;
	if (!_Rd_)
		return;

	mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rd_, MODE_WRITE);
	if (mmreg >= 0)
	{
		xMOVL.PS(xRegisterSSE(mmreg), ptr[&cpuRegs.sa]);
	}
	else
	{
		xMOV(eax, ptr[&cpuRegs.sa]);
		_deleteEEreg(_Rd_, 0);
		xMOV(ptr[&cpuRegs.GPR.r[_Rd_].UL[0]], eax);
		xMOV(ptr32[&cpuRegs.GPR.r[_Rd_].UL[1]], 0);
	}
}

// SA is 4-bit and contains the amount of bytes to shift
void recMTSA()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(ptr32[&cpuRegs.sa], g_cpuConstRegs[_Rs_].UL[0] & 0xf);
	}
	else
	{
		int mmreg;

		if ((mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ)) >= 0)
		{
			xMOVSS(ptr[&cpuRegs.sa], xRegisterSSE(mmreg));
		}
		else
		{
			xMOV(eax, ptr[&cpuRegs.GPR.r[_Rs_].UL[0]]);
			xMOV(ptr[&cpuRegs.sa], eax);
		}
		xAND(ptr32[&cpuRegs.sa], 0xf);
	}
}

void recMTSAB()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(ptr32[&cpuRegs.sa], ((g_cpuConstRegs[_Rs_].UL[0] & 0xF) ^ (_Imm_ & 0xF)));
	}
	else
	{
		_eeMoveGPRtoR(eax, _Rs_);
		xAND(eax, 0xF);
		xXOR(eax, _Imm_ & 0xf);
		xMOV(ptr[&cpuRegs.sa], eax);
	}
}

void recMTSAH()
{
	if (GPR_IS_CONST1(_Rs_))
	{
		xMOV(ptr32[&cpuRegs.sa], ((g_cpuConstRegs[_Rs_].UL[0] & 0x7) ^ (_Imm_ & 0x7)) << 1);
	}
	else
	{
		_eeMoveGPRtoR(eax, _Rs_);
		xAND(eax, 0x7);
		xXOR(eax, _Imm_ & 0x7);
		xSHL(eax, 1);
		xMOV(ptr[&cpuRegs.sa], eax);
	}
}

////////////////////////////////////////////////////
// Relatorio detalhado de opcode desconhecido/nao implementado.
//
// IMPORTANTE: recUnknown & cia rodam em TEMPO DE RECOMPILACAO (nao emitem codigo), ou
// seja, quando o bloco que contem a instrucao ruim esta sendo compilado -- o que acontece
// logo que a execucao chega nele. Por isso cpuRegs.GPR (ra, sp, a0..., etc.) ainda tem os
// valores da ENTRADA do bloco (cpuRegs.pc = inicio do bloco), que sao confiaveis.
// O "pc" global do recompilador aponta para a instrucao seguinte (ou para o branch, quando
// estamos no delay slot), entao o endereco real da instrucao ruim eh derivado dele.
namespace {

const char* const s_gprNames[32] = {
	"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};

std::string UnkSymName(u32 addr)
{
	std::string out = R5900SymbolMap.GetDescription(addr);
	const u32 start = R5900SymbolMap.GetFunctionStart(addr);
	char tmp[48];
	if (start != SymbolMap::INVALID_ADDRESS && start != addr)
	{
		snprintf(tmp, sizeof(tmp), "+0x%x", addr - start);
		out += tmp;
	}
	const int mi = R5900SymbolMap.GetModuleIndex(addr);
	out += " [" + R5900SymbolMap.GetModuleName(mi < 0 ? 0 : mi) + "]";
	return out;
}

std::string UnkDisasm(u32 addr)
{
	if (!r5900Debug.isValidAddress(addr))
		return "<endereco invalido>";
	return r5900Debug.disasm(addr, false);
}

// retAddr eh um possivel endereco de retorno? (instrucao em retAddr-8 eh jal/jalr)
bool UnkIsCallSite(u32 retAddr, std::string& what)
{
	if ((retAddr & 3) || retAddr < 0x00100000 || retAddr >= 0x02000000)
		return false;
	const u32 callPc = retAddr - 8;
	if (!r5900Debug.isValidAddress(callPc))
		return false;
	const u32 op = r5900Debug.read32(callPc);
	char tmp[96];
	if ((op >> 26) == 3) // JAL
	{
		const u32 target = ((callPc + 4) & 0xF0000000) | ((op & 0x03FFFFFF) << 2);
		snprintf(tmp, sizeof(tmp), "jal %08x ", target);
		what = tmp + UnkSymName(target);
		return true;
	}
	if ((op >> 26) == 0 && (op & 0x3F) == 9) // JALR
	{
		snprintf(tmp, sizeof(tmp), "jalr $%s (alvo indireto)", s_gprNames[(op >> 21) & 31]);
		what = tmp;
		return true;
	}
	return false;
}

void ReportUnknownOp(const char* kind)
{
	static std::unordered_set<u64> s_seen;
	static u32 s_reports = 0, s_suppressed = 0;
	const u32 maxReports = 64;

	const u32 code = cpuRegs.code;

	// Endereco real da instrucao ruim. Confere o conteudo da memoria para nao errar o -4/+4.
	u32 badPc = g_recompilingDelaySlot ? pc + 4 : pc - 4;
	const u32 cands[4] = {badPc, pc - 4, pc, pc + 4};
	for (u32 c : cands)
	{
		if (r5900Debug.isValidAddress(c) && r5900Debug.read32(c) == code)
		{
			badPc = c;
			break;
		}
	}

	const u64 key = ((u64)badPc << 32) | code;
	if (!s_seen.insert(key).second)
		return; // mesmo pc+opcode ja reportado

	// Falso positivo classico: o recompilador NAO encerra o bloco em SYSCALL (recSYSCALL tem
	// "//g_branch = 2"), entao continua compilando os nops/dados que ficam depois do stub
	// (ex.: li v1,0x23 ; syscall  == ExitThread em 0x100180). Em tempo de execucao o syscall
	// muda o pc e esse "codigo" nunca roda. Se so ha nops entre um SYSCALL e a instrucao
	// ruim, registra 1 linha e nao polui o log com o relatorio completo.
	{
		u32 a = badPc;
		for (int n = 0; n < 256 && a >= 4; n++)
		{
			a -= 4;
			if (!r5900Debug.isValidAddress(a))
				break;
			const u32 w = r5900Debug.read32(a);
			if (w == 0)
				continue; // nop
			if ((w & 0xFC00003F) == 0x0000000C) // SYSCALL
			{
				Console.WriteLn("EE: [UnkOp] %s op %08x em %08x ignorado (benigno): compile-ahead depois do SYSCALL em %08x %s",
					kind, code, badPc, a, UnkSymName(a).c_str());
				return;
			}
			break;
		}
	}

	if (s_reports >= maxReports)
	{
		if ((++s_suppressed % 256) == 1)
			Console.Error("EE: [UnkOp] relatorios suprimidos: %u (limite de %u atingido)", s_suppressed, maxReports);
		return;
	}
	s_reports++;

	const u32 blockPc = cpuRegs.pc;
	const u32 ra = cpuRegs.GPR.n.ra.UL[0];
	const u32 sp = cpuRegs.GPR.n.sp.UL[0];

	Console.Error("EE: ======== [UnkOp #%u] %s op %08x ========", s_reports, kind, code);
	Console.Error("EE: [UnkOp] instrucao ruim em %08x  %s", badPc, UnkSymName(badPc).c_str());
	Console.Error("EE: [UnkOp] bloco compilado a partir de %08x  %s%s", blockPc, UnkSymName(blockPc).c_str(),
		g_recompilingDelaySlot ? "  (instrucao esta num DELAY SLOT)" : "");
	Console.Error("EE: [UnkOp] ra=%08x %s", ra, UnkSymName(ra).c_str());
	Console.Error("EE: [UnkOp] sp=%08x  cycle=%u", sp, cpuRegs.cycle);

	// Registradores (32 bits baixos) na entrada do bloco.
	for (int r = 0; r < 32; r += 4)
	{
		Console.Error("EE: [UnkOp] %-2s=%08x %-2s=%08x %-2s=%08x %-2s=%08x", s_gprNames[r], cpuRegs.GPR.r[r].UL[0],
			s_gprNames[r + 1], cpuRegs.GPR.r[r + 1].UL[0], s_gprNames[r + 2], cpuRegs.GPR.r[r + 2].UL[0],
			s_gprNames[r + 3], cpuRegs.GPR.r[r + 3].UL[0]);
	}

	// Contexto: 8 instrucoes antes e 3 depois. '>>' marca a instrucao ruim.
	Console.Error("EE: [UnkOp] --- memoria ao redor de %08x ---", badPc);
	for (int i = -8; i <= 3; i++)
	{
		const u32 a = badPc + i * 4;
		if (!r5900Debug.isValidAddress(a))
			continue;
		Console.Error("EE: [UnkOp] %s %08x: %08x  %s", i == 0 ? ">>" : "  ", a, r5900Debug.read32(a),
			UnkDisasm(a).c_str());
	}

	// 1) Stack walk "oficial" (mesmo do debugger), com pelo menos 8 frames quando possivel.
	if (r5900Debug.isValidAddress(badPc) && r5900Debug.isValidAddress(sp))
	{
		std::vector<MipsStackWalk::StackFrame> frames = MipsStackWalk::Walk(&r5900Debug, badPc, ra, sp, 0, 0);
		Console.Error("EE: [UnkOp] --- call stack (stack walk, %u frames, max 12) ---", (u32)frames.size());
		for (size_t i = 0; i < frames.size() && i < 12; i++)
		{
			Console.Error("EE: [UnkOp]  #%u pc=%08x entry=%08x sp=%08x frame=0x%x  %s", (u32)i, frames[i].pc,
				frames[i].entry, frames[i].sp, frames[i].stackSize, UnkSymName(frames[i].pc).c_str());
		}
	}

	// 2) Varredura heuristica da stack: palavras que parecem enderecos de retorno
	// (instrucao em addr-8 eh jal/jalr). Funciona mesmo quando o pc esta em dados e o
	// stack walk nao acha prologo de funcao.
	if (r5900Debug.isValidAddress(sp))
	{
		Console.Error("EE: [UnkOp] --- possiveis enderecos de retorno na stack (varredura heuristica) ---");
		int found = 0;
		for (u32 i = 0; i < 256 && found < 16; i++)
		{
			const u32 slot = sp + i * 4;
			if (!r5900Debug.isValidAddress(slot))
				break;
			const u32 val = r5900Debug.read32(slot);
			std::string what;
			if (UnkIsCallSite(val, what))
			{
				Console.Error("EE: [UnkOp]  sp+0x%03x: ret=%08x %s  <= %s", i * 4, val, UnkSymName(val).c_str(),
					what.c_str());
				found++;
			}
		}
		if (!found)
			Console.Error("EE: [UnkOp]  (nenhum candidato nos primeiros 1KB da stack)");
	}
	Console.Error("EE: ======== [UnkOp #%u] fim ========", s_reports);
}

} // namespace

////////////////////////////////////////////////////
void recNULL()
{
	Console.Error("EE: Unimplemented op %x", cpuRegs.code);
	ReportUnknownOp("Unimplemented");
}

////////////////////////////////////////////////////
void recUnknown()
{
	// TODO : Unknown ops should throw an exception.
	Console.Error("EE: Unrecognized op %x", cpuRegs.code);
	ReportUnknownOp("Unrecognized");
}

void recMMI_Unknown()
{
	// TODO : Unknown ops should throw an exception.
	Console.Error("EE: Unrecognized MMI op %x", cpuRegs.code);
	ReportUnknownOp("Unrecognized MMI");
}

void recCOP0_Unknown()
{
	// TODO : Unknown ops should throw an exception.
	Console.Error("EE: Unrecognized COP0 op %x", cpuRegs.code);
	ReportUnknownOp("Unrecognized COP0");
}

void recCOP1_Unknown()
{
	// TODO : Unknown ops should throw an exception.
	Console.Error("EE: Unrecognized FPU/COP1 op %x", cpuRegs.code);
	ReportUnknownOp("Unrecognized FPU/COP1");
}

/**********************************************************
*    UNHANDLED YET OPCODES
*
**********************************************************/

// Suikoden 3 uses it a lot
void recCACHE() //Interpreter only!
{
	//xMOV(ptr32[&cpuRegs.code], (u32)cpuRegs.code );
	//xMOV(ptr32[&cpuRegs.pc], (u32)pc );
	//iFlushCall(FLUSH_EVERYTHING);
	//xFastCall((void*)(uptr)R5900::Interpreter::OpcodeImpl::CACHE );
	//branch = 2;
}

void recTGE()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TGE);
}

void recTGEU()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TGEU);
}

void recTLT()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TLT);
}

void recTLTU()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TLTU);
}

void recTEQ()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TEQ);
}

void recTNE()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TNE);
}

void recTGEI()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TGEI);
}

void recTGEIU()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TGEIU);
}

void recTLTI()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TLTI);
}

void recTLTIU()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TLTIU);
}

void recTEQI()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TEQI);
}

void recTNEI()
{
	recBranchCall(R5900::Interpreter::OpcodeImpl::TNEI);
}

} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900
