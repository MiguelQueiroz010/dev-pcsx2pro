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
#include "CallTreeDialog.h"
#include "CtrlDisassemblyView.h"

#include <wx/dcbuffer.h>
#include <cmath>
#include <algorithm>

namespace
{
	const int kBoxHeight = 34;
	const int kBoxHPad = 14;
	const int kBoxHGap = 26;
	const int kLayerHeight = 90;
	const int kMargin = 30;

	enum CallGraphPopupIdentifiers
	{
		ID_CALLGRAPH_GOTO = 1,
		ID_CALLGRAPH_EXPAND,
		ID_CALLGRAPH_REDIRECT_BASE, // + índice da aresta de entrada escolhida (reserva um bloco grande)
		ID_CALLGRAPH_REDIRECT_MAX = ID_CALLGRAPH_REDIRECT_BASE + 100000,
		ID_CALLGRAPH_GROUPMEMBER_BASE = ID_CALLGRAPH_REDIRECT_MAX, // + índice dentro de groupCallSites
	};
}

// ================= CallTreeDialog =================

CallTreeDialog::CallTreeDialog(CtrlDisassemblyView* parent, DebugInterface* cpu, DisassemblyManager* manager, u32 rootFuncAddr)
	// wxFRAME_FLOAT_ON_PARENT mantém a janela acima do debugger sem prendê-lo
	// (diferente de ShowModal(), que desabilitava toda a janela por trás).
	: wxFrame(parent, wxID_ANY, L"Call Tree", wxDefaultPosition, wxSize(900, 640),
		wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT)
{
	wxPanel* panel = new wxPanel(this);
	wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
	canvas = new CallGraphCanvas(panel, parent, cpu, manager, rootFuncAddr);
	sizer->Add(canvas, 1, wxEXPAND);

	wxStaticText* hint = new wxStaticText(panel, wxID_ANY,
		L"Click: select / open group. Double-click: go to function. Drag empty space: pan. Ctrl+Scroll: zoom. Right-click: options. "
		L"Esta janela não é modal -- o debugger continua acessível com ela aberta.");
	sizer->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

	panel->SetSizer(sizer);
	wxBoxSizer* outer = new wxBoxSizer(wxVERTICAL);
	outer->Add(panel, 1, wxEXPAND);
	SetSizer(outer);

	CenterOnParent();
}

void CallTreeDialog::rebuildForFunction(u32 newRootFuncAddr)
{
	canvas->setRoot(newRootFuncAddr);
	// Volta a visão pro canto (0,0) -- reaproveitar a árvore com o
	// scroll/zoom da função anterior deixaria o novo grafo fora da área
	// visível na maioria dos casos (mesmo raciocínio de
	// GraphViewDialog::rebuildForFunction()).
	canvas->Scroll(0, 0);
	canvas->Refresh();
}

// ================= CallGraphCanvas =================

wxBEGIN_EVENT_TABLE(CallGraphCanvas, wxScrolledWindow)
	EVT_PAINT(CallGraphCanvas::onPaint)
	EVT_LEFT_DOWN(CallGraphCanvas::onLeftDown)
	EVT_LEFT_UP(CallGraphCanvas::onLeftUp)
	EVT_MOTION(CallGraphCanvas::onMotion)
	EVT_MOUSE_CAPTURE_LOST(CallGraphCanvas::onMouseCaptureLost)
	EVT_MOUSEWHEEL(CallGraphCanvas::onMouseWheel)
	EVT_LEFT_DCLICK(CallGraphCanvas::onLeftDClick)
	EVT_RIGHT_DOWN(CallGraphCanvas::onRightDown)
wxEND_EVENT_TABLE()

CallGraphCanvas::CallGraphCanvas(wxWindow* parent, CtrlDisassemblyView* _disassemblyView, DebugInterface* _cpu, DisassemblyManager* _manager, u32 _rootFuncAddr)
	: wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SUNKEN | wxHSCROLL | wxVSCROLL)
	, disassemblyView(_disassemblyView)
	, cpu(_cpu)
	, manager(_manager)
	, rootFuncAddr(_rootFuncAddr)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetBackgroundColour(*wxWHITE);
	// 1px por unidade de scroll -- não 20 -- pra dar pan suave/preciso igual
	// ao IDA, sem o entalado de mover em saltos de 20px. Ver comentário em
	// onMotion() sobre o Refresh() extra que essa mesma pane de código precisa.
	SetScrollRate(1, 1);
	rebuild();
}

