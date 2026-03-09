#include "pch.h"

#include "ManagedIndexWatcher.h"

#include "Common.h"
#include "ToastNotifier.h"
#include "../FontIndexCore/FontIndexCore.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <optional>
#include <unordered_set>
#include <wil/resource.h>

namespace sfh
{
	namespace
	{
		constexpr DWORD WATCH_FILTER =
			FILE_NOTIFY_CHANGE_FILE_NAME
			| FILE_NOTIFY_CHANGE_DIR_NAME
			| FILE_NOTIFY_CHANGE_LAST_WRITE
			| FILE_NOTIFY_CHANGE_SIZE;
		constexpr DWORD WATCH_BUFFER_SIZE = 64 * 1024;

		std::wstring GetDisplayName(const std::filesystem::path& path)
		{
			return path.filename().empty() ? path.wstring() : path.filename().wstring();
		}

		void TryShowToast(const std::wstring& title, const std::wstring& message)
		{
			try
			{
				ToastNotifier().ShowToast(title, message);
			}
			catch (...)
			{
			}
		}

		bool AreSnapshotsEqual(
			const FontIndexCore::DirectorySnapshot& lhs,
			const FontIndexCore::DirectorySnapshot& rhs)
		{
			if (lhs.m_files.size() != rhs.m_files.size())
			{
				return false;
			}

			for (size_t i = 0; i < lhs.m_files.size(); ++i)
			{
				const auto& left = lhs.m_files[i];
				const auto& right = rhs.m_files[i];
				if (left.m_path != right.m_path
					|| left.m_fileSize != right.m_fileSize
					|| left.m_lastWriteTime != right.m_lastWriteTime
					|| left.m_hasContentHash != right.m_hasContentHash
					|| left.m_contentHash.m_low64 != right.m_contentHash.m_low64
					|| left.m_contentHash.m_high64 != right.m_contentHash.m_high64)
				{
					return false;
				}
			}
			return true;
		}
	}

	class ManagedIndexWatcher::Implementation
	{
	private:
		struct FolderWatch
		{
			std::filesystem::path m_path;
			wil::unique_hfile m_handle;
			wil::unique_event m_event;
			OVERLAPPED m_overlapped{};
			std::vector<std::byte> m_buffer;
		};

		IDaemon* m_daemon;
		ManagedIndexBuilder::Task m_task;
		size_t m_workerCount;
		std::chrono::milliseconds m_debounce;
		bool m_skipInitialSync;

		wil::unique_event m_exitEvent;
		std::vector<FolderWatch> m_folderWatches;
		std::jthread m_worker;

		FontIndexCore::DirectorySnapshot m_lastSnapshot;
		bool m_hasLastSnapshot = false;

	public:
		Implementation(IDaemon* daemon, Options options)
			: m_daemon(daemon),
			m_task(std::move(options.m_task)),
			m_workerCount(options.m_workerCount),
			m_debounce(options.m_debounce),
			m_skipInitialSync(options.m_skipInitialSync)
		{
			m_exitEvent.create();
			InitializeFolderWatches();
			m_worker = std::jthread([this](std::stop_token stopToken)
			{
				WorkerProcedure(stopToken);
			});
		}

		~Implementation()
		{
			m_exitEvent.SetEvent();
			for (auto& watch : m_folderWatches)
			{
				if (watch.m_handle)
				{
					CancelIoEx(watch.m_handle.get(), &watch.m_overlapped);
				}
			}
		}

	private:
		static std::filesystem::path NormalizePath(const std::filesystem::path& path)
		{
			return std::filesystem::absolute(path).lexically_normal();
		}

		bool QueueRead(FolderWatch& watch)
		{
			ResetEvent(watch.m_event.get());
			watch.m_overlapped = {};
			watch.m_overlapped.hEvent = watch.m_event.get();
			if (ReadDirectoryChangesW(
				watch.m_handle.get(),
				watch.m_buffer.data(),
				static_cast<DWORD>(watch.m_buffer.size()),
				TRUE,
				WATCH_FILTER,
				nullptr,
				&watch.m_overlapped,
				nullptr))
			{
				return true;
			}

			const auto error = GetLastError();
			if (error == ERROR_IO_PENDING)
			{
				return true;
			}
			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			{
				return false;
			}
			THROW_WIN32(error);
		}

		void InitializeFolderWatches()
		{
			std::unordered_set<std::wstring> seenPaths;
			for (const auto& sourceFolder : m_task.m_sourceFolders)
			{
				auto normalized = NormalizePath(sourceFolder);
				const auto key = normalized.wstring();
				if (!seenPaths.insert(key).second)
				{
					continue;
				}

				std::error_code ec;
				if (!std::filesystem::exists(normalized, ec) || ec)
				{
					continue;
				}

				auto handle = wil::unique_hfile(CreateFileW(
					normalized.c_str(),
					FILE_LIST_DIRECTORY,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr,
					OPEN_EXISTING,
					FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
					nullptr));
				if (!handle)
				{
					continue;
				}

				FolderWatch watch;
				watch.m_path = std::move(normalized);
				watch.m_handle = std::move(handle);
				watch.m_event.create();
				watch.m_buffer.resize(WATCH_BUFFER_SIZE);
				m_folderWatches.push_back(std::move(watch));
			}
		}

