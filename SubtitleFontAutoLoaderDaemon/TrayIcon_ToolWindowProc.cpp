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
		ApplyDarkModeToWindow(hWnd);
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
	case WM_DPICHANGED:
	{
		const UINT newDpi = HIWORD(wParam);
		InvalidateFontCache();
		EnsureFontCacheForDpi(newDpi);
		const auto* suggestedRect = reinterpret_cast<const RECT*>(lParam);
		SetWindowPos(hWnd, nullptr,
			suggestedRect->left, suggestedRect->top,
			suggestedRect->right - suggestedRect->left,
			suggestedRect->bottom - suggestedRect->top,
			SWP_NOZORDER | SWP_NOACTIVATE);
		if (hWnd == m_fontsWindow)
		{
			// Re-apply fonts to all controls
			ApplyToolWindowTitleFont(m_fontsTitleLabel);
			ApplyToolWindowFont(m_fontsStatusLabel);
			ApplyToolWindowSectionFont(m_fontsIndexesSectionLabel);
			ApplyToolWindowSectionFont(m_fontsSearchSectionLabel);
			ApplyToolWindowFont(m_fontsSearchEdit);
			ApplyToolWindowFont(m_fontsSearchSummaryLabel);
			ApplyToolWindowFont(m_fontsIndexListView);
			ApplyToolWindowFont(m_fontsResultListView);
			auto header = ListView_GetHeader(m_fontsIndexListView);
			if (header) ApplyToolWindowFont(header);
			header = ListView_GetHeader(m_fontsResultListView);
			if (header) ApplyToolWindowFont(header);
		}
		else if (hWnd == m_logsWindow)
		{
			ApplyToolWindowTitleFont(m_logsTitleLabel);
			ApplyToolWindowFont(m_logsSubtitleLabel);
			ApplyToolWindowSectionFont(m_logsContentSectionLabel);
			ApplyToolWindowFont(m_logsScrollBottomButton);
			ApplyToolWindowFont(m_logsAutoScrollCheck);
			ApplyToolWindowFont(m_logsStatusLabel);
			ApplyToolWindowFont(m_logsEdit);
			RefreshLogsEditTheme();
		}
		return 0;
	}
	case WM_GETMINMAXINFO:
	{
		auto* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
		if (minMaxInfo == nullptr)
		{
			return 0;
		}

		const UINT dpi = GetWindowDpi(hWnd);
		if (hWnd == m_fontsWindow)
		{
			minMaxInfo->ptMinTrackSize.x = ScaleDpi(860, dpi);
			minMaxInfo->ptMinTrackSize.y = ScaleDpi(700, dpi);
			return 0;
		}
		if (hWnd == m_logsWindow)
		{
			minMaxInfo->ptMinTrackSize.x = ScaleDpi(760, dpi);
			minMaxInfo->ptMinTrackSize.y = ScaleDpi(560, dpi);
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
			if (m_fontsSearchClearButton != nullptr)
			{
				const bool hasText = GetWindowTextLengthW(m_fontsSearchEdit) > 0;
				ShowWindow(m_fontsSearchClearButton, hasText ? SW_SHOW : SW_HIDE);
			}
			return 0;
		}
		if (hWnd == m_fontsWindow
			&& LOWORD(wParam) == IDC_FONTS_SEARCH_CLEAR_BUTTON
			&& HIWORD(wParam) == BN_CLICKED)
		{
			SetWindowTextW(m_fontsSearchEdit, L"");
			SetFocus(m_fontsSearchEdit);
			return 0;
		}
		if (hWnd == m_logsWindow && HIWORD(wParam) == BN_CLICKED)
		{
			switch (LOWORD(wParam))
			{
			case IDC_LOGS_SCROLL_BOTTOM_BUTTON:
				ScrollLogsEditToBottom();
				return 0;
			case IDC_LOGS_AUTO_SCROLL_CHECK:
				m_logsAutoScrollEnabled = (SendMessageW(m_logsAutoScrollCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
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
					customDraw->clrText = m_colors.primaryText;
					if ((customDraw->nmcd.dwItemSpec % 2) == 0)
					{
						customDraw->clrTextBk = m_colors.panelBackground;
					}
					else
					{
						customDraw->clrTextBk = m_colors.listAltBackground;
					}
					if ((customDraw->nmcd.uItemState & CDIS_SELECTED) != 0)
					{
						customDraw->clrText = m_colors.listSelectedText;
						customDraw->clrTextBk = m_colors.listSelectedBackground;
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
			m_fontsSearchClearButton = nullptr;
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
			m_logsAutoScrollCheck = nullptr;
			m_logsEdit = nullptr;
			m_logsUsesRichEdit = false;
		}
		return DefWindowProcW(hWnd, uMsg, wParam, lParam);
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORSTATIC:
	{
		auto control = reinterpret_cast<HWND>(lParam);
		auto dc = reinterpret_cast<HDC>(wParam);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, m_colors.primaryText);

		if (control == m_fontsTitleLabel || control == m_logsTitleLabel)
		{
			SetTextColor(dc, m_colors.accentText);
			SetBkColor(dc, m_colors.windowBackground);
			return reinterpret_cast<LRESULT>(m_windowBackgroundBrush != nullptr ? m_windowBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
		}
		if (control == m_fontsIndexesSectionLabel
			|| control == m_fontsSearchSectionLabel
			|| control == m_logsContentSectionLabel)
		{
			SetTextColor(dc, m_colors.accentText);
			SetBkColor(dc, m_colors.windowBackground);
			return reinterpret_cast<LRESULT>(m_windowBackgroundBrush != nullptr ? m_windowBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
		}
		if (control == m_fontsStatusLabel || control == m_fontsSearchSummaryLabel || control == m_logsSubtitleLabel)
		{
			SetTextColor(dc, m_colors.secondaryText);
			SetBkColor(dc, m_colors.windowBackground);
			return reinterpret_cast<LRESULT>(m_windowBackgroundBrush != nullptr ? m_windowBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
		}
		if (control == m_fontsSearchEdit)
		{
			SetBkMode(dc, OPAQUE);
			SetBkColor(dc, m_colors.inputBackground);
			return reinterpret_cast<LRESULT>(m_inputBackgroundBrush != nullptr ? m_inputBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
		}
		if (control == m_logsStatusLabel)
		{
			SetBkMode(dc, OPAQUE);
			SetBkColor(dc, m_colors.metadataBackground);
			return reinterpret_cast<LRESULT>(m_metadataBackgroundBrush != nullptr ? m_metadataBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
		}
		if (control == m_logsEdit && !m_logsUsesRichEdit)
		{
			SetBkMode(dc, OPAQUE);
			SetBkColor(dc, m_colors.logBackground);
			return reinterpret_cast<LRESULT>(m_logBackgroundBrush != nullptr ? m_logBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
		}

		SetBkColor(dc, m_colors.windowBackground);
		return reinterpret_cast<LRESULT>(m_windowBackgroundBrush != nullptr ? m_windowBackgroundBrush : GetSysColorBrush(COLOR_WINDOW));
	}
	default:
		return DefWindowProcW(hWnd, uMsg, wParam, lParam);
	}

	return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}