void CallGraphCanvas::rebuild()
{
	buildGraph();
	layoutGraph();
	updateVirtualSizeForZoom();
	Refresh();
}

wxString CallGraphCanvas::labelForNode(const GraphNode& node) const
{
	if (node.isGroup)
		return wxString::Format(L"%zu callers…", node.groupCallSites.size());

	std::string name = cpu->GetSymbolMap().GetLabelString(node.funcAddr);
	wxString label;
	if (!name.empty())
		label = wxString(name.c_str(), wxConvUTF8);
	else
		label.Printf(L"sub_%08X", node.funcAddr);
	return label;
}

void CallGraphCanvas::buildGraph()
{
	nodes.clear();
	edges.clear();
	selectedNode = -1;
	contextNode = -1;

	std::map<u32, int> addrToIndex;

	GraphNode root;
	root.funcAddr = rootFuncAddr;
	root.layer = 0;
	nodes.push_back(root);
	addrToIndex[rootFuncAddr] = 0;

	// BFS por índice (não por referência -- nodes pode realocar a cada
	// push_back, então tudo que precisa sobreviver a um push_back é lido de
	// novo por índice depois).
	std::vector<int> queue;
	queue.push_back(0);

	for (size_t qHead = 0; qHead < queue.size(); qHead++)
	{
		int idx = queue[qHead];
		u32 funcAddr = nodes[idx].funcAddr;
		int layer = nodes[idx].layer;
		nodes[idx].expanded = true;

		if (nodes[idx].isGroup)
			continue; // nó-grupo não expande sozinho, só via clique do usuário

		const std::vector<u32>& callers = manager->GetCallGraph().GetCallers(cpu, funcAddr);
		if (callers.empty())
			continue;

		if (layer + 1 > kMaxLayers)
		{
			nodes[idx].truncated = true;
			continue;
		}

		// Conta quantos chamadores DISTINTOS (por função, não por call site)
		// esse nó tem antes de decidir se agrupa. Uma função com 20 call
		// sites mas só 2 funções chamadoras não precisa virar grupo.
		std::map<u32, std::vector<u32>> callersByFunc;
		for (u32 callSiteAddr : callers)
		{
			u32 callerFunc = cpu->GetSymbolMap().GetFunctionStart(callSiteAddr);
			if (callerFunc == 0xFFFFFFFF)
				callerFunc = callSiteAddr;
			if (callerFunc == funcAddr)
				continue; // auto-recursão direta -- não desenha aresta pra si mesmo
			callersByFunc[callerFunc].push_back(callSiteAddr);
		}

		if ((int)callersByFunc.size() > kGroupThreshold)
		{
			if ((int)nodes.size() >= kMaxNodes)
			{
				nodes[idx].truncated = true;
				continue;
			}

			GraphNode group;
			group.isGroup = true;
			group.layer = layer + 1;
			group.expanded = true; // nó-grupo não expande via BFS, só via popup
			for (auto& pair : callersByFunc)
				group.groupCallSites.push_back(pair.second.front()); // um call site representativo por função
			nodes.push_back(group);
			int groupIdx = (int)nodes.size() - 1;

			GraphEdge e;
			e.fromNode = groupIdx;
			e.toNode = idx;
			e.callSiteAddr = 0; // agregado -- sem "Redirect" nessa aresta
			edges.push_back(e);
			continue;
		}

		for (auto& pair : callersByFunc)
		{
			u32 callerFunc = pair.first;
			auto it = addrToIndex.find(callerFunc);
			int callerIdx;
			if (it != addrToIndex.end())
			{
				callerIdx = it->second;
			}
			else
			{
				if ((int)nodes.size() >= kMaxNodes)
				{
					nodes[idx].truncated = true;
					continue;
				}

				GraphNode n;
				n.funcAddr = callerFunc;
				n.layer = layer + 1;
				nodes.push_back(n);
				callerIdx = (int)nodes.size() - 1;
				addrToIndex[callerFunc] = callerIdx;
				queue.push_back(callerIdx);
			}

			for (u32 callSiteAddr : pair.second)
			{
				GraphEdge e;
				e.fromNode = callerIdx;
				e.toNode = idx;
				e.callSiteAddr = callSiteAddr;
				edges.push_back(e);
			}
		}
	}
}

