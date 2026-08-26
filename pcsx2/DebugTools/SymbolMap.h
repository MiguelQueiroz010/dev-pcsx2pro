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

#include <vector>
#include <set>
#include <map>
#include <string>
#include <mutex>

#include "common/Pcsx2Types.h"

enum SymbolType {
	ST_NONE     = 0,
	ST_FUNCTION = 1,
	ST_DATA     = 2,
	// Labels "soltos" (endereços nomeados via AddLabel que não são o início de
	// uma função nem um bloco de dados -- ex: destino de um branch nomeado
	// manualmente num .sym). Note que AddFunction() também grava um AddLabel()
	// espelhado no mesmo endereço (ver AddFunction), então GetAllSymbols(ST_LABEL)
	// devolve tanto os labels soltos quanto esses espelhos; quem consome o
	// resultado e quer só os soltos precisa filtrar comparando com
	// GetFunctionStart(label.address) == label.address.
	ST_LABEL    = 4,
	ST_ALL      = 7,
};

struct SymbolInfo {
	SymbolType type;
	u32 address;
	u32 size;
};

struct SymbolEntry {
	std::string name;
	u32 address;
	u32 size;
	// Índice do módulo dono do símbolo, copiado direto de FunctionEntry::module /
	// DataEntry::module (não re-derivado por endereço, que é impreciso perto dos
	// limites de módulo). 0 = ELF base.
	int moduleIndex = 0;
};

struct LoadedModuleInfo {
	std::string name;
	u32 address;
	u32 size;
	bool active;
};

enum DataType {
	DATATYPE_NONE, DATATYPE_BYTE, DATATYPE_HALFWORD, DATATYPE_WORD, DATATYPE_ASCII
};

class SymbolMap {
public:
	SymbolMap() {}
	void Clear();
	void SortSymbols();

	bool LoadNocashSym(const char *filename, u32 baseOffset = 0);

	SymbolType GetSymbolType(u32 address) const;
	bool GetSymbolInfo(SymbolInfo *info, u32 address, SymbolType symmask = ST_FUNCTION) const;
	u32 GetNextSymbolAddress(u32 address, SymbolType symmask);
	std::string GetDescription(unsigned int address) const;
	std::vector<SymbolEntry> GetAllSymbols(SymbolType symmask);

	void AddModule(const char *name, u32 address, u32 size);
	// Deactivates whatever module currently occupies the (address+size) end-address
	// slot, regardless of which module that is. Safe only when it's guaranteed no
	// other module shares that same end address (e.g. a single, non-overlaid module).
	void UnloadModule(u32 address, u32 size);
	// Identity-safe variant: only deactivates the slot if the module CURRENTLY active
	// there is actually named `name`. Use this whenever multiple modules can share the
	// same address range (e.g. game overlays swapped in/out at the same memory
	// location) -- otherwise deactivating an inactive region can accidentally erase a
	// different, legitimately active region that happens to end at the same address.
	void UnloadModule(const char *name, u32 address, u32 size);
	u32 GetModuleRelativeAddr(u32 address, int moduleIndex = -1) const;
	u32 GetModuleAbsoluteAddr(u32 relative, int moduleIndex) const;
	int GetModuleIndex(u32 address) const;
	// Looks up a module's index by name regardless of whether it's currently active.
	// Returns -1 if no module with that name has ever been added. Used to find the
	// module to purge via ClearModuleSymbols() before reparsing an updated SYM file.
	int GetModuleIndexByName(const char *name) const;
	bool IsModuleActive(int moduleIndex) const;
	std::vector<LoadedModuleInfo> getAllModules() const;
	// Nome de exibição do módulo dono de um símbolo, para agrupamento na UI
	// (Functions: Group by Region). moduleIndex 0 (ou qualquer índice sem módulo
	// registrado) volta "ELF"; caso contrário o nome do módulo/overlay cadastrado
	// via AddModule, mesmo se esse módulo estiver inativo no momento.
	std::string GetModuleName(int moduleIndex) const;

