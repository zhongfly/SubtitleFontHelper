#include "pch.h"
#include "FileWatcher.h"
#include "Common.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <set>
#include <chrono>
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wil/resource.h>

namespace sfh
{
	class FileWatcher::Implementation
	{
	private:
		IDaemon* m_daemon;
		std::thread m_watcherThread;
		std::atomic<bool> m_shouldExit{ false };

		struct DirectoryWatch
		{
			wil::unique_handle hDir;
			std::vector<std::wstring> files; // Files in this directory to monitor
			std::vector<BYTE> buffer;
			OVERLAPPED overlapped{};
			wil::unique_event hEvent;
		};

		std::vector<std::unique_ptr<DirectoryWatch>> m_watches;
		uint32_t m_debounceMs = 500;

		std::mutex m_debounceMutex;
		std::set<std::wstring> m_pendingChanges;
		std::chrono::steady_clock::time_point m_lastChangeTime;

		void WatcherThreadProc()
		{
			try
			{
				std::vector<HANDLE> waitHandles;
				waitHandles.reserve(m_watches.size());

				// Start monitoring all directories
				for (auto& watch : m_watches)
				{
					StartDirectoryWatch(*watch);
					waitHandles.push_back(watch->hEvent.get());
				}

				while (!m_shouldExit)
				{
					DWORD waitResult = WaitForMultipleObjects(
						static_cast<DWORD>(waitHandles.size()),
						waitHandles.data(),
						FALSE,
						100); // 100ms timeout for periodic checks

					if (waitResult >= WAIT_OBJECT_0 && waitResult < WAIT_OBJECT_0 + waitHandles.size())
					{
						size_t idx = waitResult - WAIT_OBJECT_0;
						ProcessDirectoryChange(*m_watches[idx]);
						// Restart the watch
						StartDirectoryWatch(*m_watches[idx]);
					}

					// Check if debounce period has expired
					CheckDebounce();
				}
			}
			catch (...)
			{
				m_daemon->NotifyException(std::current_exception());
			}
		}

		void StartDirectoryWatch(DirectoryWatch& watch)
		{
			watch.buffer.resize(64 * 1024); // 64KB buffer
			ZeroMemory(&watch.overlapped, sizeof(watch.overlapped));
			watch.overlapped.hEvent = watch.hEvent.get();

			DWORD bytesReturned = 0;
			BOOL success = ReadDirectoryChangesW(
				watch.hDir.get(),
				watch.buffer.data(),
				static_cast<DWORD>(watch.buffer.size()),
				FALSE, // Don't watch subdirectories
				FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
				&bytesReturned,
				&watch.overlapped,
				nullptr);

			if (!success && GetLastError() != ERROR_IO_PENDING)
			{
				THROW_LAST_ERROR_MSG("Failed to start directory watch");
			}
		}

		void ProcessDirectoryChange(DirectoryWatch& watch)
		{
			DWORD bytesTransferred = 0;
			if (!GetOverlappedResult(watch.hDir.get(), &watch.overlapped, &bytesTransferred, FALSE))
			{
				// Error or operation was cancelled
				return;
			}

			if (bytesTransferred == 0)
			{
				// Buffer overflow or operation cancelled
				return;
			}

			// Parse the change notifications
			FILE_NOTIFY_INFORMATION* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(watch.buffer.data());
			while (true)
			{
				std::wstring filename(fni->FileName, fni->FileNameLength / sizeof(wchar_t));

				// Check if this file is one we're monitoring
				for (const auto& monitoredFile : watch.files)
				{
					std::filesystem::path monitoredPath(monitoredFile);
					if (_wcsicmp(monitoredPath.filename().c_str(), filename.c_str()) == 0)
					{
						// File changed - add to pending changes
						std::lock_guard<std::mutex> lock(m_debounceMutex);
						m_pendingChanges.insert(monitoredFile);
						m_lastChangeTime = std::chrono::steady_clock::now();
						break;
					}
				}

				if (fni->NextEntryOffset == 0)
					break;
				fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
					reinterpret_cast<BYTE*>(fni) + fni->NextEntryOffset);
			}
		}

		void CheckDebounce()
		{
			std::lock_guard<std::mutex> lock(m_debounceMutex);

			if (m_pendingChanges.empty())
				return;

			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastChangeTime).count();

			if (elapsed >= m_debounceMs)
			{
				// Debounce period has expired, trigger reload
				m_pendingChanges.clear();
				m_daemon->NotifyReload();
			}
		}

	public:
		Implementation(IDaemon* daemon)
			: m_daemon(daemon)
		{
		}

		~Implementation()
		{
			m_shouldExit = true;

			// Cancel all pending I/O operations
			for (auto& watch : m_watches)
			{
				CancelIo(watch->hDir.get());
			}

			if (m_watcherThread.joinable())
			{
				m_watcherThread.join();
			}
		}

		void SetMonitorFiles(std::vector<std::wstring> files, uint32_t debounceMs)
		{
			// Stop existing thread
			if (m_watcherThread.joinable())
			{
				m_shouldExit = true;
				for (auto& watch : m_watches)
				{
					CancelIo(watch->hDir.get());
				}
				m_watcherThread.join();
				m_shouldExit = false;
			}

			m_watches.clear();
			m_debounceMs = debounceMs;

			// Group files by directory
			std::map<std::wstring, std::vector<std::wstring>> dirMap;
			for (const auto& file : files)
			{
				std::filesystem::path path(file);
				std::wstring dir = path.parent_path().wstring();
				if (dir.empty())
					dir = L".";
				dirMap[dir].push_back(file);
			}

			// Create a watch for each directory
			for (auto& [dir, dirFiles] : dirMap)
			{
				auto watch = std::make_unique<DirectoryWatch>();

				watch->hDir.reset(CreateFileW(
					dir.c_str(),
					FILE_LIST_DIRECTORY,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr,
					OPEN_EXISTING,
					FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
					nullptr));

				if (!watch->hDir.is_valid())
				{
					THROW_LAST_ERROR_MSG("Failed to open directory for monitoring: %ws", dir.c_str());
				}

				watch->hEvent.create();
				watch->files = std::move(dirFiles);
				m_watches.push_back(std::move(watch));
			}

			// Start watcher thread
			if (!m_watches.empty())
			{
				m_watcherThread = std::thread([this]() { WatcherThreadProc(); });
			}
		}
	};

	FileWatcher::FileWatcher(IDaemon* daemon)
		: m_impl(std::make_unique<Implementation>(daemon))
	{
	}

	FileWatcher::~FileWatcher() = default;

	void FileWatcher::SetMonitorFiles(std::vector<std::wstring> files, uint32_t debounceMs)
	{
		m_impl->SetMonitorFiles(std::move(files), debounceMs);
	}
}
