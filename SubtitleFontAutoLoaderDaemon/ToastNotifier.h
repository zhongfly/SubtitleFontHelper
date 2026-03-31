#pragma once

#include "pch.h"

namespace sfh
{
	class ToastNotifier
	{
	public:
		static constexpr const wchar_t* AUMID = L"SubtitleFontHelper.SubtitleFontAutoLoaderDaemon";

		void ShowToast(const std::wstring& title, const std::wstring& message) const;
		void ShowToastAsync(std::wstring title, std::wstring message) const;
		void ShowTestToast() const;
	};
}
