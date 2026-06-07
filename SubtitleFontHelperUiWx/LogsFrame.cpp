#include "LogsFrame.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <wx/font.h>
#include <wx/settings.h>
#include <wx/sizer.h>

namespace
{
	constexpr std::size_t LOG_VIEW_MAX_BYTES = 1024 * 1024;
	constexpr std::size_t LOG_VIEW_MAX_LINES = 5000;

	enum LogTextStyle
	{
		LOG_STYLE_DEFAULT = 0,
		LOG_STYLE_TIMESTAMP,
		LOG_STYLE_SOURCE,
		LOG_STYLE_THREAD,
		LOG_STYLE_INFO,
		LOG_STYLE_WARN,
		LOG_STYLE_ERROR,
		LOG_STYLE_DEBUG,
	};

	struct StructuredLogLineSegments
	{
		size_t m_levelStart = 0;
		size_t m_levelLength = 0;
		size_t m_sourceStart = 0;
		size_t m_sourceLength = 0;
		size_t m_threadStart = 0;
		size_t m_threadLength = 0;
		size_t m_messageStart = 0;
		size_t m_messageLength = 0;
	};

	struct LogsTextPalette
	{
		wxColour m_background;
		wxColour m_timestampText;
		wxColour m_sourceText;
		wxColour m_threadText;
		wxColour m_messageText;
		wxColour m_infoText;
		wxColour m_warnText;
		wxColour m_errorText;
		wxColour m_debugText;
	};

	struct LogsViewportState
	{
		int m_selectionStart = 0;
		int m_selectionEnd = 0;
		int m_firstVisibleDisplayLine = 0;
	};

	constexpr wchar_t BYTE_ORDER_MARK = static_cast<wchar_t>(0xFEFF);

	wxString ToWxString(std::wstring_view value)
	{
		return wxString(value.data(), value.size());
	}

	std::string WideToUtf8BestEffort(std::wstring_view text)
	{
		if (text.empty())
		{
			return {};
		}

		int length = WideCharToMultiByte(
			CP_UTF8,
			0,
			text.data(),
			static_cast<int>(text.size()),
			nullptr,
			0,
			nullptr,
			nullptr);
		if (length <= 0)
		{
			return {};
		}

		std::string result(static_cast<size_t>(length), '\0');
		length = WideCharToMultiByte(
			CP_UTF8,
			0,
			text.data(),
			static_cast<int>(text.size()),
			result.data(),
			static_cast<int>(result.size()),
			nullptr,
			nullptr);
		if (length <= 0)
		{
			return {};
		}

		result.resize(static_cast<size_t>(length));
		return result;
	}

	std::vector<int> BuildUtf8OffsetMap(std::wstring_view text)
	{
		std::vector<int> offsets(text.size() + 1, 0);
		int byteOffset = 0;
		size_t index = 0;
		while (index < text.size())
		{
			offsets[index] = byteOffset;

			size_t charCount = 1;
			if (index + 1 < text.size()
				&& text[index] >= 0xD800
				&& text[index] <= 0xDBFF
				&& text[index + 1] >= 0xDC00
				&& text[index + 1] <= 0xDFFF)
			{
				charCount = 2;
			}

			const int byteCount = WideCharToMultiByte(
				CP_UTF8,
				0,
				text.data() + index,
				static_cast<int>(charCount),
				nullptr,
				0,
				nullptr,
				nullptr);
			if (byteCount > 0)
			{
				byteOffset += byteCount;
			}

			index += charCount;
			offsets[index] = byteOffset;
		}

		offsets[text.size()] = byteOffset;
		return offsets;
	}

	bool TryParseStructuredLogLine(std::wstring_view line, StructuredLogLineSegments& segments)
	{
		if (line.size() < 23 || line[23] != L' ')
		{
			return false;
		}

		size_t cursor = 24;
		auto parseBracketedField = [&](size_t& start, size_t& length) -> bool
		{
			if (cursor >= line.size() || line[cursor] != L'[')
			{
				return false;
			}

			const size_t end = line.find(L']', cursor + 1);
			if (end == std::wstring_view::npos)
			{
				return false;
			}

			start = cursor;
			length = end - cursor + 1;
			cursor = end + 1;
			return true;
		};

		if (!parseBracketedField(segments.m_levelStart, segments.m_levelLength))
		{
			return false;
		}
		if (cursor >= line.size() || line[cursor] != L' ')
		{
			return false;
		}
		++cursor;

		if (!parseBracketedField(segments.m_sourceStart, segments.m_sourceLength))
		{
			return false;
		}
		if (cursor >= line.size() || line[cursor] != L' ')
		{
			return false;
		}
		++cursor;

		if (!parseBracketedField(segments.m_threadStart, segments.m_threadLength))
		{
			return false;
		}

		if (cursor < line.size())
		{
			if (line[cursor] != L' ')
			{
				return false;
			}
			++cursor;
			segments.m_messageStart = cursor;
			segments.m_messageLength = line.size() - cursor;
		}

		return true;
	}

