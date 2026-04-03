#pragma once

#include "IDaemon.h"
#include "PersistantData.h"
#include "RpcServer.h"

namespace sfh
{
	class Prefetch
	{
	private:
		class Implementation;
		std::unique_ptr<Implementation> m_impl;
	public:
		Prefetch(
			IDaemon* daemon,
			size_t prefetchCount,
			const std::wstring& lruPath,
			bool missingFontNotificationsEnabled,
			std::vector<std::wstring> missingFontIgnore,
			std::vector<ConfigFile::ProcessMissingFontIgnoreElement> processMissingFontIgnore);
		~Prefetch();

		Prefetch(const Prefetch&) = delete;
		Prefetch(Prefetch&&) = delete;

		Prefetch& operator=(const Prefetch&) = delete;
		Prefetch& operator=(Prefetch&&) = delete;

		IRpcFeedbackHandler* GetRpcFeedbackHandler();
		
	};
}
