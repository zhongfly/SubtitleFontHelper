#include "pch.h"
#include "TrayIconImpl.h"

namespace
{
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
}

void sfh::SystemTray::Implementation::SetupLogsWindowControls(HWND hWnd)
{
	const UINT dpi = GetWindowDpi(hWnd);
	EnsureFontCacheForDpi(dpi);
	m_logsUsesRichEdit = false;

	m_logsTitleLabel = CreateWindowExW(
		0,
		L"STATIC",
		L"日志查看",
		WS_CHILD | WS_VISIBLE | SS_LEFT,
		16,
		16,
		700,
		28,
		hWnd,
		nullptr,
		wil::GetModuleInstanceHandle(),
		nullptr);
	m_logsSubtitleLabel = CreateWindowExW(
		0,
		L"STATIC",
		L"当前主日志文件的实时查看器，仅显示最新片段。",
		WS_CHILD | WS_VISIBLE | SS_LEFT,
		16,
		48,
		700,
		20,
		hWnd,
		nullptr,
		wil::GetModuleInstanceHandle(),
		nullptr);
	m_logsContentSectionLabel = CreateWindowExW(
		0,
		L"STATIC",
		L"日志内容",
		WS_CHILD | WS_VISIBLE | SS_LEFT,
		16,
		164,
		700,
		20,
		hWnd,
		nullptr,
		wil::GetModuleInstanceHandle(),
		nullptr);
	m_logsScrollBottomButton = CreateWindowExW(
		0,
		L"BUTTON",
		L"滚动到底部",
		WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		16,
		164,
		140,
		28,
		hWnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOGS_SCROLL_BOTTOM_BUTTON)),
		wil::GetModuleInstanceHandle(),
		nullptr);
	m_logsAutoScrollCheck = CreateWindowExW(
		0,
		L"BUTTON",
		L"自动滚动",
		WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
		16,
		164,
		120,
		28,
		hWnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOGS_AUTO_SCROLL_CHECK)),
		wil::GetModuleInstanceHandle(),
		nullptr);
	m_logsStatusLabel = CreateWindowExW(
		0,
		L"EDIT",
		L"",
		WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
		16,
		78,
		700,
		48,
		hWnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOGS_STATUS_LABEL)),
		wil::GetModuleInstanceHandle(),
		nullptr);
	m_logsEdit = CreateWindowExW(
		WS_EX_CLIENTEDGE,
		EnsureRichEditModuleLoaded() ? MSFTEDIT_CLASS : L"EDIT",
		L"",
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
		16,
		192,
		700,
		320,
		hWnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOGS_VIEW_EDIT)),
		wil::GetModuleInstanceHandle(),
		nullptr);
	if (m_logsEdit == nullptr && m_richEditModule != nullptr)
	{
		m_logsEdit = CreateWindowExW(
			WS_EX_CLIENTEDGE,
			L"EDIT",
			L"",
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
			16,
			192,
			700,
			320,
			hWnd,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOGS_VIEW_EDIT)),
			wil::GetModuleInstanceHandle(),
			nullptr);
	}
	else if (m_logsEdit != nullptr)
	{
		m_logsUsesRichEdit = (m_richEditModule != nullptr);
	}
	if (m_logsTitleLabel != nullptr)
	{
		ApplyToolWindowTitleFont(m_logsTitleLabel);
	}
	if (m_logsSubtitleLabel != nullptr)
	{
		ApplyToolWindowFont(m_logsSubtitleLabel);
	}
	if (m_logsContentSectionLabel != nullptr)
	{
		ApplyToolWindowSectionFont(m_logsContentSectionLabel);
	}
	if (m_logsScrollBottomButton != nullptr)
	{
		ApplyToolWindowFont(m_logsScrollBottomButton);
	}
	if (m_logsAutoScrollCheck != nullptr)
	{
		ApplyToolWindowFont(m_logsAutoScrollCheck);
		SendMessageW(m_logsAutoScrollCheck, BM_SETCHECK, m_logsAutoScrollEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
	}
	if (m_logsStatusLabel != nullptr)
	{
		ApplyToolWindowFont(m_logsStatusLabel);
		SendMessageW(m_logsStatusLabel, EM_LIMITTEXT, 0x7FFFFFFE, 0);
		SendMessageW(m_logsStatusLabel, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
	}
	if (m_logsEdit != nullptr)
	{
		ApplyToolWindowFont(m_logsEdit);
		if (m_logsUsesRichEdit)
		{
			SendMessageW(m_logsEdit, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
		}
		else
		{
			SendMessageW(m_logsEdit, EM_LIMITTEXT, 0x7FFFFFFE, 0);
		}
		SendMessageW(m_logsEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
		if (m_logsUsesRichEdit)
		{
			SendMessageW(m_logsEdit, EM_SETUNDOLIMIT, 0, 0);
			RefreshLogsEditTheme();
		}
	}
}

int sfh::SystemTray::Implementation::CalculateLogsStatusHeight(int availableWidth) const
{
	if (m_logsStatusLabel == nullptr)
	{
		return 30;
	}

	const int textWidth = (std::max)(120, availableWidth - 20);
	const auto font = reinterpret_cast<HFONT>(SendMessageW(m_logsStatusLabel, WM_GETFONT, 0, 0));
	HDC dc = GetDC(m_logsStatusLabel);
	if (dc == nullptr)
	{
		return 30;
	}

	HGDIOBJ oldObject = nullptr;
	if (font != nullptr)
	{
		oldObject = SelectObject(dc, font);
	}

	RECT measureRect{ 0, 0, textWidth, 0 };
	const auto* text = m_logsStatusText.empty() ? L"日志文件：SubtitleFontHelper.log | 更新时间：0000-00-00 00:00:00" : m_logsStatusText.c_str();
	DrawTextW(dc, text, -1, &measureRect, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_CALCRECT | DT_NOPREFIX);
	TEXTMETRICW metrics{};
	GetTextMetricsW(dc, &metrics);

	if (oldObject != nullptr)
	{
		SelectObject(dc, oldObject);
	}
	ReleaseDC(m_logsStatusLabel, dc);

	const int singleLineHeight = metrics.tmHeight + metrics.tmExternalLeading;
	const int measuredHeight = static_cast<int>(measureRect.bottom - measureRect.top);
	return (std::max)(singleLineHeight + 8, measuredHeight + 8);
}

void sfh::SystemTray::Implementation::LayoutLogsWindowControls(int clientWidth, int clientHeight)
{
	if (m_logsTitleLabel == nullptr
		|| m_logsSubtitleLabel == nullptr
		|| m_logsStatusLabel == nullptr
		|| m_logsContentSectionLabel == nullptr
		|| m_logsScrollBottomButton == nullptr
		|| m_logsAutoScrollCheck == nullptr
		|| m_logsEdit == nullptr)
	{
		return;
	}

	const UINT dpi = GetWindowDpi(m_logsWindow != nullptr ? m_logsWindow : m_logsTitleLabel);
	const int left = ScaleDpi(16, dpi);
	const int right = ScaleDpi(16, dpi);
	const int top = ScaleDpi(16, dpi);
	const int availableWidth = (std::max)(ScaleDpi(320, dpi), clientWidth - left - right);
	const int titleHeight = ScaleDpi(28, dpi);
	const int subtitleHeight = ScaleDpi(20, dpi);
	const int statusHeight = CalculateLogsStatusHeight(availableWidth);
	const bool inlineButton = availableWidth >= ScaleDpi(500, dpi);
	const int buttonWidth = ScaleDpi(120, dpi);
	const int buttonHeight = ScaleDpi(28, dpi);
	const int contentSectionTop = top + titleHeight + ScaleDpi(10, dpi) + subtitleHeight + ScaleDpi(14, dpi) + statusHeight + ScaleDpi(18, dpi);
	const int labelWidth = inlineButton
		? (std::max)(ScaleDpi(120, dpi), availableWidth - buttonWidth - ScaleDpi(16, dpi))
		: availableWidth;
	const int buttonLeft = inlineButton
		? (std::max)(left, left + availableWidth - buttonWidth)
		: left;
	const int buttonTop = inlineButton ? contentSectionTop - ScaleDpi(4, dpi) : contentSectionTop + ScaleDpi(24, dpi);
	const int logTop = inlineButton ? contentSectionTop + ScaleDpi(32, dpi) : buttonTop + buttonHeight + ScaleDpi(12, dpi);
	const int logHeight = (std::max)(ScaleDpi(120, dpi), clientHeight - logTop - left);

	MoveWindow(m_logsTitleLabel, left, top, availableWidth, titleHeight, TRUE);
	MoveWindow(m_logsSubtitleLabel, left, top + titleHeight + ScaleDpi(10, dpi), availableWidth, subtitleHeight, TRUE);
	MoveWindow(m_logsStatusLabel, left, top + titleHeight + ScaleDpi(10, dpi) + subtitleHeight + ScaleDpi(14, dpi), availableWidth, statusHeight, TRUE);
	MoveWindow(m_logsContentSectionLabel, left, contentSectionTop, labelWidth, ScaleDpi(20, dpi), TRUE);
	const int checkWidth = ScaleDpi(100, dpi);
	MoveWindow(m_logsScrollBottomButton, buttonLeft, buttonTop, buttonWidth, buttonHeight, TRUE);
	MoveWindow(m_logsAutoScrollCheck, buttonLeft - checkWidth - ScaleDpi(8, dpi), buttonTop + ScaleDpi(2, dpi), checkWidth, buttonHeight, TRUE);
	MoveWindow(m_logsEdit, left, logTop, availableWidth, logHeight, TRUE);
}

void sfh::SystemTray::Implementation::ScrollLogsEditToBottom()
{
	if (m_logsEdit == nullptr)
	{
		return;
	}

	const auto length = GetWindowTextLengthW(m_logsEdit);
	SendMessageW(m_logsEdit, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
	SendMessageW(m_logsEdit, EM_SCROLLCARET, 0, 0);
}

bool sfh::SystemTray::Implementation::EnsureRichEditModuleLoaded()
{
	if (m_richEditModule != nullptr)
	{
		return true;
	}

	m_richEditModule = LoadLibraryW(L"Msftedit.dll");
	return m_richEditModule != nullptr;
}

COLORREF sfh::SystemTray::Implementation::GetLogLevelColor(const ThemeColors& colors, std::wstring_view level)
{
	if (level == L"INFO")
	{
		return colors.logInfoText;
	}
	if (level == L"WARN")
	{
		return colors.logWarnText;
	}
	if (level == L"ERROR")
	{
		return colors.logErrorText;
	}
	if (level == L"DEBUG")
	{
		return colors.logDebugText;
	}
	return colors.primaryText;
}

void sfh::SystemTray::Implementation::ApplyCharacterFormatToLogsRange(
	size_t start,
	size_t length,
	COLORREF color,
	DWORD effects)
{
	if (!m_logsUsesRichEdit || m_logsEdit == nullptr || length == 0)
	{
		return;
	}

	CHARRANGE range{};
	range.cpMin = static_cast<LONG>(start);
	range.cpMax = static_cast<LONG>(start + length);
	SendMessageW(m_logsEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));

	CHARFORMAT2W format{};
	format.cbSize = sizeof(format);
	format.dwMask = CFM_COLOR | CFM_BOLD;
	format.dwEffects = effects;
	format.crTextColor = color;
	SendMessageW(m_logsEdit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
}

void sfh::SystemTray::Implementation::ApplyLogsRichTextFormatting(const std::wstring& contentText)
{
	if (!m_logsUsesRichEdit || m_logsEdit == nullptr)
	{
		return;
	}

	CHARRANGE originalSelection{};
	SendMessageW(m_logsEdit, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&originalSelection));
	SendMessageW(m_logsEdit, WM_SETREDRAW, FALSE, 0);
	RefreshLogsEditTheme();
	ApplyCharacterFormatToLogsRange(0, contentText.size(), m_colors.logMessageText);

	size_t lineStart = 0;
	while (lineStart < contentText.size())
	{
		size_t lineEnd = contentText.find(L"\r\n", lineStart);
		if (lineEnd == std::wstring::npos)
		{
			lineEnd = contentText.size();
		}

		std::wstring_view line(contentText.data() + lineStart, lineEnd - lineStart);
		if (!line.empty() && IsLogEntryStartLine(line))
		{
			StructuredLogLineSegments segments{};
			if (TryParseStructuredLogLine(line, segments))
			{
				ApplyCharacterFormatToLogsRange(lineStart, 23, m_colors.logTimestampText);

				const size_t levelNameStart = segments.m_levelStart + 1;
				const size_t levelNameLength = segments.m_levelLength >= 2 ? segments.m_levelLength - 2 : 0;
				ApplyCharacterFormatToLogsRange(
					lineStart + segments.m_levelStart,
					segments.m_levelLength,
					GetLogLevelColor(
						m_colors,
						line.substr(levelNameStart, levelNameLength)),
					CFE_BOLD);
				ApplyCharacterFormatToLogsRange(
					lineStart + segments.m_sourceStart,
					segments.m_sourceLength,
					m_colors.logSourceText,
					CFE_BOLD);
				ApplyCharacterFormatToLogsRange(
					lineStart + segments.m_threadStart,
					segments.m_threadLength,
					m_colors.logThreadText);
				if (segments.m_messageLength != 0)
				{
					ApplyCharacterFormatToLogsRange(
						lineStart + segments.m_messageStart,
						segments.m_messageLength,
						m_colors.logMessageText);
				}
			}
		}

		if (lineEnd == contentText.size())
		{
			break;
		}
		lineStart = lineEnd + 2;
	}

	SendMessageW(m_logsEdit, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&originalSelection));
	SendMessageW(m_logsEdit, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(m_logsEdit, nullptr, TRUE);
}

void sfh::SystemTray::Implementation::RefreshLogsEditTheme()
{
	if (m_logsEdit == nullptr)
	{
		return;
	}

	if (m_logsUsesRichEdit)
	{
		SendMessageW(m_logsEdit, EM_SETBKGNDCOLOR, 0, m_colors.logBackground);
	}
}

std::wstring sfh::SystemTray::Implementation::Utf8ToWideBestEffort(std::string_view utf8)
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

std::wstring sfh::SystemTray::Implementation::FormatFileTimeText(const FILETIME& fileTime)
{
	FILETIME localFileTime{};
	SYSTEMTIME localSystemTime{};
	if (FileTimeToLocalFileTime(&fileTime, &localFileTime) == FALSE
		|| FileTimeToSystemTime(&localFileTime, &localSystemTime) == FALSE)
	{
		return L"未知";
	}

	wchar_t buffer[64]{};
	StringCchPrintfW(
		buffer,
		std::size(buffer),
		L"%04u-%02u-%02u %02u:%02u:%02u",
		localSystemTime.wYear,
		localSystemTime.wMonth,
		localSystemTime.wDay,
		localSystemTime.wHour,
		localSystemTime.wMinute,
		localSystemTime.wSecond);
	return buffer;
}

void sfh::SystemTray::Implementation::ResolveLogsPath()
{
	const std::filesystem::path modulePath{wil::GetModuleFileNameW<wil::unique_hlocal_string>().get()};
	m_logsPath = (modulePath.parent_path() / LOG_FILE_NAME).wstring();
}

std::wstring sfh::SystemTray::Implementation::GetLogsDisplayName() const
{
	if (m_logsPath.empty())
	{
		return LOG_FILE_NAME;
	}

	const auto filename = std::filesystem::path(m_logsPath).filename().wstring();
	if (!filename.empty())
	{
		return filename;
	}

	return m_logsPath;
}

bool sfh::SystemTray::Implementation::TryGetLogFileMetadata(ULONGLONG& fileSize, FILETIME& lastWriteTime, bool& exists) const
{
	WIN32_FILE_ATTRIBUTE_DATA attributes{};
	if (GetFileAttributesExW(m_logsPath.c_str(), GetFileExInfoStandard, &attributes) == FALSE)
	{
		const auto error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
		{
			exists = false;
			fileSize = 0;
			lastWriteTime = {};
			return true;
		}
		return false;
	}

	exists = (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	if (!exists)
	{
		fileSize = 0;
		lastWriteTime = {};
		return true;
	}

	ULARGE_INTEGER size{};
	size.LowPart = attributes.nFileSizeLow;
	size.HighPart = attributes.nFileSizeHigh;
	fileSize = size.QuadPart;
	lastWriteTime = attributes.ftLastWriteTime;
	return true;
}

std::wstring sfh::SystemTray::Implementation::BuildLogsFallbackText(const std::wstring& message) const
{
	return message + L"\r\n\r\n日志文件：\r\n" + GetLogsDisplayName();
}

bool sfh::SystemTray::Implementation::IsAsciiDigit(wchar_t ch)
{
	return ch >= L'0' && ch <= L'9';
}

bool sfh::SystemTray::Implementation::IsLogEntryStartLine(std::wstring_view line)
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

std::wstring sfh::SystemTray::Implementation::FormatLogsContentForViewer(std::wstring text) const
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

std::wstring sfh::SystemTray::Implementation::TrimLogsToLastLines(std::wstring text, bool& truncatedByLines) const
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

bool sfh::SystemTray::Implementation::TryReadLogTail(std::wstring& text, bool& truncated, std::wstring& errorMessage)
{
	wil::unique_hfile file(CreateFileW(
		m_logsPath.c_str(),
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr));
	if (!file.is_valid())
	{
		errorMessage = L"读取日志失败。";
		return false;
	}

	LARGE_INTEGER fileSize{};
	if (GetFileSizeEx(file.get(), &fileSize) == FALSE)
	{
		errorMessage = L"读取日志大小失败。";
		return false;
	}

	const auto totalBytes = static_cast<ULONGLONG>((std::max)(LONGLONG{0}, fileSize.QuadPart));
	const auto bytesToRead = static_cast<DWORD>((std::min)(totalBytes, static_cast<ULONGLONG>(LOG_VIEW_MAX_BYTES)));
	LARGE_INTEGER offset{};
	offset.QuadPart = static_cast<LONGLONG>(totalBytes - bytesToRead);
	if (SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN) == FALSE)
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
			file.get(),
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

	bool truncatedByBytes = totalBytes > static_cast<ULONGLONG>(LOG_VIEW_MAX_BYTES);
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

void sfh::SystemTray::Implementation::UpdateLogsWindowText(const std::wstring& statusText, const std::wstring& contentText, bool scrollToBottom)
{
	m_logsStatusText = statusText;
	if (m_logsStatusLabel != nullptr)
	{
		SetWindowTextW(m_logsStatusLabel, statusText.c_str());
	}
	if (m_logsWindow != nullptr)
	{
		RECT clientRect{};
		GetClientRect(m_logsWindow, &clientRect);
		LayoutLogsWindowControls(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
	}
	if (m_logsEdit != nullptr)
	{
		SetWindowTextW(m_logsEdit, contentText.c_str());
		ApplyLogsRichTextFormatting(contentText);
		if (scrollToBottom)
		{
			const auto length = GetWindowTextLengthW(m_logsEdit);
			SendMessageW(m_logsEdit, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
			SendMessageW(m_logsEdit, EM_SCROLLCARET, 0, 0);
		}
	}
}

void sfh::SystemTray::Implementation::RefreshLogsWindowContent(bool forceReload)
{
	if (m_logsStatusLabel == nullptr || m_logsEdit == nullptr || m_logsPath.empty())
	{
		return;
	}

	ULONGLONG fileSize = 0;
	FILETIME lastWriteTime{};
	bool exists = false;
	if (!TryGetLogFileMetadata(fileSize, lastWriteTime, exists))
	{
		const std::wstring statusText = L"日志状态获取失败。";
		const std::wstring contentText = BuildLogsFallbackText(L"当前无法读取日志文件元数据。");
		UpdateLogsWindowText(statusText, contentText, false);
		m_logsLastLoadedText = contentText;
		m_logsLastReadFailed = true;
		m_logsHasObservedFile = false;
		m_logsLastFileSize = 0;
		m_logsLastWriteTime = {};
		return;
	}

	if (!exists)
	{
		const std::wstring statusText = L"日志文件尚未创建。";
		const std::wstring contentText = BuildLogsFallbackText(L"当前未找到日志文件。");
		if (forceReload || !m_logsHasObservedFile || m_logsLastLoadedText != contentText)
		{
			UpdateLogsWindowText(statusText, contentText, false);
		}
		m_logsLastLoadedText = contentText;
		m_logsLastReadFailed = false;
		m_logsHasObservedFile = false;
		m_logsLastFileSize = 0;
		m_logsLastWriteTime = {};
		return;
	}

	const bool metadataChanged = !m_logsHasObservedFile
		|| fileSize != m_logsLastFileSize
		|| CompareFileTime(&lastWriteTime, &m_logsLastWriteTime) != 0;
	if (!forceReload && !metadataChanged && !m_logsLastReadFailed)
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
		UpdateLogsWindowText(statusText, fallbackText, false);
		m_logsLastLoadedText = fallbackText;
		m_logsLastReadFailed = true;
		m_logsHasObservedFile = true;
		m_logsLastFileSize = fileSize;
		m_logsLastWriteTime = lastWriteTime;
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

	const bool shouldScrollToBottom = forceReload || (m_logsAutoScrollEnabled && metadataChanged);
	UpdateLogsWindowText(statusText, contentText, shouldScrollToBottom);
	m_logsLastLoadedText = contentText;
	m_logsLastReadFailed = false;
	m_logsHasObservedFile = true;
	m_logsLastFileSize = fileSize;
	m_logsLastWriteTime = lastWriteTime;
}
