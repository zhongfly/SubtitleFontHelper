#include "FontIndexCore.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef min
#undef max

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
#include <xxhash.h>
#include <wil/resource.h>

namespace FontIndexCore
{
	namespace
	{
		constexpr uint64_t SNAPSHOT_MAGIC = 0x314E5350484653ULL;
		constexpr uint32_t LEGACY_SNAPSHOT_VERSION = 1;
		constexpr uint32_t SNAPSHOT_VERSION_WITHOUT_CHECKSUM = 2;
		constexpr uint32_t SNAPSHOT_VERSION = 3;
		constexpr uint64_t MAX_SNAPSHOT_ENTRY_COUNT = 1000000;
		constexpr uint32_t MAX_SNAPSHOT_PATH_BYTES = 1024 * 1024;
		constexpr uint8_t SNAPSHOT_FLAG_HAS_CONTENT_HASH = 0x1;
		constexpr uint64_t SNAPSHOT_ENTRY_BASE_SIZE =
			sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint8_t);
		constexpr uint64_t SNAPSHOT_CONTENT_HASH_SIZE = sizeof(uint64_t) + sizeof(uint64_t);
		constexpr uint64_t SNAPSHOT_CHECKSUM_SIZE = sizeof(uint64_t);

		class SnapshotChecksum
		{
		private:
			XXH64_state_t* m_state = nullptr;

		public:
			SnapshotChecksum()
				: m_state(XXH64_createState())
			{
				if (!m_state)
				{
					throw std::runtime_error("failed to allocate snapshot checksum");
				}
				if (XXH64_reset(m_state, 0) != XXH_OK)
				{
					XXH64_freeState(m_state);
					m_state = nullptr;
					throw std::runtime_error("failed to initialize snapshot checksum");
				}
			}

			~SnapshotChecksum()
			{
				if (m_state)
				{
					XXH64_freeState(m_state);
				}
			}

			void Update(const void* data, size_t size)
			{
				if (size == 0)
				{
					return;
				}
				if (XXH64_update(m_state, data, size) != XXH_OK)
				{
					throw std::runtime_error("failed to update snapshot checksum");
				}
			}

			uint64_t Digest() const
			{
				return XXH64_digest(m_state);
			}
		};

		std::filesystem::path GetPersistedBaseDirectory(const std::filesystem::path& persistedPath)
		{
			const auto baseDirectory = persistedPath.parent_path();
			if (baseDirectory.empty())
			{
				return NormalizePath(std::filesystem::current_path());
			}
			return NormalizePath(baseDirectory);
		}

		std::filesystem::path ResolvePersistedPath(
			const std::filesystem::path& rawPath,
			const std::filesystem::path& baseDirectory)
		{
			if (rawPath.empty())
			{
				return {};
			}
			if (rawPath.has_root_name() || rawPath.has_root_directory())
			{
				return NormalizePath(rawPath);
			}
			return NormalizePath(baseDirectory / rawPath);
		}

