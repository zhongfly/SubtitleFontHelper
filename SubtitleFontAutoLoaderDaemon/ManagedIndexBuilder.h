#pragma once

#include "pch.h"
#include "IDaemon.h"
#include "ManagedIndexProgress.h"
#include "PersistantData.h"

#include <atomic>
#include <functional>
#include <memory>

namespace sfh
{
	class ManagedIndexBuilder
	{
	public:
		struct Task
		{
			std::filesystem::path m_indexPath;
			std::filesystem::path m_snapshotPath;
			std::vector<std::filesystem::path> m_sourceFolders;
			std::shared_ptr<ManagedIndexBuildProgressState> m_progressState;
			bool m_enableNotifications = false;
		};

		ManagedIndexBuilder(IDaemon* daemon, Task task, size_t workerCount);
		~ManagedIndexBuilder();

		ManagedIndexBuilder(const ManagedIndexBuilder&) = delete;
		ManagedIndexBuilder(ManagedIndexBuilder&&) = delete;

		ManagedIndexBuilder& operator=(const ManagedIndexBuilder&) = delete;
		ManagedIndexBuilder& operator=(ManagedIndexBuilder&&) = delete;

	private:
		std::jthread m_worker;
	};

	struct ManagedIndexBuildResult
	{
		FontDatabase m_database;
		size_t m_sourceFontFileCount = 0;
	};

	ManagedIndexBuildResult BuildManagedIndex(
		const ManagedIndexBuilder::Task& task,
		size_t workerCount,
		const std::function<bool()>& isCancelled,
		const ManagedIndexBuildFeedbackSession& feedback);
	void ValidateManagedIndexSourceFolders(const ManagedIndexBuilder::Task& task);
}
