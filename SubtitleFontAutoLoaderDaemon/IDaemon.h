#pragma once

#include <exception>
#include <filesystem>

namespace sfh
{
	class IDaemon
	{
	public:
		virtual ~IDaemon() = default;
		virtual void NotifyException(std::exception_ptr exception) = 0;
		virtual void NotifyExit() = 0;
		virtual void NotifyReload() = 0;
		virtual void NotifyManagedIndexBuildStarted(const std::filesystem::path& indexPath) = 0;
		virtual void NotifyManagedIndexBuildFinished(const std::filesystem::path& indexPath) = 0;
		virtual void NotifyManagedIndexBuilt(const std::filesystem::path& indexPath) = 0;
	};
}
