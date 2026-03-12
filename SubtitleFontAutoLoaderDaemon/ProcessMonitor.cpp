#include "pch.h"

#include "Common.h"
#include "ProcessMonitor.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WbemIdl.h>
#pragma comment(lib, "wbemuuid.lib")

#include <queue>
#include <unordered_map>

#include <wil/win32_helpers.h>
#include <wil/resource.h>
#include <wil/com.h>

namespace
{
	std::once_flag g_comSecurityInitFlag;

	void EnsureComSecurityInitialized()
	{
		std::call_once(g_comSecurityInitFlag, []()
		{
			auto hr = CoInitializeSecurity(
				nullptr,
				-1,
				nullptr,
				nullptr,
				RPC_C_AUTHN_LEVEL_DEFAULT,
				RPC_C_IMP_LEVEL_IMPERSONATE,
				nullptr,
				EOAC_NONE,
				nullptr);
			if (FAILED(hr) && hr != RPC_E_TOO_LATE)
				THROW_IF_FAILED(hr);
		});
	}
}

class sfh::ProcessMonitor::Implementation
{
private:
	struct ConfigSnapshot
	{
		std::vector<std::wstring> m_list;
		std::chrono::milliseconds m_interval = std::chrono::milliseconds(0);
	};

	struct EventRecord
	{
		uint64_t m_revision = 0;
		wil::com_ptr<IWbemClassObject> m_object;
	};

	std::mutex m_accessLock;
	std::unordered_map<uint64_t, ConfigSnapshot> m_configSnapshots;
	uint64_t m_nextConfigRevision = 1;
	std::atomic<uint64_t> m_requestedConfigRevision = 0;
	std::mutex m_configStateLock;
	std::condition_variable m_configStateCV;
	uint64_t m_appliedConfigRevision = 0;
	uint64_t m_failedConfigRevision = 0;
	std::exception_ptr m_failedConfigException;

	std::atomic<size_t> m_checkPoint = 0;
	std::atomic<bool> m_exitFlag = false;
	std::thread m_worker;
	wil::unique_event m_startEvent;

	IDaemon* m_daemon;

	constexpr static const wchar_t* QUERY_STRING_TEMPLATE =
		L"SELECT * FROM __InstanceCreationEvent WITHIN %.3f WHERE TargetInstance ISA 'Win32_Process'";

	class EventSink : public IWbemObjectSink
	{
	private:
		std::atomic<ULONG> m_refCount;
		Implementation* m_impl;
		uint64_t m_revision;
	public:
		EventSink(Implementation* impl, uint64_t revision)
			: m_refCount(1), m_impl(impl), m_revision(revision)
		{
		}

		~EventSink()
		{
		}

		virtual ULONG STDMETHODCALLTYPE AddRef()
		{
			return ++m_refCount;
		}

		virtual ULONG STDMETHODCALLTYPE Release()
		{
			auto i = --m_refCount;
			if (i == 0)
				delete this;
			return i;
		}

		virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv)
		{
			if (riid == IID_IUnknown)
			{
				*ppv = static_cast<IUnknown*>(this);
				AddRef();
				return WBEM_S_NO_ERROR;
			}
			if (riid == IID_IWbemObjectSink)
			{
				*ppv = static_cast<IWbemObjectSink*>(this);
				AddRef();
				return WBEM_S_NO_ERROR;
			}
			return E_NOINTERFACE;
		}

		virtual HRESULT STDMETHODCALLTYPE Indicate(
			LONG lObjectCount,
			IWbemClassObject __RPC_FAR* __RPC_FAR* apObjArray
		)
		{
			m_impl->PostNewEvent(m_revision, apObjArray, lObjectCount);
			return WBEM_S_NO_ERROR;
		}

		virtual HRESULT STDMETHODCALLTYPE SetStatus(
			LONG lFlags,
			HRESULT hResult,
			BSTR strParam,
			IWbemClassObject __RPC_FAR* pObjParam
		)
		{
			return WBEM_S_NO_ERROR;
		}
	};

	std::queue<EventRecord> m_queue;
	std::mutex m_queueMutex;
	std::condition_variable m_queueCV;

