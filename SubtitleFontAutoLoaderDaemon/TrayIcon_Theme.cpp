#include "pch.h"
#include "TrayIconImpl.h"

void sfh::SystemTray::Implementation::InvalidateFontCache()
{
	if (m_toolWindowFont != nullptr)
	{
		DeleteObject(m_toolWindowFont);
		m_toolWindowFont = nullptr;
	}
	if (m_toolWindowTitleFont != nullptr)
	{
		DeleteObject(m_toolWindowTitleFont);
		m_toolWindowTitleFont = nullptr;
	}
	if (m_toolWindowSectionFont != nullptr)
	{
		DeleteObject(m_toolWindowSectionFont);
		m_toolWindowSectionFont = nullptr;
	}
	m_toolWindowFontDpi = 0;
}

void sfh::SystemTray::Implementation::EnsureFontCacheForDpi(UINT dpi)
{
	if (m_toolWindowFontDpi == dpi && m_toolWindowFont != nullptr)
	{
		return;
	}
	InvalidateFontCache();
	m_toolWindowFontDpi = dpi;
}

void sfh::SystemTray::Implementation::ConfigureListViewColumn(HWND listView, int index, int width, const wchar_t* text)
{
	LVCOLUMNW column{};
	column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
	column.pszText = const_cast<wchar_t*>(text);
	column.cx = width;
	column.iSubItem = index;
	ListView_InsertColumn(listView, index, &column);
}

LONG sfh::SystemTray::Implementation::ScaleFontHeight(LONG originalHeight, int numerator, int denominator)
{
	if (originalHeight < 0)
	{
		return -MulDiv(-originalHeight, numerator, denominator);
	}
	return MulDiv((std::max)(originalHeight, 1L), numerator, denominator);
}

bool sfh::SystemTray::Implementation::TryCreateMatchedFont(const LOGFONTW& baseFont, const wchar_t* faceName, HFONT& font)
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

HFONT sfh::SystemTray::Implementation::GetToolWindowFont()
{
	if (m_toolWindowFont != nullptr)
	{
		return m_toolWindowFont;
	}

	NONCLIENTMETRICSW metrics{};
	metrics.cbSize = sizeof(metrics);
	LOGFONTW fontSpec{};
	const UINT dpi = m_toolWindowFontDpi != 0 ? m_toolWindowFontDpi : 96;
	if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi) != FALSE)
	{
		fontSpec = metrics.lfMessageFont;
	}
	else
	{
		SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(fontSpec), &fontSpec, 0, dpi);
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

HFONT sfh::SystemTray::Implementation::GetToolWindowTitleFont()
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

HFONT sfh::SystemTray::Implementation::GetToolWindowSectionFont()
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

void sfh::SystemTray::Implementation::ApplyToolWindowFont(HWND control)
{
	if (control == nullptr)
	{
		return;
	}

	auto font = GetToolWindowFont();
	SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void sfh::SystemTray::Implementation::ApplyToolWindowTitleFont(HWND control)
{
	if (control == nullptr)
	{
		return;
	}

	auto font = GetToolWindowTitleFont();
	SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void sfh::SystemTray::Implementation::ApplyToolWindowSectionFont(HWND control)
{
	if (control == nullptr)
	{
		return;
	}

	auto font = GetToolWindowSectionFont();
	SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void sfh::SystemTray::Implementation::ConfigureListViewColors(HWND listView, COLORREF backgroundColor)
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
