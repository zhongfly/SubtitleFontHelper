#pragma once

#include "PersistantData.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace FontIndexCore
{
	struct FontSourceFile
	{
		std::filesystem::path m_path;
		uint64_t m_fileSize = 0;
	};

	using AnalyzeFontFileErrorCallback = std::function<void(const std::filesystem::path&, const std::string&)>;

	bool IsSupportedFontFile(const std::filesystem::path& path);
	std::vector<FontSourceFile> EnumerateFontFiles(
		const std::vector<std::filesystem::path>& sourceFolders,
		const std::function<bool()>& isCancelled = {});
	sfh::FontDatabase BuildFontDatabase(
		const std::vector<std::filesystem::path>& fontFiles,
		size_t workerCount,
		const std::function<bool()>& isCancelled = {},
		std::atomic<size_t>* progress = nullptr,
		const AnalyzeFontFileErrorCallback& onError = {});
}
