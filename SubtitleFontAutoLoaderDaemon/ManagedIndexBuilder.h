#pragma once

#include "pch.h"
#include "IDaemon.h"

#include <functional>

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

	size_t BuildManagedIndex(
		const ManagedIndexBuilder::Task& task,
		size_t workerCount,
		const std::function<bool()>& isCancelled = {});
	void ValidateManagedIndexSourceFolders(const ManagedIndexBuilder::Task& task);
}
