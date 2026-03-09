#include "FontIndexCore.h"

#include <algorithm>
#include <system_error>

namespace FontIndexCore
{
	namespace
	{
		std::wstring NormalizeExtension(std::filesystem::path path)
		{
			std::wstring extension = path.extension().wstring();
			std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
			return extension;
		}
	}

	bool IsSupportedFontFile(const std::filesystem::path& path)
	{
		const std::wstring extension = NormalizeExtension(path);
		return extension == L".ttf" || extension == L".otf" || extension == L".ttc" || extension == L".otc";
	}

	std::vector<FontSourceFile> EnumerateFontFiles(const std::vector<std::filesystem::path>& sourceFolders)
	{
		std::vector<FontSourceFile> files;
		for (const auto& sourceFolder : sourceFolders)
		{
			std::error_code err;
			if (!std::filesystem::exists(sourceFolder, err))
			{
				continue;
			}
			for (std::filesystem::recursive_directory_iterator iter(sourceFolder, std::filesystem::directory_options::skip_permission_denied, err), end; iter != end; iter.increment(err))
			{
				if (err)
				{
					err.clear();
					continue;
				}
				if (!iter->is_regular_file(err))
				{
					if (err)
					{
						err.clear();
					}
					continue;
				}
				if (!IsSupportedFontFile(iter->path()))
				{
					continue;
				}
				files.push_back({ iter->path() });
			}
		}
		return files;
	}
}
