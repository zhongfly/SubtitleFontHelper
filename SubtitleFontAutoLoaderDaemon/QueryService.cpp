#include "pch.h"

#include "Common.h"
#include "ManagedIndexLog.h"
#include "QueryService.h"
#include "RpcServer.h"
#include "EventLog.h"

#include <wil/resource.h>
#include <wil/win32_helpers.h>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <set>
#include <stdexcept>

namespace
{
	struct SearchableFontEntry
	{
		sfh::FontSearchResult m_result;
		std::wstring m_searchKey;
		std::vector<std::wstring> m_indexPaths;
		std::unordered_set<std::wstring> m_indexPathKeys;
		std::vector<std::wstring> m_familyNames;
		std::unordered_set<std::wstring> m_familyNameKeys;
		std::vector<std::wstring> m_fullNames;
		std::unordered_set<std::wstring> m_fullNameKeys;
		std::vector<std::wstring> m_postScriptNames;
		std::unordered_set<std::wstring> m_postScriptNameKeys;
		std::unordered_set<std::wstring> m_searchKeyParts;
	};

	struct FontUiStore
	{
		sfh::FontUiSnapshot m_snapshot;
		std::vector<SearchableFontEntry> m_searchEntries;
	};

	constexpr size_t MAX_FONT_SEARCH_RESULT_COUNT = 500;

	std::wstring BuildFontUiStatusMessage(size_t indexCount)
	{
		return L"已加载 " + std::to_wstring(indexCount) + L" 个字体索引。";
	}

	std::wstring ToLowerCopy(std::wstring value)
	{
		std::transform(value.begin(), value.end(), value.begin(), towlower);
		return value;
	}

	std::wstring JoinSortedNames(std::vector<std::wstring> names)
	{
		if (names.empty())
		{
			return {};
		}

		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());

