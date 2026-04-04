#include "pch.h"
#include "TrayIconImpl.h"

void sfh::SystemTray::Implementation::ShowFontsWindow()
{
	ShowToolWindow(
		m_fontsWindow,
		ToolWindowKind::Fonts,
		L"字体浏览",
		L"");
}

void sfh::SystemTray::Implementation::ShowLogsWindow()
{
	ShowToolWindow(
		m_logsWindow,
		ToolWindowKind::Logs,
		L"日志查看",
		L"");
}

void sfh::SystemTray::Implementation::ShowToolWindow(HWND& handle, ToolWindowKind kind, const wchar_t* title, const wchar_t* text)
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

void sfh::SystemTray::Implementation::DestroyToolWindow(HWND& handle)
{
	if (handle == nullptr)
	{
		return;
	}

	HWND window = handle;
	handle = nullptr;
	DestroyWindow(window);
}

std::wstring sfh::SystemTray::Implementation::BuildFontsIndexTooltip(const FontIndexSummary& summary)
{
	return L"索引： " + summary.m_indexPath
		+ L"\r\n文件数： " + std::to_wstring(summary.m_fontFileCount)
		+ L"\r\n名称数： " + std::to_wstring(summary.m_fontNameCount);
}

std::wstring sfh::SystemTray::Implementation::BuildFontsResultTooltip(const FontSearchResult& result)
{
	return L"显示名： " + result.m_displayName
		+ L"\r\n族名： " + result.m_familyNames
		+ L"\r\n完整名称： " + result.m_fullNames
		+ L"\r\nPostScript： " + result.m_postScriptNames
		+ L"\r\n索引： " + result.m_indexPath
		+ L"\r\n路径： " + result.m_fontPath
		+ L"\r\n字体序号： " + std::to_wstring(result.m_faceIndex);
}

HWND sfh::SystemTray::Implementation::CreateFontsListView(HWND parent, int controlId, int x, int y, int width, int height)
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

void sfh::SystemTray::Implementation::SetupFontsWindowControls(HWND hWnd)
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

void sfh::SystemTray::Implementation::LayoutFontsWindowControls(int clientWidth, int clientHeight)
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

void sfh::SystemTray::Implementation::SetListViewRowText(HWND listView, int rowIndex, int columnIndex, const std::wstring& text)
{
	ListView_SetItemText(listView, rowIndex, columnIndex, const_cast<wchar_t*>(text.c_str()));
}

void sfh::SystemTray::Implementation::CopyUnicodeTextToClipboard(HWND owner, const std::wstring& text)
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

void sfh::SystemTray::Implementation::PopulateFontsIndexList(const FontUiSnapshot& snapshot)
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

void sfh::SystemTray::Implementation::PopulateFontsResultList(const FontUiSnapshot& snapshot)
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

void sfh::SystemTray::Implementation::RefreshFontsWindowContent()
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
