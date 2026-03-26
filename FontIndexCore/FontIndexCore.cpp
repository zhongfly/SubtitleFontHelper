#include "FontIndexCore.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef min
#undef max

#include <algorithm>
#include <cwctype>
#include <system_error>
#include <wil/resource.h>

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

		bool IsDirectoryPath(const std::filesystem::path& path)
		{
			const DWORD attributes = GetFileAttributesW(path.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		}

		uint64_t ToFileSize(const WIN32_FIND_DATAW& findData)
		{
			return (static_cast<uint64_t>(findData.nFileSizeHigh) << 32)
				| static_cast<uint64_t>(findData.nFileSizeLow);
		}

		std::wstring BuildFindPattern(const std::filesystem::path& directory)
		{
			std::wstring pattern = directory.wstring();
			if (!pattern.empty())
			{
				const wchar_t last = pattern.back();
				if (last != L'\\' && last != L'/')
				{
					pattern.push_back(L'\\');
				}
			}
			pattern.push_back(L'*');
			return pattern;
		}

		void EnumerateFontFilesRecursive(
			const std::filesystem::path& directory,
			std::vector<FontSourceFile>& files,
			const std::function<bool()>& isCancelled)
		{
			ThrowIfCancelled(isCancelled);
			const std::wstring pattern = BuildFindPattern(directory);

			WIN32_FIND_DATAW findData{};
			wil::unique_hfind findHandle(FindFirstFileExW(
				pattern.c_str(),
				FindExInfoBasic,
				&findData,
				FindExSearchNameMatch,
				nullptr,
				FIND_FIRST_EX_LARGE_FETCH));
			if (!findHandle.is_valid())
			{
				return;
			}

			do
			{
				ThrowIfCancelled(isCancelled);
				if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
				{
					continue;
				}

				const std::filesystem::path entryPath = directory / findData.cFileName;
				const DWORD attributes = findData.dwFileAttributes;
				if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				{
					if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
					{
						continue;
					}
					EnumerateFontFilesRecursive(entryPath, files, isCancelled);
					continue;
				}

				if (!IsSupportedFontFile(entryPath))
				{
					continue;
				}

				files.push_back({ entryPath, ToFileSize(findData) });
			} while (FindNextFileW(findHandle.get(), &findData));
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
			if (!IsDirectoryPath(sourceFolder))
			{
				continue;
			}
			EnumerateFontFilesRecursive(sourceFolder, files, isCancelled);
		}
		return files;
	}
}