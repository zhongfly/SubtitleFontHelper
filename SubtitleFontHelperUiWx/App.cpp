#include "App.h"

#include "FontsFrame.h"
#include "LogsFrame.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <stdexcept>

#include <wx/msgdlg.h>
#include <wx/app.h>

namespace
{
	void TryActivateExistingWindow(const sfh::ui::LauncherConfig& config)
	{
		const auto title = sfh::ui::BuildSingleInstanceWindowTitle(config);
		HWND existingWindow = FindWindowW(nullptr, title.c_str());
		if (existingWindow == nullptr)
		{
			return;
		}

		if (IsIconic(existingWindow))
		{
			ShowWindow(existingWindow, SW_RESTORE);
		}
		else
		{
			ShowWindow(existingWindow, SW_SHOW);
		}
		SetForegroundWindow(existingWindow);
	}
}

wxIMPLEMENT_APP(sfh::ui::App);

bool sfh::ui::App::OnInit()
{
	try
	{
		m_config = ParseLauncherConfig();
		m_singleInstance = std::make_unique<SingleInstance>(BuildSingleInstanceMutexName(m_config));
		if (!m_singleInstance->IsPrimaryInstance())
		{
			TryActivateExistingWindow(m_config);
			return false;
		}

		wxFrame* frame = nullptr;
		if (m_config.m_windowKind == WindowKind::Fonts)
		{
			frame = new FontsFrame(m_config);
		}
		else
		{
			frame = new LogsFrame(m_config);
		}
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