	LogsTextPalette GetLogTextPalette()
	{
		if (wxSystemSettings::GetAppearance().IsDark())
		{
			return {
				wxColour(24, 26, 30),
				wxColour(160, 168, 176),
				wxColour(255, 198, 109),
				wxColour(124, 214, 196),
				wxColour(244, 246, 248),
				wxColour(120, 224, 142),
				wxColour(255, 195, 82),
				wxColour(255, 112, 102),
				wxColour(111, 186, 255),
			};
		}

		return {
			wxColour(252, 252, 250),
			wxColour(91, 96, 105),
			wxColour(111, 67, 178),
			wxColour(0, 117, 117),
			wxColour(16, 18, 22),
			wxColour(0, 118, 61),
			wxColour(170, 91, 0),
			wxColour(196, 39, 39),
			wxColour(34, 100, 184),
		};
	}

	std::uint64_t FileTimeToUInt64(const FILETIME& value)
	{
		ULARGE_INTEGER converted{};
		converted.LowPart = value.dwLowDateTime;
		converted.HighPart = value.dwHighDateTime;
		return converted.QuadPart;
	}

	FILETIME UInt64ToFileTime(std::uint64_t value)
	{
		ULARGE_INTEGER converted{};
		converted.QuadPart = value;

		FILETIME result{};
		result.dwLowDateTime = converted.LowPart;
		result.dwHighDateTime = converted.HighPart;
		return result;
	}

	LogsViewportState CaptureLogsViewportState(wxStyledTextCtrl* logText)
	{
		LogsViewportState state{};
		state.m_selectionStart = logText->GetSelectionStart();
		state.m_selectionEnd = logText->GetSelectionEnd();
		state.m_firstVisibleDisplayLine = logText->GetFirstVisibleLine();
		return state;
	}

	void RestoreLogsViewportState(wxStyledTextCtrl* logText, const LogsViewportState& state)
	{
		const int textLength = logText->GetTextLength();
		const int selectionStart = (std::min)(state.m_selectionStart, textLength);
		const int selectionEnd = (std::min)(state.m_selectionEnd, textLength);
		const int lineCount = logText->GetLineCount();
		int maxFirstVisibleDisplayLine = 0;
		if (lineCount > 0)
		{
			const int lastDocumentLine = lineCount - 1;
			maxFirstVisibleDisplayLine = logText->VisibleFromDocLine(lastDocumentLine)
				+ (std::max)(1, logText->WrapCount(lastDocumentLine))
				- 1;
		}
		const int firstVisibleDisplayLine = std::clamp(
			state.m_firstVisibleDisplayLine,
			0,
			maxFirstVisibleDisplayLine);
		logText->SetSelection(selectionStart, selectionEnd);
		logText->SetFirstVisibleLine(firstVisibleDisplayLine);
	}

	bool IsWindowOrDescendant(HWND ancestor, HWND candidate)
	{
		return candidate != nullptr
			&& (candidate == ancestor || IsChild(ancestor, candidate) != FALSE);
	}

	bool IsPointInsideWindow(HWND hWnd, POINT screenPoint)
	{
		if (hWnd == nullptr)
		{
			return false;
		}

		RECT windowRect{};
		if (GetWindowRect(hWnd, &windowRect) == FALSE)
		{
			return false;
		}
		return PtInRect(&windowRect, screenPoint) != FALSE;
	}

}

sfh::ui::LogsFrame::LogsFrame(const LauncherConfig& config)
	: wxFrame(nullptr, wxID_ANY, BuildSingleInstanceWindowTitle(config), wxDefaultPosition, wxSize(960, 720)),
	  m_config(config),
	  m_refreshTimer(this, REFRESH_TIMER_ID)
{
	BuildLayout();
	ApplyWindowMetrics();
	Bind(wxEVT_TIMER, &LogsFrame::OnRefreshTimer, this, REFRESH_TIMER_ID);
	Bind(wxEVT_SIZE, &LogsFrame::OnSize, this);
	Bind(wxEVT_DPI_CHANGED, &LogsFrame::OnDpiChanged, this);
	Bind(wxEVT_CLOSE_WINDOW, &LogsFrame::OnCloseWindow, this);
	RefreshLogsContent(true);
	m_refreshTimer.Start(1000);
	CentreOnScreen();
}

sfh::ui::LogsFrame::~LogsFrame()
{
	if (m_refreshTimer.IsRunning())
	{
		m_refreshTimer.Stop();
	}
}