		std::wstring result;
		for (size_t i = 0; i < names.size(); ++i)
		{
			if (i != 0)
			{
				result += L", ";
			}
			result += names[i];
		}
		return result;
	}

	void AppendSearchKeyPart(SearchableFontEntry& entry, const std::wstring& value)
	{
		auto key = ToLowerCopy(value);
		if (key.empty() || !entry.m_searchKeyParts.insert(key).second)
		{
			return;
		}

		if (!entry.m_searchKey.empty())
		{
			entry.m_searchKey += L'\n';
		}
		entry.m_searchKey += key;
	}

	void AppendDisplayName(std::vector<std::wstring>& values, std::unordered_set<std::wstring>& keys, const std::wstring& value)
	{
		auto key = ToLowerCopy(value);
		if (!value.empty() && keys.insert(key).second)
		{
			values.push_back(value);
		}
	}

	void SyncSearchEntryResult(SearchableFontEntry& entry)
	{
		entry.m_result.m_familyNames = JoinSortedNames(entry.m_familyNames);
		entry.m_result.m_fullNames = JoinSortedNames(entry.m_fullNames);
		entry.m_result.m_postScriptNames = JoinSortedNames(entry.m_postScriptNames);
		entry.m_result.m_indexPath = JoinSortedNames(entry.m_indexPaths);
	}

	void AppendSearchName(
		SearchableFontEntry& entry,
		const sfh::FontDatabase::FontFaceElement::NameElement& name)
	{
		AppendSearchKeyPart(entry, name.m_name);

		switch (name.m_type)
		{
		case sfh::FontDatabase::FontFaceElement::NameElement::Win32FamilyName:
			AppendDisplayName(entry.m_familyNames, entry.m_familyNameKeys, name.m_name);
			break;
		case sfh::FontDatabase::FontFaceElement::NameElement::FullName:
			AppendDisplayName(entry.m_fullNames, entry.m_fullNameKeys, name.m_name);
			break;
		case sfh::FontDatabase::FontFaceElement::NameElement::PostScriptName:
			AppendDisplayName(entry.m_postScriptNames, entry.m_postScriptNameKeys, name.m_name);
			break;
		}
	}

	void AppendFontMetadata(
		SearchableFontEntry& entry,
		const sfh::FontDatabase::FontFaceElement& font)
	{
		for (const auto& name : font.m_names)
		{
			if (!name.m_name.empty())
			{
				AppendSearchName(entry, name);
			}
		}

		AppendSearchKeyPart(entry, sfh::GetManagedIndexPreferredFontName(font));
		SyncSearchEntryResult(entry);
	}

	sfh::ManagedIndexFontLogSummary BuildNormalizedFontNameSummary(const sfh::FontDatabase& db)
	{
		constexpr size_t maxVisibleNames = 12;
		constexpr size_t maxNameLength = 32;

		std::vector<std::wstring> displayNames;
		std::unordered_set<std::wstring> normalizedNames;
		displayNames.reserve(db.m_fonts.size());
		normalizedNames.reserve(db.m_fonts.size());
		for (const auto& font : db.m_fonts)
		{
			auto displayName = sfh::GetManagedIndexPreferredFontName(font);
			auto key = ToLowerCopy(displayName);
			if (normalizedNames.insert(key).second)
			{
				displayNames.push_back(std::move(displayName));
			}
		}

		std::sort(displayNames.begin(), displayNames.end(), [](const std::wstring& left, const std::wstring& right)
		{
			auto leftKey = ToLowerCopy(left);
			auto rightKey = ToLowerCopy(right);
			if (leftKey != rightKey)
			{
				return leftKey < rightKey;
			}
			return left < right;
		});

		sfh::ManagedIndexFontLogSummary summary;
		summary.m_fontCount = displayNames.size();
		if (displayNames.empty())
		{
			summary.m_fontNamesSummary = L"<none>";
			return summary;
		}

		size_t visibleCount = 0;
		for (const auto& displayName : displayNames)
		{
			if (visibleCount >= maxVisibleNames)
			{
				break;
			}

			if (!summary.m_fontNamesSummary.empty())
			{
				summary.m_fontNamesSummary += L", ";
			}
			summary.m_fontNamesSummary += sfh::TruncateManagedIndexDisplayName(displayName, maxNameLength);
			++visibleCount;
		}

		if (visibleCount < displayNames.size())
		{
			summary.m_fontNamesSummary += L", ... (+" + std::to_wstring(displayNames.size() - visibleCount) + L")";
		}
		return summary;
	}

	SearchableFontEntry BuildSearchableFontEntry(
		const sfh::LoadedFontDatabase& loadedDatabase,
		const sfh::FontDatabase::FontFaceElement& font)
	{
		SearchableFontEntry entry;
		entry.m_result.m_displayName = sfh::GetManagedIndexPreferredFontName(font);
		entry.m_result.m_fontPath = font.m_path.Get();
		entry.m_result.m_faceIndex = font.m_index;
		entry.m_indexPaths.push_back(loadedDatabase.m_indexPath.wstring());
		entry.m_indexPathKeys.insert(ToLowerCopy(loadedDatabase.m_indexPath.wstring()));
		AppendFontMetadata(entry, font);
		return entry;
	}

	void AppendIndexPath(SearchableFontEntry& entry, const std::filesystem::path& indexPath)
	{
		auto display = indexPath.wstring();
		auto key = ToLowerCopy(display);
		if (entry.m_indexPathKeys.insert(key).second)
		{
			entry.m_indexPaths.push_back(display);
			SyncSearchEntryResult(entry);
		}
	}

	void SortSearchEntries(std::vector<SearchableFontEntry>& entries)
	{
		std::stable_sort(entries.begin(), entries.end(), [](const SearchableFontEntry& left, const SearchableFontEntry& right)
		{
			if (left.m_result.m_displayName != right.m_result.m_displayName)
			{
				return left.m_result.m_displayName < right.m_result.m_displayName;
			}
			if (left.m_result.m_indexPath != right.m_result.m_indexPath)
			{
				return left.m_result.m_indexPath < right.m_result.m_indexPath;
			}
			if (left.m_result.m_fontPath != right.m_result.m_fontPath)
			{
				return left.m_result.m_fontPath < right.m_result.m_fontPath;
			}
			return left.m_result.m_faceIndex < right.m_result.m_faceIndex;
		});
	}

	sfh::FontUiSnapshot BuildInitialFontUiSnapshot()
	{
		sfh::FontUiSnapshot snapshot;
		snapshot.m_statusMessage = L"正在加载字体索引...";
		return snapshot;
	}

	sfh::FontIndexSummary BuildFontIndexSummary(const sfh::LoadedFontDatabase& loadedDatabase)
	{
		sfh::FontIndexSummary summary;
		summary.m_indexPath = loadedDatabase.m_indexPath.wstring();
		if (loadedDatabase.m_database == nullptr)
		{
			summary.m_fontNamesSummary = L"<none>";
			return summary;
		}

		std::unordered_set<std::wstring> uniquePaths;
		uniquePaths.reserve(loadedDatabase.m_database->m_fonts.size());
		for (const auto& font : loadedDatabase.m_database->m_fonts)
		{
			if (!font.m_path.empty())
			{
				uniquePaths.insert(ToLowerCopy(font.m_path.Get()));
			}
		}

		auto normalizedNameSummary = BuildNormalizedFontNameSummary(*loadedDatabase.m_database);
		summary.m_fontFileCount = uniquePaths.size();
		summary.m_fontNameCount = normalizedNameSummary.m_fontCount;
		summary.m_fontNamesSummary = std::move(normalizedNameSummary.m_fontNamesSummary);
		return summary;
	}

	std::shared_ptr<const FontUiStore> BuildFontUiStore(const std::vector<sfh::LoadedFontDatabase>& loadedDatabases)
	{
		auto store = std::make_shared<FontUiStore>();
		std::unordered_map<std::wstring, size_t> faceToEntryIndex;
		store->m_snapshot.m_isLoaded = true;
		store->m_snapshot.m_hasStaleData = false;
		store->m_snapshot.m_statusMessage = BuildFontUiStatusMessage(loadedDatabases.size());
		store->m_snapshot.m_indexSummaries.reserve(loadedDatabases.size());
		for (const auto& loadedDatabase : loadedDatabases)
		{
			store->m_snapshot.m_indexSummaries.push_back(BuildFontIndexSummary(loadedDatabase));
			if (loadedDatabase.m_database != nullptr)
			{
				for (const auto& font : loadedDatabase.m_database->m_fonts)
				{
					auto faceKey = ToLowerCopy(font.m_path.Get());
					faceKey += L"\x1f";
					faceKey += std::to_wstring(font.m_index);
					auto existing = faceToEntryIndex.find(faceKey);
					if (existing == faceToEntryIndex.end())
					{
						faceToEntryIndex.emplace(std::move(faceKey), store->m_searchEntries.size());
						store->m_searchEntries.push_back(BuildSearchableFontEntry(loadedDatabase, font));
					}
					else
					{
						AppendIndexPath(store->m_searchEntries[existing->second], loadedDatabase.m_indexPath);
						AppendFontMetadata(store->m_searchEntries[existing->second], font);
					}
				}
			}
		}
		SortSearchEntries(store->m_searchEntries);
		return store;
	}

	template <typename T, bool AllowDuplicate = true>
	class QueryTrie
	{
	private:
		struct TrieNode
		{
			std::vector<std::pair<std::wstring, std::unique_ptr<TrieNode>>> m_branch;
			std::vector<T*> m_data;

			void CollectData(std::vector<T*>& ret)
			{
				ret.insert(ret.end(), m_data.begin(), m_data.end());
				for (auto& branch : m_branch)
				{
					branch.second->CollectData(ret);
				}
			}

			void SortList()
			{
				std::sort(m_branch.begin(), m_branch.end());
			}

			std::pair<std::wstring, std::unique_ptr<TrieNode>>* SearchPrefix(wchar_t leading)
			{
				auto left = m_branch.begin();
				auto right = m_branch.end();
				auto mid = left + (right - left) / 2;
				auto result = right;
				while (left != right)
				{
					if (mid->first.empty())
					{
						throw std::logic_error("QueryTrie contains empty arc");
					}
					if (mid->first[0] == leading)
					{
						result = mid;
						break;
					}
					else if (mid->first[0] > leading)
					{
						right = mid;
					}
					else
					{
						left = mid + 1;
					}
					mid = left + (right - left) / 2;
				}
				if (result == m_branch.end())
					return nullptr;
				return &(*result);
			}
		};

		TrieNode m_rootNode;
	public:
		void AddEntry(const wchar_t* key, T* value)
		{
			if (*key == 0)
			{
				return;
			}
			const wchar_t* keyPointer = key;
			TrieNode* node = &m_rootNode;
			while (node)
			{
				if (*keyPointer == 0)
				{
					return;
				}
				auto result = node->SearchPrefix(*keyPointer);
				if (result == nullptr)
				{
					auto newNode = std::make_unique<TrieNode>();
					newNode->m_data.push_back(value);
					node->m_branch.emplace_back(keyPointer, std::move(newNode));
					node->SortList();
					return;
				}
				else
				{
					auto arcPointer = result->first.c_str();
					while (*keyPointer && *arcPointer && *keyPointer == *arcPointer)
					{
						++arcPointer;
						++keyPointer;
					}
					if (*arcPointer == 0 && *keyPointer == 0)
					{
						if constexpr (AllowDuplicate)
						{
							result->second->m_data.push_back(value);
						}
						return;
					}
					else if (*arcPointer == 0)
					{
						node = result->second.get();
					}
					else if (*keyPointer == 0)
					{
						auto keyLength = arcPointer - result->first.c_str();
						auto newNode = std::make_unique<TrieNode>();
						newNode->m_data.push_back(value);
						newNode->m_branch.emplace_back(arcPointer, std::move(result->second));
						result->first.resize(keyLength);
						result->second = std::move(newNode);
						node->SortList();
						return;
					}
					else
					{
						auto keyLength = arcPointer - result->first.c_str();
						auto intermediateNode = std::make_unique<TrieNode>();
						auto newNode = std::make_unique<TrieNode>();
						newNode->m_data.push_back(value);
						intermediateNode->m_branch.emplace_back(arcPointer, std::move(result->second));
						intermediateNode->m_branch.emplace_back(keyPointer, std::move(newNode));
						intermediateNode->SortList();
						result->first.resize(keyLength);
						result->second = std::move(intermediateNode);
						node->SortList();
						return;
					}
				}
			}
		}

		std::vector<T*> QueryEntry(const wchar_t* key, bool truncated)
		{
			if (*key == 0)
			{
				return {};
			}
			std::vector<T*> ret;
			const wchar_t* keyPointer = key;
			TrieNode* node = &m_rootNode;
			while (node)
			{
				if (*keyPointer == 0)
				{
					return ret;
				}
				auto result = node->SearchPrefix(*keyPointer);
				if (result == nullptr)
				{
					return ret;
				}
				else
				{
					auto arcPointer = result->first.c_str();
					while (*keyPointer && *arcPointer && *keyPointer == *arcPointer)
					{
						++arcPointer;
						++keyPointer;
					}
					if (*arcPointer == 0 && *keyPointer == 0)
					{
						if (truncated)
						{
							result->second->CollectData(ret);
						}
						else
						{
							ret.insert(ret.end(), result->second->m_data.begin(), result->second->m_data.end());
						}
						return ret;
					}
					else if (*arcPointer == 0)
					{
						node = result->second.get();
					}
					else if (*keyPointer == 0)
					{
						if (truncated)
						{
							result->second->CollectData(ret);
						}
						return ret;
					}
					else
					{
						return ret;
					}
				}
			}
			return ret;
		}

		void Dump(std::wostream& stream)
		{
			struct Hierarchy
			{
				const TrieNode* node;
				size_t nextArc = 0;
			};
			std::vector<Hierarchy> iterateStack;
			iterateStack.emplace_back(&m_rootNode, 0);
			std::wstring prefix;
			if (!m_rootNode.m_branch.empty() || !m_rootNode.m_data.empty())
				stream << L"\"\"\n";
			while (!iterateStack.empty())
			{
				auto& top = iterateStack.back();
				if (top.nextArc == 0)
				{
					for (size_t i = 0; i < top.node->m_data.size(); ++i)
					{
						wchar_t head = (i == top.node->m_data.size() - 1 && top.node->m_branch.empty()) ? L'└' : L'├';
						stream << prefix << head << L" [" << top.node->m_data[i]->m_path.Get() << L"]\n";
					}
				}
				if (top.nextArc == top.node->m_branch.size())
				{
					iterateStack.pop_back();
					prefix.resize(prefix.size() >= 5 ? prefix.size() - 5 : 0);
					continue;
				}
				wchar_t head = top.nextArc == top.node->m_branch.size() - 1 ? L'└' : L'├';
				stream << prefix << head << L"── \"" << top.node->m_branch[top.nextArc].first << L"\"\n";
				if (head == L'└')
					prefix += L"     ";
				else
					prefix += L"│    ";
				auto child = top.node->m_branch[top.nextArc].second.get();
				++top.nextArc;
				if (child)
				{
					iterateStack.emplace_back(child, 0);
				}
			}
		}

		void DumpToFile(const std::wstring& path)
		{
			std::wostringstream oss;
			Dump(oss);
			sfh::SetFileContent(path, sfh::WideToUtf8String(oss.str()));
		}
	};
}