void CallGraphCanvas::expandGroupMember(int groupNodeIndex, u32 callSiteAddr)
{
	if (groupNodeIndex < 0 || groupNodeIndex >= (int)nodes.size())
		return;

	int toIdx = -1;
	for (const GraphEdge& e : edges)
	{
		if (e.fromNode == groupNodeIndex)
		{
			toIdx = e.toNode;
			break;
		}
	}
	if (toIdx < 0)
		return;

	u32 callerFunc = cpu->GetSymbolMap().GetFunctionStart(callSiteAddr);
	if (callerFunc == 0xFFFFFFFF)
		callerFunc = callSiteAddr;

	// Já existe um nó de verdade pra essa função nessa camada? (usuário pode
	// abrir o grupo, expandir um membro, depois abrir o grupo de novo e
	// escolher o mesmo -- não duplica, só acrescenta a aresta.)
	for (int i = 0; i < (int)nodes.size(); i++)
	{
		if (!nodes[i].isGroup && nodes[i].funcAddr == callerFunc && nodes[i].layer == nodes[groupNodeIndex].layer)
		{
			GraphEdge e;
			e.fromNode = i;
			e.toNode = toIdx;
			e.callSiteAddr = callSiteAddr;
			edges.push_back(e);
			layoutGraph();
			updateVirtualSizeForZoom();
			Refresh();
			return;
		}
	}

	if ((int)nodes.size() >= kMaxNodes)
	{
		wxMessageBox(L"Node limit reached; cannot expand more callers in this tree.", L"Call Tree", wxICON_INFORMATION, this);
		return;
	}

	GraphNode n;
	n.funcAddr = callerFunc;
	n.layer = nodes[groupNodeIndex].layer;
	nodes.push_back(n);
	int childIdx = (int)nodes.size() - 1;

	GraphEdge e;
	e.fromNode = childIdx;
	e.toNode = toIdx;
	e.callSiteAddr = callSiteAddr;
	edges.push_back(e);

	layoutGraph();
	updateVirtualSizeForZoom();
	Refresh();
}

void CallGraphCanvas::layoutGraph()
{
	if (nodes.empty())
	{
		logicalSize = wxSize(kMargin * 2, kMargin * 2);
		return;
	}

	wxClientDC dc(this);
	dc.SetFont(GetFont());

	int maxLayer = 0;
	std::map<int, std::vector<int>> byLayer;
	for (int i = 0; i < (int)nodes.size(); i++)
	{
		byLayer[nodes[i].layer].push_back(i);
		maxLayer = std::max(maxLayer, nodes[i].layer);
	}

	// Passo 1: largura de cada caixa (texto + padding) e largura total de
	// cada camada colocando as caixas lado a lado.
	int maxLayerWidth = 0;
	std::map<int, int> layerWidth;
	for (auto& layerPair : byLayer)
	{
		int x = 0;
		for (int idx : layerPair.second)
		{
			wxString label = labelForNode(nodes[idx]);
			if (nodes[idx].truncated)
				label += L" …";
			wxSize extent = dc.GetTextExtent(label);
			int w = extent.GetWidth() + kBoxHPad * 2;
			nodes[idx].rect = wxRect(x, 0, w, kBoxHeight);
			x += w + kBoxHGap;
		}
		int width = x > 0 ? x - kBoxHGap : 0;
		layerWidth[layerPair.first] = width;
		maxLayerWidth = std::max(maxLayerWidth, width);
	}

	// Passo 2: centraliza cada camada dentro da largura máxima e define Y
	// (camada 0 = raiz embaixo, camadas maiores sobem). Agrupar chamadores
	// excedentes num nó-grupo (ver buildGraph) é o que mantém maxLayerWidth
	// (e portanto a janela) num tamanho razoável mesmo pra funções chamadas
	// de dezenas de lugares diferentes.
	int totalHeight = kMargin * 2 + (maxLayer + 1) * kLayerHeight;
	for (auto& layerPair : byLayer)
	{
		int layer = layerPair.first;
		int offsetX = kMargin + (maxLayerWidth - layerWidth[layer]) / 2;
		int y = kMargin + (maxLayer - layer) * kLayerHeight;
		for (int idx : layerPair.second)
		{
			nodes[idx].rect.SetX(nodes[idx].rect.GetX() + offsetX);
			nodes[idx].rect.SetY(y);
		}
	}

	logicalSize = wxSize(maxLayerWidth + kMargin * 2, totalHeight);
}

