#pragma once

#include <stdexcept>
#include <string>

#include "IDaemon.h"
#include "PersistantData.h"
#include "RpcServer.h"

namespace sfh
{
	class FontResourceError final : public std::runtime_error
	{
	private:
		std::wstring m_message;

	public:
		explicit FontResourceError(std::wstring message);
		const std::wstring& Message() const noexcept;
	};

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
