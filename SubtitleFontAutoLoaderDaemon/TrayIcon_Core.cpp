#include "pch.h"
#include "TrayIconImpl.h"

sfh::SystemTray::Implementation::Implementation(IDaemon* daemon)
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
	m_trayAnimationCache.reset();
	m_loadingGifStream.reset();
	m_doneGifStream.reset();
	if (m_gdiplusToken != 0)
	{
		Gdiplus::GdiplusShutdown(m_gdiplusToken);
		m_gdiplusToken = 0;
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

int sfh::SystemTray::Implementation::GetTrayIconPixelSize()
{
	const int smallIconWidth = GetSystemMetrics(SM_CXSMICON);
	const int smallIconHeight = GetSystemMetrics(SM_CYSMICON);
	return (std::max)(smallIconWidth, smallIconHeight);
}

void sfh::SystemTray::Implementation::EnsureGdiplusStarted()
{
	if (m_gdiplusToken == 0)
	{
		Gdiplus::GdiplusStartupInput input;
		THROW_HR_IF(E_FAIL, Gdiplus::GdiplusStartup(&m_gdiplusToken, &input, nullptr) != Gdiplus::Ok);
	}
}

UINT sfh::SystemTray::Implementation::NormalizeGifFrameDelay(UINT delayMs)
{
	return delayMs >= TRAY_MIN_FRAME_DELAY_MS ? delayMs : TRAY_DEFAULT_FRAME_DELAY_MS;
}

BYTE sfh::SystemTray::Implementation::CalculateLoadingTintAlpha(BYTE red, BYTE green, BYTE blue, BYTE alpha)
{
	if (alpha == 0)
	{
		return 0;
	}

	const BYTE luminance = static_cast<BYTE>((static_cast<UINT>(red) * 299
		+ static_cast<UINT>(green) * 587
		+ static_cast<UINT>(blue) * 114) / 1000);
	return static_cast<BYTE>((static_cast<UINT>(255 - luminance) * alpha) / 255);
}

bool sfh::SystemTray::Implementation::IsWhitePixelForTrayTransparency(BYTE red, BYTE green, BYTE blue, BYTE alpha)
{
	const BYTE maxChannel = (std::max)(red, (std::max)(green, blue));
	const BYTE minChannel = (std::min)(red, (std::min)(green, blue));
	return alpha != 0
		&& red >= TRAY_WHITE_TRANSPARENT_THRESHOLD
		&& green >= TRAY_WHITE_TRANSPARENT_THRESHOLD
		&& blue >= TRAY_WHITE_TRANSPARENT_THRESHOLD
		&& static_cast<UINT>(maxChannel - minChannel) <= 3;
}

IStream* sfh::SystemTray::Implementation::EnsureGifResourceStream(UINT resourceId, wil::com_ptr<IStream>& stream)
{
	if (stream)
	{
		LARGE_INTEGER origin{};
		THROW_IF_FAILED(stream->Seek(origin, STREAM_SEEK_SET, nullptr));
		return stream.get();
	}

	HRSRC resource = FindResourceW(wil::GetModuleInstanceHandle(), MAKEINTRESOURCEW(resourceId), RT_RCDATA);
	THROW_LAST_ERROR_IF_NULL(resource);
	const DWORD resourceSize = SizeofResource(wil::GetModuleInstanceHandle(), resource);
	THROW_LAST_ERROR_IF(resourceSize == 0);
	HGLOBAL loadedResource = LoadResource(wil::GetModuleInstanceHandle(), resource);
	THROW_LAST_ERROR_IF_NULL(loadedResource);
	void* resourceData = LockResource(loadedResource);
	THROW_LAST_ERROR_IF_NULL(resourceData);

	wil::unique_hglobal globalData(GlobalAlloc(GMEM_MOVEABLE, resourceSize));
	THROW_LAST_ERROR_IF_NULL(globalData.get());
	void* lockedData = GlobalLock(globalData.get());
	THROW_LAST_ERROR_IF_NULL(lockedData);
	std::memcpy(lockedData, resourceData, resourceSize);
	SetLastError(NO_ERROR);
	if (!GlobalUnlock(globalData.get()))
	{
		THROW_LAST_ERROR_IF(GetLastError() != NO_ERROR);
	}

	IStream* rawStream = nullptr;
	THROW_IF_FAILED(CreateStreamOnHGlobal(globalData.get(), TRUE, &rawStream));
	globalData.release();
	stream.attach(rawStream);

	return stream.get();
}

std::shared_ptr<sfh::SystemTray::Implementation::TrayAnimationCache> sfh::SystemTray::Implementation::EnsureTrayAnimationCache()
{
	const int iconSize = GetTrayIconPixelSize();
	if (!m_trayAnimationCache || m_trayAnimationCache->m_iconSize != iconSize)
	{
		auto cache = std::make_shared<TrayAnimationCache>();
		cache->m_iconSize = iconSize;
		cache->m_loadingFrames = LoadTrayAnimationFrames(IDR_LOADINGGIF, true, 0, iconSize);
		cache->m_doneFrames = LoadTrayAnimationFrames(IDR_DONEGIF, false, TRAY_DONE_FIRST_FRAME_INDEX, iconSize);
		m_trayAnimationCache = std::move(cache);
		m_loadingTrayAnimationFrame = 0;
		m_doneTrayAnimationFrame = 0;
	}
	return m_trayAnimationCache;
}

std::vector<sfh::SystemTray::Implementation::TrayAnimationFrame> sfh::SystemTray::Implementation::LoadTrayAnimationFrames(
	UINT resourceId,
	bool tintBlackToBlue,
	UINT firstFrameIndex,
	int iconSize)
{
	EnsureGdiplusStarted();
	IStream* stream = EnsureGifResourceStream(
		resourceId,
		resourceId == IDR_LOADINGGIF ? m_loadingGifStream : m_doneGifStream);

	Gdiplus::Bitmap source(stream, FALSE);
	THROW_HR_IF(E_FAIL, source.GetLastStatus() != Gdiplus::Ok);
	const UINT frameCount = source.GetFrameCount(&Gdiplus::FrameDimensionTime);
	THROW_HR_IF(E_UNEXPECTED, frameCount == 0);
	THROW_HR_IF(E_INVALIDARG, firstFrameIndex >= frameCount);

	std::vector<UINT> frameDelays(frameCount, TRAY_DEFAULT_FRAME_DELAY_MS);
	const UINT frameDelaySize = source.GetPropertyItemSize(PropertyTagFrameDelay);
	if (frameDelaySize >= sizeof(Gdiplus::PropertyItem))
	{
		std::vector<BYTE> frameDelayData(frameDelaySize);
		auto* frameDelayItem = reinterpret_cast<Gdiplus::PropertyItem*>(frameDelayData.data());
		if (source.GetPropertyItem(PropertyTagFrameDelay, frameDelaySize, frameDelayItem) == Gdiplus::Ok
			&& frameDelayItem->type == PropertyTagTypeLong
			&& frameDelayItem->value != nullptr)
		{
			const auto delayCount = (std::min)(
				static_cast<UINT>(frameDelayItem->length / sizeof(UINT)),
				frameCount);
			const auto* delays = static_cast<const UINT*>(frameDelayItem->value);
			for (UINT i = 0; i < delayCount; ++i)
			{
				frameDelays[i] = NormalizeGifFrameDelay(delays[i] * 10);
			}
		}
	}

	std::vector<TrayAnimationFrame> frames;
	frames.reserve(frameCount - firstFrameIndex);

	for (UINT frameIndex = firstFrameIndex; frameIndex < frameCount; ++frameIndex)
	{
		THROW_HR_IF(E_FAIL, source.SelectActiveFrame(&Gdiplus::FrameDimensionTime, frameIndex) != Gdiplus::Ok);
		Gdiplus::Bitmap frameBitmap(iconSize, iconSize, PixelFormat32bppARGB);
		THROW_HR_IF(E_FAIL, frameBitmap.GetLastStatus() != Gdiplus::Ok);

		{
			Gdiplus::Graphics graphics(&frameBitmap);
			THROW_HR_IF(E_FAIL, graphics.GetLastStatus() != Gdiplus::Ok);
			THROW_HR_IF(E_FAIL, graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy) != Gdiplus::Ok);
			THROW_HR_IF(E_FAIL, graphics.Clear(Gdiplus::Color(0, 0, 0, 0)) != Gdiplus::Ok);
			THROW_HR_IF(E_FAIL, graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver) != Gdiplus::Ok);
			THROW_HR_IF(E_FAIL, graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic) != Gdiplus::Ok);
			THROW_HR_IF(E_FAIL, graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality) != Gdiplus::Ok);
			THROW_HR_IF(E_FAIL, graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality) != Gdiplus::Ok);

			Gdiplus::Rect destination(0, 0, iconSize, iconSize);
			const Gdiplus::Status drawStatus = graphics.DrawImage(
				&source,
				destination,
				0,
				0,
				source.GetWidth(),
				source.GetHeight(),
				Gdiplus::UnitPixel);
			THROW_HR_IF(E_FAIL, drawStatus != Gdiplus::Ok);
		}

		TrayAnimationFrame frame;
		frame.m_icon.reset(CreateTrayIconFromBitmap(frameBitmap, iconSize, tintBlackToBlue));
		frame.m_delayMs = frameDelays[frameIndex];
		frames.push_back(std::move(frame));
	}

	return frames;
}

