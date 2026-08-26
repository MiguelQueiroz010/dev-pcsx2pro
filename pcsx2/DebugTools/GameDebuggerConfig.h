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

#pragma once

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "common/Pcsx2Types.h"
#include "DebugInterface.h"

struct GameOverlayRegion
{
	std::string name;          // e.g. "BATTLE_STAGE.OVL"
	u32 startAddress = 0;       // e.g. 0x00200000
	u32 endAddress = 0;         // e.g. 0x00280000
	std::string symFilePath;    // path to attached .sym / .map file
	std::string binFilePath;    // optional: reference copy of the overlay's bytes, used only to
	                             // check whether THIS overlay is currently resident at startAddress
	                             // (compared against live memory, never written to it). The SYM file
	                             // is only attached when this signature matches, so multiple regions
	                             // sharing the same address range can hand symbols off to whichever
	                             // overlay is actually loaded.
	bool enabled = true;
};

class GameDebuggerConfigManager
{
public:
	// When true, every step of loading/saving/applying GameOverlay regions writes a
	// detailed trace via Console.WriteLn/Console.Warning. Those already flow into
	// PCSX2's on-disk log when console logging is enabled in the app. Off by default.
	static bool VerboseLogging;

	static std::string GetConfigFileForGame(const std::string& discId);
	static std::vector<GameOverlayRegion> LoadRegions(const std::string& discId);
	static bool SaveRegions(const std::string& discId, const std::vector<GameOverlayRegion>& regions);

	// ADICIONAR "bool silent = false" AQUI
	static void ApplyRegionsToCpu(DebugInterface* cpu, const std::vector<GameOverlayRegion>& regions, bool silent = false);
	static void ApplyForCurrentGame(DebugInterface* cpu);

	// --- SYM RELOAD, DISPARADO PELO DISASSEMBLER (não mais por Vsync) ---
	// s_cachedRegions guarda a última lista aplicada (ver ApplyRegionsToCpu), para
	// que CheckAndReloadIfNeeded tenha o que checar sem precisar recarregar o .ini
	// do disco a cada chamada.
	static std::vector<GameOverlayRegion> s_cachedRegions;

	// discId (GameInfo::gameCRC) para o qual s_cachedRegions foi carregado por
	// último. CheckAndReloadIfNeeded compara isso contra o discId atual a cada
	// chamada: nada mais no programa chama ApplyForCurrentGame() sozinho agora que
	// esse gatilho via disassembler substituiu o antigo por Vsync, então sem essa
	// comparação s_cachedRegions nasce vazio e NUNCA é inicializado -- nem no boot,
	// nem num reboot, nem ao carregar um savestate -- porque o early-return de
	// "s_cachedRegions.empty()" (ver .cpp) desistia antes de tentar. Começa num
	// valor sentinela que nenhum CRC real jamais bate, garantindo que a primeira
	// checagem depois de qualquer boot/reboot/troca de jogo sempre dispare uma
	// carga real do debugger/<id>.ini em vez de ficar presa em "nada em cache,
	// nada a fazer".
	static std::string s_cachedRegionsDiscId;

	// Chamado por DisassemblyManager::getLine() a cada linha renderizada. Internamente
	// throttlado por tempo (não por contagem de frames, já que não depende mais do
	// Vsync do EE/IOP) para não fazer stat() de disco a cada uma das ~30-50 linhas
	// visíveis desenhadas por repaint. Quando detecta que um .sym mudou (via
	// SymFileChanged/fingerprint), recarrega silenciosamente e isso por sua vez
	// incrementa SymbolMap::generation através de UpdateActiveSymbols() -- é esse
	// contador que o DisassemblyManager compara para saber se precisa invalidar seu
	// cache de entries e redesenhar com os símbolos atualizados.
	static void CheckAndReloadIfNeeded(DebugInterface* cpu);

	// Chamado dentro de CheckAndReloadIfNeeded, mas exposto também caso algo queira
	// forçar a checagem imediatamente (ignorando o throttle).
	static void CheckAndReloadIfNeeded(DebugInterface* cpu, bool ignoreThrottle);

	// Forces the next ApplyRegionsToCpu()/ApplyForCurrentGame() call to treat every
	// region's SYM file as changed -- clearing that module's previously loaded
	// symbols and reparsing from disk -- instead of trusting the cached mtime/size
	// fingerprint below. Call this after the user edits overlay paths/options in the
	// menu (e.g. right before/after SaveRegions), so Apply always picks up the change
	// even if the new file happens to match a stale cached fingerprint from earlier.
	static void InvalidateFileCache();

private:
	// Minimum wall-clock interval between two actual fingerprint checks (stat() calls)
	// triggered from CheckAndReloadIfNeeded. The disassembler can call this dozens of
	// times per repaint (once per visible line); this keeps the real disk I/O down to
	// roughly 5x/second regardless of how many lines are on screen or how often the
	// view repaints.
	static std::chrono::steady_clock::time_point s_lastSymCheckTime;
	static const std::chrono::milliseconds kSymCheckInterval;

	// mtime+size fingerprint of a region's SYM file as of the last time we
	// successfully loaded it, so repeated Apply calls (menu button, breakpoint set,
	// game boot) only re-parse and reload a region's symbols when the file actually
	// changed on disk -- mirroring how the Cheats system re-reads a cheat file when
	// it's been rewritten instead of always trusting whatever was parsed on first load.
	struct SymFileFingerprint
	{
		std::string path;
		long long mtime = 0;
		long long size = -1;
		bool valid = false;
	};

	// Keyed by region name (module names are unique within a game's regions, since
	// SymbolMap itself indexes modules by name).
	static std::map<std::string, SymFileFingerprint> s_symFingerprints;

	// Returns true if symFilePath's on-disk mtime/size differ from what's cached
	// under cacheKey (or nothing is cached yet). Always fills outFingerprint with
	// the freshly-read state so the caller can commit it to the cache after a
	// successful reload. mtime is read via wxDateTime::GetValue() (milliseconds)
	// rather than GetTicks() (whole seconds), so two saves within the same second
	// are still told apart on filesystems that report sub-second mtimes.
	static bool SymFileChanged(const std::string& cacheKey, const std::string& symFilePath, SymFileFingerprint& outFingerprint);
};
