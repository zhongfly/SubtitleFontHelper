#pragma once

#include "PersistantData.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace sfh
{
	struct ManagedIndexFontLogSummary
	{
		size_t m_fontCount = 0;
		std::wstring m_fontNamesSummary;
	};

	inline std::unordered_set<std::wstring> BuildManagedIndexPathKeys(
		const std::unordered_set<std::wstring>& paths)
	{
		std::unordered_set<std::wstring> keys;
		keys.reserve(paths.size());
		for (const auto& path : paths)
		{
			std::wstring key = path;
			std::transform(key.begin(), key.end(), key.begin(), towlower);
			keys.insert(std::move(key));
		}
		return keys;
	}

	inline std::wstring GetManagedIndexPreferredFontName(const FontDatabase::FontFaceElement& font)
	{
		const auto findName = [&](FontDatabase::FontFaceElement::NameElement::NameType type) -> const std::wstring*
		{
			for (const auto& name : font.m_names)
			{
				if (name.m_type == type && !name.m_name.empty())
				{
					return &name.m_name;
				}
			}
			return nullptr;
		};

		if (const auto fullName = findName(FontDatabase::FontFaceElement::NameElement::FullName))
		{
			return *fullName;
		}
		if (const auto familyName = findName(FontDatabase::FontFaceElement::NameElement::Win32FamilyName))
		{
			return *familyName;
		}
		if (const auto postScriptName = findName(FontDatabase::FontFaceElement::NameElement::PostScriptName))
		{
			return *postScriptName;
		}

		const auto path = font.m_path.Get();
		if (!path.empty())
		{
			const auto fileName = std::filesystem::path(path).filename().wstring();
			if (!fileName.empty())
			{
				return fileName;
			}
			return path;
		}

		return L"<unnamed>";
	}

	inline ManagedIndexFontLogSummary BuildManagedIndexFontLogSummary(const FontDatabase& db)
	{
		constexpr size_t maxVisibleNames = 12;
		constexpr size_t maxSummaryLength = 256;

		std::vector<std::wstring> names;
		names.reserve(db.m_fonts.size());
		std::unordered_set<std::wstring> seenNames;
		seenNames.reserve(db.m_fonts.size());

		for (const auto& font : db.m_fonts)
		{
			auto name = GetManagedIndexPreferredFontName(font);
			if (seenNames.insert(name).second)
			{
				names.push_back(std::move(name));
			}
		}

		std::sort(names.begin(), names.end());

		ManagedIndexFontLogSummary summary;
		summary.m_fontCount = db.m_fonts.size();
		if (names.empty())
		{
			summary.m_fontNamesSummary = L"<none>";
			return summary;
		}

		size_t visibleCount = 0;
		std::wstring joined;
		for (const auto& name : names)
		{
			std::wstring candidate = joined;
			if (!candidate.empty())
			{
				candidate += L", ";
			}
			candidate += name;

			if (visibleCount >= maxVisibleNames || candidate.size() > maxSummaryLength)
			{
				break;
			}

			joined = std::move(candidate);
			++visibleCount;
		}

		if (joined.empty())
		{
			joined = names.front().substr(0, maxSummaryLength);
			visibleCount = 1;
		}

		if (visibleCount < names.size())
		{
			joined += L", ... (+" + std::to_wstring(names.size() - visibleCount) + L")";
		}

		summary.m_fontNamesSummary = std::move(joined);
		return summary;
	}

	inline ManagedIndexFontLogSummary BuildManagedIndexFontLogSummary(
		const FontDatabase& db,
		const std::unordered_set<std::wstring>& pathKeys)
	{
		if (pathKeys.empty())
		{
			return {};
		}

		FontDatabase filtered;
		filtered.m_fonts.reserve(db.m_fonts.size());
		for (const auto& font : db.m_fonts)
		{
			std::wstring key = font.m_path.Get();
			std::transform(key.begin(), key.end(), key.begin(), towlower);
			if (pathKeys.contains(key))
			{
				filtered.m_fonts.push_back(font);
			}
		}

		return BuildManagedIndexFontLogSummary(filtered);
	}
}
