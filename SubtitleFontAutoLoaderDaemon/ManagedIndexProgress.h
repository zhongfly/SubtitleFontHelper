#pragma once

#include "IDaemon.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <vector>

namespace sfh
{
	enum class ManagedIndexBuildStage
	{
		Idle = 0,
		Scanning,
		Hashing,
		Analyzing,
		Writing
	};

	enum class ManagedIndexWorkType
	{
		Build = 0,
		Update
	};

	struct ManagedIndexBuildProgressState
	{
		std::atomic<bool> m_buildInProgress = false;
		std::atomic<ManagedIndexWorkType> m_workType = ManagedIndexWorkType::Build;
		std::atomic<ManagedIndexBuildStage> m_stage = ManagedIndexBuildStage::Idle;
		std::atomic<size_t> m_processedFiles = 0;
		std::atomic<size_t> m_totalFiles = 0;
	};

	struct ManagedIndexBuildProgressSnapshot
	{
		bool m_buildInProgress = false;
		ManagedIndexWorkType m_workType = ManagedIndexWorkType::Build;
		ManagedIndexBuildStage m_stage = ManagedIndexBuildStage::Idle;
		size_t m_processedFiles = 0;
		size_t m_totalFiles = 0;
	};

	struct ManagedIndexTrayProgressSnapshot
	{
		size_t m_activeCount = 0;
		size_t m_buildCount = 0;
		size_t m_updateCount = 0;
		size_t m_processedFiles = 0;
		size_t m_totalFiles = 0;

		bool HasActiveWork() const
		{
			return m_activeCount != 0;
		}

		bool HasProgress() const
		{
			return m_totalFiles != 0;
		}
	};

	inline std::wstring GetManagedIndexDisplayName(const std::filesystem::path& path)
	{
		return path.filename().empty() ? path.wstring() : path.filename().wstring();
	}

	inline ManagedIndexBuildProgressSnapshot CaptureManagedIndexBuildProgressSnapshot(
		const ManagedIndexBuildProgressState& state)
	{
		return
		{
			state.m_buildInProgress.load(std::memory_order_relaxed),
			state.m_workType.load(std::memory_order_relaxed),
			state.m_stage.load(std::memory_order_relaxed),
			state.m_processedFiles.load(std::memory_order_relaxed),
			state.m_totalFiles.load(std::memory_order_relaxed)
		};
	}

	inline ManagedIndexTrayProgressSnapshot CaptureManagedIndexTrayProgressSnapshot(
		const std::vector<std::shared_ptr<ManagedIndexBuildProgressState>>& states)
	{
		ManagedIndexTrayProgressSnapshot snapshot;
		for (const auto& state : states)
		{
			if (!state)
			{
				continue;
			}

			const auto item = CaptureManagedIndexBuildProgressSnapshot(*state);
			if (!item.m_buildInProgress)
			{
				continue;
			}

			++snapshot.m_activeCount;
			if (item.m_workType == ManagedIndexWorkType::Update)
			{
				++snapshot.m_updateCount;
			}
			else
			{
				++snapshot.m_buildCount;
			}

			if (item.m_totalFiles != 0)
			{
				snapshot.m_totalFiles += item.m_totalFiles;
				snapshot.m_processedFiles += (std::min)(item.m_processedFiles, item.m_totalFiles);
			}
		}
		return snapshot;
	}

	class ManagedIndexBuildFeedbackSession
	{
	public:
		ManagedIndexBuildFeedbackSession(
			IDaemon* daemon,
			const std::filesystem::path& indexPath,
			const std::shared_ptr<ManagedIndexBuildProgressState>& progressState,
			ManagedIndexWorkType workType)
			: m_daemon(daemon),
			  m_indexPath(indexPath),
			  m_progressState(progressState)
		{
			if (m_progressState)
			{
				m_progressState->m_buildInProgress.store(true, std::memory_order_relaxed);
				m_progressState->m_workType.store(workType, std::memory_order_relaxed);
				m_progressState->m_stage.store(ManagedIndexBuildStage::Idle, std::memory_order_relaxed);
				m_progressState->m_processedFiles.store(0, std::memory_order_relaxed);
				m_progressState->m_totalFiles.store(0, std::memory_order_relaxed);
			}

			if (m_daemon)
			{
				m_daemon->NotifyManagedIndexBuildStarted(m_indexPath);
			}
		}

		~ManagedIndexBuildFeedbackSession()
		{
			if (m_progressState)
			{
				m_progressState->m_buildInProgress.store(false, std::memory_order_relaxed);
				m_progressState->m_workType.store(ManagedIndexWorkType::Build, std::memory_order_relaxed);
				m_progressState->m_stage.store(ManagedIndexBuildStage::Idle, std::memory_order_relaxed);
				m_progressState->m_processedFiles.store(0, std::memory_order_relaxed);
				m_progressState->m_totalFiles.store(0, std::memory_order_relaxed);
			}

			if (m_daemon)
			{
				m_daemon->NotifyManagedIndexBuildFinished(m_indexPath);
			}
		}

		void SetStage(ManagedIndexBuildStage stage, size_t totalFiles = 0, size_t processedFiles = 0) const
		{
			if (!m_progressState)
			{
				return;
			}

			m_progressState->m_stage.store(stage, std::memory_order_relaxed);
			m_progressState->m_totalFiles.store(totalFiles, std::memory_order_relaxed);
			m_progressState->m_processedFiles.store(processedFiles, std::memory_order_relaxed);
		}

		std::atomic<size_t>* GetProgressCounter() const
		{
			if (!m_progressState)
			{
				return nullptr;
			}
			return &m_progressState->m_processedFiles;
		}

	private:
		IDaemon* m_daemon = nullptr;
		std::filesystem::path m_indexPath;
		std::shared_ptr<ManagedIndexBuildProgressState> m_progressState;
	};
}
