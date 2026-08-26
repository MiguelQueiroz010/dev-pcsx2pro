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

#include "PrecompiledHeader.h"

#include "SymbolMap.h"
#include <algorithm>

SymbolMap R5900SymbolMap;
SymbolMap R3000SymbolMap;

bool SymbolMap::VerboseLogging = false;

#ifdef _WIN32
#define strcasecmp stricmp
#endif

#define ARRAY_SIZE(x) (sizeof((x))/sizeof(*(x)))

// Detailed step trace, only emitted when SymbolMap::VerboseLogging is enabled.
// Console.WriteLn/Console.Warning already get mirrored into PCSX2's on-disk log
// when logging is enabled in the app, so this is all that's needed to persist a trace.
#define SM_LOG(...) do { if (SymbolMap::VerboseLogging) Console.WriteLn(__VA_ARGS__); } while (0)

void SymbolMap::SortSymbols() {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	AssignFunctionIndices();
}

void SymbolMap::Clear() {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	functions.clear();
	labels.clear();
	data.clear();
	activeFunctions.clear();
	activeLabels.clear();
	activeData.clear();
	activeModuleEnds.clear();
	modules.clear();

	// Clear() doesn't go through UpdateActiveSymbols(), so bump the generation
	// here too -- otherwise a full reset wouldn't be visible to callers like
	// DisassemblyManager that only compare GetGeneration().
	generation++;
}


bool SymbolMap::LoadNocashSym(const char *filename, u32 baseOffset) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	if (!filename || !filename[0]) {
		Console.Warning("[SymbolMap] LoadNocashSym: empty/null filename, aborting.");
		return false;
	}

	FILE *f = wxFopen(filename, "r");
	if (!f) {
		Console.Warning("[SymbolMap] LoadNocashSym: could not open '%s'.", filename);
		return false;
	}

	SM_LOG("[SymbolMap] LoadNocashSym: opened '%s' (baseOffset=0x%08X)", filename, baseOffset);

	int lineNum = 0;
	int parsedCount = 0;
	int skippedCount = 0;

	while (!feof(f)) {
		char line[256], value[256] = {0};
		char *p = fgets(line, 256, f);
		if (p == NULL)
			break;
		lineNum++;

		u32 address;
		if (sscanf(line, "%08X %255s", &address, value) != 2) {
			skippedCount++;
			continue;
		}
		if (address == 0 && strcmp(value, "0") == 0)
			continue;

		address += baseOffset;

		if (value[0] == '.') {
			// data directives
			char* s = strchr(value, ':');
			if (s != NULL) {
				*s = 0;

				u32 size = 0;
				if (sscanf(s + 1, "%04X", &size) != 1) {
					skippedCount++;
					continue;
				}

				// CORREÇÃO: Trocado de 0 para -1 para atrelar os dados ao módulo ativo (Overlay) ao invés do escopo global
				if (strcasecmp(value, ".byt") == 0) {
					AddData(address, size, DATATYPE_BYTE, -1);
				} else if (strcasecmp(value, ".wrd") == 0) {
					AddData(address, size, DATATYPE_HALFWORD, -1);
				} else if (strcasecmp(value, ".dbl") == 0) {
					AddData(address, size, DATATYPE_WORD, -1);
				} else if (strcasecmp(value, ".asc") == 0) {
					AddData(address, size, DATATYPE_ASCII, -1);
				} else {
					skippedCount++;
					continue;
				}
				parsedCount++;
			} else {
				skippedCount++;
			}
		} else {				// labels
			int size = 1;
			char* seperator = strchr(value, ',');
			if (seperator != NULL) {
				*seperator = 0;
				if (sscanf(seperator + 1, "%08X", &size) != 1)
					size = 1;
			}

			// CORREÇÃO: Trocado de 0 para -1 para atrelar as funções e labels ao módulo ativo
			if (size != 1) {
				AddFunction(value, address, size, -1);
			} else {
				AddLabel(value, address, -1);
			}
			parsedCount++;
		}
	}

	fclose(f);
	Console.WriteLn("[SymbolMap] LoadNocashSym: '%s' done - %d line(s) read, %d entrie(s) parsed, %d skipped.",
		filename, lineNum, parsedCount, skippedCount);
	return true;
}

