#include "pch.h"

#include "Common.h"
#include "IDaemon.h"
#include "TrayIcon.h"
#include "PersistantData.h"
#include "ConfigWatcher.h"
#include "ManagedIndexBuilder.h"
#include "QueryService.h"
#include "RpcServer.h"
#include "ProcessMonitor.h"
#include "Prefetch.h"
#include "ToastNotifier.h"
#include "../FontIndexCore/FontIndexCore.h"

#include <queue>
#include <variant>
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <strsafe.h>
#include <shellapi.h>
#include <wil/win32_helpers.h>

namespace sfh
{
	bool g_debugOutputEnabled = false;

	namespace
	{
		size_t GetDefaultWorkerCount()
		{
			auto concurrency = std::thread::hardware_concurrency();
			if (concurrency <= 1)
				return 1;
			return (std::max)(1u, concurrency / 2);
		}

		bool IsManagedIndex(const ConfigFile::IndexFileElement& indexFile)
		{
			return !indexFile.m_sourceFolders.empty();
		}

		std::vector<std::filesystem::path> ResolveSourceFolders(const ConfigFile::IndexFileElement& indexFile)
		{
			std::vector<std::filesystem::path> paths;
			paths.reserve(indexFile.m_sourceFolders.size());
			for (const auto& path : indexFile.m_sourceFolders)
			{
				paths.emplace_back(std::filesystem::absolute(path).lexically_normal());
			}
			return paths;
		}

		std::filesystem::path ResolveConfigPath(const std::filesystem::path& directory)
		{
			std::error_code ec;
			auto tomlPath = directory / L"SubtitleFontHelper.toml";
			if (std::filesystem::exists(tomlPath, ec) && !ec)
				return tomlPath;
			return directory / L"SubtitleFontHelper.xml";
		}

		void AppendConfigWatchFiles(
			std::vector<std::filesystem::path>& watchFiles,
			const std::filesystem::path& directory)
		{
			watchFiles.emplace_back(directory / L"SubtitleFontHelper.toml");
			watchFiles.emplace_back(directory / L"SubtitleFontHelper.xml");
		}
	}

	class SingleInstanceLock
	{
	private:
		wil::unique_mutex m_mutex;
	public:
		SingleInstanceLock()
		{
			std::wstring mutexName = LR"_(SubtitleFontAutoLoaderMutex-)_";
			mutexName += GetCurrentProcessUserSid();
			m_mutex.create(mutexName.c_str());
			if (WaitForSingleObject(m_mutex.get(), 0) != WAIT_OBJECT_0)
				throw std::runtime_error("Another instance is running!");
		}

		~SingleInstanceLock()
		{
			m_mutex.ReleaseMutex();
		}
	};

	class Daemon : public IDaemon
	{
	private:
		static constexpr size_t FILE_READ_RETRY_COUNT = 3;
		static constexpr auto FILE_READ_RETRY_INTERVAL = std::chrono::milliseconds(200);

		template <typename TFunc>
		static auto ReadWithRetry(TFunc&& reader)
		{
			std::exception_ptr lastException;
			for (size_t attempt = 0; attempt < FILE_READ_RETRY_COUNT; ++attempt)
			{
				try
				{
					return reader();
				}
				catch (...)
				{
					lastException = std::current_exception();
					if (attempt + 1 != FILE_READ_RETRY_COUNT)
						std::this_thread::sleep_for(FILE_READ_RETRY_INTERVAL);
				}
			}
			std::rethrow_exception(lastException);
		}

		enum class MessageType
		{
			Init = 0,
			Exception,
			Exit,
			Reload,
			ManagedIndexBuilt
		};

		struct Message
		{
			MessageType m_type;
			std::variant<
				std::nullopt_t,
				const std::vector<std::wstring>*,
				std::exception_ptr> m_arg;
		};

		std::mutex m_queueLock;
		std::condition_variable m_queueCV;
		std::queue<Message> m_msgQueue;

		struct Service
		{
			struct IndexSlot
			{
				std::filesystem::path m_path;
				bool m_isManaged = false;
			};

