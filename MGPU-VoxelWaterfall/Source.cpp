#include "VoxelWaterfallApp.h"
#include "Source/Validation/VoxelVisualValidationRunner.h"
#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
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

    bool IsResearchCommand(const char* commandLine)
    {
        return HasCommandLineFlag(commandLine, "--run-validation-once") ||
            HasCommandLineFlag(commandLine, "--verify-two-adapter") ||
            HasCommandLineFlag(commandLine, "--benchmark-smoke") ||
            HasCommandLineFlag(commandLine, "--benchmark-full") ||
            HasCommandLineFlag(commandLine, "--profile-sweep") ||
            HasCommandLineFlag(commandLine, "--runtime-mutation-stress") ||
            HasCommandLineFlag(commandLine, "--asan-runtime-mutation-scenario") ||
            HasCommandLineFlag(commandLine, "--memory-soak") ||
            HasCommandLineFlag(commandLine, "--memory-rebuild-stress");
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

    std::string NarrowForArtifact(const std::wstring& value)
    {
        std::string text;
        text.reserve(value.size());
        for (const wchar_t ch : value)
            text.push_back(ch >= 0 && ch < 128 ? static_cast<char>(ch) : '?');
        return text;
    }

    void WriteStartupBlockedValidationArtifacts(const std::string& reason,
                                                const std::filesystem::path& requestedOutputDirectory = {})
    {
        VoxelVisualValidationConfig config{};
        config.Snapshot.ValidationRunId = "startup_blocked";
        config.Snapshot.BuildHash = "unknown: application initialization failed before provenance capture";
        config.Snapshot.ShaderHash = "unknown: application initialization failed before provenance capture";
        config.Snapshot.ShaderSetHash = "unknown: application initialization failed before provenance capture";
        config.Cases = VoxelVisualValidationConfig::DefaultCases();
        config.CompletedComparisons.reserve(config.Cases.size());
        for (const auto& validationCase : config.Cases)
        {
            VoxelVisualValidationComparisonInput comparison{};
            comparison.CaseId = validationCase.CaseId;
            comparison.ActualSingleMode = validationCase.SingleMode;
            comparison.ActualMultiMode = VoxelExecutionMode::SingleGpuFull;
            comparison.BlockedReason =
                "application initialization failed before deterministic GPU validation capture: " + reason;
            config.CompletedComparisons.push_back(std::move(comparison));
        }

        try
        {
            VoxelVisualValidationRunner runner;
            runner.RunDeterministicSuite(
                config,
                requestedOutputDirectory.empty() ? GetExecutableDirectory() / "VoxelValidation"
                                                 : requestedOutputDirectory);
        }
        catch (...)
        {
            const auto outputDirectory = requestedOutputDirectory.empty()
                                             ? GetExecutableDirectory() / "VoxelValidation"
                                             : requestedOutputDirectory;
            std::filesystem::create_directories(outputDirectory);
            std::ofstream json(outputDirectory / "voxel_visual_validation.json", std::ios::out | std::ios::trunc);
            json << "{\n"
                 << "  \"aggregate_status\":\"BLOCKED\",\n"
                 << "  \"aggregate_reason\":\"application initialization failed before validation artifact export\"\n"
                 << "}\n";
            std::ofstream csv(outputDirectory / "voxel_visual_validation.csv", std::ios::out | std::ios::trunc);
            csv << "case_id,status,compared_pixel_count,reason\n"
                << "startup,BLOCKED,0,application initialization failed before validation artifact export\n";
        }
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
                return IsResearchCommand(cmdLine) ? 2 : 0;

            if (HasCommandLineFlag(cmdLine, "--run-validation-once"))
                result = theApp.RunValidationSuiteOnce(
                    ReadCommandLinePath(cmdLine, "--validation-output-dir="));
            else if (HasCommandLineFlag(cmdLine, "--verify-two-adapter"))
                result = theApp.RunTwoAdapterVerificationOnce(
                    ReadCommandLinePath(cmdLine, "--two-adapter-output-dir="));
            else if (HasCommandLineFlag(cmdLine, "--benchmark-smoke"))
                result = theApp.RunAutomaticBenchmarkSuiteOnce(
                    BenchmarkSuite::Smoke,
                    ReadCommandLineUint(cmdLine, "--benchmark-seed=", 0),
                    ReadCommandLineUint(cmdLine, "--benchmark-repetitions=", 0),
                    ReadCommandLinePath(cmdLine, "--benchmark-output-dir="));
            else if (HasCommandLineFlag(cmdLine, "--benchmark-full"))
                result = theApp.RunAutomaticBenchmarkSuiteOnce(
                    BenchmarkSuite::Full,
                    ReadCommandLineUint(cmdLine, "--benchmark-seed=", 0),
                    ReadCommandLineUint(cmdLine, "--benchmark-repetitions=", 0),
                    ReadCommandLinePath(cmdLine, "--benchmark-output-dir="));
            else if (HasCommandLineFlag(cmdLine, "--profile-sweep"))
                result = theApp.RunProfileSweepOnce(
                    ReadCommandLineUint(cmdLine, "--profile-sweep-seed=",
                                        ReadCommandLineUint(cmdLine, "--benchmark-seed=", 0)),
                    ReadCommandLineUint(cmdLine, "--profile-sweep-warmup-frames=", 30),
                    ReadCommandLineUint(cmdLine, "--profile-sweep-measured-frames=", 120),
                    ReadCommandLineUint(cmdLine, "--profile-sweep-repetitions=", 1),
                    ReadCommandLinePath(cmdLine, "--profile-sweep-output-dir="));
            else if (HasCommandLineFlag(cmdLine, "--runtime-mutation-stress"))
                result = theApp.RunRuntimeMutationStressTestOnce(
                    ReadCommandLineUint(cmdLine, "--runtime-mutation-frames=", 1000),
                    ReadCommandLinePath(cmdLine, "--runtime-mutation-output-dir="));
            else if (HasCommandLineFlag(cmdLine, "--asan-runtime-mutation-scenario"))
                result = theApp.RunAddressSanitizerRuntimeMutationScenarioOnce(
                    ReadCommandLinePath(cmdLine, "--asan-runtime-output-dir="));
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
        return result;
    }
    catch (DxException& e)
    {
        if (HasCommandLineFlag(cmdLine, "--run-validation-once"))
        {
            WriteStartupBlockedValidationArtifacts(
                NarrowForArtifact(e.ToString()),
                ReadCommandLinePath(cmdLine, "--validation-output-dir="));
            return 3;
        }
        if (IsResearchCommand(cmdLine))
        {
            std::cerr << "Research command failed with DxException: "
                      << NarrowForArtifact(e.ToString()) << std::endl;
            return 2;
        }
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
    catch (const std::exception& e)
    {
        if (HasCommandLineFlag(cmdLine, "--run-validation-once"))
        {
            WriteStartupBlockedValidationArtifacts(
                e.what(),
                ReadCommandLinePath(cmdLine, "--validation-output-dir="));
            return 3;
        }
        if (IsResearchCommand(cmdLine))
        {
            std::cerr << "Research command failed with exception: "
                      << e.what() << std::endl;
            return 2;
        }
        MessageBoxA(nullptr, e.what(), "Unhandled exception", MB_OK);
        return 0;
    }
}