SymbolType SymbolMap::GetSymbolType(u32 address) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	if (activeFunctions.find(address) != activeFunctions.end())
		return ST_FUNCTION;
	if (activeData.find(address) != activeData.end())
		return ST_DATA;
	return ST_NONE;
}

bool SymbolMap::GetSymbolInfo(SymbolInfo *info, u32 address, SymbolType symmask) const {
	u32 functionAddress = INVALID_ADDRESS;
	u32 dataAddress = INVALID_ADDRESS;

	if (symmask & ST_FUNCTION)
		functionAddress = GetFunctionStart(address);

	if (symmask & ST_DATA)
		dataAddress = GetDataStart(address);

	if (functionAddress == INVALID_ADDRESS || dataAddress == INVALID_ADDRESS) {
		if (functionAddress != INVALID_ADDRESS) {
			if (info != NULL) {
				info->type = ST_FUNCTION;
				info->address = functionAddress;
				info->size = GetFunctionSize(functionAddress);
			}

			return true;
		}
		
		if (dataAddress != INVALID_ADDRESS) {
			if (info != NULL) {
				info->type = ST_DATA;
				info->address = dataAddress;
				info->size = GetDataSize(dataAddress);
			}

			return true;
		}

		return false;
	}

	// if both exist, return the function
	if (info != NULL) {
		info->type = ST_FUNCTION;
		info->address = functionAddress;
		info->size = GetFunctionSize(functionAddress);
	}

	return true;
}

u32 SymbolMap::GetNextSymbolAddress(u32 address, SymbolType symmask) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	const auto functionEntry = symmask & ST_FUNCTION ? activeFunctions.upper_bound(address) : activeFunctions.end();
	const auto dataEntry = symmask & ST_DATA ? activeData.upper_bound(address) : activeData.end();

	if (functionEntry == activeFunctions.end() && dataEntry == activeData.end())
		return INVALID_ADDRESS;

	u32 funcAddress = (functionEntry != activeFunctions.end()) ? functionEntry->first : 0xFFFFFFFF;
	u32 dataAddress = (dataEntry != activeData.end()) ? dataEntry->first : 0xFFFFFFFF;

	if (funcAddress <= dataAddress)
		return funcAddress;
	else
		return dataAddress;
}

std::string SymbolMap::GetDescription(unsigned int address) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	const char* labelName = NULL;

	u32 funcStart = GetFunctionStart(address);
	if (funcStart != INVALID_ADDRESS) {
		labelName = GetLabelName(funcStart);
	} else {
		u32 dataStart = GetDataStart(address);
		if (dataStart != INVALID_ADDRESS)
			labelName = GetLabelName(dataStart);
	}

	if (labelName != NULL)
		return labelName;

	char descriptionTemp[256];
	snprintf(descriptionTemp, sizeof(descriptionTemp), "(%08x)", address);
	return descriptionTemp;
}

std::vector<SymbolEntry> SymbolMap::GetAllSymbols(SymbolType symmask) {
	std::vector<SymbolEntry> result;

	if (symmask & ST_FUNCTION) {
		std::lock_guard<std::recursive_mutex> guard(m_lock);
		for (auto it = activeFunctions.begin(); it != activeFunctions.end(); it++) {
			SymbolEntry entry;
			entry.address = it->first;
			entry.size = GetFunctionSize(entry.address);
			entry.moduleIndex = it->second.module;
			const char* name = GetLabelName(entry.address);
			if (name != NULL)
				entry.name = name;
			result.push_back(entry);
		}
	}

	if (symmask & ST_DATA) {
		std::lock_guard<std::recursive_mutex> guard(m_lock);
		for (auto it = activeData.begin(); it != activeData.end(); it++) {
			SymbolEntry entry;
			entry.address = it->first;
			entry.size = GetDataSize(entry.address);
			entry.moduleIndex = it->second.module;
			const char* name = GetLabelName(entry.address);
			if (name != NULL)
				entry.name = name;
			result.push_back(entry);
		}
	}

	if (symmask & ST_LABEL) {
		// activeLabels nunca era exposto por GetAllSymbols antes disso -- era
		// por isso que labels soltos (tipo um branch target nomeado no .sym,
		// sem virar função) nunca apareciam em nenhuma UI que só consumia
		// GetAllSymbols(ST_FUNCTION). Devolve o LabelEntry cru: não tem
		// GetLabelName() aqui porque o nome JÁ é o que estamos lendo.
		std::lock_guard<std::recursive_mutex> guard(m_lock);
		for (auto it = activeLabels.begin(); it != activeLabels.end(); it++) {
			SymbolEntry entry;
			entry.address = it->first;
			entry.size = 0; // LabelEntry não guarda tamanho
			entry.moduleIndex = it->second.module;
			entry.name = it->second.name;
			result.push_back(entry);
		}
	}

	return result;
}

