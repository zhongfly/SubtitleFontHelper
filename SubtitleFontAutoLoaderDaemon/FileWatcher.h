#pragma once

#include "IDaemon.h"

#include <string>
#include <vector>
#include <memory>

namespace sfh
{
	class FileWatcher
	{
	private:
		class Implementation;
		std::unique_ptr<Implementation> m_impl;

	public:
		FileWatcher(IDaemon* daemon);
		~FileWatcher();

		FileWatcher(const FileWatcher&) = delete;
		FileWatcher(FileWatcher&&) = delete;

		FileWatcher& operator=(const FileWatcher&) = delete;
		FileWatcher& operator=(FileWatcher&&) = delete;

		// Set files to monitor, debounce period in milliseconds
		void SetMonitorFiles(std::vector<std::wstring> files, uint32_t debounceMs = 500);
	};
}
