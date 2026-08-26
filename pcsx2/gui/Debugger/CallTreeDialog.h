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
#include <wx/scrolwin.h>
#include <map>
#include <vector>

#include "DebugTools/DebugInterface.h"
#include "DebugTools/DisassemblyManager.h"

class CtrlDisassemblyView;
class CallGraphCanvas;

// Janela do "Show Call Tree": só hospeda o CallGraphCanvas, que é quem
// desenha o grafo de verdade (ver abaixo).
//
// É uma wxFrame flutuante, NÃO um wxDialog modal: antes disso era mostrada
// com ShowModal(), o que travava toda a interação com a janela do debugger
// por trás enquanto a árvore estivesse aberta. Como wxFrame, quem a cria
// (CtrlDisassemblyView::showCallTree()) faz `new CallTreeDialog(...)` e
// chama só `Show()` -- a janela fica flutuando ao lado do debugger sem
// bloqueá-lo, e se autodestrói (comportamento padrão de wxFrame) quando o
// usuário a fecha.
class CallTreeDialog : public wxFrame
{
public:
	CallTreeDialog(CtrlDisassemblyView* parent, DebugInterface* cpu, DisassemblyManager* manager, u32 rootFuncAddr);

	// Troca a função raiz exibida e reconstrói o grafo -- usado quando a
	// janela já está aberta (instância única, ver
	// CtrlDisassemblyView::showCallTree()) e o usuário pede "Show Call
	// Tree" de novo numa função diferente. Sem isso, reaproveitar a janela
	// existente só trazia ela pra frente com a árvore antiga, sem
	// atualizar pra função nova (mesmo bug que existia no Graph View, ver
	// GraphViewDialog::rebuildForFunction()).
	void rebuildForFunction(u32 newRootFuncAddr);

private:
	CallGraphCanvas* canvas;
};

// Desenha o grafo de xrefs (quem chama quem) como caixas conectadas por
// setas, no estilo do "Xrefs to" do IDA/WinGraph32 (ver print de
// referência): a função raiz fica embaixo, os chamadores dela sobem em
// camadas até a primeira função sem chamador conhecido (normalmente
// "start"/main). Cada aresta é um call site (endereço de uma instrução
// JAL) -- clique num nó seleciona, duplo-clique vai até ele no
// disassembler, botão direito dá "Go to Function" / "Redirect call..." /
// "Expand" (quando o nó foi cortado por atingir o limite de
// profundidade/nós).
class CallGraphCanvas : public wxScrolledWindow
{
public:
	CallGraphCanvas(wxWindow* parent, CtrlDisassemblyView* disassemblyView, DebugInterface* cpu, DisassemblyManager* manager, u32 rootFuncAddr);

	void rebuild();

	// Troca a função raiz e reconstrói (ver CallTreeDialog::rebuildForFunction).
	// Reseta o zoom pra 1.0 -- reaproveitar o zoom/pan do grafo anterior
	// deixaria o novo grafo fora de proporção na maioria dos casos (mesmo
	// raciocínio de GraphViewCanvas::setFunction).
	void setRoot(u32 newRootFuncAddr) { rootFuncAddr = newRootFuncAddr; zoom_ = 1.0; rebuild(); }

private:
	struct GraphNode
	{
		u32 funcAddr;
		int layer;               // 0 = raiz, aumenta subindo em direção aos chamadores
		wxRect rect;              // em coordenadas LÓGICAS (sem zoom aplicado)
		bool truncated = false;   // bateu o limite de nós/profundidade, tem chamadores não expandidos
		bool expanded = false;    // já tentou popular os chamadores deste nó

		// Nó-grupo: em vez de desenhar uma caixa por chamador quando um nó
		// tem mais chamadores do que kGroupThreshold (é o que fazia a camada
		// "estourar" pra fora da janela em funções tipo Malloc/RegisterResource
		// chamadas de dezenas de lugares), esses chamadores viram UMA caixa
		// "N callers" só. Clicar nela abre um popup listando cada um; escolher
		// um item ali expande aquele chamador específico como nó normal na
		// árvore (mesmo caminho de expandNode), então dá pra continuar
		// explorando a partir dele.
		bool isGroup = false;
		std::vector<u32> groupCallSites; // call sites representados por este grupo
	};

