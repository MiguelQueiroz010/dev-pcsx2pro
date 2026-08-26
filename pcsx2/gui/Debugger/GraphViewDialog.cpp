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
#include "GraphViewDialog.h"
#include "CtrlDisassemblyView.h"

#include <wx/dcbuffer.h>
#include <wx/clipbrd.h>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <deque>
#include <set>

namespace
{
	const int kBoxHPad = 10;
	const int kBoxVPad = 6;
	const int kBoxHGap = 64;  // era 40 -- pouco espaço lateral fazia setas de blocos vizinhos ficarem coladas na borda um do outro
	const int kLayerGap = 90; // era 50 -- vão curto entre camadas não dava espaço pro "degrau" horizontal de várias setas
	const int kMargin = 30;

	// Espalha as várias setas que saem/entram do MESMO bloco ao longo da
	// base/topo dele, em vez de todas nascerem/morrerem exatamente no
	// centro (ver computeEdgePath()). kPortSpacing é a distância entre duas
	// portas vizinhas.
	const int kPortSpacing = 16;
	// Quando várias setas cruzam o MESMO vão entre duas camadas adjacentes,
	// cada uma ganha uma leve variação de altura no trecho horizontal
	// ("raia") pra não desenhar por cima da outra.
	const int kEdgeLaneStep = 12;
	// Setas que precisam CONTORNAR blocos (loop de volta, ou salto que pula
	// mais de uma camada) saem pelo lado direito, por fora de tudo que
	// atravessam. kBypassGap é a folga entre o bloco mais à direita
	// atravessado e a primeira raia de desvio; kBypassLaneStep é o
	// espaçamento entre raias de desvio paralelas.
	const int kBypassGap = 24;
	const int kBypassLaneStep = 20;

	enum GraphViewPopupIdentifiers
	{
		ID_GRAPHVIEW_GOTO = 1,
		ID_GRAPHVIEW_COPYADDRESS,
		ID_GRAPHVIEW_REFRESH,
		ID_GRAPHVIEW_REDIRECT_EDGE,
		ID_GRAPHVIEW_ADD_BLOCK,
	};
}

// ================= GraphViewDialog =================

GraphViewDialog::GraphViewDialog(CtrlDisassemblyView* parent, DebugInterface* _cpu, DisassemblyManager* manager, u32 funcAddr)
	: wxFrame(parent, wxID_ANY, L"Graph View", wxDefaultPosition, wxSize(1000, 700),
		wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT)
	, cpu(_cpu)
{
	std::string name = cpu->GetSymbolMap().GetLabelString(funcAddr);
	wxString title = !name.empty() ? wxString::Format(L"Graph View - %s", wxString(name.c_str(), wxConvUTF8))
	                                : wxString::Format(L"Graph View - sub_%08X", funcAddr);
	SetTitle(title);

	wxPanel* panel = new wxPanel(this);
	wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
	canvas = new GraphViewCanvas(panel, parent, cpu, manager, funcAddr);
	sizer->Add(canvas, 1, wxEXPAND);

	hint = new wxStaticText(panel, wxID_ANY,
		L"Double-click: go to address. Drag empty space: pan. Ctrl+Scroll: zoom. Right-click: options. "
		L"Azul = fluxo incondicional, verde \"T\" = branch tomado, vermelho \"F\" = fallthrough, laranja = switch/jump table. "
		L"Janela não-modal -- o debugger continua acessível.");
	sizer->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

	panel->SetSizer(sizer);
	wxBoxSizer* outer = new wxBoxSizer(wxVERTICAL);
	outer->Add(panel, 1, wxEXPAND);
	SetSizer(outer);

	CenterOnParent();
}

void GraphViewDialog::rebuildForFunction(u32 newFuncAddr)
{
	canvas->setFunction(newFuncAddr);
	// Volta a visão pro canto (0,0) -- reaproveitar o grafo com o
	// scroll/zoom da função anterior deixaria o novo grafo fora da área
	// visível na maioria dos casos.
	canvas->Scroll(0, 0);
	canvas->Refresh();

	std::string name = cpu->GetSymbolMap().GetLabelString(newFuncAddr);
	wxString title = !name.empty() ? wxString::Format(L"Graph View - %s", wxString(name.c_str(), wxConvUTF8))
	                                : wxString::Format(L"Graph View - sub_%08X", newFuncAddr);
	SetTitle(title);
}

// ================= GraphViewCanvas =================

wxBEGIN_EVENT_TABLE(GraphViewCanvas, wxScrolledWindow)
	EVT_PAINT(GraphViewCanvas::onPaint)
	EVT_LEFT_DOWN(GraphViewCanvas::onLeftDown)
	EVT_LEFT_UP(GraphViewCanvas::onLeftUp)
	EVT_MOTION(GraphViewCanvas::onMotion)
	EVT_MOUSE_CAPTURE_LOST(GraphViewCanvas::onMouseCaptureLost)
	EVT_MOUSEWHEEL(GraphViewCanvas::onMouseWheel)
	EVT_LEFT_DCLICK(GraphViewCanvas::onLeftDClick)
	EVT_RIGHT_DOWN(GraphViewCanvas::onRightDown)
wxEND_EVENT_TABLE()

GraphViewCanvas::GraphViewCanvas(wxWindow* parent, CtrlDisassemblyView* _disassemblyView, DebugInterface* _cpu, DisassemblyManager* _manager, u32 _funcAddr)
	: wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SUNKEN | wxHSCROLL | wxVSCROLL)
	, disassemblyView(_disassemblyView)
	, cpu(_cpu)
	, manager(_manager)
	, funcAddr(_funcAddr)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetBackgroundColour(*wxWHITE);
	// 1px por unidade de scroll -- não 20 -- pra dar pan suave/preciso igual
	// ao IDA. Ver comentário em onMotion() sobre o Refresh() extra que essa
	// mesma parte do código precisa (mesmo padrão usado no CallGraphCanvas).
	SetScrollRate(1, 1);

	// Fonte monoespaçada pro texto de cada bloco ficar alinhado igual ao
	// disassembler linear.
	wxFont font(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
	if (font.IsOk())
		SetFont(font);

	rebuild();
}

void GraphViewCanvas::rebuild()
{
	buildBlocks();
	layoutBlocks();
	updateVirtualSizeForZoom();
	Refresh();
}

std::string GraphViewCanvas::labelForAddr(u32 addr) const
{
	std::string sym = cpu->GetSymbolMap().GetLabelString(addr);
	if (!sym.empty())
		return sym;

	char buf[16];
	snprintf(buf, sizeof(buf), "pos_%08X", addr);
	return buf;
}

