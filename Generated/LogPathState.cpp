#include "pch.h"
#include "LogPathState.h"

#include <algorithm>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sddl.h>
#pragma comment(lib, "Advapi32.lib")

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
	constexpr wchar_t LOG_FILE_NAME[] = L"SubtitleFontHelper.log";
	constexpr wchar_t LOG_PATH_MAPPING_NAME_PREFIX[] = L"Local\\SubtitleFontHelperLogPath-";
	constexpr size_t MAX_LOG_PATH_LENGTH = 32768;

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
			{
				CloseHandle(m_handle);
			}
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
				{
					CloseHandle(m_handle);
				}
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

	ScopedHandle g_logPathMapping;

	class ScopedMapView
	{
	private:
		void* m_view = nullptr;

	public:
		ScopedMapView() = default;
		explicit ScopedMapView(void* view) : m_view(view) {}
		~ScopedMapView()
		{
			if (m_view != nullptr)
			{
				UnmapViewOfFile(m_view);
			}
		}

		ScopedMapView(const ScopedMapView&) = delete;
		ScopedMapView& operator=(const ScopedMapView&) = delete;

		wchar_t* get() const
		{
			return static_cast<wchar_t*>(m_view);
		}
	};

	class ScopedLocalString
	{
	private:
		wchar_t* m_value = nullptr;

	public:
		ScopedLocalString() = default;
		~ScopedLocalString()
		{
			if (m_value != nullptr)
			{
				LocalFree(m_value);
			}
		}

		ScopedLocalString(const ScopedLocalString&) = delete;
		ScopedLocalString& operator=(const ScopedLocalString&) = delete;

		wchar_t** put()
		{
			return &m_value;
		}

		const wchar_t* get() const
		{
			return m_value;
		}
	};

	std::wstring GetCurrentProcessUserSid()
	{
		const HANDLE hToken = GetCurrentProcessToken();
		PTOKEN_USER user = nullptr;
		std::vector<char> buffer;
		DWORD returnLength = 0;
		ScopedLocalString ret;
		if (GetTokenInformation(
			hToken,
			TokenUser,
			nullptr,
			0,
			&returnLength) == FALSE && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
		{
			buffer.resize(returnLength);
			user = reinterpret_cast<PTOKEN_USER>(buffer.data());
		}
		else
		{
			return {};
		}
		if (GetTokenInformation(
			hToken,
			TokenUser,
			user,
			returnLength,
			&returnLength) == FALSE)
		{
			return {};
		}
		if (ConvertSidToStringSidW(user->User.Sid, ret.put()) == FALSE)
		{
			return {};
		}
		return ret.get();
	}

	std::wstring GetLogPathMappingName()
	{
		auto sid = GetCurrentProcessUserSid();
		if (sid.empty())
		{
			return {};
		}
		return std::wstring(LOG_PATH_MAPPING_NAME_PREFIX) + sid;
	}

	std::wstring GetModuleDirectory()
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
			{
				return L".";
			}
			if (length < modulePath.size() - 1)
			{
				break;
			}
			modulePath.resize(modulePath.size() * 2);
		}
		modulePath.resize(length);
		const auto lastSlash = modulePath.find_last_of(L"\\/");
		if (lastSlash == std::wstring::npos)
		{
			return L".";
		}
		modulePath.resize(lastSlash);
		return modulePath;
	}

	std::wstring ReadLogPathFromSharedMemory(HANDLE mapping)
	{
		if (mapping == nullptr || mapping == INVALID_HANDLE_VALUE)
		{
			return {};
		}
		ScopedMapView view(MapViewOfFile(
			mapping,
			FILE_MAP_READ,
			0,
			0,
			MAX_LOG_PATH_LENGTH * sizeof(wchar_t)));
		if (view.get() == nullptr)
		{
			return {};
		}

		size_t length = 0;
		while (length < MAX_LOG_PATH_LENGTH && view.get()[length] != L'\0')
		{
			++length;
		}
		if (length == 0)
		{
			return {};
		}
		if (length == MAX_LOG_PATH_LENGTH)
		{
			return {};
		}
		return std::wstring(view.get(), length);
	}
}

std::wstring sfh::GetDefaultLogFilePath()
{
	return GetModuleDirectory() + L"\\" + LOG_FILE_NAME;
}

std::wstring sfh::GetSharedLogFilePath()
{
	const auto mappingName = GetLogPathMappingName();
	if (mappingName.empty())
	{
		return GetDefaultLogFilePath();
	}

	ScopedHandle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName.c_str()));
	const auto sharedPath = ReadLogPathFromSharedMemory(mapping.get());
	if (!sharedPath.empty())
	{
		return sharedPath;
	}
	return GetDefaultLogFilePath();
}

void sfh::SetSharedLogFilePath(const std::wstring& path)
{
	const auto mappingName = GetLogPathMappingName();
	if (mappingName.empty())
	{
		throw std::runtime_error("unable to resolve current process user sid for shared log path");
	}

	const std::wstring& effectivePath = path.empty() ? GetDefaultLogFilePath() : path;
	if (effectivePath.size() >= MAX_LOG_PATH_LENGTH)
	{
		throw std::runtime_error("log path is too long");
	}

	ScopedHandle mapping(CreateFileMappingW(
		INVALID_HANDLE_VALUE,
		nullptr,
		PAGE_READWRITE,
		0,
		static_cast<DWORD>(MAX_LOG_PATH_LENGTH * sizeof(wchar_t)),
		mappingName.c_str()));
	if (!mapping.is_valid())
	{
		throw std::runtime_error("unable to create shared log path mapping");
	}
	g_logPathMapping = std::move(mapping);

	ScopedMapView view(MapViewOfFile(
		g_logPathMapping.get(),
		FILE_MAP_WRITE,
		0,
		0,
		MAX_LOG_PATH_LENGTH * sizeof(wchar_t)));
	if (view.get() == nullptr)
	{
		throw std::runtime_error("unable to map shared log path view");
	}

	std::wmemcpy(view.get(), effectivePath.data(), effectivePath.size());
	view.get()[effectivePath.size()] = L'\0';
}
