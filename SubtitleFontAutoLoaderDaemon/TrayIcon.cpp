#include "pch.h"

#include "TrayIcon.h"
#include "resource.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <CommCtrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <wil/win32_helpers.h>
#include <wil/resource.h>

#pragma comment(lib, "Comctl32.lib")

class sfh::SystemTray::Implementation
{
private:
	constexpr static UINT WM_TRAY_ICON_MESSAGE = WM_USER;
	constexpr static UINT WM_UPDATE_TRAY_ICON_MESSAGE = WM_USER + 1;
	constexpr static UINT WM_FONT_UI_DATA_CHANGED = WM_USER + 2;
	constexpr static UINT_PTR TRAY_REFRESH_TIMER_ID = 1;
	constexpr static UINT_PTR FONTS_SEARCH_DEBOUNCE_TIMER_ID = 2;
	constexpr static UINT_PTR LOGS_REFRESH_TIMER_ID = 3;
	static constexpr auto FONTS_SEARCH_DEBOUNCE_INTERVAL_MS = 200;
	static constexpr auto LOGS_REFRESH_INTERVAL_MS = 1000;
	static constexpr auto TRAY_REFRESH_INTERVAL_MS = 1000;
	static constexpr wchar_t TRAY_WINDOW_CLASS_NAME[] = L"AutoLoaderDaemonTray";
	static constexpr wchar_t TOOL_WINDOW_CLASS_NAME[] = L"AutoLoaderDaemonToolWindow";
	static constexpr int IDC_FONTS_SEARCH_EDIT = 1001;
	static constexpr int IDC_FONTS_INDEX_LIST = 1002;
	static constexpr int IDC_FONTS_RESULT_LIST = 1003;
	static constexpr int IDC_LOGS_STATUS_LABEL = 1004;
	static constexpr int IDC_LOGS_VIEW_EDIT = 1005;
	static constexpr int IDC_LOGS_SCROLL_BOTTOM_BUTTON = 1006;
	static constexpr size_t LOG_VIEW_MAX_BYTES = 1024 * 1024;
	static constexpr size_t LOG_VIEW_MAX_LINES = 5000;
	static constexpr wchar_t LOG_FILE_NAME[] = L"SubtitleFontHelper.log";
	static constexpr COLORREF WINDOW_BACKGROUND_COLOR = RGB(244, 240, 232);
	static constexpr COLORREF PANEL_BACKGROUND_COLOR = RGB(252, 249, 243);
	static constexpr COLORREF METADATA_BACKGROUND_COLOR = RGB(236, 231, 222);
	static constexpr COLORREF LOG_BACKGROUND_COLOR = RGB(248, 245, 239);
	static constexpr COLORREF INPUT_BACKGROUND_COLOR = RGB(255, 253, 248);
	static constexpr COLORREF PRIMARY_TEXT_COLOR = RGB(50, 44, 36);
	static constexpr COLORREF SECONDARY_TEXT_COLOR = RGB(102, 92, 78);
	static constexpr COLORREF ACCENT_TEXT_COLOR = RGB(123, 87, 43);
	static constexpr COLORREF LIST_ALT_BACKGROUND_COLOR = RGB(248, 244, 237);
	static constexpr COLORREF LIST_SELECTED_BACKGROUND_COLOR = RGB(214, 224, 232);
	static constexpr COLORREF LIST_SELECTED_TEXT_COLOR = RGB(35, 31, 26);

	enum class ToolWindowKind
	{
		Fonts = 0,
		Logs
	};

	struct ToolWindowCreateParams
	{
		Implementation* m_owner = nullptr;
		ToolWindowKind m_kind = ToolWindowKind::Fonts;
		const wchar_t* m_text = L"";
	};

	NOTIFYICONDATAW m_iconData = {};
	HWND m_hWnd = nullptr;
	HWND m_fontsWindow = nullptr;
	HWND m_logsWindow = nullptr;
	HWND m_fontsTitleLabel = nullptr;
	HWND m_fontsStatusLabel = nullptr;
	HWND m_fontsIndexesSectionLabel = nullptr;
	HWND m_fontsSearchSectionLabel = nullptr;
	HWND m_fontsSearchEdit = nullptr;
	HWND m_fontsSearchSummaryLabel = nullptr;
	HWND m_fontsIndexListView = nullptr;
	HWND m_fontsResultListView = nullptr;
	HWND m_logsTitleLabel = nullptr;
	HWND m_logsSubtitleLabel = nullptr;
	HWND m_logsStatusLabel = nullptr;
	HWND m_logsContentSectionLabel = nullptr;
	HWND m_logsScrollBottomButton = nullptr;
	HWND m_logsEdit = nullptr;
	HFONT m_toolWindowFont = nullptr;
	HFONT m_toolWindowTitleFont = nullptr;
	HFONT m_toolWindowSectionFont = nullptr;
	HBRUSH m_windowBackgroundBrush = nullptr;
	HBRUSH m_panelBackgroundBrush = nullptr;
	HBRUSH m_metadataBackgroundBrush = nullptr;
	HBRUSH m_logBackgroundBrush = nullptr;
	std::thread m_trayThread;

	IDaemon* m_daemon;
	ITrayUiDataProvider* m_trayUiDataProvider;
	std::atomic<size_t> m_checkPoint = 0;

	std::atomic<bool> m_startupLoading = true;
	std::atomic<size_t> m_managedIndexActiveCount = 0;
	std::atomic<size_t> m_managedIndexBuildCount = 0;
	std::atomic<size_t> m_managedIndexUpdateCount = 0;
	std::atomic<size_t> m_managedIndexProcessedFiles = 0;
	std::atomic<size_t> m_managedIndexTotalFiles = 0;
	std::atomic<bool> m_exitRequested = false;
	wil::unique_event m_startEvent;
	std::wstring m_logsPath;
	std::wstring m_logsStatusText;
	std::wstring m_logsLastLoadedText;
	std::vector<std::wstring> m_fontsIndexTooltips;
	std::vector<std::wstring> m_fontsResultTooltips;
	std::vector<FontSearchResult> m_fontsCurrentResults;
	ULONGLONG m_logsLastFileSize = 0;
	FILETIME m_logsLastWriteTime = {};
	bool m_logsHasObservedFile = false;
	bool m_logsLastReadFailed = false;
public:
	Implementation(IDaemon* daemon, ITrayUiDataProvider* trayUiDataProvider)
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