void sfh::ui::LogsFrame::BuildLayout()
{
	m_panel = new wxPanel(this);

	auto* rootSizer = new wxBoxSizer(wxVERTICAL);
	auto* toolbarSizer = new wxBoxSizer(wxHORIZONTAL);

	m_titleText = new wxStaticText(m_panel, wxID_ANY, L"日志查看");
	wxFont titleFont = m_titleText->GetFont();
	titleFont.SetPointSize(titleFont.GetPointSize() + 4);
	titleFont.SetWeight(wxFONTWEIGHT_BOLD);
	m_titleText->SetFont(titleFont);

	m_subtitleLabelTextValue = L"当前主日志文件的实时查看器，仅显示最新片段。";
	m_subtitleText = new wxStaticText(
		m_panel,
		wxID_ANY,
		m_subtitleLabelTextValue);
	m_statusLabelTextValue = L"正在读取日志状态...";
	m_statusText = new wxStaticText(m_panel, wxID_ANY, m_statusLabelTextValue);
	m_contentSectionText = new wxStaticText(m_panel, wxID_ANY, L"日志内容");

	m_autoScrollCheck = new wxCheckBox(m_panel, wxID_ANY, L"自动滚动");
	m_autoScrollCheck->SetValue(true);
	m_scrollBottomButton = new wxButton(m_panel, wxID_ANY, L"滚动到底部");
	m_logText = new wxStyledTextCtrl(
		m_panel,
		wxID_ANY,
		wxDefaultPosition,
		wxDefaultSize,
		wxBORDER_SIMPLE);

	wxFont logFont = m_logText->GetFont();
	logFont.SetFamily(wxFONTFAMILY_TELETYPE);
	logFont.SetFaceName(L"Consolas");
	m_logText->SetFont(logFont);
	m_logText->SetTechnology(wxSTC_TECHNOLOGY_DIRECTWRITE);
	m_logText->SetFontQuality(wxSTC_EFF_QUALITY_LCD_OPTIMIZED);
	m_logText->SetCodePage(wxSTC_CP_UTF8);
	m_logText->SetWrapMode(wxSTC_WRAP_WORD);
	m_logText->SetWrapIndentMode(wxSTC_WRAPINDENT_SAME);
	m_logText->SetUseHorizontalScrollBar(false);
	m_logText->SetScrollWidthTracking(true);
	m_logText->SetMarginType(0, wxSTC_MARGIN_SYMBOL);
	m_logText->SetMarginType(1, wxSTC_MARGIN_SYMBOL);
	m_logText->SetMarginType(2, wxSTC_MARGIN_SYMBOL);
	m_logText->SetMarginWidth(0, 0);
	m_logText->SetMarginWidth(1, 0);
	m_logText->SetMarginWidth(2, 0);
	m_logText->SetReadOnly(true);

	m_autoScrollCheck->Bind(wxEVT_CHECKBOX, &LogsFrame::OnAutoScrollChanged, this);
	m_scrollBottomButton->Bind(wxEVT_BUTTON, &LogsFrame::OnScrollBottom, this);

	rootSizer->Add(m_titleText, 0, wxALL, 16);
	rootSizer->Add(m_subtitleText, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
	rootSizer->Add(m_statusText, 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);

	toolbarSizer->Add(m_contentSectionText, 0, wxALIGN_CENTER_VERTICAL);
	toolbarSizer->AddStretchSpacer();
	toolbarSizer->Add(m_autoScrollCheck, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
	toolbarSizer->Add(m_scrollBottomButton, 0, wxALIGN_CENTER_VERTICAL);
	rootSizer->Add(toolbarSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16);
	rootSizer->Add(m_logText, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16);

	m_panel->SetSizer(rootSizer);
}

void sfh::ui::LogsFrame::ApplyWindowMetrics()
{
	if (m_panel == nullptr)
	{
		return;
	}

	const int margin = FromDIP(16);
	const int toolbarGap = FromDIP(8);

	wxFont baseFont = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
	if (!baseFont.IsOk())
	{
		baseFont = GetFont();
	}
	if (baseFont.IsOk())
	{
		wxFont titleFont = baseFont;
		titleFont.SetPointSize(baseFont.GetPointSize() + 4);
		titleFont.SetWeight(wxFONTWEIGHT_BOLD);

		wxFont sectionFont = baseFont;
		sectionFont.SetWeight(wxFONTWEIGHT_BOLD);

		wxFont logFont = wxSystemSettings::GetFont(wxSYS_ANSI_FIXED_FONT);
		if (!logFont.IsOk())
		{
			logFont = baseFont;
			logFont.SetFamily(wxFONTFAMILY_TELETYPE);
		}
		logFont.SetFaceName(L"Consolas");
		if (logFont.GetPointSize() > 0)
		{
			logFont.SetPointSize((std::max)(11, logFont.GetPointSize() + 2));
		}

		m_panel->SetFont(baseFont);
		m_titleText->SetFont(titleFont);
		m_subtitleText->SetFont(baseFont);
		m_statusText->SetFont(baseFont);
		m_contentSectionText->SetFont(sectionFont);
		m_autoScrollCheck->SetFont(baseFont);
		m_scrollBottomButton->SetFont(baseFont);
		m_logText->SetFont(logFont);
	}

	const auto palette = GetLogTextPalette();
	m_logText->SetBackgroundColour(palette.m_background);
	m_logText->SetForegroundColour(palette.m_messageText);

	m_logText->StyleSetFont(wxSTC_STYLE_DEFAULT, m_logText->GetFont());
	m_logText->StyleSetForeground(wxSTC_STYLE_DEFAULT, palette.m_messageText);
	m_logText->StyleSetBackground(wxSTC_STYLE_DEFAULT, palette.m_background);
	m_logText->StyleClearAll();
	m_logText->StyleSetForeground(LOG_STYLE_DEFAULT, palette.m_messageText);
	m_logText->StyleSetBackground(LOG_STYLE_DEFAULT, palette.m_background);
	m_logText->StyleSetFont(LOG_STYLE_DEFAULT, m_logText->GetFont());
	m_logText->StyleSetForeground(LOG_STYLE_TIMESTAMP, palette.m_timestampText);
	m_logText->StyleSetBackground(LOG_STYLE_TIMESTAMP, palette.m_background);
	m_logText->StyleSetFont(LOG_STYLE_TIMESTAMP, m_logText->GetFont());
	m_logText->StyleSetForeground(LOG_STYLE_SOURCE, palette.m_sourceText);
	m_logText->StyleSetBackground(LOG_STYLE_SOURCE, palette.m_background);
	m_logText->StyleSetFont(LOG_STYLE_SOURCE, m_logText->GetFont());
	m_logText->StyleSetBold(LOG_STYLE_SOURCE, true);
	m_logText->StyleSetForeground(LOG_STYLE_THREAD, palette.m_threadText);
	m_logText->StyleSetBackground(LOG_STYLE_THREAD, palette.m_background);
	m_logText->StyleSetFont(LOG_STYLE_THREAD, m_logText->GetFont());
	m_logText->StyleSetForeground(LOG_STYLE_INFO, palette.m_infoText);
	m_logText->StyleSetBackground(LOG_STYLE_INFO, palette.m_background);
	m_logText->StyleSetFont(LOG_STYLE_INFO, m_logText->GetFont());
	m_logText->StyleSetBold(LOG_STYLE_INFO, true);
	m_logText->StyleSetForeground(LOG_STYLE_WARN, palette.m_warnText);
	m_logText->StyleSetBackground(LOG_STYLE_WARN, palette.m_background);
	m_logText->StyleSetFont(LOG_STYLE_WARN, m_logText->GetFont());
	m_logText->StyleSetBold(LOG_STYLE_WARN, true);
	m_logText->StyleSetForeground(LOG_STYLE_ERROR, palette.m_errorText);
	m_logText->StyleSetBackground(LOG_STYLE_ERROR, palette.m_background);
	m_logText->StyleSetFont(LOG_STYLE_ERROR, m_logText->GetFont());
	m_logText->StyleSetBold(LOG_STYLE_ERROR, true);
	m_logText->StyleSetForeground(LOG_STYLE_DEBUG, palette.m_debugText);
	m_logText->StyleSetBackground(LOG_STYLE_DEBUG, palette.m_background);
	m_logText->StyleSetFont(LOG_STYLE_DEBUG, m_logText->GetFont());
	m_logText->StyleSetBold(LOG_STYLE_DEBUG, true);
	m_logText->SetTechnology(wxSTC_TECHNOLOGY_DIRECTWRITE);
	m_logText->SetFontQuality(wxSTC_EFF_QUALITY_LCD_OPTIMIZED);
	m_logText->SetCaretStyle(wxSTC_CARETSTYLE_INVISIBLE);
	m_logText->SetSelBackground(true, wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT));
	m_logText->SetSelForeground(true, wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT));
	m_logText->SetMargins(FromDIP(10), FromDIP(10));
	m_logText->SetMarginType(0, wxSTC_MARGIN_SYMBOL);
	m_logText->SetMarginType(1, wxSTC_MARGIN_SYMBOL);
	m_logText->SetMarginType(2, wxSTC_MARGIN_SYMBOL);
	m_logText->SetMarginWidth(0, 0);
	m_logText->SetMarginWidth(1, 0);
	m_logText->SetMarginWidth(2, 0);

	if (!m_hasAppliedInitialWindowSize)
	{
		SetSize(FromDIP(wxSize(960, 720)));
		m_hasAppliedInitialWindowSize = true;
	}

	SetMinSize(FromDIP(wxSize(760, 560)));
	m_scrollBottomButton->SetMinSize(FromDIP(wxSize(120, 28)));

	auto* rootSizer = m_panel->GetSizer();
	if (rootSizer != nullptr)
	{
		if (auto* item = rootSizer->GetItem(m_titleText)) item->SetBorder(margin);
		if (auto* item = rootSizer->GetItem(m_subtitleText)) item->SetBorder(margin);
		if (auto* item = rootSizer->GetItem(m_statusText)) item->SetBorder(margin);
		if (auto* item = rootSizer->GetItem(m_contentSectionText->GetContainingSizer())) item->SetBorder(margin);
		if (auto* item = rootSizer->GetItem(m_logText)) item->SetBorder(margin);
	}

	if (auto* toolbarSizer = m_autoScrollCheck->GetContainingSizer())
	{
		if (auto* item = toolbarSizer->GetItem(m_autoScrollCheck))
		{
			item->SetBorder(toolbarGap);
		}
	}

	UpdateWrappedLabels();
	Layout();
	ApplyLogTextFormatting(m_lastLoadedText);
}

