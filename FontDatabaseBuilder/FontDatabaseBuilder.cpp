#include "Common.h"
#include "ConsoleHelper.h"
#include "Win32Helper.h"
#include "../FontIndexCore/FontIndexCore.h"

#include <chrono>
#include <fstream>
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#include <fcntl.h>
#include <io.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")

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
	std::wstring perfReport;
	bool deduplicate = false;
	bool deleteDuplicates = false;
};

struct ProcessMemorySnapshot
{
	uint64_t m_workingSetBytes = 0;
	uint64_t m_peakWorkingSetBytes = 0;
	uint64_t m_privateUsageBytes = 0;
};

struct StageTelemetry
{
	uint64_t m_elapsedMs = 0;
	ProcessMemorySnapshot m_memory;
};

struct BuildTelemetryReport
{
	bool m_enabled = false;
	size_t m_workerCount = 0;
	size_t m_discoveredFiles = 0;
	size_t m_filesToAnalyze = 0;
	bool m_deduplicateEnabled = false;
	std::wstring m_outputPath;
	std::wstring m_perfReportPath;
	StageTelemetry m_enumerate;
	StageTelemetry m_deduplicate;
	StageTelemetry m_build;
	StageTelemetry m_write;
	uint64_t m_totalElapsedMs = 0;
	FontIndexCore::BuildFontDatabaseStats m_buildStats;
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
			else if (_wcsicmp(argv[i], L"-perf-report") == 0)
			{
				if (i + 1 < argc)
				{
					options.perfReport = argv[i + 1];
					++i;
				}
				else
				{
					throw std::runtime_error("missing argument for option -perf-report");
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

ProcessMemorySnapshot CaptureProcessMemorySnapshot()
{
	PROCESS_MEMORY_COUNTERS_EX counters{};
	if (GetProcessMemoryInfo(
		GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
		sizeof(counters)))
	{
		return
		{
			static_cast<uint64_t>(counters.WorkingSetSize),
			static_cast<uint64_t>(counters.PeakWorkingSetSize),
			static_cast<uint64_t>(counters.PrivateUsage)
		};
	}

	return {};
}

uint64_t ElapsedMilliseconds(
	const std::chrono::steady_clock::time_point& start,
	const std::chrono::steady_clock::time_point& end)
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

std::string WideToUtf8String(const std::wstring& value)
{
	if (value.empty())
	{
		return {};
	}

	const int length = WideCharToMultiByte(
		CP_UTF8,
		WC_ERR_INVALID_CHARS,
		value.c_str(),
		static_cast<int>(value.size()),
		nullptr,
		0,
		nullptr,
		nullptr);
	THROW_LAST_ERROR_IF(length == 0);

	std::string result;
	result.resize(length);
	const int converted = WideCharToMultiByte(
		CP_UTF8,
		WC_ERR_INVALID_CHARS,
		value.c_str(),
		static_cast<int>(value.size()),
		result.data(),
		length,
		nullptr,
		nullptr);
	THROW_LAST_ERROR_IF(converted == 0);
	return result;
}

void WritePerfReport(const BuildTelemetryReport& telemetry)
{
	if (!telemetry.m_enabled)
	{
		return;
	}

	const std::filesystem::path reportPath(telemetry.m_perfReportPath);
	if (const auto parentPath = reportPath.parent_path(); !parentPath.empty())
	{
		std::filesystem::create_directories(parentPath);
	}

	std::ofstream stream(reportPath, std::ios::binary | std::ios::trunc);
	if (!stream)
	{
		throw std::runtime_error("failed to open perf report output");
	}

	auto writeMemory = [&](const char* prefix, const ProcessMemorySnapshot& memory)
	{
		stream << prefix << "_working_set_bytes=" << memory.m_workingSetBytes << '\n';
		stream << prefix << "_peak_working_set_bytes=" << memory.m_peakWorkingSetBytes << '\n';
		stream << prefix << "_private_usage_bytes=" << memory.m_privateUsageBytes << '\n';
	};

	auto writeStage = [&](const char* prefix, const StageTelemetry& stage)
	{
		stream << prefix << "_elapsed_ms=" << stage.m_elapsedMs << '\n';
		writeMemory(prefix, stage.m_memory);
	};

	stream << "worker_count=" << telemetry.m_workerCount << '\n';
	stream << "deduplicate_enabled=" << (telemetry.m_deduplicateEnabled ? "true" : "false") << '\n';
	stream << "discovered_files=" << telemetry.m_discoveredFiles << '\n';
	stream << "files_to_analyze=" << telemetry.m_filesToAnalyze << '\n';
	stream << "output_path=" << WideToUtf8String(telemetry.m_outputPath) << '\n';

	writeStage("enumerate", telemetry.m_enumerate);
	if (telemetry.m_deduplicateEnabled)
	{
		writeStage("deduplicate", telemetry.m_deduplicate);
	}
	writeStage("build", telemetry.m_build);
	writeStage("write", telemetry.m_write);

	stream << "build_total_elapsed_ms=" << telemetry.m_buildStats.m_totalElapsedMs << '\n';
	stream << "build_analyze_elapsed_ms=" << telemetry.m_buildStats.m_analyzeElapsedMs << '\n';
	stream << "build_fallback_count=" << telemetry.m_buildStats.m_fallbackCount << '\n';
	stream << "font_face_count=" << telemetry.m_buildStats.m_fontFaceCount << '\n';
	stream << "total_elapsed_ms=" << telemetry.m_totalElapsedMs << '\n';
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
		<< "Usage: FontDatabaseBuilder.exe [-output OutputFile] [-perf-report PerfReportFile] [-dedup] [-delete-duplicates] [-worker WorkerCount] Directory... \n"
		<< "\t-output OutputFile: path to the output\n"
		<< "\t-perf-report PerfReportFile: write opt-in stage timing and memory report\n"
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
		const auto totalStart = std::chrono::steady_clock::now();
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

		BuildTelemetryReport telemetry;
		telemetry.m_enabled = !options.perfReport.empty();
		telemetry.m_workerCount = g_WorkerCount;
		telemetry.m_deduplicateEnabled = options.deduplicate;
		telemetry.m_outputPath = options.output;
		telemetry.m_perfReportPath = options.perfReport;

		std::wcout << "WORKER_COUNT = " << g_WorkerCount << std::endl;

		std::vector<std::filesystem::path> inputDirectories;
		inputDirectories.reserve(options.input.size());
		for (const auto& directory : options.input)
		{
			inputDirectories.emplace_back(directory);
		}

		const auto enumerateStart = std::chrono::steady_clock::now();
		auto discoveredFiles = FontIndexCore::EnumerateFontFiles(inputDirectories, []()
		{
			return g_cancelToken.load();
		});
		const auto enumerateEnd = std::chrono::steady_clock::now();
		if (telemetry.m_enabled)
		{
			telemetry.m_discoveredFiles = discoveredFiles.size();
			telemetry.m_enumerate.m_elapsedMs = ElapsedMilliseconds(enumerateStart, enumerateEnd);
			telemetry.m_enumerate.m_memory = CaptureProcessMemorySnapshot();
		}

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
			const auto deduplicateStart = std::chrono::steady_clock::now();
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
			if (telemetry.m_enabled)
			{
				const auto deduplicateEnd = std::chrono::steady_clock::now();
				telemetry.m_deduplicate.m_elapsedMs = ElapsedMilliseconds(deduplicateStart, deduplicateEnd);
				telemetry.m_deduplicate.m_memory = CaptureProcessMemorySnapshot();
			}
		}
		else
		{
			filesToAnalyze.reserve(discoveredFiles.size());
			for (const auto& file : discoveredFiles)
			{
				filesToAnalyze.push_back(file.m_path);
			}
		}
		if (telemetry.m_enabled)
		{
			telemetry.m_filesToAnalyze = filesToAnalyze.size();
		}

		std::wcout << "Build database..." << std::endl;

		std::mutex logLock;
		sfh::FontDatabase db;
		std::atomic<size_t> progress = 0;
		FontIndexCore::BuildFontDatabaseStats buildStats;
		const auto buildStart = std::chrono::steady_clock::now();
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
				},
				telemetry.m_enabled ? &buildStats : nullptr);
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
		const auto buildEnd = std::chrono::steady_clock::now();
		if (telemetry.m_enabled)
		{
			telemetry.m_build.m_elapsedMs = ElapsedMilliseconds(buildStart, buildEnd);
			telemetry.m_build.m_memory = CaptureProcessMemorySnapshot();
			telemetry.m_buildStats = buildStats;
		}

		std::wcout << "Writing output..." << std::endl;

		const auto writeStart = std::chrono::steady_clock::now();
		sfh::FontDatabase::WriteToFile(options.output, db);
		const auto writeEnd = std::chrono::steady_clock::now();
		if (telemetry.m_enabled)
		{
			telemetry.m_write.m_elapsedMs = ElapsedMilliseconds(writeStart, writeEnd);
			telemetry.m_write.m_memory = CaptureProcessMemorySnapshot();
			telemetry.m_totalElapsedMs = ElapsedMilliseconds(totalStart, writeEnd);
			WritePerfReport(telemetry);
		}

		std::wcout << "Done." << std::endl;
		if (telemetry.m_enabled)
		{
			std::wcout << "Perf report written to: " << telemetry.m_perfReportPath << std::endl;
		}
	}
	catch (std::exception& e)
	{
		std::cout << SetOutputRed << e.what() << std::endl << SetOutputDefault;
		return 1;
	}

	return 0;
}
