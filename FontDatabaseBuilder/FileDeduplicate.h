#pragma once

#include "Common.h"

#include <string>
#include <vector>

struct DeduplicateResult
{
	struct DuplicateGroup
	{
		std::wstring keepFile;
		std::vector<std::wstring> duplicateFiles;
	};

	std::vector<std::wstring> uniqueFiles;
	std::vector<DuplicateGroup> duplicateGroups;
};

DeduplicateResult Deduplicate(const std::vector<std::wstring>& input, const std::vector<uint64_t>& inputSize,
                              std::atomic<size_t>& progress);
