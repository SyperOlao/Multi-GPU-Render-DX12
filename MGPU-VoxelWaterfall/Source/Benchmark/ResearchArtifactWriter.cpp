#include "Source/Benchmark/ResearchArtifactWriter.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <windows.h>
#include <winternl.h>

namespace
{
    std::string EscapeJson(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char ch : value)
        {
            switch (ch)
            {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(ch); break;
            }
        }
        return escaped;
    }

    std::string EscapeCsv(const std::string& value)
    {
        if (value.find_first_of(",\"\n\r") == std::string::npos)
            return value;

        std::string escaped = "\"";
        for (const char ch : value)
            escaped += ch == '"' ? "\"\"" : std::string(1, ch);
        escaped += '"';
        return escaped;
    }

    std::string ToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return {};
        const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                                 static_cast<int>(value.size()),
                                                 nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(std::max(0, required)), '\0');
        if (required > 0)
        {
            WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                result.data(), required, nullptr, nullptr);
        }
        return result;
    }

    std::string UtcTimestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_s(&utc, &time);
        std::ostringstream stream;
        stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return stream.str();
    }

    std::string HashFile(const std::filesystem::path& path, std::string& reason)
    {
        const auto hash = ResearchProvenance::Sha256File(path);
        if (hash.rfind("unknown", 0) == 0)
        {
            reason = hash;
            return "Unknown";
        }
        reason.clear();
        return hash;
    }

    std::string RunCommand(const char* command, std::string& reason)
    {
        FILE* pipe = _popen(command, "r");
        if (!pipe)
        {
            reason = "command could not be started";
            return "Unknown";
        }

        std::array<char, 256> buffer{};
        std::string output;
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
            output += buffer.data();
        const int exitCode = _pclose(pipe);
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
            output.pop_back();

        if (exitCode != 0 || output.empty())
        {
            reason = "command failed or produced no output";
            return "Unknown";
        }

        reason.clear();
        return output;
    }

    void WriteKnownOrUnknown(std::ostream& json,
                             const char* name,
                             const std::string& value,
                             const std::string& reason,
                             const char* suffix = ",")
    {
        json << "  \"" << name << "\":";
        if (value == "Unknown")
        {
            json << "{\"value\":\"Unknown\",\"reason\":\"" << EscapeJson(reason) << "\"}" << suffix << "\n";
        }
        else
        {
            json << "\"" << EscapeJson(value) << "\"" << suffix << "\n";
        }
    }

    std::string PowerPlan()
    {
        std::string reason;
        return RunCommand("powercfg /getactivescheme", reason);
    }

    std::string WindowsVersion()
    {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
            return "Unknown: ntdll.dll is not loaded";
        const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (!rtlGetVersion)
            return "Unknown: RtlGetVersion is unavailable";

        RTL_OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (rtlGetVersion(&version) != 0)
            return "Unknown: RtlGetVersion failed";

        std::ostringstream stream;
        stream << version.dwMajorVersion << '.' << version.dwMinorVersion << "."
               << version.dwBuildNumber;
        return stream.str();
    }

    std::string CpuModel()
    {
        std::string reason;
        return RunCommand("wmic cpu get name /value", reason);
    }

    std::string AdapterDriverIdentity(const std::string& vendorId,
                                      const std::string& deviceId,
                                      const std::string& luid)
    {
        if (vendorId == "0" || deviceId == "0")
            return "Unknown: adapter unavailable";

        std::string reason;
        auto hexId = [](const std::string& decimal)
        {
            std::ostringstream stream;
            stream << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
                   << static_cast<uint32_t>(std::stoul(decimal));
            return stream.str();
        };
        const auto ven = hexId(vendorId);
        const auto dev = hexId(deviceId);
        const std::string command =
            "powershell -NoProfile -ExecutionPolicy Bypass -Command \""
            "$rows=Get-CimInstance Win32_PnPSignedDriver | Where-Object { "
            "$_.HardwareID -match 'VEN_" + ven + "' -and $_.HardwareID -match 'DEV_" + dev + "' }; "
            "$rows | Select-Object -First 1 DeviceName,DriverVersion,DriverProviderName,InfName | ConvertTo-Json -Compress\"";
        const auto scoped = RunCommand(command.c_str(), reason);
        return "luid=" + luid + ";vendor=0x" + ven + ";device=0x" + dev + ";pnp_driver=" +
            (scoped == "Unknown" ? ("Unknown:" + reason) : scoped);
    }

    std::string CompilerVersion()
    {
#ifdef _MSC_FULL_VER
        std::ostringstream stream;
        stream << "MSVC _MSC_FULL_VER=" << _MSC_FULL_VER;
        return stream.str();
#else
        return "Unknown: _MSC_FULL_VER is not defined";
#endif
    }

    std::string CompilerFlags()
    {
#ifdef _DEBUG
        return "Configuration=Debug; Platform=x64; WarningLevel=Level3; SDLCheck=true; ConformanceMode=false; LanguageStandard=stdcpp17; MultiProcessorCompilation=true";
#else
        return "Configuration=Release; Platform=x64; WarningLevel=Level3; FunctionLevelLinking=true; IntrinsicFunctions=true; SDLCheck=true; ConformanceMode=false; LanguageStandard=stdcpp17; MultiProcessorCompilation=true; EnableCOMDATFolding=true; OptimizeReferences=true";
#endif
    }

    std::string ProcessPriority()
    {
        const DWORD priority = GetPriorityClass(GetCurrentProcess());
        std::ostringstream stream;
        stream << priority;
        return stream.str();
    }

    std::string ProcessAffinity()
    {
        DWORD_PTR processMask = 0;
        DWORD_PTR systemMask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
            return "Unknown: GetProcessAffinityMask failed";
        std::ostringstream stream;
        stream << "process=0x" << std::hex << processMask << ";system=0x" << systemMask;
        return stream.str();
    }

    std::string DisplayTopology()
    {
        int count = 0;
        EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR, HDC, LPRECT, LPARAM data) -> BOOL
            {
                ++(*reinterpret_cast<int*>(data));
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&count));
        return std::to_string(count) + " monitor(s)";
    }

    std::string RamBytes()
    {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (!GlobalMemoryStatusEx(&status))
            return "Unknown: GlobalMemoryStatusEx failed";
        return std::to_string(status.ullTotalPhys);
    }

    void WriteFileHashes(std::ostream& json,
                         const std::filesystem::path& directory,
                         const char* jsonName,
                         const std::string& extension,
                         const char* suffix)
    {
        json << "  \"" << jsonName << "\":[";
        bool first = true;
        if (std::filesystem::exists(directory))
        {
            for (const auto& entry : std::filesystem::directory_iterator(directory))
            {
                if (!entry.is_regular_file() || entry.path().extension() != extension)
                    continue;
                std::string reason;
                const auto hash = HashFile(entry.path(), reason);
                json << (first ? "\n" : ",\n")
                     << "    {\"path\":\"" << EscapeJson(entry.path().string()) << "\","
                     << "\"hash\":\"" << EscapeJson(hash) << "\"";
                if (hash == "Unknown")
                    json << ",\"reason\":\"" << EscapeJson(reason) << "\"";
                json << "}";
                first = false;
            }
        }
        json << (first ? "]" : "\n  ]") << suffix << "\n";
    }
}

