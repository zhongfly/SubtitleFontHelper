#pragma once

#include "ManagedIndexProgress.h"

#include <array>
#include <filesystem>
#include <mutex>
#include <string>

namespace sfh
{
	enum class ManagedIndexFailureStage
	{
		Analyze = 0,
		Hash,
		Group
	};

	class ManagedIndexFailureCollector
	{
	private:
		struct FailureSummary
		{
			size_t m_count = 0;
			std::wstring m_samplePath;
		};

		mutable std::mutex m_lock;
		std::array<FailureSummary, 3> m_failures;

	public:
		void Record(ManagedIndexFailureStage stage, const std::filesystem::path& path);
		bool HasFailures() const;
		std::wstring BuildToastMessage(ManagedIndexWorkType workType, const std::wstring& indexName) const;
	};
}
