#include "FontUiClient.h"

#include "FontQuery.pb.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <wil/resource.h>
#include <wil/win32_helpers.h>

namespace
{
	using namespace sfh::ui;

	constexpr DWORD PIPE_OPERATION_TIMEOUT_MS = 5000;
	constexpr DWORD PIPE_CONNECT_POLL_INTERVAL_MS = 100;

	std::wstring GetCurrentProcessUserSid()
	{
		auto hToken = GetCurrentProcessToken();
		PTOKEN_USER user = nullptr;
		std::unique_ptr<char[]> buffer;
		DWORD returnLength = 0;
		wil::unique_hlocal_string sidText;
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
			throw std::runtime_error("failed to query token size");
		}

		if (GetTokenInformation(
			hToken,
			TokenUser,
			user,
			returnLength,
			&returnLength) == FALSE)
		{
			THROW_LAST_ERROR();
		}
		if (ConvertSidToStringSidW(user->User.Sid, sidText.put()) == FALSE)
		{
			THROW_LAST_ERROR();
		}
		return sidText.get();
	}

	std::string WideToUtf8String(std::wstring_view text)
	{
		if (text.empty())
		{
			return {};
		}

		const int length = WideCharToMultiByte(
			CP_UTF8,
			WC_ERR_INVALID_CHARS,
			text.data(),
			static_cast<int>(text.size()),
			nullptr,
			0,
			nullptr,
			nullptr);
		if (length <= 0)
		{
			throw std::runtime_error("WideCharToMultiByte failed");
		}

		std::string result(static_cast<size_t>(length), '\0');
		if (WideCharToMultiByte(
			CP_UTF8,
			WC_ERR_INVALID_CHARS,
			text.data(),
			static_cast<int>(text.size()),
			result.data(),
			length,
			nullptr,
			nullptr) <= 0)
		{
			throw std::runtime_error("WideCharToMultiByte failed");
		}
		return result;
	}

	std::wstring Utf8ToWideString(std::string_view text)
	{
		if (text.empty())
		{
			return {};
		}

		const int length = MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			text.data(),
			static_cast<int>(text.size()),
			nullptr,
			0);
		if (length <= 0)
		{
			throw std::runtime_error("MultiByteToWideChar failed");
		}

		std::wstring result(static_cast<size_t>(length), L'\0');
		if (MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			text.data(),
			static_cast<int>(text.size()),
			result.data(),
			length) <= 0)
		{
			throw std::runtime_error("MultiByteToWideChar failed");
		}
		return result;
	}

	void CancelPipeIo(HANDLE pipeHandle, OVERLAPPED& overlapped)
	{
		if (pipeHandle == INVALID_HANDLE_VALUE || pipeHandle == nullptr)
		{
			return;
		}

		if (CancelIoEx(pipeHandle, &overlapped) == FALSE)
		{
			const auto cancelError = GetLastError();
			if (cancelError != ERROR_NOT_FOUND)
			{
				THROW_WIN32(cancelError);
			}
		}
	}

	DWORD ReadWritePipe(wil::unique_hfile& pipe, void* buffer, DWORD size, bool isWrite, HANDLE shutdownEvent)
	{
		OVERLAPPED overlapped{};
		wil::unique_handle ioEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		THROW_LAST_ERROR_IF(!ioEvent.is_valid());
		overlapped.hEvent = ioEvent.get();

		DWORD transferredBytes = 0;
		const BOOL result = isWrite
			? WriteFile(pipe.get(), buffer, size, &transferredBytes, &overlapped)
			: ReadFile(pipe.get(), buffer, size, &transferredBytes, &overlapped);
		if (result != FALSE)
		{
			return transferredBytes;
		}

		const auto error = GetLastError();
		if (error != ERROR_IO_PENDING)
		{
			THROW_WIN32(error);
		}

		HANDLE waitHandles[] = {
			ioEvent.get(),
			shutdownEvent,
		};
		const auto waitResult = WaitForMultipleObjects(
			_countof(waitHandles),
			waitHandles,
			FALSE,
			PIPE_OPERATION_TIMEOUT_MS);
		if (waitResult == WAIT_OBJECT_0 + 1)
		{
			CancelPipeIo(pipe.get(), overlapped);
			THROW_LAST_ERROR_IF(WaitForSingleObject(ioEvent.get(), INFINITE) == WAIT_FAILED);
			if (GetOverlappedResult(pipe.get(), &overlapped, &transferredBytes, FALSE) == FALSE)
			{
				const auto completionError = GetLastError();
				if (completionError != ERROR_OPERATION_ABORTED)
				{
					THROW_WIN32(completionError);
				}
			}
			throw std::runtime_error("pipe operation canceled");
		}
		if (waitResult == WAIT_TIMEOUT)
		{
			CancelPipeIo(pipe.get(), overlapped);

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
				const auto completionError = GetLastError();
				if (completionError != ERROR_OPERATION_ABORTED)
				{
					THROW_WIN32(completionError);
				}
			}
			throw std::runtime_error(isWrite ? "pipe write timed out" : "pipe read timed out");
		}

		THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
		THROW_LAST_ERROR_IF(GetOverlappedResult(pipe.get(), &overlapped, &transferredBytes, FALSE) == FALSE);
		return transferredBytes;
	}

	void WritePipe(wil::unique_hfile& pipe, const void* src, DWORD size, HANDLE shutdownEvent)
	{
		DWORD written = ReadWritePipe(pipe, const_cast<void*>(src), size, true, shutdownEvent);
		if (written != size)
		{
			throw std::runtime_error("failed to write full pipe payload");
		}
	}

	void ReadPipe(wil::unique_hfile& pipe, void* dst, DWORD size, HANDLE shutdownEvent)
	{
		DWORD read = ReadWritePipe(pipe, dst, size, false, shutdownEvent);
		if (read != size)
		{
			throw std::runtime_error("failed to read full pipe payload");
		}
	}

	wil::unique_hfile OpenPipe(std::wstring_view pipeName, HANDLE shutdownEvent)
	{
		const std::wstring resolvedPipeName(pipeName);
		const auto deadline = GetTickCount64() + PIPE_OPERATION_TIMEOUT_MS;
		while (true)
		{
			if (shutdownEvent != nullptr
				&& WaitForSingleObject(shutdownEvent, 0) == WAIT_OBJECT_0)
			{
				throw std::runtime_error("pipe operation canceled");
			}

			wil::unique_hfile pipe(CreateFileW(
				resolvedPipeName.c_str(),
				GENERIC_READ | GENERIC_WRITE,
				0,
				nullptr,
				OPEN_EXISTING,
				FILE_FLAG_OVERLAPPED,
				nullptr));
			if (pipe.is_valid())
			{
				return pipe;
			}

			const auto error = GetLastError();
			if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)
			{
				THROW_WIN32(error);
			}

			if (GetTickCount64() >= deadline)
			{
				THROW_WIN32(error);
			}

			const DWORD waitMs = static_cast<DWORD>(std::min<ULONGLONG>(
				PIPE_CONNECT_POLL_INTERVAL_MS,
				deadline - GetTickCount64()));
			if (error == ERROR_PIPE_BUSY)
			{
				if (WaitNamedPipeW(resolvedPipeName.c_str(), waitMs) == FALSE)
				{
					const auto waitError = GetLastError();
					if (waitError != ERROR_SEM_TIMEOUT && waitError != ERROR_FILE_NOT_FOUND)
					{
						THROW_WIN32(waitError);
					}
				}
			}
			else
			{
				THROW_LAST_ERROR_IF(WaitForSingleObject(shutdownEvent, waitMs) == WAIT_FAILED);
			}
		}
	}

	void SendRequest(wil::unique_hfile& pipe, const sfh::FontQueryRequest& request, HANDLE shutdownEvent)
	{
		std::string requestBuffer;
		if (!request.SerializeToString(&requestBuffer))
		{
			throw std::runtime_error("failed to serialize request");
		}

		const auto requestLength = static_cast<uint32_t>(requestBuffer.size());
		WritePipe(pipe, &requestLength, sizeof(requestLength), shutdownEvent);
		if (!requestBuffer.empty())
		{
			WritePipe(pipe, requestBuffer.data(), requestLength, shutdownEvent);
		}
	}

	sfh::FontQueryResponse ReceiveResponse(wil::unique_hfile& pipe, HANDLE shutdownEvent)
	{
		uint32_t responseLength = 0;
		ReadPipe(pipe, &responseLength, sizeof(responseLength), shutdownEvent);
		std::vector<char> responseBuffer(static_cast<size_t>(responseLength));
		if (responseLength != 0)
		{
			ReadPipe(pipe, responseBuffer.data(), responseLength, shutdownEvent);
		}

		sfh::FontQueryResponse response;
		if (!response.ParseFromArray(responseBuffer.data(), responseLength))
		{
			throw std::runtime_error("failed to parse response");
		}
		return response;
	}

	FontUiSnapshotData ConvertSnapshot(const sfh::FontUiSnapshotMessage& message)
	{
		FontUiSnapshotData snapshot;
		snapshot.m_isLoaded = message.isloaded();
		snapshot.m_hasStaleData = message.hasstaledata();
		snapshot.m_statusMessage = Utf8ToWideString(message.statusmessage());
		snapshot.m_totalSearchResultCount = message.totalsearchresultcount();
		snapshot.m_isSearchResultTruncated = message.issearchresulttruncated();

		snapshot.m_indexSummaries.reserve(static_cast<size_t>(message.indexsummaries_size()));
		for (const auto& item : message.indexsummaries())
		{
			FontUiIndexSummaryData summary;
			summary.m_indexPath = Utf8ToWideString(item.indexpath());
			summary.m_fontFileCount = item.fontfilecount();
			summary.m_fontNameCount = item.fontnamecount();
			summary.m_fontNamesSummary = Utf8ToWideString(item.fontnamessummary());
			snapshot.m_indexSummaries.push_back(std::move(summary));
		}

		snapshot.m_searchResults.reserve(static_cast<size_t>(message.searchresults_size()));
		for (const auto& item : message.searchresults())
		{
			FontUiSearchResultData result;
			result.m_displayName = Utf8ToWideString(item.displayname());
			result.m_familyNames = Utf8ToWideString(item.familynames());
			result.m_fullNames = Utf8ToWideString(item.fullnames());
			result.m_postScriptNames = Utf8ToWideString(item.postscriptnames());
			result.m_indexPath = Utf8ToWideString(item.indexpath());
			result.m_fontPath = Utf8ToWideString(item.fontpath());
			result.m_faceIndex = item.faceindex();
			snapshot.m_searchResults.push_back(std::move(result));
		}

		return snapshot;
	}
}