class sfh::QueryService::Implementation : public sfh::IRpcRequestHandler
{
private:
	std::mutex m_accessLock;

	QueryTrie<FontDatabase::FontFaceElement, false> m_fullName;
	QueryTrie<FontDatabase::FontFaceElement, false> m_postScriptName;
	QueryTrie<FontDatabase::FontFaceElement, true> m_win32FamilyName;
	std::unordered_map<const FontDatabase::FontFaceElement*, size_t> m_fontPriority;
	std::vector<std::unique_ptr<FontDatabase>> m_dbs;
	std::atomic<std::shared_ptr<const FontUiStore>> m_fontUiStore;

	IDaemon* m_daemon;

	wil::unique_handle m_version;
	wil::unique_mapview_ptr<uint32_t> m_versionMem;
public:
	Implementation(IDaemon* daemon)
		: m_daemon(daemon)
	{
		std::wstring versionShmName = L"SubtitleFontAutoLoaderSHM-";
		versionShmName += GetCurrentProcessUserSid();
		m_version.reset(CreateFileMappingW(
			INVALID_HANDLE_VALUE,
			nullptr,
			PAGE_READWRITE,
			0, 4,
			versionShmName.c_str()));
		THROW_LAST_ERROR_IF(!m_version.is_valid());
		m_versionMem.reset(static_cast<uint32_t*>(MapViewOfFile(
			m_version.get(),
			FILE_MAP_WRITE,
			0, 0,
			sizeof(uint32_t))));
		THROW_LAST_ERROR_IF(m_versionMem.get() == nullptr);

		m_fontUiStore.store(
			std::shared_ptr<const FontUiStore>(std::make_shared<FontUiStore>(FontUiStore{ BuildInitialFontUiSnapshot() })),
			std::memory_order_release);
	}

