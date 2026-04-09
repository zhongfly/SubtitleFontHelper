#pragma once

#include <string>

namespace sfh
{
	std::wstring GetDefaultLogFilePath();
	std::wstring GetSharedLogFilePath();
	void SetSharedLogFilePath(const std::wstring& path);
}