	struct GraphEdge
	{
		int fromNode; // camada de cima (quem chama / caller) -- ou nó-grupo
		int toNode;   // camada de baixo (quem é chamado / callee)
		u32 callSiteAddr; // 0 quando fromNode é um nó-grupo (agregado, sem call site único)
	};

	void buildGraph();
	void layoutGraph();
	void updateVirtualSizeForZoom();
	wxString labelForNode(const GraphNode& node) const;

	// Expande um chamador específico escolhido no popup de um nó-grupo pra
	// virar um nó de verdade na árvore (com sua própria caixa e, se ele
	// também tiver chamadores, elegível a ser expandido de novo depois).
	void expandGroupMember(int groupNodeIndex, u32 callSiteAddr);

	void onPaint(wxPaintEvent& evt);
	void onLeftDown(wxMouseEvent& evt);
	void onLeftUp(wxMouseEvent& evt);
	void onMotion(wxMouseEvent& evt);
	void onMouseCaptureLost(wxMouseCaptureLostEvent& evt);
	void onMouseWheel(wxMouseEvent& evt);
	void onLeftDClick(wxMouseEvent& evt);
	void onRightDown(wxMouseEvent& evt);
	void onPopupClick(wxCommandEvent& evt);
	void showGroupPopup(int groupNodeIndex, const wxPoint& screenPos);

	int hitTest(const wxPoint& logicalPos) const;
	void gotoNode(int nodeIndex);

	CtrlDisassemblyView* disassemblyView;
	DebugInterface* cpu;
	DisassemblyManager* manager;
	u32 rootFuncAddr;

	std::vector<GraphNode> nodes;
	std::vector<GraphEdge> edges;
	int selectedNode = -1;
	int contextNode = -1;

	// Tamanho lógico (sem zoom) calculado por layoutGraph(); o tamanho
	// virtual real da wxScrolledWindow é isso multiplicado por zoom_,
	// recalculado em updateVirtualSizeForZoom() toda vez que o zoom muda.
	wxSize logicalSize;
	double zoom_ = 1.0;
	static constexpr double kMinZoom = 0.25;
	static constexpr double kMaxZoom = 2.5;

	// Pan: arrastar com botão esquerdo no vazio (fora de qualquer caixa).
	bool isPanning = false;
	wxPoint panStartMouse;
	int panStartScrollX = 0, panStartScrollY = 0;

	// Arrastar um nó específico (em qualquer direção) pra reposicioná-lo
	// manualmente, sobrepondo o resultado do auto-layout pra aquele nó só.
	// Distinto de isPanning: aqui o clique começou EM CIMA de um nó, não no
	// vazio. didDrag distingue "foi um clique" (ação normal: seleciona /
	// abre grupo) de "foi um arrasto de verdade" (só reposiciona, sem
	// disparar a ação de clique) usando um limiar de poucos pixels.
	int draggingNode = -1;
	wxPoint dragStartMouse;
	wxPoint dragStartNodeOrigin;
	bool didDragNode = false;

	// Limites pra não travar a UI/virar sopa de caixas num binário grande
	// onde a função raiz (tipo FrameUpdate_) é chamada de centenas de
	// lugares direta ou indiretamente. Nó que bate o limite fica marcado
	// truncated=true e ganha "Expand" no menu de contexto pra continuar
	// subindo a partir dali sob demanda.
	static const int kMaxNodes = 150;
	static const int kMaxLayers = 12;
	// Mais chamadores diretos que isso numa função e eles viram um nó-grupo
	// só, em vez de uma caixa cada (ver GraphNode::isGroup acima).
	static const int kGroupThreshold = 6;

	wxDECLARE_EVENT_TABLE();
};
