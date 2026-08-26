/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2026  PCSX2 Dev Team
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
#include "GameDebuggerConfig.h"
#include "MIPSAnalyst.h"
#include "System.h"
#include "gui/AppConfig.h"
#include "common/Path.h"

#include <wx/config.h>
#include <wx/fileconf.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include "gui/AppCoreThread.h"

#include <array>
#include <algorithm>

bool GameDebuggerConfigManager::VerboseLogging = false;
std::map<std::string, GameDebuggerConfigManager::SymFileFingerprint> GameDebuggerConfigManager::s_symFingerprints;

std::vector<GameOverlayRegion> GameDebuggerConfigManager::s_cachedRegions;

// Sentinela que nenhum GameInfo::gameCRC real pode igualar (CRCs de disco são hex
// puro), então "s_cachedRegionsDiscId == kUninitializedDiscId" só é verdade antes
// da primeiríssima checagem depois do processo subir.
static const std::string kUninitializedDiscId = "\x01__UNINITIALIZED__";
std::string GameDebuggerConfigManager::s_cachedRegionsDiscId = kUninitializedDiscId;

// Estado do throttle do CheckAndReloadIfNeeded (chamado do disassembler a cada
// linha renderizada -- o timestamp começa "zerado" para que a primeira chamada
// depois do boot sempre rode a checagem real, independente do intervalo).
std::chrono::steady_clock::time_point GameDebuggerConfigManager::s_lastSymCheckTime{};
const std::chrono::milliseconds GameDebuggerConfigManager::kSymCheckInterval{200};

// Detailed step trace, only emitted when VerboseLogging is enabled. Console.WriteLn/
// Console.Warning already get mirrored into PCSX2's on-disk log when logging is
// enabled in the app, so this is all that's needed to persist a trace.
#define GDBG_LOG(...) do { if (GameDebuggerConfigManager::VerboseLogging) Console.WriteLn(__VA_ARGS__); } while (0)

// How much of a BIN signature file we actually compare against memory. Reading/
// hashing the whole overlay (which can be hundreds of KB) is unnecessary and slow,
// especially since cpu->read8() goes through the emulated MMU per call -- the first
// bytes of an overlay are enough to tell it apart from whatever else could occupy
// that same address range.
static const size_t kOverlaySignatureBytes = 0x120;

// Small standalone CRC32 (same polynomial as zip/png) so we can compare a file
// prefix against a memory prefix as a single integer instead of a byte-by-byte
// loop over a std::vector. The table is built once (thread-safe magic static).
static u32 Crc32(const u8* data, size_t len)
{
	static const std::array<u32, 256> table = [] {
		std::array<u32, 256> t{};
		for (u32 i = 0; i < 256; i++)
		{
			u32 c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			t[i] = c;
		}
		return t;
	}();

	u32 crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++)
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

bool GameDebuggerConfigManager::SymFileChanged(const std::string& cacheKey, const std::string& symFilePath, SymFileFingerprint& outFingerprint)
{
	wxFileName fn(wxString(symFilePath.c_str(), wxConvUTF8));

	if (!fn.FileExists())
	{
		// Missing file: report "changed" so the caller still attempts the load (and
		// gets LoadNocashSym's own clean failure/warning), rather than silently
		// keeping whatever stale fingerprint/symbols were cached from before the
		// file disappeared.
		outFingerprint.path = symFilePath;
		outFingerprint.mtime = 0;
		outFingerprint.size = -1;
		outFingerprint.valid = false;
		return true;
	}

	wxDateTime modTime = fn.GetModificationTime();
	outFingerprint.path = symFilePath;
	// GetTicks() trunca pra resolução de 1s -- um recompile+save rápido do
	// toolchain caindo dentro do mesmo segundo (com o mesmo tamanho de arquivo)
	// passava batido, tanto no Apply manual quanto no reload automático
	// (CheckAndReloadIfNeeded chama esta mesma função). GetValue() devolve o
	// timestamp em milissegundos, então duas gravações no mesmo segundo só
	// continuam indistinguíveis se o SO/FS realmente não reportar mais que 1s de
	// resolução -- não é o caso na esmagadora maioria dos setups modernos
	// (NTFS/ext4/APFS todos dão sub-segundo).
	outFingerprint.mtime = modTime.IsValid() ? (long long)modTime.GetValue().ToDouble() : 0;
	outFingerprint.size = (long long)fn.GetSize().GetValue();
	outFingerprint.valid = true;

	auto it = s_symFingerprints.find(cacheKey);
	if (it == s_symFingerprints.end())
		return true; // never successfully loaded for this region before

	const SymFileFingerprint& cached = it->second;
	if (!cached.valid || cached.path != symFilePath || cached.mtime != outFingerprint.mtime || cached.size != outFingerprint.size)
		return true;

	return false;
}

