#pragma once

#include "LauncherConfig.h"

#include <wx/frame.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace sfh::ui
{
	class MainFrame : public wxFrame
	{
	public:
		explicit MainFrame(const LauncherConfig& config);

	private:
		void BuildLayout();
		wxString BuildStatusText() const;

	private:
		LauncherConfig m_config;
		wxPanel* m_panel = nullptr;
		wxStaticText* m_titleText = nullptr;
		wxStaticText* m_statusText = nullptr;
	};
}
