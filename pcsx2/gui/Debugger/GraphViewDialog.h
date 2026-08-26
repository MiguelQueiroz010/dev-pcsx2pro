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
#include <string>

#include "DebugTools/DebugInterface.h"
#include "DebugTools/DisassemblyManager.h"

class CtrlDisassemblyView;
class GraphViewCanvas;

// Janela "Graph View" no estilo IDA Pro: desenha o corpo da função atual
// como blocos básicos (grupos de instruções em sequência reta) ligados por
// setas -- azul para fluxo incondicional (J/JR sem condição), verde com "T"
// para o desvio "tomado" de um branch condicional, vermelho com "F" para o
// "não tomado" (fallthrough) e laranja para os sucessores de uma switch/jump
// table resolvida heuristicamente. Todo alvo de branch dentro da função que
// não tenha símbolo ganha uma label sintética "pos_XXXXXXXX", igual ao
// "loc_" do IDA, mostrada no bloco de destino e referenciada na própria
// linha da instrução que salta pra lá.
//
// Assim como CallTreeDialog, é uma wxFrame flutuante e NÃO-MODAL: aberta com
// `new GraphViewDialog(...)` + `Show()` a partir de
// CtrlDisassemblyView::showGraphView(), sem bloquear o resto do debugger, e
// se autodestrói (padrão de wxFrame) quando o usuário a fecha.
class GraphViewDialog : public wxFrame
{
public:
	GraphViewDialog(CtrlDisassemblyView* parent, DebugInterface* cpu, DisassemblyManager* manager, u32 funcAddr);

	// Troca a função exibida e reconstrói o grafo -- usado quando a janela
	// já está aberta (instância única, ver CtrlDisassemblyView::showGraphView())
	// e o usuário pede "Graph View" de novo numa função diferente. Sem
	// isso, reaproveitar a janela existente só trazia ela pra frente com o
	// grafo antigo, sem atualizar pra função nova. Também atualiza o
	// título da janela pro nome da função nova.
	void rebuildForFunction(u32 newFuncAddr);

private:
	GraphViewCanvas* canvas;
	wxStaticText* hint;
	DebugInterface* cpu; // guardado só pra reconstruir o título em rebuildForFunction()
};

class GraphViewCanvas : public wxScrolledWindow
{
public:
	GraphViewCanvas(wxWindow* parent, CtrlDisassemblyView* disassemblyView, DebugInterface* cpu, DisassemblyManager* manager, u32 funcAddr);

	// Reconstrói o grafo (usado tanto no primeiro desenho quanto por
	// "Refresh" no menu de contexto, útil depois de editar código/redirecionar
	// uma chamada e voltar aqui).
	void rebuild();

	// Troca a função exibida e reconstrói (ver GraphViewDialog::rebuildForFunction).
	// Reseta o zoom pra 1.0 -- reaproveitar o zoom/pan do grafo anterior
	// deixaria o novo grafo fora de proporção na maioria dos casos. Também
	// limpa manualBlockAddrs_: blocos manuais pertencem ao grafo da função
	// anterior, não faz sentido eles reaparecerem no grafo de uma função
	// diferente.
	void setFunction(u32 newFuncAddr) { funcAddr = newFuncAddr; zoom_ = 1.0; manualBlockAddrs_.clear(); rebuild(); }

private:
	struct BasicBlock
	{
		u32 start = 0;
		u32 end = 0;                 // exclusivo
		std::vector<std::string> lines; // uma linha de texto formatada por instrução
		std::string label;            // "pos_XXXXXXXX" (ou símbolo já existente) quando este bloco é alvo de algum branch/jump
		wxRect rect;                  // coordenadas LÓGICAS (sem zoom)
		bool isExit = false;          // termina em retorno (jr $ra) ou salto por registrador sem alvo conhecido
		bool isSwitchDispatch = false; // termina em "jr $reg" resolvido como switch/jump table (ver tryDetectSwitchTargets)
		// Criado via "Add code block at address..." (ver manualBlockAddrs_),
		// não pela análise de fluxo normal da função -- desenhado numa cor
		// diferente (roxo) e começa sem nenhuma aresta até o usuário
		// redirecionar algum branch pra apontar pra ele.
		bool manual = false;
	};

