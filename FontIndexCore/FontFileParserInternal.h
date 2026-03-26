#pragma once

#include "PersistantData.h"

#include <memory>
#include <vector>

namespace FontIndexCore::Internal
{
	class FontFileParser
	{
	public:
		virtual ~FontFileParser() = default;
		virtual std::vector<sfh::FontDatabase::FontFaceElement> AnalyzeFontFile(const wchar_t* path) = 0;
	};

	std::unique_ptr<FontFileParser> CreateSfntFontFileParser();
}