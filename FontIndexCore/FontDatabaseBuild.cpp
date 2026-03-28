#include "FontIndexCore.h"
#include "FontFileParserInternal.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef min
#undef max

#include <ft2build.h>

#include FT_FREETYPE_H
#include FT_TRUETYPE_IDS_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H
#include FT_TYPE1_TABLES_H

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <wil/resource.h>
#include <wil/win32_helpers.h>

namespace FontIndexCore
{
	namespace
	{
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


		enum class FontParserBackend
		{
			FreeType,
			SfntPrototype,
		};

		constexpr FontParserBackend kDefaultFontParserBackend = FontParserBackend::SfntPrototype;

		std::atomic<size_t> g_fontParserFallbackCount = 0;

		void RecordFontParserFallback(const wchar_t* path)
		{
			g_fontParserFallbackCount.fetch_add(1, std::memory_order_relaxed);
			std::wstring message = L"SFH font parser fallback: ";
			message += path;
			message += L"\n";
			OutputDebugStringW(message.c_str());
		}

		class FontAnalyzer final : public Internal::FontFileParser
		{
		private:
			class FreeTypeFontFileParser final : public Internal::FontFileParser
			{
			private:
				std::vector<unsigned char> m_buffer;

				static bool HasSfntTable(FT_Face face, FT_ULong tag)
				{
					FT_ULong length = 0;
					return FT_Load_Sfnt_Table(face, tag, 0, nullptr, &length) == FT_Err_Ok;
				}

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

					m_buffer.assign(name.string, name.string + name.string_len);

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

				static uint32_t ComputePsOutline(FT_Face face)
				{
					if (HasSfntTable(face, TTAG_CFF) || HasSfntTable(face, TTAG_CFF2))
					{
						return 1;
					}

					PS_FontInfoRec psInfo;
					return FT_Get_PS_Font_Info(face, &psInfo) != FT_Err_Invalid_Argument ? 1u : 0u;
				}

			public:
				FT_Library m_lib = nullptr;

				FreeTypeFontFileParser()
				{
					m_buffer.reserve(1024);
					const auto initError = FT_Init_FreeType(&m_lib);
					if (initError != 0)
					{
						throw std::runtime_error("failed to initialize freetype");
					}
				}

				~FreeTypeFontFileParser() override
				{
					if (m_lib)
					{
						FT_Done_FreeType(m_lib);
					}
				}

				std::vector<sfh::FontDatabase::FontFaceElement> AnalyzeFontFile(const wchar_t* path) override
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

						faceElement.m_psOutline = ComputePsOutline(face);

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

			static std::unique_ptr<Internal::FontFileParser> CreateFreeTypeParser()
			{
				return std::make_unique<FreeTypeFontFileParser>();
			}

			static std::unique_ptr<Internal::FontFileParser> CreateParser(FontParserBackend backend)
			{
				switch (backend)
				{
				case FontParserBackend::SfntPrototype:
					return Internal::CreateSfntFontFileParser();
				case FontParserBackend::FreeType:
				default:
					return CreateFreeTypeParser();
				}
			}

			static std::unique_ptr<Internal::FontFileParser> CreateFallbackParser(FontParserBackend backend)
			{
				switch (backend)
				{
				case FontParserBackend::SfntPrototype:
					return CreateFreeTypeParser();
				case FontParserBackend::FreeType:
				default:
					return {};
				}
			}

			std::unique_ptr<Internal::FontFileParser> m_primaryParser;
			std::unique_ptr<Internal::FontFileParser> m_fallbackParser;
		public:
			FontAnalyzer()
				: m_primaryParser(CreateParser(kDefaultFontParserBackend)),
				  m_fallbackParser(CreateFallbackParser(kDefaultFontParserBackend))
			{
			}

			~FontAnalyzer() = default;

			FontAnalyzer(const FontAnalyzer&) = delete;
			FontAnalyzer(FontAnalyzer&&) = delete;

			FontAnalyzer& operator=(const FontAnalyzer&) = delete;
			FontAnalyzer& operator=(FontAnalyzer&&) = delete;

			std::vector<sfh::FontDatabase::FontFaceElement> AnalyzeFontFile(const wchar_t* path) override
			{
				try
				{
					return m_primaryParser->AnalyzeFontFile(path);
				}
				catch (const std::exception&)
				{
					if (!m_fallbackParser)
					{
						throw;
					}
					RecordFontParserFallback(path);
					return m_fallbackParser->AnalyzeFontFile(path);
				}
			}
		};

		struct AnalyzeBatch
		{
			size_t m_beginIndex = 0;
			size_t m_endIndex = 0;
		};