	void NotifyFontUiDataChanged()
	{
		if (m_hWnd != nullptr)
		{
			PostMessageW(m_hWnd, WM_FONT_UI_DATA_CHANGED, 0, 0);
		}
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

	void ShowFontsWindow()
	{
		ShowToolWindow(
			m_fontsWindow,
			ToolWindowKind::Fonts,
			L"字体浏览",
			L"");
	}

	void ShowLogsWindow()
	{
		ShowToolWindow(
			m_logsWindow,
			ToolWindowKind::Logs,
			L"日志查看",
			L"");
	}

	void ShowToolWindow(HWND& handle, ToolWindowKind kind, const wchar_t* title, const wchar_t* text)
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
		createParams.m_kind = kind;
		createParams.m_text = text;
		const int initialWidth = kind == ToolWindowKind::Fonts ? 980 : 920;
		const int initialHeight = kind == ToolWindowKind::Fonts ? 820 : 720;

		handle = CreateWindowExW(
			WS_EX_APPWINDOW,
			TOOL_WINDOW_CLASS_NAME,
			title,
			WS_OVERLAPPEDWINDOW | WS_VISIBLE,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			initialWidth,
			initialHeight,
			nullptr,
			nullptr,
			wil::GetModuleInstanceHandle(),
			&createParams);
		THROW_LAST_ERROR_IF(handle == nullptr);
		SetWindowPos(handle, nullptr, 0, 0, initialWidth, initialHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
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

	static void ConfigureListViewColumn(HWND listView, int index, int width, const wchar_t* text)
	{
		LVCOLUMNW column{};
		column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
		column.pszText = const_cast<wchar_t*>(text);
		column.cx = width;
		column.iSubItem = index;
		ListView_InsertColumn(listView, index, &column);
	}

	static LONG ScaleFontHeight(LONG originalHeight, int numerator, int denominator)
	{
		if (originalHeight < 0)
		{
			return -MulDiv(-originalHeight, numerator, denominator);
		}
		return MulDiv((std::max)(originalHeight, 1L), numerator, denominator);
	}

	static bool TryCreateMatchedFont(const LOGFONTW& baseFont, const wchar_t* faceName, HFONT& font)
	{
		LOGFONTW candidateFont = baseFont;
		candidateFont.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
		StringCchCopyW(candidateFont.lfFaceName, std::size(candidateFont.lfFaceName), faceName);

		HFONT candidate = CreateFontIndirectW(&candidateFont);
		if (candidate == nullptr)
		{
			return false;
		}

		HDC screenDc = GetDC(nullptr);
		if (screenDc == nullptr)
		{
			DeleteObject(candidate);
			return false;
		}

		HGDIOBJ oldObject = SelectObject(screenDc, candidate);
		wchar_t actualFaceName[LF_FACESIZE]{};
		const int actualLength = GetTextFaceW(screenDc, static_cast<int>(std::size(actualFaceName)), actualFaceName);
		if (oldObject != nullptr)
		{
			SelectObject(screenDc, oldObject);
		}
		ReleaseDC(nullptr, screenDc);

		if (actualLength <= 0 || _wcsicmp(actualFaceName, faceName) != 0)
		{
			DeleteObject(candidate);
			return false;
		}

		font = candidate;
		return true;
	}

	HFONT GetToolWindowFont()
	{
		if (m_toolWindowFont != nullptr)
		{
			return m_toolWindowFont;
		}

		NONCLIENTMETRICSW metrics{};
		metrics.cbSize = sizeof(metrics);
		LOGFONTW fontSpec{};
		if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE)
		{
			fontSpec = metrics.lfMessageFont;
		}
		else
		{
			SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(fontSpec), &fontSpec, 0);
		}

		fontSpec.lfHeight = ScaleFontHeight(fontSpec.lfHeight, 6, 5);
		fontSpec.lfWidth = 0;
		fontSpec.lfItalic = FALSE;
		fontSpec.lfUnderline = FALSE;
		fontSpec.lfStrikeOut = FALSE;
		fontSpec.lfWeight = FW_NORMAL;
		fontSpec.lfQuality = CLEARTYPE_QUALITY;
		fontSpec.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;

		static const wchar_t* preferredFamilies[] =
		{
			L"Sarasa Mono SC",
			L"Sarasa Mono HC",
			L"Sarasa Mono J",
			L"Sarasa Mono K",
			L"Sarasa Mono TC",
			L"Cascadia Mono",
			L"Cascadia Code",
			L"NSimSun",
			L"MS Gothic",
			L"MingLiU",
			L"GulimChe",
			L"Consolas",
			L"Lucida Console",
			L"Courier New"
		};

		for (const auto* family : preferredFamilies)
		{
			HFONT candidate = nullptr;
			if (TryCreateMatchedFont(fontSpec, family, candidate))
			{
				m_toolWindowFont = candidate;
				return m_toolWindowFont;
			}
		}

		fontSpec.lfFaceName[0] = L'\0';
		m_toolWindowFont = CreateFontIndirectW(&fontSpec);
		if (m_toolWindowFont != nullptr)
		{
			return m_toolWindowFont;
		}

		return reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	}

	HFONT GetToolWindowTitleFont()
	{
		if (m_toolWindowTitleFont != nullptr)
		{
			return m_toolWindowTitleFont;
		}

		LOGFONTW titleFont{};
		auto baseFont = GetToolWindowFont();
		if (baseFont != nullptr && GetObjectW(baseFont, sizeof(titleFont), &titleFont) == sizeof(titleFont))
		{
			titleFont.lfHeight = ScaleFontHeight(titleFont.lfHeight, 4, 3);
			titleFont.lfWeight = FW_BOLD;
			titleFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
			StringCchCopyW(titleFont.lfFaceName, std::size(titleFont.lfFaceName), L"Segoe UI");
			m_toolWindowTitleFont = CreateFontIndirectW(&titleFont);
		}

		return m_toolWindowTitleFont != nullptr
			? m_toolWindowTitleFont
			: reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	}

	HFONT GetToolWindowSectionFont()
	{
		if (m_toolWindowSectionFont != nullptr)
		{
			return m_toolWindowSectionFont;
		}

		LOGFONTW sectionFont{};
		auto baseFont = GetToolWindowFont();
		if (baseFont != nullptr && GetObjectW(baseFont, sizeof(sectionFont), &sectionFont) == sizeof(sectionFont))
		{
			sectionFont.lfWeight = FW_BOLD;
			sectionFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
			StringCchCopyW(sectionFont.lfFaceName, std::size(sectionFont.lfFaceName), L"Segoe UI");
			m_toolWindowSectionFont = CreateFontIndirectW(&sectionFont);
		}

		return m_toolWindowSectionFont != nullptr
			? m_toolWindowSectionFont
			: reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	}

	void ApplyToolWindowFont(HWND control)
	{
		if (control == nullptr)
		{
			return;
		}

		auto font = GetToolWindowFont();
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}

	void ApplyToolWindowTitleFont(HWND control)
	{
		if (control == nullptr)
		{
			return;
		}

		auto font = GetToolWindowTitleFont();
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}

	void ApplyToolWindowSectionFont(HWND control)
	{
		if (control == nullptr)
		{
			return;
		}

		auto font = GetToolWindowSectionFont();
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}

	void ConfigureListViewColors(HWND listView, COLORREF backgroundColor)
	{
		if (listView == nullptr)
		{
			return;
		}

		ListView_SetBkColor(listView, backgroundColor);
		ListView_SetTextBkColor(listView, backgroundColor);
		ListView_SetTextColor(listView, PRIMARY_TEXT_COLOR);
		ListView_SetExtendedListViewStyle(
			listView,
			ListView_GetExtendedListViewStyle(listView) | LVS_EX_INFOTIP);
	}

	static std::wstring BuildFontsIndexTooltip(const FontIndexSummary& summary)
	{
		return L"索引： " + summary.m_indexPath
			+ L"\r\n文件数： " + std::to_wstring(summary.m_fontFileCount)
			+ L"\r\n名称数： " + std::to_wstring(summary.m_fontNameCount);
	}

