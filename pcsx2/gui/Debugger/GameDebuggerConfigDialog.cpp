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
#include "GameDebuggerConfigDialog.h"
#include "System.h"
#include "gui/AppCoreThread.h"

enum
{
	ID_ADD_REGION = 2001,
	ID_EDIT_REGION,
	ID_REMOVE_REGION,
	ID_APPLY_NOW,
	ID_SAVE_CONFIG
};

wxBEGIN_EVENT_TABLE(GameDebuggerConfigDialog, wxDialog)
	EVT_BUTTON(ID_ADD_REGION, GameDebuggerConfigDialog::OnAdd)
	EVT_BUTTON(ID_EDIT_REGION, GameDebuggerConfigDialog::OnEdit)
	EVT_BUTTON(ID_REMOVE_REGION, GameDebuggerConfigDialog::OnRemove)
	EVT_BUTTON(ID_APPLY_NOW, GameDebuggerConfigDialog::OnApplyNow)
	EVT_BUTTON(ID_SAVE_CONFIG, GameDebuggerConfigDialog::OnSave)
wxEND_EVENT_TABLE()

class EditOverlayRegionDialog : public wxDialog
{
public:
	EditOverlayRegionDialog(wxWindow* parent, const GameOverlayRegion& reg)
		: wxDialog(parent, wxID_ANY, L"Edit Overlay / Memory Region Definition", wxDefaultPosition, wxDefaultSize)
		, region(reg)
	{
		wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);
		wxFlexGridSizer* gridSizer = new wxFlexGridSizer(5, 3, 8, 8);

		gridSizer->Add(new wxStaticText(this, wxID_ANY, L"Region / Overlay Name:"), 0, wxALIGN_CENTER_VERTICAL);
		nameCtrl = new wxTextCtrl(this, wxID_ANY, wxString(reg.name.c_str(), wxConvUTF8));
		gridSizer->Add(nameCtrl, 1, wxEXPAND);
		gridSizer->AddSpacer(0);

		gridSizer->Add(new wxStaticText(this, wxID_ANY, L"Start Address (Hex):"), 0, wxALIGN_CENTER_VERTICAL);
		startCtrl = new wxTextCtrl(this, wxID_ANY, wxString::Format(L"%08X", reg.startAddress));
		gridSizer->Add(startCtrl, 1, wxEXPAND);
		gridSizer->AddSpacer(0);

		gridSizer->Add(new wxStaticText(this, wxID_ANY, L"End Address (Hex):"), 0, wxALIGN_CENTER_VERTICAL);
		endCtrl = new wxTextCtrl(this, wxID_ANY, wxString::Format(L"%08X", reg.endAddress));
		gridSizer->Add(endCtrl, 1, wxEXPAND);
		gridSizer->AddSpacer(0);

		gridSizer->Add(new wxStaticText(this, wxID_ANY, L"Attached SYM Table File:"), 0, wxALIGN_CENTER_VERTICAL);
		symCtrl = new wxTextCtrl(this, wxID_ANY, wxString(reg.symFilePath.c_str(), wxConvUTF8));
		gridSizer->Add(symCtrl, 1, wxEXPAND);
		wxButton* symBtn = new wxButton(this, wxID_ANY, L"Browse...");
		symBtn->Bind(wxEVT_BUTTON, &EditOverlayRegionDialog::OnBrowseSym, this);
		gridSizer->Add(symBtn, 0);

		gridSizer->Add(new wxStaticText(this, wxID_ANY, L"Optional Load File (BIN/ELF):"), 0, wxALIGN_CENTER_VERTICAL);
		binCtrl = new wxTextCtrl(this, wxID_ANY, wxString(reg.binFilePath.c_str(), wxConvUTF8));
		gridSizer->Add(binCtrl, 1, wxEXPAND);
		wxButton* binBtn = new wxButton(this, wxID_ANY, L"Browse...");
		binBtn->Bind(wxEVT_BUTTON, &EditOverlayRegionDialog::OnBrowseBin, this);
		gridSizer->Add(binBtn, 0);

