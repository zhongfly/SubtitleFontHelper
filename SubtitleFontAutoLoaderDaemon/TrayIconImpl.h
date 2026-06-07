#pragma once

#include "pch.h"

#include "TrayIcon.h"
#include "resource.h"
#include "ManagedIndexProgress.h"
#include "IDaemon.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <Objbase.h>
#include <ObjIdl.h>
#include <gdiplus.h>
#include <wil/com.h>
#include <wil/win32_helpers.h>
#include <wil/resource.h>

#pragma comment(lib, "Gdiplus.lib")

class sfh::SystemTray::Implementation
{
private:
	constexpr static UINT WM_TRAY_ICON_MESSAGE = WM_USER;
	constexpr static UINT WM_UPDATE_TRAY_ICON_MESSAGE = WM_USER + 1;
	constexpr static UINT_PTR TRAY_REFRESH_TIMER_ID = 1;
	static constexpr auto TRAY_IDLE_REFRESH_INTERVAL_MS = 1000;
	static constexpr auto TRAY_DEFAULT_FRAME_DELAY_MS = 100;
	static constexpr auto TRAY_MIN_FRAME_DELAY_MS = 40;
	static constexpr auto TRAY_ICON_ORIGIN = 0;
	static constexpr auto TRAY_ICON_MASK_PLANES = 1;
	static constexpr auto TRAY_ICON_MASK_BITS_PER_PIXEL = 1;
	static constexpr auto TRAY_ICON_COLOR_PLANES = 1;
	static constexpr auto TRAY_ICON_COLOR_BITS_PER_PIXEL = 32;
	static constexpr auto TRAY_ICON_ALPHA_MASK = 0xFF000000;
	static constexpr auto TRAY_ICON_CHECK_DURATION_MS = 2000;
	static constexpr auto TRAY_WHITE_TRANSPARENT_THRESHOLD = 250;
	static constexpr UINT TRAY_DONE_FIRST_FRAME_INDEX = 9;
	static constexpr auto TRAY_LOADING_BLUE = RGB(35, 115, 255);
	static constexpr wchar_t TRAY_WINDOW_CLASS_NAME[] = L"AutoLoaderDaemonTray";

	enum class ToolWindowKind
	{
		Fonts = 0,
		Logs
	};

	struct TrayAnimationFrame
	{
		wil::unique_hicon m_icon;
		UINT m_delayMs = TRAY_DEFAULT_FRAME_DELAY_MS;
	};

	struct TrayAnimationCache
	{
		std::vector<TrayAnimationFrame> m_loadingFrames;
		std::vector<TrayAnimationFrame> m_doneFrames;
		int m_iconSize = 0;
	};

	NOTIFYICONDATAW m_iconData = {};
	HWND m_hWnd = nullptr;
	HICON m_currentOwnedTrayIcon = nullptr;
	ULONG_PTR m_gdiplusToken = 0;
	wil::com_ptr<IStream> m_loadingGifStream;
	wil::com_ptr<IStream> m_doneGifStream;
	std::shared_ptr<TrayAnimationCache> m_trayAnimationCache;
	std::shared_ptr<TrayAnimationCache> m_currentTrayAnimationCache;
	bool m_previousTrayLoading = false;
	bool m_showTrayCompleteCheck = false;
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
	ULONGLONG m_doneTrayLastFrameStarted = 0;
	size_t m_loadingTrayAnimationFrame = 0;
	size_t m_doneTrayAnimationFrame = 0;

public:
	Implementation(IDaemon* daemon);
	~Implementation();

	void Start();
	void SetManagedIndexTrayProgress(const ManagedIndexTrayProgressSnapshot& snapshot);
	void NotifyFinishLoad();

private:
	static int GetTrayIconPixelSize();
	static UINT NormalizeGifFrameDelay(UINT delayMs);
	static BYTE CalculateLoadingTintAlpha(BYTE red, BYTE green, BYTE blue, BYTE alpha);
	static bool IsWhitePixelForTrayTransparency(BYTE red, BYTE green, BYTE blue, BYTE alpha);
	void EnsureGdiplusStarted();
	IStream* EnsureGifResourceStream(UINT resourceId, wil::com_ptr<IStream>& stream);
	std::shared_ptr<TrayAnimationCache> EnsureTrayAnimationCache();
	std::vector<TrayAnimationFrame> LoadTrayAnimationFrames(UINT resourceId, bool tintBlackToBlue, UINT firstFrameIndex, int iconSize);
	HICON CreateTrayIconFromBitmap(Gdiplus::Bitmap& bitmap, int iconSize, bool tintBlackToBlue);
	HICON CopyBaseTrayIcon();
	bool IsLoading() const;
	std::wstring BuildLoadingTooltip() const;
	HICON SelectTrayIcon(bool loading, bool completeCheck, bool& ownsIcon, std::shared_ptr<TrayAnimationCache>& cacheOwner);
	void DestroyCurrentOwnedTrayIcon();
	void SetupMessageWindow();
	void SetupTrayIcon(bool add, bool advanceAnimation);
	void DestroyTrayIcon();
	static void MessageLoop();
	LRESULT CALLBACK MessageHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static void ShowContextMenu(HWND hWnd);
	static Implementation* GetThisByWindow(HWND hWnd);
	static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

	void ShowFontsWindow();
	void ShowLogsWindow();
	void TryLaunchStandaloneUiWindow(ToolWindowKind kind, const wchar_t* windowLabel);
	std::filesystem::path BuildStandaloneUiExecutablePath() const;
	std::wstring BuildRpcPipeName() const;
	void LaunchStandaloneUiWindow(ToolWindowKind kind);
};
