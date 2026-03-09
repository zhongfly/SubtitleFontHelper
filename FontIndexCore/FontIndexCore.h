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

	struct DeduplicateResult
	{
		struct DuplicateGroup
		{
			std::filesystem::path m_keepFile;
			std::vector<std::filesystem::path> m_duplicateFiles;
		};

		std::vector<std::filesystem::path> m_uniqueFiles;
		std::vector<DuplicateGroup> m_duplicateGroups;
	};

	using FileOperationErrorCallback = std::function<void(const std::filesystem::path&, const std::string&)>;

	bool IsSupportedFontFile(const std::filesystem::path& path);
	std::vector<FontSourceFile> EnumerateFontFiles(
		const std::vector<std::filesystem::path>& sourceFolders,
		const std::function<bool()>& isCancelled = {});
	DeduplicateResult DeduplicateFiles(
		const std::vector<FontSourceFile>& input,
		size_t workerCount,
		const std::function<bool()>& isCancelled = {},
		std::atomic<size_t>* progress = nullptr,
		const FileOperationErrorCallback& onError = {});
	sfh::FontDatabase BuildFontDatabase(
		const std::vector<std::filesystem::path>& fontFiles,
		size_t workerCount,
		const std::function<bool()>& isCancelled = {},
		std::atomic<size_t>* progress = nullptr,
		const FileOperationErrorCallback& onError = {});
}
