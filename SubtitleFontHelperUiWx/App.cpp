#include "App.h"

#include "MainFrame.h"

#include <stdexcept>

#include <wx/msgdlg.h>
#include <wx/app.h>

wxIMPLEMENT_APP(sfh::ui::App);

bool sfh::ui::App::OnInit()
{
	try
	{
		m_config = ParseLauncherConfig();
		m_singleInstance = std::make_unique<SingleInstance>(BuildSingleInstanceMutexName(m_config));
		if (!m_singleInstance->IsPrimaryInstance())
		{
			throw std::runtime_error("window is already running");
		}

		auto* frame = new MainFrame(m_config);
		frame->Show(true);
		SetTopWindow(frame);
		return true;
	}
	catch (const std::exception& e)
	{
		wxMessageBox(wxString::FromUTF8(e.what()), L"SubtitleFontHelper UI", wxOK | wxICON_ERROR);
		return false;
	}
}
