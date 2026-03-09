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

				auto files = FontIndexCore::EnumerateFontFiles(task.m_sourceFolders, isCancelled);
				std::vector<std::filesystem::path> fontPaths;
				fontPaths.reserve(files.size());
				for (const auto& file : files)
				{
					fontPaths.push_back(file.m_path);
				}

				auto db = FontIndexCore::BuildFontDatabase(fontPaths, workerCount, isCancelled);
				if (stopToken.stop_requested())
				{
					return;
				}

				WriteFontDatabaseAtomically(task.m_indexPath, db);
				auto snapshot = FontIndexCore::CaptureDirectorySnapshot(task.m_sourceFolders, isCancelled);
				if (stopToken.stop_requested())
				{
					return;
				}
				FontIndexCore::WriteDirectorySnapshot(task.m_snapshotPath, snapshot);
				if (stopToken.stop_requested())
				{
					return;
				}

				TryShowToast(
					L"Subtitle Font Helper",
					L"索引建立完成：" + indexName + L"（字体文件 " + std::to_wstring(fontPaths.size()) + L" 个）");
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