void GameDebuggerConfigManager::InvalidateFileCache()
{
	size_t count = s_symFingerprints.size();
	s_symFingerprints.clear();
	GDBG_LOG("[GameDebugger] InvalidateFileCache: cleared %zu cached SYM fingerprint(s); next Apply will reparse everything.", count);
}

std::string GameDebuggerConfigManager::GetConfigFileForGame(const std::string& discId)
{
	std::string id = discId;
	if (id.empty())
	{
		Console.WriteLn("[GameDebugger] GetConfigFileForGame: empty discId, falling back to 'DEFAULT'.");
		id = "DEFAULT";
	}

	// Sanitize the id so a malformed/garbage disc CRC can't produce a path
	// wx can't create (e.g. containing path separators or control characters).
	for (char& c : id)
	{
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
			c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 0x20)
		{
			c = '_';
		}
	}

	wxDirName debugDir = EmuFolders::Settings.Combine(wxDirName(L"debugger"));
	if (!debugDir.Exists())
	{
		if (!debugDir.Mkdir())
		{
			// Failsafe: if we can't create the debugger config folder (permissions,
			// read-only install dir, etc.) fall back to the Settings root instead of
			// silently handing back a path that will fail to open later.
			Console.Warning("[GameDebugger] GetConfigFileForGame: could not create '%s', falling back to Settings root.",
				debugDir.ToString().ToUTF8().data());
			debugDir = EmuFolders::Settings;
		}
		else
		{
			GDBG_LOG("[GameDebugger] GetConfigFileForGame: created debugger config folder '%s'.",
				debugDir.ToString().ToUTF8().data());
		}
	}

	wxString filename = wxString::Format(L"%s.ini", wxString(id.c_str(), wxConvUTF8));
	std::string cfgPath = std::string(debugDir.Combine(filename).GetFullPath().ToUTF8());
	GDBG_LOG("[GameDebugger] GetConfigFileForGame: discId='%s' -> '%s'", discId.c_str(), cfgPath.c_str());
	return cfgPath;
}

std::vector<GameOverlayRegion> GameDebuggerConfigManager::LoadRegions(const std::string& discId)
{
	std::vector<GameOverlayRegion> regions;
	std::string cfgPath = GetConfigFileForGame(discId);

	if (cfgPath.empty())
	{
		Console.Warning("[GameDebugger] LoadRegions: got an empty config path, aborting.");
		return regions;
	}

	std::unique_ptr<wxFileConfig> config;
	try
	{
		// wxFileConfig can throw (or, depending on build, assert/abort) if the
		// backing file exists but is unreadable, locked, or corrupt. Previously
		// nothing here caught that, so a bad debugger/<id>.ini file could take
		// the whole emulator down the moment GameOverlays were loaded. Failsafe:
		// treat any failure to open the config as "no regions" instead of crashing.
		config.reset(new wxFileConfig(wxEmptyString, wxEmptyString, wxString(cfgPath.c_str(), wxConvUTF8), wxEmptyString, wxCONFIG_USE_LOCAL_FILE));
	}
	catch (const std::exception& e)
	{
		Console.Warning("[GameDebugger] LoadRegions: exception opening '%s': %s", cfgPath.c_str(), e.what());
		return regions;
	}
	catch (...)
	{
		Console.Warning("[GameDebugger] LoadRegions: unknown exception opening '%s'.", cfgPath.c_str());
		return regions;
	}

	if (!config)
	{
		Console.Warning("[GameDebugger] LoadRegions: failed to open '%s'.", cfgPath.c_str());
		return regions;
	}

	long numRegions = 0;
	config->Read(L"General/RegionCount", &numRegions, 0);

	// Failsafe: a corrupt/tampered ini could contain a huge or negative RegionCount.
	// Cap it instead of looping (near-)indefinitely or exhausting memory.
	const long kMaxRegions = 4096;
	if (numRegions < 0)
	{
		Console.Warning("[GameDebugger] LoadRegions: negative RegionCount (%ld) in '%s', treating as 0.", numRegions, cfgPath.c_str());
		numRegions = 0;
	}
	else if (numRegions > kMaxRegions)
	{
		Console.Warning("[GameDebugger] LoadRegions: RegionCount (%ld) in '%s' exceeds sane limit, clamping to %ld.", numRegions, cfgPath.c_str(), kMaxRegions);
		numRegions = kMaxRegions;
	}

	GDBG_LOG("[GameDebugger] LoadRegions: '%s' reports %ld region(s).", cfgPath.c_str(), numRegions);

	for (long i = 0; i < numRegions; i++)
	{
		wxString group = wxString::Format(L"Region_%ld", i);
		config->SetPath(L"/" + group);

		GameOverlayRegion reg;
		wxString name, symPath, binPath;
		wxString startStr, endStr;
		bool enabled = true;

		config->Read(L"Name", &name);
		config->Read(L"StartAddress", &startStr);
		config->Read(L"EndAddress", &endStr);
		config->Read(L"SymFilePath", &symPath);
		config->Read(L"BinFilePath", &binPath);
		config->Read(L"Enabled", &enabled, true);

		reg.name = std::string(name.ToUTF8());
		reg.symFilePath = std::string(symPath.ToUTF8());
		reg.binFilePath = std::string(binPath.ToUTF8());
		reg.enabled = enabled;

		bool startOk = sscanf(startStr.ToUTF8(), "%x", &reg.startAddress) == 1;
		bool endOk = sscanf(endStr.ToUTF8(), "%x", &reg.endAddress) == 1;

		if (!startOk || !endOk)
		{
			Console.Warning("[GameDebugger] LoadRegions: Region_%ld '%s' has an unparsable address (start='%s' end='%s'), disabling it.",
				i, reg.name.c_str(), std::string(startStr.ToUTF8()).c_str(), std::string(endStr.ToUTF8()).c_str());
			reg.enabled = false;
		}
		else if (reg.endAddress <= reg.startAddress)
		{
			Console.Warning("[GameDebugger] LoadRegions: Region_%ld '%s' has end<=start (0x%08X..0x%08X), disabling it.",
				i, reg.name.c_str(), reg.startAddress, reg.endAddress);
			reg.enabled = false;
		}

		GDBG_LOG("[GameDebugger] LoadRegions:   Region_%ld '%s' start=0x%08X end=0x%08X enabled=%d sym='%s' bin='%s'",
			i, reg.name.c_str(), reg.startAddress, reg.endAddress, reg.enabled, reg.symFilePath.c_str(), reg.binFilePath.c_str());

		regions.push_back(reg);
	}

	Console.WriteLn("[GameDebugger] LoadRegions: loaded %zu region(s) from '%s'.", regions.size(), cfgPath.c_str());
	return regions;
}