void SymbolMap::AddModule(const char *name, u32 address, u32 size) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	if (!name) {
		Console.Warning("[SymbolMap] AddModule: name is null, aborting.");
		return;
	}

	if (size == 0) {
		Console.Warning("[SymbolMap] AddModule: '%s' has size=0 (start=0x%08X), aborting.", name, address);
		return;
	}

	// Detect overflow of address+size before it's used as a map key/range.
	if (address + size < address) {
		Console.Warning("[SymbolMap] AddModule: '%s' range overflows u32 (start=0x%08X size=0x%X), aborting.", name, address, size);
		return;
	}

	const u32 endKey = address + size;

	// Whichever end-address slot this module is about to claim can currently be
	// held by a *different* module -- that's the whole point of overlays, several
	// named modules sharing the same address range and swapped in/out over time.
	// activeModuleEnds only ever has room for one resident at a given key, so
	// claiming this slot must evict whoever's there first. Doing this unconditionally,
	// before either branch below, matters because plain activeModuleEnds.insert()
	// never overwrites an existing key -- if this module is being added for the very
	// first time (not yet in `modules`) while a different, still-active module
	// occupies endKey, that insert() call would silently no-op: the new module would
	// never make it into activeModuleEnds, so it stays permanently invisible to
	// GetModuleIndex()/UpdateActiveSymbols() -- i.e. the old overlay's functions
	// would keep resolving and the new overlay's freshly-loaded SYM would never
	// actually become active, even though AddModule() and the SYM load both
	// "succeeded". This was the root cause of overlays not properly swapping out
	// each other's registered functions.
	auto evicted = activeModuleEnds.find(endKey);
	// Se o módulo que já está ativo nesse endKey é ESTE MESMO (por nome), guarda
	// esse fato antes do erase() abaixo destruir a informação -- é o que permite
	// distinguir "reativação de verdade" (módulo estava inativo, ou start/size
	// mudou) de "reaplicação redundante" (mesmo módulo, já ativo, exatamente com
	// os mesmos start/size de antes). GameDebuggerConfigManager::CheckAndReloadIfNeeded
	// chama AddModule() de novo a cada ~200ms (throttle) pra CADA região configurada,
	// mesmo quando nada mudou no disco -- sem essa checagem, UpdateActiveSymbols()
	// (e portanto SymbolMap::generation) subia nesse ritmo o tempo todo, o que por
	// sua vez fazia DisassemblyManager::getLine() invalidar seu cache de entries e
	// CpuTabPage::reloadSymbolMap() reconstruir a árvore da aba Functions -- um
	// "heartbeat" de mudança falsa a cada throttle tick, não uma mudança real.
	bool wasAlreadyActiveHereUnchanged = (evicted != activeModuleEnds.end() && !strcmp(evicted->second.name, name) &&
		evicted->second.start == address && evicted->second.size == size);
	if (evicted != activeModuleEnds.end() && strcmp(evicted->second.name, name) != 0)
		SM_LOG("[SymbolMap] AddModule: '%s' is evicting '%s' from end=0x%08X.", name, evicted->second.name, endKey);
	activeModuleEnds.erase(endKey);

	for (auto it = modules.begin(), end = modules.end(); it != end; ++it) {
		if (!strcmp(it->name, name)) {
			// Just reactivate that one.
			it->start = address;
			it->size = size;
			activeModuleEnds.insert(std::make_pair(endKey, *it));
			if (wasAlreadyActiveHereUnchanged) {
				SM_LOG("[SymbolMap] AddModule: '%s' start=0x%08X size=0x%X already active and unchanged, skipping UpdateActiveSymbols.", name, address, size);
			} else {
				SM_LOG("[SymbolMap] AddModule: reactivated '%s' start=0x%08X size=0x%X", name, address, size);
				UpdateActiveSymbols();
			}
			return;
		}
	}

	ModuleEntry mod;
	strncpy(mod.name, name, ARRAY_SIZE(mod.name));
	mod.name[ARRAY_SIZE(mod.name) - 1] = 0;
	mod.start = address;
	mod.size = size;
	mod.index = (int)modules.size() + 1;

	modules.push_back(mod);
	activeModuleEnds.insert(std::make_pair(endKey, mod));
	SM_LOG("[SymbolMap] AddModule: added new module '%s' index=%d start=0x%08X size=0x%X", name, mod.index, address, size);
	UpdateActiveSymbols();
}