void ResearchArtifactWriter::WriteSuiteManifest(const BenchmarkResearchArtifactContext& context,
                                                const std::vector<AutomaticBenchmarkConfig>& configs)
{
    std::filesystem::create_directories(context.OutputDirectory);
    std::ostringstream json;
    json << "{\n"
         << "  \"schema\":\"mgpu_voxel_benchmark_manifest.v2\",\n"
         << "  \"suite\":\"" << AutomaticBenchmarkRunner::SuiteName(context.Suite) << "\",\n"
         << "  \"run_id\":\"" << EscapeJson(context.RunId) << "\",\n"
         << "  \"status\":\"" << EscapeJson(context.GateStatus) << "\",\n"
         << "  \"reason\":\"" << EscapeJson(context.GateReason) << "\",\n"
         << "  \"randomization_seed\":" << context.RandomizationSeed << ",\n"
         << "  \"warmup_frames\":" << context.WarmupFrames << ",\n"
         << "  \"measured_frames\":" << context.MeasuredFrames << ",\n"
         << "  \"execution_count\":" << context.ExecutionCount << ",\n"
         << "  \"build_hash\":\"" << EscapeJson(context.CurrentBuildHash) << "\",\n"
         << "  \"shader_set_hash\":\"" << EscapeJson(context.CurrentShaderHash) << "\",\n"
         << "  \"shader_hash\":\"" << EscapeJson(context.CurrentShaderHash) << "\",\n"
         << "  \"validation_run_id\":\"" << EscapeJson(context.ValidationRunId) << "\",\n"
         << "  \"two_gpu_verification_run_id\":\"" << EscapeJson(context.TwoGpuVerificationRunId) << "\",\n"
         << "  \"validation_adapter_pair\":\"" << EscapeJson(context.ValidationAdapterPairIdentity) << "\",\n"
         << "  \"two_gpu_adapter_pair\":\"" << EscapeJson(context.TwoGpuAdapterPairIdentity) << "\",\n"
         << "  \"created_utc\":\"" << EscapeJson(context.CreatedUtc.empty() ? UtcTimestamp() : context.CreatedUtc) << "\",\n"
         << "  \"start_utc\":\"" << EscapeJson(context.StartUtc) << "\",\n"
         << "  \"end_utc\":\"" << EscapeJson(context.EndUtc) << "\",\n"
         << ResearchProvenance::ToJson(context.Provenance, 2) << ",\n"
         << "  \"primary_endpoint\":\"present_to_present_ms\",\n"
         << "  \"secondary_endpoints\":[\"cpu_submission_ms\",\"cpu_total_frame_ms\",\"critical_path_gpu_ms\",\"transfer_ms\"],\n"
         << "  \"statistical_method\":\"independent repetition/run is the experimental unit; frame samples are descriptive only; 95 percent CI uses two-sided Student-t over run means; paired speedup uses explicit pair_id blocks on present_to_present_ms\",\n"
         << "  \"exclusion_rules\":[\"failed visual validation\",\"failed two-GPU verification for requested Multi mode\",\"requested mode differs from actual mode\",\"invalid timestamp calibration\",\"nonzero particle transfer bytes\",\"zero render-output transfer bytes for requested Multi mode\",\"missing matching valid Single/Multi pair\"],\n"
         << "  \"artifacts\":{\n"
         << "    \"environment\":\"environment.json\",\n"
         << "    \"adapter_preflight\":\"two_adapter_preflight.json\",\n"
         << "    \"visual_validation_json\":\"voxel_visual_validation.json\",\n"
         << "    \"visual_validation_csv\":\"voxel_visual_validation.csv\",\n"
         << "    \"runs\":\"runs.csv\",\n"
         << "    \"paired_runs\":\"paired_runs.csv\",\n"
         << "    \"paired_summary\":\"paired_summary.csv\",\n"
         << "    \"invalid_records\":\"invalid_records.csv\",\n";
    if (context.GateStatus != "BLOCKED")
    {
        json << "    \"telemetry\":\"telemetry.csv\",\n";
    }
    json
         << "    \"raw_frames\":\"raw_frames.csv\",\n"
         << "    \"raw_frames_glob\":\"VoxelBenchmark_*.csv\"\n"
         << "  },\n"
         << "  \"evidence_files\":[\n";
    if (context.GateStatus == "BLOCKED")
    {
        json << "    \"invalid_records.csv\"\n";
    }
    else
    {
        json << "    \"environment.json\",\n"
             << "    \"voxel_visual_validation.json\",\n"
             << "    \"voxel_visual_validation.csv\",\n"
             << "    \"runs.csv\",\n"
             << "    \"paired_runs.csv\",\n"
             << "    \"paired_summary.csv\",\n"
             << "    \"raw_frames.csv\",\n"
             << "    \"telemetry.csv\"\n";
    }
    json
         << "  ],\n"
         << "  \"optional_artifacts\":{\n";
    if (context.GateStatus == "BLOCKED")
    {
        json << "    \"telemetry\":{\"status\":\"not_produced\",\"reason\":\"suite gate blocked before benchmark frames were measured\"},\n";
    }
    json
         << "    \"memory_timeline\":{\"status\":\"not_produced\",\"reason\":\"automatic benchmark suites do not run memory soak/rebuild sampling; use --memory-soak-once or --memory-rebuild-stress-once for memory_timeline.csv\"}\n"
         << "  },\n"
         << "  \"configs\":[\n";

    for (size_t i = 0; i < configs.size(); ++i)
    {
        const auto& config = configs[i];
        json << "    {\"config_id\":\"" << EscapeJson(config.ConfigId) << "\","
             << "\"pair_id\":\"" << EscapeJson(config.PairId) << "\","
             << "\"session_id\":\"" << EscapeJson(config.SessionId) << "\","
             << "\"block_id\":\"" << EscapeJson(config.BlockId) << "\","
             << "\"mode\":\"" << config.ModeName << "\","
             << "\"preset\":\"" << config.Preset << "\","
             << "\"total_count\":" << config.TotalCount << ","
             << "\"requested_static_budget_label\":" << config.RequestedLabelCount << ","
             << "\"requested_static_budget\":" << config.RequestedStaticBudget << ","
             << "\"requested_dynamic_budget\":" << config.RequestedDynamicBudget << ","
             << "\"secondary_share\":" << config.SecondaryShare << ","
             << "\"spatial_lod\":\"" << (config.SpatialLodEnabled ? "ThreeLevel" : "Off") << "\","
             << "\"temporal_interval\":" << config.TemporalInterval << ","
             << "\"repetition\":" << config.Repetition << ","
             << "\"order_index\":" << config.OrderIndex << ","
             << "\"block_order_index\":" << config.BlockOrderIndex << ","
             << "\"pair_member_order\":" << config.PairMemberOrder << "}"
             << (i + 1 == configs.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";
    std::string reason;
    if (!ResearchProvenance::WriteTextFileAtomic(context.OutputDirectory / "manifest.json", json.str(), &reason))
        throw std::runtime_error("Failed to write benchmark manifest atomically: " + reason);
}

void ResearchArtifactWriter::WriteEnvironment(const BenchmarkResearchArtifactContext& context,
                                              const VoxelBenchmarkProfiler::FrameMetadata& metadata)
{
    std::filesystem::create_directories(context.OutputDirectory);
    std::ofstream json(context.OutputDirectory / "environment.json", std::ios::out | std::ios::trunc);
    const auto binaryDir = std::filesystem::current_path().parent_path() / "x64" / "Release";
    const auto gitCommit = metadata.GitCommit;
    const auto gitDirty = metadata.GitDirtyState;

    json << "{\n"
         << "  \"schema\":\"mgpu_voxel_environment.v2\",\n"
         << "  \"created_utc\":\"" << UtcTimestamp() << "\",\n"
         << "  \"suite\":\"" << AutomaticBenchmarkRunner::SuiteName(context.Suite) << "\",\n"
         << "  \"run_id\":\"" << EscapeJson(context.RunId) << "\",\n";
    WriteKnownOrUnknown(json, "git_commit", gitCommit.rfind("Unknown", 0) == 0 ? "Unknown" : gitCommit,
                        gitCommit.rfind("Unknown: ", 0) == 0 ? gitCommit.substr(9) : "git command failed");
    json << "  \"git_dirty_state\":\"" << EscapeJson(gitDirty) << "\",\n"
         << "  \"executable_hash\":\"" << EscapeJson(context.CurrentBuildHash) << "\",\n"
         << "  \"shader_set_hash\":\"" << EscapeJson(context.CurrentShaderHash) << "\",\n"
         << "  \"shader_hash\":\"" << EscapeJson(context.CurrentShaderHash) << "\",\n"
         << "  \"compiler_version\":\"" << EscapeJson(CompilerVersion()) << "\",\n"
         << "  \"compiler_flags\":\"" << EscapeJson(CompilerFlags()) << "\",\n"
         << "  \"windows_build\":\"" << EscapeJson(WindowsVersion()) << "\",\n"
         << "  \"wddm_version\":{\"value\":\"Unknown\",\"reason\":\"not queried from DXGI diagnostics in this build\"},\n"
         << "  \"d3d12_runtime\":{\"value\":\"Unknown\",\"reason\":\"no runtime version API is wired\"},\n"
         << "  \"d3d12_sdk_version\":{\"value\":\"Unknown\",\"reason\":\"Agility SDK version macro is not configured for this project\"},\n"
         << "  \"cpu_model\":\"" << EscapeJson(CpuModel()) << "\",\n"
         << "  \"ram_bytes\":\"" << EscapeJson(RamBytes()) << "\",\n"
         << "  \"power_plan\":\"" << EscapeJson(PowerPlan()) << "\",\n"
         << "  \"hags_state\":{\"value\":\"Unknown\",\"reason\":\"not available without graphics registry policy query\"},\n"
         << "  \"vsync_state\":\"" << (metadata.BenchmarkConfigClass == "BenchmarkNeutral" ? "benchmark controlled" : "runtime metadata") << "\",\n"
         << "  \"tearing_state\":{\"value\":\"Unknown\",\"reason\":\"swap-chain tearing support is not exported in metadata\"},\n"
         << "  \"display_topology\":\"" << EscapeJson(DisplayTopology()) << "\",\n"
         << "  \"benchmark_resolution\":\"" << metadata.RenderWidth << "x" << metadata.RenderHeight << "\",\n"
         << "  \"debug_layer_enabled\":" << (metadata.D3D12DebugLayerEnabled ? "true" : "false") << ",\n"
         << "  \"debug_layer_reason\":\"runtime FrameMetadata value; false is not a placeholder\",\n"
         << "  \"gpu_validation_state\":{\"value\":\"Unknown\",\"reason\":\"D3D12 info queue GPU validation setting is not exported\"},\n"
         << "  \"process_priority\":\"" << EscapeJson(ProcessPriority()) << "\",\n"
         << "  \"process_affinity\":\"" << EscapeJson(ProcessAffinity()) << "\",\n"
         << "  \"primary_gpu\":{\"name\":\"" << EscapeJson(ToUtf8(metadata.PrimaryAdapterName)) << "\","
         << "\"vendor_id\":" << metadata.PrimaryVendorId << ",\"device_id\":" << metadata.PrimaryDeviceId
         << ",\"luid\":\"" << EscapeJson(metadata.PrimaryAdapterLuid) << "\","
         << "\"dedicated_video_memory\":" << metadata.PrimaryDedicatedVideoMemory << ","
         << "\"driver_identity\":\"" << EscapeJson(AdapterDriverIdentity(std::to_string(metadata.PrimaryVendorId),
                                                                         std::to_string(metadata.PrimaryDeviceId),
                                                                         metadata.PrimaryAdapterLuid)) << "\"},\n"
         << "  \"secondary_gpu\":{\"name\":\"" << EscapeJson(ToUtf8(metadata.SecondaryAdapterName)) << "\","
         << "\"vendor_id\":" << metadata.SecondaryVendorId << ",\"device_id\":" << metadata.SecondaryDeviceId
         << ",\"luid\":\"" << EscapeJson(metadata.SecondaryAdapterLuid) << "\","
         << "\"dedicated_video_memory\":" << metadata.SecondaryDedicatedVideoMemory << ","
         << "\"driver_identity\":\"" << EscapeJson(AdapterDriverIdentity(std::to_string(metadata.SecondaryVendorId),
                                                                         std::to_string(metadata.SecondaryDeviceId),
                                                                         metadata.SecondaryAdapterLuid)) << "\"},\n";
    WriteFileHashes(json, binaryDir, "dll_hashes", ".dll", ",");
    WriteFileHashes(json, binaryDir, "compiled_shader_bytecode_hashes", ".cso", "");
    json << "}\n";
}

void ResearchArtifactWriter::WriteInvalidRecords(
    const BenchmarkResearchArtifactContext& context,
    const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries)
{
    std::filesystem::create_directories(context.OutputDirectory);
    std::ofstream csv(context.OutputDirectory / "invalid_records.csv", std::ios::out | std::ios::trunc);
    csv.imbue(std::locale::classic());
    csv << "schema,suite,run_id,session_id,pair_id,block_id,requested_mode,actual_mode,repetition,reason\n";
    if (!context.GateReason.empty())
    {
        csv << "mgpu_voxel_invalid_records.v2," << AutomaticBenchmarkRunner::SuiteName(context.Suite)
            << ',' << EscapeCsv(context.RunId) << ",,suite_gate,,,,0,"
            << EscapeCsv(context.GateReason) << '\n';
    }
    for (const auto& summary : summaries)
    {
        if (summary.Valid && summary.SkipReason.empty())
            continue;
        csv << "mgpu_voxel_invalid_records.v2," << AutomaticBenchmarkRunner::SuiteName(context.Suite)
            << ',' << EscapeCsv(context.RunId)
            << ',' << EscapeCsv(summary.SessionId)
            << ',' << EscapeCsv(summary.PairId)
            << ',' << EscapeCsv(summary.BlockId)
            << ',' << EscapeCsv(summary.RequestedMode)
            << ',' << EscapeCsv(summary.ActualMode)
            << ',' << summary.Repetition
            << ',' << EscapeCsv(!summary.SkipReason.empty() ? summary.SkipReason : summary.ValidityReason)
            << '\n';
    }
}

void ResearchArtifactWriter::WriteRunsCsv(
    const BenchmarkResearchArtifactContext& context,
    const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries)
{
    std::filesystem::create_directories(context.OutputDirectory);
    std::ofstream csv(context.OutputDirectory / "runs.csv", std::ios::out | std::ios::trunc);
    csv.imbue(std::locale::classic());
    csv << "schema,suite,run_id,session_id,pair_id,block_id,requested_mode,actual_mode,repetition,valid,reason,"
        << "requested_static_budget_label,requested_static_budget,requested_dynamic_budget,"
        << "actual_total_count,actual_static_count,actual_dynamic_count,resolved_config_hash,"
        << "measured_frame_count,valid_frame_count,invalid_frame_count,"
        << "mean_present_to_present_ms,median_present_to_present_ms,p95_present_to_present_ms,"
        << "p99_present_to_present_ms,stddev_present_to_present_ms,"
        << "mean_cpu_submission_ms,median_cpu_submission_ms,p95_cpu_submission_ms,"
        << "p99_cpu_submission_ms,stddev_cpu_submission_ms,mean_cpu_total_frame_ms,"
        << "critical_path_gpu_ms,gpu_work_sum_ms,"
        << "primary_compute_ms,primary_graphics_ms,secondary_compute_ms,secondary_graphics_ms,"
        << "transfer_ms,composite_ms,total_transfer_bytes,color_transfer_bytes,depth_transfer_bytes,"
        << "particle_transfer_bytes,render_output_transfer_bytes,visual_validation_passed,"
        << "validation_case_id,validation_protocol_hash,validation_config_hash,validation_camera_hash\n";
    for (const auto& row : summaries)
    {
        csv << "mgpu_voxel_runs.v2,"
            << AutomaticBenchmarkRunner::SuiteName(context.Suite) << ','
            << EscapeCsv(context.RunId) << ','
            << EscapeCsv(row.SessionId) << ','
            << EscapeCsv(row.PairId) << ','
            << EscapeCsv(row.BlockId) << ','
            << EscapeCsv(row.RequestedMode) << ','
            << EscapeCsv(row.ActualMode) << ','
            << row.Repetition << ','
            << (row.Valid && row.SkipReason.empty() ? "true" : "false") << ','
            << EscapeCsv(!row.SkipReason.empty() ? row.SkipReason : row.ValidityReason) << ','
            << row.RequestedLabelCount << ','
            << row.RequestedStaticBudget << ','
            << row.RequestedDynamicBudget << ','
            << row.TotalVoxelCount << ','
            << row.ActualStaticVoxelCount << ','
            << row.ActualDynamicVoxelCount << ','
            << EscapeCsv(row.ResolvedConfigHash) << ','
            << row.MeasuredFrameCount << ','
            << row.ValidFrameCount << ','
            << row.InvalidFrameCount << ','
            << row.AveragePresentToPresentMs << ','
            << row.MedianPresentToPresentMs << ','
            << row.P95PresentToPresentMs << ','
            << row.P99PresentToPresentMs << ','
            << row.StdDevPresentToPresentMs << ','
            << row.AverageCpuSubmissionMs << ','
            << row.MedianCpuSubmissionMs << ','
            << row.P95CpuSubmissionMs << ','
            << row.P99CpuSubmissionMs << ','
            << row.StdDevCpuSubmissionMs << ','
            << row.AverageCpuTotalFrameMs << ','
            << row.CriticalPathGpuMs << ','
            << row.GpuWorkSumMs << ','
            << row.PrimaryComputeMs << ','
            << row.PrimaryGraphicsMs << ','
            << row.SecondaryComputeMs << ','
            << row.SecondaryGraphicsMs << ','
            << row.TransferMs << ','
            << row.CompositeMs << ','
            << row.AverageTransferBytes << ','
            << row.AverageColorTransferBytes << ','
            << row.AverageDepthTransferBytes << ','
            << row.AverageParticleTransferBytes << ','
            << row.AverageRenderOutputTransferBytes << ','
            << (row.VisualValidationPassed ? "true" : "false") << ','
            << EscapeCsv(row.VisualValidationCaseId) << ','
            << EscapeCsv(row.VisualValidationProtocolHash) << ','
            << EscapeCsv(row.VisualValidationConfigHash) << ','
            << EscapeCsv(row.VisualValidationCameraHash) << '\n';
    }
}

void ResearchArtifactWriter::WriteRawFramesCsv(
    const BenchmarkResearchArtifactContext& context,
    const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries)
{
    std::filesystem::create_directories(context.OutputDirectory);
    std::ofstream out(context.OutputDirectory / "raw_frames.csv", std::ios::out | std::ios::trunc);
    bool wroteHeader = false;
    for (const auto& summary : summaries)
    {
        if (summary.CsvPath.empty() || !std::filesystem::exists(summary.CsvPath))
            continue;

        std::ifstream in(summary.CsvPath);
        std::string line;
        bool firstLine = true;
        while (std::getline(in, line))
        {
            if (firstLine)
            {
                if (!wroteHeader)
                {
                    out << line << '\n';
                    wroteHeader = true;
                }
                firstLine = false;
                continue;
            }
            out << line << '\n';
        }
    }

    if (!wroteHeader)
    {
        out << "frame_index,suite,run_id,session_id,config_id,pair_id,block_id,repetition,"
            << "randomized_order_index,block_order_index,pair_member_order,randomization_seed,"
            << "frame_valid,invalid_reason\n";
    }
}

void ResearchArtifactWriter::WritePairedRunsCsv(
    const BenchmarkResearchArtifactContext& context,
    const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries)
{
    std::filesystem::create_directories(context.OutputDirectory);
    std::ofstream csv(context.OutputDirectory / "paired_runs.csv", std::ios::out | std::ios::trunc);
    csv.imbue(std::locale::classic());
    csv << "schema,suite,run_id,session_id,pair_id,block_id,repetition,single_mode,multi_mode,"
        << "actual_total_count,actual_static_count,actual_dynamic_count,"
        << "endpoint,single_mean_ms,multi_mean_ms,paired_difference_ms,log_speedup,"
        << "speedup,two_device_nominal_efficiency,valid,reason\n";

    std::map<std::tuple<std::string, std::string, std::string, uint32_t, std::string,
                        uint32_t, uint32_t, uint32_t>,
             const VoxelBenchmarkProfiler::BenchmarkSummary*> singles;
    auto family = [](const std::string& mode)
    {
        return mode.find("Temporal") != std::string::npos ? std::string("Temporal") : std::string("Full");
    };
    auto isSingle = [](const std::string& mode)
    {
        return mode == "SingleGpuFull" || mode == "SingleGpuTemporalDecimation";
    };
    auto isMulti = [](const std::string& mode)
    {
        return mode == "MultiGpuFull" || mode == "MultiGpuTemporalDecimation";
    };

    for (const auto& row : summaries)
    {
        if (!row.Valid || !row.SkipReason.empty() || !isSingle(row.RequestedMode))
            continue;
        singles[{row.SessionId, row.PairId, row.BlockId, row.Repetition, family(row.RequestedMode),
                 row.TotalVoxelCount, row.ActualStaticVoxelCount, row.ActualDynamicVoxelCount}] = &row;
    }

    for (const auto& row : summaries)
    {
        if (!isMulti(row.RequestedMode))
            continue;
        const auto key = std::make_tuple(row.SessionId, row.PairId, row.BlockId,
                                         row.Repetition, family(row.RequestedMode),
                                         row.TotalVoxelCount, row.ActualStaticVoxelCount,
                                         row.ActualDynamicVoxelCount);
        const auto singleIt = singles.find(key);
        const bool valid = row.Valid && row.SkipReason.empty() &&
            singleIt != singles.end() && singleIt->second->AveragePresentToPresentMs > 0.0 &&
            row.AveragePresentToPresentMs > 0.0;
        const auto reason = valid ? "" : "missing valid matching Single/Multi block";
        const double speedup = valid ? singleIt->second->AveragePresentToPresentMs / row.AveragePresentToPresentMs : 0.0;
        csv << "mgpu_voxel_paired_runs.v2," << AutomaticBenchmarkRunner::SuiteName(context.Suite)
            << ',' << EscapeCsv(context.RunId)
            << ',' << EscapeCsv(row.SessionId)
            << ',' << EscapeCsv(row.PairId)
            << ',' << EscapeCsv(row.BlockId)
            << ',' << row.Repetition
            << ',' << (valid ? EscapeCsv(singleIt->second->RequestedMode) : "")
            << ',' << EscapeCsv(row.RequestedMode)
            << ',' << row.TotalVoxelCount
            << ',' << row.ActualStaticVoxelCount
            << ',' << row.ActualDynamicVoxelCount
            << ",present_to_present_ms"
            << ',' << (valid ? singleIt->second->AveragePresentToPresentMs : 0.0)
            << ',' << row.AveragePresentToPresentMs
            << ',' << (valid ? singleIt->second->AveragePresentToPresentMs - row.AveragePresentToPresentMs : 0.0)
            << ',' << (valid ? std::log(speedup) : 0.0)
            << ',' << speedup
            << ',' << (valid ? speedup / 2.0 : 0.0)
            << ',' << (valid ? "true" : "false")
            << ',' << EscapeCsv(reason) << '\n';
    }
}

void ResearchArtifactWriter::WriteTelemetryCsv(
    const BenchmarkResearchArtifactContext& context,
    const std::vector<VoxelBenchmarkProfiler::BenchmarkSummary>& summaries)
{
    std::filesystem::create_directories(context.OutputDirectory);
    std::ofstream csv(context.OutputDirectory / "telemetry.csv", std::ios::out | std::ios::trunc);
    csv.imbue(std::locale::classic());
    csv << "schema,utc,run_id,session_id,pair_id,block_id,repetition,requested_mode,actual_mode,"
        << "valid,reason,measured_frame_count,valid_frame_count,invalid_frame_count,"
        << "actual_total_count,actual_static_count,actual_dynamic_count,"
        << "mean_present_to_present_ms,mean_cpu_submission_ms,mean_cpu_total_frame_ms,"
        << "critical_path_gpu_ms,gpu_work_sum_ms,"
        << "primary_compute_ms,primary_graphics_ms,secondary_compute_ms,secondary_graphics_ms,"
        << "transfer_ms,composite_ms,total_transfer_bytes,color_transfer_bytes,depth_transfer_bytes,"
        << "particle_transfer_bytes,render_output_transfer_bytes,total_executed_fixed_steps,"
        << "total_logical_updated_voxels,visual_validation_passed,"
        << "validation_case_id,validation_protocol_hash,validation_config_hash,validation_camera_hash\n";
    const auto timestamp = UtcTimestamp();
    for (const auto& row : summaries)
    {
        csv << "mgpu_voxel_run_telemetry.v2,"
            << timestamp << ','
            << EscapeCsv(context.RunId) << ','
            << EscapeCsv(row.SessionId) << ','
            << EscapeCsv(row.PairId) << ','
            << EscapeCsv(row.BlockId) << ','
            << row.Repetition << ','
            << EscapeCsv(row.RequestedMode) << ','
            << EscapeCsv(row.ActualMode) << ','
            << (row.Valid && row.SkipReason.empty() ? "true" : "false") << ','
            << EscapeCsv(!row.SkipReason.empty() ? row.SkipReason : row.ValidityReason) << ','
            << row.MeasuredFrameCount << ','
            << row.ValidFrameCount << ','
            << row.InvalidFrameCount << ','
            << row.TotalVoxelCount << ','
            << row.ActualStaticVoxelCount << ','
            << row.ActualDynamicVoxelCount << ','
            << row.AveragePresentToPresentMs << ','
            << row.AverageCpuSubmissionMs << ','
            << row.AverageCpuTotalFrameMs << ','
            << row.CriticalPathGpuMs << ','
            << row.GpuWorkSumMs << ','
            << row.PrimaryComputeMs << ','
            << row.PrimaryGraphicsMs << ','
            << row.SecondaryComputeMs << ','
            << row.SecondaryGraphicsMs << ','
            << row.TransferMs << ','
            << row.CompositeMs << ','
            << row.AverageTransferBytes << ','
            << row.AverageColorTransferBytes << ','
            << row.AverageDepthTransferBytes << ','
            << row.AverageParticleTransferBytes << ','
            << row.AverageRenderOutputTransferBytes << ','
            << row.TotalExecutedFixedSteps << ','
            << row.TotalLogicalUpdatedVoxelCount << ','
            << (row.VisualValidationPassed ? "true" : "false") << ','
            << EscapeCsv(row.VisualValidationCaseId) << ','
            << EscapeCsv(row.VisualValidationProtocolHash) << ','
            << EscapeCsv(row.VisualValidationConfigHash) << ','
            << EscapeCsv(row.VisualValidationCameraHash) << '\n';
    }
}

void ResearchArtifactWriter::WriteMemoryTimelineCsv(const BenchmarkResearchArtifactContext& context)
{
    std::filesystem::create_directories(context.OutputDirectory);
    std::ofstream csv(context.OutputDirectory / "memory_timeline.csv", std::ios::out | std::ios::trunc);
    csv << "schema,utc,run_id,phase,frame_index,process_private_bytes,working_set_bytes,"
        << "committed_virtual_bytes,primary_dedicated_vram_bytes,secondary_dedicated_vram_bytes,"
        << "shared_gpu_memory_bytes,descriptor_ranges,command_allocators,live_resources,"
        << "deferred_releases\n";
}

void ResearchArtifactWriter::WriteReproductionReadme(const BenchmarkResearchArtifactContext& context)
{
    std::filesystem::create_directories(context.OutputDirectory);
    std::ofstream readme(context.OutputDirectory / "README.txt", std::ios::out | std::ios::trunc);
    const auto suiteName = std::string(AutomaticBenchmarkRunner::SuiteName(context.Suite));
    readme << "MGPU Voxel Waterfall benchmark artifacts\n"
           << "schema: mgpu_voxel_reproduction_readme.v1\n"
           << "suite: " << suiteName << "\n"
           << "run_id: " << context.RunId << "\n"
           << "status: " << context.GateStatus << "\n"
           << "reason: " << context.GateReason << "\n\n"
           << "Reproduction command:\n"
           << "MGPU-VoxelWaterfall.exe --benchmark-" << (context.Suite == BenchmarkSuite::Smoke ? "smoke" : "full")
           << " --benchmark-output-dir=\"" << context.OutputDirectory.string() << "\""
           << " --benchmark-seed=" << context.RandomizationSeed
           << " --benchmark-repetitions=<predeclared-n>\n\n"
           << "Offline analysis command:\n"
           << "python MGPU-VoxelWaterfall\\Tools\\analyze_benchmark.py --input \""
           << context.OutputDirectory.string() << "\"\n\n"
           << "Telemetry evidence:\n"
           << "- telemetry.csv contains one per-run row after benchmark frames are measured.\n"
           << "- memory_timeline.csv is not produced by automatic benchmark suites; run --memory-soak or --memory-rebuild-stress for memory snapshots.\n\n"
           << "The workload is a deterministic synthetic voxel waterfall, not a physically correct fluid simulation.\n";
}
