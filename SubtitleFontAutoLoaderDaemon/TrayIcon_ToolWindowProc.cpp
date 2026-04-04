#include "pch.h"
#include "TrayIconImpl.h"

LRESULT CALLBACK sfh::SystemTray::Implementation::ToolWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
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

LRESULT sfh::SystemTray::Implementation::HandleToolWindowMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
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
