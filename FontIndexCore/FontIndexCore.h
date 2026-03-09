#pragma once

#include <filesystem>
#include <vector>

namespace FontIndexCore
{
	struct FontSourceFile
	{
		std::filesystem::path m_path;
	};

	bool IsSupportedFontFile(const std::filesystem::path& path);
	std::vector<FontSourceFile> EnumerateFontFiles(const std::vector<std::filesystem::path>& sourceFolders);
}
