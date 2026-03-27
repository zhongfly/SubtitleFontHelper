#include "pch.h"

#include "TrayIcon.h"
#include "resource.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <wil/win32_helpers.h>
#include <wil/resource.h>

class sfh::SystemTray::Implementation
{
private:
	constexpr static UINT WM_TRAY_ICON_MESSAGE = WM_USER;
	constexpr static UINT WM_UPDATE_TRAY_ICON_MESSAGE = WM_USER + 1;

	NOTIFYICONDATAW m_iconData = {};
	HWND m_hWnd = nullptr;
	std::thread m_trayThread;

	IDaemon* m_daemon;
	std::atomic<size_t> m_checkPoint = 0;

	std::atomic<bool> m_startupLoading = true;
	std::atomic<size_t> m_managedIndexBuildCount = 0;
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

	void SetManagedIndexBuildCount(size_t buildCount)
	{
		m_managedIndexBuildCount = buildCount;
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
		return m_startupLoading.load() || m_managedIndexBuildCount.load() != 0;
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
		wndClass.lpszClassName = L"AutoLoaderDaemonTray";

		RegisterClassW(&wndClass);

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
			if (m_startupLoading.load())
			{
				wcscpy_s(m_iconData.szTip, L"SubtitleFontAutoLoaderDaemon - 正在加载");
			}
			else if (m_managedIndexBuildCount.load() == 1)
			{
				wcscpy_s(m_iconData.szTip, L"SubtitleFontAutoLoaderDaemon - 正在建立索引");
			}
			else
			{
				wcscpy_s(m_iconData.szTip, L"SubtitleFontAutoLoaderDaemon - 正在建立多个索引");
			}
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
			SetupTrayIcon(true);
			break;
		case WM_ENDSESSION:
			m_daemon->NotifyExit();
			break;
		case WM_CLOSE:
			DestroyTrayIcon();
			DestroyWindow(hWnd);
			break;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		case WM_TRAY_ICON_MESSAGE:
			if (lParam == WM_RBUTTONUP)
			{
				ShowContextMenu(hWnd, uMsg, wParam, lParam, m_startupLoading.load());
			}
		case WM_UPDATE_TRAY_ICON_MESSAGE:
			SetupTrayIcon(false);
			return 0;
		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
			case ID_TRAYICONMENU_EXIT:
				m_daemon->NotifyExit();
				break;
			case ID_TRAYICONMENU_RELOAD:
				m_daemon->NotifyReload();
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

	static void ShowContextMenu(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, bool loading)
	{
		POINT cursorPos;

		GetCursorPos(&cursorPos);
		SetForegroundWindow(hWnd);
		HMENU hMenu = LoadMenuW(wil::GetModuleInstanceHandle(), MAKEINTRESOURCEW(IDR_TRAYMENU));
		HMENU hMenu1 = GetSubMenu(hMenu, loading ? 1 : 0);
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

void sfh::SystemTray::SetManagedIndexBuildCount(size_t buildCount)
{
	m_impl->SetManagedIndexBuildCount(buildCount);
}

void sfh::SystemTray::NotifyFinishLoad()
{
	m_impl->NotifyFinishLoad();
}