HICON sfh::SystemTray::Implementation::CreateTrayIconFromBitmap(Gdiplus::Bitmap& bitmap, int iconSize, bool tintBlackToBlue)
{
	HDC screenDc = GetDC(nullptr);
	THROW_LAST_ERROR_IF_NULL(screenDc);
	auto releaseScreenDc = wil::scope_exit([&]
	{
		ReleaseDC(nullptr, screenDc);
	});

	HDC memoryDc = CreateCompatibleDC(screenDc);
	THROW_LAST_ERROR_IF_NULL(memoryDc);
	auto deleteMemoryDc = wil::scope_exit([&]
	{
		DeleteDC(memoryDc);
	});

	BITMAPINFO colorBitmapInfo{};
	colorBitmapInfo.bmiHeader.biSize = sizeof(colorBitmapInfo.bmiHeader);
	colorBitmapInfo.bmiHeader.biWidth = iconSize;
	colorBitmapInfo.bmiHeader.biHeight = -iconSize;
	colorBitmapInfo.bmiHeader.biPlanes = TRAY_ICON_COLOR_PLANES;
	colorBitmapInfo.bmiHeader.biBitCount = TRAY_ICON_COLOR_BITS_PER_PIXEL;
	colorBitmapInfo.bmiHeader.biCompression = BI_RGB;
	void* colorBits = nullptr;
	HBITMAP colorBitmap = CreateDIBSection(
		screenDc,
		&colorBitmapInfo,
		DIB_RGB_COLORS,
		&colorBits,
		nullptr,
		TRAY_ICON_ORIGIN);
	THROW_LAST_ERROR_IF_NULL(colorBitmap);
	THROW_LAST_ERROR_IF_NULL(colorBits);
	auto deleteColorBitmap = wil::scope_exit([&]
	{
		DeleteObject(colorBitmap);
	});

	HBITMAP previousBitmap = static_cast<HBITMAP>(SelectObject(memoryDc, colorBitmap));
	THROW_LAST_ERROR_IF_NULL(previousBitmap);
	auto restoreBitmap = wil::scope_exit([&]
	{
		SelectObject(memoryDc, previousBitmap);
	});

	Gdiplus::BitmapData bitmapData{};
	Gdiplus::Rect lockRect(0, 0, iconSize, iconSize);
	THROW_HR_IF(E_FAIL, bitmap.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bitmapData) != Gdiplus::Ok);
	auto unlockBitmap = wil::scope_exit([&]
	{
		bitmap.UnlockBits(&bitmapData);
	});

	auto* destinationPixels = static_cast<std::uint32_t*>(colorBits);
	for (int y = 0; y < iconSize; ++y)
	{
		const auto* sourceRow = reinterpret_cast<const std::uint32_t*>(
			static_cast<const BYTE*>(bitmapData.Scan0) + static_cast<LONG_PTR>(bitmapData.Stride) * y);
		std::copy(sourceRow, sourceRow + iconSize, destinationPixels + static_cast<size_t>(y) * iconSize);
	}

	HBITMAP maskBitmap = CreateBitmap(
		iconSize,
		iconSize,
		TRAY_ICON_MASK_PLANES,
		TRAY_ICON_MASK_BITS_PER_PIXEL,
		nullptr);
	THROW_LAST_ERROR_IF_NULL(maskBitmap);
	auto deleteMaskBitmap = wil::scope_exit([&]
	{
		DeleteObject(maskBitmap);
	});

	HDC maskDc = CreateCompatibleDC(screenDc);
	THROW_LAST_ERROR_IF_NULL(maskDc);
	auto deleteMaskDc = wil::scope_exit([&]
	{
		DeleteDC(maskDc);
	});

	HBITMAP previousMaskBitmap = static_cast<HBITMAP>(SelectObject(maskDc, maskBitmap));
	THROW_LAST_ERROR_IF_NULL(previousMaskBitmap);
	auto restoreMaskBitmap = wil::scope_exit([&]
	{
		SelectObject(maskDc, previousMaskBitmap);
	});

	auto* pixels = static_cast<std::uint32_t*>(colorBits);
	const size_t pixelCount = static_cast<size_t>(iconSize) * iconSize;
	for (size_t index = 0; index < pixelCount; ++index)
	{
		const BYTE red = static_cast<BYTE>((pixels[index] >> 16) & 0xFF);
		const BYTE green = static_cast<BYTE>((pixels[index] >> 8) & 0xFF);
		const BYTE blue = static_cast<BYTE>(pixels[index] & 0xFF);
		const BYTE alpha = static_cast<BYTE>((pixels[index] >> 24) & 0xFF);
		if (IsWhitePixelForTrayTransparency(red, green, blue, alpha))
		{
			pixels[index] = 0;
			continue;
		}
		if (tintBlackToBlue)
		{
			const BYTE tintedAlpha = CalculateLoadingTintAlpha(red, green, blue, alpha);
			pixels[index] = (static_cast<std::uint32_t>(tintedAlpha) << 24)
				| (static_cast<std::uint32_t>(GetRValue(TRAY_LOADING_BLUE)) << 16)
				| (static_cast<std::uint32_t>(GetGValue(TRAY_LOADING_BLUE)) << 8)
				| static_cast<std::uint32_t>(GetBValue(TRAY_LOADING_BLUE));
		}
	}
	THROW_LAST_ERROR_IF(PatBlt(maskDc, TRAY_ICON_ORIGIN, TRAY_ICON_ORIGIN, iconSize, iconSize, BLACKNESS) == 0);

	ICONINFO iconInfo{};
	iconInfo.fIcon = TRUE;
	iconInfo.hbmMask = maskBitmap;
	iconInfo.hbmColor = colorBitmap;
	HICON icon = CreateIconIndirect(&iconInfo);
	THROW_LAST_ERROR_IF_NULL(icon);
	return icon;
}

