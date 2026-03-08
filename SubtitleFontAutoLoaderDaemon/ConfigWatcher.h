#pragma once

#include "pch.h"
#include "IDaemon.h"

namespace sfh
{
	class ConfigWatcher
	{
	private:
		class Implementation;
		std::unique_ptr<Implementation> m_impl;
	public:
		ConfigWatcher(IDaemon* daemon, std::vector<std::filesystem::path>&& files,
		              std::chrono::milliseconds debounce = std::chrono::milliseconds(500));
		~ConfigWatcher();

		ConfigWatcher(const ConfigWatcher&) = delete;
		ConfigWatcher(ConfigWatcher&&) = delete;

		ConfigWatcher& operator=(const ConfigWatcher&) = delete;
		ConfigWatcher& operator=(ConfigWatcher&&) = delete;
	};
}
