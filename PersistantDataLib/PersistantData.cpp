#include "PersistantData.h"

#include <vector>
#include <cassert>
#include <stdexcept>
#include <atomic>
#include <cwctype>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <variant>

#include <Windows.h>
#undef max
#include <combaseapi.h>
#include <propvarutil.h>
#include <shellapi.h>
#pragma comment(lib,"Shlwapi.lib")
#include <MsXml2.h>
#pragma comment(lib,"msxml2.lib")
#include <wil/resource.h>
#include <wil/com.h>


namespace
{
	uint32_t wcstou32(const wchar_t* str, int length)
	{
		uint64_t ret = 0;
		for (int i = 0; i < length; ++i)
		{
			if (str[i] > L'9' || str[i] < L'0')
				throw std::out_of_range("unexpected character in numeric string");
			ret *= 10;
			ret += str[i] - L'0';
			if (ret > std::numeric_limits<uint32_t>::max())
				throw std::out_of_range("number too large");
		}
		return static_cast<uint32_t>(ret);
	}

	bool XmlNameEquals(const wchar_t* actual, int actualLength, const wchar_t* expected)
	{
		return actualLength >= 0
			&& static_cast<size_t>(actualLength) == wcslen(expected)
			&& wcsncmp(actual, expected, actualLength) == 0;
	}

	std::string ReadUtf8TextFile(const std::wstring& path)
	{
		std::string ret;
		wil::unique_file fp;
		if (_wfopen_s(fp.put(), path.c_str(), L"rb"))
			throw std::runtime_error("unable to open file");
		if (fseek(fp.get(), 0, SEEK_END) != 0)
			throw std::runtime_error("unable to seek file");
		auto fileSize = ftell(fp.get());
		if (fileSize < 0)
			throw std::runtime_error("unable to get file size");
		ret.resize(static_cast<size_t>(fileSize));
		rewind(fp.get());
		if (!ret.empty() && fread(ret.data(), sizeof(char), ret.size(), fp.get()) != ret.size())
			throw std::runtime_error("unable to read file");
		if (ret.size() >= 3
			&& static_cast<unsigned char>(ret[0]) == 0xEF
			&& static_cast<unsigned char>(ret[1]) == 0xBB
			&& static_cast<unsigned char>(ret[2]) == 0xBF)
		{
			ret.erase(0, 3);
		}
		return ret;
	}

	std::wstring Utf8ToWideString(const std::string_view str)
	{
		if (str.empty())
			return {};
		const int length = MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			str.data(),
			static_cast<int>(str.size()),
			nullptr,
			0);
		if (length == 0)
			throw std::runtime_error("invalid utf-8 string");