void sfh::ui::LogsFrame::UpdateWrappedLabels()
{
	if (m_panel == nullptr)
	{
		return;
	}

	const int wrapWidth = std::max(FromDIP(220), m_panel->GetClientSize().GetWidth() - FromDIP(32));
	m_subtitleText->SetLabel(m_subtitleLabelTextValue);
	m_subtitleText->Wrap(wrapWidth);
	m_statusText->SetLabel(m_statusLabelTextValue);
	m_statusText->Wrap(wrapWidth);
}

void sfh::ui::LogsFrame::SetStatusLabelText(const wxString& text)
{
	m_statusLabelTextValue = text;
	if (m_statusText != nullptr)
	{
		m_statusText->SetLabel(m_statusLabelTextValue);
	}
}

bool sfh::ui::LogsFrame::IsLogViewMouseInteractionActive() const
{
	if (m_logText == nullptr || (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0)
	{
		return false;
	}

	const auto logHandle = reinterpret_cast<HWND>(m_logText->GetHandle());
	const auto frameHandle = reinterpret_cast<HWND>(GetHandle());
	const HWND captureHandle = ::GetCapture();
	if (IsWindowOrDescendant(logHandle, captureHandle) || IsWindowOrDescendant(frameHandle, captureHandle))
	{
		return true;
	}

	POINT cursorPos{};
	return GetCursorPos(&cursorPos) != FALSE && IsPointInsideWindow(frameHandle, cursorPos);
}

void sfh::ui::LogsFrame::ApplyLogTextFormatting(std::wstring_view controlText)
{
	if (m_logText == nullptr)
	{
		return;
	}

	const int textLength = m_logText->GetTextLength();
	if (textLength > 0)
	{
		m_logText->StartStyling(0);
		m_logText->SetStyling(textLength, LOG_STYLE_DEFAULT);
	}

	const auto utf8Offsets = BuildUtf8OffsetMap(controlText);
	auto applyRange = [this, &utf8Offsets](size_t start, size_t length, int style)
	{
		if (length == 0 || start >= utf8Offsets.size())
		{
			return;
		}

		const size_t end = (std::min)(start + length, utf8Offsets.size() - 1);
		const int byteStart = utf8Offsets[start];
		const int byteEnd = utf8Offsets[end];
		if (byteEnd <= byteStart)
		{
			return;
		}

		m_logText->StartStyling(byteStart);
		m_logText->SetStyling(byteEnd - byteStart, style);
	};

	size_t lineStart = 0;
	while (lineStart < controlText.size())
	{
		size_t lineEnd = controlText.find(L'\n', lineStart);
		if (lineEnd == std::wstring::npos)
		{
			lineEnd = controlText.size();
		}

		size_t lineLength = lineEnd - lineStart;
		if (lineLength > 0 && controlText[lineStart + lineLength - 1] == L'\r')
		{
			--lineLength;
		}

		std::wstring_view line(controlText.data() + lineStart, lineLength);
		if (!line.empty() && IsLogEntryStartLine(line))
		{
			StructuredLogLineSegments segments{};
			if (TryParseStructuredLogLine(line, segments))
			{
				applyRange(lineStart, 23, LOG_STYLE_TIMESTAMP);

				const size_t levelNameStart = segments.m_levelStart + 1;
				const size_t levelNameLength = segments.m_levelLength >= 2 ? segments.m_levelLength - 2 : 0;
				int levelStyle = LOG_STYLE_DEFAULT;
				const auto levelName = line.substr(levelNameStart, levelNameLength);
				if (levelName == L"INFO")
				{
					levelStyle = LOG_STYLE_INFO;
				}
				else if (levelName == L"WARN")
				{
					levelStyle = LOG_STYLE_WARN;
				}
				else if (levelName == L"ERROR")
				{
					levelStyle = LOG_STYLE_ERROR;
				}
				else if (levelName == L"DEBUG")
				{
					levelStyle = LOG_STYLE_DEBUG;
				}
				applyRange(
					lineStart + segments.m_levelStart,
					segments.m_levelLength,
					levelStyle);
				applyRange(
					lineStart + segments.m_sourceStart,
					segments.m_sourceLength,
					LOG_STYLE_SOURCE);
				applyRange(
					lineStart + segments.m_threadStart,
					segments.m_threadLength,
					LOG_STYLE_THREAD);
			}
		}

		if (lineEnd == controlText.size())
		{
			break;
		}

		lineStart = lineEnd + 1;
	}
}

void sfh::ui::LogsFrame::RefreshLogsContent(bool forceReload)
{
	if (!forceReload && IsLogViewMouseInteractionActive())
	{
		return;
	}

	if (m_config.m_logFilePath.empty())
	{
		const std::wstring statusText = L"未提供日志文件路径。";
		const std::wstring contentText = BuildLogsFallbackText(L"当前未收到日志文件路径。");
		if (forceReload || m_lastLoadedText != contentText)
		{
			UpdateLogsText(statusText, contentText, false);
		}
		m_lastLoadedText = contentText;
		m_lastReadFailed = false;
		m_hasObservedFile = false;
		m_lastFileSize = 0;
		m_lastWriteTime = 0;
		return;
	}

	std::uint64_t fileSize = 0;
	std::uint64_t lastWriteTime = 0;
	bool exists = false;
	if (!TryGetLogFileMetadata(fileSize, lastWriteTime, exists))
	{
		const std::wstring statusText = L"日志状态获取失败。";
		const std::wstring contentText = BuildLogsFallbackText(L"当前无法读取日志文件元数据。");
		UpdateLogsText(statusText, contentText, false);
		m_lastLoadedText = contentText;
		m_lastReadFailed = true;
		m_hasObservedFile = false;
		m_lastFileSize = 0;
		m_lastWriteTime = 0;
		return;
	}

	if (!exists)
	{
		const std::wstring statusText = L"日志文件尚未创建。";
		const std::wstring contentText = BuildLogsFallbackText(L"当前未找到日志文件。");
		if (forceReload || !m_hasObservedFile || m_lastLoadedText != contentText)
		{
			UpdateLogsText(statusText, contentText, false);
		}
		m_lastLoadedText = contentText;
		m_lastReadFailed = false;
		m_hasObservedFile = false;
		m_lastFileSize = 0;
		m_lastWriteTime = 0;
		return;
	}

	const bool metadataChanged = !m_hasObservedFile
		|| fileSize != m_lastFileSize
		|| lastWriteTime != m_lastWriteTime;
	if (!forceReload && !metadataChanged && !m_lastReadFailed)
	{
		return;
	}

	std::wstring contentText;
	std::wstring errorMessage;
	bool truncated = false;
	if (!TryReadLogTail(contentText, truncated, errorMessage))
	{
		const std::wstring statusText = L"读取日志失败。";
		const std::wstring fallbackText = BuildLogsFallbackText(errorMessage.empty() ? L"当前无法读取日志内容。" : errorMessage);
		UpdateLogsText(statusText, fallbackText, false);
		m_lastLoadedText = fallbackText;
		m_lastReadFailed = true;
		m_hasObservedFile = true;
		m_lastFileSize = fileSize;
		m_lastWriteTime = lastWriteTime;
		return;
	}

	std::wstring statusText = L"日志文件：";
	statusText += GetLogsDisplayName();
	statusText += L" | 更新时间：";
	statusText += FormatFileTimeText(lastWriteTime);
	if (truncated)
	{
		statusText += L" | 仅显示最新日志片段";
	}

	const bool shouldScrollToBottom = forceReload || (m_autoScrollEnabled && metadataChanged);
	UpdateLogsText(statusText, contentText, shouldScrollToBottom);
	m_lastLoadedText = contentText;
	m_lastReadFailed = false;
	m_hasObservedFile = true;
	m_lastFileSize = fileSize;
	m_lastWriteTime = lastWriteTime;
}

void sfh::ui::LogsFrame::UpdateLogsText(const std::wstring& statusText, const std::wstring& contentText, bool scrollToBottom)
{
	SetStatusLabelText(ToWxString(statusText));
	UpdateWrappedLabels();
	Layout();

	const auto viewportState = scrollToBottom
		? LogsViewportState{}
		: CaptureLogsViewportState(m_logText);
	const std::string utf8Text = WideToUtf8BestEffort(contentText);

	m_logText->Freeze();
	m_logText->SetReadOnly(false);
	m_logText->ClearAll();
	if (!utf8Text.empty())
	{
		m_logText->AppendTextRaw(utf8Text.data(), static_cast<int>(utf8Text.size()));
	}
	ApplyLogTextFormatting(contentText);
	m_logText->EmptyUndoBuffer();
	m_logText->SetReadOnly(true);
	m_logText->Thaw();

	if (scrollToBottom)
	{
		ScrollLogsToBottom();
	}
	else
	{
		RestoreLogsViewportState(m_logText, viewportState);
	}
}

void sfh::ui::LogsFrame::ScrollLogsToBottom()
{
	m_logText->ScrollToEnd();
	m_logText->SetEmptySelection(m_logText->GetTextLength());
}

void sfh::ui::LogsFrame::UpdateAutoScrollState()
{
	m_autoScrollEnabled = (m_autoScrollCheck != nullptr) && m_autoScrollCheck->GetValue();
}

std::wstring sfh::ui::LogsFrame::GetLogsDisplayName() const
{
	if (m_config.m_logFilePath.empty())
	{
		return L"(none)";
	}

	const auto filename = std::filesystem::path(m_config.m_logFilePath).filename().wstring();
	if (!filename.empty())
	{
		return filename;
	}

	return m_config.m_logFilePath;
}

std::wstring sfh::ui::LogsFrame::BuildLogsFallbackText(const std::wstring& message) const
{
	std::wstring fallbackText = message;
	fallbackText += L"\r\n\r\n日志文件：\r\n";
	fallbackText += m_config.m_logFilePath.empty() ? L"(none)" : m_config.m_logFilePath;
	return fallbackText;
}

std::wstring sfh::ui::LogsFrame::Utf8ToWideBestEffort(std::string_view utf8)
{
	for (size_t offset = 0; offset < (std::min)(utf8.size(), static_cast<size_t>(4)); ++offset)
	{
		const auto length = static_cast<int>(utf8.size() - offset);
		if (length <= 0)
		{
			break;
		}

		const auto* start = utf8.data() + offset;
		const int wideLength = MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			start,
			length,
			nullptr,
			0);
		if (wideLength <= 0)
		{
			continue;
		}

		std::wstring wide(static_cast<size_t>(wideLength), L'\0');
		if (MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			start,
			length,
			wide.data(),
			wideLength) > 0)
		{
			return wide;
		}
	}

	return L"";
}