		std::filesystem::path MakePersistedPath(
			const std::filesystem::path& rawPath,
			const std::filesystem::path& baseDirectory)
		{
			auto absolutePath = ResolvePersistedPath(rawPath, baseDirectory);
			auto relativePath = absolutePath.lexically_relative(baseDirectory);
			if (!relativePath.empty()
				&& !relativePath.has_root_name()
				&& !relativePath.has_root_directory())
			{
				return relativePath.lexically_normal();
			}
			return absolutePath;
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
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
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

		void WriteBytes(
			std::ofstream& stream,
			const void* data,
			size_t size,
			SnapshotChecksum* checksum = nullptr)
		{
			stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
			if (!stream)
			{
				throw std::runtime_error("failed to write snapshot");
			}
			if (checksum)
			{
				checksum->Update(data, size);
			}
		}

		template <typename T>
		void WriteScalar(std::ofstream& stream, const T& value, SnapshotChecksum* checksum = nullptr)
		{
			WriteBytes(stream, &value, sizeof(T), checksum);
		}

		void ReadBytes(
			std::ifstream& stream,
			void* data,
			size_t size,
			SnapshotChecksum* checksum = nullptr)
		{
			stream.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
			if (!stream)
			{
				throw std::runtime_error("failed to read snapshot");
			}
			if (checksum)
			{
				checksum->Update(data, size);
			}
		}

		template <typename T>
		T ReadScalar(std::ifstream& stream, SnapshotChecksum* checksum = nullptr)
		{
			T value{};
			ReadBytes(stream, &value, sizeof(T), checksum);
			return value;
		}

		uint64_t GetStreamSize(std::ifstream& stream)
		{
			stream.seekg(0, std::ios::end);
			const auto end = stream.tellg();
			if (end < 0)
			{
				throw std::runtime_error("failed to determine snapshot size");
			}
			stream.seekg(0, std::ios::beg);
			if (!stream)
			{
				throw std::runtime_error("failed to seek snapshot");
			}
			return static_cast<uint64_t>(end);
		}

		void ReadBytes(
			std::ifstream& stream,
			void* data,
			size_t size,
			uint64_t& remainingBytes,
			SnapshotChecksum* checksum = nullptr)
		{
			if (remainingBytes < size)
			{
				throw std::runtime_error("snapshot is truncated");
			}
			ReadBytes(stream, data, size, checksum);
			remainingBytes -= size;
		}

		template <typename T>
		T ReadScalar(std::ifstream& stream, uint64_t& remainingBytes, SnapshotChecksum* checksum = nullptr)
		{
			if (remainingBytes < sizeof(T))
			{
				throw std::runtime_error("snapshot is truncated");
			}
			auto value = ReadScalar<T>(stream, checksum);
			remainingBytes -= sizeof(T);
			return value;
		}
	}

	std::filesystem::path NormalizePath(const std::filesystem::path& path)
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

