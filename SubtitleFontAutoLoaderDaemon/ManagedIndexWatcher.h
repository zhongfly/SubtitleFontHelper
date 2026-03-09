#pragma once

#include "pch.h"
#include "IDaemon.h"
#include "ManagedIndexBuilder.h"

namespace sfh
{
	class ManagedIndexWatcher
	{
	public:
		struct Options
		{
			ManagedIndexBuilder::Task m_task;
			size_t m_workerCount = 1;
			std::chrono::milliseconds m_debounce = std::chrono::seconds(2);
			bool m_skipInitialSync = false;
		};

		ManagedIndexWatcher(IDaemon* daemon, Options options);
		~ManagedIndexWatcher();

		ManagedIndexWatcher(const ManagedIndexWatcher&) = delete;
		ManagedIndexWatcher(ManagedIndexWatcher&&) = delete;

		ManagedIndexWatcher& operator=(const ManagedIndexWatcher&) = delete;
		ManagedIndexWatcher& operator=(ManagedIndexWatcher&&) = delete;

	private:
		class Implementation;
		std::unique_ptr<Implementation> m_impl;
	};
}
