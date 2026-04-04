#pragma once

#include "pch.h"

#include "TrayIcon.h"
#include "resource.h"
#include "TrayUiData.h"
#include "ManagedIndexProgress.h"
#include "IDaemon.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <CommCtrl.h>
#include <Richedit.h>
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
	static constexpr int IDC_FONTS_SEARCH_CLEAR_BUTTON = 1007;
	static constexpr int IDC_LOGS_AUTO_SCROLL_CHECK = 1008;
	static constexpr size_t LOG_VIEW_MAX_BYTES = 1024 * 1024;
	static constexpr size_t LOG_VIEW_MAX_LINES = 5000;
	static constexpr wchar_t LOG_FILE_NAME[] = L"SubtitleFontHelper.log";

	struct ThemeColors
	{
		COLORREF windowBackground;
		COLORREF panelBackground;
		COLORREF metadataBackground;
		COLORREF logBackground;
		COLORREF inputBackground;
		COLORREF primaryText;
		COLORREF secondaryText;
		COLORREF accentText;
		COLORREF logInfoText;
		COLORREF logWarnText;
		COLORREF logErrorText;
		COLORREF logDebugText;
		COLORREF listAltBackground;
		COLORREF listSelectedBackground;
		COLORREF listSelectedText;
	};

	static constexpr ThemeColors LIGHT_COLORS = {
		RGB(244, 240, 232),  // windowBackground
		RGB(252, 249, 243),  // panelBackground
		RGB(236, 231, 222),  // metadataBackground
		RGB(248, 245, 239),  // logBackground
		RGB(255, 253, 248),  // inputBackground
		RGB(50, 44, 36),     // primaryText
		RGB(102, 92, 78),    // secondaryText
		RGB(123, 87, 43),    // accentText
		RGB(60, 105, 60),    // logInfoText
		RGB(160, 110, 30),   // logWarnText
		RGB(170, 58, 40),    // logErrorText
		RGB(94, 86, 148),    // logDebugText
		RGB(248, 244, 237),  // listAltBackground
		RGB(214, 224, 232),  // listSelectedBackground
		RGB(35, 31, 26),     // listSelectedText
	};

	static constexpr ThemeColors DARK_COLORS = {
		RGB(32, 32, 32),     // windowBackground
		RGB(42, 42, 42),     // panelBackground
		RGB(50, 48, 44),     // metadataBackground
		RGB(38, 38, 38),     // logBackground
		RGB(48, 48, 48),     // inputBackground
		RGB(220, 216, 210),  // primaryText
		RGB(160, 155, 148),  // secondaryText
		RGB(200, 165, 110),  // accentText
		RGB(132, 192, 132),  // logInfoText
		RGB(224, 188, 92),   // logWarnText
		RGB(236, 116, 102),  // logErrorText
		RGB(156, 144, 222),  // logDebugText
		RGB(38, 38, 38),     // listAltBackground
		RGB(55, 75, 105),    // listSelectedBackground
		RGB(230, 228, 224),  // listSelectedText
	};

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
	HWND m_fontsSearchClearButton = nullptr;
	HWND m_fontsIndexListView = nullptr;
	HWND m_fontsResultListView = nullptr;
	HWND m_logsTitleLabel = nullptr;
	HWND m_logsSubtitleLabel = nullptr;
	HWND m_logsStatusLabel = nullptr;
	HWND m_logsContentSectionLabel = nullptr;
	HWND m_logsScrollBottomButton = nullptr;
	HWND m_logsEdit = nullptr;
	HWND m_logsAutoScrollCheck = nullptr;
	HMODULE m_richEditModule = nullptr;
	bool m_logsUsesRichEdit = false;
	bool m_logsAutoScrollEnabled = true;
	HFONT m_toolWindowFont = nullptr;
	HFONT m_toolWindowTitleFont = nullptr;
	HFONT m_toolWindowSectionFont = nullptr;
	UINT m_toolWindowFontDpi = 0;
	HBRUSH m_windowBackgroundBrush = nullptr;
	HBRUSH m_panelBackgroundBrush = nullptr;
	HBRUSH m_metadataBackgroundBrush = nullptr;
	HBRUSH m_logBackgroundBrush = nullptr;
	HBRUSH m_inputBackgroundBrush = nullptr;
	ThemeColors m_colors = LIGHT_COLORS;
	bool m_darkModeEnabled = false;
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
	Implementation(IDaemon* daemon, ITrayUiDataProvider* trayUiDataProvider);
	~Implementation();

	void Start();
	void SetManagedIndexTrayProgress(const ManagedIndexTrayProgressSnapshot& snapshot);
	void NotifyFinishLoad();
	void NotifyFontUiDataChanged();

