#pragma once

#include "pch.h"

#include "IDaemon.h"
#include "ManagedIndexProgress.h"
#include "TrayUiData.h"

namespace sfh
{
	class SystemTray
	{
	private:
		class Implementation;
		std::unique_ptr<Implementation> m_impl;
	public:
		SystemTray(IDaemon* daemon, ITrayUiDataProvider* trayUiDataProvider);
		~SystemTray();

		SystemTray(const SystemTray&) = delete;
		SystemTray(SystemTray&&) = delete;

		SystemTray& operator=(const SystemTray&) = delete;
		SystemTray& operator=(SystemTray&&) = delete;

		void Start();
		void SetManagedIndexTrayProgress(const ManagedIndexTrayProgressSnapshot& snapshot);
		void NotifyFinishLoad();
		void NotifyFontUiDataChanged();
	};
}
