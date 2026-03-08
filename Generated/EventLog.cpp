#include "pch.h"
#include "EventLog.h"

#include <cassert>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

constexpr size_t INITIAL_BUFFER_SIZE = 256;
constexpr uint64_t MAX_LOG_FILE_SIZE = 10ull * 1024ull * 1024ull;
constexpr int MAX_LOG_ARCHIVE_COUNT = 5;
constexpr wchar_t LOG_FILE_NAME[] = L"SubtitleFontHelper.log";
constexpr wchar_t LOG_MUTEX_NAME[] = L"Local\\SubtitleFontHelperLogMutex";

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
	std::wstring AnsiToWideString(const std::string& str)
	{
		std::wstring ret;
		const int length = MultiByteToWideChar(
			CP_ACP,
			0,
			str.c_str(),
			static_cast<int>(str.size()),
			nullptr,
			0);
		if (length == 0)
		{
			std::wostringstream oss;
			oss << L"Invalid String: " << std::hex;
			for (unsigned char ch : str)
			{
				oss << std::setw(2) << std::setfill(L'0') << static_cast<unsigned int>(ch);
			}
			return oss.str();
		}
		ret.resize(length);
		MultiByteToWideChar(
			CP_ACP,
			0,
			str.c_str(),
			static_cast<int>(str.size()),
			ret.data(),
			length);
		return ret;
	}

	std::string WideToUtf8String(const std::wstring& str)
	{
		if (str.empty())
			return {};
		const int length = WideCharToMultiByte(
			CP_UTF8,
			0,
			str.c_str(),
			static_cast<int>(str.size()),
			nullptr,
			0,
			nullptr,
			nullptr);
		if (length <= 0)
			return "<utf8 conversion failed>\r\n";

		std::string ret(length, '\0');
		if (WideCharToMultiByte(
			CP_UTF8,
			0,
			str.c_str(),
			static_cast<int>(str.size()),
			ret.data(),
			length,
			nullptr,
			nullptr) == 0)
		{
			return "<utf8 conversion failed>\r\n";
		}
		return ret;
	}

	std::wstring FormatWideString(const wchar_t* fmt, va_list args)
	{
		size_t bufferSize = INITIAL_BUFFER_SIZE;
		std::unique_ptr<wchar_t[]> buffer;
		while (true)
		{
			buffer = std::make_unique<wchar_t[]>(bufferSize);
			va_list copy;
			va_copy(copy, args);
			const int outputLength = vswprintf(buffer.get(), bufferSize, fmt, copy);
			va_end(copy);
			if (outputLength < 0 || static_cast<size_t>(outputLength) >= bufferSize)
			{
				bufferSize += bufferSize / 2;
				continue;
			}
			return buffer.get();
		}
	}

	std::wstring JoinResponsePaths(const std::vector<const wchar_t*>& responsePaths)
	{
		std::wostringstream oss;
		for (size_t i = 0; i < responsePaths.size(); ++i)
		{
			if (i != 0)
				oss << L", ";
			oss << (responsePaths[i] ? responsePaths[i] : L"NULL");
		}
		return oss.str();
	}

	class ScopedHandle
	{
	private:
		HANDLE m_handle = nullptr;

	public:
		ScopedHandle() = default;
		explicit ScopedHandle(HANDLE handle) : m_handle(handle) {}
		~ScopedHandle()
		{
			if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
				CloseHandle(m_handle);
		}

		ScopedHandle(const ScopedHandle&) = delete;
		ScopedHandle& operator=(const ScopedHandle&) = delete;

		ScopedHandle(ScopedHandle&& other) noexcept : m_handle(other.m_handle)
		{
			other.m_handle = nullptr;
		}

		ScopedHandle& operator=(ScopedHandle&& other) noexcept
		{
			if (this != &other)
			{
				if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
					CloseHandle(m_handle);
				m_handle = other.m_handle;
				other.m_handle = nullptr;
			}
			return *this;
		}

		HANDLE get() const
		{
			return m_handle;
		}

		bool is_valid() const
		{
			return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
		}
	};

	class ScopedMutexLock
	{
	private:
		HANDLE m_mutex = nullptr;
		bool m_locked = false;

	public:
		explicit ScopedMutexLock(HANDLE mutex) : m_mutex(mutex)
		{
			if (m_mutex == nullptr)
				return;
			const auto waitResult = WaitForSingleObject(m_mutex, INFINITE);
			m_locked = waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED;
		}

		~ScopedMutexLock()
		{
			if (m_locked)
				ReleaseMutex(m_mutex);
		}

		ScopedMutexLock(const ScopedMutexLock&) = delete;
		ScopedMutexLock& operator=(const ScopedMutexLock&) = delete;

		bool IsLocked() const
		{
			return m_locked;
		}
	};

	class FileLogger
	{
	private:
		std::wstring m_logPath;
		ScopedHandle m_mutex;

		static std::wstring GetModuleDirectory()
		{
			std::wstring modulePath(MAX_PATH, L'\0');
			DWORD length = 0;
			while (true)
			{
				length = GetModuleFileNameW(
					reinterpret_cast<HMODULE>(&__ImageBase),
					modulePath.data(),
					static_cast<DWORD>(modulePath.size()));
				if (length == 0)
					return L".";
				if (length < modulePath.size() - 1)
					break;
				modulePath.resize(modulePath.size() * 2);
			}
			modulePath.resize(length);
			const auto lastSlash = modulePath.find_last_of(L"\\/");
			if (lastSlash == std::wstring::npos)
				return L".";
			modulePath.resize(lastSlash);
			return modulePath;
		}

		std::wstring GetArchivePath(int index) const
		{
			return m_logPath + L"." + std::to_wstring(index);
		}

		static bool FileExists(const std::wstring& path)
		{
			const auto attributes = GetFileAttributesW(path.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
		}

		void RotateIfNeeded(size_t incomingBytes)
		{
			ScopedHandle file(CreateFileW(
				m_logPath.c_str(),
				FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr));
			if (!file.is_valid())
				return;

			LARGE_INTEGER fileSize{};
			if (GetFileSizeEx(file.get(), &fileSize) == FALSE)
				return;
			if (static_cast<uint64_t>(fileSize.QuadPart) + incomingBytes < MAX_LOG_FILE_SIZE)
				return;

			DeleteFileW(GetArchivePath(MAX_LOG_ARCHIVE_COUNT).c_str());
			for (int index = MAX_LOG_ARCHIVE_COUNT - 1; index >= 1; --index)
			{
				const auto sourcePath = GetArchivePath(index);
				if (FileExists(sourcePath))
				{
					MoveFileExW(
						sourcePath.c_str(),
						GetArchivePath(index + 1).c_str(),
						MOVEFILE_REPLACE_EXISTING);
				}
			}
			if (FileExists(m_logPath))
			{
				MoveFileExW(
					m_logPath.c_str(),
					GetArchivePath(1).c_str(),
					MOVEFILE_REPLACE_EXISTING);
			}
		}

	public:
		FileLogger()
			: m_logPath(GetModuleDirectory() + L"\\" + std::wstring(LOG_FILE_NAME)),
			  m_mutex(CreateMutexW(nullptr, FALSE, LOG_MUTEX_NAME))
		{
		}

		void WriteLine(std::wstring_view level, std::wstring_view source, const std::wstring& message)
		{
			SYSTEMTIME localTime;
			GetLocalTime(&localTime);

			std::wostringstream oss;
			oss << std::setfill(L'0')
				<< std::setw(4) << localTime.wYear << L'-'
				<< std::setw(2) << localTime.wMonth << L'-'
				<< std::setw(2) << localTime.wDay << L' '
				<< std::setw(2) << localTime.wHour << L':'
				<< std::setw(2) << localTime.wMinute << L':'
				<< std::setw(2) << localTime.wSecond << L'.'
				<< std::setw(3) << localTime.wMilliseconds
				<< L" [" << level << L"]"
				<< L" [" << source << L"]"
				<< L" [pid=" << GetCurrentProcessId() << L":tid=" << GetCurrentThreadId() << L"] "
				<< message << L"\r\n";

			auto utf8 = WideToUtf8String(oss.str());
			ScopedMutexLock lock(m_mutex.get());
			if (m_mutex.is_valid() && !lock.IsLocked())
				return;

			RotateIfNeeded(utf8.size());

			ScopedHandle file(CreateFileW(
				m_logPath.c_str(),
				FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr));
			if (!file.is_valid())
				return;

			size_t totalWritten = 0;
			while (totalWritten < utf8.size())
			{
				DWORD written = 0;
				const auto remaining = utf8.size() - totalWritten;
				const auto chunkSize = static_cast<DWORD>(
					remaining > static_cast<size_t>(MAXDWORD) ? MAXDWORD : remaining);
				if (WriteFile(
					file.get(),
					utf8.data() + totalWritten,
					chunkSize,
					&written,
					nullptr) == FALSE || written == 0)
				{
					return;
				}
				totalWritten += written;
			}
		}
	};

	FileLogger& GetFileLogger()
	{
		static FileLogger logger;
		return logger;
	}

	void WriteEventLogLine(const wchar_t* level, const wchar_t* source, const std::wstring& message)
	{
		GetFileLogger().WriteLine(level, source, message);
	}
}

