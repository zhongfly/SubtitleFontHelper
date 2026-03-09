#include "FontIndexCore.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef min
#undef max

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>
#include <wil/resource.h>

namespace FontIndexCore
{
	namespace
	{
		constexpr uint64_t SNAPSHOT_MAGIC = 0x314E5350484653ULL;
		constexpr uint32_t SNAPSHOT_VERSION = 1;

		void ThrowIfCancelled(const std::function<bool()>& isCancelled)
		{
			if (isCancelled && isCancelled())
			{
				throw std::runtime_error("Operation cancelled");
			}
		}

		std::wstring NormalizePath(const std::filesystem::path& path)
		{
			DWORD length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
			THROW_LAST_ERROR_IF(length == 0);
			std::wstring result(length, L'\0');
			THROW_LAST_ERROR_IF(GetFullPathNameW(path.c_str(), length, result.data(), nullptr) == 0);
			while (!result.empty() && result.back() == L'\0')
			{
				result.pop_back();
			}
			return result;
		}

		std::string WideToUtf8(const std::wstring& value)
		{
			if (value.empty())
			{
				return {};
			}

			const int length = WideCharToMultiByte(
				CP_UTF8,
				WC_ERR_INVALID_CHARS,
				value.c_str(),
				static_cast<int>(value.size()),
				nullptr,
				0,
				nullptr,
				nullptr);
			THROW_LAST_ERROR_IF(length == 0);

			std::string result(length, '\0');
			THROW_LAST_ERROR_IF(WideCharToMultiByte(
				CP_UTF8,
				WC_ERR_INVALID_CHARS,
				value.c_str(),
				static_cast<int>(value.size()),
				result.data(),
				length,
				nullptr,
				nullptr) == 0);
			return result;
		}

		std::wstring Utf8ToWide(const std::string& value)
		{
			if (value.empty())
			{
				return {};
			}

			const int length = MultiByteToWideChar(
				CP_UTF8,
				MB_ERR_INVALID_CHARS,
				value.data(),
				static_cast<int>(value.size()),
				nullptr,
				0);
			THROW_LAST_ERROR_IF(length == 0);

			std::wstring result(length, L'\0');
			THROW_LAST_ERROR_IF(MultiByteToWideChar(
				CP_UTF8,
				MB_ERR_INVALID_CHARS,
				value.data(),
				static_cast<int>(value.size()),
				result.data(),
				length) == 0);
			return result;
		}

		bool TryGetFileMetadata(const std::filesystem::path& path, uint64_t& fileSize, uint64_t& lastWriteTime)
		{
			WIN32_FILE_ATTRIBUTE_DATA data{};
			if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
			{
				return false;
			}

			ULARGE_INTEGER sizeValue{};
			sizeValue.HighPart = data.nFileSizeHigh;
			sizeValue.LowPart = data.nFileSizeLow;
			fileSize = sizeValue.QuadPart;

			ULARGE_INTEGER timeValue{};
			timeValue.HighPart = data.ftLastWriteTime.dwHighDateTime;
			timeValue.LowPart = data.ftLastWriteTime.dwLowDateTime;
			lastWriteTime = timeValue.QuadPart;
			return true;
		}

		template <typename T>
		void WriteScalar(std::ofstream& stream, const T& value)
		{
			stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
			if (!stream)
			{
				throw std::runtime_error("failed to write snapshot");
			}
		}

		template <typename T>
		T ReadScalar(std::ifstream& stream)
		{
			T value{};
			stream.read(reinterpret_cast<char*>(&value), sizeof(T));
			if (!stream)
			{
				throw std::runtime_error("failed to read snapshot");
			}
			return value;
		}
	}

	std::filesystem::path GetDirectorySnapshotPath(const std::filesystem::path& indexPath)
	{
		return indexPath.wstring() + L".state.bin";
	}

