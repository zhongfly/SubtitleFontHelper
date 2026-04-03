#include "pch.h"

#include "Common.h"
#include "IDaemon.h"
#include "EventLog.h"
#include "TrayIcon.h"
#include "PersistantData.h"
#include "ConfigWatcher.h"
#include "ManagedIndexBuilder.h"
#include "ManagedIndexWatcher.h"
#include "QueryService.h"
#include "RpcServer.h"
#include "ProcessMonitor.h"
#include "Prefetch.h"
#include "ToastNotifier.h"
#include "../FontIndexCore/FontIndexCore.h"

#include <algorithm>
#include <cwctype>
#include <queue>
#include <filesystem>
#include <unordered_set>
#include <variant>

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
		std::wstring JoinPaths(const std::vector<std::filesystem::path>& paths)
		{
			if (paths.empty())
				return {};

			std::wstring result;
			for (size_t i = 0; i < paths.size(); ++i)
			{
				if (i != 0)
				{
					result += L"; ";
				}
				result += paths[i].wstring();
			}
			return result;
		}

		void TryLogManagedIndexConfiguration(
			const std::filesystem::path& configPath,
			const std::filesystem::path& indexPath,
			const std::vector<std::filesystem::path>& sourceFolders,
			bool needsBuild)
		{
			try
			{
				EventLog::GetInstance().LogDebugMessage(
					L"managed index config: config=\"%ls\" index=\"%ls\" sources=[%ls] action=%ls",
					configPath.c_str(),
					indexPath.c_str(),
					JoinPaths(sourceFolders).c_str(),
					needsBuild ? L"build" : L"load");
			}
			catch (...)
			{
			}
		}

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
			return directory / L"SubtitleFontHelper.toml";
		}

		void AppendConfigWatchFiles(
			std::vector<std::filesystem::path>& watchFiles,
			const std::filesystem::path& directory)
		{
			watchFiles.emplace_back(directory / L"SubtitleFontHelper.toml");
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

	class Daemon : public IDaemon, public IRpcRequestHandler, public IRpcFeedbackHandler
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
			ManagedIndexBuildStarted,
			ManagedIndexBuildFinished,
			ManagedIndexBuilt
		};

		struct Message
		{
			MessageType m_type;
			std::variant<
				std::nullopt_t,
				const std::vector<std::wstring>*,
				std::exception_ptr,
				std::filesystem::path> m_arg;
			uint64_t m_serviceGeneration = 0;
		};

		std::mutex m_queueLock;
		std::condition_variable m_queueCV;
		std::queue<Message> m_msgQueue;

		void EnqueueMessage(Message message)
		{
			std::unique_lock ul(m_queueLock);
			m_msgQueue.push(std::move(message));
			m_queueCV.notify_one();
		}

		class ServiceMessageSink : public IDaemon
		{
		private:
			enum class State
			{
				Pending = 0,
				Active
			};

			Daemon* m_owner;
			uint64_t m_generation;
			std::mutex m_lock;
			State m_state = State::Pending;
			std::queue<Message> m_pendingMessages;

			void EnqueueScoped(Message message)
			{
				std::unique_lock ul(m_lock);
				switch (m_state)
				{
				case State::Pending:
					m_pendingMessages.push(std::move(message));
					return;
				case State::Active:
					m_owner->EnqueueMessage(std::move(message));
					return;
				default:
					MarkUnreachable();
				}
			}

			void EnqueueGlobal(Message message)
			{
				m_owner->EnqueueMessage(std::move(message));
			}

			void EnqueueGlobalIfActive(Message message)
			{
				std::unique_lock ul(m_lock);
				if (m_state == State::Active)
				{
					m_owner->EnqueueMessage(std::move(message));
				}
			}

		public:
			explicit ServiceMessageSink(Daemon* owner, uint64_t generation)
				: m_owner(owner),
				  m_generation(generation)
			{
			}

			void ActivateAndFlush()
			{
				std::unique_lock ul(m_lock);
				if (m_state != State::Pending)
					return;
				std::unique_lock ownerLock(m_owner->m_queueLock);
				m_state = State::Active;
				while (!m_pendingMessages.empty())
				{
					m_owner->m_msgQueue.push(std::move(m_pendingMessages.front()));
					m_pendingMessages.pop();
				}
				m_owner->m_queueCV.notify_one();
			}

			void NotifyException(std::exception_ptr exception) override
			{
				EnqueueScoped({ MessageType::Exception, std::move(exception), m_generation });
			}

			void NotifyExit() override
			{
				EnqueueGlobalIfActive({ MessageType::Exit, std::nullopt, 0 });
			}

			void NotifyReload() override
			{
				EnqueueScoped({ MessageType::Reload, std::nullopt, m_generation });
			}

			void NotifyManagedIndexBuildStarted(const std::filesystem::path& indexPath) override
			{
				EnqueueScoped({ MessageType::ManagedIndexBuildStarted, indexPath, m_generation });
			}

			void NotifyManagedIndexBuildFinished(const std::filesystem::path& indexPath) override
			{
				EnqueueScoped({ MessageType::ManagedIndexBuildFinished, indexPath, m_generation });
			}

			void NotifyManagedIndexBuilt(const std::filesystem::path& indexPath) override
			{
				EnqueueScoped({ MessageType::ManagedIndexBuilt, indexPath, m_generation });
			}
		};

		struct Service
		{
			struct IndexSlot
			{
				std::filesystem::path m_path;
				bool m_isManaged = false;
			};

			std::unique_ptr<ServiceMessageSink> m_messageSink;
			std::unique_ptr<QueryService> m_queryService;
			std::unique_ptr<Prefetch> m_prefetch;
			std::unique_ptr<ConfigWatcher> m_configWatcher;
			std::vector<std::unique_ptr<ManagedIndexBuilder>> m_managedIndexBuilders;
			std::vector<std::unique_ptr<ManagedIndexWatcher>> m_managedIndexWatchers;
			std::vector<IndexSlot> m_indexSlots;
			std::vector<std::shared_ptr<ManagedIndexBuildProgressState>> m_managedIndexProgressStates;
		};

		std::mutex m_serviceAccessLock;
		std::unique_ptr<SystemTray> m_systemTray;
		std::unique_ptr<RpcServer> m_rpcServer;
		std::unique_ptr<ProcessMonitor> m_processMonitor;
		std::unique_ptr<Service> m_service;
		uint64_t m_activeServiceGeneration = 0;
		uint64_t m_nextServiceGeneration = 1;

		void TryLogReloadFailure(const char* message)
		{
			try
			{
				EventLog::GetInstance().LogDebugMessage("reload failed: %s", message);
			}
			catch (...)
			{
			}
		}

		void TryLogReloadFailure()
		{
			try
			{
				EventLog::GetInstance().LogDebugMessage(L"reload failed: unknown exception");
			}
			catch (...)
			{
			}
		}
		void NotifyException(std::exception_ptr exception) override
		{
			EnqueueMessage({ MessageType::Exception, std::move(exception), 0 });
		}

		void NotifyExit() override
		{
			EnqueueMessage({ MessageType::Exit, std::nullopt, 0 });
		}

		void NotifyReload() override
		{
			EnqueueMessage({ MessageType::Reload, std::nullopt, 0 });
		}

		void NotifyManagedIndexBuildStarted(const std::filesystem::path& indexPath) override
		{
			EnqueueMessage({ MessageType::ManagedIndexBuildStarted, indexPath, 0 });
		}

		void NotifyManagedIndexBuildFinished(const std::filesystem::path& indexPath) override
		{
			EnqueueMessage({ MessageType::ManagedIndexBuildFinished, indexPath, 0 });
		}

		void NotifyManagedIndexBuilt(const std::filesystem::path& indexPath) override
		{
			EnqueueMessage({ MessageType::ManagedIndexBuilt, indexPath, 0 });
		}

		bool ShouldIgnoreMessage(const Message& message) const
		{
			if (message.m_serviceGeneration == 0
				|| message.m_serviceGeneration == m_activeServiceGeneration)
			{
				return false;
			}
			return message.m_type != MessageType::Reload;
		}
	public:
		FontQueryResponse HandleRequest(const FontQueryRequest& request) override
		{
			std::lock_guard lg(m_serviceAccessLock);
			if (m_service == nullptr || m_service->m_queryService == nullptr)
				return {};
			return m_service->m_queryService->GetRpcRequestHandler()->HandleRequest(request);
		}

		void HandleFeedback(const FontQueryRequest& request) override
		{
			std::lock_guard lg(m_serviceAccessLock);
			if (m_service == nullptr || m_service->m_prefetch == nullptr)
				return;
			m_service->m_prefetch->GetRpcFeedbackHandler()->HandleFeedback(request);
		}

		int DaemonMain(const std::vector<std::wstring>& cmdline)
		{
			std::unique_lock ul(m_queueLock);
			m_msgQueue.push({ MessageType::Init, &cmdline, 0 });
			while (!m_msgQueue.empty())
			{
				Message msg = std::move(m_msgQueue.front());
				m_msgQueue.pop();
				if (ShouldIgnoreMessage(msg))
				{
					if (m_msgQueue.empty())
					{
						m_queueCV.wait(ul, [&]()
						{
							return !m_msgQueue.empty();
						});
					}
					continue;
				}
				auto [msgType, msgArg, msgGeneration] = std::move(msg);
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
					OnReload(cmdline);
					break;
				case MessageType::ManagedIndexBuildStarted:
					OnManagedIndexBuildStarted(std::get<std::filesystem::path>(msgArg));
					break;
				case MessageType::ManagedIndexBuildFinished:
					OnManagedIndexBuildFinished(std::get<std::filesystem::path>(msgArg));
					break;
				case MessageType::ManagedIndexBuilt:
					OnManagedIndexBuilt(std::get<std::filesystem::path>(msgArg));
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

		void UpdateManagedIndexBuildTrayState()
		{
			if (m_systemTray == nullptr)
			{
				return;
			}

			std::vector<std::shared_ptr<ManagedIndexBuildProgressState>> progressStates;
			{
				std::lock_guard lg(m_serviceAccessLock);
				if (m_service != nullptr)
				{
					progressStates = m_service->m_managedIndexProgressStates;
				}
			}

			m_systemTray->SetManagedIndexTrayProgress(
				CaptureManagedIndexTrayProgressSnapshot(progressStates));
		}

		void OnManagedIndexBuildStarted(const std::filesystem::path& indexPath)
		{
			(void)indexPath;
			UpdateManagedIndexBuildTrayState();
		}

		void OnManagedIndexBuildFinished(const std::filesystem::path& indexPath)
		{
			(void)indexPath;
			UpdateManagedIndexBuildTrayState();
		}

		void OnManagedIndexBuilt(const std::filesystem::path& indexPath)
		{
			if (m_service == nullptr || m_service->m_queryService == nullptr)
				return;
			auto slot = std::find_if(
				m_service->m_indexSlots.begin(),
				m_service->m_indexSlots.end(),
				[&](const Service::IndexSlot& item)
				{
					return item.m_isManaged && item.m_path == indexPath;
				});
			if (slot == m_service->m_indexSlots.end())
				return;

			auto dbs = LoadAvailableIndexDatabases(*m_service);
			m_service->m_queryService->Load(std::move(dbs));
		}

		void OnReload(const std::vector<std::wstring>& cmdline)
		{
			const bool hadService = m_service != nullptr;
			try
			{
				OnInit(cmdline);
			}
			catch (const std::exception& e)
			{
				if (!hadService)
					throw;
				TryLogReloadFailure(e.what());
				return;
			}
			catch (...)
			{
				if (!hadService)
					throw;
				TryLogReloadFailure();
				return;
			}
		}

		void OnInit(const std::vector<std::wstring>& cmdline)
		{
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

			const auto serviceGeneration = m_nextServiceGeneration++;
			newService->m_messageSink = std::make_unique<ServiceMessageSink>(this, serviceGeneration);
			std::vector<std::unique_ptr<FontDatabase>> dbs;
			std::vector<std::filesystem::path> watchFiles;
			std::vector<ManagedIndexBuilder::Task> managedIndexBuildTasks;
			std::vector<ManagedIndexWatcher::Options> managedIndexWatchOptions;
			std::vector<std::wstring> monitorProcess;
			AppendConfigWatchFiles(watchFiles, selfPath);
			for (auto& indexFile : cfg->m_indexFile)
			{
				auto indexPath = std::filesystem::absolute(indexFile.m_path).lexically_normal();
				const bool isManagedIndex = IsManagedIndex(indexFile);
				newService->m_indexSlots.push_back({ indexPath, isManagedIndex });

				ManagedIndexBuilder::Task managedTask;
				ManagedIndexWatcher::Options watchOptions;
				bool managedIndexNeedsBuild = false;
				if (isManagedIndex)
				{
					auto sourceFolders = ResolveSourceFolders(indexFile);
					managedTask.m_indexPath = indexPath;
					managedTask.m_snapshotPath = FontIndexCore::GetDirectorySnapshotPath(indexPath);
					managedTask.m_sourceFolders = sourceFolders;
					managedTask.m_progressState = std::make_shared<ManagedIndexBuildProgressState>();
					newService->m_managedIndexProgressStates.push_back(managedTask.m_progressState);
					managedTask.m_enableNotifications = cfg->managedIndexNotifications;
					managedTask.m_enableFailureNotifications = cfg->managedIndexFailureNotifications;
					watchOptions.m_workerCount = managedBuildWorkerCount;

					std::error_code ec;
					managedIndexNeedsBuild = !std::filesystem::exists(indexPath, ec) || ec;
					TryLogManagedIndexConfiguration(configPath, indexPath, sourceFolders, managedIndexNeedsBuild);
				}
				else
				{
					watchFiles.emplace_back(indexPath);
				}

				if (!managedIndexNeedsBuild)
				{
					try
					{
						dbs.emplace_back(ReadWithRetry([&]()
						{
							return FontDatabase::ReadFromFile(indexPath);
						}));
					}
					catch (...)
					{
						if (!isManagedIndex)
						{
							throw;
						}
						managedIndexNeedsBuild = true;
					}
				}

				if (!isManagedIndex)
				{
					continue;
				}

				if (managedIndexNeedsBuild)
				{
					watchOptions.m_task = managedTask;
					watchOptions.m_skipInitialSync = true;
					managedIndexBuildTasks.push_back(std::move(managedTask));
					managedIndexWatchOptions.push_back(std::move(watchOptions));
					continue;
				}

				watchOptions.m_task = std::move(managedTask);
				managedIndexWatchOptions.push_back(std::move(watchOptions));
			}
			newService->m_prefetch = std::make_unique<Prefetch>(
				newService->m_messageSink.get(),
				cfg->lruSize,
				lruCachePath,
				cfg->missingFontNotifications,
				cfg->missingFontIgnore,
				cfg->processMissingFontIgnore);
			newService->m_queryService = std::make_unique<QueryService>(newService->m_messageSink.get());
			newService->m_queryService->Load(std::move(dbs), false);
			for (auto& process : cfg->m_monitorProcess)
			{
				if (_wcsicmp(process.m_name.c_str(), L"rundll32.exe") == 0)
					throw std::logic_error("rundll32.exe is not allowed!");
				monitorProcess.emplace_back(process.m_name);
			}
			newService->m_configWatcher = std::make_unique<ConfigWatcher>(newService->m_messageSink.get(), std::move(watchFiles));
			for (auto& options : managedIndexWatchOptions)
			{
				newService->m_managedIndexWatchers.emplace_back(
					std::make_unique<ManagedIndexWatcher>(newService->m_messageSink.get(), std::move(options)));
			}
			for (auto& task : managedIndexBuildTasks)
			{
				newService->m_managedIndexBuilders.emplace_back(
					std::make_unique<ManagedIndexBuilder>(newService->m_messageSink.get(), std::move(task), managedBuildWorkerCount));
			}

			if (m_processMonitor == nullptr)
			{
				m_processMonitor = std::make_unique<ProcessMonitor>(
					this, std::chrono::milliseconds(cfg->wmiPollInterval));
				m_processMonitor->Start();
			}

			std::unique_ptr<Service> oldService;
			std::unique_ptr<RpcServer> newRpcServer;
			std::unique_ptr<SystemTray> newSystemTray;
			if (m_rpcServer == nullptr)
			{
				newRpcServer = std::make_unique<RpcServer>(this, this, this);
			}
			if (m_systemTray == nullptr)
			{
				newSystemTray = std::make_unique<SystemTray>(this);
			}
			{
				std::lock_guard lg(m_serviceAccessLock);
				m_processMonitor->SetOptions(
					std::move(monitorProcess),
					std::chrono::milliseconds(cfg->wmiPollInterval));
				m_activeServiceGeneration = serviceGeneration;
				oldService = std::move(m_service);
				m_service = std::move(newService);
				m_service->m_messageSink->ActivateAndFlush();
				if (newRpcServer)
				{
					m_rpcServer = std::move(newRpcServer);
					m_rpcServer->Start();
				}
				if (newSystemTray)
				{
					m_systemTray = std::move(newSystemTray);
					m_systemTray->Start();
				}
				m_service->m_queryService->PublishVersion();
			}
			UpdateManagedIndexBuildTrayState();
			m_systemTray->NotifyFinishLoad();
			oldService.reset();
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
