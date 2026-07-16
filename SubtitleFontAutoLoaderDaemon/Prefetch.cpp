#include "pch.h"

#include <filesystem>
#include <regex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "Prefetch.h"
#include "Common.h"
#include "EventLog.h"
#include "ToastNotifier.h"
#include "../FontIndexCore/FontIndexCore.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace
{
	constexpr DWORD PREFETCH_FONT_RESOURCE_FLAGS = FR_PRIVATE | FR_NOT_ENUM;

	std::string BuildFontResourceErrorMessage(const char* operation, const std::wstring& path)
	{
		const auto error = GetLastError();
		std::string message = operation;
		message += " failed: ";
		message += sfh::WideToUtf8String(path);
		if (error != ERROR_SUCCESS)
		{
			message += " (Win32 error ";
			message += std::to_string(error);
			message += ")";
		}
		return message;
	}

	void AddPrefetchedFontResource(const std::wstring& path)
	{
		SetLastError(ERROR_SUCCESS);
		const int addedCount = AddFontResourceExW(path.c_str(), PREFETCH_FONT_RESOURCE_FLAGS, nullptr);
		if (addedCount == 0)
		{
			throw std::runtime_error(BuildFontResourceErrorMessage("AddFontResourceExW", path));
		}
	}

	void RemovePrefetchedFontResource(const std::wstring& path)
	{
		SetLastError(ERROR_SUCCESS);
		if (RemoveFontResourceExW(path.c_str(), PREFETCH_FONT_RESOURCE_FLAGS, nullptr) == FALSE)
		{
			throw std::runtime_error(BuildFontResourceErrorMessage("RemoveFontResourceExW", path));
		}
	}

	void TryLogPrefetchCleanupFailure(const wchar_t* operation, const std::exception& e)
	{
		try
		{
			sfh::EventLog::GetInstance().LogDebugMessage(
				L"prefetch cleanup failed: operation=%ls error=\"%ls\"",
				operation,
				sfh::Utf8ToWideString(e.what()).c_str());
		}
		catch (...)
		{
		}
	}
}

template <typename T>
class SimpleLRU
{
private:
	struct Node
	{
		Node* m_prev;
		Node* m_next;

		T m_data;

		// detach a node from chain, return next node
		static Node& Detach(Node& node)
		{
			if (node.m_prev)
			{
				node.m_prev->m_next = node.m_next;
			}
			node.m_next->m_prev = node.m_prev;
			return *node.m_next;
		}

		// attach node before 'pos',return the attached node
		static Node& Attach(Node& node, Node& pos)
		{
			node.m_next = &pos;
			node.m_prev = pos.m_prev;
			pos.m_prev = &node;
			if (node.m_prev)
				node.m_prev->m_next = &node;
			return node;
		}
	};

	Node m_end{nullptr, nullptr};
	std::vector<Node> m_nodes;
	Node* m_head = &m_end;

	std::unordered_map<T, Node*> m_hashmap;

	size_t m_capacity;

	std::mutex m_lock;

	void AdjustToHead(Node& node, bool inList)
	{
		if (inList)
		{
			if (&node == m_head)
				return;
			Node::Detach(node);
		}
		Node::Attach(node, *m_head);
		m_head = &node;
	}

public:
	SimpleLRU(size_t initialSize)
		: m_capacity(initialSize)
	{
		m_nodes.reserve(initialSize);
	}

	bool HasCapacity() const
	{
		return m_capacity != 0;
	}

	bool Contains(const T& value)
	{
		std::lock_guard lg(m_lock);
		auto findResult = m_hashmap.find(value);
		if (findResult == m_hashmap.end())
		{
			return false;
		}
		AdjustToHead(*findResult->second, true);
		return true;
	}