		gridSizer->AddGrowableCol(1);
		mainSizer->Add(gridSizer, 0, wxEXPAND | wxALL, 10);

		wxSizer* btnSizer = CreateButtonSizer(wxOK | wxCANCEL);
		mainSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 10);

		Bind(wxEVT_BUTTON, &EditOverlayRegionDialog::OnOK, this, wxID_OK);

		SetSizerAndFit(mainSizer);
	}

	GameOverlayRegion GetRegion() const { return region; }

protected:
	void OnBrowseSym(wxCommandEvent& evt)
	{
		wxFileDialog dlg(this, L"Select Symbol File", wxEmptyString, wxEmptyString, L"Symbol Files (*.sym;*.map)|*.sym;*.map|All Files (*.*)|*.*", wxFD_OPEN);
		if (dlg.ShowModal() == wxID_OK)
			symCtrl->SetValue(dlg.GetPath());
	}

	void OnBrowseBin(wxCommandEvent& evt)
	{
		wxFileDialog dlg(this, L"Select Binary / Overlay File", wxEmptyString, wxEmptyString, L"Binary / Overlay Files (*.bin;*.elf;*.ovl)|*.bin;*.elf;*.ovl|All Files (*.*)|*.*", wxFD_OPEN);
		if (dlg.ShowModal() == wxID_OK)
			binCtrl->SetValue(dlg.GetPath());
	}

	void OnOK(wxCommandEvent& evt)
	{
		region.name = std::string(nameCtrl->GetValue().ToUTF8());
		region.symFilePath = std::string(symCtrl->GetValue().ToUTF8());
		region.binFilePath = std::string(binCtrl->GetValue().ToUTF8());

		sscanf(startCtrl->GetValue().ToUTF8(), "%x", &region.startAddress);
		sscanf(endCtrl->GetValue().ToUTF8(), "%x", &region.endAddress);

		EndModal(wxID_OK);
	}

private:
	GameOverlayRegion region;
	wxTextCtrl* nameCtrl;
	wxTextCtrl* startCtrl;
	wxTextCtrl* endCtrl;
	wxTextCtrl* symCtrl;
	wxTextCtrl* binCtrl;
};

