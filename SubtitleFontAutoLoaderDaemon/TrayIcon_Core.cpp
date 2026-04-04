#include "pch.h"
#include "TrayIconImpl.h"

sfh::SystemTray::Implementation::Implementation(IDaemon* daemon, ITrayUiDataProvider* trayUiDataProvider)
	: m_daemon(daemon),
	  m_trayUiDataProvider(trayUiDataProvider)
{
	m_startEvent.create(wil::EventOptions::ManualReset);
	m_trayThread = std::thread([&]()
	{
		++m_checkPoint;
		if (WaitForSingleObject(m_startEvent.get(), INFINITE) != WAIT_OBJECT_0 || m_exitRequested.load())
			return;
		try
		{
			ResolveLogsPath();
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

sfh::SystemTray::Implementation::~Implementation()
{
	m_exitRequested = true;
	m_startEvent.SetEvent();
	if (m_trayThread.joinable())
	{
		if (m_hWnd != nullptr)
			PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
		m_trayThread.join();
	}
	if (m_toolWindowFont != nullptr)
	{
		DeleteObject(m_toolWindowFont);
	}
	if (m_toolWindowTitleFont != nullptr)
	{
		DeleteObject(m_toolWindowTitleFont);
	}
	if (m_toolWindowSectionFont != nullptr)
	{
		DeleteObject(m_toolWindowSectionFont);
	}
	if (m_windowBackgroundBrush != nullptr)
	{
		DeleteObject(m_windowBackgroundBrush);
	}
	if (m_panelBackgroundBrush != nullptr)
	{
		DeleteObject(m_panelBackgroundBrush);
	}
	if (m_metadataBackgroundBrush != nullptr)
	{
		DeleteObject(m_metadataBackgroundBrush);
	}
	if (m_logBackgroundBrush != nullptr)
	{
		DeleteObject(m_logBackgroundBrush);
	}
}

void sfh::SystemTray::Implementation::Start()
{
	m_startEvent.SetEvent();
}

void sfh::SystemTray::Implementation::SetManagedIndexTrayProgress(const ManagedIndexTrayProgressSnapshot& snapshot)
{
	m_managedIndexActiveCount = snapshot.m_activeCount;
	m_managedIndexBuildCount = snapshot.m_buildCount;
	m_managedIndexUpdateCount = snapshot.m_updateCount;
	m_managedIndexProcessedFiles = snapshot.m_processedFiles;
	m_managedIndexTotalFiles = snapshot.m_totalFiles;
	if (m_hWnd != nullptr)
		PostMessageW(m_hWnd, WM_UPDATE_TRAY_ICON_MESSAGE, 0, 0);
}

void sfh::SystemTray::Implementation::NotifyFinishLoad()
{
	m_startupLoading = false;
	if (m_hWnd != nullptr)
		PostMessageW(m_hWnd, WM_UPDATE_TRAY_ICON_MESSAGE, 0, 0);
}

void sfh::SystemTray::Implementation::NotifyFontUiDataChanged()
{
	if (m_hWnd != nullptr)
	{
		PostMessageW(m_hWnd, WM_FONT_UI_DATA_CHANGED, 0, 0);
	}
}

bool sfh::SystemTray::Implementation::IsLoading() const
{
	return m_startupLoading.load() || m_managedIndexActiveCount.load() != 0;
}

std::wstring sfh::SystemTray::Implementation::BuildLoadingTooltip() const
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

void sfh::SystemTray::Implementation::SetupMessageWindow()
{
	INITCOMMONCONTROLSEX commonControls{};
	commonControls.dwSize = sizeof(commonControls);
	commonControls.dwICC = ICC_LISTVIEW_CLASSES;
	InitCommonControlsEx(&commonControls);

	if (m_windowBackgroundBrush == nullptr)
	{
		m_windowBackgroundBrush = CreateSolidBrush(WINDOW_BACKGROUND_COLOR);
	}
	if (m_panelBackgroundBrush == nullptr)
	{
		m_panelBackgroundBrush = CreateSolidBrush(PANEL_BACKGROUND_COLOR);
	}
	if (m_metadataBackgroundBrush == nullptr)
	{
		m_metadataBackgroundBrush = CreateSolidBrush(METADATA_BACKGROUND_COLOR);
	}
	if (m_logBackgroundBrush == nullptr)
	{
		m_logBackgroundBrush = CreateSolidBrush(LOG_BACKGROUND_COLOR);
	}

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
	toolWndClass.hbrBackground = m_windowBackgroundBrush != nullptr
		? m_windowBackgroundBrush
		: reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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

void sfh::SystemTray::Implementation::SetupTrayIcon(bool add)
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

void sfh::SystemTray::Implementation::DestroyTrayIcon()
{
	Shell_NotifyIconW(NIM_DELETE, &m_iconData);
}

void sfh::SystemTray::Implementation::MessageLoop()
{
	MSG msg = {};
	while (GetMessage(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

LRESULT sfh::SystemTray::Implementation::MessageHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
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
	case WM_FONT_UI_DATA_CHANGED:
		RefreshFontsWindowContent();
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

void sfh::SystemTray::Implementation::ShowContextMenu(HWND hWnd)
{
	POINT cursorPos;

	GetCursorPos(&cursorPos);
	SetForegroundWindow(hWnd);
	HMENU hMenu = LoadMenuW(wil::GetModuleInstanceHandle(), MAKEINTRESOURCEW(IDR_TRAYMENU));
	HMENU hMenu1 = GetSubMenu(hMenu, 0);
	TrackPopupMenuEx(hMenu1, TPM_LEFTALIGN | TPM_RIGHTBUTTON, cursorPos.x, cursorPos.y, hWnd, nullptr);
	DestroyMenu(hMenu);
}

sfh::SystemTray::Implementation* sfh::SystemTray::Implementation::GetThisByWindow(HWND hWnd)
{
	return reinterpret_cast<Implementation*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
}

LRESULT CALLBACK sfh::SystemTray::Implementation::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
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

// pImpl bridge
sfh::SystemTray::SystemTray(IDaemon* daemon, ITrayUiDataProvider* trayUiDataProvider)
	: m_impl(std::make_unique<Implementation>(daemon, trayUiDataProvider))
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

void sfh::SystemTray::NotifyFontUiDataChanged()
{
	m_impl->NotifyFontUiDataChanged();
}
