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
#include "DisassembleRegionDialog.h"

wxBEGIN_EVENT_TABLE(DisassembleRegionDialog, wxDialog)
	EVT_BUTTON(wxID_OK, DisassembleRegionDialog::OnOK)
wxEND_EVENT_TABLE()

DisassembleRegionDialog::DisassembleRegionDialog(wxWindow* parent, u32 defaultStartAddr, u32 defaultEndAddr)
	: wxDialog(parent, wxID_ANY, L"Export Disassembly Region to Text (*.txt)", wxDefaultPosition, wxDefaultSize)
	, startAddress(defaultStartAddr)
	, endAddress(defaultEndAddr)
{
	wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);

	wxFlexGridSizer* gridSizer = new wxFlexGridSizer(2, 2, 8, 12);

	gridSizer->Add(new wxStaticText(this, wxID_ANY, L"Start Address (Hex):"), 0, wxALIGN_CENTER_VERTICAL);
	startTextCtrl = new wxTextCtrl(this, wxID_ANY, wxString::Format(L"%08X", defaultStartAddr));
	gridSizer->Add(startTextCtrl, 1, wxEXPAND);

	gridSizer->Add(new wxStaticText(this, wxID_ANY, L"End Address (Hex):"), 0, wxALIGN_CENTER_VERTICAL);
	endTextCtrl = new wxTextCtrl(this, wxID_ANY, wxString::Format(L"%08X", defaultEndAddr));
	gridSizer->Add(endTextCtrl, 1, wxEXPAND);

	gridSizer->AddGrowableCol(1);
	mainSizer->Add(gridSizer, 0, wxEXPAND | wxALL, 12);

	wxSizer* buttonSizer = CreateButtonSizer(wxOK | wxCANCEL);
	mainSizer->Add(buttonSizer, 0, wxEXPAND | wxALL, 10);

	SetSizerAndFit(mainSizer);
}

void DisassembleRegionDialog::OnOK(wxCommandEvent& evt)
{
	wxString startStr = startTextCtrl->GetValue();
	wxString endStr = endTextCtrl->GetValue();

	u32 start = 0, end = 0;
	if (sscanf(startStr.ToUTF8(), "%x", &start) != 1 || sscanf(endStr.ToUTF8(), "%x", &end) != 1)
	{
		wxMessageBox(L"Invalid hex address entered.", L"Error", wxICON_ERROR, this);
		return;
	}

	if (start >= end)
	{
		wxMessageBox(L"Start Address must be less than End Address.", L"Error", wxICON_ERROR, this);
		return;
	}

	startAddress = start;
	endAddress = end;

	EndModal(wxID_OK);
}