	void UpdateVerison()
	{
		auto newValue = InterlockedIncrement(m_versionMem.get());
		EventLog::GetInstance().LogDaemonBumpVersion(newValue - 1, newValue);
	}

	void Load(std::vector<LoadedFontDatabase>&& loadedDatabases, bool publishVersion)
	{
		QueryTrie<FontDatabase::FontFaceElement, true> win32FamilyName;
		QueryTrie<FontDatabase::FontFaceElement, false> fullName;
		QueryTrie<FontDatabase::FontFaceElement, false> postScriptName;
		std::unordered_map<const FontDatabase::FontFaceElement*, size_t> fontPriority;
		auto fontUiStore = BuildFontUiStore(loadedDatabases);
		std::vector<std::unique_ptr<FontDatabase>> dbs;
		dbs.reserve(loadedDatabases.size());
		for (size_t priority = 0; priority < loadedDatabases.size(); ++priority)
		{
			auto db = std::move(loadedDatabases[priority].m_database);
			if (db == nullptr)
			{
				continue;
			}
			for (auto& font : db->m_fonts)
			{
				fontPriority.emplace(&font, priority);
				for (auto& name : font.m_names)
				{
					if (name.m_type == name.Win32FamilyName)
					{
						win32FamilyName.AddEntry(name.m_name.c_str(), &font);
					}
					else if (name.m_type == name.FullName)
					{
						fullName.AddEntry(name.m_name.c_str(), &font);
					}
					else if (name.m_type == name.PostScriptName)
					{
						postScriptName.AddEntry(name.m_name.c_str(), &font);
					}
				}
			}
			dbs.push_back(std::move(db));
		}

		if (g_debugOutputEnabled)
		{
			std::filesystem::path exePath{wil::GetModuleFileNameW<wil::unique_process_heap_string>().get()};
			exePath.remove_filename();
			win32FamilyName.DumpToFile(exePath / L"win32FamilyName.trie.txt");
			fullName.DumpToFile(exePath / L"fullName.trie.txt");
			postScriptName.DumpToFile(exePath / L"postScriptName.trie.txt");
		}

		std::lock_guard lg(m_accessLock);
		m_dbs = std::move(dbs);
		m_win32FamilyName = std::move(win32FamilyName);
		m_fullName = std::move(fullName);
		m_postScriptName = std::move(postScriptName);
		m_fontPriority = std::move(fontPriority);
		m_fontUiStore.store(std::move(fontUiStore), std::memory_order_release);
		if (publishVersion)
		{
			UpdateVerison();
		}
	}

