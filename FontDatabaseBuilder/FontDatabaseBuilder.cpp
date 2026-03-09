#include "Common.h"
#include "ConsoleHelper.h"
#include "Win32Helper.h"
#include "../FontIndexCore/FontIndexCore.h"

#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#include <fcntl.h>
#include <io.h>

DWORD g_ProcessorCount = []()
{
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	return info.dwNumberOfProcessors;
}();
DWORD g_WorkerCount = g_ProcessorCount / 2;

std::atomic<bool> g_cancelToken{false};

struct ProgramOptions
{
	std::vector<std::wstring> input;
	std::wstring output;
	bool deduplicate = false;
	bool deleteDuplicates = false;
};

BOOL WINAPI ControlHandler(DWORD dwCtrlType)
{
	g_cancelToken = true;
	return TRUE;
}

void FindOptions(int argc, wchar_t** argv, ProgramOptions& options)
{
	for (int i = 1; i < argc; ++i)
	{
		if (argv[i][0] == L'-')
		{
			if (_wcsicmp(argv[i], L"-output") == 0)
			{
				if (i + 1 < argc)
				{
					options.output = argv[i + 1];
					++i;
				}
				else
				{
					throw std::runtime_error("missing argument for option -output");
				}
			}
			else if (_wcsicmp(argv[i], L"-dedup") == 0)
			{
				options.deduplicate = true;
			}
			else if (_wcsicmp(argv[i], L"-delete-duplicates") == 0)
			{
				options.deduplicate = true;
				options.deleteDuplicates = true;
			}
			else if (_wcsicmp(argv[i], L"-worker") == 0)
			{
				if (i + 1 < argc)
				{
					g_WorkerCount = std::stoul(argv[i + 1]);
					if (g_WorkerCount >= 100)
						throw std::runtime_error("worker count must be within 1-99");
					++i;
				}
				else
				{
					throw std::runtime_error("missing argument for option -worker");
				}
			}
			else
			{
				char buffer[64];
				snprintf(buffer, 64, "unknown option at position %d", i);
				throw std::runtime_error(buffer);
			}
		}
		else
		{
			options.input.emplace_back(GetFullPathName(argv[i]).get());
		}
	}
	if (options.input.empty())
	{
		throw std::runtime_error("missing input directory");
	}
}

void MoveFileToRecycleBin(const std::wstring& path)
{
	std::wstring from = path;
	from.push_back(L'\0');
	from.push_back(L'\0');

	SHFILEOPSTRUCTW fileOp{};
	fileOp.wFunc = FO_DELETE;
	fileOp.pFrom = from.c_str();
	fileOp.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
	int result = SHFileOperationW(&fileOp);
	if (result != 0 || fileOp.fAnyOperationsAborted)
		throw std::runtime_error("failed to move duplicate file to recycle bin");
}

void PrintHelp()
{
	std::wcout << SetOutputDefault
		<< "Usage: FontDatabaseBuilder.exe [-output OutputFile] [-dedup] [-delete-duplicates] [-worker WorkerCount] Directory... \n"
		<< "\t-output OutputFile: path to the output\n"
		<< "\t-dedup: enable deduplication of files\n"
		<< "\t-delete-duplicates: deduplicate files and move redundant copies to recycle bin\n"
		<< "\t-worker WorkerCount: set work thread count, default is half of your processor count\n"
		<< "\tDirectory: directories need to build index" << std::endl;
}

size_t CountDuplicateFiles(const FontIndexCore::DeduplicateResult& deduplicateResult)
{
	size_t total = 0;
	for (const auto& duplicateGroup : deduplicateResult.m_duplicateGroups)
	{
		total += duplicateGroup.m_duplicateFiles.size();
	}
	return total;
}