std::wstring sfh::ui::LogsFrame::FormatFileTimeText(std::uint64_t fileTimeValue)
{
	FILETIME localFileTime{};
	SYSTEMTIME localSystemTime{};
	const auto fileTime = UInt64ToFileTime(fileTimeValue);
	if (FileTimeToLocalFileTime(&fileTime, &localFileTime) == FALSE
		|| FileTimeToSystemTime(&localFileTime, &localSystemTime) == FALSE)
	{
		return L"未知";
	}

	wchar_t buffer[64]{};
	swprintf_s(
		buffer,
		L"%04u-%02u-%02u %02u:%02u:%02u",
		localSystemTime.wYear,
		localSystemTime.wMonth,
		localSystemTime.wDay,
		localSystemTime.wHour,
		localSystemTime.wMinute,
		localSystemTime.wSecond);
	return buffer;
}

bool sfh::ui::LogsFrame::IsAsciiDigit(wchar_t ch)
{
	return ch >= L'0' && ch <= L'9';
}

bool sfh::ui::LogsFrame::IsLogEntryStartLine(std::wstring_view line)
{
	return line.size() >= 23
		&& IsAsciiDigit(line[0])
		&& IsAsciiDigit(line[1])
		&& IsAsciiDigit(line[2])
		&& IsAsciiDigit(line[3])
		&& line[4] == L'-'
		&& IsAsciiDigit(line[5])
		&& IsAsciiDigit(line[6])
		&& line[7] == L'-'
		&& IsAsciiDigit(line[8])
		&& IsAsciiDigit(line[9])
		&& line[10] == L' '
		&& IsAsciiDigit(line[11])
		&& IsAsciiDigit(line[12])
		&& line[13] == L':'
		&& IsAsciiDigit(line[14])
		&& IsAsciiDigit(line[15])
		&& line[16] == L':'
		&& IsAsciiDigit(line[17])
		&& IsAsciiDigit(line[18])
		&& line[19] == L'.'
		&& IsAsciiDigit(line[20])
		&& IsAsciiDigit(line[21])
		&& IsAsciiDigit(line[22]);
}