			std::unique_ptr<SystemTray> m_systemTray;
			std::unique_ptr<QueryService> m_queryService;
			std::unique_ptr<RpcServer> m_rpcServer;
			std::unique_ptr<ProcessMonitor> m_processMonitor;
			std::unique_ptr<Prefetch> m_prefetch;
			std::unique_ptr<ConfigWatcher> m_configWatcher;
			std::vector<std::unique_ptr<ManagedIndexBuilder>> m_managedIndexBuilders;
			std::vector<IndexSlot> m_indexSlots;
		};

		std::unique_ptr<Service> m_service;

		void NotifyException(std::exception_ptr exception) override
		{
			std::unique_lock ul(m_queueLock);
			m_msgQueue.emplace(MessageType::Exception, exception);
			m_queueCV.notify_one();
		}

		void NotifyExit() override
		{
			std::unique_lock ul(m_queueLock);
			m_msgQueue.emplace(MessageType::Exit, std::nullopt);
			m_queueCV.notify_one();
		}

		void NotifyReload() override
		{
			std::unique_lock ul(m_queueLock);
			m_msgQueue.emplace(MessageType::Reload, std::nullopt);
			m_queueCV.notify_one();
		}

		void NotifyManagedIndexBuilt() override
		{
			std::unique_lock ul(m_queueLock);
			m_msgQueue.emplace(MessageType::ManagedIndexBuilt, std::nullopt);
			m_queueCV.notify_one();
		}

	public:
		int DaemonMain(const std::vector<std::wstring>& cmdline)
		{
			std::unique_lock ul(m_queueLock);
			m_msgQueue.emplace(MessageType::Init, &cmdline);
			while (!m_msgQueue.empty())
			{
				auto [msgType, msgArg] = m_msgQueue.front();
				m_msgQueue.pop();
				ul.unlock();
				switch (msgType)
				{
				case MessageType::Init:
					OnInit(*std::get<const std::vector<std::wstring>*>(msgArg));
					break;
				case MessageType::Exception:
					OnException(std::get<std::exception_ptr>(msgArg));
					break;
				case MessageType::Exit:
					return 0;
				case MessageType::Reload:
					OnInit(cmdline);
					break;
				case MessageType::ManagedIndexBuilt:
					OnManagedIndexBuilt();
					break;
				default:
					MarkUnreachable();
				}
				ul.lock();
				if (m_msgQueue.empty())
				{
					m_queueCV.wait(ul, [&]()
					{
						return !m_msgQueue.empty();
					});
				}
			}
			MarkUnreachable();
		}

	private:
		std::vector<std::unique_ptr<FontDatabase>> LoadAvailableIndexDatabases(const Service& service)
		{
			std::vector<std::unique_ptr<FontDatabase>> dbs;
			dbs.reserve(service.m_indexSlots.size());
			for (const auto& slot : service.m_indexSlots)
			{
				if (slot.m_isManaged)
				{
					std::error_code ec;
					if (!std::filesystem::exists(slot.m_path, ec) || ec)
						continue;
				}

				dbs.emplace_back(ReadWithRetry([&]()
				{
					return FontDatabase::ReadFromFile(slot.m_path);
				}));
			}
			return dbs;
		}

		void OnManagedIndexBuilt()
		{
			if (m_service == nullptr || m_service->m_queryService == nullptr)
				return;

			auto dbs = LoadAvailableIndexDatabases(*m_service);
			m_service->m_queryService->Load(std::move(dbs));
		}