		FontIndexCore::DirectorySnapshot CaptureSnapshot(const std::stop_token& stopToken)
		{
			auto isCancelled = [&]()
			{
				return stopToken.stop_requested();
			};
			return FontIndexCore::CaptureDirectorySnapshot(m_task.m_sourceFolders, isCancelled);
		}

		bool TryReadPersistedSnapshot(FontIndexCore::DirectorySnapshot& snapshot) const
		{
			std::error_code ec;
			if (!std::filesystem::exists(m_task.m_snapshotPath, ec) || ec)
			{
				return false;
			}

			try
			{
				snapshot = FontIndexCore::ReadDirectorySnapshot(m_task.m_snapshotPath);
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		void RunRebuild(
			const std::stop_token& stopToken,
			const FontIndexCore::DirectorySnapshot& newSnapshot)
		{
			const auto indexName = GetDisplayName(m_task.m_indexPath);
			try
			{
				auto isCancelled = [&]()
				{
					return stopToken.stop_requested();
				};

				const auto fontFileCount = BuildManagedIndex(m_task, m_workerCount, isCancelled);
				if (stopToken.stop_requested())
				{
					return;
				}

				m_lastSnapshot = newSnapshot;
				m_hasLastSnapshot = true;
				TryShowToast(
					L"Subtitle Font Helper",
					L"索引同步完成：" + indexName + L"（字体文件 " + std::to_wstring(fontFileCount) + L" 个）");
				m_daemon->NotifyManagedIndexBuilt();
			}
			catch (const std::exception& e)
			{
				if (!stopToken.stop_requested())
				{
					TryShowToast(
						L"Subtitle Font Helper",
						L"索引同步失败：" + indexName + L"（" + Utf8ToWideString(e.what()) + L"）");
				}
			}
		}

		void InitializeSnapshotState(const std::stop_token& stopToken)
		{
			auto currentSnapshot = CaptureSnapshot(stopToken);
			if (m_skipInitialSync)
			{
				m_lastSnapshot = std::move(currentSnapshot);
				m_hasLastSnapshot = true;
				return;
			}

			FontIndexCore::DirectorySnapshot persistedSnapshot;
			const bool hasPersistedSnapshot = TryReadPersistedSnapshot(persistedSnapshot);
			if (hasPersistedSnapshot && AreSnapshotsEqual(persistedSnapshot, currentSnapshot))
			{
				m_lastSnapshot = std::move(currentSnapshot);
				m_hasLastSnapshot = true;
				return;
			}

			RunRebuild(stopToken, currentSnapshot);
			if (!m_hasLastSnapshot)
			{
				m_lastSnapshot = std::move(currentSnapshot);
				m_hasLastSnapshot = true;
			}
		}

		void WorkerProcedure(const std::stop_token& stopToken)
		{
			InitializeSnapshotState(stopToken);

			for (auto& watch : m_folderWatches)
			{
				QueueRead(watch);
			}

			std::vector<HANDLE> waitHandles;
			waitHandles.reserve(m_folderWatches.size() + 1);
			waitHandles.push_back(m_exitEvent.get());
			for (auto& watch : m_folderWatches)
			{
				waitHandles.push_back(watch.m_event.get());
			}

			bool pendingSync = false;
			std::optional<std::chrono::steady_clock::time_point> debounceDeadline;
			while (!stopToken.stop_requested())
			{
				DWORD timeout = INFINITE;
				if (debounceDeadline.has_value())
				{
					const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
						*debounceDeadline - std::chrono::steady_clock::now());
					timeout = remaining <= std::chrono::milliseconds::zero()
						? 0
						: static_cast<DWORD>(remaining.count());
				}

				const auto waitResult = WaitForMultipleObjects(
					static_cast<DWORD>(waitHandles.size()),
					waitHandles.data(),
					FALSE,
					timeout);
				if (waitResult == WAIT_OBJECT_0)
				{
					return;
				}
				if (waitResult == WAIT_TIMEOUT)
				{
					if (pendingSync)
					{
						auto newSnapshot = CaptureSnapshot(stopToken);
						if (!m_hasLastSnapshot || !AreSnapshotsEqual(m_lastSnapshot, newSnapshot))
						{
							RunRebuild(stopToken, newSnapshot);
						}
					}
					pendingSync = false;
					debounceDeadline.reset();
					continue;
				}
				if (waitResult >= WAIT_OBJECT_0 + 1 && waitResult < WAIT_OBJECT_0 + waitHandles.size())
				{
					auto index = waitResult - WAIT_OBJECT_0 - 1;
					DWORD transferredBytes = 0;
					if (!GetOverlappedResult(
						m_folderWatches[index].m_handle.get(),
						&m_folderWatches[index].m_overlapped,
						&transferredBytes,
						FALSE))
					{
						const auto error = GetLastError();
						if (error == ERROR_OPERATION_ABORTED && stopToken.stop_requested())
						{
							return;
						}
						if (error != ERROR_NOTIFY_ENUM_DIR)
						{
							THROW_WIN32(error);
						}
					}
					QueueRead(m_folderWatches[index]);
					pendingSync = true;
					debounceDeadline = std::chrono::steady_clock::now() + m_debounce;
					continue;
				}
				THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
				throw std::runtime_error("unexpected wait result");
			}
		}
	};

	ManagedIndexWatcher::ManagedIndexWatcher(IDaemon* daemon, Options options)
		: m_impl(std::make_unique<Implementation>(daemon, std::move(options)))
	{
	}

	ManagedIndexWatcher::~ManagedIndexWatcher() = default;
}
