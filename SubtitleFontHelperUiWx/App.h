#pragma once

#include "LauncherConfig.h"
#include "SingleInstance.h"

#include <memory>

#include <wx/app.h>

namespace sfh::ui
{
	class App : public wxApp
	{
	public:
		bool OnInit() override;

	private:
		LauncherConfig m_config;
		std::unique_ptr<SingleInstance> m_singleInstance;
	};
}
