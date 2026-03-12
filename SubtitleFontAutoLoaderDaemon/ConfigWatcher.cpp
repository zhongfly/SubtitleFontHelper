#include "pch.h"

#include "ConfigWatcher.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <wil/resource.h>

namespace
{
	struct FileSnapshot
	{
		bool m_exists = false;
		uint64_t m_size = 0;
		FILETIME m_lastWriteTime{};

		bool operator==(const FileSnapshot& rhs) const
		{
			return m_exists == rhs.m_exists
				&& m_size == rhs.m_size
				&& CompareFileTime(&m_lastWriteTime, &rhs.m_lastWriteTime) == 0;
		}
	};

	FileSnapshot CaptureSnapshot(const std::filesystem::path& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA data;
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
		{
			if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)
				return {};
			THROW_LAST_ERROR();
		}

		ULARGE_INTEGER fileSize;
		fileSize.HighPart = data.nFileSizeHigh;
		fileSize.LowPart = data.nFileSizeLow;

		FileSnapshot ret;
		ret.m_exists = true;
		ret.m_size = fileSize.QuadPart;
		ret.m_lastWriteTime = data.ftLastWriteTime;
		return ret;
	}

	class ChangeNotificationHandle
	{
	private:
		HANDLE m_handle = INVALID_HANDLE_VALUE;
	public:
		ChangeNotificationHandle() = default;

		explicit ChangeNotificationHandle(HANDLE handle)
			: m_handle(handle)
		{
		}

		~ChangeNotificationHandle()
		{
			reset();
		}

		ChangeNotificationHandle(const ChangeNotificationHandle&) = delete;
		ChangeNotificationHandle(ChangeNotificationHandle&& rhs) noexcept
			: m_handle(rhs.release())
		{
		}

		ChangeNotificationHandle& operator=(const ChangeNotificationHandle&) = delete;
		ChangeNotificationHandle& operator=(ChangeNotificationHandle&& rhs) noexcept
		{
			if (this != &rhs)
			{
				reset();
				m_handle = rhs.release();
			}
			return *this;
		}

		HANDLE get() const
		{
			return m_handle;
		}

		HANDLE release()
		{
			auto ret = m_handle;
			m_handle = INVALID_HANDLE_VALUE;
			return ret;
		}

		void reset(HANDLE handle = INVALID_HANDLE_VALUE)
		{
			if (m_handle != INVALID_HANDLE_VALUE)
				FindCloseChangeNotification(m_handle);
			m_handle = handle;
		}

		bool is_valid() const
		{
			return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr;
		}
	};
}

class sfh::ConfigWatcher::Implementation
{
private:
	struct WatchDirectory
	{
		std::filesystem::path m_path;
		ChangeNotificationHandle m_handle;
	};

	struct WatchFile
	{
		std::filesystem::path m_path;
		FileSnapshot m_snapshot;
	};

	struct WatchShard
	{
		std::vector<WatchDirectory> m_directories;
		std::vector<WatchFile> m_files;
	};

	std::vector<WatchShard> m_shards;
	std::chrono::milliseconds m_debounce;
	IDaemon* m_daemon;

	wil::unique_event m_exitEvent;
	std::vector<std::thread> m_workers;
	std::atomic<size_t> m_startedWorkers = 0;

	static constexpr DWORD WATCH_FILTER =
		FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
	static constexpr size_t MAX_DIRECTORIES_PER_SHARD = MAXIMUM_WAIT_OBJECTS - 1;
public:
	Implementation(IDaemon* daemon, std::vector<std::filesystem::path>&& files, std::chrono::milliseconds debounce)
		: m_debounce(debounce), m_daemon(daemon)
	{
		m_exitEvent.create();
		Initialize(std::move(files));
		m_workers.reserve(m_shards.size());
		for (auto& shard : m_shards)
		{
			m_workers.emplace_back([this, &shard]()
			{
				++m_startedWorkers;
				try
				{
					WorkerProcedure(shard);
				}
				catch (...)
				{
					m_daemon->NotifyException(std::current_exception());
				}
			});
		}
		while (m_startedWorkers.load() < m_workers.size())
			std::this_thread::yield();
	}