void SymbolMap::UnloadModule(u32 address, u32 size) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	size_t erased = activeModuleEnds.erase(address + size);
	SM_LOG("[SymbolMap] UnloadModule: start=0x%08X size=0x%X (%s)", address, size, erased ? "removed" : "no matching active entry");
	UpdateActiveSymbols();
}

void SymbolMap::UnloadModule(const char *name, u32 address, u32 size) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	if (!name) {
		Console.Warning("[SymbolMap] UnloadModule: name is null, falling back to unsafe address-only unload.");
		UnloadModule(address, size);
		return;
	}

	u32 endKey = address + size;
	auto it = activeModuleEnds.find(endKey);
	if (it == activeModuleEnds.end()) {
		SM_LOG("[SymbolMap] UnloadModule('%s'): end=0x%08X has no active entry, nothing to do.", name, endKey);
		return;
	}

	if (strcmp(it->second.name, name) != 0) {
		// Another module -- typically a different overlay sharing this same address
		// range -- currently occupies this slot and is legitimately active. Do NOT
		// erase it just because *this* caller's region isn't the resident one; that
		// was the root cause of two overlays with the same range only ever showing
		// one set of symbols, since whichever got processed last would win the race.
		SM_LOG("[SymbolMap] UnloadModule('%s'): end=0x%08X is currently held by '%s' instead, leaving it active.",
			name, endKey, it->second.name);
		return;
	}

	activeModuleEnds.erase(it);
	SM_LOG("[SymbolMap] UnloadModule('%s'): start=0x%08X size=0x%X removed.", name, address, size);
	UpdateActiveSymbols();
}

u32 SymbolMap::GetModuleRelativeAddr(u32 address, int moduleIndex) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	if (moduleIndex == -1) {
		moduleIndex = GetModuleIndex(address);
	}

	for (auto it = modules.begin(), end = modules.end(); it != end; ++it) {
		if (it->index == moduleIndex) {
			return address - it->start;
		}
	}
	return address;
}

u32 SymbolMap::GetModuleAbsoluteAddr(u32 relative, int moduleIndex) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	for (auto it = modules.begin(), end = modules.end(); it != end; ++it) {
		if (it->index == moduleIndex) {
			return it->start + relative;
		}
	}
	return relative;
}

int SymbolMap::GetModuleIndex(u32 address) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	auto iter = activeModuleEnds.upper_bound(address);
	if (iter == activeModuleEnds.end())
		return -1;
	return iter->second.index;
}

int SymbolMap::GetModuleIndexByName(const char *name) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	if (!name)
		return -1;

	for (auto it = modules.begin(), end = modules.end(); it != end; ++it) {
		if (!strcmp(it->name, name))
			return it->index;
	}
	return -1;
}

bool SymbolMap::IsModuleActive(int moduleIndex) const {
	if (moduleIndex == 0) {
		return true;
	}

	std::lock_guard<std::recursive_mutex> guard(m_lock);
	for (auto it = activeModuleEnds.begin(), end = activeModuleEnds.end(); it != end; ++it) {
		if (it->second.index == moduleIndex) {
			return true;
		}
	}
	return false;
}