	std::optional<T> PutNew(T&& value)
	{
		std::lock_guard lg(m_lock);
		if (m_capacity == 0)
		{
			return std::nullopt;
		}
		auto findResult = m_hashmap.find(value);
		if (m_hashmap.end() == findResult)
		{
			if (m_nodes.size() == m_capacity)
			{
				auto lastNode = m_end.m_prev;
				T evicted = lastNode->m_data;
				T newValue = std::move(value);
				auto inserted = m_hashmap.emplace(newValue, lastNode);
				try
				{
					lastNode->m_data = std::move(newValue);
					m_hashmap.erase(evicted);
					AdjustToHead(*lastNode, true);
					return evicted;
				}
				catch (...)
				{
					m_hashmap.erase(inserted.first);
					throw;
				}
			}
			else
			{
				T newValue = std::move(value);
				m_nodes.emplace_back();
				auto node = &m_nodes.back();
				try
				{
					node->m_data = std::move(newValue);
					m_hashmap.emplace(node->m_data, node);
					AdjustToHead(*node, false);
				}
				catch (...)
				{
					m_nodes.pop_back();
					throw;
				}
			}
			return std::nullopt;
		}
		AdjustToHead(*findResult->second, true);
		return std::nullopt;
	}

	std::vector<T> GetVector()
	{
		std::lock_guard lg(m_lock);
		std::vector<T> ret;
		for (Node* node = m_head; node != &m_end; node = node->m_next)
		{
			ret.emplace_back(node->m_data);
		}
		return ret;
	}
};

class sfh::Prefetch::Implementation : public sfh::IRpcFeedbackHandler
{
	struct CompiledRegexRule
	{
		std::wstring m_pattern;
		std::wregex m_regex;
	};

	struct CompiledProcessRule
	{
		std::vector<CompiledRegexRule> m_regex;
		std::vector<std::wstring> m_processes;
	};

	IDaemon* m_daemon;
	SimpleLRU<std::wstring> m_lru;
	bool m_missingFontNotificationsEnabled = true;
	std::vector<CompiledRegexRule> m_missingFontIgnore;
	std::vector<CompiledProcessRule> m_processMissingFontIgnore;
	std::mutex m_resourceLock;
	std::unordered_set<std::wstring> m_loadedFontResources;

	std::wstring m_cachePath;

public:
	Implementation(
		IDaemon* daemon,
		size_t prefetchCount,
		const std::wstring& lruPath,
		bool missingFontNotificationsEnabled,
		std::vector<std::wstring> missingFontIgnore,
		std::vector<ConfigFile::ProcessMissingFontIgnoreElement> processMissingFontIgnore)
		: m_daemon(daemon),
		  m_lru(prefetchCount),
		  m_missingFontNotificationsEnabled(missingFontNotificationsEnabled),
		  m_cachePath(lruPath)
	{
		InitializeMissingFontIgnoreRules(std::move(missingFontIgnore));
		InitializeProcessMissingFontIgnoreRules(std::move(processMissingFontIgnore));
		LoadLruCache(m_cachePath);
	}

	~Implementation()
	{
		try
		{
			SaveLruCache(m_cachePath);
		}
		catch (const std::exception& e)
		{
			TryLogPrefetchCleanupFailure(L"save lru cache", e);
		}

		try
		{
			UnloadPrefetchedFontResources();
		}
		catch (const std::exception& e)
		{
			TryLogPrefetchCleanupFailure(L"unload font resources", e);
		}
	}

	void Load(const std::wstring& path)
	{
		std::lock_guard lg(m_resourceLock);
		if (!m_lru.HasCapacity() || m_lru.Contains(path))
		{
			return;
		}

		const bool addedResource = TrackPrefetchedFontResource(path);
		std::optional<std::wstring> evicted;
		try
		{
			evicted = m_lru.PutNew(std::wstring(path));
		}
		catch (...)
		{
			if (addedResource)
			{
				TryUnloadTrackedFontResource(path, L"rollback font resource");
			}
			throw;
		}

		if (evicted.has_value())
		{
			TryUnloadTrackedFontResource(*evicted, L"evict font resource");
		}
	}

private:
	bool TrackPrefetchedFontResource(const std::wstring& path)
	{
		if (m_loadedFontResources.find(path) != m_loadedFontResources.end())
		{
			return false;
		}

		AddPrefetchedFontResource(path);
		try
		{
			m_loadedFontResources.emplace(path);
			return true;
		}
		catch (...)
		{
			try
			{
				RemovePrefetchedFontResource(path);
			}
			catch (...)
			{
			}
			throw;
		}
	}