	void PublishVersion()
	{
		std::lock_guard lg(m_accessLock);
		UpdateVerison();
	}

	FontUiSnapshot CaptureFontUiSnapshot(std::wstring_view query) const
	{
		auto store = m_fontUiStore.load(std::memory_order_acquire);
		if (!store)
		{
			return BuildInitialFontUiSnapshot();
		}

		auto snapshot = store->m_snapshot;
		snapshot.m_searchResults.clear();
		snapshot.m_totalSearchResultCount = 0;
		snapshot.m_isSearchResultTruncated = false;
		if (query.empty())
		{
			return snapshot;
		}

		auto normalizedQuery = ToLowerCopy(std::wstring(query));
		for (const auto& entry : store->m_searchEntries)
		{
			if (entry.m_searchKey.find(normalizedQuery) == std::wstring::npos)
			{
				continue;
			}

			++snapshot.m_totalSearchResultCount;
			if (snapshot.m_searchResults.size() < MAX_FONT_SEARCH_RESULT_COUNT)
			{
				snapshot.m_searchResults.push_back(entry.m_result);
			}
			else
			{
				snapshot.m_isSearchResultTruncated = true;
			}
		}
		return snapshot;
	}

	size_t GetPriority(const FontDatabase::FontFaceElement* face) const
	{
		auto result = m_fontPriority.find(face);
		THROW_HR_IF(E_UNEXPECTED, result == m_fontPriority.end());
		return result->second;
	}