// Heurística de reconstrução de switch/jump table. NÃO é execução simbólica
// de verdade -- só reconhece os dois idiomas mais comuns que o GCC (o
// compilador usado nos jogos de PS2) gera pra "jr $reg" alimentado por uma
// tabela de ponteiros em .rodata:
//
//   (A) lui  $at, %hi(tabela)          (B) lui   $at, %hi(tabela)
//       addu $at, $at, $idx4                addiu $at, $at, %lo(tabela)
//       lw   $v0, %lo(tabela)($at)          addu  $t0, $at, $idx4
//       jr   $v0                            lw    $v0, 0($t0)
//                                            jr    $v0
//
// onde $idx4 é o índice do case já multiplicado por 4 (normalmente via
// "sll"), o que essa função nunca tenta resolver -- só precisa achar o
// registrador que carrega o ENDEREÇO CONSTANTE da tabela, ignorando o lado
// dinâmico da soma. Se o código não bater com nenhum desses dois padrões
// (ex.: mais um nível de indireção, otimizações incomuns, tabela calculada
// em runtime de outra forma), a função simplesmente devolve false e quem
// chamou cai de volta no tratamento antigo (nó de saída "alvo desconhecido").
bool GraphViewCanvas::tryDetectSwitchTargets(u32 blockStart, u32 jrAddr, std::vector<u32>& targets) const
{
	targets.clear();

	if (!cpu->isValidAddress(jrAddr))
		return false;

	u32 jrRaw = cpu->read32(jrAddr);
	bool isRealJr = ((jrRaw >> 26) == 0x00) && ((jrRaw & 0x3F) == 0x08); // SPECIAL, funct JR
	if (!isRealJr)
		return false;

	int targetReg = (int)((jrRaw >> 21) & 0x1F);
	if (targetReg == 0 || targetReg == 31) // $zero ou $ra -- isso é um "return", não uma switch
		return false;

	u32 floor = blockStart;
	u32 lookbackFloor = (jrAddr > (u32)(4 * kMaxSwitchLookback)) ? jrAddr - (u32)(4 * kMaxSwitchLookback) : 0;
	if (lookbackFloor > floor)
		floor = lookbackFloor;

	struct RegState
	{
		bool wanted = false;
		bool resolved = false;
		u32 constValue = 0;
		bool hasLui = false;
		u32 luiHi = 0;
		bool hasPendingLo = false;
		u32 pendingLo = 0;
		bool pendingIsOri = false;
		bool isSum = false;
		bool sumCaptured = false;
		int sumA = -1, sumB = -1;
	};
	std::map<int, RegState> regs;
	regs[targetReg].wanted = true;

	bool haveLw = false;
	int lwBaseReg = -1;
	int32_t lwOffset = 0;

	u32 a = jrAddr;
	while (a > floor)
	{
		a -= 4;
		if (!cpu->isValidAddress(a))
			continue;

		u32 raw = cpu->read32(a);
		u32 op = raw >> 26;
		u32 funct = raw & 0x3F;
		int rs = (int)((raw >> 21) & 0x1F);
		int rt = (int)((raw >> 16) & 0x1F);
		int rd = (int)((raw >> 11) & 0x1F);
		int32_t immS = (int32_t)(int16_t)(raw & 0xFFFF);
		u32 immU = raw & 0xFFFF;

		if (!haveLw && op == 0x23 /* LW */ && rt == targetReg)
		{
			haveLw = true;
			lwBaseReg = rs;
			lwOffset = immS;
			if (rs != 0 && !regs.count(rs))
				regs[rs].wanted = true;
			continue;
		}

		if (op == 0x00 && (funct == 0x21 /* ADDU */ || funct == 0x20 /* ADD */))
		{
			auto rIt = regs.find(rd);
			if (rIt != regs.end() && rIt->second.wanted && !rIt->second.resolved &&
				!rIt->second.isSum && !rIt->second.hasLui)
			{
				rIt->second.isSum = true;
				rIt->second.sumCaptured = true;
				rIt->second.sumA = rs;
				rIt->second.sumB = rt;
				if (rs != 0 && !regs.count(rs))
					regs[rs].wanted = true;
				if (rt != 0 && !regs.count(rt))
					regs[rt].wanted = true;
			}
			continue;
		}

		if (op == 0x09 /* ADDIU */ && rs == rt)
		{
			auto rIt = regs.find(rt);
			if (rIt != regs.end() && rIt->second.wanted && !rIt->second.resolved &&
				!rIt->second.hasPendingLo && !rIt->second.hasLui)
			{
				rIt->second.hasPendingLo = true;
				rIt->second.pendingLo = (u32)immS;
				rIt->second.pendingIsOri = false;
			}
			continue;
		}

		if (op == 0x0D /* ORI */ && rs == rt)
		{
			auto rIt = regs.find(rt);
			if (rIt != regs.end() && rIt->second.wanted && !rIt->second.resolved &&
				!rIt->second.hasPendingLo && !rIt->second.hasLui)
			{
				rIt->second.hasPendingLo = true;
				rIt->second.pendingLo = immU;
				rIt->second.pendingIsOri = true;
			}
			continue;
		}

		if (op == 0x0F /* LUI */)
		{
			auto rIt = regs.find(rt);
			if (rIt != regs.end() && rIt->second.wanted && !rIt->second.resolved && !rIt->second.hasLui)
			{
				rIt->second.hasLui = true;
				rIt->second.luiHi = immU;
				u32 lo = rIt->second.hasPendingLo
					? (rIt->second.pendingIsOri ? rIt->second.pendingLo : (u32)(int32_t)(int16_t)rIt->second.pendingLo)
					: 0;
				rIt->second.constValue = (immU << 16) + lo;
				rIt->second.resolved = true;
			}
			continue;
		}
	}

	// Propaga os registradores resolvidos por "lui/addiu/ori" através de
	// somas ("addu") que ainda não fecharam durante a varredura -- basta
	// um punhado de rodadas porque a cadeia raramente passa de 1-2 níveis.
	for (int pass = 0; pass < (int)regs.size() + 2; pass++)
	{
		bool changed = false;
		for (auto& kv : regs)
		{
			RegState& r = kv.second;
			if (!r.isSum || r.resolved)
				continue;

			auto resolveOperand = [&](int reg, bool& ok, u32& value) {
				if (reg == 0) { ok = true; value = 0; return; }
				auto it = regs.find(reg);
				ok = (it != regs.end() && it->second.resolved);
				value = ok ? it->second.constValue : 0;
			};

			bool okA, okB;
			u32 valA, valB;
			resolveOperand(r.sumA, okA, valA);
			resolveOperand(r.sumB, okB, valB);

			if (okA && okB)
			{
				r.constValue = valA + valB;
				r.resolved = true;
				changed = true;
			}
			else if (okA || okB)
			{
				// Só um lado resolveu pra uma constante -- o outro é o
				// índice dinâmico do case, que ignoramos de propósito.
				r.constValue = okA ? valA : valB;
				r.resolved = true;
				changed = true;
			}
		}
		if (!changed)
			break;
	}

	if (!haveLw || lwBaseReg < 0)
		return false;

	auto baseIt = regs.find(lwBaseReg);
	if (baseIt == regs.end() || !baseIt->second.resolved)
		return false;

	u32 tableStart = baseIt->second.constValue + (u32)lwOffset;

	for (int i = 0; i < kMaxSwitchTargets; i++)
	{
		u32 wordAddr = tableStart + (u32)(i * 4);
		if (!cpu->isValidAddress(wordAddr))
			break;
		u32 val = cpu->read32(wordAddr);
		// Cada "case" da tabela deve apontar pra dentro da própria função e
		// ser um endereço de instrução válido (múltiplo de 4). O primeiro
		// valor que não bater com isso é tratado como o fim da tabela --
		// funciona bem porque dados/tabelas vizinhas na .rodata raramente
		// por coincidência caem dentro da faixa de endereços da função.
		if (val == 0 || (val & 0x3) != 0 || val < funcAddr || val >= funcEnd)
			break;
		targets.push_back(val);
	}

	return !targets.empty();
}

