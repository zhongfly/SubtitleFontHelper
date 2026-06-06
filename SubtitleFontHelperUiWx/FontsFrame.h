#pragma once

#include "FontUiClient.h"
#include "LauncherConfig.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <wx/frame.h>
#include <wx/listctrl.h>
#include <wx/panel.h>
#include <wx/srchctrl.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/thread.h>

namespace sfh::ui
{
	class FontsFrame : public wxFrame
	{
	public:
		explicit FontsFrame(const LauncherConfig& config);
		~FontsFrame() override;

	private:
		void BuildLayout();
		void ConfigureIndexList();
		void ConfigureResultList();
		void RequestRefresh();
		void StartWorker();
		void StopWorker();
		void WorkerMain();
		void ApplyWorkerResults();
		void ApplySnapshot(const FontUiSnapshotData& snapshot, std::wstring_view query);
		void ApplyRefreshFailure(std::wstring_view message);
		void PopulateIndexList(const FontUiSnapshotData& snapshot);
		void PopulateResultList(const FontUiSnapshotData& snapshot);
		void UpdateSearchSummary(const FontUiSnapshotData& snapshot, std::wstring_view query);
		void CopyResultFieldToClipboard(size_t rowIndex, bool copyDisplayName);

		void OnSearchTextChanged(wxCommandEvent& event);
		void OnSearchCancel(wxCommandEvent& event);
		void OnSearchTimer(wxTimerEvent& event);
		void OnRefreshTimer(wxTimerEvent& event);
		void OnResultActivated(wxListEvent& event);
		void OnWorkerResult(wxThreadEvent& event);
		void OnCloseWindow(wxCloseEvent& event);

	private:
		static constexpr int SEARCH_DEBOUNCE_TIMER_ID = wxID_HIGHEST + 100;
		static constexpr int REFRESH_TIMER_ID = wxID_HIGHEST + 101;

		struct RefreshResult
		{
			uint64_t m_requestId = 0;
			std::wstring m_query;
			bool m_hasSnapshot = false;
			FontUiSnapshotData m_snapshot;
			std::wstring m_errorMessage;
		};

		LauncherConfig m_config;
		FontUiClient m_client;
		wxPanel* m_panel = nullptr;
		wxStaticText* m_titleText = nullptr;
		wxStaticText* m_statusText = nullptr;
		wxStaticText* m_indexesSectionText = nullptr;
		wxListCtrl* m_indexList = nullptr;
		wxStaticText* m_searchSectionText = nullptr;
		wxSearchCtrl* m_searchCtrl = nullptr;
		wxStaticText* m_searchSummaryText = nullptr;
		wxListCtrl* m_resultList = nullptr;
		wxTimer m_searchTimer;
		wxTimer m_refreshTimer;
		std::thread m_workerThread;
		std::mutex m_workerMutex;
		std::condition_variable m_workerCv;
		std::deque<RefreshResult> m_completedResults;
		std::wstring m_pendingQuery;
		uint64_t m_nextRequestId = 1;
		uint64_t m_lastAppliedRequestId = 0;
		bool m_hasPendingRefresh = false;
		bool m_workerStop = false;
		std::vector<FontUiSearchResultData> m_currentResults;
		bool m_isClosing = false;
	};
}
