#include "FontsFrame.h"

#include <stdexcept>
#include <string>

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/font.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/utils.h>

namespace
{
	wxString ToWxString(std::wstring_view text)
	{
		return wxString(text.data(), text.size());
	}
}

sfh::ui::FontsFrame::FontsFrame(const LauncherConfig& config)
	: wxFrame(nullptr, wxID_ANY, BuildSingleInstanceWindowTitle(config), wxDefaultPosition, wxSize(980, 820)),
	  m_config(config),
	  m_client(config.m_rpcPipeName),
	  m_searchTimer(this, SEARCH_DEBOUNCE_TIMER_ID),
	  m_refreshTimer(this, REFRESH_TIMER_ID)
{
	SetMinSize(wxSize(860, 700));
	BuildLayout();
	Bind(wxEVT_TEXT, &FontsFrame::OnSearchTextChanged, this, m_searchCtrl->GetId());
	Bind(wxEVT_SEARCHCTRL_CANCEL_BTN, &FontsFrame::OnSearchCancel, this, m_searchCtrl->GetId());
	Bind(wxEVT_TIMER, &FontsFrame::OnSearchTimer, this, SEARCH_DEBOUNCE_TIMER_ID);
	Bind(wxEVT_TIMER, &FontsFrame::OnRefreshTimer, this, REFRESH_TIMER_ID);
	Bind(wxEVT_LIST_ITEM_ACTIVATED, &FontsFrame::OnResultActivated, this, m_resultList->GetId());
	Bind(wxEVT_THREAD, &FontsFrame::OnWorkerResult, this);
	Bind(wxEVT_CLOSE_WINDOW, &FontsFrame::OnCloseWindow, this);

	StartWorker();
	CentreOnScreen();
	RequestRefresh();
	m_refreshTimer.Start(1000);
}

sfh::ui::FontsFrame::~FontsFrame()
{
	m_searchTimer.Stop();
	m_refreshTimer.Stop();
	StopWorker();
}