void GraphViewCanvas::buildBlocks()
{
	blocks.clear();
	edges.clear();
	selectedBlock = -1;
	contextBlock = -1;
	truncated = false;

	u32 size = cpu->GetSymbolMap().GetFunctionSize(funcAddr);
	if (size == 0 || size == (u32)-1)
		size = 4; // função sem tamanho conhecido no mapa de símbolos -- desenha só a primeira instrução
	if (size > kMaxFuncBytes)
	{
		size = kMaxFuncBytes;
		truncated = true;
	}
	funcEnd = funcAddr + size;

	// Passo 1: varre a função inteira instrução por instrução (MIPS = 4
	// bytes fixos) guardando, pra cada endereço, se é um branch/call/etc, e
	// junto disso já monta o conjunto de "líderes": endereços onde um novo
	// bloco básico começa. Líderes são: a própria entrada da função, todo
	// alvo de branch dentro do intervalo, e o endereço logo depois do delay
	// slot de toda instrução que efetivamente encerra um bloco (o MIPS
	// executa a instrução seguinte a um branch -- o "delay slot" -- antes do
	// desvio acontecer de verdade).
	struct InstrInfo
	{
		bool isBranch = false;
		bool isConditional = false;
		bool isBranchToRegister = false;
		u32 branchTarget = 0;
		bool isCall = false; // JAL/JALR -- não interrompe o bloco básico
	};
	std::map<u32, InstrInfo> instrs;
	std::set<u32> leaders;
	leaders.insert(funcAddr);

	// Todo endereço que é alvo de um branch/jump direto OU de uma switch
	// table resolvida (ver tryDetectSwitchTargets) entra aqui -- é o
	// conjunto que decide quem ganha uma label sintética "pos_XXXXXXXX" no
	// bloco de destino (a entrada da função em si não conta: ela já tem o
	// próprio nome/símbolo mostrado no título da janela).
	std::set<u32> branchTargets;

	DisassemblyLineInfo line = DisassemblyLineInfo();
	for (u32 addr = funcAddr; addr < funcEnd; addr += 4)
	{
		if (!cpu->isValidAddress(addr))
			continue;

		manager->getLine(addr, true, line);

		InstrInfo ii;
		ii.isBranch = line.info.isBranch;
		ii.isConditional = line.info.isConditional;
		ii.isBranchToRegister = line.info.isBranchToRegister;
		ii.branchTarget = line.info.branchTarget;

		// JAL (opcode 000011) e JALR (SPECIAL, funct 001001) são chamadas:
		// a execução volta pra cá logo depois, então elas NÃO encerram o
		// bloco básico mesmo que o info.isBranch delas venha true (mesmo
		// teste de bits que PatchCallTarget usa pra reconhecer um JAL).
		u32 raw = cpu->read32(addr);
		bool isJal = (raw >> 26) == 0x03;
		bool isJalr = ((raw >> 26) == 0x00) && ((raw & 0x3F) == 0x09);
		ii.isCall = isJal || isJalr;

		instrs[addr] = ii;

		if (ii.isBranch && !ii.isCall)
		{
			u32 afterDelaySlot = addr + 8;
			if (!ii.isBranchToRegister && ii.branchTarget >= funcAddr && ii.branchTarget < funcEnd)
			{
				leaders.insert(ii.branchTarget);
				branchTargets.insert(ii.branchTarget);
			}
			else if (ii.isBranchToRegister)
			{
				// "jr $reg" -- pode ser um simples "jr $ra" (retorno) ou um
				// dispatch de switch/jump table. Tenta resolver os alvos
				// aqui mesmo, ainda na varredura linear, porque eles
				// precisam virar líderes ANTES do passo 2 (senão o bloco
				// nunca é cortado exatamente onde cada "case" começa).
				std::vector<u32> switchTargets;
				if (tryDetectSwitchTargets(funcAddr, addr, switchTargets))
				{
					for (u32 t : switchTargets)
					{
						leaders.insert(t);
						branchTargets.insert(t);
					}
				}
			}
			if (afterDelaySlot < funcEnd)
				leaders.insert(afterDelaySlot);
		}
	}

	// Passo 2: cada bloco vai de um líder até o próximo líder (ou até
	// funcEnd no último). Isso é seguro porque todo ponto onde um bloco
	// realmente termina (por causa de um branch) já virou um líder no passo
	// 1 -- então nunca sobra um branch "no meio" de um bloco.
	std::vector<u32> leaderList(leaders.begin(), leaders.end());
	std::map<u32, int> leaderToBlock;
	for (size_t i = 0; i < leaderList.size(); i++)
	{
		if ((int)blocks.size() >= kMaxBlocks)
		{
			truncated = true;
			break;
		}

		u32 start = leaderList[i];
		u32 end = (i + 1 < leaderList.size()) ? leaderList[i + 1] : funcEnd;
		if (end <= start)
			continue;

		BasicBlock b;
		b.start = start;
		b.end = end;

		// Só a entrada da função (start == funcAddr) fica sem label própria
		// aqui -- ela já aparece no título da janela. Qualquer outro bloco
		// que seja alvo de branch/jump/switch ganha "pos_XXXXXXXX" (ou o
		// símbolo já cadastrado no mapa, se houver um exatamente nesse
		// endereço), pra ficar claro de onde a seta que chega nele "vem".
		if (start != funcAddr && branchTargets.count(start))
			b.label = labelForAddr(start);

		for (u32 a = start; a < end; a += 4)
		{
			if (!cpu->isValidAddress(a))
				continue;
			manager->getLine(a, true, line);
			std::string safeName = line.name.substr(0, 24);
			std::string safeParams = line.params.substr(0, 160);
			char buf[256];
			snprintf(buf, sizeof(buf), "%08X  %-8s %s", a, safeName.c_str(), safeParams.c_str());

			// Se esta instrução é um branch/jump direto com alvo dentro da
			// função, anota na própria linha pra qual label ela pula --
			// assim não é preciso decorar o endereço hex pra saber ("pos_"
			// se não houver símbolo). Saltos por registrador (jr $ra, switch
			// já resolvida) não têm um alvo único fixo aqui, então ficam sem
			// anotação -- o(s) sucessor(es) aparecem como setas no grafo.
			auto instrIt = instrs.find(a);
			if (instrIt != instrs.end() && instrIt->second.isBranch && !instrIt->second.isCall &&
				!instrIt->second.isBranchToRegister &&
				instrIt->second.branchTarget >= funcAddr && instrIt->second.branchTarget < funcEnd)
			{
				std::string targetLabel = labelForAddr(instrIt->second.branchTarget);
				if (!targetLabel.empty())
				{
					std::string annotated(buf);
					annotated += "  -> " + targetLabel;
					b.lines.push_back(annotated.substr(0, 200));
					continue;
				}
			}

			b.lines.push_back(buf);
		}

		leaderToBlock[start] = (int)blocks.size();
		blocks.push_back(b);
	}

	// Passo 3: arestas -- olha a última instrução de cada bloco pra decidir
	// os sucessores (o alvo do branch e/ou o fallthrough).
	for (int bi = 0; bi < (int)blocks.size(); bi++)
	{
		BasicBlock& b = blocks[bi];
		if (b.end <= b.start)
			continue;

		// O endereço do TERMINADOR do bloco (branch/jr/switch) não é
		// necessariamente b.end - 4: todo desvio em MIPS deixa um delay
		// slot -- a instrução seguinte, sempre executada antes do desvio
		// acontecer de verdade -- e é por isso que os líderes do passo 1
		// usam "addr + 8" (branch + delay slot) como fim de bloco. Ou seja,
		// quando um bloco termina por causa do SEU PRÓPRIO branch, a última
		// instrução dele (b.end - 4) é o delay slot, e o branch de verdade
		// está uma instrução antes (b.end - 8). Usar b.end - 4 aqui fazia
		// procurar o branch no endereço errado (o nop do delay slot nunca é
		// branch), então caía sempre no fallback de "bloco reto" -- por
		// isso a seta "T" de um branch condicional sumia (só sobrava o "F"
		// bogus) e nenhuma switch/jump table era detectada (o jr nunca era
		// encontrado). Quando o bloco termina por outro motivo (cortado no
		// meio por um alvo externo, sem branch próprio), b.end - 8 cai numa
		// instrução comum que não é branch, e o código cai no mesmo
		// fallback de "bloco reto" corretamente -- então a correção não
		// quebra esse outro caso.
		u32 lastAddr = b.end - 4;
		u32 terminatorAddr = (b.end >= b.start + 8) ? (b.end - 8) : lastAddr;
		auto it = instrs.find(terminatorAddr);
		bool handledAsBranch = false;

		if (it != instrs.end() && it->second.isBranch && !it->second.isCall)
		{
			const InstrInfo& ii = it->second;
			handledAsBranch = true;

			if (ii.isBranchToRegister)
			{
				// jr $ra (return) ou salto por registrador. Antes de desistir
				// e tratar como saída desconhecida, tenta de novo a detecção
				// de switch/jump table -- agora já sabendo o início real do
				// bloco (b.start), o que dá um limite de lookback mais justo
				// do que o usado durante a varredura linear do passo 1.
				std::vector<u32> switchTargets;
				int resolvedEdges = 0;
				if (tryDetectSwitchTargets(b.start, terminatorAddr, switchTargets))
				{
					b.isSwitchDispatch = true;
					for (u32 t : switchTargets)
					{
						auto tIt = leaderToBlock.find(t);
						if (tIt != leaderToBlock.end())
						{
							edges.push_back(BlockEdge{bi, tIt->second, EdgeKind::Switch});
							resolvedEdges++;
						}
					}
				}

				// Mesmo com switch resolvida, se nenhum alvo bateu com um
				// bloco de verdade (mapa ruim, tabela fora do padrão
				// esperado etc.) o bloco continua marcado como saída --
				// melhor mostrar "sem sucessor conhecido" do que fingir um
				// grafo incompleto.
				if (resolvedEdges == 0)
					b.isExit = true;
			}
			else if (ii.branchTarget < funcAddr || ii.branchTarget >= funcEnd)
			{
				// Salto pra fora da função (tail call etc.) -- o alvo real
				// não está neste grafo, também tratamos como saída.
				b.isExit = true;
			}
			else
			{
				auto tIt = leaderToBlock.find(ii.branchTarget);
				if (tIt != leaderToBlock.end())
				{
					// branchAddr = terminatorAddr: é a instrução de
					// branch/jump de verdade (não o delay slot) -- usada por
					// "Redirect this branch..." pra saber o que reescrever.
					BlockEdge e{bi, tIt->second, ii.isConditional ? EdgeKind::Taken : EdgeKind::Unconditional, terminatorAddr};
					edges.push_back(e);
				}

				if (ii.isConditional)
				{
					auto fIt = leaderToBlock.find(b.end);
					if (fIt != leaderToBlock.end())
					{
						BlockEdge e{bi, fIt->second, EdgeKind::Fallthrough};
						edges.push_back(e);
					}
				}
			}
		}

		if (!handledAsBranch)
		{
			// Bloco reto: terminou porque outro branch aponta pra cá, não
			// porque a última instrução dele é um desvio -- um sucessor só,
			// em sequência.
			auto fIt = leaderToBlock.find(b.end);
			if (fIt != leaderToBlock.end())
			{
				BlockEdge e{bi, fIt->second, EdgeKind::Fallthrough};
				edges.push_back(e);
			}
		}
	}

	// Passo 4: blocos manuais (ver manualBlockAddrs_) -- adicionados via
	// "Add code block at address..." ou pelo fluxo de "Redirect this
	// branch..." quando o alvo digitado ainda não tem bloco no grafo atual.
	// Não passam pela análise de fluxo (não sabem onde "terminam" de
	// verdade, podem nem pertencer a esta função) -- só uma janela fixa de
	// kManualBlockLines instruções a partir do endereço pedido, sem nenhuma
	// aresta até o usuário redirecionar algum branch existente pra apontar
	// pra cá. Pula endereços que já viraram bloco pela análise normal (não
	// duplica).
	for (u32 manualAddr : manualBlockAddrs_)
	{
		if (leaderToBlock.find(manualAddr) != leaderToBlock.end())
			continue;
		if (!cpu->isValidAddress(manualAddr))
			continue;

		BasicBlock b;
		b.start = manualAddr;
		b.manual = true;
		b.label = labelForAddr(manualAddr);

		u32 a = manualAddr;
		for (int i = 0; i < kManualBlockLines && cpu->isValidAddress(a); i++, a += 4)
		{
			DisassemblyLineInfo manualLine = DisassemblyLineInfo();
			manager->getLine(a, true, manualLine);
			char buf[256];
			snprintf(buf, sizeof(buf), "%08X  %-8s %s", a, manualLine.name.substr(0, 24).c_str(), manualLine.params.substr(0, 160).c_str());
			b.lines.push_back(buf);
		}
		b.end = a;

		leaderToBlock[manualAddr] = (int)blocks.size();
		blocks.push_back(b);
	}

	if (blocks.empty())
	{
		BasicBlock b;
		b.start = funcAddr;
		b.end = funcAddr;
		b.lines.push_back("(sem instruções válidas nesta função)");
		b.isExit = true;
		blocks.push_back(b);
	}
}