std::string SymbolMap::GetModuleName(int moduleIndex) const {
	if (moduleIndex == 0)
		return "ELF";

	std::lock_guard<std::recursive_mutex> guard(m_lock);
	for (auto it = modules.begin(), end = modules.end(); it != end; ++it) {
		if (it->index == moduleIndex)
			return it->name;
	}

	// Nenhum módulo cadastrado com esse índice (símbolo global/legado) -- trata
	// como ELF em vez de devolver uma string vazia pra UI.
	return "ELF";
}

std::vector<LoadedModuleInfo> SymbolMap::getAllModules() const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	std::vector<LoadedModuleInfo> result;
	for (size_t i = 0; i < modules.size(); i++) {
		LoadedModuleInfo m;
		m.name = modules[i].name;
		m.address = modules[i].start;
		m.size = modules[i].size;

		u32 key = modules[i].start + modules[i].size;
		m.active = activeModuleEnds.find(key) != activeModuleEnds.end();

		result.push_back(m);
	}

	return result;
}

void SymbolMap::AddFunction(const char* name, u32 address, u32 size, int moduleIndex) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	if (!name) {
		Console.Warning("[SymbolMap] AddFunction: name is null at address 0x%08X, using empty name instead of crashing.", address);
		name = "";
	}

	if (moduleIndex == -1) {
		moduleIndex = GetModuleIndex(address);
	}

	// Is there an existing one?
	u32 relAddress = GetModuleRelativeAddr(address, moduleIndex);
	auto symbolKey = std::make_pair(moduleIndex, relAddress);
	auto existing = functions.find(symbolKey);
	if (existing == functions.end()) {
		// Fall back: maybe it's got moduleIndex = 0.
		existing = functions.find(std::make_pair(0, address));
	}

	if (existing != functions.end()) {
		existing->second.size = size;
		if (existing->second.module != moduleIndex) {
			existing->second.start = relAddress;
			existing->second.module = moduleIndex;
		}

		// Refresh the active item if it exists.
		auto active = activeFunctions.find(address);
		if (active != activeFunctions.end() && active->second.module == moduleIndex) {
			activeFunctions.erase(active);
			activeFunctions.insert(std::make_pair(address, existing->second));
		}
	} else {
		FunctionEntry func;
		func.start = relAddress;
		func.size = size;
		func.index = (int)functions.size();
		func.module = moduleIndex;
		functions[symbolKey] = func;

		if (IsModuleActive(moduleIndex)) {
			activeFunctions.insert(std::make_pair(address, func));
		}
	}

	AddLabel(name, address, moduleIndex);
	SM_LOG("[SymbolMap] AddFunction: '%s' address=0x%08X size=0x%X module=%d", name, address, size, moduleIndex);
}

u32 SymbolMap::GetFunctionStart(u32 address) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	auto it = activeFunctions.upper_bound(address);
	if (it == activeFunctions.end()) {
		// check last element
		auto rit = activeFunctions.rbegin();
		if (rit != activeFunctions.rend()) {
			u32 start = rit->first;
			u32 size = rit->second.size;
			if (start <= address && start+size > address)
				return start;
		}
		// otherwise there's no function that contains this address
		return INVALID_ADDRESS;
	}

	if (it != activeFunctions.begin()) {
		it--;
		u32 start = it->first;
		u32 size = it->second.size;
		if (start <= address && start+size > address)
			return start;
	}

	return INVALID_ADDRESS;
}

u32 SymbolMap::GetFunctionSize(u32 startAddress) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	auto it = activeFunctions.find(startAddress);
	if (it == activeFunctions.end())
		return INVALID_ADDRESS;

	return it->second.size;
}

int SymbolMap::GetFunctionNum(u32 address) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	u32 start = GetFunctionStart(address);
	if (start == INVALID_ADDRESS)
		return INVALID_ADDRESS;

	auto it = activeFunctions.find(start);
	if (it == activeFunctions.end())
		return INVALID_ADDRESS;

	return it->second.index;
}

void SymbolMap::AssignFunctionIndices() {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	int index = 0;
	for (auto mod = activeModuleEnds.begin(), modend = activeModuleEnds.end(); mod != modend; ++mod) {
		int moduleIndex = mod->second.index;
		auto begin = functions.lower_bound(std::make_pair(moduleIndex, 0));
		auto end = functions.upper_bound(std::make_pair(moduleIndex, 0xFFFFFFFF));
		for (auto it = begin; it != end; ++it) {
			it->second.index = index++;
		}
	}
}

