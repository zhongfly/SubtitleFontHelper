#include "FontIndexCore.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef min
#undef max

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <wil/resource.h>
#include <xxhash.h>

namespace FontIndexCore
{
	namespace
	{
		constexpr size_t FILE_BUFFER_SIZE = 8 * 1024 * 1024;

		void ThrowIfCancelled(const std::function<bool()>& isCancelled)
		{
			if (isCancelled && isCancelled())
			{
				throw std::runtime_error("Operation cancelled");
			}
		}

		wil::unique_virtualalloc_ptr<uint8_t> CreateFileBuffer()
		{
			wil::unique_virtualalloc_ptr<uint8_t> buffer;
			buffer.reset(static_cast<uint8_t*>(THROW_LAST_ERROR_IF_NULL(VirtualAlloc(
				nullptr,
				FILE_BUFFER_SIZE,
				MEM_COMMIT | MEM_RESERVE,
				PAGE_READWRITE))));
			return buffer;
		}

		struct XXH128Digest
		{
			XXH128_hash_t m_value{};

			bool operator<(const XXH128Digest& rhs) const
			{
				if (m_value.high64 != rhs.m_value.high64)
				{
					return m_value.high64 < rhs.m_value.high64;
				}
				return m_value.low64 < rhs.m_value.low64;
			}

			bool operator==(const XXH128Digest& rhs) const
			{
				return m_value.high64 == rhs.m_value.high64 && m_value.low64 == rhs.m_value.low64;
			}
		};

		class XXH3Context
		{
		private:
			std::unique_ptr<XXH3_state_t, decltype(&XXH3_freeState)> m_state;
			wil::unique_virtualalloc_ptr<uint8_t> m_buffer;

		public:
			XXH3Context()
				: m_state(XXH3_createState(), &XXH3_freeState)
				, m_buffer(CreateFileBuffer())
			{
				if (!m_state)
				{
					throw std::bad_alloc();
				}
			}

			XXH128Digest Calculate(const wchar_t* path)
			{
				wil::unique_hfile file(CreateFileW(
					path,
					GENERIC_READ,
					FILE_SHARE_READ,
					nullptr,
					OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
					nullptr));
				THROW_LAST_ERROR_IF(!file.is_valid());

				if (XXH3_128bits_reset(m_state.get()) != XXH_OK)
				{
					throw std::runtime_error("failed to initialize xxh3");
				}

				DWORD readBytes = 0;
				while (THROW_IF_WIN32_BOOL_FALSE(ReadFile(
					file.get(),
					m_buffer.get(),
					static_cast<DWORD>(FILE_BUFFER_SIZE),
					&readBytes,
					nullptr)))
				{
					if (readBytes == 0)
					{
						break;
					}
					if (XXH3_128bits_update(m_state.get(), m_buffer.get(), readBytes) != XXH_OK)
					{
						throw std::runtime_error("failed to update xxh3");
					}
				}

				return { XXH3_128bits_digest(m_state.get()) };
			}
		};

		struct FileRecord
		{
			const FontSourceFile* m_source = nullptr;
			XXH128Digest m_digest{};
			bool m_hashValid = false;
		};

		bool FileRecordLess(const FileRecord* lhs, const FileRecord* rhs)
		{
			if (lhs->m_digest < rhs->m_digest)
			{
				return true;
			}
			if (rhs->m_digest < lhs->m_digest)
			{
				return false;
			}
			return lhs->m_source->m_path < rhs->m_source->m_path;
		}

		bool AreFilesByteEqual(const std::filesystem::path& lhs, const std::filesystem::path& rhs, uint64_t expectedSize)
		{
			if (lhs == rhs)
			{
				return true;
			}

			wil::unique_hfile leftFile(CreateFileW(
				lhs.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
				nullptr));
			THROW_LAST_ERROR_IF(!leftFile.is_valid());

			wil::unique_hfile rightFile(CreateFileW(
				rhs.c_str(),
				GENERIC_READ,
				FILE_SHARE_READ,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
				nullptr));
			THROW_LAST_ERROR_IF(!rightFile.is_valid());

			auto leftBuffer = CreateFileBuffer();
			auto rightBuffer = CreateFileBuffer();

			uint64_t remaining = expectedSize;
			while (remaining > 0)
			{
				const DWORD chunkSize = static_cast<DWORD>(std::min<uint64_t>(remaining, FILE_BUFFER_SIZE));
				DWORD leftRead = 0;
				DWORD rightRead = 0;
				THROW_IF_WIN32_BOOL_FALSE(ReadFile(leftFile.get(), leftBuffer.get(), chunkSize, &leftRead, nullptr));
				THROW_IF_WIN32_BOOL_FALSE(ReadFile(rightFile.get(), rightBuffer.get(), chunkSize, &rightRead, nullptr));
				if (leftRead != rightRead)
				{
					return false;
				}
				if (leftRead == 0)
				{
					break;
				}
				if (memcmp(leftBuffer.get(), rightBuffer.get(), leftRead) != 0)
				{
					return false;
				}
				remaining -= leftRead;
			}

			return remaining == 0;
		}
	}