void GraphViewCanvas::layoutBlocks()
{
	if (blocks.empty())
	{
		logicalSize = wxSize(kMargin * 2, kMargin * 2);
		return;
	}

	wxClientDC dc(this);
	dc.SetFont(GetFont());

	// Camadas por BFS a partir do bloco de entrada (índice 0), só avançando
	// "pra frente": uma aresta de volta (fecho de laço/loop) só reposiciona
	// o alvo se isso empurrar a camada dele mais pra baixo -- do contrário a
	// BFS giraria pra sempre num laço. visitCount limita quantas vezes cada
	// bloco pode ser re-enfileirado, então mesmo grafos com muitos laços
	// aninhados terminam em tempo finito.
	std::vector<int> layerOf(blocks.size(), 0);
	std::vector<int> visitCount(blocks.size(), 0);
	std::vector<bool> reached(blocks.size(), false);
	reached[0] = true;
	std::deque<int> queue;
	queue.push_back(0);
	const int kMaxRelax = (int)blocks.size() * 6 + 32;
	int relaxCount = 0;
	while (!queue.empty() && relaxCount < kMaxRelax)
	{
		int cur = queue.front();
		queue.pop_front();
		relaxCount++;
		for (const BlockEdge& e : edges)
		{
			if (e.fromBlock != cur)
				continue;
			reached[e.toBlock] = true;
			if (layerOf[e.toBlock] < layerOf[cur] + 1)
			{
				layerOf[e.toBlock] = layerOf[cur] + 1;
				if (visitCount[e.toBlock]++ < 4)
					queue.push_back(e.toBlock);
			}
		}
	}

	int maxLayer = 0;
	for (int i = 0; i < (int)blocks.size(); i++)
		maxLayer = std::max(maxLayer, layerOf[i]);

	// Blocos nunca alcançados a partir da entrada (índice 0) por nenhuma
	// aresta -- na prática, só blocos manuais recém-criados (ver
	// manualBlockAddrs_) que o usuário ainda não redirecionou nenhuma seta
	// pra apontar pra eles. Em vez de deixar todos empilhados na camada 0
	// por cima do bloco de entrada, formam uma camada extra "à parte" logo
	// abaixo do resto do grafo -- ficam visíveis e prontos pra receber um
	// redirect sem bagunçar o layout do fluxo real.
	int orphanLayer = maxLayer + 1;
	bool hasOrphans = false;
	for (int i = 1; i < (int)blocks.size(); i++)
	{
		if (!reached[i])
		{
			layerOf[i] = orphanLayer;
			hasOrphans = true;
		}
	}
	if (hasOrphans)
		maxLayer = orphanLayer;

	std::map<int, std::vector<int>> byLayer;
	for (int i = 0; i < (int)blocks.size(); i++)
		byLayer[layerOf[i]].push_back(i);

	// Reordena a ordem DENTRO de cada camada pra minimizar cruzamento de
	// setas (ver comentário no header) -- só mexe na ordem, ainda não em
	// pixels. Pula a camada dos órfãos: eles não têm aresta com ninguém
	// ainda, barycenter não teria o que otimizar ali.
	reorderLayersToReduceCrossings(byLayer, layerOf);

	// Tamanho de cada caixa a partir do texto (uma linha por instrução, mais
	// uma linha extra no topo pra label "pos_XXXXXXXX"/símbolo quando houver).
	int lineH = dc.GetCharHeight() + 2;
	for (BasicBlock& b : blocks)
	{
		int w = 0;
		if (!b.label.empty())
			w = std::max(w, dc.GetTextExtent(wxString(b.label.c_str(), wxConvUTF8) + L":").GetWidth());
		for (const std::string& l : b.lines)
			w = std::max(w, dc.GetTextExtent(wxString(l.c_str(), wxConvUTF8)).GetWidth());
		w += kBoxHPad * 2;
		int labelLines = b.label.empty() ? 0 : 1;
		int h = (std::max(1, (int)b.lines.size()) + labelLines) * lineH + kBoxVPad * 2;
		b.rect = wxRect(0, 0, w, h);
	}

	// Larguras/alturas por camada: caixas lado a lado (já na ordem decidida
	// por reorderLayersToReduceCrossings), altura = maior caixa da camada.
	int maxLayerWidth = 0;
	std::map<int, int> layerWidth, layerHeight;
	for (auto& lp : byLayer)
	{
		int x = 0, rowH = 0;
		for (int idx : lp.second)
		{
			wxRect& r = blocks[idx].rect;
			r.SetX(x);
			x += r.GetWidth() + kBoxHGap;
			rowH = std::max(rowH, r.GetHeight());
		}
		int width = x > 0 ? x - kBoxHGap : 0;
		layerWidth[lp.first] = width;
		layerHeight[lp.first] = rowH;
		maxLayerWidth = std::max(maxLayerWidth, width);
	}

	int y = kMargin;
	for (int layer = 0; layer <= maxLayer; layer++)
	{
		auto it = byLayer.find(layer);
		if (it == byLayer.end())
			continue;
		// Camada dos órfãos ganha um respiro extra acima pra ficar
		// visualmente separada do fluxo normal, como uma "área de rascunho".
		if (hasOrphans && layer == orphanLayer)
			y += kLayerGap;
		int offsetX = kMargin + (maxLayerWidth - layerWidth[layer]) / 2;
		for (int idx : it->second)
		{
			blocks[idx].rect.SetX(blocks[idx].rect.GetX() + offsetX);
			blocks[idx].rect.SetY(y);
		}
		y += layerHeight[layer] + kLayerGap;
	}

	logicalSize = wxSize(maxLayerWidth + kMargin * 2, y + kMargin);

	// Salva a camada de cada bloco pra computeEdgePath() poder identificar
	// quais camadas (e blocos) cada aresta atravessa (ver blockLayer_ no
	// header).
	blockLayer_ = layerOf;
}

