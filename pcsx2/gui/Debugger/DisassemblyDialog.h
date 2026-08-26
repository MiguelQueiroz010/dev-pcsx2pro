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
#include <wx/notebook.h>
#include <wx/treectrl.h>
#include <set>
#include <functional>
#include "gui/App.h"

#include "CtrlDisassemblyView.h"
#include "CtrlRegisterList.h"
#include "CtrlMemView.h"
#include "CtrlMemSearch.h"
#include "DebugEvents.h"
#include "DebuggerLists.h"
#include "gui/MSWstuff.h"

class DebuggerHelpDialog: public wxDialog
{
public:
	DebuggerHelpDialog(wxWindow* parent);
};

inline int getDebugFontWidth()
{
	return (int) ceil(g_Conf->EmuOptions.Debugger.FontWidth*MSW_GetDPIScale());
}

inline int getDebugFontHeight()
{
	return (int)ceil(g_Conf->EmuOptions.Debugger.FontHeight*MSW_GetDPIScale());
}

class CpuTabPage: public wxPanel
{
public:
	CpuTabPage(wxWindow* parent, DebugInterface* _cpu);
	DebugInterface* getCpu() { return cpu; };
	CtrlDisassemblyView* getDisassembly() { return disassembly; };
	CtrlRegisterList* getRegisterList() { return registerList; };
	CtrlMemView* getMemoryView() { return memory; };
	CtrlMemSearch* getMemorySearch() { return memorySearch; };
	wxNotebook* getBottomTabs() { return bottomTabs; };
	void update();
	void showMemoryView() { setBottomTabPage(memory); };
	void loadCycles();
	void reloadSymbolMap();
	void clearSymbolMap();
	u32 getStepOutAddress();

	// Ctrl+F (ligado no acelerador da DisassemblyDialog, funciona de
	// qualquer lugar na aba da CPU): troca pra aba "Functions" e foca/seleciona
	// o texto da busca, pronto pra digitar.
	void focusFunctionSearch();

	wxDECLARE_EVENT_TABLE();
private:
	enum FunctionGroupMode
	{
		GROUP_BY_NAME,
		GROUP_BY_REGION,
	};

	// Precisa estar declarado ANTES de qualquer função que o use como tipo
	// (ex.: functionTreeNodeKey logo abaixo) -- usá-lo antes da definição
	// dava C2061 em todo .cpp que incluísse este header.
	struct FunctionTreeItemData : public wxTreeItemData
	{
		u32 address;
		bool isGroupHeader;
		FunctionTreeItemData(u32 _address, bool _isGroupHeader = false)
			: address(_address), isGroupHeader(_isGroupHeader) {}
	};

	void setBottomTabPage(wxWindow* win);
	void postEvent(wxEventType type, int value);

	// Reconstrói a wxTreeCtrl da aba Functions a partir de allFunctions_,
	// respeitando o modo de agrupamento atual (Nome/Região) e o texto da
	// busca (se houver, ignora o agrupamento e mostra lista plana filtrada).
	void rebuildFunctionTree();
	void populateFunctionTreeGrouped();
	void populateFunctionTreeFiltered(const wxString& filter);

	// Chave estável de um nó da árvore agrupada através de um rebuild (ver
	// definição em DisassemblyDialog.cpp) -- usada por capture/restore
	// abaixo pra manter expansão/seleção quando a árvore é reconstruída de
	// verdade (mudança de agrupamento, símbolos recarregados).
	wxString functionTreeNodeKey(const wxTreeItemId& item, FunctionTreeItemData* data);
	void captureFunctionTreeState(std::set<wxString>& expandedKeys, u32& selectedAddress);
	void restoreFunctionTreeState(const std::set<wxString>& expandedKeys, u32 selectedAddress);

	void onFunctionGroupChoice(wxCommandEvent& evt);
	void onFunctionSearchText(wxCommandEvent& evt);
	void onFunctionSearchKeyDown(wxKeyEvent& evt);
	void onFunctionTreeActivated(wxTreeEvent& evt);