		std::wstring ret;
		ret.resize(length);
		if (MultiByteToWideChar(
			CP_UTF8,
			MB_ERR_INVALID_CHARS,
			str.data(),
			static_cast<int>(str.size()),
			ret.data(),
			length) == 0)
		{
			throw std::runtime_error("invalid utf-8 string");
		}
		return ret;
	}

	std::string TrimAscii(std::string_view value)
	{
		size_t begin = 0;
		while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t'))
			++begin;
		size_t end = value.size();
		while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t'))
			--end;
		return std::string(value.substr(begin, end - begin));
	}

	class TomlConfigParser
	{
	private:
		using TomlValue = std::variant<
			std::wstring,
			uint32_t,
			bool,
			std::vector<std::wstring>,
			std::vector<uint32_t>,
			std::vector<bool>>;

		enum class Context
		{
			Root = 0,
			IndexFile,
			Unknown
		};

		std::string_view m_source;
		size_t m_pos = 0;
		size_t m_line = 1;
		size_t m_column = 1;
		Context m_context = Context::Root;
		std::unique_ptr<sfh::ConfigFile> m_config = std::make_unique<sfh::ConfigFile>();

	public:
		explicit TomlConfigParser(std::string_view source)
			: m_source(source)
		{
		}

		std::unique_ptr<sfh::ConfigFile> Parse()
		{
			while (true)
			{
				SkipStatementTrivia();
				if (IsEof())
					break;
				if (Peek() == '[')
				{
					ParseTableHeader();
				}
				else
				{
					ParseKeyValue();
				}
			}

			for (const auto& indexFile : m_config->m_indexFile)
			{
				if (indexFile.m_path.empty())
					ThrowError("index_files.path must not be empty");
			}

			return std::move(m_config);
		}

	private:
		[[noreturn]] void ThrowError(const char* message) const
		{
			throw std::runtime_error(
				"TOML parse error at line "
				+ std::to_string(m_line)
				+ ", column "
				+ std::to_string(m_column)
				+ ": "
				+ message);
		}

		bool IsEof() const
		{
			return m_pos >= m_source.size();
		}

		char Peek(size_t offset = 0) const
		{
			if (m_pos + offset >= m_source.size())
				return '\0';
			return m_source[m_pos + offset];
		}

		static bool IsLineBreak(char ch)
		{
			return ch == '\r' || ch == '\n';
		}

		static bool IsSpaceOrTab(char ch)
		{
			return ch == ' ' || ch == '\t';
		}

		static bool IsBareKeyChar(char ch)
		{
			return (ch >= 'a' && ch <= 'z')
				|| (ch >= 'A' && ch <= 'Z')
				|| (ch >= '0' && ch <= '9')
				|| ch == '_'
				|| ch == '-';
		}

		void Advance()
		{
			if (IsEof())
				return;

			if (Peek() == '\r')
			{
				++m_pos;
				if (Peek() == '\n')
					++m_pos;
				++m_line;
				m_column = 1;
				return;
			}
			if (Peek() == '\n')
			{
				++m_pos;
				++m_line;
				m_column = 1;
				return;
			}

			++m_pos;
			++m_column;
		}

		bool TryConsume(char expected)
		{
			if (Peek() != expected)
				return false;
			Advance();
			return true;
		}

		void Expect(char expected)
		{
			if (!TryConsume(expected))
				ThrowError("unexpected token");
		}

		void SkipComment()
		{
			if (Peek() != '#')
				return;
			while (!IsEof() && !IsLineBreak(Peek()))
			{
				Advance();
			}
		}

		void SkipStatementTrivia()
		{
			while (!IsEof())
			{
				if (IsSpaceOrTab(Peek()) || IsLineBreak(Peek()))
				{
					Advance();
					continue;
				}
				if (Peek() == '#')
				{
					SkipComment();
					continue;
				}
				break;
			}
		}

		void SkipInlineTrivia()
		{
			while (!IsEof() && IsSpaceOrTab(Peek()))
			{
				Advance();
			}
		}

		void SkipArrayTrivia()
		{
			while (!IsEof())
			{
				if (IsSpaceOrTab(Peek()) || IsLineBreak(Peek()))
				{
					Advance();
					continue;
				}
				if (Peek() == '#')
				{
					SkipComment();
					continue;
				}
				break;
			}
		}

		void FinishStatement()
		{
			SkipInlineTrivia();
			if (Peek() == '#')
				SkipComment();
			SkipInlineTrivia();
			if (!IsEof() && !IsLineBreak(Peek()))
				ThrowError("expected end of line");
		}

		std::string ParseKey()
		{
			size_t begin = m_pos;
			while (IsBareKeyChar(Peek()))
			{
				Advance();
			}
			if (begin == m_pos)
				ThrowError("expected key");
			return std::string(m_source.substr(begin, m_pos - begin));
		}

		std::string ParseTableName(bool arrayTable)
		{
			size_t begin = m_pos;
			while (!IsEof())
			{
				if (arrayTable)
				{
					if (Peek() == ']' && Peek(1) == ']')
						break;
				}
				else if (Peek() == ']')
				{
					break;
				}

				if (IsLineBreak(Peek()))
					ThrowError("table header must be on a single line");
				Advance();
			}
			if (IsEof())
				ThrowError("unterminated table header");
			auto name = TrimAscii(m_source.substr(begin, m_pos - begin));
			if (name.empty())
				ThrowError("table name must not be empty");
			return name;
		}

		void ParseTableHeader()
		{
			Expect('[');
			bool isArrayTable = TryConsume('[');
			auto tableName = ParseTableName(isArrayTable);
			if (isArrayTable)
			{
				Expect(']');
			}
			Expect(']');
			FinishStatement();

			if (isArrayTable && tableName == "index_files")
			{
				m_config->m_indexFile.emplace_back();
				m_context = Context::IndexFile;
				return;
			}

			m_context = Context::Unknown;
		}

		std::wstring ParseBasicString()
		{
			Expect('"');
			std::string value;
			while (!IsEof())
			{
				char ch = Peek();
				if (ch == '"')
				{
					Advance();
					return Utf8ToWideString(value);
				}
				if (IsLineBreak(ch))
					ThrowError("multiline string is not supported");
				if (ch != '\\')
				{
					value.push_back(ch);
					Advance();
					continue;
				}

				Advance();
				char escaped = Peek();
				if (escaped == '\0')
					ThrowError("unterminated escape sequence");
				switch (escaped)
				{
				case 'b':
					value.push_back('\b');
					break;
				case 't':
					value.push_back('\t');
					break;
				case 'n':
					value.push_back('\n');
					break;
				case 'f':
					value.push_back('\f');
					break;
				case 'r':
					value.push_back('\r');
					break;
				case '"':
					value.push_back('"');
					break;
				case '\\':
					value.push_back('\\');
					break;
				default:
					ThrowError("unsupported escape sequence");
				}
				Advance();
			}
			ThrowError("unterminated string");
		}

		std::wstring ParseLiteralString()
		{
			Expect('\'');
			size_t begin = m_pos;
			while (!IsEof())
			{
				if (Peek() == '\'')
				{
					auto value = Utf8ToWideString(m_source.substr(begin, m_pos - begin));
					Advance();
					return value;
				}
				if (IsLineBreak(Peek()))
					ThrowError("multiline string is not supported");
				Advance();
			}
			ThrowError("unterminated string");
		}

		uint32_t ParseInteger()
		{
			uint64_t value = 0;
			bool hasDigit = false;
			bool lastWasUnderscore = false;
			while (!IsEof())
			{
				char ch = Peek();
				if (ch >= '0' && ch <= '9')
				{
					hasDigit = true;
					lastWasUnderscore = false;
					value *= 10;
					value += ch - '0';
					if (value > std::numeric_limits<uint32_t>::max())
						ThrowError("integer is too large");
					Advance();
					continue;
				}
				if (ch == '_')
				{
					if (!hasDigit || lastWasUnderscore)
						ThrowError("invalid integer format");
					lastWasUnderscore = true;
					Advance();
					continue;
				}
				break;
			}
			if (!hasDigit || lastWasUnderscore)
				ThrowError("invalid integer format");
			return static_cast<uint32_t>(value);
		}

		bool ParseBoolean()
		{
			if (m_source.substr(m_pos, 4) == "true")
			{
				m_pos += 4;
				m_column += 4;
				return true;
			}
			if (m_source.substr(m_pos, 5) == "false")
			{
				m_pos += 5;
				m_column += 5;
				return false;
			}
			ThrowError("invalid boolean value");
		}

		TomlValue ParseScalarValue()
		{
			if (Peek() == '"')
				return ParseBasicString();
			if (Peek() == '\'')
				return ParseLiteralString();
			if (Peek() >= '0' && Peek() <= '9')
				return ParseInteger();
			if (Peek() == 't' || Peek() == 'f')
				return ParseBoolean();
			ThrowError("unsupported value type");
		}

		TomlValue ParseArray()
		{
			Expect('[');
			SkipArrayTrivia();
			if (TryConsume(']'))
				return std::vector<std::wstring>{};

			auto firstValue = ParseScalarValue();
			SkipArrayTrivia();
			if (std::holds_alternative<std::wstring>(firstValue))
			{
				std::vector<std::wstring> values;
				values.emplace_back(std::get<std::wstring>(std::move(firstValue)));
				while (!TryConsume(']'))
				{
					Expect(',');
					SkipArrayTrivia();
					if (TryConsume(']'))
						break;
					auto value = ParseScalarValue();
					if (!std::holds_alternative<std::wstring>(value))
						ThrowError("array values must have the same type");
					values.emplace_back(std::get<std::wstring>(std::move(value)));
					SkipArrayTrivia();
				}
				return values;
			}
			if (std::holds_alternative<uint32_t>(firstValue))
			{
				std::vector<uint32_t> values;
				values.emplace_back(std::get<uint32_t>(firstValue));
				while (!TryConsume(']'))
				{
					Expect(',');
					SkipArrayTrivia();
					if (TryConsume(']'))
						break;
					auto value = ParseScalarValue();
					if (!std::holds_alternative<uint32_t>(value))
						ThrowError("array values must have the same type");
					values.emplace_back(std::get<uint32_t>(value));
					SkipArrayTrivia();
				}
				return values;
			}

			std::vector<bool> values;
			values.emplace_back(std::get<bool>(firstValue));
			while (!TryConsume(']'))
			{
				Expect(',');
				SkipArrayTrivia();
				if (TryConsume(']'))
					break;
				auto value = ParseScalarValue();
				if (!std::holds_alternative<bool>(value))
					ThrowError("array values must have the same type");
				values.emplace_back(std::get<bool>(value));
				SkipArrayTrivia();
			}
			return values;
		}

		TomlValue ParseValue()
		{
			if (Peek() == '[')
				return ParseArray();
			return ParseScalarValue();
		}

		static const std::wstring& ExpectString(const TomlValue& value, const char* keyName)
		{
			auto stringValue = std::get_if<std::wstring>(&value);
			if (stringValue == nullptr)
				throw std::runtime_error(std::string("TOML key requires string value: ") + keyName);
			return *stringValue;
		}

		static uint32_t ExpectUInt32(const TomlValue& value, const char* keyName)
		{
			auto integerValue = std::get_if<uint32_t>(&value);
			if (integerValue == nullptr)
				throw std::runtime_error(std::string("TOML key requires integer value: ") + keyName);
			return *integerValue;
		}

		static const std::vector<std::wstring>& ExpectStringArray(const TomlValue& value, const char* keyName)
		{
			auto arrayValue = std::get_if<std::vector<std::wstring>>(&value);
			if (arrayValue == nullptr)
				throw std::runtime_error(std::string("TOML key requires string array value: ") + keyName);
			return *arrayValue;
		}

		void ApplyRootKey(const std::string& key, const TomlValue& value)
		{
			if (key == "wmi_poll_interval" || key == "wmiPollInterval")
			{
				m_config->wmiPollInterval = ExpectUInt32(value, key.c_str());
			}
			else if (key == "lru_size" || key == "lruSize")
			{
				m_config->lruSize = ExpectUInt32(value, key.c_str());
			}
			else if (key == "monitor_processes" || key == "monitorProcesses")
			{
				for (const auto& process : ExpectStringArray(value, key.c_str()))
				{
					m_config->m_monitorProcess.push_back({ process });
				}
			}
			else if (key == "index_files" || key == "indexFiles")
			{
				for (const auto& path : ExpectStringArray(value, key.c_str()))
				{
					m_config->m_indexFile.push_back({ path });
				}
			}
		}

		void ApplyIndexFileKey(const std::string& key, const TomlValue& value)
		{
			if (m_config->m_indexFile.empty())
				ThrowError("index_files table is not initialized");

			if (key == "path")
			{
				m_config->m_indexFile.back().m_path = ExpectString(value, key.c_str());
			}
			else if (key == "source_folders" || key == "sourceFolders")
			{
				m_config->m_indexFile.back().m_sourceFolders = ExpectStringArray(value, key.c_str());
			}
		}

		void ParseKeyValue()
		{
			auto key = ParseKey();
			SkipInlineTrivia();
			Expect('=');
			SkipInlineTrivia();
			auto value = ParseValue();
			FinishStatement();

			switch (m_context)
			{
			case Context::Root:
				ApplyRootKey(key, value);
				break;
			case Context::IndexFile:
				ApplyIndexFileKey(key, value);
				break;
			case Context::Unknown:
				break;
			default:
				throw std::logic_error("unexpected parser context");
			}
		}
	};

	std::unique_ptr<sfh::ConfigFile> ReadTomlConfigFromFile(const std::wstring& path)
	{
		auto content = ReadUtf8TextFile(path);
		TomlConfigParser parser(content);
		return parser.Parse();
	}

	class SimpleSAXContentHandler : public ISAXContentHandler
	{
	private:
		ULONG m_refCount = 1;
	public:
		virtual ~SimpleSAXContentHandler() = default;

		HRESULT STDMETHODCALLTYPE QueryInterface(const IID& riid, void** ppvObject) override
		{
			if (ppvObject == nullptr)
				return E_INVALIDARG;
			if (riid == IID_IUnknown)
			{
				*ppvObject = static_cast<IUnknown*>(this);
			}
			else if (riid == IID_ISAXContentHandler)
			{
				*ppvObject = static_cast<ISAXContentHandler*>(this);
			}
			else
			{
				return E_NOINTERFACE;
			}
			return S_OK;
		}

		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return ++m_refCount;
		}

		ULONG STDMETHODCALLTYPE Release() override
		{
			ULONG refCount = --m_refCount;
			if (refCount == 0)
				delete this;
			return refCount;
		}

		HRESULT STDMETHODCALLTYPE putDocumentLocator(ISAXLocator* pLocator) override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE startDocument() override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE endDocument() override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE startPrefixMapping(const wchar_t* pwchPrefix, int cchPrefix, const wchar_t* pwchUri,
		                                             int cchUri) override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE endPrefixMapping(const wchar_t* pwchPrefix, int cchPrefix) override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE startElement(const wchar_t* pwchNamespaceUri, int cchNamespaceUri,
		                                       const wchar_t* pwchLocalName,
		                                       int cchLocalName, const wchar_t* pwchQName, int cchQName,
		                                       ISAXAttributes* pAttributes) override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE endElement(const wchar_t* pwchNamespaceUri, int cchNamespaceUri,
		                                     const wchar_t* pwchLocalName,
		                                     int cchLocalName, const wchar_t* pwchQName, int cchQName) override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE characters(const wchar_t* pwchChars, int cchChars) override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE ignorableWhitespace(const wchar_t* pwchChars, int cchChars) override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE processingInstruction(const wchar_t* pwchTarget, int cchTarget,
		                                                const wchar_t* pwchData,
		                                                int cchData) override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE skippedEntity(const wchar_t* pwchName, int cchName) override
		{
			return S_OK;
		}
	};

	class ConfigSAXContentHandler : public SimpleSAXContentHandler
	{
	private:
		enum class ElementType:size_t
		{
			Document = 0,
			RootElement,
			IndexFileElement,
			MonitorElement
		};

		std::unique_ptr<sfh::ConfigFile> m_config;
		std::vector<ElementType> m_status;
		size_t m_ignoreDepth = 0;

	public:
		HRESULT STDMETHODCALLTYPE startDocument() override
		{
			m_config = std::make_unique<sfh::ConfigFile>();
			m_status.clear();
			m_status.emplace_back(ElementType::Document);
			m_ignoreDepth = 0;
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE endDocument() override
		{
			if (!m_status.empty() && m_status.back() == ElementType::Document)
				m_status.pop_back();
			if (m_status.empty())
				return S_OK;
			return E_FAIL;
		}

		HRESULT STDMETHODCALLTYPE startElement(const wchar_t* pwchNamespaceUri, int cchNamespaceUri,
		                                       const wchar_t* pwchLocalName,
		                                       int cchLocalName, const wchar_t* pwchQName, int cchQName,
		                                       ISAXAttributes* pAttributes) override
		{
			if (m_ignoreDepth != 0)
			{
				++m_ignoreDepth;
				return S_OK;
			}
			if (m_status.empty())
				return E_FAIL;
			switch (m_status.back())
			{
			case ElementType::Document:
				if (XmlNameEquals(pwchLocalName, cchLocalName, L"ConfigFile"))
				{
					m_status.emplace_back(ElementType::RootElement);
					const wchar_t* attrValue;
					int attrLength;

#define DEFINE_XML_ATTRIBUTE(name) static constexpr wchar_t name[] = L#name; \
	static constexpr size_t name##Cch = std::extent_v<decltype(name)>-1

					DEFINE_XML_ATTRIBUTE(wmiPollInterval);
					DEFINE_XML_ATTRIBUTE(lruSize);

#undef DEFINE_XML_ATTRIBUTE
					if (SUCCEEDED(
						pAttributes->getValueFromName(L"", 0, wmiPollInterval, wmiPollIntervalCch, &attrValue, &
							attrLength)))
					{
						try
						{
							m_config->wmiPollInterval = wcstou32(attrValue, attrLength);
						}
						catch (...)
						{
							// don't let exceptions travel across dll
							return E_FAIL;
						}
					}
					if (SUCCEEDED(
						pAttributes->getValueFromName(L"", 0, lruSize, lruSizeCch, &attrValue, &attrLength)))
					{
						try
						{
							m_config->lruSize = wcstou32(attrValue, attrLength);
						}
						catch (...)
						{
							// don't let exceptions travel across dll
							return E_FAIL;
						}
					}
				}
				else
				{
					return E_FAIL;
				}
				break;
			case ElementType::RootElement:
				if (XmlNameEquals(pwchLocalName, cchLocalName, L"IndexFile"))
				{
					m_config->m_indexFile.emplace_back();
					m_status.emplace_back(ElementType::IndexFileElement);
				}
				else if (XmlNameEquals(pwchLocalName, cchLocalName, L"MonitorProcess"))
				{
					m_config->m_monitorProcess.emplace_back();
					m_status.emplace_back(ElementType::MonitorElement);
				}
				else
				{
					m_ignoreDepth = 1;
				}
				break;
			case ElementType::IndexFileElement:
				m_ignoreDepth = 1;
				break;
			case ElementType::MonitorElement:
				m_ignoreDepth = 1;
				break;
			default:
				return E_FAIL;
			}
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE endElement(const wchar_t* pwchNamespaceUri, int cchNamespaceUri,
		                                     const wchar_t* pwchLocalName,
		                                     int cchLocalName, const wchar_t* pwchQName, int cchQName) override
		{
			if (m_ignoreDepth != 0)
			{
				--m_ignoreDepth;
				return S_OK;
			}
			if (m_status.empty())
				return E_FAIL;
			switch (m_status.back())
			{
			case ElementType::RootElement:
				if (XmlNameEquals(pwchLocalName, cchLocalName, L"ConfigFile"))
				{
					m_status.pop_back();
				}
				else
				{
					return E_FAIL;
				}
				break;
			case ElementType::IndexFileElement:
				if (XmlNameEquals(pwchLocalName, cchLocalName, L"IndexFile"))
				{
					m_status.pop_back();
				}
				else
				{
					return E_FAIL;
				}
				break;
			case ElementType::MonitorElement:
				if (XmlNameEquals(pwchLocalName, cchLocalName, L"MonitorProcess"))
				{
					m_status.pop_back();
				}
				else
				{
					return E_FAIL;
				}
				break;
			default:
				return E_FAIL;
			}
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE characters(const wchar_t* pwchChars, int cchChars) override
		{
			if (m_ignoreDepth != 0)
				return S_OK;
			if (m_status.empty())
				return E_FAIL;
			switch (m_status.back())
			{
			case ElementType::IndexFileElement:
				m_config->m_indexFile.back().m_path.append(pwchChars, cchChars);
				break;
			case ElementType::MonitorElement:
				m_config->m_monitorProcess.back().m_name.append(pwchChars, cchChars);
				break;
			}
			// ignore unexpected characters
			return S_OK;
		}

		std::unique_ptr<sfh::ConfigFile> GetConfigFile()
		{
			return std::move(m_config);
		}
	};

	std::unique_ptr<sfh::ConfigFile> ReadXmlConfigFromFile(const std::wstring& path)
	{
		auto com = wil::CoInitializeEx();

		wil::unique_variant pathVariant;
		wil::com_ptr<IStream> stream;
		THROW_IF_FAILED_MSG(
			SHCreateStreamOnFileEx(
				path.c_str(),
				STGM_FAILIFTHERE | STGM_READ | STGM_SHARE_EXCLUSIVE,
				FILE_ATTRIBUTE_NORMAL,
				FALSE,
				nullptr,
				stream.put()), "CANNOT OPEN CONFIG FILE: %ws", path.c_str());
		InitVariantFromUnknown(stream.query<IUnknown>().get(), pathVariant.addressof());

		auto saxReader = wil::CoCreateInstance<ISAXXMLReader>(CLSID_SAXXMLReader30);
		wil::com_ptr<ConfigSAXContentHandler> handler(new ConfigSAXContentHandler);
		THROW_IF_FAILED(saxReader->putContentHandler(handler.get()));
		THROW_IF_FAILED_MSG(saxReader->parse(pathVariant), "BAD CONFIG: %ws", path.c_str());

		return handler->GetConfigFile();
	}

	class FontDatabaseSAXContentHandler : public SimpleSAXContentHandler
	{
	private:
		enum class ElementType :size_t
		{
			Document = 0,
			RootElement,
			FontFaceElement,
			Win32FamilyNameElement,
			FullNameElement,
			PostScriptNameElement
		};

		std::unique_ptr<sfh::FontDatabase> m_db;
		std::vector<ElementType> m_status;

	public:
		HRESULT STDMETHODCALLTYPE startDocument() override
		{
			m_db = std::make_unique<sfh::FontDatabase>();
			m_status.clear();
			m_status.emplace_back(ElementType::Document);
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE endDocument() override
		{
			if (!m_status.empty() && m_status.back() == ElementType::Document)
				m_status.pop_back();
			if (m_status.empty())
				return S_OK;
			return E_FAIL;
		}

		template <typename T, size_t N>
		HRESULT RetrieveAttribute(ISAXAttributes* pAttributes, T& ptr, std::wstring T::* mptr, const wchar_t (&name)[N])
		{
			const wchar_t* attrValue;
			int attrLength;
			assert(mptr != nullptr);
			assert(pAttributes != nullptr);
			RETURN_HR_IF(E_FAIL, FAILED(pAttributes->getValueFromName(L"", 0, name, N - 1, &attrValue, &attrLength)));
			(ptr.*mptr).assign(attrValue, attrLength);
			return S_OK;
		}

		template <typename T, size_t N>
		HRESULT RetrieveAttribute(ISAXAttributes* pAttributes, T& ptr, uint32_t T::* mptr, const wchar_t (&name)[N])
		{
			const wchar_t* attrValue;
			int attrLength;
			assert(mptr != nullptr);
			assert(pAttributes != nullptr);
			RETURN_HR_IF(E_FAIL, FAILED(pAttributes->getValueFromName(L"", 0, name, N - 1, &attrValue, &attrLength)));
			try
			{
				ptr.*mptr = wcstou32(attrValue, attrLength);
			}
			catch (...)
			{
				return E_FAIL;
			}
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE startElement(const wchar_t* pwchNamespaceUri, int cchNamespaceUri,
		                                       const wchar_t* pwchLocalName,
		                                       int cchLocalName, const wchar_t* pwchQName, int cchQName,
		                                       ISAXAttributes* pAttributes) override
		{
			if (m_status.empty())
				return E_FAIL;
			switch (m_status.back())
			{
			case ElementType::Document:
				if (wcsncmp(pwchLocalName, L"FontDatabase", cchLocalName) == 0)
				{
					m_status.emplace_back(ElementType::RootElement);
				}
				else
				{
					return E_FAIL;
				}
				break;
			case ElementType::RootElement:
				if (wcsncmp(pwchLocalName, L"FontFace", cchLocalName) == 0)
				{
					m_db->m_fonts.emplace_back();
					m_status.emplace_back(ElementType::FontFaceElement);
					RETURN_IF_FAILED(
						RetrieveAttribute(pAttributes, m_db->m_fonts.back(), &sfh::FontDatabase::FontFaceElement::m_path
							,
							L"path"));
					RETURN_IF_FAILED(
						RetrieveAttribute(pAttributes, m_db->m_fonts.back(), &sfh::FontDatabase::FontFaceElement::
							m_index,
							L"index"));
					RETURN_IF_FAILED(
						RetrieveAttribute(pAttributes, m_db->m_fonts.back(), &sfh::FontDatabase::FontFaceElement::
							m_weight,
							L"weight"));
					RETURN_IF_FAILED(
						RetrieveAttribute(pAttributes, m_db->m_fonts.back(), &sfh::FontDatabase::FontFaceElement::
							m_oblique,
							L"oblique"));
					RETURN_IF_FAILED(
						RetrieveAttribute(pAttributes, m_db->m_fonts.back(), &sfh::FontDatabase::FontFaceElement::
							m_psOutline,
							L"psOutline"));
				}
				else
				{
					return E_FAIL;
				}
				break;
			case ElementType::FontFaceElement:
				if (wcsncmp(pwchLocalName, L"Win32FamilyName", cchLocalName) == 0)
				{
					m_status.emplace_back(ElementType::Win32FamilyNameElement);
					m_db->m_fonts.back().m_names.emplace_back();
					m_db->m_fonts.back().m_names.back().m_type =
						sfh::FontDatabase::FontFaceElement::NameElement::Win32FamilyName;
				}
				else if (wcsncmp(pwchLocalName, L"FullName", cchLocalName) == 0)
				{
					m_status.emplace_back(ElementType::FullNameElement);
					m_db->m_fonts.back().m_names.emplace_back();
					m_db->m_fonts.back().m_names.back().m_type =
						sfh::FontDatabase::FontFaceElement::NameElement::FullName;
				}
				else if (wcsncmp(pwchLocalName, L"PostScriptName", cchLocalName) == 0)
				{
					m_status.emplace_back(ElementType::PostScriptNameElement);
					m_db->m_fonts.back().m_names.emplace_back();
					m_db->m_fonts.back().m_names.back().m_type =
						sfh::FontDatabase::FontFaceElement::NameElement::PostScriptName;
				}
				else
				{
					return E_FAIL;
				}
				break;
			default:
				return E_FAIL;
			}
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE endElement(const wchar_t* pwchNamespaceUri, int cchNamespaceUri,
		                                     const wchar_t* pwchLocalName,
		                                     int cchLocalName, const wchar_t* pwchQName, int cchQName) override
		{
			if (m_status.empty())
				return E_FAIL;
			switch (m_status.back())
			{
			case ElementType::RootElement:
				if (wcsncmp(pwchLocalName, L"FontDatabase", cchLocalName) == 0)
				{
					m_status.pop_back();
				}
				else
				{
					return E_FAIL;
				}
				break;
			case ElementType::FontFaceElement:
				if (wcsncmp(pwchLocalName, L"FontFace", cchLocalName) == 0)
				{
					m_status.pop_back();
				}
				else
				{
					return E_FAIL;
				}
				break;
			case ElementType::Win32FamilyNameElement:
				if (wcsncmp(pwchLocalName, L"Win32FamilyName", cchLocalName) == 0)
				{
					m_status.pop_back();
				}
				else
				{
					return E_FAIL;
				}
				break;
			case ElementType::FullNameElement:
				if (wcsncmp(pwchLocalName, L"FullName", cchLocalName) == 0)
				{
					m_status.pop_back();
				}
				else
				{
					return E_FAIL;
				}
				break;
			case ElementType::PostScriptNameElement:
				if (wcsncmp(pwchLocalName, L"PostScriptName", cchLocalName) == 0)
				{
					m_status.pop_back();
				}
				else
				{
					return E_FAIL;
				}
				break;
			default:
				return E_FAIL;
			}
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE characters(const wchar_t* pwchChars, int cchChars) override
		{
			if (m_status.empty())
				return E_FAIL;
			switch (m_status.back())
			{
			case ElementType::Win32FamilyNameElement:
			case ElementType::FullNameElement:
			case ElementType::PostScriptNameElement:
				m_db->m_fonts.back().m_names.back().m_name.assign(pwchChars, cchChars);
			}
			// ignore unexpected characters
			return S_OK;
		}

		std::unique_ptr<sfh::FontDatabase> GetFontDatabase()
		{
			return std::move(m_db);
		}
	};
}

std::unique_ptr<sfh::ConfigFile> sfh::ConfigFile::ReadFromFile(const std::wstring& path)
{
	auto extension = std::filesystem::path(path).extension().wstring();
	if (_wcsicmp(extension.c_str(), L".toml") == 0)
		return ReadTomlConfigFromFile(path);
	return ReadXmlConfigFromFile(path);
}

std::unique_ptr<sfh::FontDatabase> sfh::FontDatabase::ReadFromFile(const std::wstring& path)
{
	auto com = wil::CoInitializeEx();

	wil::unique_variant pathVariant;
	wil::com_ptr<IStream> stream;
	THROW_IF_FAILED_MSG(
		SHCreateStreamOnFileEx(
			path.c_str(),
			STGM_FAILIFTHERE | STGM_READ | STGM_SHARE_EXCLUSIVE,
			FILE_ATTRIBUTE_NORMAL,
			FALSE,
			nullptr,
			stream.put()), "CANNOT OPEN FONTDATABASE: %ws", path.c_str());
	InitVariantFromUnknown(stream.query<IUnknown>().get(), pathVariant.addressof());

	auto saxReader = wil::CoCreateInstance<ISAXXMLReader>(CLSID_SAXXMLReader30);
	wil::com_ptr<FontDatabaseSAXContentHandler> handler(new FontDatabaseSAXContentHandler);
	THROW_IF_FAILED(saxReader->putContentHandler(handler.get()));
	THROW_IF_FAILED_MSG(saxReader->parse(pathVariant), "BAD FONTDATABASE: %ws", path.c_str());

	return handler->GetFontDatabase();
}

namespace sfh
{
	void WriteDocumentToFile(wil::com_ptr<IStream> stream, wil::com_ptr<IXMLDOMDocument> document)
	{
		auto mxWriter = wil::CoCreateInstance<IMXWriter>(CLSID_MXXMLWriter30);
		auto saxReader = wil::CoCreateInstance<ISAXXMLReader>(CLSID_SAXXMLReader30);

		wil::unique_variant output;
		wil::unique_variant unk;

		THROW_IF_FAILED(mxWriter->put_encoding(wil::make_bstr(L"UTF-8").get()));
		THROW_IF_FAILED(mxWriter->put_standalone(VARIANT_TRUE));
		THROW_IF_FAILED(mxWriter->put_indent(VARIANT_TRUE));
		InitVariantFromUnknown(stream.query<IUnknown>().get(), unk.reset_and_addressof());
		THROW_IF_FAILED(mxWriter->put_output(unk));

		THROW_IF_FAILED(saxReader->putContentHandler(mxWriter.query<ISAXContentHandler>().get()));
		InitVariantFromUnknown(mxWriter.query<IUnknown>().get(), unk.reset_and_addressof());
		THROW_IF_FAILED(saxReader->putProperty(L"http://xml.org/sax/properties/lexical-handler", unk));
		THROW_IF_FAILED(saxReader->putProperty(L"http://xml.org/sax/properties/declaration-handler", unk));
		InitVariantFromUnknown(document.query<IUnknown>().get(), unk.reset_and_addressof());
		THROW_IF_FAILED(saxReader->parse(unk));
	}

	wil::com_ptr<IXMLDOMDocument> ConfigFileToDocument(const ConfigFile& config)
	{
		auto document = wil::CoCreateInstance<IXMLDOMDocument>(CLSID_DOMDocument30);

		wil::com_ptr<IXMLDOMElement> rootElement;
		THROW_IF_FAILED(document->createElement(wil::make_bstr(L"ConfigFile").get(), rootElement.put()));
		wil::unique_variant value;
		InitVariantFromString(std::to_wstring(config.wmiPollInterval).c_str(), value.addressof());
		THROW_IF_FAILED(rootElement->setAttribute(wil::make_bstr(L"wmiPollInterval").get(), value));
		InitVariantFromString(std::to_wstring(config.lruSize).c_str(), value.addressof());
		THROW_IF_FAILED(rootElement->setAttribute(wil::make_bstr(L"lruSize").get(), value));
		for (auto& indexFile : config.m_indexFile)
		{
			wil::com_ptr<IXMLDOMElement> indexFileElement;
			THROW_IF_FAILED(document->createElement(wil::make_bstr(L"IndexFile").get(), indexFileElement.put()));

			THROW_IF_FAILED(indexFileElement->put_text(wil::make_bstr(indexFile.m_path.c_str()).get()));
			THROW_IF_FAILED(rootElement->appendChild(indexFileElement.get(), nullptr));
		}
		for (auto& monitorProcess : config.m_monitorProcess)
		{
			wil::com_ptr<IXMLDOMElement> monitorProcessElement;
			THROW_IF_FAILED(
				document->createElement(wil::make_bstr(L"MonitorProcess").get(), monitorProcessElement.put()));

			THROW_IF_FAILED(monitorProcessElement->put_text(wil::make_bstr(monitorProcess.m_name.c_str()).get()));
			THROW_IF_FAILED(rootElement->appendChild(monitorProcessElement.get(), nullptr));
		}

		THROW_IF_FAILED(document->putref_documentElement(rootElement.get()));

		return document;
	}

	wil::com_ptr<IXMLDOMDocument> FontDatabaseToDocument(const FontDatabase& db)
	{
		auto document = wil::CoCreateInstance<IXMLDOMDocument>(CLSID_DOMDocument30);

		wil::com_ptr<IXMLDOMElement> rootElement;
		THROW_IF_FAILED(document->createElement(wil::make_bstr(L"FontDatabase").get(), rootElement.put()));

		for (auto& font : db.m_fonts)
		{
			wil::com_ptr<IXMLDOMElement> fontfaceElement;
			THROW_IF_FAILED(document->createElement(wil::make_bstr(L"FontFace").get(), fontfaceElement.put()));

			wil::unique_variant value;

			InitVariantFromString(font.m_path.c_str(), value.reset_and_addressof());
			THROW_IF_FAILED(fontfaceElement->setAttribute(wil::make_bstr(L"path").get(), value));
			InitVariantFromString(std::to_wstring(font.m_index).c_str(), value.reset_and_addressof());
			THROW_IF_FAILED(fontfaceElement->setAttribute(wil::make_bstr(L"index").get(), value));
			InitVariantFromString(std::to_wstring(font.m_weight).c_str(), value.reset_and_addressof());
			THROW_IF_FAILED(fontfaceElement->setAttribute(wil::make_bstr(L"weight").get(), value));
			InitVariantFromString(std::to_wstring(font.m_oblique).c_str(), value.reset_and_addressof());
			THROW_IF_FAILED(fontfaceElement->setAttribute(wil::make_bstr(L"oblique").get(), value));
			InitVariantFromString(std::to_wstring(font.m_psOutline).c_str(), value.reset_and_addressof());
			THROW_IF_FAILED(fontfaceElement->setAttribute(wil::make_bstr(L"psOutline").get(), value));

			for (auto& name : font.m_names)
			{
				wil::com_ptr<IXMLDOMElement> nameElement;
				assert(name.m_type < std::extent_v<decltype(FontDatabase::FontFaceElement::NameElement::TYPEMAP)>);
				THROW_IF_FAILED(
					document->createElement(wil::make_bstr(FontDatabase::FontFaceElement::NameElement::TYPEMAP[name
						.m_type]).get(), nameElement.put()));
				THROW_IF_FAILED(nameElement->put_text(wil::make_bstr(name.m_name.c_str()).get()));
				THROW_IF_FAILED(fontfaceElement->appendChild(nameElement.get(), nullptr));
			}
			THROW_IF_FAILED(rootElement->appendChild(fontfaceElement.get(), nullptr));
		}
		THROW_IF_FAILED(document->putref_documentElement(rootElement.get()));

		return document;
	}
}


void sfh::FontDatabase::WriteToFile(const std::wstring& path, const FontDatabase& db)
{
	auto com = wil::CoInitializeEx();

	wil::com_ptr<IStream> stream;
	THROW_IF_FAILED(
		SHCreateStreamOnFileEx(
			path.c_str(),
			STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE,
			FILE_ATTRIBUTE_NORMAL,
			TRUE,
			nullptr,
			stream.put()));

	auto document = FontDatabaseToDocument(db);
	WriteDocumentToFile(stream, document);
}
