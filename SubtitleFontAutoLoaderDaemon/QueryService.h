#pragma once

#include "IDaemon.h"
#include "PersistantData.h"
#include "TrayUiData.h"

#include <filesystem>
#include <memory>
#include <string_view>

namespace sfh
{
	class IRpcRequestHandler;

	struct LoadedFontDatabase
	{
		std::filesystem::path m_indexPath;
		std::unique_ptr<FontDatabase> m_database;
	};

	class QueryService
	{
	private:
		class Implementation;
		std::unique_ptr<Implementation> m_impl;
	public:
		QueryService(IDaemon* daemon);
		~QueryService();

		QueryService(const QueryService&) = delete;
		QueryService(QueryService&&) = delete;

		QueryService& operator=(const QueryService&) = delete;
		QueryService& operator=(QueryService&&) = delete;

		void Load(std::vector<LoadedFontDatabase>&& dbs, bool publishVersion = true);
		void PublishVersion();
		FontUiSnapshot CaptureFontUiSnapshot(std::wstring_view query) const;

		IRpcRequestHandler* GetRpcRequestHandler();
	};
}