std::wstring sfh::ui::LogsFrame::FormatLogsContentForViewer(std::wstring text)
{
	if (text.empty())
	{
		return text;
	}

	text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());
	while (!text.empty() && text.back() == L'\n')
	{
		text.pop_back();
	}

	std::wstring formatted;
	formatted.reserve(text.size() + text.size() / 8);

	size_t start = 0;
	bool firstVisibleLine = true;
	bool previousLineBlank = true;
	while (start <= text.size())
	{
		const size_t end = text.find(L'\n', start);
		const size_t length = end == std::wstring::npos ? text.size() - start : end - start;
		std::wstring_view line(text.data() + start, length);
		const bool isBlankLine = line.empty();
		const bool isNewEntry = !isBlankLine && IsLogEntryStartLine(line);

		if (!formatted.empty())
		{
			formatted += L"\r\n";
		}
		if (isNewEntry && !firstVisibleLine && !previousLineBlank)
		{
			formatted += L"\r\n";
		}

		formatted.append(line);

		if (!isBlankLine)
		{
			firstVisibleLine = false;
		}
		previousLineBlank = isBlankLine;

		if (end == std::wstring::npos)
		{
			break;
		}
		start = end + 1;
	}

	return formatted;
}

std::wstring sfh::ui::LogsFrame::TrimLogsToLastLines(std::wstring text, bool& truncatedByLines)
{
	size_t lineCount = 0;
	for (wchar_t ch : text)
	{
		if (ch == L'\n')
		{
			++lineCount;
		}
	}

	if (!text.empty() && text.back() != L'\n')
	{
		++lineCount;
	}

	if (lineCount <= LOG_VIEW_MAX_LINES)
	{
		truncatedByLines = false;
		return text;
	}

	size_t newlineSeen = 0;
	size_t startIndex = 0;
	for (size_t i = text.size(); i > 0; --i)
	{
		if (text[i - 1] == L'\n')
		{
			++newlineSeen;
			if (newlineSeen >= LOG_VIEW_MAX_LINES)
			{
				startIndex = i;
				break;
			}
		}
	}

	truncatedByLines = true;
	return text.substr(startIndex);
}

