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
#include <wx/wx.h>

#include "DebugTools/DebugInterface.h"
#include "DebugTools/DisassemblyManager.h"

class CallTreeDialog;
class GraphViewDialog;

class CtrlDisassemblyView: public wxWindow
{
public:
	CtrlDisassemblyView(wxWindow* parent, DebugInterface* _cpu);

	void mouseEvent(wxMouseEvent& evt);
	void paintEvent(wxPaintEvent & evt);
	void keydownEvent(wxKeyEvent& evt);
	void scrollbarEvent(wxScrollWinEvent& evt);
	void sizeEvent(wxSizeEvent& evt);
	void focusEvent(wxFocusEvent& evt) { Refresh(); };
#ifdef _WIN32
	WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam);
#endif

	void scanFunctions();
	// Also drops every tracked pending edit (see m_originalInstructions below):
	// this is called on reboot/reset/game-change, at which point the underlying
	// RAM has already gone back to its real original contents on its own, so any
	// edit this session had made is moot -- there's nothing left to restore.
	void clearFunctions() { manager.clear(); m_originalInstructions.clear(); };
	void redraw();
	void getOpcodeText(u32 address, char* dest);
	
	u32 getInstructionSizeAt(u32 address)
	{
		u32 start = manager.getStartAddress(address);
		u32 next  = manager.getNthNextAddress(start,1);
		return next-address;
	}

	void gotoAddress(u32 addr);
	void gotoPc() { gotoAddress(cpu->getPC()); };
	void scrollStepping(u32 newPc);
	// Reescreve o alvo de uma instrução JAL em callSiteAddr para newTarget.
	// Público porque CallTreeDialog (aberto via showCallTree()) chama isso
	// diretamente pro item de menu "Redirect call...". Passa pelo mesmo
	// caminho de recordOriginalInstructionIfNeeded()/SysClearExecutionCache()
	// que replaceWithNOP()/assembleOpcode() já usam, então "Restore Original
	// Instruction(s)" também desfaz um redirect. Retorna false (e não mexe em
	// nada) se a instrução em callSiteAddr não for um JAL de fato.
	bool PatchCallTarget(u32 callSiteAddr, u32 newTarget);
	// Generaliza PatchCallTarget pra qualquer branch/jump, não só JAL --
	// usado pelo "Redirect this branch..." do Graph View (setas coloridas
	// T/F/incondicional/switch). Decide o encoding certo pelo opcode:
	// J (26 bits absolutos, mesmo esquema de PatchCallTarget) ou branch
	// condicional (16 bits relativos a branchAddr+4). JR/JALR (registrador)
	// não são suportados aqui -- o alvo é dinâmico, não tem campo imediato
	// pra reescrever.
	bool PatchBranchTarget(u32 branchAddr, u32 newTarget);
	wxDECLARE_EVENT_TABLE();