HICON sfh::SystemTray::Implementation::CopyBaseTrayIcon()
{
	HICON baseIcon = LoadIconW(wil::GetModuleInstanceHandle(), MAKEINTRESOURCEW(IDI_TRAYICON));
	THROW_LAST_ERROR_IF_NULL(baseIcon);
	HICON iconCopy = CopyIcon(baseIcon);
	THROW_LAST_ERROR_IF_NULL(iconCopy);
	return iconCopy;
}

HICON sfh::SystemTray::Implementation::SelectTrayIcon(
	bool loading,
	bool completeCheck,
	bool& ownsIcon,
	std::shared_ptr<TrayAnimationCache>& cacheOwner)
{
	ownsIcon = false;
	cacheOwner.reset();
	if (loading || completeCheck)
	{
		cacheOwner = m_trayAnimationCache;
		const TrayAnimationCache& cache = *cacheOwner;
		const auto& frames = loading ? cache.m_loadingFrames : cache.m_doneFrames;
		THROW_HR_IF(E_UNEXPECTED, frames.empty());
		const size_t frameIndex = loading
			? m_loadingTrayAnimationFrame % frames.size()
			: (std::min)(m_doneTrayAnimationFrame, frames.size() - 1);
		return frames[frameIndex].m_icon.get();
	}

	ownsIcon = true;
	return CopyBaseTrayIcon();
}

