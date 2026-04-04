#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sfh
{
	struct FontIndexSummary
	{
		std::wstring m_indexPath;
		size_t m_fontFileCount = 0;
		size_t m_fontNameCount = 0;
		std::wstring m_fontNamesSummary;
	};

	struct FontUiSnapshot
	{
		bool m_isLoaded = false;
		bool m_hasStaleData = false;
		std::wstring m_statusMessage;
		std::vector<FontIndexSummary> m_indexSummaries;
	};

	class ITrayUiDataProvider
	{
	public:
		virtual ~ITrayUiDataProvider() = default;
		virtual FontUiSnapshot CaptureFontUiSnapshot(std::wstring_view query) = 0;
	};
}