	void RetainPriority(std::vector<FontDatabase::FontFaceElement*>& faces, size_t priority) const
	{
		std::erase_if(faces, [&](FontDatabase::FontFaceElement* face)
		{
			return GetPriority(face) != priority;
		});
	}

	void SortByPriority(std::vector<FontDatabase::FontFaceElement*>& faces) const
	{
		std::stable_sort(faces.begin(), faces.end(), [&](FontDatabase::FontFaceElement* left,
		                                                 FontDatabase::FontFaceElement* right)
		{
			return GetPriority(left) < GetPriority(right);
		});
	}

	static std::wstring GetFamilyDedupKey(const FontDatabase::FontFaceElement* face)
	{
		std::vector<const std::wstring*> familyNames;
		for (const auto& name : face->m_names)
		{
			if (name.m_type == FontDatabase::FontFaceElement::NameElement::Win32FamilyName)
			{
				familyNames.push_back(&name.m_name);
			}
		}

		std::sort(familyNames.begin(), familyNames.end(), [](const std::wstring* left, const std::wstring* right)
		{
			return *left < *right;
		});
		familyNames.erase(std::unique(familyNames.begin(), familyNames.end(), [](const std::wstring* left,
		                                                                         const std::wstring* right)
		{
			return *left == *right;
		}), familyNames.end());

		std::wstring key;
		for (const auto* familyName : familyNames)
		{
			key += *familyName;
			key.push_back(L'\x1f');
		}
		return key;
	}

	void RetainDistinctFamilyFaces(std::vector<FontDatabase::FontFaceElement*>& faces) const
	{
		SortByPriority(faces);
		std::set<std::tuple<std::wstring, uint32_t, uint32_t, uint32_t>> seen;
		std::vector<FontDatabase::FontFaceElement*> filtered;
		filtered.reserve(faces.size());
		for (auto face : faces)
		{
			auto [iter, inserted] = seen.emplace(
				GetFamilyDedupKey(face),
				face->m_weight,
				face->m_oblique,
				face->m_psOutline);
			if (inserted)
			{
				filtered.push_back(face);
			}
		}
		faces = std::move(filtered);
	}