bool GameDebuggerConfigManager::SaveRegions(const std::string& discId, const std::vector<GameOverlayRegion>& regions)
{
	std::string cfgPath = GetConfigFileForGame(discId);
	if (cfgPath.empty())
	{
		Console.Warning("[GameDebugger] SaveRegions: got an empty config path, aborting.");
		return false;
	}

	std::unique_ptr<wxFileConfig> config;
	try
	{
		config.reset(new wxFileConfig(wxEmptyString, wxEmptyString, wxString(cfgPath.c_str(), wxConvUTF8), wxEmptyString, wxCONFIG_USE_LOCAL_FILE));
	}
	catch (const std::exception& e)
	{
		Console.Warning("[GameDebugger] SaveRegions: exception opening '%s': %s", cfgPath.c_str(), e.what());
		return false;
	}
	catch (...)
	{
		Console.Warning("[GameDebugger] SaveRegions: unknown exception opening '%s'.", cfgPath.c_str());
		return false;
	}

	if (!config)
	{
		Console.Warning("[GameDebugger] SaveRegions: failed to open '%s'.", cfgPath.c_str());
		return false;
	}

	try
	{
		config->DeleteAll();

		config->SetPath(L"/General");
		config->Write(L"RegionCount", (long)regions.size());

		for (size_t i = 0; i < regions.size(); i++)
		{
			wxString group = wxString::Format(L"/Region_%zu", i);
			config->SetPath(group);

			const auto& reg = regions[i];
			config->Write(L"Name", wxString(reg.name.c_str(), wxConvUTF8));
			config->Write(L"StartAddress", wxString::Format(L"0x%08X", reg.startAddress));
			config->Write(L"EndAddress", wxString::Format(L"0x%08X", reg.endAddress));
			config->Write(L"SymFilePath", wxString(reg.symFilePath.c_str(), wxConvUTF8));
			config->Write(L"BinFilePath", wxString(reg.binFilePath.c_str(), wxConvUTF8));
			config->Write(L"Enabled", reg.enabled);

			GDBG_LOG("[GameDebugger] SaveRegions:   Region_%zu '%s' start=0x%08X end=0x%08X enabled=%d",
				i, reg.name.c_str(), reg.startAddress, reg.endAddress, reg.enabled);
		}

		if (!config->Flush())
		{
			Console.Warning("[GameDebugger] SaveRegions: Flush() failed for '%s'.", cfgPath.c_str());
			return false;
		}
	}
	catch (const std::exception& e)
	{
		Console.Warning("[GameDebugger] SaveRegions: exception writing '%s': %s", cfgPath.c_str(), e.what());
		return false;
	}
	catch (...)
	{
		Console.Warning("[GameDebugger] SaveRegions: unknown exception writing '%s'.", cfgPath.c_str());
		return false;
	}

	Console.WriteLn("[GameDebugger] SaveRegions: saved %zu region(s) to '%s'.", regions.size(), cfgPath.c_str());
	return true;
}

