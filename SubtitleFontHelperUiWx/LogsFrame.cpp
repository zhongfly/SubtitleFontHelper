#include "LogsFrame.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Richedit.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include <wx/font.h>
#include <wx/settings.h>
#include <wx/sizer.h>

namespace
{
	constexpr std::size_t LOG_VIEW_MAX_BYTES = 1024 * 1024;
	constexpr std::size_t LOG_VIEW_MAX_LINES = 5000;

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
		DWORD m_selectionStart = 0;
		DWORD m_selectionEnd = 0;
		LONG m_firstVisibleLine = 0;
		POINT m_scrollPosition{};
		bool m_hasScrollPosition = false;
	};

	wxString ToWxString(std::wstring_view value)
	{
		return wxString(value.data(), value.size());
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
				wxColour(38, 38, 38),
				wxColour(146, 140, 132),
				wxColour(220, 185, 126),
				wxColour(134, 140, 150),
				wxColour(232, 228, 220),
				wxColour(132, 192, 132),
				wxColour(224, 188, 92),
				wxColour(236, 116, 102),
				wxColour(120, 172, 230),
			};
		}

		return {
			wxColour(248, 245, 239),
			wxColour(112, 103, 92),
			wxColour(111, 78, 36),
			wxColour(128, 120, 108),
			wxColour(26, 26, 26),
			wxColour(35, 98, 62),
			wxColour(160, 110, 30),
			wxColour(170, 58, 40),
			wxColour(52, 92, 138),
		};
	}

	const wxColour& GetLogLevelColour(const LogsTextPalette& palette, std::wstring_view level)
	{
		if (level == L"INFO")
		{
			return palette.m_infoText;
		}
		if (level == L"WARN")
		{
			return palette.m_warnText;
		}
		if (level == L"ERROR")
		{
			return palette.m_errorText;
		}
		if (level == L"DEBUG")
		{
			return palette.m_debugText;
		}

		return palette.m_messageText;
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

	LogsViewportState CaptureLogsViewportState(HWND editHandle)
	{
		LogsViewportState state{};
		SendMessageW(
			editHandle,
			EM_GETSEL,
		reinterpret_cast<WPARAM>(&state.m_selectionStart),
		reinterpret_cast<LPARAM>(&state.m_selectionEnd));
	state.m_firstVisibleLine = static_cast<LONG>(SendMessageW(editHandle, EM_GETFIRSTVISIBLELINE, 0, 0));
	POINT scrollPosition{};
	if (SendMessageW(editHandle, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scrollPosition)) != 0)
	{
		state.m_scrollPosition = scrollPosition;
		state.m_hasScrollPosition = true;
	}
	return state;
}

	void RestoreLogsViewportState(HWND editHandle, const LogsViewportState& state)
	{
	const DWORD textLength = static_cast<DWORD>(GetWindowTextLengthW(editHandle));
	const DWORD selectionStart = (std::min)(state.m_selectionStart, textLength);
	const DWORD selectionEnd = (std::min)(state.m_selectionEnd, textLength);
	SendMessageW(editHandle, EM_SETSEL, selectionStart, selectionEnd);
	if (state.m_hasScrollPosition)
	{
		POINT scrollPosition = state.m_scrollPosition;
		SendMessageW(editHandle, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scrollPosition));
		return;
	}

	const LONG currentFirstVisibleLine = static_cast<LONG>(SendMessageW(editHandle, EM_GETFIRSTVISIBLELINE, 0, 0));
	SendMessageW(editHandle, EM_LINESCROLL, 0, state.m_firstVisibleLine - currentFirstVisibleLine);
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
	m_logText = new wxTextCtrl(
		m_panel,
		wxID_ANY,
		wxEmptyString,
		wxDefaultPosition,
		wxDefaultSize,
		wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP | wxHSCROLL | wxTE_RICH2);

	wxFont logFont = m_logText->GetFont();
	logFont.SetFamily(wxFONTFAMILY_TELETYPE);
	m_logText->SetFont(logFont);

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

	wxTextAttr defaultStyle;
	defaultStyle.SetFont(m_logText->GetFont());
	defaultStyle.SetTextColour(palette.m_messageText);
	defaultStyle.SetBackgroundColour(palette.m_background);
	defaultStyle.SetFontWeight(wxFONTWEIGHT_NORMAL);
	m_logText->SetDefaultStyle(defaultStyle);

	if (const auto editHandle = reinterpret_cast<HWND>(m_logText->GetHandle()); editHandle != nullptr)
	{
		const int textMargin = FromDIP(10);
		SendMessageW(editHandle, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(textMargin, textMargin));
	}

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
	ApplyLogTextFormatting();
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

void sfh::ui::LogsFrame::ApplyLogTextFormatting()
{
	if (m_logText == nullptr)
	{
		return;
	}

	const auto palette = GetLogTextPalette();
	const long textLength = m_logText->GetLastPosition();
	if (textLength > 0)
	{
		wxTextAttr baseStyle;
		baseStyle.SetFont(m_logText->GetFont());
		baseStyle.SetTextColour(palette.m_messageText);
		baseStyle.SetBackgroundColour(palette.m_background);
		baseStyle.SetFontWeight(wxFONTWEIGHT_NORMAL);
		m_logText->SetStyle(0, textLength, baseStyle);
	}

	const std::wstring controlText = m_logText->GetValue().ToStdWstring();
	auto applyRange = [this, &palette](size_t start, size_t length, const wxColour& color, wxFontWeight weight = wxFONTWEIGHT_NORMAL)
	{
		if (length == 0)
		{
			return;
		}

		wxTextAttr style;
		style.SetFont(m_logText->GetFont());
		style.SetTextColour(color);
		style.SetBackgroundColour(palette.m_background);
		style.SetFontWeight(weight);
		m_logText->SetStyle(static_cast<long>(start), static_cast<long>(start + length), style);
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
				applyRange(lineStart, 23, palette.m_timestampText);

				const size_t levelNameStart = segments.m_levelStart + 1;
				const size_t levelNameLength = segments.m_levelLength >= 2 ? segments.m_levelLength - 2 : 0;
				applyRange(
					lineStart + segments.m_levelStart,
					segments.m_levelLength,
					GetLogLevelColour(palette, line.substr(levelNameStart, levelNameLength)),
					wxFONTWEIGHT_BOLD);
				applyRange(
					lineStart + segments.m_sourceStart,
					segments.m_sourceLength,
					palette.m_sourceText,
					wxFONTWEIGHT_BOLD);
				applyRange(
					lineStart + segments.m_threadStart,
					segments.m_threadLength,
					palette.m_threadText);
				applyRange(
					lineStart + segments.m_messageStart,
					segments.m_messageLength,
					palette.m_messageText);
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

	HWND editHandle = reinterpret_cast<HWND>(m_logText->GetHandle());
	if (editHandle == nullptr)
	{
		m_logText->ChangeValue(ToWxString(contentText));
		if (scrollToBottom)
		{
			ScrollLogsToBottom();
		}
		return;
	}

	const auto viewportState = scrollToBottom
		? LogsViewportState{}
		: CaptureLogsViewportState(editHandle);

	m_logText->Freeze();
	m_logText->ChangeValue(ToWxString(contentText));
	ApplyLogTextFormatting();
	m_logText->Thaw();

	if (scrollToBottom)
	{
		ScrollLogsToBottom();
	}
	else
	{
		RestoreLogsViewportState(editHandle, viewportState);
	}
}

void sfh::ui::LogsFrame::ScrollLogsToBottom()
{
	m_logText->SetInsertionPointEnd();
	m_logText->ShowPosition(m_logText->GetLastPosition());
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