GameDebuggerConfigDialog::GameDebuggerConfigDialog(wxWindow* parent, DebugInterface* _cpu)
	: wxDialog(parent, wxID_ANY, L"Per-Game Debugger Memory Regions & SYM Table Settings", wxDefaultPosition, wxSize(650, 420), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, cpu(_cpu)
{
	discId = std::string(GameInfo::gameCRC.ToUTF8());
	if (discId.empty())
		discId = "DEFAULT";

	regions = GameDebuggerConfigManager::LoadRegions(discId);

	wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);

	wxString labelText = wxString::Format(L"Active Game CRC: %s", wxString(discId.c_str(), wxConvUTF8));
	gameLabel = new wxStaticText(this, wxID_ANY, labelText);
	wxFont boldFont = gameLabel->GetFont();
	boldFont.SetWeight(wxFONTWEIGHT_BOLD);
	gameLabel->SetFont(boldFont);
	mainSizer->Add(gameLabel, 0, wxALL, 10);

	regionListView = new wxListView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
	regionListView->InsertColumn(0, L"Region Name", wxLIST_FORMAT_LEFT, 130);
	regionListView->InsertColumn(1, L"Start Addr", wxLIST_FORMAT_LEFT, 90);
	regionListView->InsertColumn(2, L"End Addr", wxLIST_FORMAT_LEFT, 90);
	regionListView->InsertColumn(3, L"SYM File", wxLIST_FORMAT_LEFT, 150);
	regionListView->InsertColumn(4, L"BIN File", wxLIST_FORMAT_LEFT, 150);

	mainSizer->Add(regionListView, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

	wxBoxSizer* btnSizer = new wxBoxSizer(wxHORIZONTAL);
	btnSizer->Add(new wxButton(this, ID_ADD_REGION, L"Add Region"), 0, wxRIGHT, 5);
	btnSizer->Add(new wxButton(this, ID_EDIT_REGION, L"Edit Region"), 0, wxRIGHT, 5);
	btnSizer->Add(new wxButton(this, ID_REMOVE_REGION, L"Remove Region"), 0, wxRIGHT, 15);
	btnSizer->Add(new wxButton(this, ID_APPLY_NOW, L"Apply to Debugger Now"), 0, wxRIGHT, 5);
	btnSizer->Add(new wxButton(this, ID_SAVE_CONFIG, L"Save Per-Game Settings"), 0, wxRIGHT, 5);

	mainSizer->Add(btnSizer, 0, wxALL | wxALIGN_RIGHT, 10);

	SetSizer(mainSizer);
	PopulateList();
}

void GameDebuggerConfigDialog::PopulateList()
{
	regionListView->DeleteAllItems();
	for (size_t i = 0; i < regions.size(); i++)
	{
		const auto& reg = regions[i];
		long idx = regionListView->InsertItem(i, wxString(reg.name.c_str(), wxConvUTF8));
		regionListView->SetItem(idx, 1, wxString::Format(L"0x%08X", reg.startAddress));
		regionListView->SetItem(idx, 2, wxString::Format(L"0x%08X", reg.endAddress));
		regionListView->SetItem(idx, 3, wxString(reg.symFilePath.c_str(), wxConvUTF8));
		regionListView->SetItem(idx, 4, wxString(reg.binFilePath.c_str(), wxConvUTF8));
	}
}

void GameDebuggerConfigDialog::OnAdd(wxCommandEvent& evt)
{
	GameOverlayRegion newReg;
	newReg.name = std::string(wxString::Format(L"OVERLAY_%zu", regions.size() + 1).ToUTF8());
	newReg.startAddress = 0x00200000;
	newReg.endAddress = 0x00280000;

	EditOverlayRegionDialog dlg(this, newReg);
	if (dlg.ShowModal() == wxID_OK)
	{
		regions.push_back(dlg.GetRegion());
		PopulateList();
	}
}

void GameDebuggerConfigDialog::OnEdit(wxCommandEvent& evt)
{
	long selected = regionListView->GetFirstSelected();
	if (selected < 0 || selected >= (long)regions.size())
	{
		wxMessageBox(L"Please select a region to edit.", L"Info", wxICON_INFORMATION, this);
		return;
	}

	EditOverlayRegionDialog dlg(this, regions[selected]);
	if (dlg.ShowModal() == wxID_OK)
	{
		regions[selected] = dlg.GetRegion();
		PopulateList();
	}
}

void GameDebuggerConfigDialog::OnRemove(wxCommandEvent& evt)
{
	long selected = regionListView->GetFirstSelected();
	if (selected < 0 || selected >= (long)regions.size())
	{
		wxMessageBox(L"Please select a region to remove.", L"Info", wxICON_INFORMATION, this);
		return;
	}

	regions.erase(regions.begin() + selected);
	PopulateList();
}

void GameDebuggerConfigDialog::OnApplyNow(wxCommandEvent& evt)
{
	GameDebuggerConfigManager::ApplyRegionsToCpu(cpu, regions);
	wxMessageBox(L"Loaded overlay regions & symbol maps into active Debugger!", L"Success", wxICON_INFORMATION, this);
}

void GameDebuggerConfigDialog::OnSave(wxCommandEvent& evt)
{
	GameDebuggerConfigManager::SaveRegions(discId, regions);
	GameDebuggerConfigManager::ApplyRegionsToCpu(cpu, regions);
	wxMessageBox(L"Per-game debugger settings saved successfully!", L"Saved", wxICON_INFORMATION, this);
	EndModal(wxID_OK);
}
