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
	static constexpr size_t LOG_VIEW_MAX_BYTES = 1024 * 1024;
	static constexpr size_t LOG_VIEW_MAX_LINES = 5000;
	static constexpr wchar_t LOG_FILE_NAME[] = L"SubtitleFontHelper.log";

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
	HWND m_fontsStatusLabel = nullptr;
	HWND m_fontsSearchEdit = nullptr;
	HWND m_fontsSearchSummaryLabel = nullptr;
	HWND m_fontsIndexListView = nullptr;
	HWND m_fontsResultListView = nullptr;
	HWND m_logsStatusLabel = nullptr;
	HWND m_logsEdit = nullptr;
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
	std::wstring m_logsLastLoadedText;
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
			L"Fonts",
			L"");
	}

	void ShowLogsWindow()
	{
		ShowToolWindow(
			m_logsWindow,
			ToolWindowKind::Logs,
			L"Logs",
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

		handle = CreateWindowExW(
			WS_EX_APPWINDOW,
			TOOL_WINDOW_CLASS_NAME,
			title,
			WS_OVERLAPPEDWINDOW | WS_VISIBLE,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			720,
			360,
			nullptr,
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

	static void ConfigureListViewColumn(HWND listView, int index, int width, const wchar_t* text)
	{
		LVCOLUMNW column{};
		column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
		column.pszText = const_cast<wchar_t*>(text);
		column.cx = width;
		column.iSubItem = index;
		ListView_InsertColumn(listView, index, &column);
	}

	static HWND CreateFontsListView(HWND parent, int controlId, int x, int y, int width, int height)
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
			auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
			SendMessageW(listView, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		}
		return listView;
	}

	void SetupFontsWindowControls(HWND hWnd)
	{
		auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
		m_fontsStatusLabel = CreateWindowExW(
			0,
			L"STATIC",
			L"",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			16,
			16,
			700,
			20,
			hWnd,
			nullptr,
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_fontsIndexListView = CreateFontsListView(hWnd, IDC_FONTS_INDEX_LIST, 16, 44, 700, 130);
		m_fontsSearchEdit = CreateWindowExW(
			WS_EX_CLIENTEDGE,
			L"EDIT",
			L"",
			WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
			16,
			190,
			700,
			24,
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
			222,
			700,
			20,
			hWnd,
			nullptr,
			wil::GetModuleInstanceHandle(),
			nullptr);
		m_fontsResultListView = CreateFontsListView(hWnd, IDC_FONTS_RESULT_LIST, 16, 248, 700, 190);

		if (m_fontsStatusLabel != nullptr)
		{
			SendMessageW(m_fontsStatusLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		}
		if (m_fontsSearchEdit != nullptr)
		{
			SendMessageW(m_fontsSearchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		}
		if (m_fontsSearchSummaryLabel != nullptr)
		{
			SendMessageW(m_fontsSearchSummaryLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		}

		if (m_fontsIndexListView != nullptr)
		{
			ConfigureListViewColumn(m_fontsIndexListView, 0, 260, L"Index");
			ConfigureListViewColumn(m_fontsIndexListView, 1, 90, L"Files");
			ConfigureListViewColumn(m_fontsIndexListView, 2, 90, L"Names");
		}
		if (m_fontsResultListView != nullptr)
		{
			ConfigureListViewColumn(m_fontsResultListView, 0, 180, L"Display");
			ConfigureListViewColumn(m_fontsResultListView, 1, 180, L"Family");
			ConfigureListViewColumn(m_fontsResultListView, 2, 180, L"Full");
			ConfigureListViewColumn(m_fontsResultListView, 3, 180, L"PostScript");
			ConfigureListViewColumn(m_fontsResultListView, 4, 80, L"Face");
			ConfigureListViewColumn(m_fontsResultListView, 5, 220, L"Index");
			ConfigureListViewColumn(m_fontsResultListView, 6, 260, L"Path");
		}
	}

	void SetupLogsWindowControls(HWND hWnd)
	{
		auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
		m_logsStatusLabel = CreateWindowExW(
			0,
			L"STATIC",
			L"",
			WS_CHILD | WS_VISIBLE | SS_LEFT,
			16,
			16,
			700,
			20,
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
			44,
			700,
			320,
			hWnd,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOGS_VIEW_EDIT)),
			wil::GetModuleInstanceHandle(),
			nullptr);
		if (m_logsStatusLabel != nullptr)
		{
			SendMessageW(m_logsStatusLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		}
		if (m_logsEdit != nullptr)
		{
			SendMessageW(m_logsEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
			SendMessageW(m_logsEdit, EM_LIMITTEXT, 0x7FFFFFFE, 0);
		}
	}

	void LayoutFontsWindowControls(int clientWidth, int clientHeight)
	{
		if (m_fontsStatusLabel == nullptr
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
		const int indexHeight = 130;
		const int searchEditTop = top + 28 + indexHeight + 16;
		const int resultTop = searchEditTop + 58;
		const int resultHeight = (std::max)(120, clientHeight - resultTop - 16);

		MoveWindow(m_fontsStatusLabel, left, top, availableWidth, 20, TRUE);
		MoveWindow(m_fontsIndexListView, left, top + 28, availableWidth, indexHeight, TRUE);
		MoveWindow(m_fontsSearchEdit, left, searchEditTop, availableWidth, 24, TRUE);
		MoveWindow(m_fontsSearchSummaryLabel, left, searchEditTop + 32, availableWidth, 20, TRUE);
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
		if (m_logsStatusLabel == nullptr || m_logsEdit == nullptr)
		{
			return;
		}

		const int left = 16;
		const int right = 16;
		const int top = 16;
		const int availableWidth = (std::max)(320, clientWidth - left - right);
		const int logTop = top + 28;
		const int logHeight = (std::max)(120, clientHeight - logTop - 16);

		MoveWindow(m_logsStatusLabel, left, top, availableWidth, 20, TRUE);
		MoveWindow(m_logsEdit, left, logTop, availableWidth, logHeight, TRUE);
	}

	static void SetListViewRowText(HWND listView, int rowIndex, int columnIndex, const std::wstring& text)
	{
		ListView_SetItemText(listView, rowIndex, columnIndex, const_cast<wchar_t*>(text.c_str()));
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
		return message + L"\r\n\r\n路径：\r\n" + m_logsPath;
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
		truncated = truncatedByBytes || truncatedByLines;
		return true;
	}

	void UpdateLogsWindowText(const std::wstring& statusText, const std::wstring& contentText, bool scrollToBottom)
	{
		if (m_logsStatusLabel != nullptr)
		{
			SetWindowTextW(m_logsStatusLabel, statusText.c_str());
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

		ListView_DeleteAllItems(m_fontsIndexListView);
		for (size_t i = 0; i < snapshot.m_indexSummaries.size(); ++i)
		{
			const auto& summary = snapshot.m_indexSummaries[i];
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

		ListView_DeleteAllItems(m_fontsResultListView);
		for (size_t i = 0; i < snapshot.m_searchResults.size(); ++i)
		{
			const auto& result = snapshot.m_searchResults[i];
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
		case WM_COMMAND:
			if (hWnd == m_fontsWindow
				&& LOWORD(wParam) == IDC_FONTS_SEARCH_EDIT
				&& HIWORD(wParam) == EN_CHANGE)
			{
				KillTimer(hWnd, FONTS_SEARCH_DEBOUNCE_TIMER_ID);
				SetTimer(hWnd, FONTS_SEARCH_DEBOUNCE_TIMER_ID, FONTS_SEARCH_DEBOUNCE_INTERVAL_MS, nullptr);
				return 0;
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
				m_fontsStatusLabel = nullptr;
				m_fontsSearchEdit = nullptr;
				m_fontsSearchSummaryLabel = nullptr;
				m_fontsIndexListView = nullptr;
				m_fontsResultListView = nullptr;
			}
			else if (hWnd == m_logsWindow)
			{
				m_logsWindow = nullptr;
				m_logsStatusLabel = nullptr;
				m_logsEdit = nullptr;
			}
			return DefWindowProcW(hWnd, uMsg, wParam, lParam);
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

		std::wstring statusText = L"日志路径：";
		statusText += m_logsPath;
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