// ATUALIZE A ASSINATURA DA FUNÇÃO
namespace
{
	// Endereço de retorno das funções MIPS -- $ra é o registrador 31. "jr $ra"
	// codifica como SPECIAL (opcode 0) + rs=31 + funct=JR(0x08): 000000 11111
	// 00000 00000 00000 001000 = 0x03E00008. É o padrão de retorno mais comum
	// gerado por gcc pra EE (uma função, um epílogo, um "jr $ra" só no fim) --
	// não cobre funções com múltiplos "jr $ra" (early return) nem tail-calls
	// via "j"/"jr" pra outra função, mas é o caso comum o bastante pra ser uma
	// melhoria real sobre confiar só no scanner de prólogo do MIPSAnalyst.
	const u32 kInstrJrRa = 0x03E00008;

	// Limite de instruções varridas procurando o "jr $ra" de uma função antes
	// de desistir (função absurdamente grande, ou o alvo do JAL não era
	// realmente o início de uma função MIPS de verdade -- ex.: dado
	// confundido com código, ou um alvo dentro do meio de outra função que o
	// compilador só alcança via jump computado).
	const int kMaxFunctionScanInstructions = 8192; // 32KB

	// Varre pra frente a partir de funcAddr procurando "jr $ra" (delay slot
	// incluso) dentro de [scanFloor, scanCeiling). Retorna o tamanho da função
	// (bytes) até e incluindo o delay slot, ou 0 se não achou dentro do
	// limite/dos bounds -- chamador deve tratar 0 como "não deu pra
	// determinar o tamanho com confiança, não adiciona o símbolo".
	u32 ScanFunctionSizeByReturn(DebugInterface* cpu, u32 funcAddr, u32 scanFloor, u32 scanCeiling)
	{
		u32 addr = funcAddr;
		int steps = 0;
		while (addr + 4 < scanCeiling && steps < kMaxFunctionScanInstructions)
		{
			if (!cpu->isValidAddress(addr))
				return 0;

			u32 instr = cpu->read32(addr);
			if (instr == kInstrJrRa)
			{
				u32 end = addr + 8; // inclui o delay slot
				return (end > funcAddr && end <= scanCeiling) ? (end - funcAddr) : 0;
			}

			addr += 4;
			steps++;
		}
		return 0;
	}

	// Varre [start, end) procurando instruções JAL (opcode 000011 -- JALR não
	// entra aqui porque o alvo é um registrador, não dá pra resolver
	// estaticamente, mesma limitação documentada em CallGraphIndex::Build no
	// DisassemblyManager) e, pra cada alvo que caia dentro da própria região e
	// ainda não tenha uma função conhecida começando exatamente ali, tenta
	// registrar uma nova função: início = alvo do JAL (é praticamente
	// garantido ser o início de uma função de verdade, já que é chamada via
	// call), tamanho = distância até o primeiro "jr $ra" encontrado (ver
	// ScanFunctionSizeByReturn acima). Isso pega funções que o scanner de
	// prólogo heurístico do MIPSAnalyst erra (padrões de prólogo atípicos,
	// funções muito curtas, etc.) mas que são alcançáveis via call de dentro
	// da própria região -- e é mais confiável que o scanner de prólogo porque
	// "isso é chamado E termina em jr $ra" é um sinal bem mais forte de "isso
	// é uma função" do que só reconhecer a forma do prólogo.
	int AnalyzeCallTargetsInRegion(DebugInterface* cpu, int moduleIdx, u32 start, u32 end)
	{
		int added = 0;

		for (u32 addr = start; addr + 4 < end; addr += 4)
		{
			if (!cpu->isValidAddress(addr))
				continue;

			u32 instr = cpu->read32(addr);
			u32 opcode = instr >> 26;
			if (opcode != 0x03) // não é JAL
				continue;

			u32 target = ((instr & 0x03FFFFFF) << 2) | ((addr + 4) & 0xF0000000);
			if (target < start || target >= end)
				continue; // call pra fora da região (ex.: pro ELF base) -- não é problema nosso aqui

			u32 existingStart = cpu->GetSymbolMap().GetFunctionStart(target);
			if (existingStart == target)
				continue; // já é o início de uma função conhecida -- nada a fazer

			if (existingStart != SymbolMap::INVALID_ADDRESS)
				continue; // cai DENTRO de outra função já registrada -- provável limite
				          // impreciso dela, mas dividir/encolher uma função existente é
				          // arriscado o bastante pra ficar fora do escopo desta passada.

			u32 size = ScanFunctionSizeByReturn(cpu, target, start, end);
			if (size == 0)
				continue; // não achou "jr $ra" com confiança -- não adivinha um tamanho

			char name[32];
			snprintf(name, sizeof(name), "sub_%08X", target);
			cpu->GetSymbolMap().AddFunction(name, target, size, moduleIdx);
			added++;

			GDBG_LOG("[GameDebugger]   -> AnalyzeCallTargets: found '%s' via JAL at 0x%08X, size=0x%X (ends at jr $ra 0x%08X).",
				name, addr, size, target + size - 8);
		}

		return added;
	}
} // namespace