	bool TryCaptureDirectorySnapshotEntry(const std::filesystem::path& path, DirectorySnapshotEntry& entry)
	{
		if (!IsSupportedFontFile(path))
		{
			return false;
		}
		uint64_t fileSize = 0;
		uint64_t lastWriteTime = 0;
		if (!TryGetFileMetadata(path, fileSize, lastWriteTime))
		{
			return false;
		}
		entry.m_path = NormalizePath(path);
		entry.m_fileSize = fileSize;
		entry.m_lastWriteTime = lastWriteTime;
		return true;
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

			DirectorySnapshotEntry entry{};
			if (!TryCaptureDirectorySnapshotEntry(file.m_path, entry))
			{
				continue;
			}
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
		auto remainingBytes = GetStreamSize(stream);

		if (ReadScalar<uint64_t>(stream, remainingBytes) != SNAPSHOT_MAGIC)
		{
			throw std::runtime_error("invalid snapshot header");
		}
		const auto version = ReadScalar<uint32_t>(stream, remainingBytes);
		if (version != LEGACY_SNAPSHOT_VERSION
			&& version != SNAPSHOT_VERSION_WITHOUT_CHECKSUM
			&& version != SNAPSHOT_VERSION)
		{
			throw std::runtime_error("unsupported snapshot version");
		}

		std::optional<SnapshotChecksum> checksum;
		SnapshotChecksum* checksumContext = nullptr;
		if (version == SNAPSHOT_VERSION)
		{
			if (remainingBytes < SNAPSHOT_CHECKSUM_SIZE)
			{
				throw std::runtime_error("snapshot is truncated");
			}
			remainingBytes -= SNAPSHOT_CHECKSUM_SIZE;
			checksum.emplace();
			checksumContext = &*checksum;
		}

		DirectorySnapshot snapshot;
		const auto baseDirectory = GetPersistedBaseDirectory(snapshotPath);
		const auto count = ReadScalar<uint64_t>(stream, remainingBytes, checksumContext);
		if (count > MAX_SNAPSHOT_ENTRY_COUNT)
		{
			throw std::runtime_error("snapshot entry count is too large");
		}
		if (count > remainingBytes / SNAPSHOT_ENTRY_BASE_SIZE)
		{
			throw std::runtime_error("snapshot entry count exceeds remaining data");
		}
		snapshot.m_files.reserve(static_cast<size_t>(count));
		for (uint64_t i = 0; i < count; ++i)
		{
			const auto pathLength = ReadScalar<uint32_t>(stream, remainingBytes, checksumContext);
			if (pathLength > MAX_SNAPSHOT_PATH_BYTES)
			{
				throw std::runtime_error("snapshot path is too large");
			}
			if (remainingBytes < static_cast<uint64_t>(pathLength))
			{
				throw std::runtime_error("snapshot path length exceeds remaining data");
			}
			std::string utf8Path(pathLength, '\0');
			ReadBytes(stream, utf8Path.data(), pathLength, remainingBytes, checksumContext);

			DirectorySnapshotEntry entry;
			entry.m_path = ResolvePersistedPath(Utf8ToWide(utf8Path), baseDirectory);
			entry.m_fileSize = ReadScalar<uint64_t>(stream, remainingBytes, checksumContext);
			entry.m_lastWriteTime = ReadScalar<uint64_t>(stream, remainingBytes, checksumContext);
			const auto flags = ReadScalar<uint8_t>(stream, remainingBytes, checksumContext);
			if ((flags & ~SNAPSHOT_FLAG_HAS_CONTENT_HASH) != 0)
			{
				throw std::runtime_error("snapshot contains unsupported flags");
			}
			entry.m_hasContentHash = (flags & SNAPSHOT_FLAG_HAS_CONTENT_HASH) != 0;
			if (entry.m_hasContentHash)
			{
				if (remainingBytes < SNAPSHOT_CONTENT_HASH_SIZE)
				{
					throw std::runtime_error("snapshot content hash exceeds remaining data");
				}
				entry.m_contentHash.m_low64 = ReadScalar<uint64_t>(stream, remainingBytes, checksumContext);
				entry.m_contentHash.m_high64 = ReadScalar<uint64_t>(stream, remainingBytes, checksumContext);
			}
			snapshot.m_files.push_back(std::move(entry));
		}
		if (version == SNAPSHOT_VERSION)
		{
			const auto storedChecksum = ReadScalar<uint64_t>(stream, remainingBytes);
			if (storedChecksum != checksum->Digest())
			{
				throw std::runtime_error("snapshot checksum mismatch");
			}
		}
		if (remainingBytes != 0)
		{
			throw std::runtime_error("snapshot has unexpected trailing bytes");
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

		const auto baseDirectory = GetPersistedBaseDirectory(snapshotPath);
		const auto tempPath = snapshotPath.wstring() + L".tmp";
		auto cleanupTempFile = wil::scope_exit([&]()
		{
			std::error_code ec;
			std::filesystem::remove(tempPath, ec);
		});
		std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
		if (!stream)
		{
			throw std::runtime_error("failed to create snapshot");
		}

		SnapshotChecksum checksum;
		WriteScalar(stream, SNAPSHOT_MAGIC);
		WriteScalar(stream, SNAPSHOT_VERSION);
		if (snapshot.m_files.size() > MAX_SNAPSHOT_ENTRY_COUNT)
		{
			throw std::runtime_error("snapshot entry count is too large");
		}
		WriteScalar(stream, static_cast<uint64_t>(snapshot.m_files.size()), &checksum);
		for (const auto& entry : snapshot.m_files)
		{
			const auto persistedPath = MakePersistedPath(entry.m_path, baseDirectory);
			const auto utf8Path = WideToUtf8(persistedPath.wstring());
			if (utf8Path.size() > MAX_SNAPSHOT_PATH_BYTES)
			{
				throw std::runtime_error("snapshot path is too large");
			}
			WriteScalar(stream, static_cast<uint32_t>(utf8Path.size()), &checksum);
			WriteBytes(stream, utf8Path.data(), utf8Path.size(), &checksum);
			WriteScalar(stream, entry.m_fileSize, &checksum);
			WriteScalar(stream, entry.m_lastWriteTime, &checksum);
			WriteScalar(stream, static_cast<uint8_t>(entry.m_hasContentHash ? SNAPSHOT_FLAG_HAS_CONTENT_HASH : 0x0), &checksum);
			if (entry.m_hasContentHash)
			{
				WriteScalar(stream, entry.m_contentHash.m_low64, &checksum);
				WriteScalar(stream, entry.m_contentHash.m_high64, &checksum);
			}
		}
		WriteScalar(stream, checksum.Digest());
		stream.close();
		if (!stream)
		{
			throw std::runtime_error("failed to flush snapshot");
		}

		THROW_LAST_ERROR_IF(!MoveFileExW(
			tempPath.c_str(),
			snapshotPath.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
		cleanupTempFile.release();
	}
}