private:
	// DPI helpers
	static int ScaleDpi(int baseValue, UINT dpi)
	{
		return MulDiv(baseValue, static_cast<int>(dpi), 96);
	}

	static UINT GetWindowDpi(HWND hWnd)
	{
		UINT dpi = GetDpiForWindow(hWnd);
		return dpi != 0 ? dpi : 96;
	}

	void InvalidateFontCache();
	void EnsureFontCacheForDpi(UINT dpi);

	// Dark mode
	void DetectDarkMode();
	void RecreateThemeBrushes();
	void ApplyDarkModeToWindow(HWND hWnd);
	void RefreshThemeForOpenWindows();

	// Core (tray icon, message loop)
	bool IsLoading() const;
	std::wstring BuildLoadingTooltip() const;
	void SetupMessageWindow();
	void SetupTrayIcon(bool add);
	void DestroyTrayIcon();
	static void MessageLoop();
	LRESULT CALLBACK MessageHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static void ShowContextMenu(HWND hWnd);
	static Implementation* GetThisByWindow(HWND hWnd);
	static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK ToolWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

	// Theme (fonts, brushes, colors)
	static void ConfigureListViewColumn(HWND listView, int index, int width, const wchar_t* text);
	static LONG ScaleFontHeight(LONG originalHeight, int numerator, int denominator);
	static bool TryCreateMatchedFont(const LOGFONTW& baseFont, const wchar_t* faceName, HFONT& font);
	HFONT GetToolWindowFont();
	HFONT GetToolWindowTitleFont();
	HFONT GetToolWindowSectionFont();
	void ApplyToolWindowFont(HWND control);
	void ApplyToolWindowTitleFont(HWND control);
	void ApplyToolWindowSectionFont(HWND control);
	void ConfigureListViewColors(HWND listView, COLORREF backgroundColor);

	// Fonts window
	void ShowFontsWindow();
	void ShowLogsWindow();
	void ShowToolWindow(HWND& handle, ToolWindowKind kind, const wchar_t* title, const wchar_t* text);
	void DestroyToolWindow(HWND& handle);
	static std::wstring BuildFontsIndexTooltip(const FontIndexSummary& summary);
	static std::wstring BuildFontsResultTooltip(const FontSearchResult& result);
	HWND CreateFontsListView(HWND parent, int controlId, int x, int y, int width, int height);
	void SetupFontsWindowControls(HWND hWnd);
	void LayoutFontsWindowControls(int clientWidth, int clientHeight);
	static void SetListViewRowText(HWND listView, int rowIndex, int columnIndex, const std::wstring& text);
	void CopyUnicodeTextToClipboard(HWND owner, const std::wstring& text);
	void PopulateFontsIndexList(const FontUiSnapshot& snapshot);
	void PopulateFontsResultList(const FontUiSnapshot& snapshot);
	void RefreshFontsWindowContent();

	// Logs window
	void SetupLogsWindowControls(HWND hWnd);
	int CalculateLogsStatusHeight(int availableWidth) const;
	void LayoutLogsWindowControls(int clientWidth, int clientHeight);
	void ScrollLogsEditToBottom();
	bool EnsureRichEditModuleLoaded();
	static COLORREF GetLogLevelColor(const ThemeColors& colors, std::wstring_view level);
	void ApplyColorToLogsRange(size_t start, size_t length, COLORREF color);
	void ApplyLogsRichTextFormatting(const std::wstring& contentText);
	void RefreshLogsEditTheme();
	static std::wstring Utf8ToWideBestEffort(std::string_view utf8);
	static std::wstring FormatFileTimeText(const FILETIME& fileTime);
	void ResolveLogsPath();
	std::wstring GetLogsDisplayName() const;
	bool TryGetLogFileMetadata(ULONGLONG& fileSize, FILETIME& lastWriteTime, bool& exists) const;
	std::wstring BuildLogsFallbackText(const std::wstring& message) const;
	static bool IsAsciiDigit(wchar_t ch);
	static bool IsLogEntryStartLine(std::wstring_view line);
	std::wstring FormatLogsContentForViewer(std::wstring text) const;
	std::wstring TrimLogsToLastLines(std::wstring text, bool& truncatedByLines) const;
	bool TryReadLogTail(std::wstring& text, bool& truncated, std::wstring& errorMessage);
	void UpdateLogsWindowText(const std::wstring& statusText, const std::wstring& contentText, bool scrollToBottom);
	void RefreshLogsWindowContent(bool forceReload);

	// Tool window message handler
	LRESULT HandleToolWindowMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
};
