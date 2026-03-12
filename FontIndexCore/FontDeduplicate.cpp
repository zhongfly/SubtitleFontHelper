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

		XXH128Digest ToDigest(const Hash128Value& hash)
		{
			return { XXH128_hash_t{ hash.m_low64, hash.m_high64 } };
		}

		Hash128Value ToHashValue(const XXH128Digest& digest)
		{
			Hash128Value value;
			value.m_low64 = digest.m_value.low64;
			value.m_high64 = digest.m_value.high64;
			return value;
		}

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

		struct SnapshotRecord
		{
			size_t m_index = 0;
			const DirectorySnapshotEntry* m_entry = nullptr;
			XXH128Digest m_digest{};
			bool m_hashValid = false;
		};

		bool SnapshotRecordLess(const SnapshotRecord* lhs, const SnapshotRecord* rhs)
		{
			if (lhs->m_digest < rhs->m_digest)
			{
				return true;
			}
			if (rhs->m_digest < lhs->m_digest)
			{
				return false;
			}
			return lhs->m_entry->m_path < rhs->m_entry->m_path;
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

	void PopulateMissingContentHashes(
		DirectorySnapshot& snapshot,
		size_t workerCount,
		const std::function<bool()>& isCancelled,
		std::atomic<size_t>* progress,
		const FileOperationErrorCallback& onError)
	{
		std::unordered_map<uint64_t, std::vector<size_t>> sizeGroups;
		sizeGroups.reserve(snapshot.m_files.size());

		for (size_t i = 0; i < snapshot.m_files.size(); ++i)
		{
			sizeGroups[snapshot.m_files[i].m_fileSize].push_back(i);
		}

		std::vector<size_t> pendingHashes;
		pendingHashes.reserve(snapshot.m_files.size());
		for (const auto& [fileSize, indices] : sizeGroups)
		{
			if (indices.size() <= 1)
			{
				continue;
			}

			for (auto index : indices)
			{
				if (!snapshot.m_files[index].m_hasContentHash)
				{
					pendingHashes.push_back(index);
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
					if (isCancelled && isCancelled())
					{
						return;
					}

					size_t recordIndex = 0;
					{
						std::lock_guard lg(queueLock);
						if (nextPendingIndex == pendingHashes.size())
						{
							return;
						}
						recordIndex = pendingHashes[nextPendingIndex];
						++nextPendingIndex;
					}

					auto& entry = snapshot.m_files[recordIndex];
					try
					{
						entry.m_contentHash = ToHashValue(context.Calculate(entry.m_path.c_str()));
						entry.m_hasContentHash = true;
					}
					catch (const std::exception& e)
					{
						if (onError)
						{
							onError(entry.m_path, e.what());
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
	}

	std::vector<std::vector<size_t>> GroupEquivalentFiles(
		const DirectorySnapshot& snapshot,
		const std::function<bool()>& isCancelled,
		const FileOperationErrorCallback& onError)
	{
		std::unordered_map<uint64_t, std::vector<SnapshotRecord>> sizeGroups;
		sizeGroups.reserve(snapshot.m_files.size());
		for (size_t i = 0; i < snapshot.m_files.size(); ++i)
		{
			sizeGroups[snapshot.m_files[i].m_fileSize].push_back({ i, &snapshot.m_files[i] });
		}

		std::vector<std::vector<size_t>> result;
		result.reserve(snapshot.m_files.size());
		for (auto& [fileSize, records] : sizeGroups)
		{
			ThrowIfCancelled(isCancelled);

			if (records.size() == 1)
			{
				result.push_back({ records.front().m_index });
				continue;
			}

			std::vector<size_t> invalidHashEntries;
			std::vector<SnapshotRecord*> validRecords;
			validRecords.reserve(records.size());
			for (auto& record : records)
			{
				if (record.m_entry->m_hasContentHash)
				{
					record.m_digest = ToDigest(record.m_entry->m_contentHash);
					record.m_hashValid = true;
					validRecords.push_back(&record);
				}
				else
				{
					invalidHashEntries.push_back(record.m_index);
				}
			}

			for (auto index : invalidHashEntries)
			{
				result.push_back({ index });
			}

			if (validRecords.empty())
			{
				continue;
			}

			std::sort(validRecords.begin(), validRecords.end(), SnapshotRecordLess);

			size_t groupBegin = 0;
			while (groupBegin < validRecords.size())
			{
				size_t groupEnd = groupBegin + 1;
				while (groupEnd < validRecords.size()
					&& validRecords[groupBegin]->m_digest == validRecords[groupEnd]->m_digest)
				{
					++groupEnd;
				}

				std::vector<std::vector<SnapshotRecord*>> confirmedGroups;
				for (size_t i = groupBegin; i < groupEnd; ++i)
				{
					auto* candidate = validRecords[i];
					bool assigned = false;
					for (auto& confirmedGroup : confirmedGroups)
					{
						try
						{
							if (AreFilesByteEqual(
								confirmedGroup.front()->m_entry->m_path,
								candidate->m_entry->m_path,
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
								onError(candidate->m_entry->m_path, e.what());
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
					std::sort(confirmedGroup.begin(), confirmedGroup.end(), [](const SnapshotRecord* lhs, const SnapshotRecord* rhs)
					{
						return lhs->m_entry->m_path < rhs->m_entry->m_path;
					});

					std::vector<size_t> group;
					group.reserve(confirmedGroup.size());
					for (const auto* record : confirmedGroup)
					{
						group.push_back(record->m_index);
					}
					result.push_back(std::move(group));
				}

				groupBegin = groupEnd;
			}
		}

		std::sort(result.begin(), result.end(), [&](const std::vector<size_t>& lhs, const std::vector<size_t>& rhs)
		{
			return snapshot.m_files[lhs.front()].m_path < snapshot.m_files[rhs.front()].m_path;
		});
		return result;
	}

	DeduplicateResult DeduplicateFiles(
		const std::vector<FontSourceFile>& input,
		size_t workerCount,
		const std::function<bool()>& isCancelled,
		std::atomic<size_t>* progress,
		const FileOperationErrorCallback& onError)
	{
		DeduplicateResult result;
		DirectorySnapshot snapshot;
		snapshot.m_files.reserve(input.size());

		std::unordered_map<uint64_t, size_t> sizeGroupCounts;
		sizeGroupCounts.reserve(input.size());
		for (const auto& file : input)
		{
			++sizeGroupCounts[file.m_fileSize];
			DirectorySnapshotEntry entry;
			entry.m_path = file.m_path;
			entry.m_fileSize = file.m_fileSize;
			snapshot.m_files.push_back(std::move(entry));
		}

		if (progress)
		{
			for (const auto& entry : snapshot.m_files)
			{
				if (sizeGroupCounts[entry.m_fileSize] == 1)
				{
					++(*progress);
				}
			}
		}

		PopulateMissingContentHashes(snapshot, workerCount, isCancelled, progress, onError);
		auto groups = GroupEquivalentFiles(snapshot, isCancelled, onError);
		for (const auto& group : groups)
		{
			result.m_uniqueFiles.push_back(snapshot.m_files[group.front()].m_path);
			if (group.size() <= 1)
			{
				continue;
			}

			DeduplicateResult::DuplicateGroup duplicateGroup;
			duplicateGroup.m_keepFile = snapshot.m_files[group.front()].m_path;
			for (size_t i = 1; i < group.size(); ++i)
			{
				duplicateGroup.m_duplicateFiles.push_back(snapshot.m_files[group[i]].m_path);
			}
			result.m_duplicateGroups.push_back(std::move(duplicateGroup));
		}

		return result;
	}
}
