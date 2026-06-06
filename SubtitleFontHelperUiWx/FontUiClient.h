#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sfh::ui
{
	struct FontUiIndexSummaryData
	{
		std::wstring m_indexPath;
		uint64_t m_fontFileCount = 0;
		uint64_t m_fontNameCount = 0;
		std::wstring m_fontNamesSummary;
	};

	struct FontUiSearchResultData
	{
		std::wstring m_displayName;
		std::wstring m_familyNames;
		std::wstring m_fullNames;
		std::wstring m_postScriptNames;
		std::wstring m_indexPath;
		std::wstring m_fontPath;
		uint32_t m_faceIndex = 0;
	};

	struct FontUiSnapshotData
	{
		bool m_isLoaded = false;
		bool m_hasStaleData = false;
		std::wstring m_statusMessage;
		std::vector<FontUiIndexSummaryData> m_indexSummaries;
		std::vector<FontUiSearchResultData> m_searchResults;
		uint64_t m_totalSearchResultCount = 0;
		bool m_isSearchResultTruncated = false;
	};

	class FontUiClient
	{
	public:
		explicit FontUiClient(std::wstring pipeName = {});
		~FontUiClient();

		FontUiClient(const FontUiClient&) = delete;
		FontUiClient& operator=(const FontUiClient&) = delete;

		FontUiSnapshotData CaptureSnapshot(std::wstring_view query) const;
		void Close();

		static std::wstring BuildDefaultPipeName();

	private:
		class Implementation;
		FontUiSnapshotData MakeRequest(std::wstring_view query) const;
		std::unique_ptr<Implementation> m_impl;
	};
}
