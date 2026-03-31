#include "pch.h"

#include "ToastNotifier.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <wil/com.h>
#include <wil/resource.h>

#pragma comment(lib, "runtimeobject.lib")

namespace sfh
{
	namespace
	{
		std::wstring NormalizePathForComparison(const std::filesystem::path& path)
		{
			std::error_code ec;
			auto normalized = std::filesystem::absolute(path, ec);
			if (ec)
			{
				normalized = path;
			}
			normalized = normalized.lexically_normal();

			auto text = normalized.wstring();
			std::transform(text.begin(), text.end(), text.begin(), towlower);
			return text;
		}

		std::wstring EscapeXml(const std::wstring& value)
		{
			std::wstring result;
			result.reserve(value.size());
			for (const auto ch : value)
			{
				switch (ch)
				{
				case L'&':
					result += L"&amp;";
					break;
				case L'<':
					result += L"&lt;";
					break;
				case L'>':
					result += L"&gt;";
					break;
				case L'"':
					result += L"&quot;";
					break;
				case L'\'':
					result += L"&apos;";
					break;
				default:
					result.push_back(ch);
					break;
				}
			}
			return result;
		}

		std::filesystem::path GetShortcutPath()
		{
			wil::unique_cotaskmem_string programsPath;
			THROW_IF_FAILED(SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_CREATE, nullptr, programsPath.put()));
			return std::filesystem::path(programsPath.get()) / L"SubtitleFontHelper.lnk";
		}

		bool IsExpectedShortcut(IShellLinkW* shellLink, IPropertyStore* propertyStore, const std::filesystem::path& modulePath)
		{
			wchar_t linkPath[MAX_PATH]{};
			if (FAILED(shellLink->GetPath(linkPath, std::size(linkPath), nullptr, SLGP_RAWPATH))
				|| linkPath[0] == L'\0')
			{
				return false;
			}

			PROPVARIANT appIdProp{};
			if (FAILED(propertyStore->GetValue(PKEY_AppUserModel_ID, &appIdProp)))
			{
				return false;
			}
			auto clearAppId = wil::scope_exit([&]()
			{
				PropVariantClear(&appIdProp);
			});

			if (appIdProp.vt != VT_LPWSTR || appIdProp.pwszVal == nullptr)
			{
				return false;
			}

			return NormalizePathForComparison(linkPath) == NormalizePathForComparison(modulePath)
				&& _wcsicmp(appIdProp.pwszVal, ToastNotifier::AUMID) == 0;
		}

		void ConfigureShortcut(IShellLinkW* shellLink, IPropertyStore* propertyStore, const std::filesystem::path& modulePath)
		{
			PROPVARIANT appIdProp{};
			THROW_IF_FAILED(InitPropVariantFromString(ToastNotifier::AUMID, &appIdProp));
			auto clearAppId = wil::scope_exit([&]()
			{
				PropVariantClear(&appIdProp);
			});

			THROW_IF_FAILED(shellLink->SetPath(modulePath.c_str()));
			THROW_IF_FAILED(shellLink->SetArguments(L""));
			THROW_IF_FAILED(shellLink->SetIconLocation(modulePath.c_str(), 0));
			THROW_IF_FAILED(propertyStore->SetValue(PKEY_AppUserModel_ID, appIdProp));
			THROW_IF_FAILED(propertyStore->Commit());
		}

		void InstallShortcut(const std::filesystem::path& shortcutPath, const std::filesystem::path& modulePath)
		{
			wil::com_ptr<IShellLinkW> shellLink;
			THROW_IF_FAILED(CoCreateInstance(
				CLSID_ShellLink,
				nullptr,
				CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(shellLink.put())));

			wil::com_ptr<IPropertyStore> propertyStore;
			THROW_IF_FAILED(shellLink->QueryInterface(IID_PPV_ARGS(propertyStore.put())));
			ConfigureShortcut(shellLink.get(), propertyStore.get(), modulePath);

			wil::com_ptr<IPersistFile> persistFile;
			THROW_IF_FAILED(shellLink->QueryInterface(IID_PPV_ARGS(persistFile.put())));
			THROW_IF_FAILED(persistFile->Save(shortcutPath.c_str(), TRUE));
		}

