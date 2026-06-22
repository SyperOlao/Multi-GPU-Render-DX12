#include "VoxelWaterfallApp.h"
#include <array>
#include <filesystem>
#include <string>

using namespace Common;

namespace
{
    std::filesystem::path GetExecutableDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    void SetVoxelWaterfallWorkingDirectory()
    {
        const auto executableDirectory = GetExecutableDirectory();
        const std::array<std::filesystem::path, 3> candidates =
        {
            std::filesystem::current_path(),
            std::filesystem::current_path() / L"MGPU-VoxelWaterfall",
            executableDirectory / L"..\\.." / L"MGPU-VoxelWaterfall"
        };

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate / L"Shaders"))
            {
                SetCurrentDirectoryW(candidate.c_str());
                return;
            }
        }
    }

    bool HasCommandLineFlag(const char* commandLine, const char* flag)
    {
        if (!commandLine || !flag)
            return false;
        const std::string text(commandLine);
        return text.find(flag) != std::string::npos;
    }

    uint32_t ReadCommandLineUint(const char* commandLine, const char* key, const uint32_t fallback)
    {
        if (!commandLine || !key)
            return fallback;
        const std::string text(commandLine);
        const std::string token(key);
        const auto pos = text.find(token);
        if (pos == std::string::npos)
            return fallback;
        const auto start = pos + token.size();
        const auto end = text.find_first_of(" \t\r\n", start);
        const auto value = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        try
        {
            return static_cast<uint32_t>(std::stoul(value, nullptr, 0));
        }
        catch (...)
        {
            return fallback;
        }
    }

    std::filesystem::path ReadCommandLinePath(const char* commandLine, const char* key)
    {
        if (!commandLine || !key)
            return {};
        const std::string text(commandLine);
        const std::string token(key);
        const auto pos = text.find(token);
        if (pos == std::string::npos)
            return {};
        const auto start = pos + token.size();
        if (start >= text.size())
            return {};
        if (text[start] == '"')
        {
            const auto endQuote = text.find('"', start + 1);
            return text.substr(start + 1, endQuote == std::string::npos ? std::string::npos : endQuote - start - 1);
        }
        const auto end = text.find_first_of(" \t\r\n", start);
        return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

}

int WINAPI WinMain(const HINSTANCE hInstance, HINSTANCE prevInstance,
                   PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        SetVoxelWaterfallWorkingDirectory();

        int result = 0;
        {
            VoxelWaterfallApp theApp(hInstance);
            if (!theApp.Initialize())
                return 0;

            if (HasCommandLineFlag(cmdLine, "--run-validation-once"))
                result = theApp.RunValidationSuiteOnce();
            else if (HasCommandLineFlag(cmdLine, "--verify-two-adapter"))
                result = theApp.RunTwoAdapterVerificationOnce();
            else if (HasCommandLineFlag(cmdLine, "--benchmark-smoke"))
                result = theApp.RunAutomaticBenchmarkSuiteOnce(
                    BenchmarkSuite::Smoke,
                    ReadCommandLineUint(cmdLine, "--benchmark-seed=", 0),
                    ReadCommandLinePath(cmdLine, "--benchmark-output-dir="));
            else if (HasCommandLineFlag(cmdLine, "--benchmark-full"))
                result = theApp.RunAutomaticBenchmarkSuiteOnce(
                    BenchmarkSuite::Full,
                    ReadCommandLineUint(cmdLine, "--benchmark-seed=", 0),
                    ReadCommandLinePath(cmdLine, "--benchmark-output-dir="));
            else if (HasCommandLineFlag(cmdLine, "--profile-sweep"))
                result = theApp.RunProfileSweepOnce(
                    ReadCommandLineUint(cmdLine, "--profile-sweep-seed=",
                                        ReadCommandLineUint(cmdLine, "--benchmark-seed=", 0)),
                    ReadCommandLineUint(cmdLine, "--profile-sweep-warmup-frames=", 30),
                    ReadCommandLineUint(cmdLine, "--profile-sweep-measured-frames=", 120),
                    ReadCommandLineUint(cmdLine, "--profile-sweep-repetitions=", 1),
                    ReadCommandLinePath(cmdLine, "--profile-sweep-output-dir="));
            else if (HasCommandLineFlag(cmdLine, "--memory-soak"))
                result = theApp.RunMemorySoakTestOnce(
                    ReadCommandLineUint(cmdLine, "--memory-duration-seconds=", 600),
                    ReadCommandLinePath(cmdLine, "--memory-output-dir="));
            else if (HasCommandLineFlag(cmdLine, "--memory-rebuild-stress"))
                result = theApp.RunMemoryRebuildStressTestOnce(
                    ReadCommandLineUint(cmdLine, "--memory-rebuild-cycles=", 100),
                    ReadCommandLineUint(cmdLine, "--memory-stable-seconds=", 60),
                    ReadCommandLinePath(cmdLine, "--memory-output-dir="));
            else
                result = theApp.Run();
        }
        ExitProcess(static_cast<UINT>(result));
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}
