#include "pch.h"

#include <filesystem>
#include <regex>
#include <stdexcept>

#include "Prefetch.h"
#include "Common.h"
#include "ToastNotifier.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

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

	bool Put(T&& value)
	{
		std::lock_guard lg(m_lock);
		if (m_capacity == 0)
		{
			return false;
		}
		auto findResult = m_hashmap.find(value);
		if (m_hashmap.end() == findResult)
		{
			// not found
			if (m_nodes.size() == m_capacity)
			{
				// full
				auto lastNode = m_end.m_prev;
				m_hashmap.erase(lastNode->m_data);
				m_hashmap.insert({value, lastNode});
				lastNode->m_data = std::move(value);
				AdjustToHead(*lastNode, true);
			}
			else
			{
				m_nodes.emplace_back();
				auto node = &m_nodes.back();
				m_hashmap.insert({value, node});
				node->m_data = std::move(value);
				AdjustToHead(*node, false);
			}
			return true;
		}
		else
		{
			AdjustToHead(*findResult->second, true);
			return false;
		}
	}

	bool Put(const T& value)
	{
		T v = value;
		return Put(std::move(v));
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
		SaveLruCache(m_cachePath);
	}

	void Load(const std::wstring& path)
	{
		if (m_lru.Put(path))
		{
			AddFontResourceExW(path.c_str(), FR_PRIVATE | FR_NOT_ENUM, nullptr);
		}
	}

private:
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
			Load(wideLine);
		}
	}

	void SaveLruCache(const std::filesystem::path& path)
	{
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
