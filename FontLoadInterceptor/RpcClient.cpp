#include "pch.h"

#include "RpcClient.h"

#include <mutex>
#include <cstring>
#include <string>
#include <cwchar>
#include <sddl.h>
#include <unordered_set>
#include <sstream>

#include <wil/resource.h>

#undef max

#include "EventLog.h"
#include "Detour.h"

#include "FontQuery.pb.h"

namespace sfh
{
	std::wstring GetCurrentProcessUserSid()
	{
		auto hToken = GetCurrentProcessToken();
		PTOKEN_USER user;
		std::unique_ptr<char[]> buffer;
		DWORD returnLength;
		wil::unique_hlocal_string ret;
		if (GetTokenInformation(
			hToken,
			TokenUser,
			nullptr,
			0,
			&returnLength) == FALSE && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
		{
			buffer = std::make_unique<char[]>(returnLength);
			user = reinterpret_cast<PTOKEN_USER>(buffer.get());
		}
		else
		{
			MarkUnreachable();
		}
		THROW_LAST_ERROR_IF(GetTokenInformation(
			hToken,
			TokenUser,
			user,
			returnLength,
			&returnLength) == FALSE);
		THROW_LAST_ERROR_IF(ConvertSidToStringSidW(user->User.Sid, ret.put()) == FALSE);
		return ret.get();
	}

	class QueryCache
	{
	private:
		wil::unique_handle m_version;
		wil::unique_mapview_ptr<uint32_t> m_versionMem;
		uint32_t m_lastKnownVersion = std::numeric_limits<uint32_t>::max();
		bool m_good = false;

		std::unordered_set<std::wstring> m_cache;
		std::mutex m_lock;

		QueryCache()
		{
			try
			{
				std::wstring versionShmName = L"SubtitleFontAutoLoaderSHM-";
				versionShmName += GetCurrentProcessUserSid();
				m_version.reset(CreateFileMappingW(
					INVALID_HANDLE_VALUE,
					nullptr,
					PAGE_READWRITE,
					0, 4,
					versionShmName.c_str()));
				THROW_LAST_ERROR_IF(!m_version.is_valid());
				m_versionMem.reset(static_cast<uint32_t*>(MapViewOfFile(
					m_version.get(),
					FILE_MAP_WRITE,
					0, 0,
					sizeof(uint32_t))));
				THROW_LAST_ERROR_IF(m_versionMem.get() == nullptr);
				m_good = true;
			}
			catch (...)
			{
			}
		}

	public:
		static QueryCache& GetInstance()
		{
			static QueryCache instance;
			return instance;
		}

		void CheckNewVersion()
		{
			if (!m_versionMem)
			{
				return;
			}

			uint32_t newVersion = InterlockedCompareExchange(m_versionMem.get(), 0, 0);
			if (newVersion != m_lastKnownVersion)
			{
				m_lastKnownVersion = newVersion;
				m_cache.clear();
			}
		}

		bool IsQueryNeeded(const wchar_t* str)
		{
			if (!m_good)return true;
			std::lock_guard lg(m_lock);
			CheckNewVersion();
			if (m_cache.find(str) != m_cache.end())
				return false;
			return true;
		}

		void AddToCache(const wchar_t* str)
		{
			if (!m_good)return;
			std::lock_guard lg(m_lock);
			CheckNewVersion();
			m_cache.emplace(str);
		}
	};

	constexpr DWORD PIPE_OPERATION_TIMEOUT_MS = 5000;

	DWORD ReadWritePipe(
		wil::unique_hfile& pipe,
		void* buffer,
		DWORD size,
		bool isWrite)
	{
		OVERLAPPED overlapped{};
		wil::unique_handle ioEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		THROW_LAST_ERROR_IF(!ioEvent.is_valid());
		overlapped.hEvent = ioEvent.get();

		DWORD transferredBytes = 0;
		const BOOL result = isWrite
			? WriteFile(pipe.get(), buffer, size, &transferredBytes, &overlapped)
			: ReadFile(pipe.get(), buffer, size, &transferredBytes, &overlapped);
		if (result == FALSE)
		{
			const auto error = GetLastError();
			if (error != ERROR_IO_PENDING)
			{
				THROW_WIN32(error);
			}

			const auto waitResult = WaitForSingleObject(ioEvent.get(), PIPE_OPERATION_TIMEOUT_MS);
			if (waitResult == WAIT_TIMEOUT)
			{
				if (CancelIoEx(pipe.get(), &overlapped) == FALSE)
				{
					const auto cancelError = GetLastError();
					if (cancelError != ERROR_NOT_FOUND)
					{
						// Continue waiting below so the stack-based OVERLAPPED cannot outlive this scope.
					}
				}

				const auto cancelWaitResult = WaitForSingleObject(ioEvent.get(), PIPE_OPERATION_TIMEOUT_MS);
				if (cancelWaitResult == WAIT_TIMEOUT)
				{
					pipe.reset();
					const auto closeWaitResult = WaitForSingleObject(ioEvent.get(), INFINITE);
					THROW_LAST_ERROR_IF(closeWaitResult == WAIT_FAILED);
					throw std::runtime_error(isWrite ? "pipe write timed out" : "pipe read timed out");
				}
				THROW_LAST_ERROR_IF(cancelWaitResult == WAIT_FAILED);
				if (GetOverlappedResult(pipe.get(), &overlapped, &transferredBytes, FALSE) == FALSE)
				{
					const auto completeError = GetLastError();
					if (completeError != ERROR_OPERATION_ABORTED)
					{
						THROW_WIN32(completeError);
					}
				}
				throw std::runtime_error(isWrite ? "pipe write timed out" : "pipe read timed out");
			}
			THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
			THROW_LAST_ERROR_IF(GetOverlappedResult(pipe.get(), &overlapped, &transferredBytes, FALSE) == FALSE);
		}

		return transferredBytes;
	}

	void WritePipe(wil::unique_hfile& pipe, const void* src, DWORD size)
	{
		DWORD writeBytes = ReadWritePipe(pipe, const_cast<void*>(src), size, true);
		if (writeBytes != size)
			throw std::runtime_error("can't write much data");
	}

	void ReadPipe(wil::unique_hfile& pipe, void* dst, DWORD size)
	{
		DWORD readBytes = ReadWritePipe(pipe, dst, size, false);
		if (readBytes != size)
			throw std::runtime_error("can't read enough data");
	}

	std::wstring AnsiStringToWideString(const char* str)
	{
		std::wstring ret;
		const int length = MultiByteToWideChar(
			CP_ACP,
			MB_ERR_INVALID_CHARS,
			str,
			-1,
			nullptr,
			0);
		if (length <= 0)
			throw std::runtime_error("MultiByteToWideChar failed");
		ret.resize(length);
		MultiByteToWideChar(
			CP_ACP,
			MB_ERR_INVALID_CHARS,
			str,
			-1,
			ret.data(),
			length);
		while (!ret.empty() && ret.back() == 0)
			ret.pop_back();
		return ret;
	}

	std::string WideToUtf8String(const std::wstring& wStr)
	{
		std::string ret;
		const int length = WideCharToMultiByte(
			CP_UTF8,
			WC_ERR_INVALID_CHARS,
			wStr.c_str(),
			static_cast<int>(wStr.size()),
			nullptr,
			0,
			nullptr,
			nullptr);
		if (length <= 0)
			throw std::runtime_error("WideCharToMultiByte failed");
		ret.resize(length);
		WideCharToMultiByte(
			CP_UTF8,
			WC_ERR_INVALID_CHARS,
			wStr.c_str(),
			static_cast<int>(wStr.size()),
			ret.data(),
			length,
			nullptr,
			nullptr);
		return ret;
	}

	std::wstring Utf8ToWideString(const std::string& str)
	{
		std::wstring ret;
		const int length = MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			str.c_str(),
			static_cast<int>(str.size()),
			nullptr,
			0);
		if (length <= 0)
			throw std::runtime_error("MultiByteToWideChar failed");
		ret.resize(length);
		MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			str.c_str(),
			static_cast<int>(str.size()),
			ret.data(),
			length);
		return ret;
	}

	wil::unique_hfile OpenPipe()
	{
		std::wstring pipeName = LR"_(\\.\pipe\SubtitleFontAutoLoaderRpc-)_";
		pipeName += GetCurrentProcessUserSid();
		wil::unique_hfile pipe(CreateFileW(
			pipeName.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_OVERLAPPED,
			nullptr));
		if (!pipe.is_valid())
		{
			if (GetLastError() == ERROR_PIPE_BUSY)
			{
				// wait previous request finish
				THROW_LAST_ERROR_IF(WaitNamedPipeW(pipeName.c_str(), PIPE_OPERATION_TIMEOUT_MS) == FALSE);
				pipe.reset(CreateFileW(
					pipeName.c_str(),
					GENERIC_READ | GENERIC_WRITE,
					0,
					nullptr,
					OPEN_EXISTING,
					FILE_FLAG_OVERLAPPED,
					nullptr));
			}
			THROW_LAST_ERROR_IF(!pipe.is_valid());
		}

		return pipe;
	}

	class PipeClientPool
	{
	private:
		std::vector<wil::unique_hfile> m_pool;
		std::mutex m_mutex;

		PipeClientPool()
		{
		}

	public:
		static PipeClientPool& GetInstance()
		{
			static PipeClientPool instance;
			return instance;
		}

		wil::unique_hfile GetOne()
		{
			std::lock_guard lg(m_mutex);
			if (m_pool.empty())
			{
				m_pool.emplace_back(OpenPipe());
			}
			auto ret = std::move(m_pool.back());
			m_pool.pop_back();
			return ret;
		}

		void PutOne(wil::unique_hfile&& pipe)
		{
			std::lock_guard lg(m_mutex);
			m_pool.emplace_back(std::move(pipe));
		}

		void Invalidate()
		{
			std::lock_guard lg(m_mutex);
			m_pool.clear();
		}
	};

	void SendRequst(wil::unique_hfile& pipe, const FontQueryRequest& request)
	{
		std::ostringstream oss;
		request.SerializeToOstream(&oss);
		std::string requestBuffer = std::move(oss).str();

		auto requestLength = static_cast<uint32_t>(requestBuffer.size());
		WritePipe(pipe, &requestLength, sizeof(uint32_t));
		WritePipe(pipe, requestBuffer.data(), static_cast<DWORD>(requestLength));
	}

	template <typename ReturnType>
	ReturnType FetchResponse(wil::unique_hfile& pipe)
	{
		uint32_t responseLength;
		ReadPipe(pipe, &responseLength, sizeof(uint32_t));
		std::vector<char> responseBuffer(responseLength);
		ReadPipe(pipe, responseBuffer.data(), responseLength);

		ReturnType response;
		if (!response.ParseFromArray(responseBuffer.data(), responseLength))
			throw std::runtime_error("bad response");
		return response;
	}

	template <>
	void FetchResponse(wil::unique_hfile&)
	{
	}

	template <typename ReturnType>
	ReturnType MakeRequest(const FontQueryRequest& request)
	{
		bool shouldPut = true;
		auto pipe = PipeClientPool::GetInstance().GetOne();
		auto _ = wil::scope_exit([&]()
		{
			if (shouldPut)
				PipeClientPool::GetInstance().PutOne(std::move(pipe));
		});

		try
		{
			// send
			SendRequst(pipe, request);

			// recv
			return FetchResponse<ReturnType>(pipe);
		}
		catch (...)
		{
			shouldPut = false;
			PipeClientPool::GetInstance().Invalidate();
			throw;
		}
	}

	FontQueryResponse QueryFont(const wchar_t* str)
	{
		FontQueryRequest request;
		request.set_version(1);
		request.set_querystring(WideToUtf8String(str));

		return MakeRequest<FontQueryResponse>(request);
	}

	void SendFeedback(FontLoadFeedback& feedback)
	{
		FontQueryRequest request;
		request.set_version(1);
		request.set_allocated_feedbackdata(&feedback);

		MakeRequest<void>(request);

		auto _ = request.release_feedbackdata();
	}

	void SendFeedbackAsync(FontLoadFeedback&& feedback)
	{
		std::thread([fb = std::move(feedback)]()mutable
		{
			SendFeedback(fb);
		}).detach();
	}

	static bool MatchesAnyKnownFaceName(const FontFace& face, const std::string& faceName)
	{
		return std::ranges::find(face.familyname(), faceName) != face.familyname().end()
			|| std::ranges::find(face.postscriptname(), faceName) != face.postscriptname().end()
			|| std::ranges::find(face.gdifullname(), faceName) != face.gdifullname().end();
	}

	void TryLoad(const wchar_t* query, const FontQueryResponse& response)
	{
		struct EnumInfo
		{
			const FontQueryResponse* response;
			std::vector<char> maskedFace;
			bool hasSystemMatch = false;
		};

		wil::unique_hdc_window hDC = wil::GetWindowDC(HWND_DESKTOP);
		LOGFONTW lf{};
		if (query && wcsnlen(query, LF_FACESIZE) < LF_FACESIZE)
		{
			wcscpy_s(lf.lfFaceName, LF_FACESIZE, query);
		}

		EnumInfo enumInfo;
		enumInfo.response = &response;
		enumInfo.maskedFace.assign(response.fonts_size(), 0);

		if (lf.lfFaceName[0] != 0)
		{
			Detour::Original::EnumFontFamiliesExW(
			hDC.get(), &lf, [](const LOGFONT* lpelfe, const TEXTMETRIC* lpntme, DWORD dwFontType, LPARAM lParam)-> int
			{
				EnumInfo& info = *reinterpret_cast<EnumInfo*>(lParam);
				info.hasSystemMatch = true;
				auto faceName = WideToUtf8String(lpelfe->lfFaceName);
				for (int i = 0; i < info.response->fonts_size(); ++i)
				{
					if (info.maskedFace[i])continue;
					auto& face = info.response->fonts()[i];
					if (MatchesAnyKnownFaceName(face, faceName)
						&& (!!face.oblique() == !!lpelfe->lfItalic && face.weight() == lpelfe->lfWeight)
					)
					{
						info.maskedFace[i] = 1;
					}
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&enumInfo), 0);
		}

		FontLoadFeedback feedback;
		bool hasFeedback = false;

		for (int i = 0; i < response.fonts_size(); ++i)
		{
			if (enumInfo.maskedFace[i])continue;
			auto path = Utf8ToWideString(response.fonts()[i].path());
			feedback.add_path(response.fonts()[i].path());
			hasFeedback = true;

			AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr);

			EventLog::GetInstance().LogDllLoadFont(GetCurrentProcessId(), GetCurrentThreadId(), path.c_str());
		}

		if (response.fonts_size() == 0 && !enumInfo.hasSystemMatch && query != nullptr && *query != L'\0')
		{
			feedback.set_missingquery(WideToUtf8String(query));
			hasFeedback = true;
		}

		if (hasFeedback)
		{
			SendFeedbackAsync(std::move(feedback));
		}
	}

	void QueryAndLoad(const wchar_t* query)
	{
		try
		{
			if (query == nullptr)
				return;
			// strip GDI added prefix '@'
			if (*query == L'@')
				++query;
			// skip empty string
			if (*query == L'\0')
				return;
			if (!QueryCache::GetInstance().IsQueryNeeded(query))
				return;
			auto response = QueryFont(query);

			std::vector<std::wstring> paths;
			for (int i = 0; i < response.fonts_size(); ++i)
			{
				auto& font = response.fonts()[i];
				auto path = Utf8ToWideString(font.path());
				paths.emplace_back(std::move(path));
			}
			QueryCache::GetInstance().AddToCache(query);
			std::vector<const wchar_t*> logData;
			for (auto& s : paths)
			{
				logData.push_back(s.c_str());
			}
			if (logData.empty())
			{
				EventLog::GetInstance().LogDllQueryNoResult(GetCurrentProcessId(), GetCurrentThreadId(), query);
			}
			else
			{
				EventLog::GetInstance().LogDllQuerySuccess(GetCurrentProcessId(), GetCurrentThreadId(), query, logData);
			}

			TryLoad(query, response);
		}
		catch (std::exception& e)
		{
			EventLog::GetInstance().LogDllQueryFailure(GetCurrentProcessId(), GetCurrentThreadId(), query,
			                                           AnsiStringToWideString(e.what()).c_str());
			// ignore exceptions
		}
	}

	void QueryAndLoad(const char* query)
	{
		if (query == nullptr)
			return;
		if (*query == '\0')
			return;
		try
		{
			auto wstr = AnsiStringToWideString(query);
			QueryAndLoad(wstr.c_str());
		}
		catch (...)
		{
		}
	}
}
