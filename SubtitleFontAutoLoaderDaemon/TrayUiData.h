#pragma once

#include <cstdint>
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

	struct FontSearchResult
	{
		std::wstring m_displayName;
		std::wstring m_familyNames;
		std::wstring m_fullNames;
		std::wstring m_postScriptNames;
		std::wstring m_indexPath;
		std::wstring m_fontPath;
		uint32_t m_faceIndex = 0;
	};

	struct FontUiSnapshot
	{
		bool m_isLoaded = false;
		bool m_hasStaleData = false;
		std::wstring m_statusMessage;
		std::vector<FontIndexSummary> m_indexSummaries;
		std::vector<FontSearchResult> m_searchResults;
		size_t m_totalSearchResultCount = 0;
		bool m_isSearchResultTruncated = false;
	};

	class ITrayUiDataProvider
	{
	public:
		virtual ~ITrayUiDataProvider() = default;
		virtual FontUiSnapshot CaptureFontUiSnapshot(std::wstring_view query) = 0;
	};
}
