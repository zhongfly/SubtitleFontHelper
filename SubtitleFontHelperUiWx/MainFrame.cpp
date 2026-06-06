#include "MainFrame.h"

#include <wx/font.h>

sfh::ui::MainFrame::MainFrame(const LauncherConfig& config)
	: wxFrame(nullptr, wxID_ANY, BuildSingleInstanceWindowTitle(config), wxDefaultPosition, wxSize(960, 720)),
	  m_config(config)
{
	BuildLayout();
	CentreOnScreen();
}

void sfh::ui::MainFrame::BuildLayout()
{
	m_panel = new wxPanel(this);
	auto* rootSizer = new wxBoxSizer(wxVERTICAL);

	m_titleText = new wxStaticText(
		m_panel,
		wxID_ANY,
		m_config.m_windowKind == WindowKind::Fonts ? L"字体窗口迁移中" : L"日志窗口迁移中");
	wxFont titleFont = m_titleText->GetFont();
	titleFont.SetPointSize(titleFont.GetPointSize() + 4);
	titleFont.SetWeight(wxFONTWEIGHT_BOLD);
	m_titleText->SetFont(titleFont);

	m_statusText = new wxStaticText(m_panel, wxID_ANY, BuildStatusText());

	rootSizer->Add(m_titleText, 0, wxALL, 16);
	rootSizer->Add(m_statusText, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);

	m_panel->SetSizer(rootSizer);
}

wxString sfh::ui::MainFrame::BuildStatusText() const
{
	wxString text;
	text << L"rpc pipe: " << (m_config.m_rpcPipeName.empty() ? L"(none)" : m_config.m_rpcPipeName);
	if (m_config.m_windowKind == WindowKind::Logs)
	{
		text << L"\nlog file: " << (m_config.m_logFilePath.empty() ? L"(none)" : m_config.m_logFilePath);
	}
	return text;
}
