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

	struct Hash128Value
	{
		uint64_t m_low64 = 0;
		uint64_t m_high64 = 0;
	};

	struct DirectorySnapshotEntry
	{
		std::filesystem::path m_path;
		uint64_t m_fileSize = 0;
		uint64_t m_lastWriteTime = 0;
		bool m_hasContentHash = false;
		Hash128Value m_contentHash{};
	};

	struct DirectorySnapshot
	{
		std::vector<DirectorySnapshotEntry> m_files;
	};

	struct BuildFontDatabaseStats
	{
		uint64_t m_totalElapsedMs = 0;
		uint64_t m_analyzeElapsedMs = 0;
		size_t m_fallbackCount = 0;
		size_t m_fontFaceCount = 0;
	};

	using FileOperationErrorCallback = std::function<void(const std::filesystem::path&, const std::string&)>;

	void ThrowIfCancelled(const std::function<bool()>& isCancelled);

	bool IsSupportedFontFile(const std::filesystem::path& path);
	std::filesystem::path NormalizePath(const std::filesystem::path& path);
	bool TryCaptureDirectorySnapshotEntry(const std::filesystem::path& path, DirectorySnapshotEntry& entry);
	std::vector<FontSourceFile> EnumerateFontFiles(
		const std::vector<std::filesystem::path>& sourceFolders,
		const std::function<bool()>& isCancelled = {});
	std::filesystem::path GetDirectorySnapshotPath(const std::filesystem::path& indexPath);
	DirectorySnapshot CaptureDirectorySnapshot(
		const std::vector<std::filesystem::path>& sourceFolders,
		const std::function<bool()>& isCancelled = {});
	DirectorySnapshot ReadDirectorySnapshot(const std::filesystem::path& snapshotPath);
	void WriteDirectorySnapshot(const std::filesystem::path& snapshotPath, const DirectorySnapshot& snapshot);
	void PopulateMissingContentHashes(
		DirectorySnapshot& snapshot,
		size_t workerCount,
		const std::function<bool()>& isCancelled = {},
		std::atomic<size_t>* progress = nullptr,
		const FileOperationErrorCallback& onError = {});
	std::vector<std::vector<size_t>> GroupEquivalentFiles(
		const DirectorySnapshot& snapshot,
		const std::function<bool()>& isCancelled = {},
		const FileOperationErrorCallback& onError = {});
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
		const FileOperationErrorCallback& onError = {},
		BuildFontDatabaseStats* stats = nullptr);
}