	void UnloadTrackedFontResource(const std::wstring& path)
	{
		auto loaded = m_loadedFontResources.find(path);
		if (loaded == m_loadedFontResources.end())
		{
			return;
		}

		RemovePrefetchedFontResource(path);
		m_loadedFontResources.erase(loaded);
	}

	void TryUnloadTrackedFontResource(const std::wstring& path, const wchar_t* operation)
	{
		try
		{
			UnloadTrackedFontResource(path);
		}
		catch (const std::exception& e)
		{
			TryLogPrefetchCleanupFailure(operation, e);
		}
	}

	void UnloadPrefetchedFontResources()
	{
		std::lock_guard lg(m_resourceLock);
		std::vector<std::wstring> snapshot;
		snapshot.reserve(m_loadedFontResources.size());
		for (const auto& path : m_loadedFontResources)
		{
			snapshot.push_back(path);
		}

		for (const auto& path : snapshot)
		{
			TryUnloadTrackedFontResource(path, L"unload font resource");
		}
	}

	static bool HasIgnoreCaseFlag(const std::wstring& flags)
	{
		return flags.find(L'i') != std::wstring::npos;
	}

	static const std::wstring& NormalizeToBaseName(const std::wstring& processName, std::wstring& storage)
	{
		if (processName.empty())
		{
			storage.clear();
			return storage;
		}

		storage = std::filesystem::path(processName).filename().wstring();
		return storage;
	}

	static CompiledRegexRule CompileRegexRule(
		const std::wstring& pattern,
		bool ignoreCase,
		const char* configName)
	{
		if (pattern.empty())
			throw std::runtime_error(std::string(configName) + " must not be empty");

		auto flags = std::regex_constants::ECMAScript | std::regex_constants::optimize;
		if (ignoreCase)
		{
			flags |= std::regex_constants::icase;
		}

		try
		{
			return { pattern, std::wregex(pattern, flags) };
		}
		catch (const std::regex_error& e)
		{
			throw std::runtime_error(
				std::string("invalid regex in ")
				+ configName
				+ ": "
				+ e.what());
		}
	}

	static bool IsCaseInsensitiveItem(const std::wstring& pattern, std::wstring& normalizedPattern)
	{
		if (pattern.rfind(L"i:", 0) == 0)
		{
			normalizedPattern = pattern.substr(2);
			return true;
		}

		normalizedPattern = pattern;
		return false;
	}

	void InitializeMissingFontIgnoreRules(std::vector<std::wstring>&& missingFontIgnore)
	{
		m_missingFontIgnore.reserve(missingFontIgnore.size());
		for (auto& item : missingFontIgnore)
		{
			std::wstring pattern;
			const bool ignoreCase = IsCaseInsensitiveItem(item, pattern);
			m_missingFontIgnore.emplace_back(
				CompileRegexRule(pattern, ignoreCase, "notifications.missing_font_ignore"));
		}
	}

	void InitializeProcessMissingFontIgnoreRules(
		std::vector<ConfigFile::ProcessMissingFontIgnoreElement>&& processMissingFontIgnore)
	{
		m_processMissingFontIgnore.reserve(processMissingFontIgnore.size());
		for (auto& rule : processMissingFontIgnore)
		{
			CompiledProcessRule compiledRule;
			compiledRule.m_processes = std::move(rule.m_processes);
			compiledRule.m_regex.reserve(rule.m_regex.size());
			for (const auto& regex : rule.m_regex)
			{
				compiledRule.m_regex.emplace_back(
					CompileRegexRule(
						regex,
						HasIgnoreCaseFlag(rule.m_flags),
						"notifications.process_missing_font_ignore.regex"));
			}
			m_processMissingFontIgnore.emplace_back(std::move(compiledRule));
		}
	}

	static bool MatchesRegexRule(const CompiledRegexRule& rule, const std::wstring& missingQuery)
	{
		return std::regex_match(missingQuery, rule.m_regex);
	}

