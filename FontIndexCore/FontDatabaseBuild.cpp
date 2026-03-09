#include "FontIndexCore.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef min
#undef max

#include <ft2build.h>

#include FT_FREETYPE_H
#include FT_TRUETYPE_IDS_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include FT_TYPE1_TABLES_H

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>
#include <wil/resource.h>
#include <wil/win32_helpers.h>

namespace FontIndexCore
{
	namespace
	{
		void ThrowIfCancelled(const std::function<bool()>& isCancelled)
		{
			if (isCancelled && isCancelled())
			{
				throw std::runtime_error("Operation cancelled");
			}
		}

		class FileMapping
		{
		private:
			wil::unique_hfile m_hfile;
			wil::unique_handle m_hmap;
			wil::unique_mapview_ptr<void> m_map;
		public:
			explicit FileMapping(const wchar_t* path)
			{
				m_hfile.reset(
					CreateFileW(
						path,
						GENERIC_READ,
						FILE_SHARE_READ,
						nullptr,
						OPEN_EXISTING,
						FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
						nullptr));
				THROW_LAST_ERROR_IF(!m_hfile.is_valid());

				m_hmap.reset(CreateFileMappingW(
					m_hfile.get(),
					nullptr,
					PAGE_READONLY,
					0,
					0,
					nullptr));
				THROW_LAST_ERROR_IF(!m_hmap.is_valid());

				m_map.reset(MapViewOfFile(
					m_hmap.get(),
					FILE_MAP_READ,
					0,
					0,
					0));
				THROW_LAST_ERROR_IF(m_map.get() == nullptr);
			}

			void* GetMappedPointer() const
			{
				return m_map.get();
			}

			size_t GetMappedLength() const
			{
				MEMORY_BASIC_INFORMATION info;
				THROW_LAST_ERROR_IF(VirtualQuery(m_map.get(), &info, sizeof(info)) == 0);
				return info.RegionSize;
			}
		};

		class FontAnalyzer
		{
		private:
			class Implementation
			{
			private:
				std::vector<unsigned char> m_buffer;

				std::wstring ConvertMBCSName(const FT_SfntName& name)
				{
					if (name.string_len == 0)return {};
					UINT codePage;
					switch (name.encoding_id)
					{
					case TT_MS_ID_BIG_5:
						codePage = 950;
						break;
					case TT_MS_ID_GB2312:
						codePage = 936;
						break;
					case TT_MS_ID_WANSUNG:
						codePage = 949;
						break;
					default:
						throw std::logic_error("unexpected name encoding");
					}

					m_buffer.clear();
					for (FT_UInt i = 0; i < name.string_len - 1; i += 2)
					{
						if (name.string[i])
						{
							m_buffer.push_back(name.string[i]);
						}
						m_buffer.push_back(name.string[i + 1]);
					}

					int length = MultiByteToWideChar(
						codePage,
						MB_ERR_INVALID_CHARS,
						reinterpret_cast<char*>(m_buffer.data()),
						static_cast<int>(m_buffer.size()),
						nullptr,
						0);
					THROW_LAST_ERROR_IF(length == 0);

					std::wstring ret(length, 0);

					length = MultiByteToWideChar(
						codePage,
						MB_ERR_INVALID_CHARS,
						reinterpret_cast<char*>(m_buffer.data()),
						static_cast<int>(m_buffer.size()),
						ret.data(),
						static_cast<int>(ret.size()));
					THROW_LAST_ERROR_IF(length == 0);
					ret.resize(length);

					return ret;
				}

				static std::wstring ConvertUtf16BEName(const FT_SfntName& name)
				{
					size_t length = name.string_len / 2;
					std::wstring ret(length, 0);
					memcpy(ret.data(), name.string, length * sizeof(wchar_t));
					std::transform(ret.begin(), ret.end(), ret.begin(), _byteswap_ushort);
					return ret;
				}

				std::wstring ConvertSfntName(const FT_SfntName& name)
				{
					switch (name.encoding_id)
					{
					case TT_MS_ID_BIG_5:
					case TT_MS_ID_GB2312:
					case TT_MS_ID_WANSUNG:
						return ConvertMBCSName(name);
					default:
						return ConvertUtf16BEName(name);
					}
				}

			public:
				FT_Library m_lib;

				Implementation()
				{
					m_buffer.reserve(1024);
					FT_Init_FreeType(&m_lib);
				}

				~Implementation()
				{
					FT_Done_FreeType(m_lib);
				}

