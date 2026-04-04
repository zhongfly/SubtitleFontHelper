#include "pch.h"

#include "TrayIcon.h"
#include "resource.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <wil/win32_helpers.h>
#include <wil/resource.h>

class sfh::SystemTray::Implementation
{
private:
	constexpr static UINT WM_TRAY_ICON_MESSAGE = WM_USER;
	constexpr static UINT WM_UPDATE_TRAY_ICON_MESSAGE = WM_USER + 1;
	constexpr static UINT_PTR TRAY_REFRESH_TIMER_ID = 1;
	static constexpr auto TRAY_REFRESH_INTERVAL_MS = 1000;
	static constexpr wchar_t TRAY_WINDOW_CLASS_NAME[] = L"AutoLoaderDaemonTray";
	static constexpr wchar_t TOOL_WINDOW_CLASS_NAME[] = L"AutoLoaderDaemonToolWindow";

	struct ToolWindowCreateParams
	{
		Implementation* m_owner = nullptr;
		const wchar_t* m_text = L"";
	};

	NOTIFYICONDATAW m_iconData = {};
	HWND m_hWnd = nullptr;
	HWND m_fontsWindow = nullptr;
	HWND m_logsWindow = nullptr;
	std::thread m_trayThread;

	IDaemon* m_daemon;
	std::atomic<size_t> m_checkPoint = 0;

	std::atomic<bool> m_startupLoading = true;
	std::atomic<size_t> m_managedIndexActiveCount = 0;
	std::atomic<size_t> m_managedIndexBuildCount = 0;
	std::atomic<size_t> m_managedIndexUpdateCount = 0;
	std::atomic<size_t> m_managedIndexProcessedFiles = 0;
	std::atomic<size_t> m_managedIndexTotalFiles = 0;
	std::atomic<bool> m_exitRequested = false;
	wil::unique_event m_startEvent;
public:
	Implementation(IDaemon* daemon)
		: m_daemon(daemon)
	{
		m_startEvent.create(wil::EventOptions::ManualReset);
		m_trayThread = std::thread([&]()
		{
			++m_checkPoint;
			if (WaitForSingleObject(m_startEvent.get(), INFINITE) != WAIT_OBJECT_0 || m_exitRequested.load())
				return;
			try
			{
				SetupMessageWindow();
				MessageLoop();
			}
			catch (...)
			{
				m_daemon->NotifyException(std::current_exception());
			}
		});
		while (m_checkPoint.load() == 0)
			std::this_thread::yield();
	}

	~Implementation()
	{
		m_exitRequested = true;
		m_startEvent.SetEvent();
		if (m_trayThread.joinable())
		{
			if (m_hWnd != nullptr)
				PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
			m_trayThread.join();
		}
	}

	void Start()
	{
		m_startEvent.SetEvent();
	}

	void SetManagedIndexTrayProgress(const ManagedIndexTrayProgressSnapshot& snapshot)
	{
		m_managedIndexActiveCount = snapshot.m_activeCount;
		m_managedIndexBuildCount = snapshot.m_buildCount;
		m_managedIndexUpdateCount = snapshot.m_updateCount;
		m_managedIndexProcessedFiles = snapshot.m_processedFiles;
		m_managedIndexTotalFiles = snapshot.m_totalFiles;
		if (m_hWnd != nullptr)
			PostMessageW(m_hWnd, WM_UPDATE_TRAY_ICON_MESSAGE, 0, 0);
	}

	void NotifyFinishLoad()
	{
		m_startupLoading = false;
		if (m_hWnd != nullptr)
			PostMessageW(m_hWnd, WM_UPDATE_TRAY_ICON_MESSAGE, 0, 0);
	}

private:
	bool IsLoading() const
	{
		return m_startupLoading.load() || m_managedIndexActiveCount.load() != 0;
	}

	std::wstring BuildLoadingTooltip() const
	{
		if (m_startupLoading.load())
		{
			return L"SubtitleFontAutoLoaderDaemon - 正在加载";
		}

		const auto activeCount = m_managedIndexActiveCount.load();
		if (activeCount == 0)
		{
			return L"SubtitleFontAutoLoaderDaemon";
		}

		const auto buildCount = m_managedIndexBuildCount.load();
		const auto updateCount = m_managedIndexUpdateCount.load();
		const auto processedFiles = m_managedIndexProcessedFiles.load();
		const auto totalFiles = m_managedIndexTotalFiles.load();

		std::wstring actionText;
		if (buildCount != 0 && updateCount != 0)
		{
			actionText = L"建立/更新";
		}
		else if (updateCount != 0)
		{
			actionText = L"更新";
		}
		else
		{
			actionText = L"建立";
		}

		std::wstring tooltip = L"SubtitleFontAutoLoaderDaemon - 正在" + actionText
			+ std::to_wstring(activeCount) + L"个索引";
		if (totalFiles != 0)
		{
			tooltip += L"：进度" + std::to_wstring((std::min)(processedFiles, totalFiles))
				+ L"/" + std::to_wstring(totalFiles);
		}
		return tooltip;
	}