void PrintDuplicateFiles(const FontIndexCore::DeduplicateResult& deduplicateResult)
{
	if (deduplicateResult.m_duplicateGroups.empty())
	{
		std::wcout << SetOutputGreen << L"No duplicate files found." << std::endl << SetOutputDefault;
		return;
	}

	std::wcout << SetOutputYellow << L"Duplicate groups: " << deduplicateResult.m_duplicateGroups.size()
		<< L", duplicate files: " << CountDuplicateFiles(deduplicateResult) << std::endl
		<< SetOutputDefault;

	for (const auto& duplicateGroup : deduplicateResult.m_duplicateGroups)
	{
		std::wcout << SetOutputGreen << L"[Keep] " << SetOutputDefault << duplicateGroup.m_keepFile.c_str() << std::endl;
		for (const auto& duplicatePath : duplicateGroup.m_duplicateFiles)
		{
			std::wcout << L"  " << SetOutputYellow << L"[Duplicate] " << SetOutputDefault << duplicatePath.c_str() <<
				std::endl;
		}
		std::wcout << std::endl;
	}
}

int wmain(int argc, wchar_t* argv[], wchar_t* envp[])
{
	// set control signal handler
	SetConsoleCtrlHandler(ControlHandler, TRUE);
	// prepare utf8 console
	UINT originalCP = GetConsoleCP();
	UINT originalOutputCP = GetConsoleOutputCP();
	auto revertConsoleCP = wil::scope_exit([=]()
	{
		SetConsoleCP(originalCP);
		SetConsoleOutputCP(originalOutputCP);
		std::wcout << SetOutputDefault;
	});
	SetConsoleCP(CP_UTF8);
	SetConsoleOutputCP(CP_UTF8);
	setlocale(LC_ALL, ".UTF8");
	// cin/wcin is broken for wide characters
	// use helper function instead
	// use wcout to print wide string
	// use cout to print utf8 string
	std::wcout.imbue(std::locale(".UTF8"));
	std::wcerr.imbue(std::locale(".UTF8"));

	try
	{
		// validate arguments
		ProgramOptions options;
		try
		{
			FindOptions(argc, argv, options);
		}
		catch (std::exception& e)
		{
			std::cout << SetOutputRed << e.what() << std::endl;
			PrintHelp();
			return 1;
		}

		std::wcout << SetOutputDefault << "Current Directory: \n    " << SetOutputYellow << GetCurrentDirectory().get()
			<< std::endl;

		std::wcout << SetOutputDefault << "Input directory: \n";
		for (auto& i : options.input)
		{
			if (!IsDirectory(i.c_str()))
			{
				std::wcout << "    " << SetOutputRed << i << " is not a directory!" << std::endl;
				return 1;
			}
			std::wcout << "    " << SetOutputYellow << i << std::endl;
		}

		if (options.output.empty())
		{
			std::wcout << SetOutputYellow << "Output file path not specified. Generate path from first input." <<
				std::endl;
			options.output = options.input[0];
			if (options.output.back() != '\\')options.output.push_back('\\');
			options.output += L"FontIndex.xml";
			std::wcout << SetOutputDefault << L"Output path: \n    " << SetOutputYellow << options.output << std::endl;
			while (AskConsoleQuestionBoolean(L"Do you want to change output path?"))
			{
				std::wcout << "Enter output path: ";

				std::wstring userPath = ConsoleReadLine();
				options.output = userPath;
				std::wcout << SetOutputDefault << L"Output path: \n    " << SetOutputYellow << options.output << std::endl;
			}
		}
		else
		{
			std::wcout << SetOutputDefault << L"Output path: \n    " << SetOutputYellow << options.output << std::endl;
		}
		if (options.output.empty())
		{
			std::wcout << SetOutputRed << L"Output path is empty!" << std::endl << SetOutputDefault;
			return 1;
		}
		std::wcout << SetOutputDefault;

		std::wcout << "WORKER_COUNT = " << g_WorkerCount << std::endl;

		std::vector<std::filesystem::path> inputDirectories;
		inputDirectories.reserve(options.input.size());
		for (const auto& directory : options.input)
		{
			inputDirectories.emplace_back(directory);
		}

		auto discoveredFiles = FontIndexCore::EnumerateFontFiles(inputDirectories, []()
		{
			return g_cancelToken.load();
		});

		std::wcout << "Discovered " << discoveredFiles.size() << " files." << std::endl;

		if (discoveredFiles.empty())
		{
			std::wcout << "Nothing to do." << std::endl;
			return 0;
		}

		std::vector<std::filesystem::path> filesToAnalyze;

		if (options.deduplicate)
		{
			std::wcout << "Deduplicate..." << std::endl;
			std::atomic<size_t> progress = 0;
			const size_t total = discoveredFiles.size();
			FontIndexCore::DeduplicateResult deduplicateResult;
			std::thread thr([&]()
			{
				deduplicateResult = FontIndexCore::DeduplicateFiles(
					discoveredFiles,
					g_WorkerCount,
					[]()
					{
						return g_cancelToken.load();
					},
					&progress,
					[](const std::filesystem::path& path, const std::string& errorMessage)
					{
						EraseLineStruct::EraseLine();
						std::wcout << SetOutputRed << L"Error hashing file: " << path.c_str() << L'\n';
						std::cout << "Error description: " << errorMessage << std::endl << SetOutputDefault;
					});
			});
			while (!g_cancelToken)
			{
				EraseLineStruct::EraseLine();
				PrintProgressBar(progress, total, 28);
				if (progress == total)
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (thr.joinable())
				thr.join();
			std::wcout << std::endl;
			ThrowIfCancelled();
			PrintDuplicateFiles(deduplicateResult);
			filesToAnalyze = std::move(deduplicateResult.m_uniqueFiles);
			if (options.deleteDuplicates)
			{
				if (deduplicateResult.m_duplicateGroups.empty())
				{
					std::wcout << "Move duplicates to recycle bin..." << std::endl;
					std::wcout << SetOutputGreen << L"No duplicate files to recycle." << std::endl <<
						SetOutputDefault;
				}
				else
				{
					std::wcout << "Move duplicates to recycle bin..." << std::endl;
					for (const auto& duplicateGroup : deduplicateResult.m_duplicateGroups)
					{
						std::wcout << SetOutputGreen << L"[Keep] " << SetOutputDefault << duplicateGroup.m_keepFile.c_str() <<
							std::endl;
						for (const auto& duplicatePath : duplicateGroup.m_duplicateFiles)
						{
							try
							{
								MoveFileToRecycleBin(duplicatePath.c_str());
								std::wcout << L"  " << SetOutputYellow << L"[Recycled] " << SetOutputDefault <<
									duplicatePath.c_str() << std::endl;
							}
							catch (std::exception& e)
							{
								std::wcout << L"  " << SetOutputRed << L"[Failed] " << duplicatePath.c_str() <<
									std::endl;
								std::cout << e.what() << std::endl << SetOutputDefault;
							}
						}
						std::wcout << std::endl;
					}
				}
			}
			std::wcout << "Discovered " << filesToAnalyze.size() << " files." << std::endl;
		}
		else
		{
			filesToAnalyze.reserve(discoveredFiles.size());
			for (const auto& file : discoveredFiles)
			{
				filesToAnalyze.push_back(file.m_path);
			}
		}

		std::wcout << "Build database..." << std::endl;

		std::mutex logLock;
		sfh::FontDatabase db;
		std::atomic<size_t> progress = 0;
		std::thread buildThread([&]()
		{
			db = FontIndexCore::BuildFontDatabase(
				filesToAnalyze,
				g_WorkerCount,
				[]()
				{
					return g_cancelToken.load();
				},
				&progress,
				[&](const std::filesystem::path& path, const std::string& errorMessage)
				{
					std::lock_guard lg(logLock);
					EraseLineStruct::EraseLine();
					std::wcout << SetOutputRed << L"Error analyzing file: " << path.c_str() << L'\n';
					std::cout << "Error description: " << errorMessage << std::endl << SetOutputDefault;
				});
		});

		while (!g_cancelToken)
		{
			{
				std::lock_guard lg(logLock);
				EraseLineStruct::EraseLine();
				PrintProgressBar(progress.load(), filesToAnalyze.size(), 28);
				if (progress == filesToAnalyze.size())
					break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		if (buildThread.joinable())
		{
			buildThread.join();
		}
		ThrowIfCancelled();
		std::wcout << std::endl;

		std::wcout << "Writing output..." << std::endl;

		sfh::FontDatabase::WriteToFile(options.output, db);

		std::wcout << "Done." << std::endl;
	}
	catch (std::exception& e)
	{
		std::cout << SetOutputRed << e.what() << std::endl << SetOutputDefault;
		return 1;
	}

	return 0;
}