		void OnInit(const std::vector<std::wstring>& cmdline)
		{
			{
				std::unique_lock lg(m_queueLock);
				while (!m_msgQueue.empty())
					m_msgQueue.pop();
			}
			auto newService = std::make_unique<Service>();
			std::filesystem::path selfPath{wil::GetModuleFileNameW<wil::unique_hlocal_string>().get()};
			selfPath.remove_filename();
			auto configPath = ResolveConfigPath(selfPath);
			auto lruCachePath = selfPath / L"lruCache.txt";
			auto cfg = ReadWithRetry([&]()
			{
				return ConfigFile::ReadFromFile(configPath);
			});
			const auto managedBuildWorkerCount = GetDefaultWorkerCount();

			newService->m_systemTray = std::make_unique<SystemTray>(this);
			std::vector<std::unique_ptr<FontDatabase>> dbs;
			std::vector<std::filesystem::path> watchFiles;
			std::vector<ManagedIndexBuilder::Task> managedIndexBuildTasks;
			AppendConfigWatchFiles(watchFiles, selfPath);
			for (auto& indexFile : cfg->m_indexFile)
			{
				auto indexPath = std::filesystem::absolute(indexFile.m_path).lexically_normal();
				const bool isManagedIndex = IsManagedIndex(indexFile);
				newService->m_indexSlots.push_back({ indexPath, isManagedIndex });
				if (isManagedIndex)
				{
					std::error_code ec;
					if (!std::filesystem::exists(indexPath, ec) || ec)
					{
						ManagedIndexBuilder::Task task;
						task.m_indexPath = indexPath;
						task.m_snapshotPath = FontIndexCore::GetDirectorySnapshotPath(indexPath);
						task.m_sourceFolders = ResolveSourceFolders(indexFile);
						managedIndexBuildTasks.push_back(std::move(task));
						continue;
					}
				}

				if (!isManagedIndex)
					watchFiles.emplace_back(indexPath);
				dbs.emplace_back(ReadWithRetry([&]()
				{
					return FontDatabase::ReadFromFile(indexPath);
				}));
			}
			newService->m_prefetch = std::make_unique<Prefetch>(this, cfg->lruSize, lruCachePath);
			newService->m_queryService = std::make_unique<QueryService>(this);
			newService->m_rpcServer = std::make_unique<RpcServer>(
				this,
				newService->m_queryService->GetRpcRequestHandler(),
				newService->m_prefetch->GetRpcFeedbackHandler());
			newService->m_queryService->Load(std::move(dbs));
			newService->m_processMonitor = std::make_unique<ProcessMonitor>(
				this, std::chrono::milliseconds(cfg->wmiPollInterval));
			std::vector<std::wstring> monitorProcess;
			for (auto& process : cfg->m_monitorProcess)
			{
				monitorProcess.emplace_back(process.m_name);
			}
			newService->m_processMonitor->SetMonitorList(std::move(monitorProcess));
			newService->m_configWatcher = std::make_unique<ConfigWatcher>(this, std::move(watchFiles));
			m_service = std::move(newService);
			for (auto& task : managedIndexBuildTasks)
			{
				m_service->m_managedIndexBuilders.emplace_back(
					std::make_unique<ManagedIndexBuilder>(this, std::move(task), managedBuildWorkerCount));
			}
			m_service->m_systemTray->NotifyFinishLoad();
		}

		void OnException(std::exception_ptr exception)
		{
			std::rethrow_exception(exception);
		}
	};

	std::vector<std::wstring> GetCommandLineVector(const wchar_t* cmdline)
	{
		std::vector<std::wstring> ret;
		int argc = 0;
		wchar_t** argv = CommandLineToArgvW(cmdline, &argc);
		for (int i = 0; i < argc; ++i)
		{
			ret.emplace_back(argv[i]);
		}
		return ret;
	}

	void ProcessCommandLine(const std::vector<std::wstring>& cmdline)
	{
		for (size_t i = 1; i < cmdline.size(); ++i)
		{
			if (_wcsicmp(cmdline[i].c_str(), L"-debug") == 0)
			{
				g_debugOutputEnabled = true;
			}
		}
	}

	bool HasCommandLineFlag(const std::vector<std::wstring>& cmdline, const wchar_t* option)
	{
		for (size_t i = 1; i < cmdline.size(); ++i)
		{
			if (_wcsicmp(cmdline[i].c_str(), option) == 0)
			{
				return true;
			}
		}
		return false;
	}
}

int __stdcall wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
	// initialize locale for ACP
	setlocale(LC_ALL, "");
	SetEnvironmentVariableW(L"__NO_DETOUR", L"TRUE");
	auto cmdline = sfh::GetCommandLineVector(GetCommandLineW());
	sfh::ProcessCommandLine(cmdline);
	try
	{
		if (sfh::HasCommandLineFlag(cmdline, L"--toast-test"))
		{
			sfh::ToastNotifier().ShowTestToast();
			return 0;
		}

		sfh::SingleInstanceLock lock;
		return sfh::Daemon().DaemonMain(cmdline);
	}
	catch (std::exception& e)
	{
		MessageBoxA(nullptr, e.what(), "Error", MB_OK | MB_ICONERROR);
		return 1;
	}
}
