#include "pch.h"

#include "Common.h"
#include "IDaemon.h"
#include "TrayIcon.h"
#include "PersistantData.h"
#include "ConfigWatcher.h"
#include "QueryService.h"
#include "RpcServer.h"
#include "ProcessMonitor.h"
#include "Prefetch.h"

#include <queue>
#include <variant>
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <strsafe.h>
#include <shellapi.h>
#include <wil/win32_helpers.h>

#include "event.h"

namespace sfh
{
	bool g_debugOutputEnabled = false;

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
			Reload
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
			std::unique_ptr<SystemTray> m_systemTray;
			std::unique_ptr<QueryService> m_queryService;
			std::unique_ptr<RpcServer> m_rpcServer;
			std::unique_ptr<ProcessMonitor> m_processMonitor;
			std::unique_ptr<Prefetch> m_prefetch;
			std::unique_ptr<ConfigWatcher> m_configWatcher;
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
			auto configPath = selfPath / L"SubtitleFontHelper.xml";
			auto lruCachePath = selfPath / L"lruCache.txt";
			auto cfg = ReadWithRetry([&]()
			{
				return ConfigFile::ReadFromFile(configPath);
			});

			newService->m_systemTray = std::make_unique<SystemTray>(this);
			std::vector<std::unique_ptr<FontDatabase>> dbs;
			std::vector<std::filesystem::path> watchFiles;
			watchFiles.emplace_back(configPath);
			for (auto& indexFile : cfg->m_indexFile)
			{
				auto indexPath = std::filesystem::absolute(indexFile.m_path).lexically_normal();
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
			newService->m_systemTray->NotifyFinishLoad();
			m_service = std::move(newService);
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
		sfh::SingleInstanceLock lock;
		return sfh::Daemon().DaemonMain(cmdline);
	}
	catch (std::exception& e)
	{
		MessageBoxA(nullptr, e.what(), "Error", MB_OK | MB_ICONERROR);
		return 1;
	}
}