void GraphViewCanvas::reorderLayersToReduceCrossings(std::map<int, std::vector<int>>& byLayer, const std::vector<int>& layerOf)
{
	if (byLayer.size() < 2)
		return;

	// Índice dentro da camada de cada bloco -- é o que barycenter usa como
	// "posição X" de um vizinho (ainda não temos pixels nessa altura).
	std::map<int, int> positionInLayer;
	auto rebuildPositions = [&]() {
		for (auto& lp : byLayer)
			for (int i = 0; i < (int)lp.second.size(); i++)
				positionInLayer[lp.second[i]] = i;
	};
	rebuildPositions();

	// Lista de vizinhos (em qualquer direção -- pega tanto arestas que
	// entram quanto que saem do bloco) usada pro cálculo do barycenter.
	auto neighborsOf = [&](int block) {
		std::vector<int> result;
		for (const BlockEdge& e : edges)
		{
			if (e.fromBlock == block)
				result.push_back(e.toBlock);
			else if (e.toBlock == block)
				result.push_back(e.fromBlock);
		}
		return result;
	};

	// Poucas passadas alternando pra baixo/pra cima já converge bem o
	// bastante pra grafos do tamanho de uma função (dezenas de blocos, não
	// milhares) -- não precisa rodar até estabilizar de verdade.
	const int kPasses = 4;
	for (int pass = 0; pass < kPasses; pass++)
	{
		bool downward = (pass % 2) == 0;
		std::vector<int> layerKeys;
		for (auto& lp : byLayer)
			layerKeys.push_back(lp.first);
		if (!downward)
			std::reverse(layerKeys.begin(), layerKeys.end());

		for (int layer : layerKeys)
		{
			std::vector<int>& order = byLayer[layer];
			if (order.size() < 2)
				continue;

			// Barycenter = média da posição (na camada vizinha já fixada
			// nesta passada) dos vizinhos de cada bloco. Bloco sem nenhum
			// vizinho ainda posicionado mantém a posição atual (não pula
			// pro início/fim à toa).
			std::vector<std::pair<double, int>> keyed;
			for (int idx : order)
			{
				std::vector<int> neighbors = neighborsOf(idx);
				double sum = 0;
				int count = 0;
				for (int n : neighbors)
				{
					// Só conta vizinho de camada ADJACENTE na direção que
					// esta passada está considerando "já fixada" -- evita
					// que um vizinho de camada distante (aresta de loop
					// longa) puxe a ordenação de forma instável.
					int expectedLayer = downward ? layer - 1 : layer + 1;
					if (layerOf[n] != expectedLayer)
						continue;
					sum += positionInLayer[n];
					count++;
				}
				double bary = count > 0 ? (sum / count) : (double)positionInLayer[idx];
				keyed.push_back({bary, idx});
			}

			std::stable_sort(keyed.begin(), keyed.end(),
				[](const std::pair<double, int>& a, const std::pair<double, int>& b) { return a.first < b.first; });

			for (int i = 0; i < (int)keyed.size(); i++)
				order[i] = keyed[i].second;

			rebuildPositions();
		}
	}
}

void GraphViewCanvas::updateVirtualSizeForZoom()
{
	SetVirtualSize((int)(logicalSize.GetWidth() * zoom_), (int)(logicalSize.GetHeight() * zoom_));
}

wxColour GraphViewCanvas::colourForBlock(int index) const
{
	const BasicBlock& b = blocks[index];
	if (index == selectedBlock)
		return wxColour(60, 60, 90);
	if (!b.lines.empty() && b.start <= cpu->getPC() && cpu->getPC() < b.end)
		return wxColour(150, 90, 20); // laranja escuro: bloco onde o PC atual está
	if (b.start == funcAddr)
		return wxColour(30, 90, 200); // entrada da função, em azul (mesmo tom do CallGraphCanvas)
	if (b.isExit)
		return wxColour(110, 30, 30); // saída (return/salto sem alvo conhecido), em vermelho escuro
	if (b.isSwitchDispatch)
		return wxColour(120, 75, 15); // termina num "jr" resolvido como switch/jump table, em marrom/laranja escuro
	if (b.manual)
		return wxColour(90, 40, 120); // roxo -- bloco criado manualmente (ver manualBlockAddrs_), ainda sem aresta apontando pra ele
	return wxColour(45, 45, 45);
}