public:
	Implementation(IDaemon* daemon, std::chrono::milliseconds interval)
		: m_daemon(daemon)
	{
		m_configSnapshots.emplace(0, ConfigSnapshot{ {}, interval });
		m_startEvent.create(wil::EventOptions::ManualReset);
		m_worker = std::thread([&]()
		{
			++m_checkPoint;
			if (WaitForSingleObject(m_startEvent.get(), INFINITE) != WAIT_OBJECT_0 || m_exitFlag.load())
				return;
			try
			{
				WorkerProcedure();
			}
			catch (...)
			{
				{
					std::lock_guard lg(m_configStateLock);
					m_failedConfigRevision = m_requestedConfigRevision.load();
					m_failedConfigException = std::current_exception();
				}
				m_configStateCV.notify_all();
				daemon->NotifyException(std::current_exception());
			}
		});
		while (m_checkPoint.load() == 0)
			std::this_thread::yield();
	}

	~Implementation()
	{
		m_exitFlag = true;
		m_startEvent.SetEvent();
		m_queueCV.notify_one();
		m_configStateCV.notify_all();
		if (m_worker.joinable())
			m_worker.join();
	}

	void Start()
	{
		m_startEvent.SetEvent();
	}

	void SetOptions(std::vector<std::wstring>&& list, std::chrono::milliseconds interval)
	{
		uint64_t revision = 0;
		{
			std::lock_guard lg(m_accessLock);
			revision = m_nextConfigRevision++;
			m_configSnapshots[revision] = { std::move(list), interval };
		}
		{
			std::lock_guard lg(m_configStateLock);
			m_requestedConfigRevision.store(revision);
		}
		m_queueCV.notify_one();
		std::unique_lock ul(m_configStateLock);
		m_configStateCV.wait(ul, [&]()
		{
			return m_exitFlag.load()
				|| m_appliedConfigRevision == revision
				|| m_failedConfigRevision == revision;
		});
		if (m_exitFlag.load())
			throw std::runtime_error("process monitor stopped before applying configuration");
		if (m_failedConfigRevision == revision)
		{
			auto exception = m_failedConfigException;
			ul.unlock();
			{
				std::lock_guard lg(m_accessLock);
				m_configSnapshots.erase(revision);
			}
			std::rethrow_exception(exception);
		}
	}