	enum class EdgeKind
	{
		Unconditional, // J / JAL-como-jump / JR com alvo conhecido -- sempre segue por aqui
		Taken,         // desvio de um branch condicional quando a condição é verdadeira ("T")
		Fallthrough,   // sequência reta: o branch condicional não foi tomado, ou não havia branch nenhum aqui ("F")
		Switch         // sucessor de uma switch/jump table (jr $reg alimentado por uma tabela de endereços)
	};

	struct BlockEdge
	{
		int fromBlock;
		int toBlock;
		EdgeKind kind;
		// Endereço da instrução de branch/jump que gera esta aresta (0 pra
		// aresta "sem branch, cai reto pro próximo bloco" -- essa não tem
		// instrução própria pra redirecionar). Usado por "Redirect this
		// branch..." (menu de contexto ao clicar na seta) e por
		// computeEdgePath() pra rotear com base no endereço de origem real.
		u32 branchAddr = 0;
		// true quando este bloco foi criado manualmente via "Add code block
		// at address..." em vez de descoberto pela análise de fluxo normal
		// -- ver manualBlockAddrs_ abaixo.
		bool manual = false;
	};

	void buildBlocks();
	void layoutBlocks();
	// Reordena os blocos DENTRO de cada camada pra minimizar cruzamento de
	// setas, usando o método clássico de "barycenter" (Sugiyama/Eades):
	// várias passadas alternando pra baixo/pra cima, em cada uma reordenando
	// uma camada pela posição X média dos vizinhos já posicionados na camada
	// anterior. É isso que transforma a bagunça de linhas diagonais
	// cruzando tudo (ver print de referência) num layout onde a maioria das
	// arestas fica vertical ou quase, como o Graph View do IDA. Roda ANTES
	// de layoutBlocks() calcular X de verdade -- só decide a ORDEM dos
	// blocos em cada camada, não a posição em pixels.
	void reorderLayersToReduceCrossings(std::map<int, std::vector<int>>& byLayer, const std::vector<int>& layerOf);
	void updateVirtualSizeForZoom();
	wxColour colourForBlock(int index) const;

	// Caminho em cotovelo (ortogonal-ish) de uma aresta: desce reto a partir
	// do meio da base do bloco de origem, faz UMA quebra horizontal na
	// metade da distância vertical, desce reto até o topo do bloco de
	// destino. Arestas de volta (loop) saem/entram pela lateral em vez de
	// cruzar por cima das caixas no meio do caminho. Usado tanto pelo
	// desenho em onPaint() quanto por hitTestEdge() (mesma geometria pros
	// dois, senão o hover não bate com o que é desenhado).
	std::vector<wxPoint> computeEdgePath(const BlockEdge& e) const;
	// Índice da aresta cujo caminho passa a menos de kEdgeHitTolerance
	// pixels (lógicos) de logicalPos, ou -1. Testa segmento por segmento do
	// polyline de computeEdgePath().
	int hitTestEdge(const wxPoint& logicalPos) const;
	// Centraliza a visão (scroll) no bloco index e seleciona ele -- usado
	// tanto por gotoBlock() (duplo-clique/"Go to Address") quanto por
	// clicar numa seta (focar no bloco de destino sem sair do Graph View).
	void focusOnBlock(int index);
	// Acha um bloco existente cujo .start == addr, ou -- se não achar nenhum
	// -- cria um bloco "manual" novo ali (ver manualBlockAddrs_) e
	// reconstrói o grafo. Usado por "Add code block at address..." e pelo
	// fluxo de "Redirect this branch..." quando o alvo digitado ainda não
	// tem bloco nenhum no grafo atual.
	int findOrCreateBlockAt(u32 addr);

	// Retorna o símbolo de "addr" se o mapa de símbolos já tiver um, ou uma
	// label sintética "pos_XXXXXXXX" -- usado tanto pra rotular o bloco de
	// destino quanto pra anotar a instrução de origem do salto.
	std::string labelForAddr(u32 addr) const;

	// Tentativa heurística de resolver "jr $reg" como uma switch/jump table:
	// varre pra trás a partir de jrAddr (dentro do próprio bloco) procurando
	// o padrão lui/addiu-ou-ori formando o endereço da tabela + um "lw" que
	// carrega o registrador usado no jr a partir dela, e então lê endereços
	// consecutivos da tabela em memória até um deles não parecer um alvo
	// válido dentro da função (ou até kMaxSwitchTargets). Como é heurística
	// -- não há execução simbólica de verdade -- qualquer coisa fora do
	// padrão comum gerado pelo compilador simplesmente falha e o bloco cai
	// de volta pro tratamento antigo (nó de saída "alvo desconhecido").
	bool tryDetectSwitchTargets(u32 blockStart, u32 jrAddr, std::vector<u32>& targets) const;