void CallGraphCanvas::updateVirtualSizeForZoom()
{
	SetVirtualSize((int)(logicalSize.GetWidth() * zoom_), (int)(logicalSize.GetHeight() * zoom_));
}

void CallGraphCanvas::onPaint(wxPaintEvent& evt)
{
	wxAutoBufferedPaintDC dc(this);
	DoPrepareDC(dc);
	dc.Clear();
	dc.SetUserScale(zoom_, zoom_);
	dc.SetFont(GetFont());

	// Arestas primeiro (ficam atrás das caixas).
	for (const GraphEdge& e : edges)
	{
		const wxRect& from = nodes[e.fromNode].rect; // caller (camada de cima) -- ou nó-grupo
		const wxRect& to = nodes[e.toNode].rect;      // callee (camada de baixo)

		wxPoint start(from.GetX() + from.GetWidth() / 2, from.GetY() + from.GetHeight());
		wxPoint end(to.GetX() + to.GetWidth() / 2, to.GetY());

		dc.SetPen(wxPen(wxColour(90, 90, 90), 1));
		dc.DrawLine(start, end);

		double angle = atan2((double)(end.y - start.y), (double)(end.x - start.x));
		const double arrowLen = 9.0;
		const double arrowAngle = 0.5;
		wxPoint p1(end.x - (int)(arrowLen * cos(angle - arrowAngle)), end.y - (int)(arrowLen * sin(angle - arrowAngle)));
		wxPoint p2(end.x - (int)(arrowLen * cos(angle + arrowAngle)), end.y - (int)(arrowLen * sin(angle + arrowAngle)));
		dc.SetBrush(wxBrush(wxColour(90, 90, 90)));
		wxPoint arrowPoly[3] = {end, p1, p2};
		dc.DrawPolygon(3, arrowPoly);
	}

	// Depois as caixas.
	for (int i = 0; i < (int)nodes.size(); i++)
	{
		const GraphNode& node = nodes[i];

		wxColour fill = wxColour(40, 40, 40);
		wxColour text = *wxWHITE;
		if (!node.isGroup && node.funcAddr == rootFuncAddr)
			fill = wxColour(30, 90, 200); // raiz em azul, igual ao print de referência
		if (node.isGroup)
			fill = wxColour(100, 40, 120); // roxo: nó-grupo, clique abre lista
		if (node.truncated)
			fill = wxColour(120, 60, 20); // laranja escuro: tem mais chamadores não expandidos
		if (i == selectedNode)
			fill = fill.ChangeLightness(140);

		dc.SetBrush(wxBrush(fill));
		dc.SetPen(wxPen(i == selectedNode ? *wxYELLOW : wxColour(20, 20, 20), i == selectedNode ? 2 : 1));
		dc.DrawRoundedRectangle(node.rect, 4);

		dc.SetTextForeground(text);
		wxString label = labelForNode(node);
		if (node.truncated)
			label += L" …";
		wxSize extent = dc.GetTextExtent(label);
		dc.DrawText(label, node.rect.GetX() + (node.rect.GetWidth() - extent.GetWidth()) / 2,
			node.rect.GetY() + (node.rect.GetHeight() - extent.GetHeight()) / 2);
	}
}