void SymbolMap::UpdateActiveSymbols() {
	// return;   (slow in debug mode)
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	// Every structural change (module add/remove, symbol clear/reload, function
	// resize, function removal) already calls UpdateActiveSymbols() as its last
	// step -- see AddModule/UnloadModule/ClearModuleSymbols/SetFunctionSize/
	// RemoveFunction, plus GameDebuggerConfigManager after a successful
	// LoadNocashSym. Bumping the generation counter here, in this one place,
	// means every one of those call sites is covered automatically without
	// having to remember to bump it individually at each of them.
	generation++;

	std::map<int, u32> activeModuleIndexes;
	for (auto it = activeModuleEnds.begin(), end = activeModuleEnds.end(); it != end; ++it) {
		activeModuleIndexes[it->second.index] = it->second.start;
	}

	activeFunctions.clear();
	activeLabels.clear();
	activeData.clear();

	for (auto it = functions.begin(), end = functions.end(); it != end; ++it) {
		const auto mod = activeModuleIndexes.find(it->second.module);
		if (it->second.module <= 0) {
			activeFunctions.insert(std::make_pair(it->second.start, it->second));
		} else if (mod != activeModuleIndexes.end()) {
			activeFunctions.insert(std::make_pair(mod->second + it->second.start, it->second));
		}
	}

	for (auto it = labels.begin(), end = labels.end(); it != end; ++it) {
		const auto mod = activeModuleIndexes.find(it->second.module);
		if (it->second.module <= 0) {
			activeLabels.insert(std::make_pair(it->second.addr, it->second));
		} else if (mod != activeModuleIndexes.end()) {
			activeLabels.insert(std::make_pair(mod->second + it->second.addr, it->second));
		}
	}

	for (auto it = data.begin(), end = data.end(); it != end; ++it) {
		const auto mod = activeModuleIndexes.find(it->second.module);
		if (it->second.module <= 0) {
			activeData.insert(std::make_pair(it->second.start, it->second));
		} else if (mod != activeModuleIndexes.end()) {
			activeData.insert(std::make_pair(mod->second + it->second.start, it->second));
		}
	}

	AssignFunctionIndices();
}

void SymbolMap::ClearModuleSymbols(int moduleIndex) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	if (moduleIndex <= 0) {
		Console.Warning("[SymbolMap] ClearModuleSymbols: refusing to clear moduleIndex=%d (that range holds global/non-module symbols).", moduleIndex);
		return;
	}

	auto fBegin = functions.lower_bound(std::make_pair(moduleIndex, 0u));
	auto fEnd = functions.upper_bound(std::make_pair(moduleIndex, 0xFFFFFFFFu));
	size_t removedFunctions = std::distance(fBegin, fEnd);
	functions.erase(fBegin, fEnd);

	auto lBegin = labels.lower_bound(std::make_pair(moduleIndex, 0u));
	auto lEnd = labels.upper_bound(std::make_pair(moduleIndex, 0xFFFFFFFFu));
	size_t removedLabels = std::distance(lBegin, lEnd);
	labels.erase(lBegin, lEnd);

	auto dBegin = data.lower_bound(std::make_pair(moduleIndex, 0u));
	auto dEnd = data.upper_bound(std::make_pair(moduleIndex, 0xFFFFFFFFu));
	size_t removedData = std::distance(dBegin, dEnd);
	data.erase(dBegin, dEnd);

	SM_LOG("[SymbolMap] ClearModuleSymbols: module=%d removed %zu function(s), %zu label(s), %zu data entrie(s).",
		moduleIndex, removedFunctions, removedLabels, removedData);

	// The active* views were built from the maps we just pruned -- refresh them so
	// stale entries don't keep resolving (e.g. GetSymbolType/GetFunctionStart still
	// reporting a function that no longer exists in the backing map).
	UpdateActiveSymbols();
}