	DirectorySnapshot CaptureDirectorySnapshot(
		const std::vector<std::filesystem::path>& sourceFolders,
		const std::function<bool()>& isCancelled)
	{
		DirectorySnapshot snapshot;
		const auto files = EnumerateFontFiles(sourceFolders, isCancelled);
		snapshot.m_files.reserve(files.size());

		for (const auto& file : files)
		{
			ThrowIfCancelled(isCancelled);

			uint64_t fileSize = 0;
			uint64_t lastWriteTime = 0;
			if (!TryGetFileMetadata(file.m_path, fileSize, lastWriteTime))
			{
				continue;
			}

			DirectorySnapshotEntry entry;
			entry.m_path = NormalizePath(file.m_path);
			entry.m_fileSize = fileSize;
			entry.m_lastWriteTime = lastWriteTime;
			snapshot.m_files.push_back(std::move(entry));
		}

		std::sort(snapshot.m_files.begin(), snapshot.m_files.end(), [](const DirectorySnapshotEntry& lhs, const DirectorySnapshotEntry& rhs)
		{
			return lhs.m_path < rhs.m_path;
		});

		return snapshot;
	}

	DirectorySnapshot ReadDirectorySnapshot(const std::filesystem::path& snapshotPath)
	{
		if (!std::filesystem::exists(snapshotPath))
		{
			return {};
		}

		std::ifstream stream(snapshotPath, std::ios::binary);
		if (!stream)
		{
			throw std::runtime_error("failed to open snapshot");
		}

		if (ReadScalar<uint64_t>(stream) != SNAPSHOT_MAGIC)
		{
			throw std::runtime_error("invalid snapshot header");
		}
		if (ReadScalar<uint32_t>(stream) != SNAPSHOT_VERSION)
		{
			throw std::runtime_error("unsupported snapshot version");
		}

		DirectorySnapshot snapshot;
		const auto count = ReadScalar<uint64_t>(stream);
		snapshot.m_files.reserve(static_cast<size_t>(count));
		for (uint64_t i = 0; i < count; ++i)
		{
			const auto pathLength = ReadScalar<uint32_t>(stream);
			std::string utf8Path(pathLength, '\0');
			stream.read(utf8Path.data(), pathLength);
			if (!stream)
			{
				throw std::runtime_error("failed to read snapshot path");
			}

			DirectorySnapshotEntry entry;
			entry.m_path = Utf8ToWide(utf8Path);
			entry.m_fileSize = ReadScalar<uint64_t>(stream);
			entry.m_lastWriteTime = ReadScalar<uint64_t>(stream);
			const auto flags = ReadScalar<uint8_t>(stream);
			entry.m_hasContentHash = (flags & 0x1) != 0;
			if (entry.m_hasContentHash)
			{
				entry.m_contentHash.m_low64 = ReadScalar<uint64_t>(stream);
				entry.m_contentHash.m_high64 = ReadScalar<uint64_t>(stream);
			}
			snapshot.m_files.push_back(std::move(entry));
		}

		return snapshot;
	}

	void WriteDirectorySnapshot(const std::filesystem::path& snapshotPath, const DirectorySnapshot& snapshot)
	{
		const auto parent = snapshotPath.parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent);
		}

		const auto tempPath = snapshotPath.wstring() + L".tmp";
		std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
		if (!stream)
		{
			throw std::runtime_error("failed to create snapshot");
		}

		WriteScalar(stream, SNAPSHOT_MAGIC);
		WriteScalar(stream, SNAPSHOT_VERSION);
		WriteScalar(stream, static_cast<uint64_t>(snapshot.m_files.size()));
		for (const auto& entry : snapshot.m_files)
		{
			const auto utf8Path = WideToUtf8(entry.m_path.wstring());
			WriteScalar(stream, static_cast<uint32_t>(utf8Path.size()));
			stream.write(utf8Path.data(), static_cast<std::streamsize>(utf8Path.size()));
			if (!stream)
			{
				throw std::runtime_error("failed to write snapshot path");
			}
			WriteScalar(stream, entry.m_fileSize);
			WriteScalar(stream, entry.m_lastWriteTime);
			WriteScalar(stream, static_cast<uint8_t>(entry.m_hasContentHash ? 0x1 : 0x0));
			if (entry.m_hasContentHash)
			{
				WriteScalar(stream, entry.m_contentHash.m_low64);
				WriteScalar(stream, entry.m_contentHash.m_high64);
			}
		}
		stream.close();
		if (!stream)
		{
			throw std::runtime_error("failed to flush snapshot");
		}

		THROW_LAST_ERROR_IF(!MoveFileExW(
			tempPath.c_str(),
			snapshotPath.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
	}
}