class sfh::ui::FontUiClient::Implementation
{
public:
	explicit Implementation(std::wstring pipeName)
		: m_pipeName(std::move(pipeName))
	{
		if (m_pipeName.empty())
		{
			m_pipeName = FontUiClient::BuildDefaultPipeName();
		}
		m_shutdownEvent.create(wil::EventOptions::ManualReset);
	}

	wil::unique_hfile& EnsurePipeLocked()
	{
		if (!m_pipe.is_valid())
		{
			m_pipe = OpenPipe(m_pipeName, m_shutdownEvent.get());
			m_activePipeHandle.store(m_pipe.get(), std::memory_order_release);
		}
		return m_pipe;
	}

	void ResetPipeLocked()
	{
		m_activePipeHandle.store(INVALID_HANDLE_VALUE, std::memory_order_release);
		m_pipe.reset();
	}

	std::wstring m_pipeName;
	std::atomic<HANDLE> m_activePipeHandle = INVALID_HANDLE_VALUE;
	std::mutex m_pipeMutex;
	wil::unique_hfile m_pipe;
	wil::unique_event m_shutdownEvent;
};

sfh::ui::FontUiClient::FontUiClient(std::wstring pipeName)
	: m_impl(std::make_unique<Implementation>(std::move(pipeName)))
{
}