	static std::wstring BuildFontsResultTooltip(const FontSearchResult& result)
	{
		return L"显示名： " + result.m_displayName
			+ L"\r\n族名： " + result.m_familyNames
			+ L"\r\n完整名称： " + result.m_fullNames
			+ L"\r\nPostScript： " + result.m_postScriptNames
			+ L"\r\n索引： " + result.m_indexPath
			+ L"\r\n路径： " + result.m_fontPath
			+ L"\r\n字体序号： " + std::to_wstring(result.m_faceIndex);
	}

	HWND CreateFontsListView(HWND parent, int controlId, int x, int y, int width, int height)
	{
		HWND listView = CreateWindowExW(
			WS_EX_CLIENTEDGE,
			WC_LISTVIEWW,
			L"",
			WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
			x,
			y,
			width,
			height,
			parent,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
			wil::GetModuleInstanceHandle(),
			nullptr);
		if (listView != nullptr)
		{
			ListView_SetExtendedListViewStyle(
				listView,
				LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
			ApplyToolWindowFont(listView);
			ConfigureListViewColors(listView, PANEL_BACKGROUND_COLOR);
			auto header = ListView_GetHeader(listView);
			ApplyToolWindowFont(header);
		}
		return listView;
	}

	void SetupFontsWindowControls(HWND hWnd)
	{
		m_fontsTitleLabel = CreateWindowExW(
			0,
			L"STATIC",
			L"字体浏览",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			16,
			16,
			700,
			28,
			hWnd,
			nullptr,
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_fontsStatusLabel = CreateWindowExW(
			0,
			L"STATIC",
			L"",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			16,
			48,
			700,
			20,
			hWnd,
			nullptr,
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_fontsIndexesSectionLabel = CreateWindowExW(
			0,
			L"STATIC",
			L"已加载索引",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			16,
			80,
			700,
			20,
			hWnd,
			nullptr,
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_fontsIndexListView = CreateFontsListView(hWnd, IDC_FONTS_INDEX_LIST, 16, 108, 700, 130);
		m_fontsSearchSectionLabel = CreateWindowExW(
			0,
			L"STATIC",
			L"搜索字体",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			16,
			250,
			700,
			20,
			hWnd,
			nullptr,
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_fontsSearchEdit = CreateWindowExW(
			WS_EX_CLIENTEDGE,
			L"EDIT",
			L"",
			WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
			16,
			278,
			700,
			30,
			hWnd,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONTS_SEARCH_EDIT)),
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_fontsSearchSummaryLabel = CreateWindowExW(
			0,
			L"STATIC",
			L"输入字体名称进行搜索。",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			16,
			316,
			700,
			20,
			hWnd,
			nullptr,
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_fontsResultListView = CreateFontsListView(hWnd, IDC_FONTS_RESULT_LIST, 16, 344, 700, 190);

		if (m_fontsTitleLabel != nullptr)
		{
			ApplyToolWindowTitleFont(m_fontsTitleLabel);
		}
		if (m_fontsStatusLabel != nullptr)
		{
			ApplyToolWindowFont(m_fontsStatusLabel);
		}
		if (m_fontsIndexesSectionLabel != nullptr)
		{
			ApplyToolWindowSectionFont(m_fontsIndexesSectionLabel);
		}
		if (m_fontsSearchSectionLabel != nullptr)
		{
			ApplyToolWindowSectionFont(m_fontsSearchSectionLabel);
		}
		if (m_fontsSearchEdit != nullptr)
		{
			ApplyToolWindowFont(m_fontsSearchEdit);
			SendMessageW(m_fontsSearchEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
			SendMessageW(m_fontsSearchEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"搜索族名、完整名称或 PostScript 名称"));
		}
		if (m_fontsSearchSummaryLabel != nullptr)
		{
			ApplyToolWindowFont(m_fontsSearchSummaryLabel);
		}

		if (m_fontsIndexListView != nullptr)
		{
			ConfigureListViewColumn(m_fontsIndexListView, 0, 260, L"索引");
			ConfigureListViewColumn(m_fontsIndexListView, 1, 90, L"文件数");
			ConfigureListViewColumn(m_fontsIndexListView, 2, 90, L"名称数");
		}
		if (m_fontsResultListView != nullptr)
		{
			ConfigureListViewColumn(m_fontsResultListView, 0, 180, L"显示名");
			ConfigureListViewColumn(m_fontsResultListView, 1, 180, L"族名");
			ConfigureListViewColumn(m_fontsResultListView, 2, 180, L"完整名称");
			ConfigureListViewColumn(m_fontsResultListView, 3, 180, L"PostScript");
			ConfigureListViewColumn(m_fontsResultListView, 4, 80, L"序号");
			ConfigureListViewColumn(m_fontsResultListView, 5, 220, L"索引");
			ConfigureListViewColumn(m_fontsResultListView, 6, 260, L"路径");
		}
	}

	void SetupLogsWindowControls(HWND hWnd)
	{
		m_logsTitleLabel = CreateWindowExW(
			0,
			L"STATIC",
			L"日志查看",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			16,
			16,
			700,
			28,
			hWnd,
			nullptr,
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_logsSubtitleLabel = CreateWindowExW(
			0,
			L"STATIC",
			L"当前主日志文件的实时查看器，仅显示最新片段。",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			16,
			48,
			700,
			20,
			hWnd,
			nullptr,
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_logsContentSectionLabel = CreateWindowExW(
			0,
			L"STATIC",
			L"日志内容",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			16,
			164,
			700,
			20,
			hWnd,
			nullptr,
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_logsScrollBottomButton = CreateWindowExW(
			0,
			L"BUTTON",
			L"滚动到底部",
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			16,
			164,
			140,
			28,
			hWnd,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOGS_SCROLL_BOTTOM_BUTTON)),
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_logsStatusLabel = CreateWindowExW(
			0,
			L"EDIT",
			L"",
			WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
			16,
			78,
			700,
			48,
			hWnd,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOGS_STATUS_LABEL)),
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_logsEdit = CreateWindowExW(
			WS_EX_CLIENTEDGE,
			L"EDIT",
			L"",
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
			16,
			192,
			700,
			320,
			hWnd,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOGS_VIEW_EDIT)),
			wil::GetModuleInstanceHandle(),
			nullptr);
		if (m_logsTitleLabel != nullptr)
		{
			ApplyToolWindowTitleFont(m_logsTitleLabel);
		}
		if (m_logsSubtitleLabel != nullptr)
		{
			ApplyToolWindowFont(m_logsSubtitleLabel);
		}
		if (m_logsContentSectionLabel != nullptr)
		{
			ApplyToolWindowSectionFont(m_logsContentSectionLabel);
		}
		if (m_logsScrollBottomButton != nullptr)
		{
			ApplyToolWindowFont(m_logsScrollBottomButton);
		}
		if (m_logsStatusLabel != nullptr)
		{
			ApplyToolWindowFont(m_logsStatusLabel);
			SendMessageW(m_logsStatusLabel, EM_LIMITTEXT, 0x7FFFFFFE, 0);
			SendMessageW(m_logsStatusLabel, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
		}
		if (m_logsEdit != nullptr)
		{
			ApplyToolWindowFont(m_logsEdit);
			SendMessageW(m_logsEdit, EM_LIMITTEXT, 0x7FFFFFFE, 0);
			SendMessageW(m_logsEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
		}
	}

	int CalculateLogsStatusHeight(int availableWidth) const
	{
		if (m_logsStatusLabel == nullptr)
		{
			return 30;
		}

		const int textWidth = (std::max)(120, availableWidth - 20);
		const auto font = reinterpret_cast<HFONT>(SendMessageW(m_logsStatusLabel, WM_GETFONT, 0, 0));
		HDC dc = GetDC(m_logsStatusLabel);
		if (dc == nullptr)
		{
			return 30;
		}

		HGDIOBJ oldObject = nullptr;
		if (font != nullptr)
		{
			oldObject = SelectObject(dc, font);
		}

		RECT measureRect{ 0, 0, textWidth, 0 };
		const auto* text = m_logsStatusText.empty() ? L"日志文件：SubtitleFontHelper.log | 更新时间：0000-00-00 00:00:00" : m_logsStatusText.c_str();
		DrawTextW(dc, text, -1, &measureRect, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_CALCRECT | DT_NOPREFIX);
		TEXTMETRICW metrics{};
		GetTextMetricsW(dc, &metrics);

		if (oldObject != nullptr)
		{
			SelectObject(dc, oldObject);
		}
		ReleaseDC(m_logsStatusLabel, dc);

		const int singleLineHeight = metrics.tmHeight + metrics.tmExternalLeading;
		const int measuredHeight = static_cast<int>(measureRect.bottom - measureRect.top);
		return (std::max)(singleLineHeight + 8, measuredHeight + 8);
	}

	void LayoutFontsWindowControls(int clientWidth, int clientHeight)
	{
		if (m_fontsTitleLabel == nullptr
			|| m_fontsStatusLabel == nullptr
			|| m_fontsIndexesSectionLabel == nullptr
			|| m_fontsSearchSectionLabel == nullptr
			|| m_fontsSearchEdit == nullptr
			|| m_fontsSearchSummaryLabel == nullptr
			|| m_fontsIndexListView == nullptr
			|| m_fontsResultListView == nullptr)
		{
			return;
		}

		const int left = 16;
		const int right = 16;
		const int top = 16;
		const int availableWidth = (std::max)(320, clientWidth - left - right);
		const int titleHeight = 28;
		const int statusHeight = 20;
		const int sectionHeight = 20;
		const int indexTop = top + titleHeight + 12 + statusHeight + 16 + sectionHeight + 8;
		const int indexHeight = (std::min)(148, (std::max)(118, clientHeight / 4));
		const int searchSectionTop = indexTop + indexHeight + 18;
		const int searchEditTop = searchSectionTop + sectionHeight + 8;
		const int resultTop = searchEditTop + 66;
		const int resultHeight = (std::max)(180, clientHeight - resultTop - 16);

		MoveWindow(m_fontsTitleLabel, left, top, availableWidth, titleHeight, TRUE);
		MoveWindow(m_fontsStatusLabel, left, top + titleHeight + 12, availableWidth, statusHeight, TRUE);
		MoveWindow(m_fontsIndexesSectionLabel, left, top + titleHeight + 12 + statusHeight + 16, availableWidth, sectionHeight, TRUE);
		MoveWindow(m_fontsIndexListView, left, indexTop, availableWidth, indexHeight, TRUE);
		MoveWindow(m_fontsSearchSectionLabel, left, searchSectionTop, availableWidth, sectionHeight, TRUE);
		MoveWindow(m_fontsSearchEdit, left, searchEditTop, availableWidth, 30, TRUE);
		MoveWindow(m_fontsSearchSummaryLabel, left, searchEditTop + 38, availableWidth, 20, TRUE);
		MoveWindow(m_fontsResultListView, left, resultTop, availableWidth, resultHeight, TRUE);

		ListView_SetColumnWidth(m_fontsIndexListView, 0, (std::max)(180, availableWidth * 35 / 100));
		ListView_SetColumnWidth(m_fontsIndexListView, 1, (std::max)(90, availableWidth * 15 / 100));
		ListView_SetColumnWidth(m_fontsIndexListView, 2, (std::max)(90, availableWidth * 15 / 100));

		ListView_SetColumnWidth(m_fontsResultListView, 0, 120);
		ListView_SetColumnWidth(m_fontsResultListView, 1, 110);
		ListView_SetColumnWidth(m_fontsResultListView, 2, 110);
		ListView_SetColumnWidth(m_fontsResultListView, 3, 110);
		ListView_SetColumnWidth(m_fontsResultListView, 4, 55);
		ListView_SetColumnWidth(m_fontsResultListView, 5, (std::max)(120, availableWidth * 22 / 100));
		ListView_SetColumnWidth(m_fontsResultListView, 6, (std::max)(160, availableWidth * 28 / 100));
	}

	void LayoutLogsWindowControls(int clientWidth, int clientHeight)
	{
		if (m_logsTitleLabel == nullptr
			|| m_logsSubtitleLabel == nullptr
			|| m_logsStatusLabel == nullptr
			|| m_logsContentSectionLabel == nullptr
			|| m_logsScrollBottomButton == nullptr
			|| m_logsEdit == nullptr)
		{
			return;
		}

		const int left = 16;
		const int right = 16;
		const int top = 16;
		const int availableWidth = (std::max)(320, clientWidth - left - right);
		const int titleHeight = 28;
		const int subtitleHeight = 20;
		const int statusHeight = CalculateLogsStatusHeight(availableWidth);
		const bool inlineButton = availableWidth >= 500;
		const int buttonWidth = 120;
		const int buttonHeight = 28;
		const int contentSectionTop = top + titleHeight + 10 + subtitleHeight + 14 + statusHeight + 18;
		const int labelWidth = inlineButton
			? (std::max)(120, availableWidth - buttonWidth - 16)
			: availableWidth;
		const int buttonLeft = inlineButton
			? (std::max)(left, left + availableWidth - buttonWidth)
			: left;
		const int buttonTop = inlineButton ? contentSectionTop - 4 : contentSectionTop + 24;
		const int logTop = inlineButton ? contentSectionTop + 32 : buttonTop + buttonHeight + 12;
		const int logHeight = (std::max)(120, clientHeight - logTop - 16);

		MoveWindow(m_logsTitleLabel, left, top, availableWidth, titleHeight, TRUE);
		MoveWindow(m_logsSubtitleLabel, left, top + titleHeight + 10, availableWidth, subtitleHeight, TRUE);
		MoveWindow(m_logsStatusLabel, left, top + titleHeight + 10 + subtitleHeight + 14, availableWidth, statusHeight, TRUE);
		MoveWindow(m_logsContentSectionLabel, left, contentSectionTop, labelWidth, 20, TRUE);
		MoveWindow(m_logsScrollBottomButton, buttonLeft, buttonTop, buttonWidth, buttonHeight, TRUE);
		MoveWindow(m_logsEdit, left, logTop, availableWidth, logHeight, TRUE);
	}

	static void SetListViewRowText(HWND listView, int rowIndex, int columnIndex, const std::wstring& text)
	{
		ListView_SetItemText(listView, rowIndex, columnIndex, const_cast<wchar_t*>(text.c_str()));
	}

	void ScrollLogsEditToBottom()
	{
		if (m_logsEdit == nullptr)
		{
			return;
		}

		const auto length = GetWindowTextLengthW(m_logsEdit);
		SendMessageW(m_logsEdit, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
		SendMessageW(m_logsEdit, EM_SCROLLCARET, 0, 0);
	}

	void CopyUnicodeTextToClipboard(HWND owner, const std::wstring& text)
	{
		if (owner == nullptr || text.empty())
		{
			return;
		}

		if (OpenClipboard(owner) == FALSE)
		{
			return;
		}

		struct ClipboardCloser
		{
			~ClipboardCloser() { CloseClipboard(); }
		} closer;

		if (EmptyClipboard() == FALSE)
		{
			return;
		}

		const auto bytes = (text.size() + 1) * sizeof(wchar_t);
		HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (memory == nullptr)
		{
			return;
		}

		void* locked = GlobalLock(memory);
		if (locked == nullptr)
		{
			GlobalFree(memory);
			return;
		}

		memcpy(locked, text.c_str(), bytes);
		GlobalUnlock(memory);

		if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
		{
			GlobalFree(memory);
		}
	}

	static std::wstring Utf8ToWideBestEffort(std::string_view utf8)
	{
		for (size_t offset = 0; offset < (std::min)(utf8.size(), static_cast<size_t>(4)); ++offset)
		{
			const auto length = static_cast<int>(utf8.size() - offset);
			if (length <= 0)
			{
				break;
			}

			const auto* start = utf8.data() + offset;
			const int wideLength = MultiByteToWideChar(
				CP_UTF8,
				MB_ERR_INVALID_CHARS,
				start,
				length,
				nullptr,
				0);
			if (wideLength <= 0)
			{
				continue;
			}

			std::wstring wide(static_cast<size_t>(wideLength), L'\0');
			if (MultiByteToWideChar(
				CP_UTF8,
				MB_ERR_INVALID_CHARS,
				start,
				length,
				wide.data(),
				wideLength) > 0)
			{
				return wide;
			}
		}

		return L"";
	}

	static std::wstring FormatFileTimeText(const FILETIME& fileTime)
	{
		FILETIME localFileTime{};
		SYSTEMTIME localSystemTime{};
		if (FileTimeToLocalFileTime(&fileTime, &localFileTime) == FALSE
			|| FileTimeToSystemTime(&localFileTime, &localSystemTime) == FALSE)
		{
			return L"未知";
		}

		wchar_t buffer[64]{};
		StringCchPrintfW(
			buffer,
			std::size(buffer),
			L"%04u-%02u-%02u %02u:%02u:%02u",
			localSystemTime.wYear,
			localSystemTime.wMonth,
			localSystemTime.wDay,
			localSystemTime.wHour,
			localSystemTime.wMinute,
			localSystemTime.wSecond);
		return buffer;
	}

	void ResolveLogsPath()
	{
		const std::filesystem::path modulePath{wil::GetModuleFileNameW<wil::unique_hlocal_string>().get()};
		m_logsPath = (modulePath.parent_path() / LOG_FILE_NAME).wstring();
	}

	std::wstring GetLogsDisplayName() const
	{
		if (m_logsPath.empty())
		{
			return LOG_FILE_NAME;
		}

		const auto filename = std::filesystem::path(m_logsPath).filename().wstring();
		if (!filename.empty())
		{
			return filename;
		}

		return m_logsPath;
	}

	bool TryGetLogFileMetadata(ULONGLONG& fileSize, FILETIME& lastWriteTime, bool& exists) const
	{
		WIN32_FILE_ATTRIBUTE_DATA attributes{};
		if (GetFileAttributesExW(m_logsPath.c_str(), GetFileExInfoStandard, &attributes) == FALSE)
		{
			const auto error = GetLastError();
			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			{
				exists = false;
				fileSize = 0;
				lastWriteTime = {};
				return true;
			}
			return false;
		}

		exists = (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
		if (!exists)
		{
			fileSize = 0;
			lastWriteTime = {};
			return true;
		}

		ULARGE_INTEGER size{};
		size.LowPart = attributes.nFileSizeLow;
		size.HighPart = attributes.nFileSizeHigh;
		fileSize = size.QuadPart;
		lastWriteTime = attributes.ftLastWriteTime;
		return true;
	}

	std::wstring BuildLogsFallbackText(const std::wstring& message) const
	{
		return message + L"\r\n\r\n日志文件：\r\n" + GetLogsDisplayName();
	}

	static bool IsAsciiDigit(wchar_t ch)
	{
		return ch >= L'0' && ch <= L'9';
	}

	static bool IsLogEntryStartLine(std::wstring_view line)
	{
		return line.size() >= 23
			&& IsAsciiDigit(line[0])
			&& IsAsciiDigit(line[1])
			&& IsAsciiDigit(line[2])
			&& IsAsciiDigit(line[3])
			&& line[4] == L'-'
			&& IsAsciiDigit(line[5])
			&& IsAsciiDigit(line[6])
			&& line[7] == L'-'
			&& IsAsciiDigit(line[8])
			&& IsAsciiDigit(line[9])
			&& line[10] == L' '
			&& IsAsciiDigit(line[11])
			&& IsAsciiDigit(line[12])
			&& line[13] == L':'
			&& IsAsciiDigit(line[14])
			&& IsAsciiDigit(line[15])
			&& line[16] == L':'
			&& IsAsciiDigit(line[17])
			&& IsAsciiDigit(line[18])
			&& line[19] == L'.'
			&& IsAsciiDigit(line[20])
			&& IsAsciiDigit(line[21])
			&& IsAsciiDigit(line[22]);
	}

	std::wstring FormatLogsContentForViewer(std::wstring text) const
	{
		if (text.empty())
		{
			return text;
		}

		text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());

		std::wstring formatted;
		formatted.reserve(text.size() + text.size() / 8);

		size_t start = 0;
		bool firstVisibleLine = true;
		bool previousLineBlank = true;
		while (start <= text.size())
		{
			const size_t end = text.find(L'\n', start);
			const size_t length = end == std::wstring::npos ? text.size() - start : end - start;
			std::wstring_view line(text.data() + start, length);
			const bool isBlankLine = line.empty();
			const bool isNewEntry = !isBlankLine && IsLogEntryStartLine(line);

			if (!formatted.empty())
			{
				formatted += L"\r\n";
			}
			if (isNewEntry && !firstVisibleLine && !previousLineBlank)
			{
				formatted += L"\r\n";
			}

			formatted.append(line);

			if (!isBlankLine)
			{
				firstVisibleLine = false;
			}
			previousLineBlank = isBlankLine;

			if (end == std::wstring::npos)
			{
				break;
			}
			start = end + 1;
		}

		return formatted;
	}

	std::wstring TrimLogsToLastLines(std::wstring text, bool& truncatedByLines) const
	{
		size_t lineCount = 0;
		for (wchar_t ch : text)
		{
			if (ch == L'\n')
			{
				++lineCount;
			}
		}

		if (!text.empty() && text.back() != L'\n')
		{
			++lineCount;
		}

		if (lineCount <= LOG_VIEW_MAX_LINES)
		{
			truncatedByLines = false;
			return text;
		}

		size_t newlineSeen = 0;
		size_t startIndex = 0;
		for (size_t i = text.size(); i > 0; --i)
		{
			if (text[i - 1] == L'\n')
			{
				++newlineSeen;
				if (newlineSeen >= LOG_VIEW_MAX_LINES)
				{
					startIndex = i;
					break;
				}
			}
		}

		truncatedByLines = true;
		return text.substr(startIndex);
	}

	bool TryReadLogTail(std::wstring& text, bool& truncated, std::wstring& errorMessage)
	{
		wil::unique_hfile file(CreateFileW(
			m_logsPath.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr));
		if (!file.is_valid())
		{
			errorMessage = L"读取日志失败。";
			return false;
		}

		LARGE_INTEGER fileSize{};
		if (GetFileSizeEx(file.get(), &fileSize) == FALSE)
		{
			errorMessage = L"读取日志大小失败。";
			return false;
		}

		const auto totalBytes = static_cast<ULONGLONG>((std::max)(LONGLONG{0}, fileSize.QuadPart));
		const auto bytesToRead = static_cast<DWORD>((std::min)(totalBytes, static_cast<ULONGLONG>(LOG_VIEW_MAX_BYTES)));
		LARGE_INTEGER offset{};
		offset.QuadPart = static_cast<LONGLONG>(totalBytes - bytesToRead);
		if (SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN) == FALSE)
		{
			errorMessage = L"定位日志尾部失败。";
			return false;
		}

		std::string buffer(static_cast<size_t>(bytesToRead), '\0');
		size_t totalRead = 0;
		while (totalRead < buffer.size())
		{
			DWORD readBytes = 0;
			if (ReadFile(
				file.get(),
				buffer.data() + totalRead,
				static_cast<DWORD>(buffer.size() - totalRead),
				&readBytes,
				nullptr) == FALSE)
			{
				errorMessage = L"读取日志内容失败。";
				return false;
			}
			if (readBytes == 0)
			{
				break;
			}
			totalRead += readBytes;
		}
		buffer.resize(totalRead);

		bool truncatedByBytes = totalBytes > static_cast<ULONGLONG>(LOG_VIEW_MAX_BYTES);
		if (truncatedByBytes)
		{
			const auto newlinePos = buffer.find('\n');
			if (newlinePos != std::string::npos)
			{
				buffer.erase(0, newlinePos + 1);
			}
		}

		text = Utf8ToWideBestEffort(buffer);
		if (text.empty() && !buffer.empty())
		{
			errorMessage = L"解析日志文本失败。";
			return false;
		}

		bool truncatedByLines = false;
		text = TrimLogsToLastLines(std::move(text), truncatedByLines);
		text = FormatLogsContentForViewer(std::move(text));
		truncated = truncatedByBytes || truncatedByLines;
		return true;
	}

	void UpdateLogsWindowText(const std::wstring& statusText, const std::wstring& contentText, bool scrollToBottom)
	{
		m_logsStatusText = statusText;
		if (m_logsStatusLabel != nullptr)
		{
			SetWindowTextW(m_logsStatusLabel, statusText.c_str());
		}
		if (m_logsWindow != nullptr)
		{
			RECT clientRect{};
			GetClientRect(m_logsWindow, &clientRect);
			LayoutLogsWindowControls(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
		}
		if (m_logsEdit != nullptr)
		{
			SetWindowTextW(m_logsEdit, contentText.c_str());
			if (scrollToBottom)
			{
				const auto length = GetWindowTextLengthW(m_logsEdit);
				SendMessageW(m_logsEdit, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
				SendMessageW(m_logsEdit, EM_SCROLLCARET, 0, 0);
			}
		}
	}

	void PopulateFontsIndexList(const FontUiSnapshot& snapshot)
	{
		if (m_fontsIndexListView == nullptr)
		{
			return;
		}

		m_fontsIndexTooltips.clear();
		m_fontsIndexTooltips.reserve(snapshot.m_indexSummaries.size());
		ListView_DeleteAllItems(m_fontsIndexListView);
		for (size_t i = 0; i < snapshot.m_indexSummaries.size(); ++i)
		{
			const auto& summary = snapshot.m_indexSummaries[i];
			m_fontsIndexTooltips.push_back(BuildFontsIndexTooltip(summary));
			LVITEMW item{};
			item.mask = LVIF_TEXT;
			item.iItem = static_cast<int>(i);
			item.pszText = const_cast<wchar_t*>(summary.m_indexPath.c_str());
			ListView_InsertItem(m_fontsIndexListView, &item);
			SetListViewRowText(m_fontsIndexListView, static_cast<int>(i), 1, std::to_wstring(summary.m_fontFileCount));
			SetListViewRowText(m_fontsIndexListView, static_cast<int>(i), 2, std::to_wstring(summary.m_fontNameCount));
		}
	}

	void PopulateFontsResultList(const FontUiSnapshot& snapshot)
	{
		if (m_fontsResultListView == nullptr)
		{
			return;
		}

		m_fontsResultTooltips.clear();
		m_fontsResultTooltips.reserve(snapshot.m_searchResults.size());
		m_fontsCurrentResults = snapshot.m_searchResults;
		ListView_DeleteAllItems(m_fontsResultListView);
		for (size_t i = 0; i < snapshot.m_searchResults.size(); ++i)
		{
			const auto& result = snapshot.m_searchResults[i];
			m_fontsResultTooltips.push_back(BuildFontsResultTooltip(result));
			LVITEMW item{};
			item.mask = LVIF_TEXT;
			item.iItem = static_cast<int>(i);
			item.pszText = const_cast<wchar_t*>(result.m_displayName.c_str());
			ListView_InsertItem(m_fontsResultListView, &item);
			SetListViewRowText(m_fontsResultListView, static_cast<int>(i), 1, result.m_familyNames);
			SetListViewRowText(m_fontsResultListView, static_cast<int>(i), 2, result.m_fullNames);
			SetListViewRowText(m_fontsResultListView, static_cast<int>(i), 3, result.m_postScriptNames);
			SetListViewRowText(m_fontsResultListView, static_cast<int>(i), 4, std::to_wstring(result.m_faceIndex));
			SetListViewRowText(m_fontsResultListView, static_cast<int>(i), 5, result.m_indexPath);
			SetListViewRowText(m_fontsResultListView, static_cast<int>(i), 6, result.m_fontPath);
		}
	}

	LRESULT HandleToolWindowMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		switch (uMsg)
		{
		case WM_CREATE:
		{
			auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
			auto createParams = reinterpret_cast<const ToolWindowCreateParams*>(create->lpCreateParams);
			if (createParams != nullptr && createParams->m_kind == ToolWindowKind::Fonts)
			{
				SetupFontsWindowControls(hWnd);
				RECT clientRect{};
				GetClientRect(hWnd, &clientRect);
				LayoutFontsWindowControls(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
				RefreshFontsWindowContent();
			}
			else if (createParams != nullptr && createParams->m_kind == ToolWindowKind::Logs)
			{
				SetupLogsWindowControls(hWnd);
				RECT clientRect{};
				GetClientRect(hWnd, &clientRect);
				LayoutLogsWindowControls(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
				RefreshLogsWindowContent(true);
				SetTimer(hWnd, LOGS_REFRESH_TIMER_ID, LOGS_REFRESH_INTERVAL_MS, nullptr);
			}
			return 0;
		}
		case WM_SIZE:
			if (hWnd == m_fontsWindow)
			{
				LayoutFontsWindowControls(LOWORD(lParam), HIWORD(lParam));
				return 0;
			}
			if (hWnd == m_logsWindow)
			{
				LayoutLogsWindowControls(LOWORD(lParam), HIWORD(lParam));
				return 0;
			}
			break;
		case WM_GETMINMAXINFO:
		{
			auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
			if (minMaxInfo == nullptr)
			{
				return 0;
			}

			if (hWnd == m_fontsWindow)
			{
				minMaxInfo->ptMinTrackSize.x = 860;
				minMaxInfo->ptMinTrackSize.y = 700;
				return 0;
			}
			if (hWnd == m_logsWindow)
			{
				minMaxInfo->ptMinTrackSize.x = 760;
				minMaxInfo->ptMinTrackSize.y = 560;
				return 0;
			}
			break;
		}
		case WM_COMMAND:
			if (hWnd == m_fontsWindow
				&& LOWORD(wParam) == IDC_FONTS_SEARCH_EDIT
				&& HIWORD(wParam) == EN_CHANGE)
			{
				KillTimer(hWnd, FONTS_SEARCH_DEBOUNCE_TIMER_ID);
				SetTimer(hWnd, FONTS_SEARCH_DEBOUNCE_TIMER_ID, FONTS_SEARCH_DEBOUNCE_INTERVAL_MS, nullptr);
				return 0;
			}
			if (hWnd == m_logsWindow && HIWORD(wParam) == BN_CLICKED)
			{
				switch (LOWORD(wParam))
				{
				case IDC_LOGS_SCROLL_BOTTOM_BUTTON:
					ScrollLogsEditToBottom();
					return 0;
				}
			}
			break;
		case WM_NOTIFY:
			if ((hWnd == m_fontsWindow || hWnd == m_logsWindow) && lParam != 0)
			{
				const auto* notifyHeader = reinterpret_cast<const NMHDR*>(lParam);
				if (notifyHeader->code == LVN_GETINFOTIPW)
				{
					auto* infoTip = reinterpret_cast<NMLVGETINFOTIPW*>(lParam);
					const std::wstring* tooltipText = nullptr;
					if (notifyHeader->idFrom == IDC_FONTS_INDEX_LIST
						&& infoTip->iItem >= 0
						&& static_cast<size_t>(infoTip->iItem) < m_fontsIndexTooltips.size())
					{
						tooltipText = &m_fontsIndexTooltips[static_cast<size_t>(infoTip->iItem)];
					}
					else if (notifyHeader->idFrom == IDC_FONTS_RESULT_LIST
						&& infoTip->iItem >= 0
						&& static_cast<size_t>(infoTip->iItem) < m_fontsResultTooltips.size())
					{
						tooltipText = &m_fontsResultTooltips[static_cast<size_t>(infoTip->iItem)];
					}

					if (tooltipText != nullptr && infoTip->pszText != nullptr && infoTip->cchTextMax > 0)
					{
						StringCchCopyW(infoTip->pszText, static_cast<size_t>(infoTip->cchTextMax), tooltipText->c_str());
					}
					return 0;
				}
				if (notifyHeader->idFrom == IDC_FONTS_RESULT_LIST && notifyHeader->code == NM_DBLCLK)
				{
					auto* activate = reinterpret_cast<NMITEMACTIVATE*>(lParam);
					if (activate->iItem >= 0 && static_cast<size_t>(activate->iItem) < m_fontsCurrentResults.size())
					{
						const bool copyDisplayName = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
						const auto& result = m_fontsCurrentResults[static_cast<size_t>(activate->iItem)];
						CopyUnicodeTextToClipboard(
							hWnd,
							copyDisplayName ? result.m_displayName : result.m_fontPath);
					}
					return 0;
				}
				if ((notifyHeader->idFrom == IDC_FONTS_INDEX_LIST || notifyHeader->idFrom == IDC_FONTS_RESULT_LIST)
					&& notifyHeader->code == NM_CUSTOMDRAW)
				{
					auto* customDraw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
					switch (customDraw->nmcd.dwDrawStage)
					{
					case CDDS_PREPAINT:
						return CDRF_NOTIFYITEMDRAW;
					case CDDS_ITEMPREPAINT:
						customDraw->clrText = PRIMARY_TEXT_COLOR;
						if ((customDraw->nmcd.dwItemSpec % 2) == 0)
						{
							customDraw->clrTextBk = PANEL_BACKGROUND_COLOR;
						}
						else
						{
							customDraw->clrTextBk = LIST_ALT_BACKGROUND_COLOR;
						}
						if ((customDraw->nmcd.uItemState & CDIS_SELECTED) != 0)
						{
							customDraw->clrText = LIST_SELECTED_TEXT_COLOR;
							customDraw->clrTextBk = LIST_SELECTED_BACKGROUND_COLOR;
						}
						return CDRF_NEWFONT;
					default:
						break;
					}
				}
			}
			break;
		case WM_TIMER:
			if (hWnd == m_fontsWindow && wParam == FONTS_SEARCH_DEBOUNCE_TIMER_ID)
			{
				KillTimer(hWnd, FONTS_SEARCH_DEBOUNCE_TIMER_ID);
				RefreshFontsWindowContent();
				return 0;
			}
			if (hWnd == m_logsWindow && wParam == LOGS_REFRESH_TIMER_ID)
			{
				RefreshLogsWindowContent(false);
				return 0;
			}
			break;
		case WM_CLOSE:
			if (hWnd == m_fontsWindow)
			{
				KillTimer(hWnd, FONTS_SEARCH_DEBOUNCE_TIMER_ID);
			}
			else if (hWnd == m_logsWindow)
			{
				KillTimer(hWnd, LOGS_REFRESH_TIMER_ID);
			}
			DestroyWindow(hWnd);
			return 0;
		case WM_NCDESTROY:
			if (hWnd == m_fontsWindow)
			{
				m_fontsWindow = nullptr;
				m_fontsTitleLabel = nullptr;
				m_fontsStatusLabel = nullptr;
				m_fontsIndexesSectionLabel = nullptr;
				m_fontsSearchSectionLabel = nullptr;
				m_fontsSearchEdit = nullptr;
				m_fontsSearchSummaryLabel = nullptr;
				m_fontsIndexListView = nullptr;
				m_fontsResultListView = nullptr;
				m_fontsCurrentResults.clear();
				m_fontsIndexTooltips.clear();
				m_fontsResultTooltips.clear();
			}
			else if (hWnd == m_logsWindow)
			{
				m_logsWindow = nullptr;
				m_logsTitleLabel = nullptr;
				m_logsSubtitleLabel = nullptr;
				m_logsStatusLabel = nullptr;
				m_logsContentSectionLabel = nullptr;
				m_logsScrollBottomButton = nullptr;
				m_logsEdit = nullptr;
			}
			return DefWindowProcW(hWnd, uMsg, wParam, lParam);
		case WM_CTLCOLOREDIT:
		case WM_CTLCOLORSTATIC:
		{
			auto control = reinterpret_cast<HWND>(lParam);
			auto dc = reinterpret_cast<HDC>(wParam);
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, PRIMARY_TEXT_COLOR);

			if (control == m_fontsTitleLabel || control == m_logsTitleLabel)
			{
				SetTextColor(dc, ACCENT_TEXT_COLOR);
				SetBkColor(dc, WINDOW_BACKGROUND_COLOR);
				return reinterpret_cast<LRESULT>(m_windowBackgroundBrush != nullptr ? m_windowBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
			}
			if (control == m_fontsIndexesSectionLabel
				|| control == m_fontsSearchSectionLabel
				|| control == m_logsContentSectionLabel)
			{
				SetTextColor(dc, ACCENT_TEXT_COLOR);
				SetBkColor(dc, WINDOW_BACKGROUND_COLOR);
				return reinterpret_cast<LRESULT>(m_windowBackgroundBrush != nullptr ? m_windowBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
			}
			if (control == m_fontsStatusLabel || control == m_fontsSearchSummaryLabel || control == m_logsSubtitleLabel)
			{
				SetTextColor(dc, SECONDARY_TEXT_COLOR);
				SetBkColor(dc, WINDOW_BACKGROUND_COLOR);
				return reinterpret_cast<LRESULT>(m_windowBackgroundBrush != nullptr ? m_windowBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
			}
			if (control == m_fontsSearchEdit)
			{
				SetBkMode(dc, OPAQUE);
				SetBkColor(dc, INPUT_BACKGROUND_COLOR);
				return reinterpret_cast<LRESULT>(m_panelBackgroundBrush != nullptr ? m_panelBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
			}
			if (control == m_logsStatusLabel)
			{
				SetBkMode(dc, OPAQUE);
				SetBkColor(dc, METADATA_BACKGROUND_COLOR);
				return reinterpret_cast<LRESULT>(m_metadataBackgroundBrush != nullptr ? m_metadataBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
			}
			if (control == m_logsEdit)
			{
				SetBkMode(dc, OPAQUE);
				SetBkColor(dc, LOG_BACKGROUND_COLOR);
				return reinterpret_cast<LRESULT>(m_logBackgroundBrush != nullptr ? m_logBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
			}

			SetBkColor(dc, WINDOW_BACKGROUND_COLOR);
			return reinterpret_cast<LRESULT>(m_windowBackgroundBrush != nullptr ? m_windowBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
		}
		default:
			return DefWindowProcW(hWnd, uMsg, wParam, lParam);
		}

		return DefWindowProcW(hWnd, uMsg, wParam, lParam);
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

	void RefreshFontsWindowContent()
	{
		if (m_trayUiDataProvider == nullptr)
		{
			return;
		}

		std::wstring query;
		if (m_fontsSearchEdit != nullptr)
		{
			auto length = GetWindowTextLengthW(m_fontsSearchEdit);
			if (length > 0)
			{
				std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
				GetWindowTextW(m_fontsSearchEdit, buffer.data(), length + 1);
				buffer.resize(static_cast<size_t>(length));
				query = std::move(buffer);
			}
		}

		auto snapshot = m_trayUiDataProvider->CaptureFontUiSnapshot(query);
		if (m_fontsStatusLabel != nullptr)
		{
			SetWindowTextW(m_fontsStatusLabel, snapshot.m_statusMessage.c_str());
		}
		PopulateFontsIndexList(snapshot);

		if (m_fontsSearchSummaryLabel != nullptr)
		{
			if (query.empty())
			{
				SetWindowTextW(m_fontsSearchSummaryLabel, L"输入字体名称进行搜索。");
			}
			else
			{
				std::wstring summary = L"命中 " + std::to_wstring(snapshot.m_totalSearchResultCount) + L" 条结果。";
				if (snapshot.m_isSearchResultTruncated)
				{
					summary += L" 结果已截断，仅显示前 500 条。";
				}
				SetWindowTextW(m_fontsSearchSummaryLabel, summary.c_str());
			}
		}

		PopulateFontsResultList(snapshot);
	}

	void RefreshLogsWindowContent(bool forceReload)
	{
		if (m_logsStatusLabel == nullptr || m_logsEdit == nullptr || m_logsPath.empty())
		{
			return;
		}

		ULONGLONG fileSize = 0;
		FILETIME lastWriteTime{};
		bool exists = false;
		if (!TryGetLogFileMetadata(fileSize, lastWriteTime, exists))
		{
			const std::wstring statusText = L"日志状态获取失败。";
			const std::wstring contentText = BuildLogsFallbackText(L"当前无法读取日志文件元数据。");
			UpdateLogsWindowText(statusText, contentText, false);
			m_logsLastLoadedText = contentText;
			m_logsLastReadFailed = true;
			m_logsHasObservedFile = false;
			m_logsLastFileSize = 0;
			m_logsLastWriteTime = {};
			return;
		}

		if (!exists)
		{
			const std::wstring statusText = L"日志文件尚未创建。";
			const std::wstring contentText = BuildLogsFallbackText(L"当前未找到日志文件。");
			if (forceReload || !m_logsHasObservedFile || m_logsLastLoadedText != contentText)
			{
				UpdateLogsWindowText(statusText, contentText, false);
			}
			m_logsLastLoadedText = contentText;
			m_logsLastReadFailed = false;
			m_logsHasObservedFile = false;
			m_logsLastFileSize = 0;
			m_logsLastWriteTime = {};
			return;
		}

		const bool metadataChanged = !m_logsHasObservedFile
			|| fileSize != m_logsLastFileSize
			|| CompareFileTime(&lastWriteTime, &m_logsLastWriteTime) != 0;
		if (!forceReload && !metadataChanged && !m_logsLastReadFailed)
		{
			return;
		}

		std::wstring contentText;
		std::wstring errorMessage;
		bool truncated = false;
		if (!TryReadLogTail(contentText, truncated, errorMessage))
		{
			const std::wstring statusText = L"读取日志失败。";
			const std::wstring fallbackText = BuildLogsFallbackText(errorMessage.empty() ? L"当前无法读取日志内容。" : errorMessage);
			UpdateLogsWindowText(statusText, fallbackText, false);
			m_logsLastLoadedText = fallbackText;
			m_logsLastReadFailed = true;
			m_logsHasObservedFile = true;
			m_logsLastFileSize = fileSize;
			m_logsLastWriteTime = lastWriteTime;
			return;
		}

		std::wstring statusText = L"日志文件：";
		statusText += GetLogsDisplayName();
		statusText += L" | 更新时间：";
		statusText += FormatFileTimeText(lastWriteTime);
		if (truncated)
		{
			statusText += L" | 仅显示最新日志片段";
		}

		const bool shouldScrollToBottom = forceReload || metadataChanged;
		UpdateLogsWindowText(statusText, contentText, shouldScrollToBottom);
		m_logsLastLoadedText = contentText;
		m_logsLastReadFailed = false;
		m_logsHasObservedFile = true;
		m_logsLastFileSize = fileSize;
		m_logsLastWriteTime = lastWriteTime;
	}
};

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
