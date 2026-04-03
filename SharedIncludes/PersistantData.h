#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace sfh
{
	struct ConfigFile
	{
		struct IndexFileElement
		{
			// content
			std::wstring m_path;
			std::vector<std::wstring> m_sourceFolders;
		};

		struct MonitorProcessElement
		{
			// content
			std::wstring m_name;
		};

		struct ProcessMissingFontIgnoreElement
		{
			// content
			std::vector<std::wstring> m_regex;
			std::vector<std::wstring> m_processes;
			std::wstring m_flags;
		};

		uint32_t wmiPollInterval = 500;
		uint32_t lruSize = 100;
		bool managedIndexNotifications = false;
		bool managedIndexFailureNotifications = true;
		bool missingFontNotifications = false;
		std::vector<std::wstring> missingFontIgnore;
		std::vector<ProcessMissingFontIgnoreElement> processMissingFontIgnore;

		// content
		std::vector<IndexFileElement> m_indexFile;
		std::vector<MonitorProcessElement> m_monitorProcess;

		static std::unique_ptr<ConfigFile> ReadFromFile(const std::wstring& path);
	};


	class SharedPath
	{
	private:
		std::shared_ptr<const std::wstring> m_value;

		static const std::wstring& Empty()
		{
			static const std::wstring s_empty;
			return s_empty;
		}

	public:
		SharedPath() = default;
		SharedPath(const wchar_t* value) : m_value(value && *value ? std::make_shared<const std::wstring>(value) : nullptr) {}
		SharedPath(const std::wstring& value) : m_value(value.empty() ? nullptr : std::make_shared<const std::wstring>(value)) {}
		SharedPath(std::wstring&& value) : m_value(value.empty() ? nullptr : std::make_shared<const std::wstring>(std::move(value))) {}

		SharedPath& operator=(const wchar_t* value) { m_value = (value && *value) ? std::make_shared<const std::wstring>(value) : nullptr; return *this; }
		SharedPath& operator=(const std::wstring& value) { m_value = value.empty() ? nullptr : std::make_shared<const std::wstring>(value); return *this; }
		SharedPath& operator=(std::wstring&& value) { m_value = value.empty() ? nullptr : std::make_shared<const std::wstring>(std::move(value)); return *this; }

		void assign(const wchar_t* value, size_t length) { m_value = (length == 0) ? nullptr : std::make_shared<const std::wstring>(value, length); }

		const std::wstring& Get() const { return m_value ? *m_value : Empty(); }
		operator const std::wstring&() const { return Get(); }
		const wchar_t* c_str() const { return Get().c_str(); }
		bool empty() const { return !m_value || m_value->empty(); }
		std::wstring wstring() const { return Get(); }

		void SetShared(std::shared_ptr<const std::wstring> ptr) { m_value = std::move(ptr); }
	};


	struct FontDatabase
	{
		struct FontFaceElement
		{
			struct NameElement
			{
				// name
				enum NameType : size_t
				{
					Win32FamilyName = 0,
					FullName,
					PostScriptName
				} m_type;

				static constexpr const wchar_t* TYPEMAP[] = {
					L"Win32FamilyName",
					L"FullName",
					L"PostScriptName"
				};

				// content
				std::wstring m_name;

				// help functions
				NameElement() = default;

				NameElement(NameType nameType, std::wstring&& name)
					: m_type(nameType), m_name(name)
				{
				}

				bool operator<(const NameElement& rhs) const
				{
					if (this->m_type == rhs.m_type)
						return this->m_name < rhs.m_name;
					return this->m_type < rhs.m_type;
				}

				bool operator==(const NameElement& rhs) const
				{
					return this->m_type == rhs.m_type && this->m_name == rhs.m_name;
				}
			};

			// attribute
			SharedPath m_path;
			uint32_t m_index = std::numeric_limits<uint32_t>::max();
			uint32_t m_weight;
			uint32_t m_oblique;
			uint32_t m_psOutline;
			// content
			std::vector<NameElement> m_names;
		};

		std::vector<FontFaceElement> m_fonts;

		void DeduplicatePaths();

		static std::unique_ptr<FontDatabase> ReadFromFile(const std::wstring& path);
		static void WriteToFile(const std::wstring& path, const FontDatabase& db);
	};
}