void sfh::SystemTray::Implementation::DestroyCurrentOwnedTrayIcon()
{
	if (m_currentOwnedTrayIcon != nullptr)
	{
		DestroyIcon(m_currentOwnedTrayIcon);
		m_currentOwnedTrayIcon = nullptr;
	}
}

void sfh::SystemTray::Implementation::SetupMessageWindow()
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

void sfh::SystemTray::Implementation::SetupTrayIcon(bool add, bool advanceAnimation)
{
	const bool loading = IsLoading();
	const ULONGLONG now = GetTickCount64();
	if (m_previousTrayLoading && !loading)
	{
		m_showTrayCompleteCheck = true;
		m_doneTrayLastFrameStarted = 0;
		m_doneTrayAnimationFrame = 0;
	}
	m_previousTrayLoading = loading;

	if (add)
	{
		RtlZeroMemory(&m_iconData, sizeof(m_iconData));
		m_iconData.cbSize = sizeof(m_iconData);
		m_iconData.hWnd = m_hWnd;
		m_iconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
		m_iconData.uCallbackMessage = WM_TRAY_ICON_MESSAGE;
	}
	if (loading || m_showTrayCompleteCheck)
	{
		EnsureTrayAnimationCache();
	}
	if (m_showTrayCompleteCheck && m_doneTrayLastFrameStarted != 0
		&& now - m_doneTrayLastFrameStarted >= TRAY_ICON_CHECK_DURATION_MS)
	{
		m_showTrayCompleteCheck = false;
		m_doneTrayLastFrameStarted = 0;
	}
	if (loading)
	{
		const auto tooltip = BuildLoadingTooltip();
		StringCchCopyW(m_iconData.szTip, std::size(m_iconData.szTip), tooltip.c_str());
		if (advanceAnimation)
		{
			const auto& loadingFrames = m_trayAnimationCache->m_loadingFrames;
			m_loadingTrayAnimationFrame = (m_loadingTrayAnimationFrame + 1) % loadingFrames.size();
		}
	}
	else
	{
		wcscpy_s(m_iconData.szTip, L"SubtitleFontAutoLoaderDaemon");
		if (m_showTrayCompleteCheck && advanceAnimation)
		{
			const auto& doneFrames = m_trayAnimationCache->m_doneFrames;
			if (m_doneTrayAnimationFrame + 1 < doneFrames.size())
			{
				++m_doneTrayAnimationFrame;
				if (m_doneTrayAnimationFrame + 1 == doneFrames.size())
				{
					m_doneTrayLastFrameStarted = now;
				}
			}
		}
		if (m_showTrayCompleteCheck)
		{
			const auto& doneFrames = m_trayAnimationCache->m_doneFrames;
			if (m_doneTrayAnimationFrame + 1 >= doneFrames.size() && m_doneTrayLastFrameStarted == 0)
			{
				m_doneTrayLastFrameStarted = now;
			}
		}
	}

	HICON previousTrayIcon = m_iconData.hIcon;
	HICON previousOwnedTrayIcon = m_currentOwnedTrayIcon;
	auto previousTrayAnimationCache = std::move(m_currentTrayAnimationCache);
	bool nextIconOwned = false;
	std::shared_ptr<TrayAnimationCache> nextTrayAnimationCache;
	HICON nextTrayIcon = SelectTrayIcon(loading, m_showTrayCompleteCheck, nextIconOwned, nextTrayAnimationCache);
	m_currentOwnedTrayIcon = nextIconOwned ? nextTrayIcon : nullptr;
	m_currentTrayAnimationCache = std::move(nextTrayAnimationCache);
	m_iconData.hIcon = nextTrayIcon;
	const BOOL notifyIconResult = Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &m_iconData);
	if (!notifyIconResult)
	{
		m_currentOwnedTrayIcon = previousOwnedTrayIcon;
		m_currentTrayAnimationCache = std::move(previousTrayAnimationCache);
		if (nextIconOwned)
		{
			DestroyIcon(nextTrayIcon);
		}
		m_iconData.hIcon = previousTrayIcon;
		THROW_LAST_ERROR();
	}
	if (previousOwnedTrayIcon != nullptr)
	{
		DestroyIcon(previousOwnedTrayIcon);
	}
	previousTrayAnimationCache.reset();

	if (m_hWnd != nullptr)
	{
		UINT refreshInterval = TRAY_IDLE_REFRESH_INTERVAL_MS;
		if (loading)
		{
			const auto& loadingFrames = m_trayAnimationCache->m_loadingFrames;
			refreshInterval = loadingFrames[m_loadingTrayAnimationFrame % loadingFrames.size()].m_delayMs;
		}
		else if (m_showTrayCompleteCheck)
		{
			const auto& doneFrames = m_trayAnimationCache->m_doneFrames;
			if (m_doneTrayAnimationFrame + 1 < doneFrames.size())
			{
				refreshInterval = doneFrames[m_doneTrayAnimationFrame].m_delayMs;
			}
			else
			{
				const ULONGLONG elapsed = now - m_doneTrayLastFrameStarted;
				refreshInterval = elapsed < TRAY_ICON_CHECK_DURATION_MS
					? static_cast<UINT>(TRAY_ICON_CHECK_DURATION_MS - elapsed)
					: TRAY_MIN_FRAME_DELAY_MS;
			}
			refreshInterval = (std::max)(refreshInterval, static_cast<UINT>(TRAY_MIN_FRAME_DELAY_MS));
		}
		if (loading || m_showTrayCompleteCheck)
		{
			THROW_LAST_ERROR_IF(SetTimer(m_hWnd, TRAY_REFRESH_TIMER_ID, refreshInterval, nullptr) == 0);
		}
		else
		{
			KillTimer(m_hWnd, TRAY_REFRESH_TIMER_ID);
		}
	}
}

void sfh::SystemTray::Implementation::DestroyTrayIcon()
{
	const BOOL notifyIconResult = Shell_NotifyIconW(NIM_DELETE, &m_iconData);
	DestroyCurrentOwnedTrayIcon();
	THROW_LAST_ERROR_IF(!notifyIconResult);
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
		SetupTrayIcon(true, false);
		break;
	case WM_TIMER:
		if (wParam == TRAY_REFRESH_TIMER_ID)
		{
			SetupTrayIcon(false, true);
			return 0;
		}
		break;
	case WM_ENDSESSION:
		m_daemon->NotifyExit();
		break;
	case WM_CLOSE:
		KillTimer(hWnd, TRAY_REFRESH_TIMER_ID);
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
		SetupTrayIcon(false, false);
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
			SetupTrayIcon(true, false);
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