int CallGraphCanvas::hitTest(const wxPoint& logicalPos) const
{
	for (int i = 0; i < (int)nodes.size(); i++)
	{
		if (nodes[i].rect.Contains(logicalPos))
			return i;
	}
	return -1;
}

void CallGraphCanvas::onLeftDown(wxMouseEvent& evt)
{
	wxPoint device = CalcUnscrolledPosition(evt.GetPosition());
	wxPoint logical((int)(device.x / zoom_), (int)(device.y / zoom_));
	int idx = hitTest(logical);

	if (idx < 0)
	{
		// Clique no vazio: começa a arrastar a visão (pan), em vez de só
		// desmarcar seleção -- é o jeito mais natural de navegar num grafo
		// grande sem depender só das barras de rolagem.
		isPanning = true;
		panStartMouse = evt.GetPosition();
		int ppuX, ppuY;
		GetScrollPixelsPerUnit(&ppuX, &ppuY);
		wxPoint viewStart = GetViewStart();
		panStartScrollX = viewStart.x * ppuX;
		panStartScrollY = viewStart.y * ppuY;
		CaptureMouse();
		SetCursor(wxCursor(wxCURSOR_SIZING));
		selectedNode = -1;
		Refresh();
		return;
	}

	// Clique EM CIMA de um nó: prepara um possível arrasto do nó em si (em
	// qualquer direção -- X e Y livres). A ação de clique de verdade
	// (selecionar / abrir popup de grupo) só dispara em onLeftUp se o mouse
	// não passou do limiar de arrasto -- ver didDragNode.
	draggingNode = idx;
	dragStartMouse = evt.GetPosition();
	dragStartNodeOrigin = nodes[idx].rect.GetPosition();
	didDragNode = false;
	CaptureMouse();
	selectedNode = idx;
	Refresh();
}

void CallGraphCanvas::onLeftUp(wxMouseEvent& evt)
{
	if (draggingNode >= 0)
	{
		int idx = draggingNode;
		draggingNode = -1;
		if (HasCapture())
			ReleaseMouse();

		if (!didDragNode)
		{
			// Foi um clique de verdade (não um arrasto) -- executa a ação
			// normal que antes ficava em onLeftDown.
			if (nodes[idx].isGroup)
				showGroupPopup(idx, evt.GetPosition());
		}
		didDragNode = false;
		Refresh();
		return;
	}

	if (isPanning)
	{
		isPanning = false;
		if (HasCapture())
			ReleaseMouse();
		SetCursor(wxNullCursor);
	}
	evt.Skip();
}

void CallGraphCanvas::onMotion(wxMouseEvent& evt)
{
	if (draggingNode >= 0 && evt.LeftIsDown())
	{
		wxPoint deltaScreen = evt.GetPosition() - dragStartMouse;
		if (!didDragNode && (std::abs(deltaScreen.x) > 3 || std::abs(deltaScreen.y) > 3))
			didDragNode = true;

		if (didDragNode)
		{
			// Livre nas duas direções -- X e Y não são travados a nenhum
			// eixo, diferente do layout automático que só varia em Y por
			// camada.
			wxPoint newPos = dragStartNodeOrigin + wxPoint((int)(deltaScreen.x / zoom_), (int)(deltaScreen.y / zoom_));
			newPos.x = std::max(0, newPos.x);
			newPos.y = std::max(0, newPos.y);
			nodes[draggingNode].rect.SetPosition(newPos);

			// Estica o tamanho lógico se o nó foi arrastado pra fora da
			// área conhecida, senão a barra de rolagem não alcança ele.
			int rightEdge = newPos.x + nodes[draggingNode].rect.GetWidth() + kMargin;
			int bottomEdge = newPos.y + nodes[draggingNode].rect.GetHeight() + kMargin;
			bool resized = false;
			if (rightEdge > logicalSize.GetWidth()) { logicalSize.SetWidth(rightEdge); resized = true; }
			if (bottomEdge > logicalSize.GetHeight()) { logicalSize.SetHeight(bottomEdge); resized = true; }
			if (resized)
				updateVirtualSizeForZoom();

			Refresh(false);
		}
		return;
	}

	if (isPanning && evt.LeftIsDown())
	{
		wxPoint delta = evt.GetPosition() - panStartMouse;
		int ppuX, ppuY;
		GetScrollPixelsPerUnit(&ppuX, &ppuY);
		if (ppuX <= 0) ppuX = 1;
		if (ppuY <= 0) ppuY = 1;

		int newPixelX = panStartScrollX - delta.x;
		int newPixelY = panStartScrollY - delta.y;
		Scroll(newPixelX / ppuX, newPixelY / ppuY);
		// Scroll() sozinho não garante repaint em toda plataforma/versão do wx
		// quando a janela usa wxBG_STYLE_PAINT + DC bufferizado com redesenho
		// manual completo (nosso caso) -- é o que fazia a posição de scroll
		// mudar de verdade internamente (por isso o cursor de "mover" já
		// aparecia certinho) sem o conteúdo visualmente acompanhar. Refresh(false)
		// força o repaint sem repetir o apagamento de fundo que o onPaint já faz
		// sozinho via dc.Clear().
		Refresh(false);
		return;
	}
	evt.Skip();
}