bool SymbolMap::SetFunctionSize(u32 startAddress, u32 newSize) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	auto funcInfo = activeFunctions.find(startAddress);
	if (funcInfo != activeFunctions.end()) {
		auto symbolKey = std::make_pair(funcInfo->second.module, funcInfo->second.start);
		auto func = functions.find(symbolKey);
		if (func != functions.end()) {
			func->second.size = newSize;
			UpdateActiveSymbols();
		}
	}

	// TODO: check for overlaps
	return true;
}

bool SymbolMap::RemoveFunction(u32 startAddress, bool removeName) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	auto it = activeFunctions.find(startAddress);
	if (it == activeFunctions.end())
		return false;

	auto symbolKey = std::make_pair(it->second.module, it->second.start);
	auto it2 = functions.find(symbolKey);
	if (it2 != functions.end()) {
		functions.erase(it2);
	}
	activeFunctions.erase(it);

	if (removeName) {
		auto labelIt = activeLabels.find(startAddress);
		if (labelIt != activeLabels.end()) {
			symbolKey = std::make_pair(labelIt->second.module, labelIt->second.addr);
			auto labelIt2 = labels.find(symbolKey);
			if (labelIt2 != labels.end()) {
				labels.erase(labelIt2);
			}
			activeLabels.erase(labelIt);
		}
	}

	return true;
}

void SymbolMap::AddLabel(const char* name, u32 address, int moduleIndex) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	if (!name) {
		Console.Warning("[SymbolMap] AddLabel: name is null at address 0x%08X, using empty name instead of crashing.", address);
		name = "";
	}

	if (moduleIndex == -1) {
		moduleIndex = GetModuleIndex(address);
	}

	// Is there an existing one?
	u32 relAddress = GetModuleRelativeAddr(address, moduleIndex);
	auto symbolKey = std::make_pair(moduleIndex, relAddress);
	auto existing = labels.find(symbolKey);
	if (existing == labels.end()) {
		// Fall back: maybe it's got moduleIndex = 0.
		existing = labels.find(std::make_pair(0, address));
	}

	if (existing != labels.end()) {
		// We leave an existing label alone, rather than overwriting.
		// But we'll still upgrade it to the correct module / relative address.
		if (existing->second.module != moduleIndex) {
			// The entry's own key in `labels` must move too -- previously only the
			// mapped value was edited in place while the map key stayed on the old
			// (module, addr) pair, so future lookups by the correct symbolKey would
			// silently miss this label. Move it properly instead.
			LabelEntry updated = existing->second;
			updated.addr = relAddress;
			updated.module = moduleIndex;

			labels.erase(existing);
			labels[symbolKey] = updated;

			// Refresh the active item if it exists.
			auto active = activeLabels.find(address);
			if (active != activeLabels.end() && active->second.module == moduleIndex) {
				activeLabels.erase(active);
				activeLabels.insert(std::make_pair(address, updated));
			}

			SM_LOG("[SymbolMap] AddLabel: moved existing label '%s' to module=%d relAddr=0x%08X", updated.name, moduleIndex, relAddress);
		}
	} else {
		LabelEntry label;
		label.addr = relAddress;
		label.module = moduleIndex;
		strncpy(label.name, name, ARRAY_SIZE(label.name));
		label.name[ARRAY_SIZE(label.name) - 1] = 0;

		labels[symbolKey] = label;
		if (IsModuleActive(moduleIndex)) {
			activeLabels.insert(std::make_pair(address, label));
		}

		SM_LOG("[SymbolMap] AddLabel: added '%s' address=0x%08X module=%d", label.name, address, moduleIndex);
	}
}

void SymbolMap::SetLabelName(const char* name, u32 address, bool updateImmediately) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	if (!name) {
		Console.Warning("[SymbolMap] SetLabelName: name is null at address 0x%08X, using empty name instead of crashing.", address);
		name = "";
	}

	auto labelInfo = activeLabels.find(address);
	if (labelInfo == activeLabels.end()) {
		AddLabel(name, address);
	} else {
		auto symbolKey = std::make_pair(labelInfo->second.module, labelInfo->second.addr);
		auto label = labels.find(symbolKey);
		if (label != labels.end()) {
			strncpy(label->second.name, name, ARRAY_SIZE(label->second.name));
			label->second.name[ARRAY_SIZE(label->second.name) - 1] = 0;

			SM_LOG("[SymbolMap] SetLabelName: address=0x%08X renamed to '%s'", address, name);

			// Allow the caller to skip this as it causes extreme startup slowdown
			// when this gets called for every function identified by the function replacement code.
			if (updateImmediately) {
				UpdateActiveSymbols();
			}
		} else {
			Console.Warning("[SymbolMap] SetLabelName: active label at 0x%08X has no backing entry (module=%d relAddr=0x%08X), skipping rename.",
				address, labelInfo->second.module, labelInfo->second.addr);
		}
	}
}

