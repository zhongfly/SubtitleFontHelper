#include "pch.h"

#include "ManagedIndexWatcher.h"

#include "Common.h"
#include "ToastNotifier.h"
#include "PersistantData.h"
#include "../FontIndexCore/FontIndexCore.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <optional>
#include <algorithm>
#include <unordered_map>
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

		constexpr size_t TOAST_FILE_NAME_LIMIT = 3;

		std::wstring BuildChangedFileList(const wchar_t* label, const std::unordered_set<std::wstring>& paths)
		{
			if (paths.empty())
			{
				return {};
			}

			std::vector<std::wstring> names;
			names.reserve(paths.size());
			for (const auto& path : paths)
			{
				names.push_back(GetDisplayName(std::filesystem::path(path)));
			}
			std::sort(names.begin(), names.end());

			const size_t visibleCount = names.size() < TOAST_FILE_NAME_LIMIT
				? names.size()
				: TOAST_FILE_NAME_LIMIT;
			std::wstring line = std::wstring(label) + L"：";
			for (size_t i = 0; i < visibleCount; ++i)
			{
				if (i != 0)
				{
					line += L"、";
				}
				line += names[i];
			}
			if (names.size() > visibleCount)
			{
				line += L" 等 " + std::to_wstring(names.size()) + L" 个";
			}
			return line;
		}

		std::wstring BuildSyncToastMessage(
			const std::wstring& indexName,
			const std::unordered_set<std::wstring>& addedPaths,
			const std::unordered_set<std::wstring>& removedPaths,
			const std::unordered_set<std::wstring>& modifiedPaths)
		{
			std::wstring message = L"索引同步完成：" + indexName
				+ L"（新增 " + std::to_wstring(addedPaths.size())
				+ L"，删除 " + std::to_wstring(removedPaths.size())
				+ L"，修改 " + std::to_wstring(modifiedPaths.size())
				+ L"）";

			const auto addedLine = BuildChangedFileList(L"新增", addedPaths);
			if (!addedLine.empty())
			{
				message += L"\n" + addedLine;
			}

			const auto removedLine = BuildChangedFileList(L"删除", removedPaths);
			if (!removedLine.empty())
			{
				message += L"\n" + removedLine;
			}

			const auto modifiedLine = BuildChangedFileList(L"修改", modifiedPaths);
			if (!modifiedLine.empty())
			{
				message += L"\n" + modifiedLine;
			}

			return message;
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

		void ThrowIfCancelled(const std::function<bool()>& isCancelled)
		{
			if (isCancelled && isCancelled())
			{
				throw std::runtime_error("Operation cancelled");
			}
		}

		void WriteFontDatabaseAtomically(const std::filesystem::path& path, const FontDatabase& db)
		{
			const auto parent = path.parent_path();
			if (!parent.empty())
			{
				std::filesystem::create_directories(parent);
			}

			const auto tempPath = path.wstring() + L".tmp";
			FontDatabase::WriteToFile(tempPath, db);
			THROW_LAST_ERROR_IF(!MoveFileExW(
				tempPath.c_str(),
				path.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
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

		bool HasSameMetadata(
			const FontIndexCore::DirectorySnapshotEntry& lhs,
			const FontIndexCore::DirectorySnapshotEntry& rhs)
		{
			return lhs.m_path == rhs.m_path
				&& lhs.m_fileSize == rhs.m_fileSize
				&& lhs.m_lastWriteTime == rhs.m_lastWriteTime;
		}

		std::unordered_set<std::wstring> ExtractIndexedPaths(const FontDatabase& db)
		{
			std::unordered_set<std::wstring> paths;
			for (const auto& font : db.m_fonts)
			{
				paths.insert(font.m_path);
			}
			return paths;
		}

		size_t SelectCanonicalEntryIndex(
			const std::vector<size_t>& group,
			const FontIndexCore::DirectorySnapshot& snapshot,
			const std::unordered_set<std::wstring>& indexedPaths,
			const std::unordered_map<std::wstring, const FontIndexCore::DirectorySnapshotEntry*>& oldEntries)
		{
			for (const auto index : group)
			{
				const auto& entry = snapshot.m_files[index];
				const auto key = entry.m_path.wstring();
				const auto oldEntry = oldEntries.find(key);
				if (!indexedPaths.contains(key) || oldEntry == oldEntries.end())
				{
					continue;
				}
				if (HasSameMetadata(*oldEntry->second, entry))
				{
					return index;
				}
			}
			return group.front();
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

		FontDatabase m_database;
		bool m_hasDatabase = false;
		std::unordered_set<std::wstring> m_indexedPaths;

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
				try
				{
					WorkerProcedure(stopToken);
				}
				catch (...)
				{
					if (!stopToken.stop_requested())
					{
						m_daemon->NotifyException(std::current_exception());
					}
				}
			});
		}

		~Implementation()
		{
			m_worker.request_stop();
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

		bool IsExternalBuildInProgress() const
		{
			return m_task.m_buildInProgress && m_task.m_buildInProgress->load();
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
			ValidateManagedIndexSourceFolders(m_task);
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

		void ApplyCachedHashes(
			const FontIndexCore::DirectorySnapshot& oldSnapshot,
			FontIndexCore::DirectorySnapshot& newSnapshot) const
		{
			std::unordered_map<std::wstring, const FontIndexCore::DirectorySnapshotEntry*> oldEntries;
			oldEntries.reserve(oldSnapshot.m_files.size());
			for (const auto& entry : oldSnapshot.m_files)
			{
				oldEntries.emplace(entry.m_path.wstring(), &entry);
			}

			for (auto& entry : newSnapshot.m_files)
			{
				const auto oldEntry = oldEntries.find(entry.m_path.wstring());
				if (oldEntry == oldEntries.end())
				{
					continue;
				}
				if (!HasSameMetadata(*oldEntry->second, entry) || !oldEntry->second->m_hasContentHash)
				{
					continue;
				}
				entry.m_hasContentHash = true;
				entry.m_contentHash = oldEntry->second->m_contentHash;
			}
		}

		void EnsureLoadedDatabase()
		{
			if (m_hasDatabase)
			{
				return;
			}

			std::error_code ec;
			if (!std::filesystem::exists(m_task.m_indexPath, ec) || ec)
			{
				m_database = {};
				m_indexedPaths.clear();
				m_hasDatabase = true;
				return;
			}

			auto db = FontDatabase::ReadFromFile(m_task.m_indexPath.wstring());
			m_database = std::move(*db);
			m_indexedPaths = ExtractIndexedPaths(m_database);
			m_hasDatabase = true;
		}

		std::vector<std::vector<size_t>> BuildContentGroups(
			FontIndexCore::DirectorySnapshot& snapshot,
			const std::stop_token& stopToken)
		{
			auto isCancelled = [&]()
			{
				return stopToken.stop_requested();
			};

			std::vector<std::string> failures;
			FontIndexCore::PopulateMissingContentHashes(
				snapshot,
				m_workerCount,
				isCancelled,
				nullptr,
				[&](const std::filesystem::path& path, const std::string& errorMessage)
				{
					failures.push_back(WideToUtf8String(path.wstring()) + ": " + errorMessage);
				});
			auto groups = FontIndexCore::GroupEquivalentFiles(
				snapshot,
				isCancelled,
				[&](const std::filesystem::path& path, const std::string& errorMessage)
				{
					failures.push_back(WideToUtf8String(path.wstring()) + ": " + errorMessage);
				});
			if (!failures.empty())
			{
				throw std::runtime_error(failures.front());
			}
			return groups;
		}

		void RunIncrementalSync(
			const std::stop_token& stopToken,
			FontIndexCore::DirectorySnapshot newSnapshot)
		{
			const auto indexName = GetDisplayName(m_task.m_indexPath);
			auto isCancelled = [&]()
			{
				return stopToken.stop_requested();
			};

			try
			{
				if (m_hasLastSnapshot)
				{
					ApplyCachedHashes(m_lastSnapshot, newSnapshot);
				}

				const auto groups = BuildContentGroups(newSnapshot, stopToken);
				EnsureLoadedDatabase();
				ThrowIfCancelled(isCancelled);

				std::unordered_map<std::wstring, const FontIndexCore::DirectorySnapshotEntry*> oldEntries;
				oldEntries.reserve(m_lastSnapshot.m_files.size());
				if (m_hasLastSnapshot)
				{
					for (const auto& entry : m_lastSnapshot.m_files)
					{
						oldEntries.emplace(entry.m_path.wstring(), &entry);
					}
				}

				std::unordered_set<std::wstring> currentPaths;
				currentPaths.reserve(newSnapshot.m_files.size());
				std::unordered_set<std::wstring> changedPaths;
				changedPaths.reserve(newSnapshot.m_files.size());
				std::unordered_set<std::wstring> addedPaths;
				addedPaths.reserve(newSnapshot.m_files.size());
				std::unordered_set<std::wstring> modifiedPaths;
				modifiedPaths.reserve(newSnapshot.m_files.size());
				for (const auto& entry : newSnapshot.m_files)
				{
					const auto key = entry.m_path.wstring();
					currentPaths.insert(key);
					const auto oldEntry = oldEntries.find(key);
					if (oldEntry == oldEntries.end())
					{
						changedPaths.insert(key);
						addedPaths.insert(key);
					}
					else if (!HasSameMetadata(*oldEntry->second, entry))
					{
						changedPaths.insert(key);
						modifiedPaths.insert(key);
					}
				}

				std::unordered_set<std::wstring> removedPaths;
				removedPaths.reserve(oldEntries.size());
				if (m_hasLastSnapshot)
				{
					for (const auto& [path, entry] : oldEntries)
					{
						if (!currentPaths.contains(path))
						{
							removedPaths.insert(path);
						}
					}
				}

				std::unordered_set<std::wstring> newCanonicalPaths;
				newCanonicalPaths.reserve(groups.size());
				std::vector<std::filesystem::path> pathsToAnalyze;
				pathsToAnalyze.reserve(groups.size());
				for (const auto& group : groups)
				{
					const auto canonicalIndex = SelectCanonicalEntryIndex(
						group,
						newSnapshot,
						m_indexedPaths,
						oldEntries);
					const auto& canonicalPath = newSnapshot.m_files[canonicalIndex].m_path;
					const auto key = canonicalPath.wstring();
					newCanonicalPaths.insert(key);
					if (changedPaths.contains(key) || !m_indexedPaths.contains(key))
					{
						pathsToAnalyze.push_back(canonicalPath);
					}
				}

				std::unordered_set<std::wstring> pathsToRemove = removedPaths;
				for (const auto& path : changedPaths)
				{
					pathsToRemove.insert(path);
				}
				for (const auto& path : m_indexedPaths)
				{
					if (!newCanonicalPaths.contains(path))
					{
						pathsToRemove.insert(path);
					}
				}

				if (!pathsToRemove.empty())
				{
					std::erase_if(m_database.m_fonts, [&](const FontDatabase::FontFaceElement& font)
					{
						return pathsToRemove.contains(font.m_path);
					});
				}

				if (!pathsToAnalyze.empty())
				{
					std::vector<std::string> analyzeFailures;
					auto analyzed = FontIndexCore::BuildFontDatabase(
						pathsToAnalyze,
						m_workerCount,
						isCancelled,
						nullptr,
						[&](const std::filesystem::path& path, const std::string& errorMessage)
						{
							analyzeFailures.push_back(WideToUtf8String(path.wstring()) + ": " + errorMessage);
						});
					if (!analyzeFailures.empty())
					{
						throw std::runtime_error(analyzeFailures.front());
					}

					m_database.m_fonts.insert(
						m_database.m_fonts.end(),
						std::make_move_iterator(analyzed.m_fonts.begin()),
						std::make_move_iterator(analyzed.m_fonts.end()));
				}

				ThrowIfCancelled(isCancelled);
				WriteFontDatabaseAtomically(m_task.m_indexPath, m_database);
				ThrowIfCancelled(isCancelled);
				FontIndexCore::WriteDirectorySnapshot(m_task.m_snapshotPath, newSnapshot);
				ThrowIfCancelled(isCancelled);

				m_lastSnapshot = std::move(newSnapshot);
				m_hasLastSnapshot = true;
				m_indexedPaths = std::move(newCanonicalPaths);
				m_hasDatabase = true;

				TryShowToast(
					L"Subtitle Font Helper",
					BuildSyncToastMessage(indexName, addedPaths, removedPaths, modifiedPaths));
				m_daemon->NotifyManagedIndexBuilt(m_task.m_indexPath);
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
			FontIndexCore::DirectorySnapshot persistedSnapshot;
			const bool hasPersistedSnapshot = TryReadPersistedSnapshot(persistedSnapshot);
			if (hasPersistedSnapshot)
			{
				ApplyCachedHashes(persistedSnapshot, currentSnapshot);
			}

			if (m_skipInitialSync)
			{
				m_lastSnapshot = std::move(currentSnapshot);
				m_hasLastSnapshot = true;
				return;
			}

			if (hasPersistedSnapshot && AreSnapshotsEqual(persistedSnapshot, currentSnapshot))
			{
				EnsureLoadedDatabase();
				m_lastSnapshot = std::move(currentSnapshot);
				m_hasLastSnapshot = true;
				return;
			}

			RunIncrementalSync(stopToken, std::move(currentSnapshot));
			if (!m_hasLastSnapshot)
			{
				m_lastSnapshot = CaptureSnapshot(stopToken);
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
						if (IsExternalBuildInProgress())
						{
							debounceDeadline = std::chrono::steady_clock::now() + m_debounce;
							continue;
						}
						auto newSnapshot = CaptureSnapshot(stopToken);
						if (m_hasLastSnapshot)
						{
							ApplyCachedHashes(m_lastSnapshot, newSnapshot);
						}
						if (!m_hasLastSnapshot || !AreSnapshotsEqual(m_lastSnapshot, newSnapshot))
						{
							RunIncrementalSync(stopToken, std::move(newSnapshot));
						}
					}
					pendingSync = false;
					debounceDeadline.reset();
					continue;
				}
				if (waitResult >= WAIT_OBJECT_0 + 1 && waitResult < WAIT_OBJECT_0 + waitHandles.size())
				{
					const auto index = waitResult - WAIT_OBJECT_0 - 1;
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