void CallGraphCanvas::onMouseCaptureLost(wxMouseCaptureLostEvent& evt)
{
	isPanning = false;
	draggingNode = -1;
	didDragNode = false;
	SetCursor(wxNullCursor);
}

void CallGraphCanvas::onMouseWheel(wxMouseEvent& evt)
{
	if (!evt.ControlDown())
	{
		evt.Skip();
		return;
	}

	// Ctrl+Scroll: zoom centrado aproximadamente no cursor -- guarda a
	// posição lógica sob o mouse antes de mudar o zoom e realinha o scroll
	// depois, senão dar zoom sempre puxa a visão pro canto (0,0).
	wxPoint device = CalcUnscrolledPosition(evt.GetPosition());
	wxPoint logicalUnderMouse((int)(device.x / zoom_), (int)(device.y / zoom_));

	double step = evt.GetWheelRotation() > 0 ? 1.1 : (1.0 / 1.1);
	zoom_ = std::min(kMaxZoom, std::max(kMinZoom, zoom_ * step));

	updateVirtualSizeForZoom();

	int newDeviceX = (int)(logicalUnderMouse.x * zoom_) - evt.GetPosition().x;
	int newDeviceY = (int)(logicalUnderMouse.y * zoom_) - evt.GetPosition().y;
	int ppuX, ppuY;
	GetScrollPixelsPerUnit(&ppuX, &ppuY);
	if (ppuX <= 0) ppuX = 1;
	if (ppuY <= 0) ppuY = 1;
	Scroll(newDeviceX / ppuX, newDeviceY / ppuY);

	Refresh();
}

void CallGraphCanvas::onLeftDClick(wxMouseEvent& evt)
{
	wxPoint device = CalcUnscrolledPosition(evt.GetPosition());
	wxPoint logical((int)(device.x / zoom_), (int)(device.y / zoom_));
	int idx = hitTest(logical);
	if (idx < 0 || nodes[idx].isGroup)
		return;
	gotoNode(idx);
}

void CallGraphCanvas::gotoNode(int nodeIndex)
{
	if (nodeIndex < 0 || nodeIndex >= (int)nodes.size() || nodes[nodeIndex].isGroup)
		return;
	disassemblyView->gotoAddress(nodes[nodeIndex].funcAddr);
}

void CallGraphCanvas::showGroupPopup(int groupNodeIndex, const wxPoint& screenPos)
{
	// Lista de intermediação pedida: cada chamador agrupado vira um item de
	// menu (que no wx já rola sozinho se a lista for grande); escolher um
	// chama expandGroupMember() pra transformá-lo num nó de verdade na árvore.
	wxMenu popup;
	const GraphNode& group = nodes[groupNodeIndex];
	for (int i = 0; i < (int)group.groupCallSites.size(); i++)
	{
		u32 callSiteAddr = group.groupCallSites[i];
		u32 funcAddr = cpu->GetSymbolMap().GetFunctionStart(callSiteAddr);
		if (funcAddr == 0xFFFFFFFF)
			funcAddr = callSiteAddr;

		std::string name = cpu->GetSymbolMap().GetLabelString(funcAddr);
		wxString label = !name.empty() ? wxString(name.c_str(), wxConvUTF8) : wxString::Format(L"sub_%08X", funcAddr);

		popup.Append(ID_CALLGRAPH_GROUPMEMBER_BASE + i, label);
	}

	contextNode = groupNodeIndex;
	popup.Bind(wxEVT_MENU, &CallGraphCanvas::onPopupClick, this);
	PopupMenu(&popup, screenPos);
}

