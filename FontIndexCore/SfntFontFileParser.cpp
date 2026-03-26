#include "FontFileParserInternal.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef min
#undef max

#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>
#include <wil/resource.h>

namespace
{
	constexpr uint32_t MakeTag(char a, char b, char c, char d)
	{
		return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24)
			| (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16)
			| (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8)
			| static_cast<uint32_t>(static_cast<uint8_t>(d));
	}

	constexpr uint32_t kSfntVersionTrueType = 0x00010000u;
	constexpr uint32_t kSfntVersionOpenType = MakeTag('O', 'T', 'T', 'O');
	constexpr uint32_t kSfntVersionAppleTrueType = MakeTag('t', 'r', 'u', 'e');
	constexpr uint32_t kSfntVersionType1 = MakeTag('t', 'y', 'p', '1');
	constexpr uint32_t kSfntVersionTrueType2 = 0x00020000u;
	constexpr uint32_t kCollectionTag = MakeTag('t', 't', 'c', 'f');

	constexpr uint32_t kTagCff = MakeTag('C', 'F', 'F', ' ');
	constexpr uint32_t kTagCff2 = MakeTag('C', 'F', 'F', '2');
	constexpr uint32_t kTagGlyf = MakeTag('g', 'l', 'y', 'f');
	constexpr uint32_t kTagHead = MakeTag('h', 'e', 'a', 'd');
	constexpr uint32_t kTagName = MakeTag('n', 'a', 'm', 'e');
	constexpr uint32_t kTagOs2 = MakeTag('O', 'S', '/', '2');

	constexpr uint16_t kMsIdGb2312 = 3;
	constexpr uint16_t kMsIdBig5 = 4;
	constexpr uint16_t kMsIdWansung = 5;

	constexpr uint16_t kNameIdFontFamily = 1;
	constexpr uint16_t kNameIdFullName = 4;
	constexpr uint16_t kNameIdPostScriptName = 6;

	constexpr uint16_t kPlatformMicrosoft = 3;

	constexpr uint16_t kFsSelectionItalic = 1;
	constexpr uint16_t kFsSelectionBold = 32;
	constexpr uint16_t kFsSelectionOblique = 512;
	constexpr uint16_t kMacStyleBold = 1;
	constexpr uint16_t kMacStyleItalic = 2;

	bool IsSupportedSfntVersion(uint32_t version)
	{
		return version == kSfntVersionTrueType
			|| version == kSfntVersionOpenType
			|| version == kSfntVersionAppleTrueType
			|| version == kSfntVersionType1
			|| version == kSfntVersionTrueType2;
	}

	uint16_t ReadUInt16BE(const uint8_t* data)
	{
		return static_cast<uint16_t>(
			(static_cast<uint16_t>(data[0]) << 8)
			| static_cast<uint16_t>(data[1]));
	}

	uint32_t ReadUInt32BE(const uint8_t* data)
	{
		return (static_cast<uint32_t>(data[0]) << 24)
			| (static_cast<uint32_t>(data[1]) << 16)
			| (static_cast<uint32_t>(data[2]) << 8)
			| static_cast<uint32_t>(data[3]);
	}

	class FileMapping
	{
	private:
		wil::unique_hfile m_file;
		wil::unique_handle m_mapping;
		wil::unique_mapview_ptr<void> m_view;
		size_t m_length = 0;

	public:
		explicit FileMapping(const wchar_t* path)
		{
			m_file.reset(
				CreateFileW(
					path,
					GENERIC_READ,
					FILE_SHARE_READ,
					nullptr,
					OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
					nullptr));
			THROW_LAST_ERROR_IF(!m_file.is_valid());

			LARGE_INTEGER size{};
			THROW_LAST_ERROR_IF(!GetFileSizeEx(m_file.get(), &size));
			if (size.QuadPart <= 0)
			{
				throw std::runtime_error("empty font file");
			}
			m_length = static_cast<size_t>(size.QuadPart);

			m_mapping.reset(CreateFileMappingW(
				m_file.get(),
				nullptr,
				PAGE_READONLY,
				0,
				0,
				nullptr));
			THROW_LAST_ERROR_IF(!m_mapping.is_valid());

			m_view.reset(MapViewOfFile(
				m_mapping.get(),
				FILE_MAP_READ,
				0,
				0,
				0));
			THROW_LAST_ERROR_IF(m_view.get() == nullptr);
		}

		const uint8_t* Data() const
		{
			return static_cast<const uint8_t*>(m_view.get());
		}

		size_t Size() const
		{
			return m_length;
		}
	};

	struct TableRecord
	{
		uint32_t m_tag = 0;
		uint32_t m_offset = 0;
		uint32_t m_length = 0;
	};

	struct FaceDirectory
	{
		uint32_t m_sfntVersion = 0;
		std::vector<TableRecord> m_tables;
	};

	struct Os2Info
	{
		bool m_hasTable = false;
		bool m_hasFsSelection = false;
		uint16_t m_version = 0xffffu;
		uint16_t m_weightClass = 0;
		uint16_t m_fsSelection = 0;
	};

	class SfntFontFileParser final : public FontIndexCore::Internal::FontFileParser
	{
	private:
		std::vector<char> m_mbcsBuffer;

		std::span<const uint8_t> GetSlice(
			const uint8_t* data,
			size_t size,
			uint32_t offset,
			uint32_t length) const
		{
			const size_t start = static_cast<size_t>(offset);
			const size_t spanLength = static_cast<size_t>(length);
			if (start > size || spanLength > size - start)
			{
				throw std::runtime_error("invalid table range");
			}
			return { data + start, spanLength };
		}

		std::vector<uint32_t> GetFaceOffsets(const uint8_t* data, size_t size) const
		{
			if (size < 4)
			{
				throw std::runtime_error("font file is too small");
			}

			const uint32_t signature = ReadUInt32BE(data);
			if (signature == kCollectionTag)
			{
				if (size < 12)
				{
					throw std::runtime_error("invalid font collection header");
				}

				const uint32_t faceCount = ReadUInt32BE(data + 8);
				if (faceCount == 0)
				{
					throw std::runtime_error("empty font collection");
				}
				if (static_cast<size_t>(faceCount) > (size - 12) / 4)
				{
					throw std::runtime_error("invalid font collection header");
				}

				std::vector<uint32_t> offsets;
				offsets.reserve(faceCount);
				for (uint32_t faceIndex = 0; faceIndex < faceCount; ++faceIndex)
				{
					offsets.push_back(ReadUInt32BE(data + 12 + faceIndex * 4));
				}
				return offsets;
			}

			if (!IsSupportedSfntVersion(signature))
			{
				throw std::runtime_error("unsupported sfnt version");
			}

			return { 0 };
		}

		FaceDirectory ReadFaceDirectory(
			const uint8_t* data,
			size_t size,
			uint32_t faceOffset) const
		{
			auto header = GetSlice(data, size, faceOffset, 12);
			FaceDirectory directory;
			directory.m_sfntVersion = ReadUInt32BE(header.data());
			if (!IsSupportedSfntVersion(directory.m_sfntVersion))
			{
				throw std::runtime_error("unsupported sfnt face");
			}

			const uint16_t tableCount = ReadUInt16BE(header.data() + 4);
			auto records = GetSlice(
				data,
				size,
				faceOffset + 12,
				static_cast<uint32_t>(tableCount) * 16);
			directory.m_tables.reserve(tableCount);

			for (uint16_t tableIndex = 0; tableIndex < tableCount; ++tableIndex)
			{
				const auto* record = records.data() + static_cast<size_t>(tableIndex) * 16;
				directory.m_tables.push_back({
					ReadUInt32BE(record),
					ReadUInt32BE(record + 8),
					ReadUInt32BE(record + 12)
					});
			}

			return directory;
		}

		const TableRecord* FindTable(const FaceDirectory& directory, uint32_t tag) const
		{
			for (const auto& table : directory.m_tables)
			{
				if (table.m_tag == tag)
				{
					return &table;
				}
			}
			return nullptr;
		}

		std::wstring ConvertMBCSName(uint16_t encodingId, std::span<const uint8_t> bytes)
		{
			UINT codePage = 0;
			switch (encodingId)
			{
			case kMsIdBig5:
				codePage = 950;
				break;
			case kMsIdGb2312:
				codePage = 936;
				break;
			case kMsIdWansung:
				codePage = 949;
				break;
			default:
				throw std::logic_error("unexpected name encoding");
			}

			m_mbcsBuffer.clear();
			m_mbcsBuffer.reserve(bytes.size());
			for (size_t i = 0; i + 1 < bytes.size(); i += 2)
			{
				if (bytes[i] != 0)
				{
					m_mbcsBuffer.push_back(static_cast<char>(bytes[i]));
				}
				m_mbcsBuffer.push_back(static_cast<char>(bytes[i + 1]));
			}

			if (m_mbcsBuffer.empty())
			{
				throw std::runtime_error("invalid MBCS font name");
			}

			int length = MultiByteToWideChar(
				codePage,
				MB_ERR_INVALID_CHARS,
				m_mbcsBuffer.data(),
				static_cast<int>(m_mbcsBuffer.size()),
				nullptr,
				0);
			if (length == 0)
			{
				throw std::runtime_error("invalid MBCS font name");
			}

			std::wstring ret(length, 0);
			length = MultiByteToWideChar(
				codePage,
				MB_ERR_INVALID_CHARS,
				m_mbcsBuffer.data(),
				static_cast<int>(m_mbcsBuffer.size()),
				ret.data(),
				length);
			if (length == 0)
			{
				throw std::runtime_error("invalid MBCS font name");
			}
			ret.resize(length);
			return ret;
		}

		static std::wstring ConvertUtf16BEName(std::span<const uint8_t> bytes)
		{
			const size_t length = bytes.size() / 2;
			if (length == 0)
			{
				return {};
			}

			std::wstring ret(length, 0);
			memcpy(ret.data(), bytes.data(), length * sizeof(wchar_t));
			std::transform(ret.begin(), ret.end(), ret.begin(), _byteswap_ushort);
			return ret;
		}

		std::wstring ConvertSfntName(uint16_t encodingId, std::span<const uint8_t> bytes)
		{
			switch (encodingId)
			{
			case kMsIdBig5:
			case kMsIdGb2312:
			case kMsIdWansung:
				return ConvertMBCSName(encodingId, bytes);
			default:
				return ConvertUtf16BEName(bytes);
			}
		}

		void ParseNames(
			const uint8_t* data,
			size_t size,
			const TableRecord* nameTable,
			sfh::FontDatabase::FontFaceElement& faceElement)
		{
			if (!nameTable || nameTable->m_length < 6)
			{
				return;
			}

			auto tableData = GetSlice(data, size, nameTable->m_offset, nameTable->m_length);
			const uint16_t nameCount = ReadUInt16BE(tableData.data() + 2);
			const uint16_t stringOffset = ReadUInt16BE(tableData.data() + 4);
			if (stringOffset > tableData.size())
			{
				return;
			}
			if (static_cast<size_t>(nameCount) > (tableData.size() - 6) / 12)
			{
				return;
			}

			for (uint16_t nameIndex = 0; nameIndex < nameCount; ++nameIndex)
			{
				const auto* record = tableData.data() + 6 + static_cast<size_t>(nameIndex) * 12;
				const uint16_t platformId = ReadUInt16BE(record);
				if (platformId != kPlatformMicrosoft)
				{
					continue;
				}

				sfh::FontDatabase::FontFaceElement::NameElement::NameType nameType;
				switch (ReadUInt16BE(record + 6))
				{
				case kNameIdFontFamily:
					nameType = sfh::FontDatabase::FontFaceElement::NameElement::Win32FamilyName;
					break;
				case kNameIdFullName:
					nameType = sfh::FontDatabase::FontFaceElement::NameElement::FullName;
					break;
				case kNameIdPostScriptName:
					nameType = sfh::FontDatabase::FontFaceElement::NameElement::PostScriptName;
					break;
				default:
					continue;
				}

				const uint16_t encodingId = ReadUInt16BE(record + 2);
				const uint16_t byteLength = ReadUInt16BE(record + 8);
				const uint16_t offset = ReadUInt16BE(record + 10);
				if (offset > tableData.size() - stringOffset || byteLength > tableData.size() - stringOffset - offset)
				{
					continue;
				}

				try
				{
					auto value = ConvertSfntName(
						encodingId,
						tableData.subspan(
							static_cast<size_t>(stringOffset) + offset,
							byteLength));
					if (!value.empty())
					{
						faceElement.m_names.emplace_back(nameType, std::move(value));
					}
				}
				catch (...)
				{
				}
			}

			std::sort(faceElement.m_names.begin(), faceElement.m_names.end());
			faceElement.m_names.erase(
				std::unique(faceElement.m_names.begin(), faceElement.m_names.end()),
				faceElement.m_names.end());
		}

		Os2Info ReadOs2Info(
			const uint8_t* data,
			size_t size,
			const TableRecord* os2Table) const
		{
			Os2Info info;
			if (!os2Table || os2Table->m_length < 6)
			{
				return info;
			}

			auto tableData = GetSlice(data, size, os2Table->m_offset, os2Table->m_length);
			info.m_hasTable = true;
			info.m_version = ReadUInt16BE(tableData.data());
			info.m_weightClass = ReadUInt16BE(tableData.data() + 4);
			if (tableData.size() >= 64)
			{
				info.m_hasFsSelection = true;
				info.m_fsSelection = ReadUInt16BE(tableData.data() + 62);
			}
			return info;
		}

		bool ReadMacStyle(
			const uint8_t* data,
			size_t size,
			const TableRecord* headTable,
			uint16_t& macStyle) const
		{
			if (!headTable || headTable->m_length < 46)
			{
				return false;
			}

			auto tableData = GetSlice(data, size, headTable->m_offset, headTable->m_length);
			macStyle = ReadUInt16BE(tableData.data() + 44);
			return true;
		}

		uint32_t ComputeWeight(const Os2Info& os2, bool isBold) const
		{
			if (os2.m_hasTable && os2.m_version != 0xffffu && os2.m_weightClass != 0)
			{
				return os2.m_weightClass;
			}
			return isBold ? 700u : 300u;
		}

		bool ComputeBold(
			const uint8_t* data,
			size_t size,
			const FaceDirectory& directory,
			const Os2Info& os2,
			const TableRecord* headTable) const
		{
			const bool hasOutline = directory.m_sfntVersion == kSfntVersionType1
				|| FindTable(directory, kTagGlyf) != nullptr
				|| FindTable(directory, kTagCff) != nullptr
				|| FindTable(directory, kTagCff2) != nullptr;
			if (hasOutline && os2.m_hasTable && os2.m_version != 0xffffu && os2.m_hasFsSelection)
			{
				return (os2.m_fsSelection & kFsSelectionBold) != 0;
			}

			uint16_t macStyle = 0;
			return ReadMacStyle(data, size, headTable, macStyle)
				&& (macStyle & kMacStyleBold) != 0;
		}

		bool ComputeItalic(
			const uint8_t* data,
			size_t size,
			const FaceDirectory& directory,
			const Os2Info& os2,
			const TableRecord* headTable) const
		{
			const bool hasOutline = directory.m_sfntVersion == kSfntVersionType1
				|| FindTable(directory, kTagGlyf) != nullptr
				|| FindTable(directory, kTagCff) != nullptr
				|| FindTable(directory, kTagCff2) != nullptr;
			if (hasOutline && os2.m_hasTable && os2.m_version != 0xffffu && os2.m_hasFsSelection)
			{
				if ((os2.m_fsSelection & kFsSelectionOblique) != 0)
				{
					return true;
				}
				return (os2.m_fsSelection & kFsSelectionItalic) != 0;
			}

			uint16_t macStyle = 0;
			return ReadMacStyle(data, size, headTable, macStyle)
				&& (macStyle & kMacStyleItalic) != 0;
		}

		uint32_t ComputePsOutline(
			const FaceDirectory& directory,
			const TableRecord* cffTable) const
		{
			if (directory.m_sfntVersion == kSfntVersionType1)
			{
				return 1;
			}
			return cffTable != nullptr ? 1u : 0u;
		}

	public:
		SfntFontFileParser()
		{
			m_mbcsBuffer.reserve(1024);
		}

		std::vector<sfh::FontDatabase::FontFaceElement> AnalyzeFontFile(const wchar_t* path) override
		{
			FileMapping mapping(path);
			const auto faceOffsets = GetFaceOffsets(mapping.Data(), mapping.Size());

			std::vector<sfh::FontDatabase::FontFaceElement> ret;
			ret.reserve(faceOffsets.size());

			for (size_t faceIndex = 0; faceIndex < faceOffsets.size(); ++faceIndex)
			{
				const auto directory = ReadFaceDirectory(mapping.Data(), mapping.Size(), faceOffsets[faceIndex]);
				const auto* nameTable = FindTable(directory, kTagName);
				const auto* os2Table = FindTable(directory, kTagOs2);
				const auto* headTable = FindTable(directory, kTagHead);
				const auto* cffTable = FindTable(directory, kTagCff);

				const auto os2 = ReadOs2Info(mapping.Data(), mapping.Size(), os2Table);
				const bool isBold = ComputeBold(mapping.Data(), mapping.Size(), directory, os2, headTable);
				const bool isItalic = ComputeItalic(mapping.Data(), mapping.Size(), directory, os2, headTable);

				sfh::FontDatabase::FontFaceElement faceElement;
				faceElement.m_path = path;
				faceElement.m_index = static_cast<uint32_t>(faceIndex);
				faceElement.m_weight = ComputeWeight(os2, isBold);
				faceElement.m_oblique = isItalic ? 1u : 0u;
				faceElement.m_psOutline = ComputePsOutline(directory, cffTable);

				ParseNames(mapping.Data(), mapping.Size(), nameTable, faceElement);
				ret.emplace_back(std::move(faceElement));
			}

			return ret;
		}
	};
}

std::unique_ptr<FontIndexCore::Internal::FontFileParser> FontIndexCore::Internal::CreateSfntFontFileParser()
{
	return std::make_unique<SfntFontFileParser>();
}