std::vector<wxPoint> GraphViewCanvas::computeEdgePath(const BlockEdge& e) const
{
	const wxRect& from = blocks[e.fromBlock].rect;
	const wxRect& to = blocks[e.toBlock].rect;

	// Índice de 'e' dentro de 'edges' via aritmética de ponteiro -- válido
	// porque todo chamador de computeEdgePath() passa uma referência pra um
	// elemento de dentro do próprio vetor membro 'edges' (ver onPaint() e
	// hitTestEdge()).
	const int myIdx = edges.empty() ? 0 : (int)(&e - &edges[0]);

	const bool haveLayers = blockLayer_.size() == blocks.size() && !blocks.empty();
	const int fromLayer = haveLayers ? blockLayer_[e.fromBlock] : 0;
	const int toLayer = haveLayers ? blockLayer_[e.toBlock] : 0;
	const bool isBack = to.GetY() < from.GetY();
	// "Salta camada": segue pra frente mas pula uma ou mais camadas
	// intermediárias (ex.: um "b" que passa direto por cima de um bloco
	// vizinho). A quebra única do caminho normal (reto-lateral-reto) não
	// basta aqui -- a descida em X fixo cortaria bem no meio de qualquer
	// bloco que estivesse nessas camadas do meio.
	const bool isMultiHop = haveLayers && !isBack && (toLayer - fromLayer) > 1;

	std::vector<wxPoint> path;

	if (isBack || isMultiHop)
	{
		// Contorna por fora (lado direito) de TODAS as camadas que a seta
		// atravessa, em vez de cortar reto por cima dos blocos do meio -- é
		// o roteamento "desvia de obstáculo" do IDA. bypassX fica à direita
		// do bloco mais largo entre todas as camadas cobertas (origem,
		// destino e qualquer coisa entre elas), então a corrida vertical
		// nunca entra na coluna de nenhum bloco.
		int loLayer = isBack ? toLayer : fromLayer;
		int hiLayer = isBack ? fromLayer : toLayer;

		int rightmost = std::max(from.GetX() + from.GetWidth(), to.GetX() + to.GetWidth());
		if (haveLayers)
		{
			for (int i = 0; i < (int)blocks.size(); i++)
			{
				if (blockLayer_[i] >= loLayer && blockLayer_[i] <= hiLayer)
					rightmost = std::max(rightmost, blocks[i].rect.GetX() + blocks[i].rect.GetWidth());
			}
		}

		// Cada seta que precisa contornar (loop ou salto de camada) ganha
		// sua própria raia de desvio, em ordem estável -- sem isso, várias
		// delas convergiam todas pro mesmo bypassX e ficavam desenhadas
		// grudadas uma na outra.
		auto needsBypass = [&](const BlockEdge& o) {
			bool oBack = blocks[o.toBlock].rect.GetY() < blocks[o.fromBlock].rect.GetY();
			bool oMultiHop = haveLayers && !oBack && (blockLayer_[o.toBlock] - blockLayer_[o.fromBlock]) > 1;
			return oBack || oMultiHop;
		};
		int lane = 0;
		for (int i = 0; i < myIdx; i++)
			if (needsBypass(edges[i]))
				lane++;

		int bypassX = rightmost + kBypassGap + lane * kBypassLaneStep;

		wxPoint start, end;
		if (isBack)
		{
			// Loop: sai/entra pela lateral direita dos blocos (visual de
			// "volta"), não por cima/baixo.
			start = wxPoint(from.GetX() + from.GetWidth(), from.GetY() + from.GetHeight() / 2);
			end = wxPoint(to.GetX() + to.GetWidth(), to.GetY() + to.GetHeight() / 2);
		}
		else
		{
			// Salto pra frente que pula camada: continua saindo por baixo e
			// entrando por cima, só que contornando por fora em vez de
			// descer reto.
			start = wxPoint(from.GetX() + from.GetWidth() / 2, from.GetY() + from.GetHeight());
			end = wxPoint(to.GetX() + to.GetWidth() / 2, to.GetY());
		}

		path.push_back(start);
		path.push_back(wxPoint(bypassX, start.y));
		path.push_back(wxPoint(bypassX, end.y));
		path.push_back(end);
	}
	else
	{
		// Caminho normal: uma seta pra frente entre camadas ADJACENTES.
		// Como o vão entre duas camadas nunca tem bloco nenhum dentro (só
		// os próprios blocos ficam nas "linhas"), o trecho horizontal desse
		// caminho já nasce livre de colisão -- não precisa de desvio, só
		// de portas/raias espalhadas pra não ficar tudo colado (ver abaixo).
		auto portX = [&](const wxRect& box, bool wantOut) {
			std::vector<int> siblings;
			for (int i = 0; i < (int)edges.size(); i++)
			{
				if (wantOut ? (edges[i].fromBlock == e.fromBlock) : (edges[i].toBlock == e.toBlock))
					siblings.push_back(i);
			}
			std::stable_sort(siblings.begin(), siblings.end(), [&](int a, int b) {
				int xa = wantOut ? blocks[edges[a].toBlock].rect.GetX() : blocks[edges[a].fromBlock].rect.GetX();
				int xb = wantOut ? blocks[edges[b].toBlock].rect.GetX() : blocks[edges[b].fromBlock].rect.GetX();
				return xa < xb;
			});

			int n = (int)siblings.size();
			if (n <= 1)
				return box.GetX() + box.GetWidth() / 2;

			int rank = 0;
			for (int i = 0; i < n; i++)
				if (siblings[i] == myIdx) { rank = i; break; }

			int usable = std::max(box.GetWidth() - kBoxHPad * 2, kPortSpacing);
			int span = std::min(usable, kPortSpacing * (n - 1));
			int startX = box.GetX() + box.GetWidth() / 2 - span / 2;
			return startX + (span * rank) / (n - 1);
		};

		// Fluxo normal, pra frente: sai de baixo do bloco de origem, desce
		// reto até a metade do vão entre as duas camadas, dá UMA quebra
		// horizontal ali, desce reto de novo até o topo do destino. É o que
		// dá aquele visual "em degrau" do Graph View do IDA em vez de uma
		// diagonal cruzando por cima de tudo.
		wxPoint start(portX(from, true), from.GetY() + from.GetHeight());
		wxPoint end(portX(to, false), to.GetY());
		int midY = (start.y + end.y) / 2;

		if (start.x != end.x && haveLayers)
		{
			// Não deixa o trecho horizontal de várias setas que cruzam o
			// MESMO vão entre camadas cair na mesma altura -- sem isso elas
			// desenham uma em cima da outra e viram uma linha grossa
			// indistinguível. Cada seta que atravessa o mesmo par (camada
			// de origem, camada de destino) ganha sua própria "raia".
			int laneCount = 0, laneIndex = 0, seen = 0;
			for (int i = 0; i < (int)edges.size(); i++)
			{
				const BlockEdge& other = edges[i];
				if (blockLayer_[other.fromBlock] != fromLayer || blockLayer_[other.toBlock] != toLayer)
					continue;
				if (i == myIdx)
					laneIndex = seen;
				seen++;
				laneCount++;
			}
			if (laneCount > 1)
			{
				int span = std::min(std::max(kLayerGap - 24, kEdgeLaneStep), kEdgeLaneStep * (laneCount - 1));
				midY = (midY - span / 2) + (span * laneIndex) / (laneCount - 1);
			}
		}

		path.push_back(start);
		if (start.x != end.x)
		{
			path.push_back(wxPoint(start.x, midY));
			path.push_back(wxPoint(end.x, midY));
		}
		path.push_back(end);
	}

	return path;
}

namespace
{
	// Distância de p até o segmento [a,b] -- usada por hitTestEdge pra achar
	// qual trecho do polyline está mais perto do mouse.
	double DistancePointToSegment(const wxPoint& p, const wxPoint& a, const wxPoint& b)
	{
		double dx = b.x - a.x, dy = b.y - a.y;
		double lenSq = dx * dx + dy * dy;
		if (lenSq < 0.0001)
			return std::hypot(p.x - a.x, p.y - a.y);
		double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lenSq;
		t = std::max(0.0, std::min(1.0, t));
		double px = a.x + t * dx, py = a.y + t * dy;
		return std::hypot(p.x - px, p.y - py);
	}
}

int GraphViewCanvas::hitTestEdge(const wxPoint& logicalPos) const
{
	for (int i = 0; i < (int)edges.size(); i++)
	{
		std::vector<wxPoint> path = computeEdgePath(edges[i]);
		for (size_t s = 0; s + 1 < path.size(); s++)
		{
			if (DistancePointToSegment(logicalPos, path[s], path[s + 1]) <= kEdgeHitTolerance)
				return i;
		}
	}
	return -1;
}

