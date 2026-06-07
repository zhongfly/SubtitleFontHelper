#include "pch.h"
#include "TrayIconImpl.h"
#include "Common.h"
#include "EventLog.h"

namespace
{
	void ShowStandaloneUiLaunchErrorMessage(const wchar_t* windowLabel, long errorCode)
	{
		const std::wstring message = std::wstring(L"\u65e0\u6cd5\u542f\u52a8\u72ec\u7acb") + windowLabel + L"\u7a97\u53e3\u3002";
		try
		{
			sfh::ErrorMessageBox(message, L"SubtitleFontAutoLoaderDaemon", errorCode);
		}
		catch (...)
		{
			MessageBoxW(nullptr, message.c_str(), L"SubtitleFontAutoLoaderDaemon", MB_OK | MB_ICONERROR);
		}
	}
}

void sfh::SystemTray::Implementation::ShowFontsWindow()
{
	TryLaunchStandaloneUiWindow(ToolWindowKind::Fonts, L"\u5b57\u4f53");
}

void sfh::SystemTray::Implementation::ShowLogsWindow()
{
	TryLaunchStandaloneUiWindow(ToolWindowKind::Logs, L"\u65e5\u5fd7");
}

void sfh::SystemTray::Implementation::TryLaunchStandaloneUiWindow(ToolWindowKind kind, const wchar_t* windowLabel)
{
	try
	{
		LaunchStandaloneUiWindow(kind);
	}
	catch (const wil::ResultException& e)
	{
		try
		{
			EventLog::GetInstance().LogDebugMessage(
				L"launch standalone ui failed: window=%ls hr=%lld",
				windowLabel,
				static_cast<long long>(e.GetErrorCode()));
		}
		catch (...)
		{
		}

		ShowStandaloneUiLaunchErrorMessage(windowLabel, static_cast<long>(e.GetErrorCode()));
	}
	catch (...)
	{
		const std::wstring message = std::wstring(L"\u65e0\u6cd5\u542f\u52a8\u72ec\u7acb") + windowLabel + L"\u7a97\u53e3\u3002";
		MessageBoxW(nullptr, message.c_str(), L"SubtitleFontAutoLoaderDaemon", MB_OK | MB_ICONERROR);
	}
}

std::filesystem::path sfh::SystemTray::Implementation::BuildStandaloneUiExecutablePath() const
{
	auto selfPath = wil::GetModuleFileNameW();
	std::filesystem::path exePath = selfPath.get();
	return exePath.parent_path() / L"SubtitleFontHelperUiWx.exe";
}

std::wstring sfh::SystemTray::Implementation::BuildRpcPipeName() const
{
	std::wstring pipeName = LR"_(\\.\pipe\SubtitleFontAutoLoaderRpc-)_";
	pipeName += sfh::GetCurrentProcessUserSid();
	return pipeName;
}

void sfh::SystemTray::Implementation::LaunchStandaloneUiWindow(ToolWindowKind kind)
{
	const auto executablePath = BuildStandaloneUiExecutablePath();
	THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !std::filesystem::exists(executablePath));

	std::wostringstream commandLine;
	commandLine << L'"' << executablePath.wstring() << L'"';
	if (kind == ToolWindowKind::Fonts)
	{
		commandLine << L" --window fonts --rpc-pipe \"" << BuildRpcPipeName() << L'"';
	}
	else
	{
		commandLine << L" --window logs --log-file \"" << EventLog::GetInstance().GetLogFilePath() << L'"';
	}

	auto mutableCommandLine = commandLine.str();
	mutableCommandLine.push_back(L'\0');

	STARTUPINFOW startupInfo{};
	startupInfo.cb = sizeof(startupInfo);
	startupInfo.dwFlags = STARTF_USESHOWWINDOW;
	startupInfo.wShowWindow = SW_SHOWNORMAL;

	wil::unique_process_information processInfo;
	THROW_LAST_ERROR_IF(CreateProcessW(
		executablePath.c_str(),
		mutableCommandLine.data(),
		nullptr,
		nullptr,
		FALSE,
		CREATE_UNICODE_ENVIRONMENT,
		nullptr,
		executablePath.parent_path().c_str(),
		&startupInfo,
		processInfo.addressof()) == FALSE);
}