	std::optional<size_t> GetHighestPriority(const std::vector<FontDatabase::FontFaceElement*>& first,
	                                         const std::vector<FontDatabase::FontFaceElement*>& second) const
	{
		std::optional<size_t> ret;
		auto update = [&](const std::vector<FontDatabase::FontFaceElement*>& faces)
		{
			for (auto face : faces)
			{
				auto priority = GetPriority(face);
				if (!ret.has_value() || priority < *ret)
					ret = priority;
			}
		};
		update(first);
		update(second);
		return ret;
	}

	static void AppendFontFace(FontQueryResponse& response, const std::vector<FontDatabase::FontFaceElement*>& faces,
	                           std::vector<std::pair<std::wstring_view, uint32_t>>& dedup)
	{
		for (auto face : faces)
		{
			auto unique = std::make_pair(std::wstring_view(face->m_path.Get()), face->m_index);
			if (std::find(dedup.begin(), dedup.end(), unique) != dedup.end())
				continue;
			dedup.push_back(unique);
			auto font = response.add_fonts();
			for (auto name : face->m_names)
			{
				switch (name.m_type)
				{
				case FontDatabase::FontFaceElement::NameElement::Win32FamilyName:
					font->add_familyname(WideToUtf8String(name.m_name));
					break;
				case FontDatabase::FontFaceElement::NameElement::FullName:
					font->add_gdifullname(WideToUtf8String(name.m_name));
					break;
				case FontDatabase::FontFaceElement::NameElement::PostScriptName:
					font->add_postscriptname(WideToUtf8String(name.m_name));
					break;
				}
			}
			font->set_path(WideToUtf8String(face->m_path.Get()));
			font->set_weight(face->m_weight);
			font->set_oblique(face->m_oblique);
			font->set_ispsoutline(face->m_psOutline);
		}
	}

	FontQueryResponse HandleRequest(const FontQueryRequest& request) override
	{
		std::lock_guard lg(m_accessLock);
		FontQueryResponse ret;
		std::wstring queryString = Utf8ToWideString(request.querystring());
		bool doTruncated = false;
		if (queryString.size() == 31)
			doTruncated = true;
		std::vector<std::pair<std::wstring_view, uint32_t>> dedup;

		ret.set_version(1);

		auto family = m_win32FamilyName.QueryEntry(queryString.c_str(), doTruncated);
		RetainDistinctFamilyFaces(family);
		if (!family.empty())
		{
			AppendFontFace(ret, family, dedup);
			return ret;
		}

		auto postscript = m_postScriptName.QueryEntry(queryString.c_str(), doTruncated);
		auto fullname = m_fullName.QueryEntry(queryString.c_str(), doTruncated);
		auto highestPriority = GetHighestPriority(postscript, fullname);
		if (highestPriority.has_value())
		{
			RetainPriority(postscript, *highestPriority);
			RetainPriority(fullname, *highestPriority);
		}
		AppendFontFace(ret, postscript, dedup);
		AppendFontFace(ret, fullname, dedup);
		return ret;
	}

	IRpcRequestHandler* GetRpcRequestHandler()
	{
		return this;
	}
};

sfh::QueryService::QueryService(IDaemon* daemon)
	: m_impl(std::make_unique<Implementation>(daemon))
{
}

sfh::QueryService::~QueryService() = default;

void sfh::QueryService::Load(std::vector<LoadedFontDatabase>&& dbs, bool publishVersion)
{
	m_impl->Load(std::move(dbs), publishVersion);
}

void sfh::QueryService::PublishVersion()
{
	m_impl->PublishVersion();
}

sfh::FontUiSnapshot sfh::QueryService::CaptureFontUiSnapshot(std::wstring_view query) const
{
	return m_impl->CaptureFontUiSnapshot(query);
}

sfh::IRpcRequestHandler* sfh::QueryService::GetRpcRequestHandler()
{
	return m_impl->GetRpcRequestHandler();
}