	void SetupMessageWindow()
	{
		WNDCLASSW wndClass;
		RtlZeroMemory(&wndClass, sizeof(wndClass));
		wndClass.lpfnWndProc = WindowProc;
		wndClass.hInstance = wil::GetModuleInstanceHandle();
		wndClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
		wndClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wndClass.lpszMenuName = MAKEINTRESOURCEW(IDR_TRAYMENU);
		wndClass.lpszClassName = TRAY_WINDOW_CLASS_NAME;

		THROW_LAST_ERROR_IF(RegisterClassW(&wndClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS);

		WNDCLASSW toolWndClass;
		RtlZeroMemory(&toolWndClass, sizeof(toolWndClass));
		toolWndClass.lpfnWndProc = ToolWindowProc;
		toolWndClass.hInstance = wil::GetModuleInstanceHandle();
		toolWndClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
		toolWndClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		toolWndClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		toolWndClass.lpszClassName = TOOL_WINDOW_CLASS_NAME;

		THROW_LAST_ERROR_IF(RegisterClassW(&toolWndClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS);

		THROW_LAST_ERROR_IF(
			CreateWindowExW(
				0,
				wndClass.lpszClassName,
				L"AutoLoaderDaemonTray",
				WS_OVERLAPPEDWINDOW,
				CW_USEDEFAULT,
				CW_USEDEFAULT,
				CW_USEDEFAULT,
				CW_USEDEFAULT,
				nullptr,
				nullptr,
				wndClass.hInstance,
				this
			) == nullptr);
	}

	void SetupTrayIcon(bool add)
	{
		if (add)
		{
			RtlZeroMemory(&m_iconData, sizeof(m_iconData));
			m_iconData.cbSize = sizeof(m_iconData);
			m_iconData.hWnd = m_hWnd;
			m_iconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
			m_iconData.uCallbackMessage = WM_TRAY_ICON_MESSAGE;
		}
		if (IsLoading())
		{
			const auto tooltip = BuildLoadingTooltip();
			StringCchCopyW(m_iconData.szTip, std::size(m_iconData.szTip), tooltip.c_str());
			m_iconData.hIcon = LoadIconW(wil::GetModuleInstanceHandle(), MAKEINTRESOURCEW(IDI_TRAYICONLOADING));
		}
		else
		{
			wcscpy_s(m_iconData.szTip, L"SubtitleFontAutoLoaderDaemon");
			m_iconData.hIcon = LoadIconW(wil::GetModuleInstanceHandle(), MAKEINTRESOURCEW(IDI_TRAYICON));
		}
		Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &m_iconData);
	}

	void DestroyTrayIcon()
	{
		Shell_NotifyIconW(NIM_DELETE, &m_iconData);
	}

	static void MessageLoop()
	{
		MSG msg = {};
		while (GetMessage(&msg, nullptr, 0, 0))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	LRESULT CALLBACK MessageHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		const UINT WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
		switch (uMsg)
		{
		case WM_CREATE:
			SetTimer(hWnd, TRAY_REFRESH_TIMER_ID, TRAY_REFRESH_INTERVAL_MS, nullptr);
			SetupTrayIcon(true);
			break;
		case WM_TIMER:
			if (wParam == TRAY_REFRESH_TIMER_ID)
			{
				SetupTrayIcon(false);
				return 0;
			}
			break;
		case WM_ENDSESSION:
			m_daemon->NotifyExit();
			break;
		case WM_CLOSE:
			KillTimer(hWnd, TRAY_REFRESH_TIMER_ID);
			DestroyToolWindow(m_fontsWindow);
			DestroyToolWindow(m_logsWindow);
			DestroyTrayIcon();
			DestroyWindow(hWnd);
			break;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		case WM_TRAY_ICON_MESSAGE:
			if (lParam == WM_RBUTTONUP)
			{
				ShowContextMenu(hWnd);
			}
			return 0;
		case WM_UPDATE_TRAY_ICON_MESSAGE:
			SetupTrayIcon(false);
			return 0;
		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
			case ID_TRAYICONMENU_FONTS:
				ShowFontsWindow();
				break;
			case ID_TRAYICONMENU_LOGS:
				ShowLogsWindow();
				break;
			case ID_TRAYICONMENU_EXIT:
				m_daemon->NotifyExit();
				break;
			}
			return 0;
		default:
			if (uMsg == WM_TASKBARCREATED)
			{
				SetupTrayIcon(true);
			}
		}
		return DefWindowProcW(hWnd, uMsg, wParam, lParam);
	}

	void ShowFontsWindow()
	{
		ShowToolWindow(
			m_fontsWindow,
			L"Fonts",
			L"Fonts 窗口占位，后续任务中会接入索引概览和字体搜索。");
	}