	DebugInterface* cpu;
	CtrlDisassemblyView* disassembly;
	CtrlRegisterList* registerList;
	// Cache local da última leitura de GetAllSymbols(); evita re-consultar o
	// SymbolMap (que pega o mutex e copia strings) a cada tecla digitada na
	// busca ou troca de modo de agrupamento. Reconstruído em reloadSymbolMap().
	std::vector<SymbolEntry> allFunctions_;
	// Idem, pra ST_LABEL -- labels soltos (ex.: MODO_BATTLE) e labels dentro
	// do corpo de uma função aparecem como filhos dela na árvore agrupada.
	std::vector<SymbolEntry> allLabels_;
	wxPanel* functionsPanel;
	wxChoice* functionGroupChoice;
	wxTextCtrl* functionSearchBox;
	wxTreeCtrl* functionTree;
	// Segunda árvore, só pra resultado de busca (ver rebuildFunctionTree()
	// em DisassemblyDialog.cpp): troca de visibilidade com functionTree em
	// vez de a busca reconstruir a árvore agrupada (que pode ter milhares
	// de itens) a cada tecla digitada.
	wxTreeCtrl* functionSearchTree;
	// true quando functionTree reflete o estado atual de allFunctions_/
	// allLabels_/modo de agrupamento. Fica false (== "precisa reconstruir na
	// próxima vez que for exibida") após reloadSymbolMap() detectar símbolos
	// novos ou o usuário trocar o modo de agrupamento; nunca fica false só
	// por causa de digitar/apagar na busca.
	bool groupedTreeBuilt_ = false;
	CtrlMemView* memory;
	CtrlMemSearch* memorySearch;
	wxNotebook* bottomTabs;
	wxNotebook* leftTabs;
	BreakpointList* breakpointList;
	wxStaticText* cyclesText;
	ThreadList* threadList;
	StackFramesList* stackFrames;
	u32 lastCycles;
	// Última geração do SymbolMap (SymbolMap::GetGeneration()) vista por
	// reloadSymbolMap(). Comparar com isso em vez de só "symbolCount != 0" é o
	// que faz a lista reagir a um overlay carregado DEPOIS da primeira leitura
	// -- reloadSymbolMap() é chamado toda vez que o disassembler percebe uma
	// mudança de geração (ver GameDebuggerConfigManager/render-path trigger),
	// mas se só checarmos symbolCount, uma vez que ele passou de 0 a lista
	// nunca mais é reconstruída, mesmo que módulos/overlays novos tenham
	// entrado no SymbolMap. GetAllSymbols(ST_FUNCTION) só devolve símbolos de
	// módulos ATIVOS, então isso também é o que faz a Região sumir/aparecer
	// corretamente quando um overlay é trocado.
	u32 lastSymGeneration_;
	u32 symbolCount;
};

class DisassemblyDialog : public wxFrame
{
public:
	DisassemblyDialog( wxWindow* parent=NULL );
	virtual ~DisassemblyDialog() = default;

	static wxString GetNameStatic() { return L"DisassemblyDialog"; }
	wxString GetDialogName() const { return GetNameStatic(); }
	
	void update();
	void reset();
	void populate();
	void setDebugMode(bool debugMode, bool switchPC);

	wxDECLARE_EVENT_TABLE();
protected:
	void onBreakRunClicked(wxCommandEvent& evt);
	void onStepOverClicked(wxCommandEvent& evt);
	void onStepIntoClicked(wxCommandEvent& evt);
	void onStepOutClicked(wxCommandEvent& evt);
	void onHelpClicked(wxCommandEvent& evt);
	void onGameConfigClicked(wxCommandEvent& evt);
	void onDebuggerEvent(wxCommandEvent& evt);
	void onPageChanging(wxCommandEvent& evt);
	void onBreakpointClicked(wxCommandEvent& evt);
	void onSizeEvent(wxSizeEvent& event);
	void onFocusFunctionSearch(wxCommandEvent& evt);
	void onClose(wxCloseEvent& evt);
	void stepOver();
	void stepInto();
	void stepOut();
	void gotoPc();
private:
	CpuTabPage* eeTab;
	CpuTabPage* iopTab;
	CpuTabPage* currentCpu;
	wxNotebook* middleBook;

	wxBoxSizer* topSizer;
	wxButton *breakRunButton, *stepIntoButton, *stepOverButton, *stepOutButton, *breakpointButton, *gameConfigButton, *helpButton;
};
