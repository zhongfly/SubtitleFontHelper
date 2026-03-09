#include "pch.h"

#include "ManagedIndexBuilder.h"

#include "Common.h"
#include "ToastNotifier.h"
#include "PersistantData.h"
#include "../FontIndexCore/FontIndexCore.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <wil/resource.h>

namespace sfh
{
	namespace
	{
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
	}

	size_t BuildManagedIndex(
		const ManagedIndexBuilder::Task& task,
		size_t workerCount,
		const std::function<bool()>& isCancelled)
	{
		auto snapshot = FontIndexCore::CaptureDirectorySnapshot(task.m_sourceFolders, isCancelled);
		FontIndexCore::PopulateMissingContentHashes(snapshot, workerCount, isCancelled);
		auto groups = FontIndexCore::GroupEquivalentFiles(snapshot, isCancelled);

		std::vector<std::filesystem::path> fontPaths;
		fontPaths.reserve(groups.size());
		for (const auto& group : groups)
		{
			fontPaths.push_back(snapshot.m_files[group.front()].m_path);
		}

		auto db = FontIndexCore::BuildFontDatabase(fontPaths, workerCount, isCancelled);
		ThrowIfCancelled(isCancelled);
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
			TryShowToast(L"Subtitle Font Helper", L"开始建立索引：" + indexName);

			try
			{
				auto isCancelled = [&]()
				{
					return stopToken.stop_requested();
				};

				const auto fontFileCount = BuildManagedIndex(task, workerCount, isCancelled);

				TryShowToast(
					L"Subtitle Font Helper",
					L"索引建立完成：" + indexName + L"（字体文件 " + std::to_wstring(fontFileCount) + L" 个）");
				daemon->NotifyManagedIndexBuilt();
			}
			catch (const std::exception& e)
			{
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