bool sfh::ui::LogsFrame::TryGetLogFileMetadata(std::uint64_t& fileSize, std::uint64_t& lastWriteTime, bool& exists) const
{
	WIN32_FILE_ATTRIBUTE_DATA attributes{};
	if (GetFileAttributesExW(m_config.m_logFilePath.c_str(), GetFileExInfoStandard, &attributes) == FALSE)
	{
		const auto error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
		{
			exists = false;
			fileSize = 0;
			lastWriteTime = 0;
			return true;
		}
		return false;
	}

	exists = (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	if (!exists)
	{
		fileSize = 0;
		lastWriteTime = 0;
		return true;
	}

	ULARGE_INTEGER size{};
	size.LowPart = attributes.nFileSizeLow;
	size.HighPart = attributes.nFileSizeHigh;
	fileSize = size.QuadPart;
	lastWriteTime = FileTimeToUInt64(attributes.ftLastWriteTime);
	return true;
}

bool sfh::ui::LogsFrame::TryReadLogTail(std::wstring& text, bool& truncated, std::wstring& errorMessage) const
{
	HANDLE file = CreateFileW(
		m_config.m_logFilePath.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		errorMessage = L"读取日志失败。";
		return false;
	}

	struct HandleCloser
	{
		HANDLE m_value = INVALID_HANDLE_VALUE;

		~HandleCloser()
		{
			if (m_value != INVALID_HANDLE_VALUE)
			{
				CloseHandle(m_value);
			}
		}
	} handleCloser{ file };

	LARGE_INTEGER fileSize{};
	if (GetFileSizeEx(file, &fileSize) == FALSE)
	{
		errorMessage = L"读取日志大小失败。";
		return false;
	}

	const auto totalBytes = static_cast<ULONGLONG>((std::max)(LONGLONG{ 0 }, fileSize.QuadPart));
	const auto bytesToRead = static_cast<DWORD>((std::min)(totalBytes, static_cast<ULONGLONG>(LOG_VIEW_MAX_BYTES)));
	LARGE_INTEGER offset{};
	offset.QuadPart = static_cast<LONGLONG>(totalBytes - bytesToRead);
	if (SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) == FALSE)
	{
		errorMessage = L"定位日志尾部失败。";
		return false;
	}

	std::string buffer(static_cast<size_t>(bytesToRead), '\0');
	size_t totalRead = 0;
	while (totalRead < buffer.size())
	{
		DWORD readBytes = 0;
		if (ReadFile(
			file,
			buffer.data() + totalRead,
			static_cast<DWORD>(buffer.size() - totalRead),
			&readBytes,
			nullptr) == FALSE)
		{
			errorMessage = L"读取日志内容失败。";
			return false;
		}
		if (readBytes == 0)
		{
			break;
		}
		totalRead += readBytes;
	}
	buffer.resize(totalRead);

	const bool truncatedByBytes = totalBytes > static_cast<ULONGLONG>(LOG_VIEW_MAX_BYTES);
	if (truncatedByBytes)
	{
		const auto newlinePos = buffer.find('\n');
		if (newlinePos != std::string::npos)
		{
			buffer.erase(0, newlinePos + 1);
		}
	}

	text = Utf8ToWideBestEffort(buffer);
	if (text.empty() && !buffer.empty())
	{
		errorMessage = L"解析日志文本失败。";
		return false;
	}
	if (!text.empty() && text.front() == BYTE_ORDER_MARK)
	{
		text.erase(text.begin());
	}

	bool truncatedByLines = false;
	text = TrimLogsToLastLines(std::move(text), truncatedByLines);
	text = FormatLogsContentForViewer(std::move(text));
	truncated = truncatedByBytes || truncatedByLines;
	return true;
}

void sfh::ui::LogsFrame::OnRefreshTimer(wxTimerEvent& event)
{
	RefreshLogsContent(false);
	event.Skip();
}

void sfh::ui::LogsFrame::OnAutoScrollChanged(wxCommandEvent& event)
{
	UpdateAutoScrollState();
	event.Skip();
}

void sfh::ui::LogsFrame::OnScrollBottom(wxCommandEvent& event)
{
	ScrollLogsToBottom();
	event.Skip();
}

void sfh::ui::LogsFrame::OnSize(wxSizeEvent& event)
{
	event.Skip();
	CallAfter([this]()
	{
		if (!IsBeingDeleted())
		{
			UpdateWrappedLabels();
			Layout();
		}
	});
}

void sfh::ui::LogsFrame::OnDpiChanged(wxDPIChangedEvent& event)
{
	event.Skip();
	CallAfter([this]()
	{
		ApplyWindowMetrics();
	});
}

void sfh::ui::LogsFrame::OnCloseWindow(wxCloseEvent& event)
{
	if (m_refreshTimer.IsRunning())
	{
		m_refreshTimer.Stop();
	}
	event.Skip();
}
