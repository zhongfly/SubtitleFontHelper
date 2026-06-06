#pragma once

#include <string>

namespace sfh::ui
{
	class SingleInstance
	{
	public:
		explicit SingleInstance(const std::wstring& mutexName);
		~SingleInstance();

		SingleInstance(const SingleInstance&) = delete;
		SingleInstance& operator=(const SingleInstance&) = delete;

		bool IsPrimaryInstance() const noexcept;

	private:
		void* m_mutex = nullptr;
		bool m_isPrimaryInstance = false;
	};
}