	// Removes every function/label/data entry belonging to moduleIndex (from both the
	// backing functions/labels/data maps and, after an internal UpdateActiveSymbols(),
	// the active* views). Needed before reloading a module's SYM file: AddFunction/
	// AddLabel/AddData intentionally never overwrite an existing entry, so simply
	// re-running LoadNocashSym on a changed or swapped-out overlay would leave stale
	// symbols (renamed, resized, or removed in the new file) sitting there forever.
	// Refuses to act on moduleIndex <= 0, since that range is used for global/
	// non-module symbols shared by everything.
	void ClearModuleSymbols(int moduleIndex);

	void AddFunction(const char* name, u32 address, u32 size, int moduleIndex = -1);
	u32 GetFunctionStart(u32 address) const;
	int GetFunctionNum(u32 address) const;
	u32 GetFunctionSize(u32 startAddress) const;
	bool SetFunctionSize(u32 startAddress, u32 newSize);
	bool RemoveFunction(u32 startAddress, bool removeName);

	void AddLabel(const char* name, u32 address, int moduleIndex = -1);
	std::string GetLabelString(u32 address) const;
	void SetLabelName(const char* name, u32 address, bool updateImmediately = true);
	bool GetLabelValue(const char* name, u32& dest);

	void AddData(u32 address, u32 size, DataType type, int moduleIndex = -1);
	u32 GetDataStart(u32 address) const;
	u32 GetDataSize(u32 startAddress) const;
	DataType GetDataType(u32 startAddress) const;

	static const u32 INVALID_ADDRESS = (u32)-1;

	void UpdateActiveSymbols();
	bool IsEmpty() const { return activeFunctions.empty() && activeLabels.empty() && activeData.empty(); };

	// Bumped by UpdateActiveSymbols() every time it runs -- which is the single
	// choke point every structural change (AddModule, UnloadModule, ClearModuleSymbols,
	// SetFunctionSize, RemoveFunction, a freshly reloaded SYM file) already funnels
	// through -- and also by Clear() directly, since that one resets everything
	// without going through UpdateActiveSymbols(). Cheap (u32 compare) way for
	// callers like DisassemblyManager to detect "something in the symbol map
	// changed since I last looked" without re-parsing or re-hashing anything
	// themselves.
	u32 GetGeneration() const { return generation; }

	// When true, every SymbolMap mutation writes a detailed step-by-step trace via
	// Console.WriteLn/Console.Warning. Those calls already flow into PCSX2's on-disk
	// log (when console logging is enabled in the app), so toggling this on is enough
	// to get a saved, detailed trace of everything SymbolMap does. Off by default to
	// avoid spamming the console/log during normal play.
	static bool VerboseLogging;
private:
	void AssignFunctionIndices();
	const char *GetLabelName(u32 address) const;
	const char *GetLabelNameRel(u32 relAddress, int moduleIndex) const;

	struct FunctionEntry {
		u32 start;
		u32 size;
		int index;
		int module;
	};

	struct LabelEntry {
		u32 addr;
		int module;
		char name[128];
	};

	struct DataEntry {
		DataType type;
		u32 start;
		u32 size;
		int module;
	};

	struct ModuleEntry {
		// Note: this index is +1, 0 matches any for backwards-compat.
		int index;
		u32 start;
		u32 size;
		char name[128];
	};

	// These are flattened, read-only copies of the actual data in active modules only.
	std::map<u32, const FunctionEntry> activeFunctions;
	std::map<u32, const LabelEntry> activeLabels;
	std::map<u32, const DataEntry> activeData;

	// This is indexed by the end address of the module.
	std::map<u32, const ModuleEntry> activeModuleEnds;

	typedef std::pair<int, u32> SymbolKey;

	// These are indexed by the module id and relative address in the module.
	std::map<SymbolKey, FunctionEntry> functions;
	std::map<SymbolKey, LabelEntry> labels;
	std::map<SymbolKey, DataEntry> data;
	std::vector<ModuleEntry> modules;

	mutable std::recursive_mutex m_lock;

	// See GetGeneration(). Incremented at the top of UpdateActiveSymbols().
	u32 generation = 0;
};

extern SymbolMap R5900SymbolMap;
extern SymbolMap R3000SymbolMap;