void GraphViewCanvas::focusOnBlock(int index)
{
	if (index < 0 || index >= (int)blocks.size())
		return;

	selectedBlock = index;

	// Centraliza a área visível no bloco (em vez de só garantir que ele
	// apareça na borda) -- é o "dar foco no bloco de destino" pedido pra
	// clique numa seta.
	const wxRect& r = blocks[index].rect;
	wxSize clientSize = GetClientSize();
	int centerX = (int)((r.GetX() + r.GetWidth() / 2) * zoom_) - clientSize.GetWidth() / 2;
	int centerY = (int)((r.GetY() + r.GetHeight() / 2) * zoom_) - clientSize.GetHeight() / 2;

	int ppuX, ppuY;
	GetScrollPixelsPerUnit(&ppuX, &ppuY);
	if (ppuX <= 0) ppuX = 1;
	if (ppuY <= 0) ppuY = 1;
	Scroll(std::max(0, centerX) / ppuX, std::max(0, centerY) / ppuY);

	Refresh();
}

int GraphViewCanvas::findOrCreateBlockAt(u32 addr)
{
	for (int i = 0; i < (int)blocks.size(); i++)
	{
		if (blocks[i].start == addr)
			return i;
	}

	// Não existe ainda -- vira um bloco manual novo. Registra o endereço em
	// manualBlockAddrs_ (persiste através de rebuild()/Refresh) e refaz o
	// grafo inteiro, já que layoutBlocks() precisa recalcular tudo do zero
	// pra encaixar o bloco novo em algum lugar sensato.
	manualBlockAddrs_.push_back(addr);
	rebuild();

	for (int i = 0; i < (int)blocks.size(); i++)
	{
		if (blocks[i].start == addr)
			return i;
	}
	return -1; // endereço inválido/sem instrução legível -- rebuild não conseguiu criar o bloco
}

void GraphViewCanvas::onPaint(wxPaintEvent& evt)
{
	wxAutoBufferedPaintDC dc(this);
	DoPrepareDC(dc);
	dc.Clear();
	dc.SetUserScale(zoom_, zoom_);
	dc.SetFont(GetFont());

	// Arestas primeiro, atrás das caixas.
	for (int ei = 0; ei < (int)edges.size(); ei++)
	{
		const BlockEdge& e = edges[ei];
		std::vector<wxPoint> path = computeEdgePath(e);
		bool isHovered = (ei == hoveredEdge);

		wxColour colour;
		const wchar_t* edgeLetter = nullptr; // "T"/"F" desenhado perto da metade da seta, só nos branches condicionais
		switch (e.kind)
		{
			case EdgeKind::Taken: colour = wxColour(40, 160, 60); edgeLetter = L"T"; break;
			case EdgeKind::Fallthrough: colour = wxColour(190, 60, 50); edgeLetter = L"F"; break;
			case EdgeKind::Switch: colour = wxColour(230, 140, 40); break;
			default: colour = wxColour(60, 110, 200); break;
		}

		// Seta sob o mouse fica branca e mais grossa -- é o "identificadas
		// ao passar o mouse" pedido, sem precisar de tooltip separado (a
		// barra de dica embaixo da janela já mostra origem/destino, ver
		// onMotion()).
		wxPen pen(isHovered ? *wxWHITE : colour, isHovered ? 3 : 2);
		dc.SetPen(pen);
		for (size_t s = 0; s + 1 < path.size(); s++)
			dc.DrawLine(path[s], path[s + 1]);

		wxPoint end = path.back();
		wxPoint beforeEnd = path[path.size() - 2];
		double angle = atan2((double)(end.y - beforeEnd.y), (double)(end.x - beforeEnd.x));
		const double arrowLen = 9.0, arrowAngle = 0.5;
		wxPoint p1(end.x - (int)(arrowLen * cos(angle - arrowAngle)), end.y - (int)(arrowLen * sin(angle - arrowAngle)));
		wxPoint p2(end.x - (int)(arrowLen * cos(angle + arrowAngle)), end.y - (int)(arrowLen * sin(angle + arrowAngle)));
		dc.SetBrush(wxBrush(isHovered ? *wxWHITE : colour));
		wxPoint arrowPoly[3] = {end, p1, p2};
		dc.DrawPolygon(3, arrowPoly);

		if (edgeLetter)
		{
			// "T"/"F" perto do primeiro cotovelo, deslocado um pouco pro
			// lado pra não ficar em cima da linha.
			wxPoint mid = path[path.size() / 2];
			dc.SetTextForeground(isHovered ? *wxWHITE : colour);
			dc.DrawText(edgeLetter, mid.x + 6, mid.y - dc.GetCharHeight() / 2);
		}
	}

	// Depois as caixas, com o texto das instruções dentro.
	for (int i = 0; i < (int)blocks.size(); i++)
	{
		const BasicBlock& b = blocks[i];
		wxColour fill = colourForBlock(i);

		dc.SetBrush(wxBrush(fill));
		dc.SetPen(wxPen(i == selectedBlock ? *wxYELLOW : wxColour(20, 20, 20), i == selectedBlock ? 2 : 1));
		dc.DrawRectangle(b.rect);

		int lineH = dc.GetCharHeight() + 2;
		int ty = b.rect.GetY() + kBoxVPad;

		if (!b.label.empty())
		{
			// Label do bloco (símbolo já existente ou "pos_XXXXXXXX" quando
			// este bloco é alvo de algum branch/jump) num tom de destaque,
			// separada visualmente das instruções abaixo -- igual ao
			// "loc_XXXXXXXX:" que o IDA mostra antes de um bloco referenciado.
			dc.SetTextForeground(wxColour(255, 205, 90));
			dc.DrawText(wxString(b.label.c_str(), wxConvUTF8) + L":", b.rect.GetX() + kBoxHPad, ty);
			ty += lineH;
		}

		dc.SetTextForeground(*wxWHITE);
		for (const std::string& l : b.lines)
		{
			dc.DrawText(wxString(l.c_str(), wxConvUTF8), b.rect.GetX() + kBoxHPad, ty);
			ty += lineH;
		}
	}
}

int GraphViewCanvas::hitTest(const wxPoint& logicalPos) const
{
	for (int i = 0; i < (int)blocks.size(); i++)
	{
		if (blocks[i].rect.Contains(logicalPos))
			return i;
	}
	return -1;
}

void GraphViewCanvas::gotoBlock(int index)
{
	if (index < 0 || index >= (int)blocks.size())
		return;
	disassemblyView->gotoAddress(blocks[index].start);
}

void GraphViewCanvas::onLeftDown(wxMouseEvent& evt)
{
	wxPoint device = CalcUnscrolledPosition(evt.GetPosition());
	wxPoint logical((int)(device.x / zoom_), (int)(device.y / zoom_));
	int idx = hitTest(logical);

	if (idx < 0)
	{
		// Antes de assumir pan, checa se o clique foi em cima de uma seta:
		// clicar nela dá foco (centraliza+seleciona) no bloco de DESTINO,
		// sem precisar caçar a caixa manualmente numa árvore grande.
		int edgeIdx = hitTestEdge(logical);
		if (edgeIdx >= 0)
		{
			focusOnBlock(edges[edgeIdx].toBlock);
			return;
		}

		// Clique no vazio de verdade: arrasta a visão (pan).
		isPanning = true;
		panStartMouse = evt.GetPosition();
		int ppuX, ppuY;
		GetScrollPixelsPerUnit(&ppuX, &ppuY);
		wxPoint viewStart = GetViewStart();
		panStartScrollX = viewStart.x * ppuX;
		panStartScrollY = viewStart.y * ppuY;
		CaptureMouse();
		SetCursor(wxCursor(wxCURSOR_SIZING));
		selectedBlock = -1;
		Refresh();
		return;
	}

	// Clique EM CIMA de um bloco: seleciona igual antes E prepara um
	// possível arrasto dele (em qualquer direção) -- ver draggingBlock.
	draggingBlock = idx;
	dragStartMouse = evt.GetPosition();
	dragStartBlockOrigin = blocks[idx].rect.GetPosition();
	didDragBlock = false;
	CaptureMouse();
	selectedBlock = idx;
	Refresh();
}