		std::vector<AnalyzeBatch> BuildAnalyzeBatches(const std::vector<std::filesystem::path>& fontFiles)
		{
			constexpr size_t kMaxBatchSize = 64;

			std::vector<AnalyzeBatch> batches;
			batches.reserve((fontFiles.size() + kMaxBatchSize - 1) / kMaxBatchSize);

			size_t batchBeginIndex = 0;
			size_t batchSize = 0;
			std::filesystem::path currentDirectory;

			for (size_t index = 0; index < fontFiles.size(); ++index)
			{
				const auto directory = fontFiles[index].parent_path();
				const bool shouldFlush = batchSize != 0
					&& (directory != currentDirectory || batchSize >= kMaxBatchSize);
				if (shouldFlush)
				{
					batches.push_back({ batchBeginIndex, index });
					batchBeginIndex = index;
					batchSize = 0;
				}
				if (batchSize == 0)
				{
					currentDirectory = directory;
				}
				++batchSize;
			}

			batches.push_back({ batchBeginIndex, fontFiles.size() });
			return batches;
		}

		using SharedPathPool = std::unordered_map<std::wstring_view, std::shared_ptr<const std::wstring>>;

		std::shared_ptr<const std::wstring> InternPath(
			const std::filesystem::path& path,
			SharedPathPool& pool)
		{
			const auto& nativePath = path.native();
			if (nativePath.empty())
			{
				return {};
			}

			auto it = pool.find(std::wstring_view(nativePath));
			if (it != pool.end())
			{
				return it->second;
			}

			auto sharedPath = std::make_shared<const std::wstring>(nativePath);
			pool.emplace(std::wstring_view(*sharedPath), sharedPath);
			return sharedPath;
		}

		void AssignSharedPath(
			std::vector<sfh::FontDatabase::FontFaceElement>& fonts,
			const std::shared_ptr<const std::wstring>& sharedPath)
		{
			for (auto& font : fonts)
			{
				font.m_path.SetShared(sharedPath);
			}
		}
	}

	sfh::FontDatabase BuildFontDatabase(
		const std::vector<std::filesystem::path>& fontFiles,
		size_t workerCount,
		const std::function<bool()>& isCancelled,
		std::atomic<size_t>* progress,
		const FileOperationErrorCallback& onError,
		BuildFontDatabaseStats* stats)
	{
		sfh::FontDatabase db;
		db.m_fonts.reserve(fontFiles.size());

		const auto totalStart = std::chrono::steady_clock::now();
		g_fontParserFallbackCount.store(0, std::memory_order_relaxed);

		if (fontFiles.empty())
		{
			if (stats)
			{
				*stats = {};
			}
			return db;
		}

		const auto analyzeBatches = BuildAnalyzeBatches(fontFiles);
		const size_t workerCountValue = std::max<size_t>(1, workerCount);
		const auto analyzeStart = std::chrono::steady_clock::now();

		std::mutex resultLock;
		SharedPathPool pathPool;
		pathPool.reserve(fontFiles.size());
		std::atomic<size_t> nextBatchIndex = 0;

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

					const size_t currentBatchIndex = nextBatchIndex.fetch_add(1, std::memory_order_relaxed);
					if (currentBatchIndex >= analyzeBatches.size())
					{
						return;
					}

					const auto& batch = analyzeBatches[currentBatchIndex];
					for (size_t currentIndex = batch.m_beginIndex; currentIndex < batch.m_endIndex; ++currentIndex)
					{
						ThrowIfCancelled(isCancelled);

						try
						{
							auto result = analyzer.AnalyzeFontFile(fontFiles[currentIndex].c_str());
							std::lock_guard lg(resultLock);
							AssignSharedPath(result, InternPath(fontFiles[currentIndex], pathPool));
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

		const auto analyzeEnd = std::chrono::steady_clock::now();
		const auto fallbackCount = g_fontParserFallbackCount.load(std::memory_order_relaxed);
		if (fallbackCount != 0)
		{
			std::string message = "SFH font parser fallback count: ";
			message += std::to_string(fallbackCount);
			message += "\n";
			OutputDebugStringA(message.c_str());
		}

		ThrowIfCancelled(isCancelled);
		const auto totalEnd = std::chrono::steady_clock::now();
		if (stats)
		{
			stats->m_totalElapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart).count());
			stats->m_analyzeElapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(analyzeEnd - analyzeStart).count());
			stats->m_fallbackCount = fallbackCount;
			stats->m_fontFaceCount = db.m_fonts.size();
		}
		return db;
	}
}