	~Implementation()
	{
		m_exitEvent.SetEvent();
		for (auto& worker : m_workers)
		{
			if (worker.joinable())
				worker.join();
		}
	}

private:
	void Initialize(std::vector<std::filesystem::path>&& files)
	{
		for (auto& file : files)
		{
			auto normalized = std::filesystem::absolute(file).lexically_normal();
			const bool isDuplicateFile = std::ranges::any_of(m_shards, [&](const WatchShard& shard)
			{
				return std::find_if(shard.m_files.begin(), shard.m_files.end(), [&](const WatchFile& watchFile)
				{
					return watchFile.m_path == normalized;
				}) != shard.m_files.end();
			});
			if (isDuplicateFile)
			{
				continue;
			}

			auto parentPath = normalized.parent_path();
			if (parentPath.empty())
				throw std::logic_error("watch file must have parent path");

			WatchShard* targetShard = nullptr;
			for (auto& shard : m_shards)
			{
				auto directory = std::find_if(shard.m_directories.begin(), shard.m_directories.end(),
				                              [&](const WatchDirectory& entry)
				{
					return entry.m_path == parentPath;
				});
				if (directory != shard.m_directories.end())
				{
					targetShard = &shard;
					break;
				}
			}

			if (targetShard == nullptr)
			{
				if (m_shards.empty() || m_shards.back().m_directories.size() >= MAX_DIRECTORIES_PER_SHARD)
				{
					m_shards.emplace_back();
				}
				targetShard = &m_shards.back();
				HANDLE handle = FindFirstChangeNotificationW(parentPath.c_str(), FALSE, WATCH_FILTER);
				THROW_LAST_ERROR_IF(handle == INVALID_HANDLE_VALUE || handle == nullptr);
				targetShard->m_directories.push_back({ parentPath, ChangeNotificationHandle(handle) });
			}

			targetShard->m_files.push_back({ normalized, CaptureSnapshot(normalized) });
		}
	}

	bool RefreshWatchedFiles(WatchShard& shard)
	{
		bool changed = false;
		for (auto& file : shard.m_files)
		{
			auto newSnapshot = CaptureSnapshot(file.m_path);
			if (!(newSnapshot == file.m_snapshot))
			{
				file.m_snapshot = newSnapshot;
				changed = true;
			}
		}
		return changed;
	}

	void WorkerProcedure(WatchShard& shard)
	{
		std::vector<HANDLE> waitList;
		waitList.reserve(shard.m_directories.size() + 1);
		waitList.push_back(m_exitEvent.get());
		for (auto& directory : shard.m_directories)
		{
			waitList.push_back(directory.m_handle.get());
		}

		std::optional<std::chrono::steady_clock::time_point> debounceDeadline;
		while (true)
		{
			DWORD timeout = INFINITE;
			if (debounceDeadline.has_value())
			{
				auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
					*debounceDeadline - std::chrono::steady_clock::now());
				timeout = remaining <= std::chrono::milliseconds::zero()
					? 0
					: static_cast<DWORD>(remaining.count());
			}

			auto waitResult = WaitForMultipleObjects(
				static_cast<DWORD>(waitList.size()),
				waitList.data(),
				FALSE,
				timeout);
			if (waitResult == WAIT_OBJECT_0)
				return;
			if (waitResult == WAIT_TIMEOUT)
			{
				if (debounceDeadline.has_value() && RefreshWatchedFiles(shard))
					m_daemon->NotifyReload();
				debounceDeadline.reset();
				continue;
			}
			if (waitResult >= WAIT_OBJECT_0 + 1 && waitResult < WAIT_OBJECT_0 + waitList.size())
			{
				auto index = waitResult - WAIT_OBJECT_0 - 1;
				THROW_LAST_ERROR_IF(FindNextChangeNotification(shard.m_directories[index].m_handle.get()) == FALSE);
				debounceDeadline = std::chrono::steady_clock::now() + m_debounce;
				continue;
			}
			THROW_LAST_ERROR_IF(waitResult == WAIT_FAILED);
			throw std::runtime_error("unexpected wait result");
		}
	}
};

sfh::ConfigWatcher::ConfigWatcher(IDaemon* daemon, std::vector<std::filesystem::path>&& files,
                                  std::chrono::milliseconds debounce)
	: m_impl(std::make_unique<Implementation>(daemon, std::move(files), debounce))
{
}

sfh::ConfigWatcher::~ConfigWatcher() = default;
