#pragma once

#include <string>

namespace sfh::ui
{
	enum class WindowKind
	{
		Fonts,
		Logs
	};

	struct LauncherConfig
	{
		WindowKind m_windowKind = WindowKind::Fonts;
		std::wstring m_rpcPipeName;
		std::wstring m_logFilePath;
	};

	LauncherConfig ParseLauncherConfig();
	std::wstring BuildSingleInstanceMutexName(const LauncherConfig& config);
	std::wstring BuildSingleInstanceWindowTitle(const LauncherConfig& config);
}
