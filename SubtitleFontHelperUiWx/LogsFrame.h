#pragma once

#include "LauncherConfig.h"

#include <cstdint>
#include <string>
#include <string_view>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/frame.h>
#include <wx/panel.h>
#include <wx/stc/stc.h>
#include <wx/stattext.h>
#include <wx/timer.h>

namespace sfh::ui
{
	class LogsFrame : public wxFrame
	{
	public:
		explicit LogsFrame(const LauncherConfig& config);
		~LogsFrame() override;

	private:
		static constexpr int REFRESH_TIMER_ID = wxID_HIGHEST + 201;

		void BuildLayout();
		void ApplyWindowMetrics();
		void UpdateWrappedLabels();
		void SetStatusLabelText(const wxString& text);
		void ApplyLogTextFormatting(std::wstring_view controlText);
		bool IsLogViewMouseInteractionActive() const;
		void RefreshLogsContent(bool forceReload);
		void UpdateLogsText(const std::wstring& statusText, const std::wstring& contentText, bool scrollToBottom);
		void ScrollLogsToBottom();
		void UpdateAutoScrollState();
		std::wstring GetLogsDisplayName() const;
		std::wstring BuildLogsFallbackText(const std::wstring& message) const;
		static std::wstring Utf8ToWideBestEffort(std::string_view utf8);
		static std::wstring FormatFileTimeText(std::uint64_t fileTimeValue);
		static bool IsAsciiDigit(wchar_t ch);
		static bool IsLogEntryStartLine(std::wstring_view line);
		static std::wstring FormatLogsContentForViewer(std::wstring text);
		static std::wstring TrimLogsToLastLines(std::wstring text, bool& truncatedByLines);
		bool TryGetLogFileMetadata(std::uint64_t& fileSize, std::uint64_t& lastWriteTime, bool& exists) const;
		bool TryReadLogTail(std::wstring& text, bool& truncated, std::wstring& errorMessage) const;

		void OnRefreshTimer(wxTimerEvent& event);
		void OnAutoScrollChanged(wxCommandEvent& event);
		void OnScrollBottom(wxCommandEvent& event);
		void OnSize(wxSizeEvent& event);
		void OnDpiChanged(wxDPIChangedEvent& event);
		void OnCloseWindow(wxCloseEvent& event);

	private:
		LauncherConfig m_config;
		wxPanel* m_panel = nullptr;
		wxStaticText* m_titleText = nullptr;
		wxStaticText* m_subtitleText = nullptr;
		wxStaticText* m_statusText = nullptr;
		wxStaticText* m_contentSectionText = nullptr;
		wxCheckBox* m_autoScrollCheck = nullptr;
		wxButton* m_scrollBottomButton = nullptr;
		wxStyledTextCtrl* m_logText = nullptr;
		wxTimer m_refreshTimer;
		bool m_autoScrollEnabled = true;
		bool m_hasAppliedInitialWindowSize = false;
		wxString m_subtitleLabelTextValue;
		wxString m_statusLabelTextValue;
		std::wstring m_lastLoadedText;
		std::uint64_t m_lastFileSize = 0;
		std::uint64_t m_lastWriteTime = 0;
		bool m_hasObservedFile = false;
		bool m_lastReadFailed = false;
	};
}