		bool TryUpdateShortcut(const std::filesystem::path& shortcutPath, const std::filesystem::path& modulePath)
		{
			wil::com_ptr<IShellLinkW> shellLink;
			THROW_IF_FAILED(CoCreateInstance(
				CLSID_ShellLink,
				nullptr,
				CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(shellLink.put())));

			wil::com_ptr<IPersistFile> persistFile;
			THROW_IF_FAILED(shellLink->QueryInterface(IID_PPV_ARGS(persistFile.put())));
			if (FAILED(persistFile->Load(shortcutPath.c_str(), STGM_READWRITE)))
			{
				return false;
			}

			wil::com_ptr<IPropertyStore> propertyStore;
			THROW_IF_FAILED(shellLink->QueryInterface(IID_PPV_ARGS(propertyStore.put())));
			if (IsExpectedShortcut(shellLink.get(), propertyStore.get(), modulePath))
			{
				return true;
			}

			ConfigureShortcut(shellLink.get(), propertyStore.get(), modulePath);
			THROW_IF_FAILED(persistFile->Save(nullptr, TRUE));
			return true;
		}

		void EnsureShortcutInstalled()
		{
			const auto shortcutPath = GetShortcutPath();
			const auto modulePath = std::filesystem::path(wil::GetModuleFileNameW<wil::unique_hlocal_string>().get());

			std::error_code ec;
			if (std::filesystem::exists(shortcutPath, ec) && !ec)
			{
				if (TryUpdateShortcut(shortcutPath, modulePath))
				{
					return;
				}

				ec.clear();
				std::filesystem::remove(shortcutPath, ec);
				if (ec)
				{
					throw std::filesystem::filesystem_error(
						"failed to replace invalid toast shortcut",
						shortcutPath,
						ec);
				}
			}

			InstallShortcut(shortcutPath, modulePath);
		}

		void ShowToastOnStaThread(const std::wstring& title, const std::wstring& message)
		{
			winrt::init_apartment(winrt::apartment_type::single_threaded);
			EnsureShortcutInstalled();

			const std::wstring xml = LR"(<toast><visual><binding template="ToastGeneric"><text>)"
				+ EscapeXml(title)
				+ LR"(</text><text>)"
				+ EscapeXml(message)
				+ LR"(</text></binding></visual></toast>)";

			winrt::Windows::Data::Xml::Dom::XmlDocument doc;
			doc.LoadXml(xml);

			auto notification = winrt::Windows::UI::Notifications::ToastNotification(doc);
			auto notifier = winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(ToastNotifier::AUMID);
			notifier.Show(notification);
		}

		void ShowToastWorker(std::wstring title, std::wstring message, std::exception_ptr* backgroundError)
		{
			try
			{
				ShowToastOnStaThread(title, message);
			}
			catch (...)
			{
				if (backgroundError != nullptr)
				{
					*backgroundError = std::current_exception();
				}
			}
		}
	}

	void ToastNotifier::ShowToast(const std::wstring& title, const std::wstring& message) const
	{
		std::exception_ptr backgroundError;
		std::thread worker([title, message, &backgroundError]()
		{
			ShowToastWorker(title, message, &backgroundError);
		});
		worker.join();
		if (backgroundError)
		{
			std::rethrow_exception(backgroundError);
		}
	}

	void ToastNotifier::ShowToastAsync(std::wstring title, std::wstring message) const
	{
		std::thread(
			[title = std::move(title), message = std::move(message)]() mutable
			{
				ShowToastWorker(std::move(title), std::move(message), nullptr);
			}).detach();
	}

	void ToastNotifier::ShowTestToast() const
	{
		ShowToast(L"Subtitle Font Helper", L"Toast notification test");
	}
}