void GameDebuggerConfigManager::ApplyRegionsToCpu(DebugInterface* cpu, const std::vector<GameOverlayRegion>& regions, bool silent)
{
	if (!cpu)
	{
		Console.Warning("[GameDebugger] ApplyRegionsToCpu: cpu is null, aborting.");
		return;
	}

	// SALVA NO CACHE PARA O CheckAndReloadIfNeeded (disassembler) USAR DEPOIS
	s_cachedRegions = regions;

	// SILENCIA O LOG
	if (!silent) 
		Console.WriteLn("[GameDebugger] ApplyRegionsToCpu: Applying %zu region(s)...", regions.size());
	
	for (size_t idx = 0; idx < regions.size(); idx++)
	{
		const auto& reg = regions[idx];

		// Failsafe: one malformed region (bad addresses, missing files, whatever)
		// must never abort the rest of the list or throw back into the caller --
		// previously an exception here (or a bad write below) could take the whole
		// emulator down while just trying to apply GameOverlays.
		try
		{
			if (!reg.enabled)
			{
				GDBG_LOG("[GameDebugger] Region[%zu] '%s' is disabled, skipping.", idx, reg.name.c_str());
				continue;
			}

			if (reg.endAddress <= reg.startAddress)
			{
				Console.Warning("[GameDebugger] Region[%zu] '%s' has an invalid range (0x%08X..0x%08X), skipping.",
					idx, reg.name.c_str(), reg.startAddress, reg.endAddress);
				continue;
			}

			u32 size = reg.endAddress - reg.startAddress;

			GDBG_LOG("[GameDebugger] Region[%zu] '%s': addr=0x%08X..0x%08X size=0x%X",
				idx, reg.name.c_str(), reg.startAddress, reg.endAddress, size);

			// 1. Decide whether this region is "active": if a BIN signature file is
			// attached, it must match what's currently resident in EE memory at
			// StartAddr. This lets several regions share the same address range
			// (different overlays swapped in/out at runtime) and only the one whose
			// BIN actually matches gets its module + SYM symbols attached -- the
			// others are left inactive until their own BIN matches instead.
			// NOTE: the BIN file is only ever READ here, never written to memory.
			bool shouldActivate = true;

			if (!reg.binFilePath.empty())
			{
				shouldActivate = false;

				if (!cpu->isAlive())
				{
					Console.Warning("[GameDebugger]   -> BIN check skipped: CPU not alive.");
				}
				else
				{
					if (!cpu->isCpuPaused())
					{
						GDBG_LOG("[GameDebugger]   -> BIN check for '%s': CPU is running, comparing against a live (possibly torn) snapshot. Pause for a fully reliable check.",
							reg.binFilePath.c_str());
					}

					GDBG_LOG("[GameDebugger]   -> Comparing '%s' against memory at 0x%08X...",
						reg.binFilePath.c_str(), reg.startAddress);

					wxFFile file(wxString(reg.binFilePath.c_str(), wxConvUTF8), L"rb");
					if (!file.IsOpened())
					{
						Console.Warning("[GameDebugger]   -> BIN file could not be opened: '%s'", reg.binFilePath.c_str());
					}
					else
					{
						ssize_t len = file.Length();
						if (len <= 0)
						{
							Console.Warning("[GameDebugger]   -> BIN file is empty or unreadable.");
						}
						// Failsafe: refuse to read past the region's own declared end
						// address, and refuse if start+len would overflow u32. Without
						// this, a startAddress near 0xFFFFFFFF (or a region smaller
						// than the signature) reads past the intended range into
						// whatever memory follows -- a very plausible crash/freeze.
						else if (reg.startAddress + (u32)std::min<ssize_t>(len, (ssize_t)kOverlaySignatureBytes) < reg.startAddress)
						{
							Console.Warning("[GameDebugger]   -> BIN compare for '%s' would overflow the address space (start=0x%08X), aborting.",
								reg.binFilePath.c_str(), reg.startAddress);
						}
						else
						{
							// Only the first kOverlaySignatureBytes matter, further capped
							// to whatever actually fits in this region and in the file
							// itself (a smaller BIN or a smaller region both just shrink
							// the signature instead of erroring out).
							size_t sigLen = std::min<size_t>({(size_t)len, kOverlaySignatureBytes, (size_t)size});

							std::vector<u8> fileSig(sigLen);
							if (file.Read(fileSig.data(), sigLen) != sigLen)
							{
								Console.Warning("[GameDebugger]   -> BIN file read incomplete (expected %zu byte(s)).", sigLen);
							}
							else
							{
								std::vector<u8> memSig(sigLen);
								for (size_t i = 0; i < sigLen; i++)
									memSig[i] = cpu->read8(reg.startAddress + (u32)i);

								u32 fileCrc = Crc32(fileSig.data(), sigLen);
								u32 memCrc = Crc32(memSig.data(), sigLen);

								if (fileCrc == memCrc)
								{
									shouldActivate = true;
									GDBG_LOG("[GameDebugger]   -> BIN signature MATCHES memory (%zu byte(s), CRC32=0x%08X). Region is active.", sigLen, fileCrc);
								}
								else
								{
									GDBG_LOG("[GameDebugger]   -> BIN signature does NOT match memory (%zu byte(s), file CRC32=0x%08X, mem CRC32=0x%08X). Region is inactive.",
										sigLen, fileCrc, memCrc);
								}
							}
						}
					}
				}
			}

			// 2. Add (or remove) the module entry in SymbolMap based on the check above.
			if (reg.name.empty())
			{
				Console.Warning("[GameDebugger] Region[%zu] has no name, skipping AddModule (SYM step still runs).", idx);
			}
			else if (size > 0)
			{
				if (shouldActivate)
				{
					GDBG_LOG("[GameDebugger]   -> AddModule '%s' at 0x%08X size=0x%X", reg.name.c_str(), reg.startAddress, size);
					cpu->GetSymbolMap().AddModule(reg.name.c_str(), reg.startAddress, size);
				}
				else
				{
					// Failsafe cleanup: if this region was active from a previous
					// Apply (its BIN used to match), and now doesn't, deactivate it
					// so its stale symbols don't linger and conflict with whichever
					// overlay is actually resident now.
					//
					// IMPORTANT: this must be identity-safe (by name), not just by
					// address range. Two overlay regions that share the same
					// start/end (the whole point of overlays swapped in/out at the
					// same address) also share the same activeModuleEnds key, keyed
					// only by end address. The address-only UnloadModule() would
					// erase whatever module currently sits there -- including a
					// *different*, legitimately active overlay -- purely because of
					// iteration order. That's what made only one of two overlays
					// ever end up with live symbols.
					GDBG_LOG("[GameDebugger]   -> UnloadModule '%s' at 0x%08X size=0x%X (BIN no longer matches)", reg.name.c_str(), reg.startAddress, size);
					cpu->GetSymbolMap().UnloadModule(reg.name.c_str(), reg.startAddress, size);
				}
			}

			// 3. Load the attached SYM/MAP file, but only for an active region.
			// NOTE: LoadNocashSym baseOffset=0 because no$psx/PCSX2 .sym files store
			// ABSOLUTE addresses already. Passing startAddress would double-offset them.
			if (!reg.symFilePath.empty())
			{
				if (!shouldActivate)
				{
					GDBG_LOG("[GameDebugger]   -> Skipping SYM '%s': region is inactive (BIN signature not present in memory).", reg.symFilePath.c_str());
				}
				else
				{
					SymFileFingerprint freshFingerprint;
					bool changed = SymFileChanged(reg.name, reg.symFilePath, freshFingerprint);

					if (!changed)
					{
						GDBG_LOG("[GameDebugger]   -> SYM '%s' unchanged on disk since last load (same mtime/size), skipping reparse.",
							reg.symFilePath.c_str());
					}
					else
					{
						// New region, path was swapped to a different file, or the
						// file was rewritten on disk since we last read it. Purge
						// whatever this module's symbols currently are *before*
						// reparsing: AddFunction/AddLabel/AddData intentionally never
						// overwrite an existing entry, so a plain reload would just
						// pile the updated file's symbols on top of the old ones
						// instead of replacing them (renamed/removed functions would
						// stick around forever, and switching between two overlays
						// that had reused this module name would keep showing
						// whichever set got parsed first).
						int existingModule = cpu->GetSymbolMap().GetModuleIndexByName(reg.name.c_str());
						if (existingModule > 0)
						{
							GDBG_LOG("[GameDebugger]   -> SYM for '%s' changed on disk: clearing previously loaded symbols (module=%d) before reparsing.",
								reg.name.c_str(), existingModule);
							cpu->GetSymbolMap().ClearModuleSymbols(existingModule);
						}

						GDBG_LOG("[GameDebugger]   -> LoadNocashSym '%s' (baseOffset=0, absolute addresses)",
							reg.symFilePath.c_str());

						bool symOk = cpu->GetSymbolMap().LoadNocashSym(reg.symFilePath.c_str(), 0);
						if (symOk)
						{
							cpu->GetSymbolMap().UpdateActiveSymbols();
							s_symFingerprints[reg.name] = freshFingerprint;
							GDBG_LOG("[GameDebugger]   -> SYM loaded and active symbols updated OK.");
						}
						else
						{
							Console.Warning("[GameDebugger]   -> SYM file failed to load: '%s'", reg.symFilePath.c_str());
							// Don't cache a fingerprint for a failed load -- retry next Apply.
						}
					}
				}
			}
			else
			{
				// The user cleared this region's SYM path in the menu. If we'd
				// previously loaded symbols for it, they're now orphaned (no file
				// backs them anymore) -- purge them and drop the stale cache entry
				// instead of leaving functions/labels the user can no longer edit
				// or account for.
				auto cacheIt = s_symFingerprints.find(reg.name);
				if (cacheIt != s_symFingerprints.end())
				{
					int existingModule = cpu->GetSymbolMap().GetModuleIndexByName(reg.name.c_str());
					if (existingModule > 0)
					{
						GDBG_LOG("[GameDebugger]   -> SYM path cleared for '%s': purging previously loaded symbols (module=%d).",
							reg.name.c_str(), existingModule);
						cpu->GetSymbolMap().ClearModuleSymbols(existingModule);
					}
					s_symFingerprints.erase(cacheIt);
				}
			}

			// 4. Fallback: overlay ativo mas sem NENHUMA função conhecida nessa
			// região (sem .sym anexado, .sym que só tem labels/dados, ou .sym
			// que falhou ao carregar). Sem isso a aba Functions fica muda pra
			// essa região inteira -- roda o scanner heurístico de prólogo/
			// epílogo do MIPSAnalyst (o mesmo usado pro ELF base) como
			// segunda opção, só pra ter *alguma* função navegável. Roda uma
			// única vez por módulo: assim que existe pelo menos uma
			// ST_FUNCTION cadastrada pra esse moduleIndex (via .sym real ou
			// via essa própria heurística numa Apply anterior), as próximas
			// chamadas pulam o scan.
			if (shouldActivate && size > 0 && !reg.name.empty())
			{
				int moduleIdx = cpu->GetSymbolMap().GetModuleIndexByName(reg.name.c_str());
				if (moduleIdx > 0)
				{
					bool hasAnyFunction = false;
					for (const SymbolEntry& fn : cpu->GetSymbolMap().GetAllSymbols(ST_FUNCTION))
					{
						if (fn.moduleIndex == moduleIdx)
						{
							hasAnyFunction = true;
							break;
						}
					}

					if (!hasAnyFunction)
					{
						GDBG_LOG("[GameDebugger]   -> No ST_FUNCTION symbols for active region '%s' (module=%d), running heuristic MIPSAnalyst::ScanForFunctions fallback over 0x%08X-0x%08X.",
							reg.name.c_str(), moduleIdx, reg.startAddress, reg.startAddress + size - 1);

						MIPSAnalyst::ScanForFunctions(cpu->GetSymbolMap(), reg.startAddress, reg.startAddress + size - 1, /*insertSymbols=*/true);
						cpu->GetSymbolMap().UpdateActiveSymbols();
					}
				}

				// 5. Análise por chamadas (JAL) + heurística de "jr $ra": ao
				// contrário do passo 4 acima, roda SEMPRE que a região está
				// ativa (não só quando ela não tem função nenhuma ainda) --
				// mesmo um .sym real ou o scanner de prólogo pode deixar
				// passar funções que só são alcançadas via call de dentro da
				// própria região. "é chamada por JAL e termina em jr $ra" é
				// um sinal mais forte de fronteira de função de verdade do
				// que só reconhecer a forma do prólogo, e por só ADICIONAR
				// (nunca redimensionar/remover o que já existe) é seguro
				// rodar em toda Apply sem risco de atropelar um .sym real.
				if (moduleIdx > 0)
				{
					int added = AnalyzeCallTargetsInRegion(cpu, moduleIdx, reg.startAddress, reg.startAddress + size);
					if (added > 0)
					{
						GDBG_LOG("[GameDebugger]   -> AnalyzeCallTargets: added %d function(s) for region '%s' via JAL+jr $ra analysis.",
							added, reg.name.c_str());
						cpu->GetSymbolMap().UpdateActiveSymbols();
					}
				}
			}
		}
		catch (const std::exception& e)
		{
			Console.Warning("[GameDebugger] Region[%zu] '%s' raised an exception while applying: %s", idx, reg.name.c_str(), e.what());
		}
		catch (...)
		{
			Console.Warning("[GameDebugger] Region[%zu] '%s' raised an unknown exception while applying, skipping it.", idx, reg.name.c_str());
		}
	}

	if (!silent)
		Console.WriteLn("[GameDebugger] ApplyRegionsToCpu: Done.");
}

