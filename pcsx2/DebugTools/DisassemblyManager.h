/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2014  PCSX2 Dev Team
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

#pragma once

#include "SymbolMap.h"
#include "common/Threading.h"
#include "common/Pcsx2Types.h"
#include "DebugInterface.h"
#include "MIPSAnalyst.h"

enum DisassemblyLineType { DISTYPE_OPCODE, DISTYPE_MACRO, DISTYPE_DATA, DISTYPE_OTHER };

struct DisassemblyLineInfo
{
	DisassemblyLineType type;
	MIPSAnalyst::MipsOpcodeInfo info;
	std::string name;
	std::string params;
	u32 totalSize;

	DisassemblyLineInfo() : type(DISTYPE_OTHER), info(), name(), params(), totalSize() {}
};

enum LineType { LINE_UP, LINE_DOWN };

struct BranchLine
{
	u32 first;
	u32 second;
	LineType type;
	int laneIndex;

	bool operator<(const BranchLine& other) const
	{
		return first < other.first;
	}
};

class DisassemblyEntry
{
public:
	virtual ~DisassemblyEntry() { };
	virtual void recheck() = 0;
	virtual int getNumLines() = 0;
	virtual int getLineNum(u32 address, bool findStart) = 0;
	virtual u32 getLineAddress(int line) = 0;
	virtual u32 getTotalSize() = 0;
	virtual bool disassemble(u32 address, DisassemblyLineInfo& dest, bool insertSymbols) = 0;
	virtual void getBranchLines(u32 start, u32 size, std::vector<BranchLine>& dest) { };
};

class DisassemblyFunction: public DisassemblyEntry
{
public:
	DisassemblyFunction(DebugInterface* _cpu, u32 _address, u32 _size);
	virtual void recheck();
	virtual int getNumLines();
	virtual int getLineNum(u32 address, bool findStart);
	virtual u32 getLineAddress(int line);
	virtual u32 getTotalSize() { return size; };
	virtual bool disassemble(u32 address, DisassemblyLineInfo& dest, bool insertSymbols);
	virtual void getBranchLines(u32 start, u32 size, std::vector<BranchLine>& dest);
private:
	void generateBranchLines();
	void load();
	void clear();
	void addOpcodeSequence(u32 start, u32 end);

	DebugInterface* cpu;
	u32 address;
	u32 size;
	u32 hash;
	std::vector<BranchLine> lines;
	std::map<u32,DisassemblyEntry*> entries;
	std::vector<u32> lineAddresses;
};

class DisassemblyOpcode: public DisassemblyEntry
{
public:
	DisassemblyOpcode(DebugInterface* _cpu, u32 _address, int _num): cpu(_cpu), address(_address), num(_num) { };
	virtual ~DisassemblyOpcode() { };
	virtual void recheck() { };
	virtual int getNumLines() { return num; };
	virtual int getLineNum(u32 address, bool findStart) { return (address-this->address)/4; };
	virtual u32 getLineAddress(int line) { return address+line*4; };
	virtual u32 getTotalSize() { return num*4; };
	virtual bool disassemble(u32 address, DisassemblyLineInfo& dest, bool insertSymbols);
	virtual void getBranchLines(u32 start, u32 size, std::vector<BranchLine>& dest);
private:
	DebugInterface* cpu;
	u32 address;
	int num;
};


class DisassemblyMacro: public DisassemblyEntry
{
public:
	DisassemblyMacro(DebugInterface* _cpu, u32 _address):
		cpu(_cpu), type(MACRO_LI), name(), immediate(0), address(_address), numOpcodes(0), rt(0), dataSize(0) { };
	virtual ~DisassemblyMacro() { };

	void setMacroLi(u32 _immediate, u8 _rt);
	void setMacroMemory(const std::string& _name, u32 _immediate, u8 _rt, int _dataSize);

	virtual void recheck() { };
	virtual int getNumLines() { return 1; };
	virtual int getLineNum(u32 address, bool findStart) { return 0; };
	virtual u32 getLineAddress(int line) { return address; };
	virtual u32 getTotalSize() { return numOpcodes*4; };
	virtual bool disassemble(u32 address, DisassemblyLineInfo& dest, bool insertSymbols) ;
private:
	enum MacroType { MACRO_LI, MACRO_MEMORYIMM };

	DebugInterface* cpu;
	MacroType type;
	std::string name;
	u32 immediate;
	u32 address;
	u32 numOpcodes;
	u8 rt;
	int dataSize;
};


class DisassemblyData: public DisassemblyEntry
{
public:
	DisassemblyData(DebugInterface* _cpu, u32 _address, u32 _size, DataType _type);
	virtual ~DisassemblyData() { };