private:
	void drawBranchLine(wxDC& dc, std::map<u32,int>& addressPositions, BranchLine& line);
	void render(wxDC& dc);
	void calculatePixelPositions();
	bool getDisasmAddressText(u32 address, char* dest, bool abbreviateLabels, bool showData);
	u32 yToAddress(int y);
	bool curAddressIsVisible();
	void followBranch();
	void toggleBreakpoint(bool toggleEnabled);
	void updateStatusBarText();
	std::string disassembleRange(u32 start, u32 size);
	std::string disassembleCurAddress();
	void copyInstructions(u32 startAddr, u32 endAddr, bool withDisasm);
	void disassembleToFile();
	void disassembleRegionToFile();
	void replaceWithNOP();
	// Restores every address in [selectRangeStart, selectRangeEnd) that has a
	// tracked original value (see m_originalInstructions below) back to that
	// value -- undoes replaceWithNOP()/assembleOpcode() edits, address by
	// address, for whatever's currently selected. Addresses in the selection
	// that were never edited (or already restored) are silently skipped.
	void restoreSelectedInstructions();
	// Called once, right before the FIRST time address gets overwritten (by
	// replaceWithNOP() or assembleOpcode()), to save whatever's there now into
	// m_originalInstructions. A no-op if that address is already tracked, so a
	// second edit to the same address before it's restored doesn't clobber the
	// true original with an already-edited value.
	void recordOriginalInstructionIfNeeded(u32 address);
	void editBreakpoint();
	std::set<std::string> getSelectedLineArguments();
	void drawArguments(wxDC& dc, const DisassemblyLineInfo &line, int x, int y, wxColor& textColor,
		const std::set<std::string> &currentArguments);
	void showCallTree();
	void showGraphView();
	// Handlers de wxEVT_DESTROY das janelas não-modais de Call Tree/Graph
	// View, pra zerar o ponteiro de instância única quando o usuário fecha
	// a janela. Métodos membro de verdade em vez de lambda com captura no
	// Bind() -- lambdas com captura não compilam em wxEVT_DESTROY nesta
	// versão do wxWidgets 3.0 (wxEventFunctorFunctor exige um functor
	// default-construtível pra esse event tag, e uma lambda com [this] não
	// é). Bind com ponteiro de método membro sempre funciona.
	void onCallTreeDestroy(wxWindowDestroyEvent& evt) { activeCallTree_ = nullptr; evt.Skip(); }
	void onGraphViewDestroy(wxWindowDestroyEvent& evt) { activeGraphView_ = nullptr; evt.Skip(); }
	int getBackgroundColor(unsigned int address);
	void assembleOpcode(u32 address, std::string defaultText);

	void postEvent(wxEventType type, wxString text);
	void postEvent(wxEventType type, int value);

	void onPopupClick(wxCommandEvent& evt);

	void setCurAddress(u32 newAddress, bool extend = false)
	{
		newAddress = manager.getStartAddress(newAddress);
		u32 after = manager.getNthNextAddress(newAddress,1);
		curAddress = newAddress;
		selectRangeStart = extend ? std::min(selectRangeStart, newAddress) : newAddress;
		selectRangeEnd = extend ? std::max(selectRangeEnd, after) : after;
		updateStatusBarText();
	}

	void scrollAddressIntoView();
	struct {
		int addressStart;
		int opcodeStart;
		int argumentsStart;
		int arrowsStart;
	} pixelPositions;

	DebugInterface* cpu;
	DisassemblyManager manager;
	u32 windowStart;
	u32 curAddress;
	u32 selectRangeStart;
	u32 selectRangeEnd;
	int visibleRows;
	int rowHeight;
	int charWidth;
	bool displaySymbols;
	std::vector<u32> jumpStack;

	// Original 32-bit word at each address we've overwritten via "Replace with
	// NOP" or "Assemble Opcode(s)", captured the first time that address gets
	// edited (see recordOriginalInstructionIfNeeded()). restoreSelectedInstructions()
	// writes these back and drops the entry once restored. Deliberately an
	// in-memory, per-view map rather than anything persisted to disk: a savestate
	// load or reboot already resets the underlying RAM to its real original
	// contents on its own, so there's nothing meaningful left to "restore" for
	// this session's edits at that point (see clearFunctions()).
	std::map<u32, u32> m_originalInstructions;

	wxIcon bpEnabled,bpDisabled;
	wxMenu menu;

	// Instância única do Call Tree / Graph View (ver showCallTree()/
	// showGraphView()): em vez de empilhar uma janela nova a cada "botão
	// direito > Show Call Tree/Graph View", reaproveita a que já está
	// aberta (Raise()+SetFocus()). Zerado via wxEVT_DESTROY quando o
	// usuário fecha a janela, então não fica um ponteiro pendurado.
	CallTreeDialog* activeCallTree_ = nullptr;
	GraphViewDialog* activeGraphView_ = nullptr;
};