	void onPaint(wxPaintEvent& evt);
	void onLeftDown(wxMouseEvent& evt);
	void onLeftUp(wxMouseEvent& evt);
	void onMotion(wxMouseEvent& evt);
	void onMouseCaptureLost(wxMouseCaptureLostEvent& evt);
	void onMouseWheel(wxMouseEvent& evt);
	void onLeftDClick(wxMouseEvent& evt);
	void onRightDown(wxMouseEvent& evt);
	void onPopupClick(wxCommandEvent& evt);

	int hitTest(const wxPoint& logicalPos) const;
	void gotoBlock(int index);

	CtrlDisassemblyView* disassemblyView;
	DebugInterface* cpu;
	DisassemblyManager* manager;
	u32 funcAddr;
	u32 funcEnd = 0;
	bool truncated = false; // função maior que kMaxFuncBytes/kMaxBlocks -- só desenhamos o começo dela

	std::vector<BasicBlock> blocks;
	std::vector<BlockEdge> edges;
	// Camada (índice de profundidade no BFS) de cada bloco, salva por
	// layoutBlocks() -- usada por computeEdgePath() pra saber quais camadas
	// (e portanto quais blocos) uma aresta atravessa, tanto pra separar
	// setas que cruzam o mesmo vão em raias quanto pra desviar por fora de
	// qualquer bloco no meio do caminho (arestas de loop e as que pulam
	// mais de uma camada).
	std::vector<int> blockLayer_;
	int selectedBlock = -1;
	int contextBlock = -1;

	wxSize logicalSize;
	double zoom_ = 1.0;
	static constexpr double kMinZoom = 0.25;
	static constexpr double kMaxZoom = 2.5;

	// Pan: arrastar com botão esquerdo no vazio (mesmo comportamento do
	// CallGraphCanvas -- ver CallTreeDialog.h para o porquê).
	bool isPanning = false;
	wxPoint panStartMouse;
	int panStartScrollX = 0, panStartScrollY = 0;

	// Arrastar um bloco específico em qualquer direção pra reposicioná-lo
	// manualmente -- mesmo esquema do CallGraphCanvas (ver CallTreeDialog.h).
	int draggingBlock = -1;
	wxPoint dragStartMouse;
	wxPoint dragStartBlockOrigin;
	bool didDragBlock = false;

	// Endereços de blocos criados manualmente via "Add code block at
	// address..." -- ver findOrCreateBlockAt(). buildBlocks() sempre
	// inclui esses como "raízes" adicionais (além de funcAddr), então
	// sobrevivem a um rebuild()/Refresh e continuam visíveis mesmo sem
	// nenhuma aresta apontando pra eles ainda (até o usuário redirecionar
	// alguma seta pra lá).
	std::vector<u32> manualBlockAddrs_;

	// Seta sob o mouse no momento (índice em 'edges'), ou -1. Destacada em
	// branco/mais grossa no onPaint() e mostra o alvo dela na barra de
	// dica -- é o "identificadas ao passar o mouse" pedido.
	int hoveredEdge = -1;
	// Clique em cima de uma seta: menu de contexto ganha "Redirect this
	// branch...". Setas sem branchAddr (fall-through puro) não aparecem
	// aqui porque não tem instrução pra reescrever.
	int contextEdge = -1;
	static const int kEdgeHitTolerance = 5; // pixels lógicos

	// Limites de segurança pra não travar a UI numa função gigante.
	static const int kMaxBlocks = 400;
	static const u32 kMaxFuncBytes = 0x8000; // 32KB (8192 instruções)

	// Limites da detecção heurística de switch/jump table.
	static const int kMaxSwitchTargets = 64;    // maior chave de switch que vamos tentar desenhar
	static const int kMaxSwitchLookback = 24;   // quantas instruções pra trás dentro do bloco procuramos o padrão lui/lw

	// Quantas instruções um bloco manual (ver manualBlockAddrs_) mostra por
	// padrão -- ele não faz análise de fluxo (não sabe onde "termina" de
	// verdade), então é só uma janela fixa de instruções a partir do
	// endereço pedido.
	static const int kManualBlockLines = 10;

	wxDECLARE_EVENT_TABLE();
};
