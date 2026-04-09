#include "pch.h"

#include "ManagedIndexFailureNotification.h"

namespace
{
	std::wstring GetFailureStageLabel(sfh::ManagedIndexFailureStage stage)
	{
		switch (stage)
		{
		case sfh::ManagedIndexFailureStage::Analyze:
			return L"解析失败";
		case sfh::ManagedIndexFailureStage::Hash:
			return L"哈希失败";
		case sfh::ManagedIndexFailureStage::Group:
			return L"分组失败";
		default:
			return L"未知失败";
		}
	}

	std::wstring GetFailureSamplePath(const std::filesystem::path& path)
	{
		const auto fileName = path.filename().wstring();
		if (!fileName.empty())
		{
			return fileName;
		}
		return path.wstring();
	}
}

void sfh::ManagedIndexFailureCollector::Record(
	ManagedIndexFailureStage stage,
	const std::filesystem::path& path)
{
	const auto index = static_cast<size_t>(stage);
	std::lock_guard lg(m_lock);
	auto& summary = m_failures[index];
	++summary.m_count;
	if (summary.m_samplePath.empty())
	{
		summary.m_samplePath = GetFailureSamplePath(path);
	}
}

bool sfh::ManagedIndexFailureCollector::HasFailures() const
{
	std::lock_guard lg(m_lock);
	for (const auto& item : m_failures)
	{
		if (item.m_count != 0)
		{
			return true;
		}
	}
	return false;
}

std::wstring sfh::ManagedIndexFailureCollector::BuildToastMessage(
	ManagedIndexWorkType workType,
	const std::wstring& indexName) const
{
	std::array<FailureSummary, 3> failuresCopy;
	{
		std::lock_guard lg(m_lock);
		failuresCopy = m_failures;
	}

	std::wstring message = workType == ManagedIndexWorkType::Update
		? L"索引更新包含已跳过的错误文件："
		: L"索引建立包含已跳过的错误文件：";
	message += indexName;

	for (size_t index = 0; index < failuresCopy.size(); ++index)
	{
		const auto& item = failuresCopy[index];
		if (item.m_count == 0)
		{
			continue;
		}

		message += L"\n";
		message += GetFailureStageLabel(static_cast<sfh::ManagedIndexFailureStage>(index));
		message += L" ";
		message += std::to_wstring(item.m_count);
		message += L" 个";
		if (!item.m_samplePath.empty())
		{
			message += L"（例如 ";
			message += item.m_samplePath;
			message += L"）";
		}
	}

	return message;
}