const char *SymbolMap::GetLabelName(u32 address) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	auto it = activeLabels.find(address);
	if (it == activeLabels.end())
		return NULL;

	return it->second.name;
}

const char *SymbolMap::GetLabelNameRel(u32 relAddress, int moduleIndex) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	auto it = labels.find(std::make_pair(moduleIndex, relAddress));
	if (it == labels.end())
		return NULL;

	return it->second.name;
}

std::string SymbolMap::GetLabelString(u32 address) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	const char *label = GetLabelName(address);
	if (label == NULL)
		return "";
	return label;
}

bool SymbolMap::GetLabelValue(const char* name, u32& dest) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	if (!name) {
		Console.Warning("[SymbolMap] GetLabelValue: name is null, returning not-found instead of crashing.");
		return false;
	}

	for (auto it = activeLabels.begin(); it != activeLabels.end(); it++) {
		if (strcasecmp(name, it->second.name) == 0) {
			dest = it->first;
			return true;
		}
	}

	return false;
}

void SymbolMap::AddData(u32 address, u32 size, DataType type, int moduleIndex) {
	std::lock_guard<std::recursive_mutex> guard(m_lock);

	if (moduleIndex == -1) {
		moduleIndex = GetModuleIndex(address);
	}

	// Is there an existing one?
	u32 relAddress = GetModuleRelativeAddr(address, moduleIndex);
	auto symbolKey = std::make_pair(moduleIndex, relAddress);
	auto existing = data.find(symbolKey);
	if (existing == data.end()) {
		// Fall back: maybe it's got moduleIndex = 0.
		existing = data.find(std::make_pair(0, address));
	}

	if (existing != data.end()) {
		existing->second.size = size;
		existing->second.type = type;
		if (existing->second.module != moduleIndex) {
			existing->second.module = moduleIndex;
			existing->second.start = relAddress;
		}

		// Refresh the active item if it exists.
		auto active = activeData.find(address);
		if (active != activeData.end() && active->second.module == moduleIndex) {
			activeData.erase(active);
			activeData.insert(std::make_pair(address, existing->second));
		}
	} else {
		DataEntry entry;
		entry.start = relAddress;
		entry.size = size;
		entry.type = type;
		entry.module = moduleIndex;

		data[symbolKey] = entry;
		if (IsModuleActive(moduleIndex)) {
			activeData.insert(std::make_pair(address, entry));
		}
	}

	SM_LOG("[SymbolMap] AddData: address=0x%08X size=0x%X type=%d module=%d", address, size, (int)type, moduleIndex);
}

u32 SymbolMap::GetDataStart(u32 address) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	auto it = activeData.upper_bound(address);
	if (it == activeData.end())
	{
		// check last element
		auto rit = activeData.rbegin();

		if (rit != activeData.rend())
		{
			u32 start = rit->first;
			u32 size = rit->second.size;
			if (start <= address && start+size > address)
				return start;
		}
		// otherwise there's no data that contains this address
		return INVALID_ADDRESS;
	}

	if (it != activeData.begin()) {
		it--;
		u32 start = it->first;
		u32 size = it->second.size;
		if (start <= address && start+size > address)
			return start;
	}

	return INVALID_ADDRESS;
}

u32 SymbolMap::GetDataSize(u32 startAddress) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	auto it = activeData.find(startAddress);
	if (it == activeData.end())
		return INVALID_ADDRESS;
	return it->second.size;
}

DataType SymbolMap::GetDataType(u32 startAddress) const {
	std::lock_guard<std::recursive_mutex> guard(m_lock);
	auto it = activeData.find(startAddress);
	if (it == activeData.end())
		return DATATYPE_NONE;
	return it->second.type;
}