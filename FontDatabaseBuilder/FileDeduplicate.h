#pragma once

#include "Common.h"

#include <string>
#include <vector>

struct DeduplicateResult
{
	std::vector<std::wstring> uniqueFiles;
	std::vector<std::wstring> duplicateFiles;
};

DeduplicateResult Deduplicate(const std::vector<std::wstring>& input, const std::vector<uint64_t>& inputSize,
                              std::atomic<size_t>& progress);