	static bool MatchesProcessName(
		const std::vector<std::wstring>& processes,
		const std::wstring& processName)
	{
		if (processName.empty())
			return false;

		std::wstring normalizedProcessName;
		const auto& baseName = NormalizeToBaseName(processName, normalizedProcessName);
		for (const auto& candidate : processes)
		{
			if (CompareStringOrdinal(
				candidate.c_str(),
				-1,
				baseName.c_str(),
				-1,
				TRUE) == CSTR_EQUAL)
			{
				return true;
			}
		}
		return false;
	}

	void LoadLruCache(const std::filesystem::path& path)
	{
		std::ifstream input(path);
		if (!input.is_open())
			return;
		std::string line;
		while (std::getline(input, line))
		{
			if (line.empty())
				continue;
			auto wideLine = Utf8ToWideString(line);
			std::string validationError;
			if (!FontIndexCore::IsValidFontFile(wideLine, validationError))
			{
				EventLog::GetInstance().LogDebugMessage(
					L"prefetch cache skip invalid font: path=%ls error=\"%ls\"",
					wideLine.c_str(),
					Utf8ToWideString(validationError).c_str());
				continue;
			}
			try
			{
				Load(wideLine);
			}
			catch (const std::runtime_error& e)
			{
				EventLog::GetInstance().LogDebugMessage(
					L"prefetch cache skip load failure: path=%ls error=\"%ls\"",
					wideLine.c_str(),
					Utf8ToWideString(e.what()).c_str());
			}
		}
	}

	void SaveLruCache(const std::filesystem::path& path)
	{
		const auto parentPath = path.parent_path();
		if (!parentPath.empty())
		{
			std::error_code ec;
			std::filesystem::create_directories(parentPath, ec);
		}

		std::ofstream output(path, std::ios::out);
		if (!output.is_open())
			return;
		auto snapshot = m_lru.GetVector();
		for (auto iter = snapshot.rbegin(); iter != snapshot.rend(); ++iter)
		{
			auto line = WideToUtf8String(*iter);
			output << line << '\n';
		}
	}

public:
	bool ShouldIgnoreMissingFontNotification(
		const std::wstring& missingQuery,
		const std::wstring& processName) const
	{
		for (const auto& rule : m_missingFontIgnore)
		{
			if (MatchesRegexRule(rule, missingQuery))
			{
				return true;
			}
		}

		for (const auto& rule : m_processMissingFontIgnore)
		{
			if (!MatchesProcessName(rule.m_processes, processName))
				continue;
			for (const auto& regex : rule.m_regex)
			{
				if (MatchesRegexRule(regex, missingQuery))
					return true;
			}
		}

		return false;
	}

	void HandleFeedback(const FontQueryRequest& request) override
	{
		const auto& data = request.feedbackdata();
		for (const auto& item : data.path())
		{
			auto path = Utf8ToWideString(item);
			Load(path);
		}
		if (m_missingFontNotificationsEnabled && !data.missingquery().empty())
		{
			try
			{
				const auto missingFamilyName = Utf8ToWideString(data.missingquery());
				std::wstring processName;
				if (!data.processname().empty())
				{
					processName = Utf8ToWideString(data.processname());
				}
				if (ShouldIgnoreMissingFontNotification(missingFamilyName, processName))
				{
					return;
				}
				ToastNotifier().ShowToastAsync(
					L"Subtitle Font Helper",
					L"未找到字体：" + missingFamilyName);
			}
			catch (...)
			{
			}
		}
	}

	IRpcFeedbackHandler* GetRpcFeedbackHandler()
	{
		return this;
	}
};

sfh::Prefetch::Prefetch(
	IDaemon* daemon,
	size_t prefetchCount,
	const std::wstring& lruPath,
	bool missingFontNotificationsEnabled,
	std::vector<std::wstring> missingFontIgnore,
	std::vector<ConfigFile::ProcessMissingFontIgnoreElement> processMissingFontIgnore)
	: m_impl(std::make_unique<Implementation>(
		daemon,
		prefetchCount,
		lruPath,
		missingFontNotificationsEnabled,
		std::move(missingFontIgnore),
		std::move(processMissingFontIgnore)))
{
}

sfh::Prefetch::~Prefetch() = default;

sfh::IRpcFeedbackHandler* sfh::Prefetch::GetRpcFeedbackHandler()
{
	return m_impl->GetRpcFeedbackHandler();
}