void sfh::ui::FontsFrame::BuildLayout()
{
	m_panel = new wxPanel(this);
	auto* rootSizer = new wxBoxSizer(wxVERTICAL);

	m_titleText = new wxStaticText(m_panel, wxID_ANY, L"字体浏览");
	wxFont titleFont = m_titleText->GetFont();
	titleFont.SetPointSize(titleFont.GetPointSize() + 4);
	titleFont.SetWeight(wxFONTWEIGHT_BOLD);
	m_titleText->SetFont(titleFont);

	m_statusText = new wxStaticText(m_panel, wxID_ANY, L"正在连接 daemon...");
	m_indexesSectionText = new wxStaticText(m_panel, wxID_ANY, L"已加载索引");
	wxFont sectionFont = m_indexesSectionText->GetFont();
	sectionFont.SetWeight(wxFONTWEIGHT_BOLD);
	m_indexesSectionText->SetFont(sectionFont);

	m_indexList = new wxListCtrl(
		m_panel,
		wxID_ANY,
		wxDefaultPosition,
		wxDefaultSize,
		wxLC_REPORT | wxLC_SINGLE_SEL);
	ConfigureIndexList();

	m_searchSectionText = new wxStaticText(m_panel, wxID_ANY, L"搜索字体");
	m_searchSectionText->SetFont(sectionFont);

	m_searchCtrl = new wxSearchCtrl(m_panel, wxID_ANY);
	m_searchCtrl->ShowCancelButton(true);
	m_searchCtrl->SetDescriptiveText(L"搜索族名、完整名称或 PostScript 名称");

	m_searchSummaryText = new wxStaticText(m_panel, wxID_ANY, L"输入字体名称进行搜索。");

	m_resultList = new wxListCtrl(
		m_panel,
		wxID_ANY,
		wxDefaultPosition,
		wxDefaultSize,
		wxLC_REPORT | wxLC_SINGLE_SEL);
	ConfigureResultList();

	rootSizer->Add(m_titleText, 0, wxALL, 16);
	rootSizer->Add(m_statusText, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
	rootSizer->Add(m_indexesSectionText, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
	rootSizer->Add(m_indexList, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 16);
	rootSizer->Add(m_searchSectionText, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
	rootSizer->Add(m_searchCtrl, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 16);
	rootSizer->Add(m_searchSummaryText, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
	rootSizer->Add(m_resultList, 2, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 16);

	m_panel->SetSizer(rootSizer);
}

void sfh::ui::FontsFrame::ConfigureIndexList()
{
	m_indexList->AppendColumn(L"索引", wxLIST_FORMAT_LEFT, 420);
	m_indexList->AppendColumn(L"文件数", wxLIST_FORMAT_LEFT, 120);
	m_indexList->AppendColumn(L"名称数", wxLIST_FORMAT_LEFT, 120);
}

void sfh::ui::FontsFrame::ConfigureResultList()
{
	m_resultList->AppendColumn(L"显示名", wxLIST_FORMAT_LEFT, 180);
	m_resultList->AppendColumn(L"族名", wxLIST_FORMAT_LEFT, 170);
	m_resultList->AppendColumn(L"完整名称", wxLIST_FORMAT_LEFT, 190);
	m_resultList->AppendColumn(L"PostScript", wxLIST_FORMAT_LEFT, 170);
	m_resultList->AppendColumn(L"序号", wxLIST_FORMAT_LEFT, 70);
	m_resultList->AppendColumn(L"索引", wxLIST_FORMAT_LEFT, 210);
	m_resultList->AppendColumn(L"路径", wxLIST_FORMAT_LEFT, 280);
}

void sfh::ui::FontsFrame::RequestRefresh()
{
	if (m_isClosing)
	{
		return;
	}

	{
		std::lock_guard lg(m_workerMutex);
		m_pendingQuery = m_searchCtrl != nullptr
			? m_searchCtrl->GetValue().ToStdWstring()
			: std::wstring();
		m_hasPendingRefresh = true;
	}
	m_workerCv.notify_one();
}

void sfh::ui::FontsFrame::StartWorker()
{
	m_workerThread = std::thread(&FontsFrame::WorkerMain, this);
}

void sfh::ui::FontsFrame::StopWorker()
{
	{
		std::lock_guard lg(m_workerMutex);
		if (m_workerStop)
		{
			return;
		}
		m_workerStop = true;
	}
	m_workerCv.notify_one();
	m_client.Close();
	if (m_workerThread.joinable())
	{
		m_workerThread.join();
	}
}

void sfh::ui::FontsFrame::WorkerMain()
{
	while (true)
	{
		RefreshResult result;
		{
			std::unique_lock ul(m_workerMutex);
			m_workerCv.wait(ul, [&]()
			{
				return m_workerStop || m_hasPendingRefresh;
			});
			if (m_workerStop)
			{
				return;
			}

			result.m_requestId = m_nextRequestId++;
			result.m_query = m_pendingQuery;
			m_hasPendingRefresh = false;
		}

		try
		{
			result.m_snapshot = m_client.CaptureSnapshot(result.m_query);
			result.m_hasSnapshot = true;
		}
		catch (const std::exception& e)
		{
			result.m_errorMessage = wxString::FromUTF8(e.what()).ToStdWstring();
		}
		catch (...)
		{
			result.m_errorMessage = L"获取字体数据时发生未知错误。";
		}

		{
			std::lock_guard lg(m_workerMutex);
			m_completedResults.push_back(std::move(result));
		}
		wxQueueEvent(this, new wxThreadEvent(wxEVT_THREAD));
	}
}

void sfh::ui::FontsFrame::ApplyWorkerResults()
{
	std::deque<RefreshResult> completedResults;
	{
		std::lock_guard lg(m_workerMutex);
		completedResults.swap(m_completedResults);
	}

	for (const auto& result : completedResults)
	{
		if (result.m_requestId <= m_lastAppliedRequestId)
		{
			continue;
		}

		if (result.m_hasSnapshot)
		{
			ApplySnapshot(result.m_snapshot, result.m_query);
		}
		else
		{
			ApplyRefreshFailure(result.m_errorMessage);
		}

		m_lastAppliedRequestId = result.m_requestId;
	}
}

void sfh::ui::FontsFrame::ApplySnapshot(const FontUiSnapshotData& snapshot, std::wstring_view query)
{
	m_statusText->SetLabel(ToWxString(snapshot.m_statusMessage));
	PopulateIndexList(snapshot);
	PopulateResultList(snapshot);
	UpdateSearchSummary(snapshot, query);
	Layout();
}

void sfh::ui::FontsFrame::ApplyRefreshFailure(std::wstring_view message)
{
	m_statusText->SetLabel(ToWxString(message));
	m_searchSummaryText->SetLabel(L"当前无法获取字体数据。");
	m_currentResults.clear();
	m_indexList->DeleteAllItems();
	m_resultList->DeleteAllItems();
	Layout();
}

void sfh::ui::FontsFrame::PopulateIndexList(const FontUiSnapshotData& snapshot)
{
	m_indexList->Freeze();
	m_indexList->DeleteAllItems();
	for (size_t i = 0; i < snapshot.m_indexSummaries.size(); ++i)
	{
		const auto& item = snapshot.m_indexSummaries[i];
		const long row = m_indexList->InsertItem(static_cast<long>(i), ToWxString(item.m_indexPath));
		m_indexList->SetItem(row, 1, std::to_wstring(item.m_fontFileCount));
		m_indexList->SetItem(row, 2, std::to_wstring(item.m_fontNameCount));
	}
	m_indexList->Thaw();
}

void sfh::ui::FontsFrame::PopulateResultList(const FontUiSnapshotData& snapshot)
{
	m_currentResults = snapshot.m_searchResults;
	m_resultList->Freeze();
	m_resultList->DeleteAllItems();
	for (size_t i = 0; i < snapshot.m_searchResults.size(); ++i)
	{
		const auto& item = snapshot.m_searchResults[i];
		const long row = m_resultList->InsertItem(static_cast<long>(i), ToWxString(item.m_displayName));
		m_resultList->SetItem(row, 1, ToWxString(item.m_familyNames));
		m_resultList->SetItem(row, 2, ToWxString(item.m_fullNames));
		m_resultList->SetItem(row, 3, ToWxString(item.m_postScriptNames));
		m_resultList->SetItem(row, 4, std::to_wstring(item.m_faceIndex));
		m_resultList->SetItem(row, 5, ToWxString(item.m_indexPath));
		m_resultList->SetItem(row, 6, ToWxString(item.m_fontPath));
	}
	m_resultList->Thaw();
}

void sfh::ui::FontsFrame::UpdateSearchSummary(const FontUiSnapshotData& snapshot, std::wstring_view query)
{
	if (query.empty())
	{
		m_searchSummaryText->SetLabel(L"输入字体名称进行搜索。");
		return;
	}

	std::wstring summary = L"命中 " + std::to_wstring(snapshot.m_totalSearchResultCount) + L" 条结果。";
	if (snapshot.m_isSearchResultTruncated)
	{
		summary += L" 结果已截断，仅显示前 500 条。";
	}
	m_searchSummaryText->SetLabel(ToWxString(summary));
}

void sfh::ui::FontsFrame::CopyResultFieldToClipboard(size_t rowIndex, bool copyDisplayName)
{
	if (rowIndex >= m_currentResults.size())
	{
		return;
	}

	const auto& text = copyDisplayName ? m_currentResults[rowIndex].m_displayName : m_currentResults[rowIndex].m_fontPath;
	if (!wxTheClipboard->Open())
	{
		return;
	}

	wxTheClipboard->SetData(new wxTextDataObject(ToWxString(text)));
	wxTheClipboard->Close();
}

void sfh::ui::FontsFrame::OnSearchTextChanged(wxCommandEvent& event)
{
	m_searchTimer.Stop();
	m_searchTimer.StartOnce(200);
	event.Skip();
}

void sfh::ui::FontsFrame::OnSearchCancel(wxCommandEvent& event)
{
	m_searchCtrl->Clear();
	RequestRefresh();
	event.Skip();
}

void sfh::ui::FontsFrame::OnSearchTimer(wxTimerEvent& event)
{
	RequestRefresh();
	event.Skip();
}

void sfh::ui::FontsFrame::OnRefreshTimer(wxTimerEvent& event)
{
	RequestRefresh();
	event.Skip();
}

void sfh::ui::FontsFrame::OnResultActivated(wxListEvent& event)
{
	const bool copyDisplayName = wxGetKeyState(WXK_SHIFT);
	CopyResultFieldToClipboard(static_cast<size_t>(event.GetIndex()), copyDisplayName);
}

void sfh::ui::FontsFrame::OnWorkerResult(wxThreadEvent& event)
{
	if (!m_isClosing)
	{
		ApplyWorkerResults();
	}
	event.Skip();
}

void sfh::ui::FontsFrame::OnCloseWindow(wxCloseEvent& event)
{
	m_isClosing = true;
	m_searchTimer.Stop();
	m_refreshTimer.Stop();
	StopWorker();
	event.Skip();
}