void GameDebuggerConfigManager::ApplyForCurrentGame(DebugInterface* cpu)
{
	if (!cpu)
	{
		Console.Warning("[GameDebugger] ApplyForCurrentGame: cpu is null, aborting.");
		return;
	}

	try
	{
		std::string discId = std::string(GameInfo::gameCRC.ToUTF8());
		GDBG_LOG("[GameDebugger] ApplyForCurrentGame: discId='%s'", discId.c_str());

		std::vector<GameOverlayRegion> regions = LoadRegions(discId);
		ApplyRegionsToCpu(cpu, regions);
	}
	catch (const std::exception& e)
	{
		Console.Warning("[GameDebugger] ApplyForCurrentGame: exception: %s", e.what());
	}
	catch (...)
	{
		Console.Warning("[GameDebugger] ApplyForCurrentGame: unknown exception.");
	}
}
void GameDebuggerConfigManager::CheckAndReloadIfNeeded(DebugInterface* cpu)
{
	CheckAndReloadIfNeeded(cpu, false);
}

void GameDebuggerConfigManager::CheckAndReloadIfNeeded(DebugInterface* cpu, bool ignoreThrottle)
{
	if (!cpu || !cpu->isAlive())
		return;

	std::string discId = std::string(GameInfo::gameCRC.ToUTF8());

	// True on the very first call after process start, and again whenever the
	// running game changes (boot, reboot into a different title, or a savestate
	// loaded for a different game than the one we last applied regions for).
	//
	// Crucially this is ALSO what fixes the "reboot / load a savestate and SYM
	// just stops applying" bug: nothing else in the app calls ApplyForCurrentGame()
	// any more now that this disassembler-driven check replaced the old
	// Vsync-based one, so without this comparison s_cachedRegions starts empty and
	// stays empty forever -- there was no path left that ever did the first load.
	// Comparing discId (rather than just "is s_cachedRegions empty") also makes
	// this correct for a game that legitimately has zero configured overlays: once
	// we've confirmed that for its discId, we stop hammering the disk re-checking
	// it every throttle tick.
	bool needsBootstrap = (s_cachedRegionsDiscId != discId);

	// Chamado de dentro de DisassemblyManager::getLine(), ou seja, potencialmente
	// dezenas de vezes por repaint (uma por linha visível). O throttle é o que
	// diferencia isso de martelar stat() no disco a cada linha desenhada: sem
	// ignoreThrottle, só deixamos passar de fato a cada kSymCheckInterval. Um
	// bootstrap (troca de jogo) sempre passa direto, mesmo com o throttle ainda
	// "quente" de antes -- symbols errados/ausentes do jogo anterior não devem
	// esperar até 200ms para sumir da tela.
	if (!ignoreThrottle && !needsBootstrap)
	{
		auto now = std::chrono::steady_clock::now();
		if (now - s_lastSymCheckTime < kSymCheckInterval)
			return;
	}
	s_lastSymCheckTime = std::chrono::steady_clock::now();

	if (needsBootstrap)
	{
		GDBG_LOG("[GameDebugger] CheckAndReloadIfNeeded: game changed ('%s' -> '%s'), (re)loading region list from debugger/<id>.ini.",
			s_cachedRegionsDiscId.c_str(), discId.c_str());
		s_cachedRegionsDiscId = discId;
		ApplyForCurrentGame(cpu); // lê o .ini do disco e já popula s_cachedRegions via ApplyRegionsToCpu
		return;
	}

	if (s_cachedRegions.empty())
		return; // confirmado: este jogo não tem overlays configurados, nada a fazer

	// Reaplica as regiões silenciosamente:
	// 1. Lê a RAM do PS2 e compara o CRC com o BIN (troca os overlays instantaneamente se bater).
	// 2. Chama o SymFileChanged() -- se o .sym foi editado/recompilado/sobrescrito no
	//    disco desde a última vez, recarrega as tabelas novas no ato.
	// ApplyRegionsToCpu() chama UpdateActiveSymbols() por baixo dos panos sempre que
	// algo muda de fato, o que por sua vez incrementa SymbolMap::generation -- é esse
	// contador que DisassemblyManager::getLine() compara para saber se precisa
	// invalidar seu próprio cache de entries e redesenhar com os símbolos atualizados.
	ApplyRegionsToCpu(cpu, s_cachedRegions, true);
}
