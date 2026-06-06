#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "SingleInstance.h"

#include <wil/resource.h>

sfh::ui::SingleInstance::SingleInstance(const std::wstring& mutexName)
{
	m_mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
	THROW_LAST_ERROR_IF_NULL(m_mutex);
	m_isPrimaryInstance = (GetLastError() != ERROR_ALREADY_EXISTS);
}

sfh::ui::SingleInstance::~SingleInstance()
{
	if (m_mutex != nullptr)
	{
		CloseHandle(static_cast<HANDLE>(m_mutex));
		m_mutex = nullptr;
	}
}

bool sfh::ui::SingleInstance::IsPrimaryInstance() const noexcept
{
	return m_isPrimaryInstance;
}
