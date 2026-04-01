#include "pch.h"

#include "ManagedIndexWatcher.h"

#include "Common.h"
#include "EventLog.h"
#include "ManagedIndexLog.h"
#include "ToastNotifier.h"
#include "PersistantData.h"
#include "../FontIndexCore/FontIndexCore.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <optional>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <wil/resource.h>

namespace sfh
{
	namespace
	{
		constexpr uint64_t SNAPSHOT_MAGIC = 0x314E5350484653ULL;
		constexpr uint32_t LEGACY_SNAPSHOT_VERSION = 1;
		constexpr uint32_t SNAPSHOT_VERSION_WITHOUT_CHECKSUM = 2;
		constexpr uint32_t SNAPSHOT_VERSION = 3;

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

		std::wstring MakePathKey(const std::filesystem::path& path)
		{
			std::wstring key = path.wstring();
			std::transform(key.begin(), key.end(), key.begin(), towlower);
			return key;
		}

		bool HasSamePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
		{
			return MakePathKey(lhs) == MakePathKey(rhs);
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
			std::wstring message = L"索引更新完成：" + indexName
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

		void TryLogManagedIndexUpdateComplete(
			const std::filesystem::path& indexPath,
			const FontDatabase& updatedDatabase,
			const FontDatabase& previousDatabase,
			const std::unordered_set<std::wstring>& addedPaths,
			const std::unordered_set<std::wstring>& removedPaths,
			const std::unordered_set<std::wstring>& modifiedPaths)
		{
			try
			{
				const auto overallSummary = BuildManagedIndexFontLogSummary(updatedDatabase);
				const auto addedSummary = BuildManagedIndexFontLogSummary(
					updatedDatabase,
					BuildManagedIndexPathKeys(addedPaths));
				const auto removedSummary = BuildManagedIndexFontLogSummary(
					previousDatabase,
					BuildManagedIndexPathKeys(removedPaths));
				const auto modifiedSummary = BuildManagedIndexFontLogSummary(
					updatedDatabase,
					BuildManagedIndexPathKeys(modifiedPaths));
				EventLog::GetInstance().LogDebugMessage(
					L"managed index update complete: index=\"%ls\" indexedFontCount=%zu added=%zu addedFontNames=[%ls] removed=%zu removedFontNames=[%ls] modified=%zu modifiedFontNames=[%ls]",
					indexPath.c_str(),
					overallSummary.m_fontCount,
					addedPaths.size(),
					addedSummary.m_fontNamesSummary.c_str(),
					removedPaths.size(),
					removedSummary.m_fontNamesSummary.c_str(),
					modifiedPaths.size(),
					modifiedSummary.m_fontNamesSummary.c_str());
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

		size_t CountHashWorkItems(const FontIndexCore::DirectorySnapshot& snapshot)
		{
			std::unordered_map<uint64_t, size_t> sizeGroupCounts;
			sizeGroupCounts.reserve(snapshot.m_files.size());
			for (const auto& entry : snapshot.m_files)
			{
				++sizeGroupCounts[entry.m_fileSize];
			}

			size_t pendingHashes = 0;
			for (const auto& entry : snapshot.m_files)
			{
				if (sizeGroupCounts[entry.m_fileSize] > 1 && !entry.m_hasContentHash)
				{
					++pendingHashes;
				}
			}
			return pendingHashes;
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
				if (!HasSamePath(left.m_path, right.m_path)
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
			return HasSamePath(lhs.m_path, rhs.m_path)
				&& lhs.m_fileSize == rhs.m_fileSize
				&& lhs.m_lastWriteTime == rhs.m_lastWriteTime;
		}

		std::unordered_set<std::wstring> ExtractIndexedPaths(const FontDatabase& db)
		{
			std::unordered_set<std::wstring> paths;
			for (const auto& font : db.m_fonts)
			{
				paths.insert(MakePathKey(font.m_path.Get()));
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
				const auto key = MakePathKey(entry.m_path);
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

		const DWORD FILE_NOTIFY_INFORMATION_HEADER_SIZE =
			static_cast<DWORD>(FIELD_OFFSET(FILE_NOTIFY_INFORMATION, FileName));

		struct PendingSnapshotChanges
		{
			std::unordered_map<std::wstring, std::filesystem::path> m_changedPaths;
			bool m_requiresFullRescan = false;

			bool HasPendingWork() const
			{
				return m_requiresFullRescan || !m_changedPaths.empty();
			}

			void MarkChangedPath(std::filesystem::path path)
			{
				if (m_requiresFullRescan)
					return;
				m_changedPaths.insert_or_assign(MakePathKey(path), std::move(path));
			}

			void RequireFullRescan()
			{
				m_changedPaths.clear();
				m_requiresFullRescan = true;
			}

			void Reset()
			{
				m_changedPaths.clear();
				m_requiresFullRescan = false;
			}
		};

		void SortSnapshot(FontIndexCore::DirectorySnapshot& snapshot)
		{
			std::sort(snapshot.m_files.begin(), snapshot.m_files.end(),
				[](const FontIndexCore::DirectorySnapshotEntry& lhs,
					const FontIndexCore::DirectorySnapshotEntry& rhs)
				{
					return lhs.m_path < rhs.m_path;
				});
		}

		enum class SnapshotChangeStatus { Unchanged, Changed, FullRescan };

		struct TargetedSnapshotResult
		{
			FontIndexCore::DirectorySnapshot snapshot;
			SnapshotChangeStatus status;
		};
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
		bool m_shouldUpgradePersistedSnapshot = false;

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
		bool IsDirectoryLevelChange(const std::filesystem::path& path) const
		{
			const auto attributes = GetFileAttributesW(path.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES)
				return true;
			return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		}

		std::filesystem::path BuildNotificationPath(
			const FolderWatch& watch,
			const FILE_NOTIFY_INFORMATION& notification) const
		{
			const auto charCount = notification.FileNameLength / sizeof(WCHAR);
			std::wstring relativeName(notification.FileName, charCount);
			return FontIndexCore::NormalizePath(watch.m_path / relativeName);
		}

		void AccumulateNotification(
			const FolderWatch& watch,
			const FILE_NOTIFY_INFORMATION& notification,
			PendingSnapshotChanges& pending) const
		{
			switch (notification.Action)
			{
			case FILE_ACTION_ADDED:
			case FILE_ACTION_REMOVED:
			case FILE_ACTION_MODIFIED:
			case FILE_ACTION_RENAMED_OLD_NAME:
			case FILE_ACTION_RENAMED_NEW_NAME:
				break;
			default:
				pending.RequireFullRescan();
				return;
			}

			if (notification.FileNameLength == 0
				|| (notification.FileNameLength % sizeof(WCHAR)) != 0)
			{
				pending.RequireFullRescan();
				return;
			}

			const auto path = BuildNotificationPath(watch, notification);
			if (IsDirectoryLevelChange(path))
			{
				pending.RequireFullRescan();
				return;
			}
			pending.MarkChangedPath(path);
		}

		void AccumulateNotificationBuffer(
			const FolderWatch& watch,
			DWORD transferredBytes,
			PendingSnapshotChanges& pending) const
		{
			if (pending.m_requiresFullRescan)
				return;
			if (transferredBytes < FILE_NOTIFY_INFORMATION_HEADER_SIZE)
			{
				pending.RequireFullRescan();
				return;
			}

			const auto* current = reinterpret_cast<const std::byte*>(watch.m_buffer.data());
			DWORD remaining = transferredBytes;
			while (remaining >= FILE_NOTIFY_INFORMATION_HEADER_SIZE)
			{
				const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(current);
				const DWORD recordSize = info->NextEntryOffset == 0
					? remaining
					: info->NextEntryOffset;
				if (recordSize < FILE_NOTIFY_INFORMATION_HEADER_SIZE
					|| recordSize > remaining
					|| info->FileNameLength > recordSize - FILE_NOTIFY_INFORMATION_HEADER_SIZE)
				{
					pending.RequireFullRescan();
					return;
				}

				AccumulateNotification(watch, *info, pending);
				if (pending.m_requiresFullRescan)
					return;

				if (info->NextEntryOffset == 0)
					return;
				current += info->NextEntryOffset;
				remaining -= info->NextEntryOffset;
			}
			pending.RequireFullRescan();
		}

		TargetedSnapshotResult BuildTargetedSnapshot(
			const std::stop_token& stopToken,
			const PendingSnapshotChanges& pending)
		{
			if (!m_hasLastSnapshot || pending.m_requiresFullRescan)
				return { CaptureSnapshot(stopToken), SnapshotChangeStatus::FullRescan };

			if (pending.m_changedPaths.empty())
				return { m_lastSnapshot, SnapshotChangeStatus::Unchanged };

			auto isCancelled = [&]() { return stopToken.stop_requested(); };

			std::unordered_map<std::wstring, const FontIndexCore::DirectorySnapshotEntry*> oldEntryMap;
			oldEntryMap.reserve(m_lastSnapshot.m_files.size());
			for (const auto& entry : m_lastSnapshot.m_files)
				oldEntryMap.emplace(MakePathKey(entry.m_path), &entry);

			bool changed = false;
			FontIndexCore::DirectorySnapshot snapshot;
			snapshot.m_files.reserve(m_lastSnapshot.m_files.size() + pending.m_changedPaths.size());

			for (const auto& entry : m_lastSnapshot.m_files)
			{
				if (!pending.m_changedPaths.contains(MakePathKey(entry.m_path)))
					snapshot.m_files.push_back(entry);
			}

			for (const auto& [key, path] : pending.m_changedPaths)
			{
				ThrowIfCancelled(isCancelled);
				FontIndexCore::DirectorySnapshotEntry updated{};
				const bool captured = FontIndexCore::TryCaptureDirectorySnapshotEntry(path, updated);
				const auto oldIt = oldEntryMap.find(key);
				const bool existed = oldIt != oldEntryMap.end();

				if (captured && existed && HasSameMetadata(*oldIt->second, updated))
				{
					snapshot.m_files.push_back(*oldIt->second);
				}
				else if (captured)
				{
					snapshot.m_files.push_back(std::move(updated));
					changed = true;
				}
				else if (existed)
				{
					changed = true;
				}
			}

			SortSnapshot(snapshot);
			return { std::move(snapshot), changed ? SnapshotChangeStatus::Changed : SnapshotChangeStatus::Unchanged };
		}

		bool IsExternalBuildInProgress() const
		{
			return m_task.m_progressState
				&& m_task.m_progressState->m_buildInProgress.load(std::memory_order_relaxed);
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
				auto normalized = FontIndexCore::NormalizePath(sourceFolder);
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
			catch (const std::exception& e)
			{
				EventLog::GetInstance().LogDebugMessage(
					L"managed index snapshot read failed: path=\"%ls\" error=\"%hs\"",
					m_task.m_snapshotPath.c_str(),
					e.what());
				return false;
			}
			catch (...)
			{
				EventLog::GetInstance().LogDebugMessage(
					L"managed index snapshot read failed: path=\"%ls\" error=\"unknown exception\"",
					m_task.m_snapshotPath.c_str());
				return false;
			}
		}

		bool NeedsPersistedSnapshotUpgrade() const
		{
			std::ifstream stream(m_task.m_snapshotPath, std::ios::binary);
			if (!stream)
			{
				return false;
			}

			uint64_t magic = 0;
			stream.read(reinterpret_cast<char*>(&magic), sizeof(magic));
			if (!stream || magic != SNAPSHOT_MAGIC)
			{
				return false;
			}

			uint32_t version = 0;
			stream.read(reinterpret_cast<char*>(&version), sizeof(version));
			if (!stream)
			{
				return false;
			}

			return version < SNAPSHOT_VERSION;
		}

		void TryUpgradePersistedSnapshot(const FontIndexCore::DirectorySnapshot& snapshot) const
		{
			try
			{
				FontIndexCore::WriteDirectorySnapshot(m_task.m_snapshotPath, snapshot);
			}
			catch (...)
			{
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
				oldEntries.emplace(MakePathKey(entry.m_path), &entry);
			}

			for (auto& entry : newSnapshot.m_files)
			{
				const auto oldEntry = oldEntries.find(MakePathKey(entry.m_path));
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
			const std::stop_token& stopToken,
			const ManagedIndexBuildFeedbackSession& feedback)
		{
			auto isCancelled = [&]()
			{
				return stopToken.stop_requested();
			};

			std::vector<std::string> failures;
			std::mutex failureLock;
			feedback.SetStage(ManagedIndexBuildStage::Hashing, CountHashWorkItems(snapshot));
			FontIndexCore::PopulateMissingContentHashes(
				snapshot,
				m_workerCount,
				isCancelled,
				feedback.GetProgressCounter(),
				[&](const std::filesystem::path& path, const std::string& errorMessage)
				{
					std::lock_guard lg(failureLock);
					failures.push_back(WideToUtf8String(path.wstring()) + ": " + errorMessage);
				});
			auto groups = FontIndexCore::GroupEquivalentFiles(
				snapshot,
				isCancelled,
				[&](const std::filesystem::path& path, const std::string& errorMessage)
				{
					std::lock_guard lg(failureLock);
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
				ManagedIndexBuildFeedbackSession feedback(
					m_daemon,
					m_task.m_indexPath,
					m_task.m_progressState,
					ManagedIndexWorkType::Update);
				if (m_hasLastSnapshot)
				{
					ApplyCachedHashes(m_lastSnapshot, newSnapshot);
				}

				const auto groups = BuildContentGroups(newSnapshot, stopToken, feedback);
				EnsureLoadedDatabase();
				ThrowIfCancelled(isCancelled);
				const auto previousDatabase = m_database;

				std::unordered_map<std::wstring, const FontIndexCore::DirectorySnapshotEntry*> oldEntries;
				oldEntries.reserve(m_lastSnapshot.m_files.size());
				if (m_hasLastSnapshot)
				{
					for (const auto& entry : m_lastSnapshot.m_files)
					{
						oldEntries.emplace(MakePathKey(entry.m_path), &entry);
					}
				}

				std::unordered_set<std::wstring> currentPaths;
				currentPaths.reserve(newSnapshot.m_files.size());
				std::unordered_set<std::wstring> changedPathKeys;
				changedPathKeys.reserve(newSnapshot.m_files.size());
				std::unordered_set<std::wstring> addedPaths;
				addedPaths.reserve(newSnapshot.m_files.size());
				std::unordered_set<std::wstring> modifiedPaths;
				modifiedPaths.reserve(newSnapshot.m_files.size());
				for (const auto& entry : newSnapshot.m_files)
				{
					const auto key = MakePathKey(entry.m_path);
					currentPaths.insert(key);
					const auto oldEntry = oldEntries.find(key);
					if (oldEntry == oldEntries.end())
					{
						changedPathKeys.insert(key);
						addedPaths.insert(entry.m_path.wstring());
					}
					else if (!HasSameMetadata(*oldEntry->second, entry))
					{
						changedPathKeys.insert(key);
						modifiedPaths.insert(entry.m_path.wstring());
					}
				}

				std::unordered_set<std::wstring> removedPaths;
				removedPaths.reserve(oldEntries.size());
				std::unordered_set<std::wstring> removedPathKeys;
				removedPathKeys.reserve(oldEntries.size());
				if (m_hasLastSnapshot)
				{
					for (const auto& [path, entry] : oldEntries)
					{
						if (!currentPaths.contains(path))
						{
							removedPathKeys.insert(path);
							removedPaths.insert(entry->m_path.wstring());
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
					const auto key = MakePathKey(canonicalPath);
					newCanonicalPaths.insert(key);
					if (changedPathKeys.contains(key) || !m_indexedPaths.contains(key))
					{
						pathsToAnalyze.push_back(canonicalPath);
					}
				}

				std::unordered_set<std::wstring> pathsToRemoveKeys = removedPathKeys;
				for (const auto& path : changedPathKeys)
				{
					pathsToRemoveKeys.insert(path);
				}
				for (const auto& path : m_indexedPaths)
				{
					if (!newCanonicalPaths.contains(path))
					{
						pathsToRemoveKeys.insert(path);
					}
				}

				if (!pathsToRemoveKeys.empty())
				{
					std::erase_if(m_database.m_fonts, [&](const FontDatabase::FontFaceElement& font)
					{
						return pathsToRemoveKeys.contains(MakePathKey(font.m_path.Get()));
					});
				}

				if (!pathsToAnalyze.empty())
				{
					std::vector<std::string> analyzeFailures;
					std::mutex analyzeFailureLock;
					feedback.SetStage(ManagedIndexBuildStage::Analyzing, pathsToAnalyze.size());
					auto analyzed = FontIndexCore::BuildFontDatabase(
						pathsToAnalyze,
						m_workerCount,
						isCancelled,
						feedback.GetProgressCounter(),
						[&](const std::filesystem::path& path, const std::string& errorMessage)
						{
							std::lock_guard lg(analyzeFailureLock);
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
					m_database.DeduplicatePaths();
				}

				ThrowIfCancelled(isCancelled);
				feedback.SetStage(
					ManagedIndexBuildStage::Writing,
					pathsToAnalyze.size(),
					pathsToAnalyze.size());
				WriteFontDatabaseAtomically(m_task.m_indexPath, m_database);
				ThrowIfCancelled(isCancelled);
				FontIndexCore::WriteDirectorySnapshot(m_task.m_snapshotPath, newSnapshot);
				ThrowIfCancelled(isCancelled);

				m_lastSnapshot = std::move(newSnapshot);
				m_hasLastSnapshot = true;
				m_indexedPaths = std::move(newCanonicalPaths);
				m_hasDatabase = true;
				TryLogManagedIndexUpdateComplete(
					m_task.m_indexPath,
					m_database,
					previousDatabase,
					addedPaths,
					removedPaths,
					modifiedPaths);

				if (m_task.m_enableNotifications)
				{
					TryShowToast(
						L"Subtitle Font Helper",
						BuildSyncToastMessage(indexName, addedPaths, removedPaths, modifiedPaths));
				}
				m_daemon->NotifyManagedIndexBuilt(m_task.m_indexPath);
			}
			catch (const std::exception& e)
			{
				if (!stopToken.stop_requested() && m_task.m_enableNotifications)
				{
					TryShowToast(
						L"Subtitle Font Helper",
						L"索引更新失败：" + indexName + L"（" + Utf8ToWideString(e.what()) + L"）");
				}
			}
		}

		void InitializeSnapshotState(const std::stop_token& stopToken)
		{
			auto currentSnapshot = CaptureSnapshot(stopToken);
			FontIndexCore::DirectorySnapshot persistedSnapshot;
			const bool hasPersistedSnapshot = TryReadPersistedSnapshot(persistedSnapshot);
			const bool shouldUpgradePersistedSnapshot =
				hasPersistedSnapshot && NeedsPersistedSnapshotUpgrade();
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
				m_shouldUpgradePersistedSnapshot = shouldUpgradePersistedSnapshot;
				return;
			}

			RunIncrementalSync(stopToken, std::move(currentSnapshot));
		}

		void WorkerProcedure(const std::stop_token& stopToken)
		{
			InitializeSnapshotState(stopToken);

			for (auto& watch : m_folderWatches)
			{
				QueueRead(watch);
			}
			if (m_shouldUpgradePersistedSnapshot)
			{
				TryUpgradePersistedSnapshot(m_lastSnapshot);
				m_shouldUpgradePersistedSnapshot = false;
			}

			std::vector<HANDLE> waitHandles;
			waitHandles.reserve(m_folderWatches.size() + 1);
			waitHandles.push_back(m_exitEvent.get());
			for (auto& watch : m_folderWatches)
			{
				waitHandles.push_back(watch.m_event.get());
			}

			PendingSnapshotChanges pendingChanges;
			std::optional<std::chrono::steady_clock::time_point> debounceDeadline;
			if (!m_hasLastSnapshot)
			{
				pendingChanges.RequireFullRescan();
				debounceDeadline = std::chrono::steady_clock::now() + m_debounce;
			}
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
					bool shouldRetryInitialSync = false;
					if (pendingChanges.HasPendingWork())
					{
						if (IsExternalBuildInProgress())
						{
							debounceDeadline = std::chrono::steady_clock::now() + m_debounce;
							continue;
						}
						auto result = BuildTargetedSnapshot(stopToken, pendingChanges);
						if (result.status == SnapshotChangeStatus::Changed)
						{
							if (m_hasLastSnapshot)
								ApplyCachedHashes(m_lastSnapshot, result.snapshot);
							RunIncrementalSync(stopToken, std::move(result.snapshot));
						}
						else if (result.status == SnapshotChangeStatus::FullRescan)
						{
							if (m_hasLastSnapshot)
								ApplyCachedHashes(m_lastSnapshot, result.snapshot);
							if (!m_hasLastSnapshot || !AreSnapshotsEqual(m_lastSnapshot, result.snapshot))
								RunIncrementalSync(stopToken, std::move(result.snapshot));
						}
						shouldRetryInitialSync = !m_hasLastSnapshot;
					}
					pendingChanges.Reset();
					if (shouldRetryInitialSync)
					{
						pendingChanges.RequireFullRescan();
						debounceDeadline = std::chrono::steady_clock::now() + m_debounce;
					}
					else
					{
						debounceDeadline.reset();
					}
					continue;
				}
				if (waitResult >= WAIT_OBJECT_0 + 1 && waitResult < WAIT_OBJECT_0 + waitHandles.size())
				{
					const auto index = waitResult - WAIT_OBJECT_0 - 1;
					auto& watch = m_folderWatches[index];
					DWORD transferredBytes = 0;
					bool requiresFullRescan = false;
					if (!GetOverlappedResult(
						watch.m_handle.get(),
						&watch.m_overlapped,
						&transferredBytes,
						FALSE))
					{
						const auto error = GetLastError();
						if (error == ERROR_OPERATION_ABORTED && stopToken.stop_requested())
						{
							return;
						}
						if (error == ERROR_NOTIFY_ENUM_DIR)
						{
							requiresFullRescan = true;
						}
						else
						{
							THROW_WIN32(error);
						}
					}
					else if (transferredBytes == 0)
					{
						requiresFullRescan = true;
					}

					if (requiresFullRescan)
					{
						pendingChanges.RequireFullRescan();
					}
					else
					{
						AccumulateNotificationBuffer(watch, transferredBytes, pendingChanges);
					}

					if (!QueueRead(watch))
					{
						pendingChanges.RequireFullRescan();
					}
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
