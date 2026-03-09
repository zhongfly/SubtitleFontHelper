#include "FontIndexCore.h"

#include <algorithm>
#include <cwctype>
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

		void ThrowIfCancelled(const std::function<bool()>& isCancelled)
		{
			if (isCancelled && isCancelled())
			{
				throw std::runtime_error("Operation cancelled");
			}
		}
	}

	bool IsSupportedFontFile(const std::filesystem::path& path)
	{
		const std::wstring extension = NormalizeExtension(path);
		return extension == L".ttf" || extension == L".otf" || extension == L".ttc" || extension == L".otc";
	}

	std::vector<FontSourceFile> EnumerateFontFiles(
		const std::vector<std::filesystem::path>& sourceFolders,
		const std::function<bool()>& isCancelled)
	{
		std::vector<FontSourceFile> files;
		for (const auto& sourceFolder : sourceFolders)
		{
			ThrowIfCancelled(isCancelled);
			std::error_code err;
			if (!std::filesystem::exists(sourceFolder, err))
			{
				continue;
			}
			for (std::filesystem::recursive_directory_iterator iter(sourceFolder, std::filesystem::directory_options::skip_permission_denied, err), end; iter != end; iter.increment(err))
			{
				ThrowIfCancelled(isCancelled);
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
				const auto fileSize = iter->file_size(err);
				if (err)
				{
					err.clear();
					continue;
				}
				files.push_back({ iter->path(), static_cast<uint64_t>(fileSize) });
			}
		}
		return files;
	}
}
