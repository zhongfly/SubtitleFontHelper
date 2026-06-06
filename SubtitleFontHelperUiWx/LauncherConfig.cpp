#include "LauncherConfig.h"

#include <Windows.h>
#include <shellapi.h>

#include <stdexcept>
#include <string_view>
#include <vector>

namespace
{
	using namespace sfh::ui;

	std::wstring GetArgumentValue(const std::vector<std::wstring>& arguments, size_t& index)
	{
		if (index + 1 >= arguments.size())
		{
			throw std::runtime_error("missing argument value");
		}
		++index;
		return arguments[index];
	}

	std::vector<std::wstring> GetCommandLineArguments()
	{
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (argv == nullptr)
		{
			throw std::runtime_error("failed to parse command line");
		}

		struct ArgvReleaser
		{
			LPWSTR* m_value = nullptr;

			~ArgvReleaser()
			{
				if (m_value != nullptr)
				{
					LocalFree(m_value);
				}
			}
		} releaser{ argv };

		std::vector<std::wstring> arguments;
		arguments.reserve(static_cast<size_t>(argc));
		for (int i = 1; i < argc; ++i)
		{
			arguments.emplace_back(argv[i]);
		}
		return arguments;
	}
}

sfh::ui::LauncherConfig sfh::ui::ParseLauncherConfig()
{
	LauncherConfig config;
	const auto arguments = GetCommandLineArguments();
	for (size_t i = 0; i < arguments.size(); ++i)
	{
		const std::wstring_view argument(arguments[i]);
		if (argument == L"--window")
		{
			const auto value = GetArgumentValue(arguments, i);
			if (value == L"fonts")
			{
				config.m_windowKind = WindowKind::Fonts;
			}
			else if (value == L"logs")
			{
				config.m_windowKind = WindowKind::Logs;
			}
			else
			{
				throw std::runtime_error("unknown window kind");
			}
		}
		else if (argument == L"--rpc-pipe")
		{
			config.m_rpcPipeName = GetArgumentValue(arguments, i);
		}
		else if (argument == L"--log-file")
		{
			config.m_logFilePath = GetArgumentValue(arguments, i);
		}
		else
		{
			throw std::runtime_error("unknown command line option");
		}
	}

	return config;
}

std::wstring sfh::ui::BuildSingleInstanceMutexName(const LauncherConfig& config)
{
	std::wstring name = L"Local\\SubtitleFontHelperUiWx-";
	name += (config.m_windowKind == WindowKind::Fonts) ? L"Fonts" : L"Logs";
	return name;
}

std::wstring sfh::ui::BuildSingleInstanceWindowTitle(const LauncherConfig& config)
{
	return config.m_windowKind == WindowKind::Fonts
		? L"SubtitleFontHelper - Fonts"
		: L"SubtitleFontHelper - Logs";
}