	DeduplicateResult DeduplicateFiles(
		const std::vector<FontSourceFile>& input,
		size_t workerCount,
		const std::function<bool()>& isCancelled,
		std::atomic<size_t>* progress,
		const FileOperationErrorCallback& onError)
	{
		DeduplicateResult result;
		std::unordered_map<uint64_t, std::vector<FileRecord>> groups;
		groups.reserve(input.size());

		for (const auto& file : input)
		{
			groups[file.m_fileSize].push_back({ &file });
		}

		std::vector<FileRecord*> pendingHashes;
		pendingHashes.reserve(input.size());
		for (auto& [fileSize, records] : groups)
		{
			if (records.size() == 1)
			{
				result.m_uniqueFiles.push_back(records.front().m_source->m_path);
				if (progress)
				{
					++(*progress);
				}
			}
			else
			{
				for (auto& record : records)
				{
					pendingHashes.push_back(&record);
				}
			}
		}

		const size_t workerCountValue = std::max<size_t>(1, workerCount);
		size_t nextPendingIndex = 0;
		std::mutex queueLock;

		std::vector<std::thread> workers;
		workers.reserve(workerCountValue);
		for (size_t i = 0; i < workerCountValue; ++i)
		{
			workers.emplace_back([&]()
			{
				XXH3Context context;
				while (true)
				{
					ThrowIfCancelled(isCancelled);

					FileRecord* record = nullptr;
					{
						std::lock_guard lg(queueLock);
						if (nextPendingIndex == pendingHashes.size())
						{
							return;
						}
						record = pendingHashes[nextPendingIndex];
						++nextPendingIndex;
					}

					try
					{
						record->m_digest = context.Calculate(record->m_source->m_path.c_str());
						record->m_hashValid = true;
					}
					catch (const std::exception& e)
					{
						if (onError)
						{
							onError(record->m_source->m_path, e.what());
						}
					}

					if (progress)
					{
						++(*progress);
					}
				}
			});
		}

		for (auto& worker : workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}

		ThrowIfCancelled(isCancelled);

		for (auto& [fileSize, records] : groups)
		{
			if (records.size() <= 1)
			{
				continue;
			}

			std::vector<FileRecord*> validRecords;
			validRecords.reserve(records.size());
			for (auto& record : records)
			{
				if (record.m_hashValid)
				{
					validRecords.push_back(&record);
				}
			}

			if (validRecords.empty())
			{
				continue;
			}

			std::sort(validRecords.begin(), validRecords.end(), FileRecordLess);

			size_t groupBegin = 0;
			while (groupBegin < validRecords.size())
			{
				size_t groupEnd = groupBegin + 1;
				while (groupEnd < validRecords.size()
					&& validRecords[groupBegin]->m_digest == validRecords[groupEnd]->m_digest)
				{
					++groupEnd;
				}

				std::vector<std::vector<FileRecord*>> confirmedGroups;
				for (size_t i = groupBegin; i < groupEnd; ++i)
				{
					auto* candidate = validRecords[i];
					bool assigned = false;
					for (auto& confirmedGroup : confirmedGroups)
					{
						try
						{
							if (AreFilesByteEqual(
								confirmedGroup.front()->m_source->m_path,
								candidate->m_source->m_path,
								fileSize))
							{
								confirmedGroup.push_back(candidate);
								assigned = true;
								break;
							}
						}
						catch (const std::exception& e)
						{
							if (onError)
							{
								onError(candidate->m_source->m_path, e.what());
							}
						}
					}

					if (!assigned)
					{
						confirmedGroups.push_back({ candidate });
					}
				}

				for (auto& confirmedGroup : confirmedGroups)
				{
					std::sort(confirmedGroup.begin(), confirmedGroup.end(), [](const FileRecord* lhs, const FileRecord* rhs)
					{
						return lhs->m_source->m_path < rhs->m_source->m_path;
					});

					result.m_uniqueFiles.push_back(confirmedGroup.front()->m_source->m_path);
					if (confirmedGroup.size() > 1)
					{
						DeduplicateResult::DuplicateGroup duplicateGroup;
						duplicateGroup.m_keepFile = confirmedGroup.front()->m_source->m_path;
						for (size_t i = 1; i < confirmedGroup.size(); ++i)
						{
							duplicateGroup.m_duplicateFiles.push_back(confirmedGroup[i]->m_source->m_path);
						}
						result.m_duplicateGroups.push_back(std::move(duplicateGroup));
					}
				}

				groupBegin = groupEnd;
			}
		}

		return result;
	}
}