				std::vector<sfh::FontDatabase::FontFaceElement> AnalyzeFontFile(const wchar_t* path)
				{
					std::vector<sfh::FontDatabase::FontFaceElement> ret;
					FileMapping mapping(path);
					FT_Face face;

					if (FT_New_Memory_Face(
						m_lib,
						static_cast<FT_Byte*>(mapping.GetMappedPointer()),
						static_cast<FT_Long>(mapping.GetMappedLength()),
						-1,
						&face) != 0)
						throw std::runtime_error("failed to open font!");

					int faceCount = face->num_faces;
					ret.reserve(faceCount);
					FT_Done_Face(face);

					for (int faceIndex = 0; faceIndex < faceCount; ++faceIndex)
					{
						sfh::FontDatabase::FontFaceElement faceElement;
						faceElement.m_path = path;
						faceElement.m_index = faceIndex;

						if (FT_New_Memory_Face(
							m_lib,
							static_cast<FT_Byte*>(mapping.GetMappedPointer()),
							static_cast<FT_Long>(mapping.GetMappedLength()),
							faceIndex,
							&face) != 0)
							throw std::runtime_error("failed to open fontface!");
						auto doneFace = wil::scope_exit([&]() { FT_Done_Face(face); });

						TT_OS2* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
						if (os2 && os2->version != 0xffff && os2->usWeightClass)
							faceElement.m_weight = os2->usWeightClass;
						else
							faceElement.m_weight = face->style_flags & FT_STYLE_FLAG_BOLD ? 700 : 300;

						faceElement.m_oblique = face->style_flags & FT_STYLE_FLAG_ITALIC ? 1 : 0;

						PS_FontInfoRec psInfo;
						faceElement.m_psOutline = FT_Get_PS_Font_Info(face, &psInfo) != FT_Err_Invalid_Argument;

						FT_UInt nameCount = FT_Get_Sfnt_Name_Count(face);
						for (FT_UInt nameIndex = 0; nameIndex < nameCount; ++nameIndex)
						{
							FT_SfntName name;
							if (FT_Get_Sfnt_Name(face, nameIndex, &name) != 0)
								continue;
							if (name.platform_id != TT_PLATFORM_MICROSOFT)
								continue;

							sfh::FontDatabase::FontFaceElement::NameElement::NameType nameType;

							switch (name.name_id)
							{
							case TT_NAME_ID_FONT_FAMILY:
								nameType = sfh::FontDatabase::FontFaceElement::NameElement::Win32FamilyName;
								break;
							case TT_NAME_ID_FULL_NAME:
								nameType = sfh::FontDatabase::FontFaceElement::NameElement::FullName;
								break;
							case TT_NAME_ID_PS_NAME:
								nameType = sfh::FontDatabase::FontFaceElement::NameElement::PostScriptName;
								break;
							default:
								continue;
							}

							try
							{
								faceElement.m_names.emplace_back(nameType, ConvertSfntName(name));
							}
							catch (...)
							{
							}
						}

						std::sort(faceElement.m_names.begin(), faceElement.m_names.end());
						faceElement.m_names.erase(
							std::unique(faceElement.m_names.begin(), faceElement.m_names.end()),
							faceElement.m_names.end());

						ret.emplace_back(std::move(faceElement));
					}
					return ret;
				}
			};

			std::unique_ptr<Implementation> m_impl;
		public:
			FontAnalyzer()
				: m_impl(std::make_unique<Implementation>())
			{
			}

			~FontAnalyzer() = default;

			FontAnalyzer(const FontAnalyzer&) = delete;
			FontAnalyzer(FontAnalyzer&&) = delete;

			FontAnalyzer& operator=(const FontAnalyzer&) = delete;
			FontAnalyzer& operator=(FontAnalyzer&&) = delete;

			std::vector<sfh::FontDatabase::FontFaceElement> AnalyzeFontFile(const wchar_t* path)
			{
				return m_impl->AnalyzeFontFile(path);
			}
		};
	}

	sfh::FontDatabase BuildFontDatabase(
		const std::vector<std::filesystem::path>& fontFiles,
		size_t workerCount,
		const std::function<bool()>& isCancelled,
		std::atomic<size_t>* progress,
		const AnalyzeFontFileErrorCallback& onError)
	{
		sfh::FontDatabase db;
		db.m_fonts.reserve(fontFiles.size());

		if (fontFiles.empty())
		{
			return db;
		}

		const size_t workerCountValue = std::max<size_t>(1, workerCount);

		std::mutex resultLock;
		std::mutex consumeLock;
		size_t nextFileIndex = 0;

		std::vector<std::thread> workers;
		workers.reserve(workerCountValue);

		for (size_t i = 0; i < workerCountValue; ++i)
		{
			workers.emplace_back([&]()
			{
				FontAnalyzer analyzer;
				while (true)
				{
					ThrowIfCancelled(isCancelled);

					size_t currentIndex;
					{
						std::lock_guard lg(consumeLock);
						if (nextFileIndex == fontFiles.size())
						{
							return;
						}
						currentIndex = nextFileIndex;
						++nextFileIndex;
					}

					try
					{
						auto result = analyzer.AnalyzeFontFile(fontFiles[currentIndex].c_str());
						std::lock_guard lg(resultLock);
						db.m_fonts.insert(
							db.m_fonts.end(),
							std::make_move_iterator(result.begin()),
							std::make_move_iterator(result.end()));
					}
					catch (const std::exception& e)
					{
						if (onError)
						{
							onError(fontFiles[currentIndex], e.what());
						}
					}

					if (progress)
					{
						++(*progress);
					}
				}
			});
		}

		for (auto& worker : workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}

		ThrowIfCancelled(isCancelled);
		return db;
	}
}