	virtual void recheck();
	virtual int getNumLines() { return (int)lines.size(); };
	virtual int getLineNum(u32 address, bool findStart);
	virtual u32 getLineAddress(int line) { return lineAddresses[line]; };
	virtual u32 getTotalSize() { return size; };
	virtual bool disassemble(u32 address, DisassemblyLineInfo& dest, bool insertSymbols);
private:
	void createLines();

	struct DataEntry
	{
		std::string text;
		u32 size;
		int lineNum;
	};

	DebugInterface* cpu;
	u32 address;
	u32 size;
	u32 hash;
	DataType type;
	std::map<u32,DataEntry> lines;
	std::vector<u32> lineAddresses;
};

class DisassemblyComment: public DisassemblyEntry
{
public:
	DisassemblyComment(DebugInterface* _cpu, u32 _address, u32 _size,const std::string& name, const std::string& param);
	virtual ~DisassemblyComment() { };

	virtual void recheck() { };
	virtual int getNumLines() { return 1; };
	virtual int getLineNum(u32 address, bool findStart) { return 0; };
	virtual u32 getLineAddress(int line) { return address; };
	virtual u32 getTotalSize() { return size; };
	virtual bool disassemble(u32 address, DisassemblyLineInfo& dest, bool insertSymbols);
private:
	[[maybe_unused]]DebugInterface* cpu;
	u32 address;
	u32 size;
	std::string name;
	std::string param;
};

class DebugInterface;

// Índice de cross-reference (quem chama cada função), usado pela árvore de
// call-tree do menu de contexto do disassembler (botão direito > "Show call
// tree"). Escaneia todas as funções ativas do SymbolMap uma vez, procurando
// JAL/JALR diretos (isLinkedBranch && !isBranchToRegister -- JALR fica de fora
// porque o alvo é dinâmico/registrador, não estático) e guarda, por endereço
// de função alvo, a lista de endereços das instruções de call que apontam pra
// ela. Resultado fica em cache até Invalidate() ser chamado; a UI é
// responsável por chamar Invalidate() sempre que memória executável for
// escrita (NOP, patch de instrução, restore) ou o SymbolMap mudar de geração,
// senão a árvore mostra call sites que não existem mais (ou some novos).
class CallGraphIndex
{
public:
	// Retorna os endereços das instruções que fazem JAL para funcAddr.
	// Constrói o índice inteiro na primeira chamada após Invalidate()/criação.
	const std::vector<u32>& GetCallers(DebugInterface* cpu, u32 funcAddr);
	void Invalidate() { built = false; callers.clear(); }
	bool IsBuilt() const { return built; }

private:
	void Build(DebugInterface* cpu);

	std::map<u32, std::vector<u32>> callers;
	bool built = false;
	static const std::vector<u32> empty;
};

class DisassemblyManager
{
public:
	void clear();

	void setCpu(DebugInterface* _cpu) { cpu = _cpu; lastKnownSymGeneration = 0; clear(); };
	void setMaxParamChars(int num) { maxParamChars = num; clear(); };
	void getLine(u32 address, bool insertSymbols, DisassemblyLineInfo& dest);
	void analyze(u32 address, u32 size);
	std::vector<BranchLine> getBranchLines(u32 start, u32 size);

	u32 getStartAddress(u32 address);
	u32 getNthPreviousAddress(u32 address, int n = 1);
	u32 getNthNextAddress(u32 address, int n = 1);

	// Ver CallGraphIndex acima. Chamar InvalidateCallGraph() em todo ponto que
	// escreve na memória de instruções (NOP, patch, restore) antes de reabrir a
	// árvore de call-tree.
	CallGraphIndex& GetCallGraph() { return callGraph; }
	void InvalidateCallGraph() { callGraph.Invalidate(); }

	static int getMaxParamChars() { return maxParamChars; };
private:
	DisassemblyEntry* getEntry(u32 address);
	std::map<u32,DisassemblyEntry*> entries;
	DebugInterface* cpu = NULL;
	static int maxParamChars;

	// Gatilho 1 (render): SymbolMap::GetGeneration() é incrementado toda vez que a
	// estrutura de símbolos muda (função renomeada/redimensionada, label
	// adicionada/removida, módulo trocado, .sym recarregado -- ver UpdateActiveSymbols
	// em SymbolMap.cpp). getLine() compara essa geração a cada linha renderizada; um
	// simples "u32 !=" é barato o bastante pra rodar em toda chamada sem throttle.
	// Quando diverge, o cache de 'entries' inteiro é invalidado (clear()) e
	// reconstruído sob demanda via analyze(), então funções/labels/tipos de dado
	// novos aparecem já no próximo repaint, não só quando o usuário rola até uma
	// área ainda não visitada.
	u32 lastKnownSymGeneration = 0;

	CallGraphIndex callGraph;
};

bool isInInterval(u32 start, u32 size, u32 value);