	void ShowLogsWindow()
	{
		ShowToolWindow(
			m_logsWindow,
			L"Logs",
			L"Logs 窗口占位，后续任务中会接入内置日志查看器。");
	}

	void ShowToolWindow(HWND& handle, const wchar_t* title, const wchar_t* text)
	{
		if (handle != nullptr)
		{
			if (IsIconic(handle))
			{
				ShowWindow(handle, SW_RESTORE);
			}
			else
			{
				ShowWindow(handle, SW_SHOW);
			}
			SetForegroundWindow(handle);
			return;
		}

		ToolWindowCreateParams createParams{};
		createParams.m_owner = this;
		createParams.m_text = text;

		handle = CreateWindowExW(
			WS_EX_TOOLWINDOW,
			TOOL_WINDOW_CLASS_NAME,
			title,
			WS_OVERLAPPEDWINDOW | WS_VISIBLE,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			720,
			360,
			m_hWnd,
			nullptr,
			wil::GetModuleInstanceHandle(),
			&createParams);
		THROW_LAST_ERROR_IF(handle == nullptr);
		SetForegroundWindow(handle);
	}

	void DestroyToolWindow(HWND& handle)
	{
		if (handle == nullptr)
		{
			return;
		}

		HWND window = handle;
		handle = nullptr;
		DestroyWindow(window);
	}

	LRESULT HandleToolWindowMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		(void)wParam;
		switch (uMsg)
		{
		case WM_CREATE:
		{
			auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
			auto createParams = reinterpret_cast<const ToolWindowCreateParams*>(create->lpCreateParams);
			HWND label = CreateWindowExW(
				0,
				L"STATIC",
				createParams != nullptr ? createParams->m_text : L"",
				WS_CHILD | WS_VISIBLE | SS_LEFT,
				16,
				16,
				660,
				280,
				hWnd,
				nullptr,
				wil::GetModuleInstanceHandle(),
				nullptr);
			if (label != nullptr)
			{
				auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
				SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
			}
			return 0;
		}
		case WM_CLOSE:
			DestroyWindow(hWnd);
			return 0;
		case WM_NCDESTROY:
			if (hWnd == m_fontsWindow)
			{
				m_fontsWindow = nullptr;
			}
			else if (hWnd == m_logsWindow)
			{
				m_logsWindow = nullptr;
			}
			return DefWindowProcW(hWnd, uMsg, wParam, lParam);
		default:
			return DefWindowProcW(hWnd, uMsg, wParam, lParam);
		}
	}

	static void ShowContextMenu(HWND hWnd)
	{
		POINT cursorPos;

		GetCursorPos(&cursorPos);
		SetForegroundWindow(hWnd);
		HMENU hMenu = LoadMenuW(wil::GetModuleInstanceHandle(), MAKEINTRESOURCEW(IDR_TRAYMENU));
		HMENU hMenu1 = GetSubMenu(hMenu, 0);
		TrackPopupMenuEx(hMenu1, TPM_LEFTALIGN | TPM_RIGHTBUTTON, cursorPos.x, cursorPos.y, hWnd, nullptr);
		DestroyMenu(hMenu);
	}

	static Implementation* GetThisByWindow(HWND hWnd)
	{
		return reinterpret_cast<Implementation*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
	}

	static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		if (uMsg == WM_CREATE)
		{
			auto pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
			auto that = reinterpret_cast<Implementation*>(pCreate->lpCreateParams);
			that->m_hWnd = hWnd;
			SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(that));
		}
		if (auto that = GetThisByWindow(hWnd))
		{
			return that->MessageHandler(hWnd, uMsg, wParam, lParam);
		}

		return DefWindowProcW(hWnd, uMsg, wParam, lParam);
	}

	static LRESULT CALLBACK ToolWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		if (uMsg == WM_CREATE)
		{
			auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
			auto createParams = reinterpret_cast<const ToolWindowCreateParams*>(create->lpCreateParams);
			if (createParams != nullptr && createParams->m_owner != nullptr)
			{
				SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createParams->m_owner));
			}
		}

		if (auto that = GetThisByWindow(hWnd))
		{
			return that->HandleToolWindowMessage(hWnd, uMsg, wParam, lParam);
		}

		return DefWindowProcW(hWnd, uMsg, wParam, lParam);
	}
};

sfh::SystemTray::SystemTray(IDaemon* daemon)
	: m_impl(std::make_unique<Implementation>(daemon))
{
}

sfh::SystemTray::~SystemTray() = default;

void sfh::SystemTray::Start()
{
	m_impl->Start();
}

void sfh::SystemTray::SetManagedIndexTrayProgress(const ManagedIndexTrayProgressSnapshot& snapshot)
{
	m_impl->SetManagedIndexTrayProgress(snapshot);
}

void sfh::SystemTray::NotifyFinishLoad()
{
	m_impl->NotifyFinishLoad();
}