sfh::EventLog::EventLog() = default;

sfh::EventLog::~EventLog() = default;

sfh::EventLog& sfh::EventLog::GetInstance()
{
	static EventLog instance;
	return instance;
}

void sfh::EventLog::LogDllAttach(uint32_t processId)
{
	WriteEventLogLine(L"INFO", L"dll", L"DllAttach processId=" + std::to_wstring(processId));
}

void sfh::EventLog::LogDllQuerySuccess(uint32_t processId, uint32_t threadId, const wchar_t* requestName,
	const std::vector<const wchar_t*> responsePaths)
{
	std::wostringstream oss;
	oss << L"QuerySuccess processId=" << processId
		<< L" threadId=" << threadId
		<< L" request=\"" << (requestName ? requestName : L"NULL") << L"\""
		<< L" paths=[" << JoinResponsePaths(responsePaths) << L"]";
	WriteEventLogLine(L"INFO", L"dll", oss.str());
}

void sfh::EventLog::LogDllQueryFailure(uint32_t processId, uint32_t threadId, const wchar_t* requestName,
	const wchar_t* reason)
{
	std::wostringstream oss;
	oss << L"QueryFailure processId=" << processId
		<< L" threadId=" << threadId
		<< L" request=\"" << (requestName ? requestName : L"NULL") << L"\""
		<< L" reason=\"" << (reason ? reason : L"NULL") << L"\"";
	WriteEventLogLine(L"ERROR", L"dll", oss.str());
}