private:
	ConfigSnapshot GetConfigSnapshot(uint64_t revision)
	{
		std::lock_guard lg(m_accessLock);
		if (auto iter = m_configSnapshots.find(revision); iter != m_configSnapshots.end())
			return iter->second;
		return {};
	}

	wil::com_ptr<IWbemObjectSink> CreateSinkStub(IWbemServices* wbemService, uint64_t revision)
	{
		wil::com_ptr<EventSink> sink(new EventSink(this, revision));
		wil::com_ptr<IWbemObjectSink> sinkStub;

		wil::com_ptr<IUnsecuredApartment> apartment;
		THROW_IF_FAILED(CoCreateInstance(
			CLSID_UnsecuredApartment,
			NULL,
			CLSCTX_LOCAL_SERVER,
			IID_IUnsecuredApartment,
			apartment.put_void()));

		auto wbemApartment = apartment.try_query<IWbemUnsecuredApartment>();
		if (wbemApartment)
		{
			wil::com_ptr<IUnknown> unkStub;
			THROW_IF_FAILED(wbemApartment->CreateSinkStub(
				sink.get(),
				WBEM_FLAG_UNSECAPP_DEFAULT_CHECK_ACCESS,
				nullptr,
				reinterpret_cast<IWbemObjectSink**>(unkStub.put())));
			sinkStub = unkStub.query<IWbemObjectSink>();
		}
		else
		{
			wil::com_ptr<IUnknown> unkStub;
			THROW_IF_FAILED(apartment->CreateObjectStub(sink.get(), unkStub.put_unknown()));
			sinkStub = unkStub.query<IWbemObjectSink>();
		}

		auto snapshot = GetConfigSnapshot(revision);
		wchar_t queryString[128];
		swprintf(queryString, std::extent_v<decltype(queryString)>, QUERY_STRING_TEMPLATE,
		         static_cast<double>(snapshot.m_interval.count()) / 1000.0);

		THROW_IF_FAILED(wbemService->ExecNotificationQueryAsync(
			wil::make_bstr(L"WQL").get(),
			wil::make_bstr(queryString).get(),
			WBEM_FLAG_SEND_STATUS,
			nullptr,
			sinkStub.get()
		));
		return sinkStub;
	}

	void WorkerProcedure()
	{
		auto com = wil::CoInitializeEx();
		EnsureComSecurityInitialized();

		auto wbemLocator = wil::CoCreateInstance<IWbemLocator>(CLSID_WbemLocator);
		wil::com_ptr<IWbemServices> wbemService;
		THROW_IF_FAILED(wbemLocator->ConnectServer(
			wil::make_bstr(L"ROOT\\CIMV2").get(),
			nullptr,
			nullptr,
			nullptr,
			0,
			nullptr,
			nullptr,
			wbemService.put()
		));

		THROW_IF_FAILED(CoSetProxyBlanket(
			wbemService.query<IUnknown>().get(),
			RPC_C_AUTHN_DEFAULT,
			RPC_C_AUTHZ_DEFAULT,
			nullptr,
			RPC_C_AUTHN_LEVEL_DEFAULT,
			RPC_C_IMP_LEVEL_IMPERSONATE,
			nullptr,
			EOAC_NONE
		));

		wil::com_ptr<IWbemObjectSink> sinkStub;
		uint64_t subscribedRevision = 0;

		while (!m_exitFlag.load())
		{
			const auto requestedRevision = m_requestedConfigRevision.load();
			if (requestedRevision != 0 && requestedRevision != subscribedRevision)
			{
				try
				{
					auto newSinkStub = CreateSinkStub(wbemService.get(), requestedRevision);
					auto oldSinkStub = std::move(sinkStub);
					sinkStub = std::move(newSinkStub);
					subscribedRevision = requestedRevision;
					{
						std::lock_guard lg(m_configStateLock);
						m_appliedConfigRevision = requestedRevision;
						m_failedConfigException = nullptr;
					}
					m_configStateCV.notify_all();
					++m_checkPoint;
					if (oldSinkStub)
						wbemService->CancelAsyncCall(oldSinkStub.get());
				}
				catch (...)
				{
					{
						std::lock_guard lg(m_configStateLock);
						m_failedConfigRevision = requestedRevision;
						m_failedConfigException = std::current_exception();
						m_requestedConfigRevision.store(subscribedRevision);
					}
					m_configStateCV.notify_all();
				}
				continue;
			}

			std::unique_lock lock(m_queueMutex);
			m_queueCV.wait(lock, [&]()
			{
				return !m_queue.empty()
					|| m_exitFlag
					|| m_requestedConfigRevision.load() != subscribedRevision;
			});
			if (m_exitFlag)
				break;
			if (m_requestedConfigRevision.load() != subscribedRevision)
				continue;
			if (m_queue.empty())
				continue;

			EventRecord eventRecord = std::move(m_queue.front());
			m_queue.pop();
			lock.unlock();
			try
			{
				auto snapshot = GetConfigSnapshot(eventRecord.m_revision);
				HandleProcessCreation(eventRecord.m_object.get(), wbemService.get(), snapshot.m_list);
			}
			catch (...)
			{
			}
		}

		if (sinkStub)
			wbemService->CancelAsyncCall(sinkStub.get());
	}

	void PostNewEvent(uint64_t revision, IWbemClassObject** events, size_t count)
	{
		std::lock_guard lg(m_queueMutex);
		for (size_t i = 0; i < count; ++i)
			m_queue.push({ revision, events[i] });
		m_queueCV.notify_one();
	}

	void HandleProcessCreation(
		IWbemClassObject* object,
		IWbemServices* service,
		const std::vector<std::wstring>& monitorList)
	{
		wil::unique_variant targetInstanceVariant;
		THROW_IF_FAILED(object->Get(L"TargetInstance", 0, targetInstanceVariant.addressof(), nullptr, nullptr));
		auto targetInstance = wil::com_query<IWbemClassObject>(targetInstanceVariant.punkVal);
		wil::unique_variant processIdVariant;
		wil::unique_variant executablePathVariant;
		THROW_IF_FAILED(targetInstance->Get(L"ProcessId", 0, processIdVariant.addressof(), nullptr, nullptr));
		THROW_IF_FAILED(targetInstance->Get(L"ExecutablePath", 0, executablePathVariant.addressof(), nullptr, nullptr));
		bool filteredOut = true;
		const wchar_t* executablePath = executablePathVariant.bstrVal;
		if (executablePath == nullptr)
			return;
		size_t exePathLength = wcslen(executablePath);
		for (auto& name : monitorList)
		{
			const wchar_t* comparisonStart = executablePath + exePathLength - name.size();
			if ((comparisonStart == executablePath
					|| (comparisonStart > executablePath
						&& (*(comparisonStart - 1) == L'\\' || *(comparisonStart - 1) == L'/')))
				&& _wcsicmp(comparisonStart, name.c_str()) == 0)
			{
				filteredOut = false;
				break;
			}
		}

		// check the process's owner
		if (!filteredOut)
		{
			wil::unique_variant objectPath;
			THROW_IF_FAILED(targetInstance->Get(L"__PATH", 0, objectPath.addressof(), nullptr, nullptr));
			wil::com_ptr<IWbemClassObject> outputParams;
			THROW_IF_FAILED(service->ExecMethod(
				objectPath.bstrVal,
				wil::make_bstr(L"GetOwnerSid").get(),
				0,
				nullptr,
				nullptr,
				outputParams.put(),
				nullptr));
			wil::unique_variant sidString;
			THROW_IF_FAILED(outputParams->Get(L"Sid", 0, sidString.addressof(), nullptr, nullptr));
			auto selfSid = GetCurrentProcessUserSid();
			if (wcscmp(sidString.bstrVal, selfSid.c_str()) != 0)
				filteredOut = true;
		}

		if (!filteredOut)
			InjectInspector(processIdVariant.uintVal, executablePath);
	}

	bool Is32BitProcess(uint32_t processId)
	{
		SYSTEM_INFO systemInfo;
		GetNativeSystemInfo(&systemInfo);
		if (systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL)
		{
			return true;
		}

		wil::unique_handle hProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
		THROW_LAST_ERROR_IF(!hProcess.is_valid());
		USHORT imageType, hostType;
		THROW_LAST_ERROR_IF(IsWow64Process2(hProcess.get(), &imageType, &hostType) == FALSE);
		if (imageType == IMAGE_FILE_MACHINE_I386)
			return true;
		return false;
	}

	void InjectInspector(uint32_t processId, const wchar_t* executablePath)
	{
		auto selfPathPtr = wil::GetModuleFileNameW();
		std::wstring dllPath = selfPathPtr.get();
		size_t lastSlash = dllPath.rfind(L'\\');
		if (lastSlash != std::wstring::npos) dllPath.erase(lastSlash);
		if (Is32BitProcess(processId))
		{
			dllPath += L"\\FontLoadInterceptor32.dll";
		}
		else
		{
			dllPath += L"\\FontLoadInterceptor64.dll";
		}
		std::wostringstream oss;
		oss << L"rundll32.exe \"" << dllPath << "\",InjectProcess " << processId;

		STARTUPINFOW startupInfo;
		wil::unique_process_information processInfo;
		RtlZeroMemory(&startupInfo, sizeof(startupInfo));
		RtlZeroMemory(&processInfo, sizeof(processInfo));

		startupInfo.cb = sizeof(startupInfo);
		auto cmdline = oss.str();
		cmdline.push_back(L'\0');

		THROW_LAST_ERROR_IF(CreateProcessW(
			nullptr,
			cmdline.data(),
			nullptr,
			nullptr,
			FALSE,
			CREATE_UNICODE_ENVIRONMENT,
			nullptr,
			nullptr,
			&startupInfo,
			processInfo.addressof()
		) == FALSE);
	}
};

sfh::ProcessMonitor::ProcessMonitor(IDaemon* daemon, std::chrono::milliseconds interval)
	: m_impl(std::make_unique<Implementation>(daemon, interval))
{
}

sfh::ProcessMonitor::~ProcessMonitor() = default;

void sfh::ProcessMonitor::Start()
{
	m_impl->Start();
}

void sfh::ProcessMonitor::SetOptions(std::vector<std::wstring>&& list, std::chrono::milliseconds interval)
{
	m_impl->SetOptions(std::move(list), interval);
}