void GraphViewCanvas::onLeftUp(wxMouseEvent& evt)
{
	if (draggingBlock >= 0)
	{
		draggingBlock = -1;
		didDragBlock = false;
		if (HasCapture())
			ReleaseMouse();
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

void GraphViewCanvas::onMotion(wxMouseEvent& evt)
{
	if (draggingBlock >= 0 && evt.LeftIsDown())
	{
		wxPoint deltaScreen = evt.GetPosition() - dragStartMouse;
		if (!didDragBlock && (std::abs(deltaScreen.x) > 3 || std::abs(deltaScreen.y) > 3))
			didDragBlock = true;

		if (didDragBlock)
		{
			wxPoint newPos = dragStartBlockOrigin + wxPoint((int)(deltaScreen.x / zoom_), (int)(deltaScreen.y / zoom_));
			newPos.x = std::max(0, newPos.x);
			newPos.y = std::max(0, newPos.y);
			blocks[draggingBlock].rect.SetPosition(newPos);

			int rightEdge = newPos.x + blocks[draggingBlock].rect.GetWidth() + 30;
			int bottomEdge = newPos.y + blocks[draggingBlock].rect.GetHeight() + 30;
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
		// com wxBG_STYLE_PAINT + DC bufferizado (nosso onPaint sempre redesenha
		// tudo do zero) -- sem isso a posição de scroll muda de verdade mas a
		// tela não acompanha visualmente.
		Refresh(false);
		return;
	}

	// Hover em seta (só quando não está arrastando bloco nem panning, senão
	// ficaria testando toda seta a cada pixel de um arrasto à toa). Muda o
	// cursor pra "mão" como indicação extra de que é clicável, além do
	// destaque em branco desenhado em onPaint().
	{
		wxPoint device = CalcUnscrolledPosition(evt.GetPosition());
		wxPoint logical((int)(device.x / zoom_), (int)(device.y / zoom_));
		int edgeIdx = (hitTest(logical) < 0) ? hitTestEdge(logical) : -1;
		if (edgeIdx != hoveredEdge)
		{
			hoveredEdge = edgeIdx;
			SetCursor(edgeIdx >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
			Refresh(false);
		}
	}
	evt.Skip();
}

void GraphViewCanvas::onMouseCaptureLost(wxMouseCaptureLostEvent& evt)
{
	isPanning = false;
	draggingBlock = -1;
	didDragBlock = false;
	hoveredEdge = -1;
	SetCursor(wxNullCursor);
}

void GraphViewCanvas::onMouseWheel(wxMouseEvent& evt)
{
	if (!evt.ControlDown())
	{
		evt.Skip();
		return;
	}

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

void GraphViewCanvas::onLeftDClick(wxMouseEvent& evt)
{
	wxPoint device = CalcUnscrolledPosition(evt.GetPosition());
	wxPoint logical((int)(device.x / zoom_), (int)(device.y / zoom_));
	int idx = hitTest(logical);
	if (idx < 0)
		return;
	gotoBlock(idx);
}

void GraphViewCanvas::onRightDown(wxMouseEvent& evt)
{
	wxPoint device = CalcUnscrolledPosition(evt.GetPosition());
	wxPoint logical((int)(device.x / zoom_), (int)(device.y / zoom_));
	int idx = hitTest(logical);

	if (idx >= 0)
	{
		contextBlock = idx;
		contextEdge = -1;
		selectedBlock = idx;
		Refresh();

		wxMenu popup;
		popup.Append(ID_GRAPHVIEW_GOTO, L"Go to Address");
		popup.Append(ID_GRAPHVIEW_COPYADDRESS, L"Copy Block Address");
		popup.AppendSeparator();
		popup.Append(ID_GRAPHVIEW_REFRESH, L"Refresh Graph");
		popup.Bind(wxEVT_MENU, &GraphViewCanvas::onPopupClick, this);
		PopupMenu(&popup, evt.GetPosition());
		return;
	}

	int edgeIdx = hitTestEdge(logical);
	if (edgeIdx >= 0)
	{
		contextEdge = edgeIdx;
		contextBlock = -1;

		wxMenu popup;
		popup.Append(ID_GRAPHVIEW_GOTO, L"Focus Target Block");
		if (edges[edgeIdx].branchAddr != 0)
			popup.Append(ID_GRAPHVIEW_REDIRECT_EDGE, L"Redirect this branch...");
		popup.Bind(wxEVT_MENU, &GraphViewCanvas::onPopupClick, this);
		PopupMenu(&popup, evt.GetPosition());
		return;
	}

	// Vazio de verdade: oferece criar um bloco manual num endereço
	// específico (ver findOrCreateBlockAt/manualBlockAddrs_).
	wxMenu popup;
	popup.Append(ID_GRAPHVIEW_ADD_BLOCK, L"Add code block at address...");
	popup.AppendSeparator();
	popup.Append(ID_GRAPHVIEW_REFRESH, L"Refresh Graph");
	popup.Bind(wxEVT_MENU, &GraphViewCanvas::onPopupClick, this);
	PopupMenu(&popup, evt.GetPosition());
}

void GraphViewCanvas::onPopupClick(wxCommandEvent& evt)
{
	switch (evt.GetId())
	{
		case ID_GRAPHVIEW_GOTO:
			if (contextEdge >= 0)
				focusOnBlock(edges[contextEdge].toBlock);
			else
				gotoBlock(contextBlock);
			break;
		case ID_GRAPHVIEW_COPYADDRESS:
			if (contextBlock >= 0 && contextBlock < (int)blocks.size() && wxTheClipboard->Open())
			{
				wxTheClipboard->SetData(new wxTextDataObject(wxString::Format(L"%08X", blocks[contextBlock].start)));
				wxTheClipboard->Close();
			}
			break;
		case ID_GRAPHVIEW_REFRESH:
			rebuild();
			break;
		case ID_GRAPHVIEW_REDIRECT_EDGE:
		{
			if (contextEdge < 0 || contextEdge >= (int)edges.size())
				break;
			const BlockEdge& e = edges[contextEdge];
			if (e.branchAddr == 0)
				break;

			u32 defaultTarget = blocks[e.toBlock].start;
			wxString newTargetStr = wxGetTextFromUser(
				L"Enter the new target address (hex) for this branch:",
				L"Redirect branch", wxString::Format(L"%08X", defaultTarget), this);
			if (newTargetStr.empty())
				break;

			unsigned long newTarget = 0;
			if (!newTargetStr.ToULong(&newTarget, 16))
			{
				wxMessageBox(L"Invalid hex address.", L"Error.", wxICON_ERROR, this);
				break;
			}

			if (disassemblyView->PatchBranchTarget(e.branchAddr, (u32)newTarget))
			{
				// Garante que o novo alvo tem um bloco no grafo (cria um
				// manual se ainda não existir dentro dos limites da
				// função atual) antes de reconstruir -- senão o rebuild()
				// dispara pela análise de fluxo normal, que já vai achar o
				// novo alvo sozinho SE ele estiver dentro de [funcAddr,
				// funcEnd); fora disso (redirecionou pra outra função)
				// findOrCreateBlockAt cobre criando um bloco manual.
				if (newTarget < funcAddr || newTarget >= funcEnd)
					manualBlockAddrs_.push_back((u32)newTarget);
				rebuild();
			}
			break;
		}
		case ID_GRAPHVIEW_ADD_BLOCK:
		{
			wxString addrStr = wxGetTextFromUser(
				L"Enter the address (hex) to add as a code block:",
				L"Add code block", wxEmptyString, this);
			if (addrStr.empty())
				break;

			unsigned long addr = 0;
			if (!addrStr.ToULong(&addr, 16))
			{
				wxMessageBox(L"Invalid hex address.", L"Error.", wxICON_ERROR, this);
				break;
			}

			int idx = findOrCreateBlockAt((u32)addr);
			if (idx < 0)
				wxMessageBox(L"Could not read code at that address.", L"Error.", wxICON_ERROR, this);
			else
				focusOnBlock(idx);
			break;
		}
	}
}