void sfh::EventLog::LogDaemonTryAttach(uint32_t processId, const wchar_t* processName,
	const wchar_t* processArchitecture)
{
	std::wostringstream oss;
	oss << L"TryAttach processId=" << processId
		<< L" process=\"" << (processName ? processName : L"NULL") << L"\""
		<< L" architecture=\"" << (processArchitecture ? processArchitecture : L"NULL") << L"\"";
	WriteEventLogLine(L"INFO", L"daemon", oss.str());
}

void sfh::EventLog::LogDaemonBumpVersion(uint32_t oldVersion, uint32_t newVersion)
{
	WriteEventLogLine(
		L"INFO",
		L"daemon",
		L"BumpVersion old=" + std::to_wstring(oldVersion) + L" new=" + std::to_wstring(newVersion));
}

void sfh::EventLog::LogDllInjectProcessSuccess(uint32_t processId)
{
	WriteEventLogLine(L"INFO", L"dll", L"InjectProcessSuccess processId=" + std::to_wstring(processId));
}

void sfh::EventLog::LogDllInjectProcessFailure(uint32_t processId, const wchar_t* reason)
{
	std::wostringstream oss;
	oss << L"InjectProcessFailure processId=" << processId
		<< L" reason=\"" << (reason ? reason : L"NULL") << L"\"";
	WriteEventLogLine(L"ERROR", L"dll", oss.str());
}

void sfh::EventLog::LogDllQueryNoResult(uint32_t processId, uint32_t threadId, const wchar_t* requestName)
{
	std::wostringstream oss;
	oss << L"QueryNoResult processId=" << processId
		<< L" threadId=" << threadId
		<< L" request=\"" << (requestName ? requestName : L"NULL") << L"\"";
	WriteEventLogLine(L"INFO", L"dll", oss.str());
}

void sfh::EventLog::LogDllLoadFont(uint32_t processId, uint32_t threadId, const wchar_t* path)
{
	std::wostringstream oss;
	oss << L"LoadFont processId=" << processId
		<< L" threadId=" << threadId
		<< L" path=\"" << (path ? path : L"NULL") << L"\"";
	WriteEventLogLine(L"INFO", L"dll", oss.str());
}

void sfh::EventLog::LogDebugMessageSingle(const wchar_t* str)
{
	WriteEventLogLine(L"DEBUG", L"debug", str ? str : L"NULL");
}

void sfh::EventLog::LogDebugMessage(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	size_t bufferSize = INITIAL_BUFFER_SIZE;
	std::unique_ptr<char[]> buffer;
	while (true)
	{
		buffer = std::make_unique<char[]>(bufferSize);
		va_list copy;
		va_copy(copy, args);
		const int outputLength = vsnprintf(buffer.get(), bufferSize, fmt, copy);
		va_end(copy);
		if (outputLength < 0 || static_cast<size_t>(outputLength) >= bufferSize)
		{
			bufferSize += bufferSize / 2;
			continue;
		}
		break;
	}
	va_end(args);
	LogDebugMessageSingle(AnsiToWideString(buffer.get()).c_str());
}

void sfh::EventLog::LogDebugMessage(const wchar_t* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	auto message = FormatWideString(fmt, args);
	va_end(args);
	LogDebugMessageSingle(message.c_str());
}