void CallGraphCanvas::onRightDown(wxMouseEvent& evt)
{
	wxPoint device = CalcUnscrolledPosition(evt.GetPosition());
	wxPoint logical((int)(device.x / zoom_), (int)(device.y / zoom_));
	int idx = hitTest(logical);
	if (idx < 0)
		return;

	contextNode = idx;

	if (nodes[idx].isGroup)
	{
		showGroupPopup(idx, evt.GetPosition());
		return;
	}

	wxMenu popup;
	popup.Append(ID_CALLGRAPH_GOTO, L"Go to Function");

	if (nodes[idx].truncated)
		popup.Append(ID_CALLGRAPH_EXPAND, L"Expand (show more callers)");

	// Uma entrada de "Redirect" por aresta de entrada real (call site == 0
	// significa que a aresta vem de um nó-grupo agregado, sem uma instrução
	// JAL única pra reescrever -- essas não entram aqui).
	std::vector<int> incoming;
	for (int i = 0; i < (int)edges.size(); i++)
	{
		if (edges[i].toNode == idx && edges[i].callSiteAddr != 0)
			incoming.push_back(i);
	}

	if (!incoming.empty())
	{
		popup.AppendSeparator();
		for (int edgeIdx : incoming)
		{
			const GraphEdge& e = edges[edgeIdx];
			wxString callerLabel = labelForNode(nodes[e.fromNode]);
			wxString item = wxString::Format(L"Redirect call from %s (%08X)...", callerLabel, e.callSiteAddr);
			popup.Append(ID_CALLGRAPH_REDIRECT_BASE + edgeIdx, item);
		}
	}

	popup.Bind(wxEVT_MENU, &CallGraphCanvas::onPopupClick, this);
	PopupMenu(&popup, evt.GetPosition());
}

void CallGraphCanvas::onPopupClick(wxCommandEvent& evt)
{
	int id = evt.GetId();

	if (id == ID_CALLGRAPH_GOTO)
	{
		gotoNode(contextNode);
		return;
	}

	if (id == ID_CALLGRAPH_EXPAND)
	{
		rebuild();
		return;
	}

	if (id >= ID_CALLGRAPH_GROUPMEMBER_BASE)
	{
		int memberIdx = id - ID_CALLGRAPH_GROUPMEMBER_BASE;
		if (contextNode < 0 || contextNode >= (int)nodes.size())
			return;
		const GraphNode& group = nodes[contextNode];
		if (memberIdx < 0 || memberIdx >= (int)group.groupCallSites.size())
			return;
		expandGroupMember(contextNode, group.groupCallSites[memberIdx]);
		return;
	}

	if (id >= ID_CALLGRAPH_REDIRECT_BASE)
	{
		int edgeIdx = id - ID_CALLGRAPH_REDIRECT_BASE;
		if (edgeIdx < 0 || edgeIdx >= (int)edges.size())
			return;

		u32 callSiteAddr = edges[edgeIdx].callSiteAddr;
		u32 defaultTarget = nodes[edges[edgeIdx].toNode].funcAddr;

		wxString newTargetStr = wxGetTextFromUser(
			L"Enter the new target address (hex) for this call:",
			L"Redirect call", wxString::Format(L"%08X", defaultTarget), this);
		if (newTargetStr.empty())
			return;

		unsigned long newTarget = 0;
		if (!newTargetStr.ToULong(&newTarget, 16))
		{
			wxMessageBox(L"Invalid hex address.", L"Error.", wxICON_ERROR, this);
			return;
		}

		if (disassemblyView->PatchCallTarget(callSiteAddr, (u32)newTarget))
			rebuild(); // cache de xref já foi invalidado dentro de PatchCallTarget
	}
}
