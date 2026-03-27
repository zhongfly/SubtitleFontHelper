#include "pch.h"

#include "ManagedIndexBuilder.h"

#include "Common.h"
#include "EventLog.h"
#include "ToastNotifier.h"
#include "PersistantData.h"
#include "../FontIndexCore/FontIndexCore.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <unordered_map>
#include <wil/resource.h>

namespace sfh
{
	namespace
	{
		std::wstring JoinPaths(const std::vector<std::filesystem::path>& paths)
		{
			if (paths.empty())
				return {};

			std::wstring result;
			for (size_t i = 0; i < paths.size(); ++i)
			{
				if (i != 0)
				{
					result += L"; ";
				}
				result += paths[i].wstring();
			}
			return result;
		}

		void TryLogManagedIndexBuildStart(const ManagedIndexBuilder::Task& task)
		{
			try
			{
				EventLog::GetInstance().LogDebugMessage(
					L"managed index build start: index=\"%ls\" sources=[%ls]",
					task.m_indexPath.c_str(),
					JoinPaths(task.m_sourceFolders).c_str());
			}
			catch (...)
			{
			}
		}

		void TryLogManagedIndexBuildComplete(const ManagedIndexBuilder::Task& task, size_t fontFileCount)
		{
			try
			{
				EventLog::GetInstance().LogDebugMessage(
					L"managed index build complete: index=\"%ls\" fontFileCount=%zu",
					task.m_indexPath.c_str(),
					fontFileCount);
			}
			catch (...)
			{
			}
		}

		void TryLogManagedIndexBuildFailure(const ManagedIndexBuilder::Task& task, const std::exception& e)
		{
			try
			{
				EventLog::GetInstance().LogDebugMessage(
					L"managed index build failed: index=\"%ls\" error=\"%ls\"",
					task.m_indexPath.c_str(),
					Utf8ToWideString(e.what()).c_str());
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
	}

	void ValidateManagedIndexSourceFolders(const ManagedIndexBuilder::Task& task)
	{
		if (task.m_sourceFolders.empty())
		{
			throw std::runtime_error("managed index source_folders is empty");
		}

		for (const auto& sourceFolder : task.m_sourceFolders)
		{
			const auto attributes = GetFileAttributesW(sourceFolder.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			{
				throw std::runtime_error(
					"source folder is not accessible: " + WideToUtf8String(sourceFolder.wstring()));
			}
		}
	}

	size_t BuildManagedIndex(
		const ManagedIndexBuilder::Task& task,
		size_t workerCount,
		const std::function<bool()>& isCancelled,
		const ManagedIndexBuildFeedbackSession& feedback)
	{
		ValidateManagedIndexSourceFolders(task);
		feedback.SetStage(ManagedIndexBuildStage::Scanning);
		auto snapshot = FontIndexCore::CaptureDirectorySnapshot(task.m_sourceFolders, isCancelled);
		std::vector<std::string> failures;
		feedback.SetStage(ManagedIndexBuildStage::Hashing, CountHashWorkItems(snapshot));
		FontIndexCore::PopulateMissingContentHashes(
			snapshot,
			workerCount,
			isCancelled,
			feedback.GetProgressCounter(),
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

		std::vector<std::filesystem::path> fontPaths;
		fontPaths.reserve(groups.size());
		for (const auto& group : groups)
		{
			fontPaths.push_back(snapshot.m_files[group.front()].m_path);
		}

		feedback.SetStage(ManagedIndexBuildStage::Analyzing, fontPaths.size());
		auto db = FontIndexCore::BuildFontDatabase(
			fontPaths,
			workerCount,
			isCancelled,
			feedback.GetProgressCounter(),
			[&](const std::filesystem::path& path, const std::string& errorMessage)
			{
				failures.push_back(WideToUtf8String(path.wstring()) + ": " + errorMessage);
			});
		if (!failures.empty())
		{
			throw std::runtime_error(failures.front());
		}
		ThrowIfCancelled(isCancelled);
		feedback.SetStage(ManagedIndexBuildStage::Writing, fontPaths.size(), fontPaths.size());
		WriteFontDatabaseAtomically(task.m_indexPath, db);
		ThrowIfCancelled(isCancelled);
		FontIndexCore::WriteDirectorySnapshot(task.m_snapshotPath, snapshot);
		ThrowIfCancelled(isCancelled);
		return snapshot.m_files.size();
	}

	ManagedIndexBuilder::ManagedIndexBuilder(IDaemon* daemon, Task task, size_t workerCount)
		: m_worker([daemon, task = std::move(task), workerCount](std::stop_token stopToken)
		{
			const auto indexName = GetDisplayName(task.m_indexPath);
			ManagedIndexBuildFeedbackSession feedback(
				daemon,
				task.m_indexPath,
				task.m_progressState,
				task.m_enableProgressNotifications);
			TryLogManagedIndexBuildStart(task);

			try
			{
				auto isCancelled = [&]()
				{
					return stopToken.stop_requested();
				};

				const auto fontFileCount = BuildManagedIndex(task, workerCount, isCancelled, feedback);

				TryShowToast(
					L"Subtitle Font Helper",
					L"索引建立完成：" + indexName + L"（字体文件 " + std::to_wstring(fontFileCount) + L" 个）");
				TryLogManagedIndexBuildComplete(task, fontFileCount);
				daemon->NotifyManagedIndexBuilt(task.m_indexPath);
			}
			catch (const std::exception& e)
			{
				TryLogManagedIndexBuildFailure(task, e);
				if (!stopToken.stop_requested())
				{
					TryShowToast(
						L"Subtitle Font Helper",
						L"索引建立失败：" + indexName + L"（" + Utf8ToWideString(e.what()) + L"）");
				}
			}
		})
	{
	}

	ManagedIndexBuilder::~ManagedIndexBuilder() = default;
}