sfh::ui::FontUiClient::~FontUiClient()
{
	Close();
	std::lock_guard lg(m_impl->m_pipeMutex);
	m_impl->ResetPipeLocked();
}

sfh::ui::FontUiSnapshotData sfh::ui::FontUiClient::CaptureSnapshot(std::wstring_view query) const
{
	return MakeRequest(query);
}

void sfh::ui::FontUiClient::Close()
{
	m_impl->m_shutdownEvent.SetEvent();
	const HANDLE pipeHandle = m_impl->m_activePipeHandle.load(std::memory_order_acquire);
	if (pipeHandle != INVALID_HANDLE_VALUE && pipeHandle != nullptr)
	{
		CancelIoEx(pipeHandle, nullptr);
	}
}

std::wstring sfh::ui::FontUiClient::BuildDefaultPipeName()
{
	std::wstring pipeName = LR"_(\\.\pipe\SubtitleFontAutoLoaderRpc-)_";
	pipeName += GetCurrentProcessUserSid();
	return pipeName;
}

sfh::ui::FontUiSnapshotData sfh::ui::FontUiClient::MakeRequest(std::wstring_view query) const
{
	sfh::FontQueryRequest request;
	request.set_version(1);
	auto* fontUiRequest = request.mutable_fontuirequest();
	fontUiRequest->set_query(WideToUtf8String(query));

	std::lock_guard lg(m_impl->m_pipeMutex);
	auto& pipe = m_impl->EnsurePipeLocked();
	try
	{
		SendRequest(pipe, request, m_impl->m_shutdownEvent.get());
		auto response = ReceiveResponse(pipe, m_impl->m_shutdownEvent.get());
		if (response.version() != 1 || !response.has_fontuisnapshot())
		{
			throw std::runtime_error("daemon did not return a font UI snapshot");
		}
		return ConvertSnapshot(response.fontuisnapshot());
	}
	catch (...)
	{
		m_impl->ResetPipeLocked();
		throw;
	}
}
