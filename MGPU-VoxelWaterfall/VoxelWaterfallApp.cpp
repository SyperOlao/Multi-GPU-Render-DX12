#include "VoxelWaterfallApp.h"
#include "ComputePSO.h"
#include "GDescriptorHeap.h"
#include "GCommandList.h"
#include "GCommandQueue.h"
#include "GRootSignature.h"
#include "Source/Assets/SampleAssetManifest.h"
#include "Source/Devices/DeviceSelectionPolicy.h"

#include <array>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <DirectXMath.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <psapi.h>

#include "GameObject.h"
#include "GDeviceFactory.h"
#include "GModel.h"
#include "GResource.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "MathHelper.h"
#include "ModelRenderer.h"
#include "Source/Voxels/VoxelGpuPartition.h"
#include "Source/Voxels/VoxelResearchEnvironmentGenerator.h"
#include "Source/Voxels/VoxelSimulationSchedulerReferenceTests.h"
#include "Source/Voxels/VoxelSpatialLodReferenceTests.h"
#include "Source/Voxels/VoxelWaterfallDynamicReferenceTests.h"
#include "Source/Scene/VoxelResearchCameraController.h"
#include "Transform.h"
#include "Window.h"

#pragma comment(lib, "Psapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr float DebugUiScale = 2.25f;
    constexpr const char* VoxelSceneGpuObjectName = "VoxelSceneGpuPartitions";
    uint64_t gProvenanceFileHashCount = 0;
    uint64_t gGitProcessSpawnCount = 0;

    struct StaticLayerSnapshot
    {
        bool Valid = false;
        uint32_t StaticVoxelBudget = 0;
        StaticVoxelBudgetPreset StaticBudgetPreset = StaticVoxelBudgetPreset::Small;
        StaticVoxelStorageMode StaticStorageMode = StaticVoxelStorageMode::SurfaceOnly;
        uint32_t StaticGenerationSeed = 0;
        float StaticVoxelSize = 0.0f;
        uint32_t ActualStaticVoxelCount = 0;
        VoxelGridCoordinate GridOrigin{};
        DirectX::SimpleMath::Vector3 BoundsMin = DirectX::SimpleMath::Vector3::Zero;
        DirectX::SimpleMath::Vector3 BoundsMax = DirectX::SimpleMath::Vector3::Zero;
    };

    std::string LuidToString(const LUID& luid)
    {
        std::ostringstream stream;
        stream << std::hex << std::uppercase
            << static_cast<uint32_t>(luid.HighPart) << ':'
            << static_cast<uint32_t>(luid.LowPart);
        return stream.str();
    }

    void FillAdapterMetadata(
        const std::shared_ptr<GDevice>& device,
        uint32_t& vendorId,
        uint32_t& deviceId,
        uint64_t& dedicatedMemory,
        std::string& adapterLuid)
    {
        if (!device)
            return;

        const auto& desc = device->GetDesc();
        vendorId = desc.VendorId;
        deviceId = desc.DeviceId;
        dedicatedMemory = static_cast<uint64_t>(desc.DedicatedVideoMemory);
        adapterLuid = LuidToString(desc.AdapterLuid);
    }

    std::filesystem::path GetExecutableDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    std::string FilesystemTimestampToken()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_s(&utc, &time);
        std::ostringstream stream;
        stream << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
        return stream.str();
    }

    bool DirectoryHasFiles(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return false;
        return std::filesystem::directory_iterator(path, ec) != std::filesystem::directory_iterator();
    }

    std::string HashFileOrUnknown(const std::filesystem::path& path)
    {
        ++gProvenanceFileHashCount;
        return ResearchProvenance::Sha256File(path);
    }

    std::string ToGenericUtf8Path(std::filesystem::path path)
    {
        return path.generic_string();
    }

    std::string RuntimeShaderRelativePath(const std::filesystem::path& shaderDirectory,
                                          const std::filesystem::path& shaderPath)
    {
        std::error_code error;
        const auto relative = std::filesystem::relative(shaderPath, shaderDirectory, error);
        const auto path = error ? shaderPath.filename() : relative;
        return "Shaders/" + ToGenericUtf8Path(path);
    }

    std::string HashShaderBytecodeSetOrUnknown(const std::filesystem::path& shaderDirectory)
    {
        if (!std::filesystem::exists(shaderDirectory) || !std::filesystem::is_directory(shaderDirectory))
            return "unknown";

        std::vector<std::filesystem::path> shaderPaths;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(shaderDirectory))
        {
            if (entry.is_regular_file() && entry.path().extension() == L".cso")
                shaderPaths.push_back(entry.path());
        }
        if (shaderPaths.empty())
            return "unknown";

        std::sort(shaderPaths.begin(), shaderPaths.end(),
                  [&shaderDirectory](const auto& lhs, const auto& rhs)
                  {
                      return RuntimeShaderRelativePath(shaderDirectory, lhs) <
                          RuntimeShaderRelativePath(shaderDirectory, rhs);
                  });

        std::ostringstream canonical;
        canonical << "mgpu_voxel_shader_bytecode_set.v1\n";
        for (const auto& shaderPath : shaderPaths)
        {
            const std::string contentHash = HashFileOrUnknown(shaderPath);
            if (contentHash.rfind("unknown", 0) == 0)
                return "unknown";
            const std::string relativePath = RuntimeShaderRelativePath(shaderDirectory, shaderPath);
            canonical << relativePath << "=" << contentHash << "\n";
        }
        return ResearchProvenance::Sha256Hex(canonical.str());
    }

    std::string HResultToHex(const HRESULT hr)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::setw(8) << std::setfill('0')
            << static_cast<uint32_t>(hr);
        return stream.str();
    }

    std::string RunCommandTrimmed(const char* command)
    {
        if (std::strstr(command, "git ") != nullptr)
            ++gGitProcessSpawnCount;
        FILE* pipe = _popen(command, "r");
        if (!pipe)
            return "Unknown: command could not be started";
        std::array<char, 256> buffer{};
        std::string output;
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
            output += buffer.data();
        const int exitCode = _pclose(pipe);
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
            output.pop_back();
        if (exitCode != 0 || output.empty())
            return "Unknown: command failed or produced no output";
        return output;
    }

    std::pair<std::string, std::string> ParseGitStatusPorcelainV2(const std::string& output)
    {
        std::istringstream stream(output);
        std::string line;
        std::string commit = "Unknown: git status did not report branch.oid";
        bool dirty = false;
        while (std::getline(stream, line))
        {
            if (line.rfind("# branch.oid ", 0) == 0)
            {
                commit = line.substr(std::strlen("# branch.oid "));
                continue;
            }
            if (!line.empty() && line[0] != '#')
                dirty = true;
        }
        return {commit, dirty ? "dirty" : "clean"};
    }

    TwoAdapterQueueCalibration CollectQueueCalibration(
        const std::string& name,
        const std::shared_ptr<GCommandQueue>& queue)
    {
        TwoAdapterQueueCalibration record{};
        record.QueueName = name;
        record.HResult = HResultToHex(E_POINTER);
        if (!queue || !queue->GetD3D12CommandQueue())
            return record;

        UINT64 frequency = 0;
        HRESULT hr = queue->GetD3D12CommandQueue()->GetTimestampFrequency(&frequency);
        record.HResult = HResultToHex(hr);
        record.Frequency = frequency;
        if (FAILED(hr) || frequency == 0)
            return record;

        UINT64 gpu0 = 0;
        UINT64 cpu0 = 0;
        hr = queue->GetD3D12CommandQueue()->GetClockCalibration(&gpu0, &cpu0);
        record.HResult = HResultToHex(hr);
        if (FAILED(hr))
            return record;

        UINT64 gpu1 = 0;
        UINT64 cpu1 = 0;
        hr = queue->GetD3D12CommandQueue()->GetClockCalibration(&gpu1, &cpu1);
        record.HResult = HResultToHex(hr);
        record.GpuTimestamp = gpu1;
        record.CpuTimestamp = cpu1;
        record.Valid = SUCCEEDED(hr) && gpu1 >= gpu0 && cpu1 >= cpu0;
        return record;
    }

    void StoreMatrix(const DirectX::SimpleMath::Matrix& matrix, float (&values)[16])
    {
        DirectX::XMFLOAT4X4 stored{};
        DirectX::XMStoreFloat4x4(&stored, matrix);
        std::memcpy(values, &stored, sizeof(stored));
    }

    const VoxelSceneLayer* FindStaticLayer(const VoxelSceneWorkload& workload)
    {
        const auto it = std::find_if(
            workload.Layers.begin(),
            workload.Layers.end(),
            [](const VoxelSceneLayer& layer)
            {
                return layer.LayerType == VoxelSceneLayerType::Static;
            });
        return it == workload.Layers.end() ? nullptr : &(*it);
    }

    StaticLayerSnapshot CaptureStaticLayerSnapshot(const VoxelSceneWorkload& workload)
    {
        StaticLayerSnapshot snapshot{};
        const auto* staticLayer = FindStaticLayer(workload);
        if (!staticLayer)
            return snapshot;

        snapshot.Valid = true;
        snapshot.StaticVoxelBudget = workload.StaticTelemetry.RequestedVoxelBudget > 0
                                          ? workload.StaticTelemetry.RequestedVoxelBudget
                                          : workload.StaticVoxelBudget;
        snapshot.StaticBudgetPreset = workload.StaticTelemetry.BudgetPreset;
        snapshot.StaticStorageMode = workload.StaticTelemetry.StorageMode;
        snapshot.StaticGenerationSeed = workload.StaticTelemetry.GenerationSeed;
        snapshot.StaticVoxelSize = staticLayer->RenderSettings.VoxelSize;
        snapshot.ActualStaticVoxelCount = staticLayer->LogicalVoxelCount();
        snapshot.GridOrigin = staticLayer->GridOrigin;
        snapshot.BoundsMin = staticLayer->BoundsMin;
        snapshot.BoundsMax = staticLayer->BoundsMax;
        return snapshot;
    }

    bool StaticGenerationInputsMatch(
        const StaticLayerSnapshot& snapshot,
        const VoxelSceneWorkload& workload)
    {
        return snapshot.Valid &&
            snapshot.StaticVoxelBudget == workload.StaticVoxelBudget &&
            snapshot.StaticBudgetPreset == workload.StaticBudgetPreset &&
            snapshot.StaticStorageMode == workload.StaticStorageMode &&
            snapshot.StaticGenerationSeed == workload.StaticGenerationSeed &&
            snapshot.StaticVoxelSize == workload.StaticVoxelSize;
    }

    bool SameGridOrigin(const VoxelGridCoordinate& lhs, const VoxelGridCoordinate& rhs)
    {
        return lhs.X == rhs.X && lhs.Y == rhs.Y && lhs.Z == rhs.Z;
    }

    bool SameVector(const DirectX::SimpleMath::Vector3& lhs, const DirectX::SimpleMath::Vector3& rhs)
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }

    bool SlowVoxelValidationEnabled()
    {
        wchar_t value[8] = {};
        const DWORD length = GetEnvironmentVariableW(L"VOXEL_ENABLE_SLOW_VALIDATION", value,
                                                     static_cast<DWORD>(std::size(value)));
        return length > 0 && length < std::size(value) && value[0] == L'1';
    }

    void AssertStaticLayerShapePreserved(
        const StaticLayerSnapshot& before,
        const VoxelSceneWorkload& after)
    {
        if (!StaticGenerationInputsMatch(before, after))
            return;

        const auto* staticLayer = FindStaticLayer(after);
        assert(staticLayer && "Static layer must survive settings changes when static generation inputs are unchanged");
        assert(staticLayer->RenderSettings.VoxelSize == before.StaticVoxelSize);
        assert(staticLayer->LogicalVoxelCount() == before.ActualStaticVoxelCount);
        assert(SameGridOrigin(staticLayer->GridOrigin, before.GridOrigin));
        assert(SameVector(staticLayer->BoundsMin, before.BoundsMin));
        assert(SameVector(staticLayer->BoundsMax, before.BoundsMax));
    }

    std::filesystem::path ResolveVoxelWaterfallAssetPath(const std::filesystem::path& relativePath)
    {
        const std::array<std::filesystem::path, 4> candidates =
        {
            relativePath,
            std::filesystem::path(L"MGPU-VoxelWaterfall") / relativePath,
            GetExecutableDirectory() / relativePath,
            GetExecutableDirectory() / L"..\\.." / L"MGPU-VoxelWaterfall" / relativePath
        };

        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
        }

        return relativePath;
    }

    bool UsesMultiGpuExecution(const VoxelExecutionMode mode)
    {
        return mode == VoxelExecutionMode::MultiGpuFull ||
            mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    }

    bool UsesTemporalExecution(const VoxelExecutionMode mode)
    {
        return mode == VoxelExecutionMode::SingleGpuTemporalDecimation ||
            mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    }

    const char* ExecutionModeNameLiteral(const VoxelExecutionMode mode)
    {
        switch (mode)
        {
        case VoxelExecutionMode::SingleGpuFull:
            return "SingleGpuFull";
        case VoxelExecutionMode::MultiGpuFull:
            return "MultiGpuFull";
        case VoxelExecutionMode::SingleGpuTemporalDecimation:
            return "SingleGpuTemporalDecimation";
        case VoxelExecutionMode::MultiGpuTemporalDecimation:
            return "MultiGpuTemporalDecimation";
        default:
            return "Unknown";
        }
    }

    const char* ProfileName(const VoxelResearchWorkloadProfile profile)
    {
        switch (profile)
        {
        case VoxelResearchWorkloadProfile::StaticRenderOnly:
            return "StaticRenderOnly";
        case VoxelResearchWorkloadProfile::DynamicSimulationAndRender:
            return "DynamicSimulationAndRender";
        case VoxelResearchWorkloadProfile::MixedStaticAndDynamic:
            return "MixedStaticAndDynamic";
        case VoxelResearchWorkloadProfile::OcclusionValidation:
            return "OcclusionValidation";
        case VoxelResearchWorkloadProfile::SpatialLodDemonstration:
            return "SpatialLodDemonstration";
        case VoxelResearchWorkloadProfile::DemoMixed:
            return "DemoMixed";
        default:
            return "Unknown";
        }
    }

    const char* AdapterOwnerName(const VoxelAdapterOwner owner)
    {
        switch (owner)
        {
        case VoxelAdapterOwner::Primary:
            return "Primary";
        case VoxelAdapterOwner::Secondary:
            return "Secondary";
        default:
            return "Unknown";
        }
    }

    const char* PartitionIdName(const VoxelAdapterPartitionId partitionId)
    {
        switch (partitionId)
        {
        case VoxelAdapterPartitionId::PrimaryPartition:
            return "PrimaryPartition";
        case VoxelAdapterPartitionId::SecondaryPartition:
            return "SecondaryPartition";
        default:
            return "UnknownPartition";
        }
    }

    VoxelResearchScenePreset ScenePresetForProfile(const VoxelResearchWorkloadProfile profile)
    {
        switch (profile)
        {
        case VoxelResearchWorkloadProfile::StaticRenderOnly:
            return VoxelResearchScenePreset::StaticVoxelEnvironment;
        case VoxelResearchWorkloadProfile::DynamicSimulationAndRender:
            return VoxelResearchScenePreset::DynamicWaterfall;
        case VoxelResearchWorkloadProfile::MixedStaticAndDynamic:
            return VoxelResearchScenePreset::MixedVoxelEnvironment;
        case VoxelResearchWorkloadProfile::OcclusionValidation:
            return VoxelResearchScenePreset::OcclusionValidation;
        case VoxelResearchWorkloadProfile::SpatialLodDemonstration:
            return VoxelResearchScenePreset::SpatialLodDemonstration;
        case VoxelResearchWorkloadProfile::DemoMixed:
            return VoxelResearchScenePreset::DemoMixed;
        default:
            return VoxelResearchScenePreset::MixedVoxelEnvironment;
        }
    }

    const char* ResearchRunnerSuiteName(const ResearchRunnerSuite suite)
    {
        switch (suite)
        {
        case ResearchRunnerSuite::Validation:
            return "Validation";
        case ResearchRunnerSuite::TwoGpuVerify:
            return "TwoGpuVerify";
        case ResearchRunnerSuite::Smoke:
            return "Smoke";
        case ResearchRunnerSuite::Full:
            return "Full";
        case ResearchRunnerSuite::ProfileSweep:
            return "ProfileSweep";
        case ResearchRunnerSuite::MemorySoak:
            return "MemorySoak";
        case ResearchRunnerSuite::RebuildStress:
            return "RebuildStress";
        default:
            return "Unknown";
        }
    }

    const char* PartitionStrategyName(const VoxelPartitionStrategy strategy)
    {
        switch (strategy)
        {
        case VoxelPartitionStrategy::HashedChunks:
            return "HashedChunks";
        case VoxelPartitionStrategy::SpatialPlane:
            return "SpatialPlane";
        default:
            return "Unknown";
        }
    }

    const char* LoadBalanceScenarioName(const VoxelLoadBalanceScenario scenario)
    {
        switch (scenario)
        {
        case VoxelLoadBalanceScenario::Balanced:
            return "Balanced";
        case VoxelLoadBalanceScenario::PrimaryHeavy:
            return "PrimaryHeavy";
        case VoxelLoadBalanceScenario::SecondaryHeavy:
            return "SecondaryHeavy";
        default:
            return "Unknown";
        }
    }

    const char* TemporalPolicyName(const VoxelTemporalPolicy policy)
    {
        return policy == VoxelTemporalPolicy::Decimated ? "Decimated" : "Full";
    }

    const char* CameraModeName(const VoxelResearchCameraMode mode)
    {
        switch (mode)
        {
        case VoxelResearchCameraMode::Interactive:
            return "Interactive";
        case VoxelResearchCameraMode::FixedOverview:
            return "FixedOverview";
        case VoxelResearchCameraMode::FixedOcclusion:
            return "FixedOcclusion";
        case VoxelResearchCameraMode::WaterfallCloseup:
            return "WaterfallCloseup";
        case VoxelResearchCameraMode::LodSweepRoute:
            return "LodSweepRoute";
        case VoxelResearchCameraMode::BenchmarkRoute:
            return "BenchmarkRoute";
        case VoxelResearchCameraMode::DemoMixedOverview:
            return "DemoMixedOverview";
        default:
            return "Unknown";
        }
    }

    const char* LightingPresetName(const VoxelResearchLightingPreset preset)
    {
        switch (preset)
        {
        case VoxelResearchLightingPreset::BenchmarkNeutral:
            return "BenchmarkNeutral";
        case VoxelResearchLightingPreset::DemoStaticSky:
            return "DemoStaticSky";
        case VoxelResearchLightingPreset::OcclusionValidationLighting:
            return "OcclusionValidationLighting";
        default:
            return "Unknown";
        }
    }

    std::array<float, 4> BackgroundColorForLightingPreset(const VoxelResearchLightingPreset preset)
    {
        switch (preset)
        {
        case VoxelResearchLightingPreset::DemoStaticSky:
            return {0.13f, 0.18f, 0.24f, 1.0f};
        case VoxelResearchLightingPreset::OcclusionValidationLighting:
            return {0.055f, 0.060f, 0.070f, 1.0f};
        case VoxelResearchLightingPreset::BenchmarkNeutral:
        default:
            return {0.03f, 0.035f, 0.04f, 1.0f};
        }
    }

    std::pair<uint32_t, uint32_t> ResolutionForPreset(const VoxelRenderResolutionPreset preset)
    {
        switch (preset)
        {
        case VoxelRenderResolutionPreset::R1280x720:
            return {1280, 720};
        case VoxelRenderResolutionPreset::R2560x1440:
            return {2560, 1440};
        case VoxelRenderResolutionPreset::R3840x2160:
            return {3840, 2160};
        case VoxelRenderResolutionPreset::R1920x1080:
        default:
            return {1920, 1080};
        }
    }

    const char* ResolutionPresetName(const VoxelRenderResolutionPreset preset)
    {
        switch (preset)
        {
        case VoxelRenderResolutionPreset::R1280x720:
            return "1280x720";
        case VoxelRenderResolutionPreset::R1920x1080:
            return "1920x1080";
        case VoxelRenderResolutionPreset::R2560x1440:
            return "2560x1440";
        case VoxelRenderResolutionPreset::R3840x2160:
            return "3840x2160";
        default:
            return "Unknown";
        }
    }

    struct ValidationCapturedPath
    {
        bool Available = false;
        std::string BlockedReason;
        VoxelExecutionMode ActualMode = VoxelExecutionMode::SingleGpuFull;
        uint32_t FrameIndex = 0;
        PEPEngine::Graphics::GTexture Color;
        PEPEngine::Graphics::GTexture HardwareDepth;
        PEPEngine::Graphics::GTexture SecondaryColor;
        PEPEngine::Graphics::GTexture SecondaryLinearDepth;
        PEPEngine::Graphics::GTexture LinearDepth;
        bool UseSecondaryDepth = false;
    };

    struct ValidationDepthConstants
    {
        uint32_t RenderSize[2] = {};
        float NearZ = 0.25f;
        float FarZ = 900.0f;
        float DepthEpsilon = 0.02f;
        uint32_t UseSecondary = 0;
        float SecondaryInvalidDepth = FLT_MAX;
        float Padding0 = 0.0f;
    };
    static_assert(sizeof(ValidationDepthConstants) == 32);

    struct ValidationCompareConstants
    {
        uint32_t RenderSize[2] = {};
        float ColorTolerance = 0.025f;
        float DepthTolerance = 0.02f;
        float ValidDepthMax = 1000000.0f;
        float Padding1 = 0.0f;
    };
    static_assert(sizeof(ValidationCompareConstants) == 24);

    std::string SanitizeValidationToken(std::string value)
    {
        for (auto& ch : value)
        {
            if (ch == ':' || ch == '/' || ch == '\\' || ch == '?' || ch == '*' ||
                ch == '"' || ch == '<' || ch == '>' || ch == '|')
            {
                ch = '_';
            }
        }
        return value;
    }

    uint32_t BytesPerPixel(const DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return 4;
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
            return 4;
        default:
            return 0;
        }
    }

    std::string FormatName(const DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        default: return "UNKNOWN";
        }
    }

    std::string NarrowForLog(const std::wstring& value)
    {
        std::string text;
        text.reserve(value.size());
        for (const wchar_t ch : value)
            text.push_back(ch >= 0 && ch < 128 ? static_cast<char>(ch) : '?');
        return text;
    }

    std::string DxFailureReason(const char* prefix, const DxException& ex)
    {
        return std::string(prefix) + ": " + HResultToHex(ex.ErrorCode) + " " + NarrowForLog(ex.ToString());
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC Texture2DSrvDesc(const DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Format = format;
        desc.Texture2D.MostDetailedMip = 0;
        desc.Texture2D.MipLevels = 1;
        return desc;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC Texture2DUavDesc(const DXGI_FORMAT format)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Format = format;
        return desc;
    }

    PEPEngine::Graphics::GTexture CreateValidationTexture(
        const std::shared_ptr<PEPEngine::Graphics::GDevice>& device,
        const uint32_t width,
        const uint32_t height,
        const DXGI_FORMAT format,
        const D3D12_RESOURCE_FLAGS flags,
        const std::wstring& name)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = std::max(1u, width);
        desc.Height = std::max(1u, height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;
        return PEPEngine::Graphics::GTexture(device, desc, name, PEPEngine::Graphics::TextureUsage::Albedo);
    }

    std::filesystem::path RawPath(
        const std::filesystem::path& outputDirectory,
        const std::string& caseId,
        const char* suffix)
    {
        return outputDirectory / "references" /
            (SanitizeValidationToken(caseId) + "_" + suffix + ".raw");
    }

    bool ExportTextureRaw(
        const std::shared_ptr<PEPEngine::Graphics::GDevice>& device,
        const std::shared_ptr<PEPEngine::Graphics::GCommandQueue>& queue,
        PEPEngine::Graphics::GTexture& texture,
        const std::filesystem::path& path,
        std::string& reason)
    {
        const auto resource = texture.GetD3D12Resource();
        if (!device || !queue || !resource)
        {
            reason = "reference texture export failed: missing D3D12 resource or queue";
            return false;
        }

        const auto desc = resource->GetDesc();
        const uint32_t bytesPerPixel = BytesPerPixel(desc.Format);
        if (bytesPerPixel == 0)
        {
            reason = "reference texture export failed: unsupported texture format " +
                std::to_string(static_cast<int>(desc.Format));
            return false;
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT numRows = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 totalBytes = 0;
        device->GetDXDevice()->GetCopyableFootprints(
            &desc, 0, 1, 0, &layout, &numRows, &rowSizeInBytes, &totalBytes);

        PEPEngine::Graphics::GResource readback(
            device,
            CD3DX12_RESOURCE_DESC::Buffer(totalBytes),
            L"VoxelValidationTextureReadback",
            nullptr,
            D3D12_RESOURCE_STATE_COPY_DEST,
            CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK));

        const auto cmdList = queue->GetCommandList();
        cmdList->TransitionBarrier(texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmdList->FlushResourceBarriers();

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.GetD3D12Resource().Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = layout;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cmdList->GetGraphicsCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        const auto fence = queue->ExecuteCommandList(cmdList);
        queue->WaitForFenceValue(fence);

        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            reason = "reference texture export failed: could not open " + path.string();
            return false;
        }

        void* mapped = nullptr;
        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(totalBytes)};
        HRESULT hr = readback.GetD3D12Resource()->Map(0, &readRange, &mapped);
        if (FAILED(hr) || !mapped)
        {
            reason = "reference texture export failed: readback Map failed with " + HResultToHex(hr);
            return false;
        }

        const std::string header =
            "MGPU_VOXEL_TEXTURE_RAW_V1\nwidth " + std::to_string(desc.Width) +
            "\nheight " + std::to_string(desc.Height) +
            "\nformat " + FormatName(desc.Format) +
            "\nbytes_per_pixel " + std::to_string(bytesPerPixel) +
            "\nlayout tight_rows\n\n";
        file.write(header.data(), static_cast<std::streamsize>(header.size()));

        const auto* base = static_cast<const uint8_t*>(mapped) + layout.Offset;
        const size_t tightRowBytes = static_cast<size_t>(desc.Width) * bytesPerPixel;
        for (UINT row = 0; row < numRows; ++row)
        {
            file.write(reinterpret_cast<const char*>(base + row * layout.Footprint.RowPitch),
                       static_cast<std::streamsize>(tightRowBytes));
        }

        const D3D12_RANGE emptyRange{0, 0};
        readback.GetD3D12Resource()->Unmap(0, &emptyRange);
        return true;
    }

    std::string BuildValidationConfigHash(const VoxelVisualValidationCase& validationCase,
                                          const VoxelVisualValidationSnapshot& snapshot)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(8)
               << "{\"actual_dynamic_count\":" << validationCase.ActualDynamicCount
               << ",\"actual_static_count\":" << validationCase.ActualStaticCount
               << ",\"actual_total_count\":" << validationCase.ActualTotalCount
               << ",\"candidate_mode\":\"" << ExecutionModeNameLiteral(validationCase.CandidateMode)
               << "\",\"checkpoint_id\":\"" << validationCase.CheckpointId
               << "\",\"kind\":\"" << validationCase.ValidationKind
               << "\",\"lod_enabled\":" << (validationCase.SpatialLodEnabled ? "true" : "false")
               << ",\"reference_mode\":\"" << ExecutionModeNameLiteral(validationCase.ReferenceMode)
               << "\",\"render_height\":" << validationCase.RenderHeight
               << ",\"render_width\":" << validationCase.RenderWidth
               << ",\"requested_dynamic_budget\":" << validationCase.RequestedDynamicBudget
               << ",\"requested_label_count\":" << validationCase.RequestedLabelCount
               << ",\"requested_static_budget\":" << validationCase.RequestedStaticBudget
               << ",\"sample_count\":" << validationCase.SampleCount
               << ",\"secondary_share\":" << validationCase.SecondaryShare
               << ",\"temporal_interval\":" << validationCase.TemporalInterval
               << ",\"total_voxels\":" << snapshot.TotalVoxelCount << "}";
        return ResearchProvenance::Sha256Hex(stream.str());
    }

    std::string BuildValidationCameraHash(const VoxelVisualValidationSnapshot& snapshot)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(8)
               << "{\"far_z\":" << snapshot.FarZ
               << ",\"near_z\":" << snapshot.NearZ
               << ",\"projection\":[";
        for (uint32_t i = 0; i < 16; ++i)
            stream << (i == 0 ? "" : ",") << snapshot.Projection[i];
        stream << "],\"view\":[";
        for (uint32_t i = 0; i < 16; ++i)
            stream << (i == 0 ? "" : ",") << snapshot.View[i];
        stream << "]}";
        return ResearchProvenance::Sha256Hex(stream.str());
    }

    std::string BuildValidationProtocolHash(const VoxelVisualValidationConfig& config)
    {
        return VoxelVisualValidationRunner::BuildProtocolHash(config);
    }

    std::string BuildValidationCaseSetHash(const std::vector<VoxelVisualValidationCase>& cases,
                                           const VoxelVisualValidationSnapshot& snapshot)
    {
        std::ostringstream canonical;
        canonical.imbue(std::locale::classic());
        canonical << "case_set=mgpu_voxel_visual_validation_cases.v2\n"
                  << "generator=" << snapshot.GeneratorVersion << "\n"
                  << "protocol=" << snapshot.ProtocolVersion << "\n";
        for (const auto& validationCase : cases)
        {
            canonical << validationCase.CaseId << ":"
                      << validationCase.ValidationKind << ":"
                      << validationCase.ConfigKey << ":"
                      << validationCase.CheckpointId << ":"
                      << validationCase.ReferenceConfigHash << ":"
                      << validationCase.CandidateConfigHash << ":"
                      << validationCase.ConfigHash << ":"
                      << validationCase.CameraHash << "\n";
        }
        return ResearchProvenance::Sha256Hex(canonical.str());
    }

    bool IsTemporalMode(const VoxelExecutionMode mode)
    {
        return mode == VoxelExecutionMode::SingleGpuTemporalDecimation ||
            mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    }

    std::string ValidationModeFamily(const VoxelExecutionMode mode)
    {
        return IsTemporalMode(mode) ? "Temporal" : "Full";
    }

    VoxelExecutionMode SingleModeForFamily(const VoxelExecutionMode mode)
    {
        return IsTemporalMode(mode)
                   ? VoxelExecutionMode::SingleGpuTemporalDecimation
                   : VoxelExecutionMode::SingleGpuFull;
    }

    VoxelExecutionMode MultiModeForFamily(const VoxelExecutionMode mode)
    {
        return IsTemporalMode(mode)
                   ? VoxelExecutionMode::MultiGpuTemporalDecimation
                   : VoxelExecutionMode::MultiGpuFull;
    }

    std::string ValidationConfigKey(const AutomaticBenchmarkConfig& config)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << AutomaticBenchmarkRunner::SuiteName(config.Suite)
               << "|family=" << ValidationModeFamily(config.Mode)
               << "|preset=" << config.Preset
               << "|label=" << config.RequestedLabelCount
               << "|static=" << config.RequestedStaticBudget
               << "|dynamic=" << config.RequestedDynamicBudget
               << "|share=" << std::fixed << std::setprecision(2) << config.SecondaryShare
               << "|lod=" << (config.SpatialLodEnabled ? "on" : "off")
               << "|interval=" << config.TemporalInterval;
        return stream.str();
    }

    std::vector<std::pair<std::string, uint64_t>> ValidationCheckpointsForConfig(
        const AutomaticBenchmarkConfig& config)
    {
        std::vector<std::pair<std::string, uint64_t>> checkpoints;
        checkpoints.emplace_back("steady_240", 240);
        if (config.TemporalInterval > 1)
        {
            checkpoints.emplace_back("temporal_phase_1", static_cast<uint64_t>(config.TemporalInterval) + 1);
            checkpoints.emplace_back("temporal_phase_n_minus_1",
                                     static_cast<uint64_t>(config.TemporalInterval) * 2 - 1);
        }
        if (config.SpatialLodEnabled)
        {
            checkpoints.emplace_back("lod_near", 180);
            checkpoints.emplace_back("lod_mid", 240);
            checkpoints.emplace_back("lod_far", 300);
        }
        return checkpoints;
    }

    std::string CameraModeForCheckpoint(const std::string& checkpointId)
    {
        if (checkpointId == "lod_near")
            return "WaterfallCloseup";
        if (checkpointId == "lod_far")
            return "LodSweepRoute";
        return "FixedOverview";
    }

    VoxelResearchCameraMode CameraModeFromName(const std::string& name)
    {
        if (name == "WaterfallCloseup")
            return VoxelResearchCameraMode::WaterfallCloseup;
        if (name == "LodSweepRoute")
            return VoxelResearchCameraMode::LodSweepRoute;
        if (name == "BenchmarkRoute")
            return VoxelResearchCameraMode::BenchmarkRoute;
        return VoxelResearchCameraMode::FixedOverview;
    }

    enum class ValidationSide
    {
        Reference,
        Candidate
    };

    AutomaticBenchmarkConfig ConfigForValidationMode(
        const VoxelVisualValidationCase& validationCase,
        const VoxelExecutionMode mode,
        const ValidationSide side)
    {
        AutomaticBenchmarkConfig config{};
        config.Suite = validationCase.Suite == "Smoke" ? BenchmarkSuite::Smoke : BenchmarkSuite::Full;
        config.Mode = mode;
        config.ModeName = ExecutionModeNameLiteral(mode);
        config.Preset = validationCase.Preset.c_str();
        config.RequestedLabelCount = validationCase.RequestedLabelCount;
        config.RequestedStaticBudget = validationCase.RequestedStaticBudget;
        config.RequestedDynamicBudget = validationCase.RequestedDynamicBudget;
        config.TotalCount = validationCase.RequestedLabelCount;
        config.SecondaryShare = validationCase.SecondaryShare;
        config.SpatialLodEnabled =
            validationCase.ValidationKind == "approximation_fidelity"
                ? (side == ValidationSide::Candidate && validationCase.SpatialLodEnabled)
                : validationCase.SpatialLodEnabled;
        config.TemporalInterval =
            validationCase.ValidationKind == "approximation_fidelity"
                ? (side == ValidationSide::Reference ? 1u : validationCase.TemporalInterval)
                : validationCase.TemporalInterval;
        config.RandomizationSeed = validationCase.RandomizationSeed;
        config.ConfigId = validationCase.ConfigKey + "|" + ExecutionModeNameLiteral(mode);
        return config;
    }

    std::vector<VoxelVisualValidationCase> BuildVisualValidationCasesForSuite(
        const BenchmarkSuite suite,
        const uint32_t seedOverride)
    {
        const auto configs = AutomaticBenchmarkRunner::BuildConfigs(suite, seedOverride);
        std::vector<VoxelVisualValidationCase> cases;
        std::set<std::string> seenImplementation;
        std::set<std::string> seenFidelity;

        for (const auto& config : configs)
        {
            const std::string configKey = ValidationConfigKey(config);
            const auto checkpoints = ValidationCheckpointsForConfig(config);
            const auto singleMode = SingleModeForFamily(config.Mode);
            const auto multiMode = MultiModeForFamily(config.Mode);

            const std::string implementationKey = configKey + "|implementation";
            if (seenImplementation.insert(implementationKey).second)
            {
                for (uint32_t checkpointIndex = 0; checkpointIndex < checkpoints.size(); ++checkpointIndex)
                {
                    const auto& checkpoint = checkpoints[checkpointIndex];
                    VoxelVisualValidationCase validationCase{};
                    validationCase.ValidationKind = "implementation_equivalence";
                    validationCase.ModeFamily = ValidationModeFamily(config.Mode);
                    validationCase.Suite = AutomaticBenchmarkRunner::SuiteName(suite);
                    validationCase.Preset = config.Preset;
                    validationCase.ConfigKey = configKey;
                    validationCase.RequestedLabelCount = config.RequestedLabelCount;
                    validationCase.RequestedStaticBudget = config.RequestedStaticBudget;
                    validationCase.RequestedDynamicBudget = config.RequestedDynamicBudget;
                    validationCase.RandomizationSeed = config.RandomizationSeed;
                    validationCase.SecondaryShare = config.SecondaryShare;
                    validationCase.SingleMode = singleMode;
                    validationCase.MultiMode = multiMode;
                    validationCase.ReferenceMode = singleMode;
                    validationCase.CandidateMode = multiMode;
                    validationCase.SpatialLodEnabled = config.SpatialLodEnabled;
                    validationCase.TemporalInterval = config.TemporalInterval;
                    validationCase.FixedStepCount = checkpoint.second;
                    validationCase.CheckpointId = checkpoint.first;
                    validationCase.CheckpointIndex = checkpointIndex;
                    validationCase.CameraMode = CameraModeForCheckpoint(checkpoint.first);
                    validationCase.CaseId =
                        configKey + "|equivalence|" + checkpoint.first;
                    cases.push_back(std::move(validationCase));
                }
            }

            if (config.TemporalInterval > 1 || config.SpatialLodEnabled)
            {
                const std::string fidelityKey =
                    configKey + "|fidelity|" + ExecutionModeNameLiteral(config.Mode);
                if (seenFidelity.insert(fidelityKey).second)
                {
                    for (uint32_t checkpointIndex = 0; checkpointIndex < checkpoints.size(); ++checkpointIndex)
                    {
                        const auto& checkpoint = checkpoints[checkpointIndex];
                        VoxelVisualValidationCase validationCase{};
                        validationCase.ValidationKind = "approximation_fidelity";
                        validationCase.ModeFamily = ValidationModeFamily(config.Mode);
                        validationCase.Suite = AutomaticBenchmarkRunner::SuiteName(suite);
                        validationCase.Preset = config.Preset;
                        validationCase.ConfigKey = configKey + "|" + ExecutionModeNameLiteral(config.Mode);
                        validationCase.RequestedLabelCount = config.RequestedLabelCount;
                        validationCase.RequestedStaticBudget = config.RequestedStaticBudget;
                        validationCase.RequestedDynamicBudget = config.RequestedDynamicBudget;
                        validationCase.RandomizationSeed = config.RandomizationSeed;
                        validationCase.SecondaryShare = config.SecondaryShare;
                        validationCase.SingleMode = VoxelExecutionMode::SingleGpuFull;
                        validationCase.MultiMode = config.Mode;
                        validationCase.ReferenceMode = VoxelExecutionMode::SingleGpuFull;
                        validationCase.CandidateMode = config.Mode;
                        validationCase.SpatialLodEnabled = config.SpatialLodEnabled;
                        validationCase.TemporalInterval = config.TemporalInterval;
                        validationCase.FixedStepCount = checkpoint.second;
                        validationCase.CheckpointId = checkpoint.first;
                        validationCase.CheckpointIndex = checkpointIndex;
                        validationCase.CameraMode = CameraModeForCheckpoint(checkpoint.first);
                        validationCase.CaseId =
                            configKey + "|fidelity|" + ExecutionModeNameLiteral(config.Mode) + "|" +
                            checkpoint.first;
                        cases.push_back(std::move(validationCase));
                    }
                }
            }
        }

        return cases;
    }

    std::string AdapterLuidPair(const std::string& primary, const std::string& secondary)
    {
        return primary + "->" + secondary;
    }

    std::string AdapterDriverIdentity(const std::string& luid,
                                      const uint32_t vendorId,
                                      const uint32_t deviceId)
    {
        std::ostringstream stream;
        stream << "luid=" << luid << ";vendor=0x" << std::hex << std::uppercase << vendorId
               << ";device=0x" << deviceId;
        return stream.str();
    }

    ResearchProvenanceRecord BuildResearchProvenanceRecord(
        const VoxelBenchmarkProfiler::FrameMetadata& metadata,
        const std::string& buildHash,
        const std::string& shaderSetHash,
        const std::string& protocolHash,
        const std::string& caseConfigHash,
        const std::string& cameraHash)
    {
        ResearchProvenanceRecord record{};
        record.Fields["build.executable_sha256"] = buildHash;
        record.Fields["build.shader_bytecode_set_sha256"] = shaderSetHash;
        record.Fields["adapter.luid_pair"] = AdapterLuidPair(metadata.PrimaryAdapterLuid,
                                                              metadata.SecondaryAdapterLuid);
        record.Fields["adapter.primary_driver_version"] =
            AdapterDriverIdentity(metadata.PrimaryAdapterLuid, metadata.PrimaryVendorId, metadata.PrimaryDeviceId);
        record.Fields["adapter.secondary_driver_version"] =
            AdapterDriverIdentity(metadata.SecondaryAdapterLuid, metadata.SecondaryVendorId, metadata.SecondaryDeviceId);
        record.Fields["validation.protocol_sha256"] = protocolHash;
        record.Fields["validation.case_config_sha256"] = caseConfigHash;
        record.Fields["validation.camera_sha256"] = cameraHash;
        record.Fields["render.resolution"] =
            std::to_string(metadata.RenderWidth) + "x" + std::to_string(metadata.RenderHeight);
        record.Fields["render.color_format"] = "R8G8B8A8_UNORM";
        record.Fields["render.depth_format"] = "R32_FLOAT";
        record.Fields["render.sample_count"] = "1";
        record.Fields["workload.static_seed"] = std::to_string(metadata.Seed);
        record.Fields["workload.dynamic_seed"] = std::to_string(metadata.Seed);
        record.Fields["workload.requested_static_count"] = std::to_string(metadata.StaticVoxelBudget);
        record.Fields["workload.requested_dynamic_count"] = std::to_string(metadata.DynamicVoxelBudget);
        record.Fields["workload.actual_static_count"] = std::to_string(metadata.ActualStaticVoxelCount);
        record.Fields["workload.actual_dynamic_count"] = std::to_string(metadata.ActualDynamicVoxelCount);
        record.Fields["workload.temporal_interval"] = std::to_string(metadata.TemporalDecimationInterval);
        record.Fields["workload.spatial_lod"] = metadata.SpatialLodPolicy;
        record.Fields["workload.partition_strategy"] = metadata.PartitionStrategy;
        record.Fields["workload.chunk_size"] =
            std::to_string(metadata.ChunkSizeX) + "x" +
            std::to_string(metadata.ChunkSizeY) + "x" +
            std::to_string(metadata.ChunkSizeZ);
        record.Fields["runtime.toolchain"] = metadata.BuildConfiguration + ";" + metadata.GitCommit;
        return record;
    }

    const char* BenchmarkConfigClassName(const VoxelBenchmarkConfigClass configClass)
    {
        switch (configClass)
        {
        case VoxelBenchmarkConfigClass::ValidMatchingBenchmark:
            return "Valid matching benchmark configuration";
        case VoxelBenchmarkConfigClass::Diagnostic:
            return "Diagnostic configuration";
        case VoxelBenchmarkConfigClass::InvalidMixedQuality:
            return "Invalid mixed-quality configuration";
        default:
            return "Invalid mixed-quality configuration";
        }
    }

    std::pair<VoxelBenchmarkConfigClass, std::string> ClassifyBenchmarkConfig(
        const VoxelSceneWorkload& workload,
        const VoxelExecutionMode requestedMode)
    {
        const bool requestedTemporal = UsesTemporalExecution(requestedMode);
        const bool workloadTemporal = workload.TemporalPolicy == VoxelTemporalPolicy::Decimated;
        if (requestedTemporal != workloadTemporal)
            return {
                VoxelBenchmarkConfigClass::InvalidMixedQuality,
                "Requested execution mode temporal policy does not match workload temporal policy"
            };

        if (workload.BenchmarkConfigClass == VoxelBenchmarkConfigClass::Diagnostic)
            return {VoxelBenchmarkConfigClass::Diagnostic, workload.BenchmarkConfigReason};

        if (workload.Profile == VoxelResearchWorkloadProfile::OcclusionValidation ||
            workload.Profile == VoxelResearchWorkloadProfile::DemoMixed ||
            workload.CameraMode == VoxelResearchCameraMode::Interactive ||
            workload.PartitionStrategy == VoxelPartitionStrategy::SpatialPlane ||
            workload.SpatialLod.FreezeCamera ||
            workload.SecondaryShare <= 0.0f ||
            workload.SecondaryShare >= 1.0f ||
            workload.StaticStorageMode == StaticVoxelStorageMode::DenseSolidStress)
        {
            return {
                VoxelBenchmarkConfigClass::Diagnostic,
                "Diagnostic partition, edge-share, dense-solid, occlusion validation, or presentation configuration"
            };
        }

        return {VoxelBenchmarkConfigClass::ValidMatchingBenchmark, ""};
    }

    VoxelAdapterOwner AdapterOwnerForPartition(const VoxelExecutionMode mode,
                                               const bool multiGpuAvailable,
                                               const VoxelAdapterPartition& partition)
    {
        return UsesMultiGpuExecution(mode) &&
                   multiGpuAvailable &&
                   partition.PartitionId == VoxelAdapterPartitionId::SecondaryPartition
                   ? VoxelAdapterOwner::Secondary
                   : VoxelAdapterOwner::Primary;
    }

    std::shared_ptr<PEPEngine::Graphics::GDevice> DeviceForAdapterOwner(
        const VoxelAdapterOwner owner,
        const std::shared_ptr<PEPEngine::Graphics::GDevice>& primaryDevice,
        const std::shared_ptr<PEPEngine::Graphics::GDevice>& secondaryDevice)
    {
        return owner == VoxelAdapterOwner::Secondary ? secondaryDevice : primaryDevice;
    }

    bool SameAdapterLuid(const LUID& left, const LUID& right)
    {
        return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
    }

    std::wstring ResolveVoxelWaterfallAssetPathW(const wchar_t* relativePath)
    {
        return ResolveVoxelWaterfallAssetPath(relativePath).wstring();
    }

    std::string ResolveVoxelWaterfallAssetPathA(const char* relativePath)
    {
        return ResolveVoxelWaterfallAssetPath(relativePath).string();
    }

    std::string EscapeJsonString(const std::string& value)
    {
        std::ostringstream out;
        for (const char c : value)
        {
            switch (c)
            {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << c; break;
            }
        }
        return out.str();
    }

    std::string WideToUtf8Local(const std::wstring& value)
    {
        if (value.empty())
            return {};
        const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                                 static_cast<int>(value.size()),
                                                 nullptr, 0, nullptr, nullptr);
        if (required <= 0)
            return {};
        std::string result(static_cast<size_t>(required), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                            result.data(), required, nullptr, nullptr);
        return result;
    }

    struct MemoryAuditSnapshot
    {
        uint64_t FrameSerial = 0;
        double ElapsedSeconds = 0.0;
        uint64_t PrivateUsageBytes = 0;
        uint64_t WorkingSetBytes = 0;
        uint64_t PagefileUsageBytes = 0;
        uint64_t PrimaryLocalUsageBytes = 0;
        uint64_t PrimaryNonLocalUsageBytes = 0;
        uint64_t SecondaryLocalUsageBytes = 0;
        uint64_t SecondaryNonLocalUsageBytes = 0;
        uint64_t CommandListsCreated = 0;
        uint64_t CommandListsAvailable = 0;
        uint64_t CommandListsInFlight = 0;
        uint64_t DescriptorCapacity = 0;
        uint64_t DescriptorActive = 0;
        uint64_t DescriptorFree = 0;
        uint64_t DescriptorStaleRanges = 0;
        uint64_t LiveResources = 0;
        uint64_t CreatedResources = 0;
        uint64_t DestroyedResources = 0;
    };

    void AccumulateDeviceMemoryStats(
        const std::shared_ptr<GDevice>& device,
        uint64_t& localUsage,
        uint64_t& nonLocalUsage,
        MemoryAuditSnapshot& snapshot)
    {
        if (!device)
            return;

        const auto stats = device->GetLifetimeStats();
        localUsage += stats.VideoMemory.LocalCurrentUsage;
        nonLocalUsage += stats.VideoMemory.NonLocalCurrentUsage;
        for (const auto& queue : stats.Queues)
        {
            snapshot.CommandListsCreated += queue.CreatedCommandLists;
            snapshot.CommandListsAvailable += queue.AvailableCommandLists;
            snapshot.CommandListsInFlight += queue.InFlightCommandLists;
        }
        for (const auto& allocator : stats.DescriptorAllocators)
        {
            snapshot.DescriptorCapacity += allocator.DescriptorCapacity;
            snapshot.DescriptorActive += allocator.ActiveDescriptors;
            snapshot.DescriptorFree += allocator.FreeDescriptors;
            snapshot.DescriptorStaleRanges += allocator.StaleRanges;
        }
    }

    MemoryAuditSnapshot CaptureMemoryAuditSnapshot(
        const uint64_t frameSerial,
        const double elapsedSeconds,
        const std::shared_ptr<GDevice>& primaryDevice,
        const std::shared_ptr<GDevice>& secondaryDevice)
    {
        MemoryAuditSnapshot snapshot{};
        snapshot.FrameSerial = frameSerial;
        snapshot.ElapsedSeconds = elapsedSeconds;

        PROCESS_MEMORY_COUNTERS_EX processMemory{};
        processMemory.cb = sizeof(processMemory);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&processMemory),
                                 sizeof(processMemory)))
        {
            snapshot.PrivateUsageBytes = static_cast<uint64_t>(processMemory.PrivateUsage);
            snapshot.WorkingSetBytes = static_cast<uint64_t>(processMemory.WorkingSetSize);
            snapshot.PagefileUsageBytes = static_cast<uint64_t>(processMemory.PagefileUsage);
        }

        AccumulateDeviceMemoryStats(primaryDevice,
                                    snapshot.PrimaryLocalUsageBytes,
                                    snapshot.PrimaryNonLocalUsageBytes,
                                    snapshot);
        if (secondaryDevice && secondaryDevice != primaryDevice)
        {
            AccumulateDeviceMemoryStats(secondaryDevice,
                                        snapshot.SecondaryLocalUsageBytes,
                                        snapshot.SecondaryNonLocalUsageBytes,
                                        snapshot);
        }

        const auto resourceStats = GResource::GetLifetimeStats();
        snapshot.LiveResources = resourceStats.LiveResources;
        snapshot.CreatedResources = resourceStats.CreatedResources;
        snapshot.DestroyedResources = resourceStats.DestroyedResources;
        return snapshot;
    }

    void WriteMemoryTimelineHeader(std::ofstream& file)
    {
        file << "frame_serial,elapsed_seconds,private_usage_bytes,working_set_bytes,committed_pagefile_bytes,"
             << "primary_local_vram_bytes,primary_shared_gpu_bytes,secondary_local_vram_bytes,secondary_shared_gpu_bytes,"
             << "command_lists_created,command_lists_available,command_lists_in_flight,"
             << "descriptor_capacity,descriptor_active,descriptor_free,descriptor_stale_ranges,"
             << "live_d3d12_resources,created_d3d12_resources,destroyed_d3d12_resources\n";
    }

    void WriteMemoryTimelineRow(std::ofstream& file, const MemoryAuditSnapshot& snapshot)
    {
        file << snapshot.FrameSerial << ','
             << std::fixed << std::setprecision(3) << snapshot.ElapsedSeconds << ','
             << snapshot.PrivateUsageBytes << ','
             << snapshot.WorkingSetBytes << ','
             << snapshot.PagefileUsageBytes << ','
             << snapshot.PrimaryLocalUsageBytes << ','
             << snapshot.PrimaryNonLocalUsageBytes << ','
             << snapshot.SecondaryLocalUsageBytes << ','
             << snapshot.SecondaryNonLocalUsageBytes << ','
             << snapshot.CommandListsCreated << ','
             << snapshot.CommandListsAvailable << ','
             << snapshot.CommandListsInFlight << ','
             << snapshot.DescriptorCapacity << ','
             << snapshot.DescriptorActive << ','
             << snapshot.DescriptorFree << ','
             << snapshot.DescriptorStaleRanges << ','
             << snapshot.LiveResources << ','
             << snapshot.CreatedResources << ','
             << snapshot.DestroyedResources << '\n';
    }

    bool HasPositivePrivateUsageTrend(
        const std::vector<MemoryAuditSnapshot>& snapshots,
        const size_t warmupCount)
    {
        if (snapshots.size() <= warmupCount + 4)
            return false;

        const auto& first = snapshots[warmupCount];
        const auto& last = snapshots.back();
        const double seconds = std::max(1.0, last.ElapsedSeconds - first.ElapsedSeconds);
        const int64_t delta = static_cast<int64_t>(last.PrivateUsageBytes) -
            static_cast<int64_t>(first.PrivateUsageBytes);
        const double bytesPerMinute = static_cast<double>(delta) * 60.0 / seconds;
        return bytesPerMinute > 1024.0 * 1024.0;
    }

}

VoxelWaterfallApp::VoxelWaterfallApp(const HINSTANCE hInstance) : D3DApp(hInstance)
{
    mSceneBounds.Center = Vector3(0.0f, 0.0f, 0.0f);
    mSceneBounds.Radius = 200;
}

VoxelWaterfallApp::~VoxelWaterfallApp()
{
    if (imguiInitialized)
    {
        if (!skipDestructorGpuFlush)
            Flush();
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }
}

void VoxelWaterfallApp::Update(const GameTimer& gt)
{
    UpdateAfterFrameResourceAcquire(gt);
}

void VoxelWaterfallApp::UpdateAfterFrameResourceAcquire(const GameTimer& gt)
{
    cpuFrameStart = std::chrono::steady_clock::now();
    assert(currentFrameResourceReady && currentFrameResource &&
           "UpdateAfterFrameResourceAcquire requires a ready frame resource");

    if (researchCameraController)
    {
        researchCameraController->SetMode(voxelWorkload.CameraMode);
        researchCameraController->SetInputBlocked(voxelWorkload.CameraMode != VoxelResearchCameraMode::Interactive ||
                                                  benchmarkProfiler.IsActive() ||
                                                  benchmarkController.IsAutomaticActive());
        researchCameraController->SetDeterministicTime(static_cast<double>(simulationFrameIndex) / 60.0);
    }

    for (auto& e : gameObjects)
    {
        e->Update();
    }

    UpdateMaterials();

    UpdateShadowTransform(gt);
    UpdateMainPassCB(gt);
    UpdateShadowPassCB(gt);
    UpdateSsaoCB(gt);
}

void VoxelWaterfallApp::Draw(const GameTimer& gt)
{
    (void)DrawFrame(gt);
}

bool VoxelWaterfallApp::DrawFrame(const GameTimer& gt)
{
    if (isResizing) return false;
    assert(currentFrameResourceReady && currentFrameResource &&
           "DrawFrame requires a ready frame resource");

    struct DrawFrameScope
    {
        bool& Flag;
        explicit DrawFrameScope(bool& flag) : Flag(flag) { Flag = true; }
        ~DrawFrameScope() { Flag = false; }
    } drawFrameScope(isDrawingFrame);

    const bool benchmarkWasActive = benchmarkProfiler.IsActive();
    std::optional<VoxelBenchmarkProfiler::FrameMetadata> benchmarkFrameMetadata;
    if (benchmarkWasActive)
    {
        benchmarkFrameMetadata = BuildBenchmarkMetadata();
        benchmarkProfiler.BeginFrame(*benchmarkFrameMetadata);
    }

    const UINT timestampHeapIndex = 2 * currentFrameResourceIndex;

    const auto primaryComputeQueue = primeDevice->GetCommandQueue(GQueueType::Compute);
    const auto secondaryComputeQueue = secondDevice
                                           ? secondDevice->GetCommandQueue(GQueueType::Compute)
                                           : primaryComputeQueue;
    const auto primaryCopyQueue = primeDevice->GetCommandQueue(GQueueType::Copy);
    const auto secondaryCopyQueue = secondDevice
                                        ? secondDevice->GetCommandQueue(GQueueType::Copy)
                                        : primaryCopyQueue;
    const auto secondaryGraphicsQueue = secondDevice
                                            ? secondDevice->GetCommandQueue(GQueueType::Graphics)
                                            : primeDevice->GetCommandQueue(GQueueType::Graphics);
    auto renderQueue = primeDevice->GetCommandQueue(GQueueType::Graphics);

    if (lastPrimaryPartitionGraphicsFenceValue != 0)
    {
        primaryComputeQueue->Wait(renderQueue->GetFence(), lastPrimaryPartitionGraphicsFenceValue);
    }
    if (lastSecondaryPartitionGraphicsFenceValue != 0)
    {
        const auto secondaryFenceQueue =
            lastSecondaryPartitionGraphicsFenceOwner == VoxelAdapterOwner::Secondary
                ? secondaryGraphicsQueue
                : renderQueue;
        const auto secondaryComputeWaitQueue =
            lastSecondaryPartitionGraphicsFenceOwner == VoxelAdapterOwner::Secondary
                ? secondaryComputeQueue
                : primaryComputeQueue;
        secondaryComputeWaitQueue->Wait(secondaryFenceQueue->GetFence(), lastSecondaryPartitionGraphicsFenceValue);
    }

    const double wallDeltaSeconds = std::max(0.0, static_cast<double>(gt.DeltaTime()));
    VoxelSimulationSchedulerContext schedulerContext{
        voxelWorkload,
        executionMode,
        (benchmarkWasActive || visualValidationFixedStepMode)
            ? VoxelSimulationSchedulerMode::Benchmark
            : VoxelSimulationSchedulerMode::Interactive,
        interactiveMaxCatchUpSteps,
        visualValidationFixedStepMode ? visualValidationFixedStepsPerFrame : 1u,
        multiGpuAvailable,
        simulationFrameIndex,
        timestampHeapIndex,
        wallDeltaSeconds,
        voxelSimulationAccumulator,
        voxelSimulationTime,
        voxelSimulationStepsThisFrame,
        voxelInterpolationAlpha,
        voxelRecycledCount,
        voxelAliveCount,
        voxelExpectedCount,
        primaryComputeQueue,
        secondaryComputeQueue,
        primaryComputeQueueFenceValue,
        secondaryComputeQueueFenceValue,
        currentFrameResource->ComputeFenceValue,
        benchmarkProfiler
    };
    const auto simulationResult = voxelScheduler.DispatchFrame(schedulerContext);
    currentFrameResource->PrimaryComputeFenceValue = simulationResult.PrimaryComputeFenceValue;
    currentFrameResource->SecondaryComputeFenceValue = simulationResult.SecondaryComputeFenceValue;

    frameGraphTelemetry = {};
    frameGraphTelemetry.FrameResourceIndex = currentFrameResourceIndex;
    frameGraphTelemetry.WallDeltaMs = wallDeltaSeconds * 1000.0;
    frameGraphTelemetry.FrameResourceBackpressurePollCount = currentFrameResourceBackpressurePollCount;
    frameGraphTelemetry.FrameResourceBackpressureMs = currentPrimaryWaitMs;
    frameGraphTelemetry.DrainedMessageCount = currentFrameDrainedMessageCount;
    frameGraphTelemetry.CurrentFramePumpDepth = currentFramePumpDepth;
    frameGraphTelemetry.MaximumObservedFramePumpDepth = maximumObservedFramePumpDepth;
    frameGraphTelemetry.RejectedRecursiveFrameRequests = rejectedRecursiveFrameRequests;
    frameGraphTelemetry.RequestedMode = requestedExecutionMode;
    frameGraphTelemetry.ActualMode = simulationResult.UsedMultiGpuMode
                                          ? executionMode
                                          : (executionMode == VoxelExecutionMode::MultiGpuTemporalDecimation
                                                 ? VoxelExecutionMode::SingleGpuTemporalDecimation
                                                 : VoxelExecutionMode::SingleGpuFull);
    frameGraphTelemetry.PrimaryComputeSubmitted = simulationResult.PrimaryComputeSubmitted;
    frameGraphTelemetry.SecondaryComputeSubmitted = simulationResult.SecondaryComputeSubmitted;
    frameGraphTelemetry.SecondaryConfiguredUpdateInterval =
        simulationResult.SecondaryConfiguredUpdateInterval;
    frameGraphTelemetry.SecondaryEffectiveUpdateInterval =
        simulationResult.SecondaryEffectiveUpdateInterval;
    frameGraphTelemetry.FixedSimulationStepIndex = simulationResult.FixedSimulationStepIndex;
    frameGraphTelemetry.SecondarySimulationDispatchedThisFrame =
        simulationResult.SecondaryWorkThisFrame;
    frameGraphTelemetry.SecondaryStepsSinceLastUpdate =
        simulationResult.SecondaryStepsSinceLastUpdate;
    frameGraphTelemetry.SecondaryInterpolationPhase =
        simulationResult.SecondaryInterpolationPhase;
    frameGraphTelemetry.SecondaryCoarseDeltaTime =
        simulationResult.SecondaryCoarseDeltaTime;
    frameGraphTelemetry.SchedulerMode = simulationResult.SchedulerMode;
    frameGraphTelemetry.RequestedFixedSteps = simulationResult.RequestedFixedSteps;
    frameGraphTelemetry.ExecutedFixedSteps = simulationResult.ExecutedFixedSteps;
    frameGraphTelemetry.DroppedSimulationSteps = simulationResult.DroppedStepCount;
    frameGraphTelemetry.DroppedSimulationTime = simulationResult.DroppedSimulationTime;
    const double droppedSeconds = std::max(0.0, simulationResult.DroppedSimulationTime);
    frameGraphTelemetry.AcceptedSimulationDeltaMs =
        (benchmarkWasActive
             ? static_cast<double>(simulationResult.ExecutedFixedSteps) * (1000.0 / 60.0)
             : std::max(0.0, wallDeltaSeconds - droppedSeconds) * 1000.0);
    frameGraphTelemetry.SimulationStepsPerWallSecond =
        wallDeltaSeconds > 0.0
            ? static_cast<double>(simulationResult.ExecutedFixedSteps) / wallDeltaSeconds
            : 0.0;
    frameGraphTelemetry.SimulationDispatchCount = simulationResult.SimulationDispatchCount;
    frameGraphTelemetry.LogicalUpdatedVoxelCount = simulationResult.LogicalUpdatedVoxelCount;
    frameGraphTelemetry.PrimaryComputeFenceValue = simulationResult.PrimaryComputeFenceValue;
    frameGraphTelemetry.SecondaryComputeFenceValue = simulationResult.SecondaryComputeFenceValue;
    frameGraphTelemetry.ParticleTransferBytes = 0;

    if (!voxelWorkload.SpatialLod.FreezeCamera || !spatialLodCameraInitialized)
    {
        spatialLodCameraPosition = mainPassCB.EyePosW;
        spatialLodCameraInitialized = true;
    }
    if (voxelCompositeDebugView == VoxelCompositeDebugView::SpatialLodColors)
        voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::LodLevel;
    else if (voxelCompositeDebugView == VoxelCompositeDebugView::PartitionOwnershipColors)
        voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::AdapterOwnership;
    for (auto& partition : voxelWorkload.Partitions)
    {
        if (partition.GpuPartition)
            partition.GpuPartition->ConfigureSpatialLod(voxelWorkload.SpatialLod, spatialLodCameraPosition);
    }

    const auto voxelFrameRenderPlan = BuildVoxelFrameRenderPlan();
    ValidateVoxelFrameRenderPlan(voxelFrameRenderPlan);
    std::vector<VoxelPartitionRenderResult> primaryVoxelRenderResults;
    std::vector<VoxelPartitionRenderResult> secondaryVoxelRenderResults;
    const bool hasSecondaryVoxelDrawWork = std::any_of(
        voxelFrameRenderPlan.SecondaryOwnedPartitions.begin(),
        voxelFrameRenderPlan.SecondaryOwnedPartitions.end(),
        [](const VoxelFramePartitionRenderPlan& partition)
        {
            return partition.GpuPartition && partition.LogicalVoxelCount > 0;
        });

    PrimaryBasePassContext primaryBaseContext{
        renderQueue,
        primaryComputeQueue->GetFence(),
        simulationResult.PrimaryComputeFenceValue,
        timestampHeapIndex,
        *currentFrameResource,
        benchmarkProfiler,
        graphicsPassFenceValue,
        &frameGraphTelemetry,
        &voxelFrameRenderPlan,
        &primaryVoxelRenderResults,
        [this, &voxelFrameRenderPlan, &primaryVoxelRenderResults](const std::shared_ptr<GCommandList>& cmdList)
        {
            VoxelRenderPassContext basePassContext{
                primeDeviceSignature,
                ssaoPrimeRootSignature,
                srvTexturesMemory,
                *currentFrameResource,
                fullViewport,
                fullRect,
                *shadowPath,
                *ambientPrimePath,
                *antiAliasingPrimePath,
                defaultPrimePipelineResources,
                typedRenderer,
                &voxelFrameRenderPlan.PrimaryOwnedPartitions,
                &primaryVoxelRenderResults,
                &benchmarkProfiler,
                MainWindow->GetCurrentBackBuffer()
            };
            basePassContext.DynamicShadowsEnabled = voxelWorkload.DynamicShadowsEnabled;
            basePassContext.BackgroundColor = BackgroundColorForLightingPreset(voxelWorkload.LightingMode);
            voxelRenderPasses.RecordPrimaryBase(cmdList, basePassContext);
        }
    };
    renderPipeline.SubmitPrimaryBasePass(primaryBaseContext);
    for (const auto& result : primaryVoxelRenderResults)
    {
        frameGraphTelemetry.PrimarySpatialLodStats.Lod0Rendered += result.LodStats.Lod0Rendered;
        frameGraphTelemetry.PrimarySpatialLodStats.Lod1Rendered += result.LodStats.Lod1Rendered;
        frameGraphTelemetry.PrimarySpatialLodStats.Lod2Rendered += result.LodStats.Lod2Rendered;
        frameGraphTelemetry.PrimarySpatialLodStats.Aggregated += result.LodStats.Aggregated;
        frameGraphTelemetry.PrimarySpatialLodStats.ProbeOverflow += result.LodStats.ProbeOverflow;
        frameGraphTelemetry.PrimarySpatialLodStats.TotalProbeCount += result.LodStats.TotalProbeCount;
        frameGraphTelemetry.PrimarySpatialLodStats.EmittedGroups += result.LodStats.EmittedGroups;
        frameGraphTelemetry.PrimarySpatialLodStats.HashCapacity += result.LodStats.HashCapacity;
        frameGraphTelemetry.PrimarySpatialLodStats.MaxProbeCount =
            std::max(frameGraphTelemetry.PrimarySpatialLodStats.MaxProbeCount, result.LodStats.MaxProbeCount);
        if (result.UsedIndirectDraw)
            frameGraphTelemetry.PrimaryIndirectDrawCalls += result.DrawCallCount;
    }
    lastPrimaryPartitionGraphicsFenceValue = currentFrameResource->PrimaryBaseRenderFenceValue;
    if (!hasSecondaryVoxelDrawWork)
    {
        lastSecondaryPartitionGraphicsFenceValue = currentFrameResource->PrimaryBaseRenderFenceValue;
        lastSecondaryPartitionGraphicsFenceOwner = VoxelAdapterOwner::Primary;
    }
    const bool runSecondaryGraphics =
        simulationResult.UsedMultiGpuMode &&
        multiGpuVoxelRenderTargets.IsInitialized() &&
        hasSecondaryVoxelDrawWork;
    bool secondaryImageReadyThisFrame = false;

    if (runSecondaryGraphics)
    {
        auto& secondaryFrameTargets = multiGpuVoxelRenderTargets.GetFrames()[currentFrameResourceIndex];
        const auto secondaryDesc = secondaryFrameTargets.SecondaryLocalColor.GetD3D12ResourceDesc();
        const D3D12_VIEWPORT secondaryViewport{
            0.0f,
            0.0f,
            static_cast<float>(secondaryDesc.Width),
            static_cast<float>(secondaryDesc.Height),
            0.0f,
            1.0f
        };
        const D3D12_RECT secondaryScissor{
            0,
            0,
            static_cast<LONG>(secondaryDesc.Width),
            static_cast<LONG>(secondaryDesc.Height)
        };

        SecondaryVoxelGraphicsPassContext secondaryGraphicsContext{
            secondaryGraphicsQueue,
            secondaryComputeQueue->GetFence(),
            simulationResult.SecondaryComputeFenceValue,
            timestampHeapIndex,
            *currentFrameResource,
            &voxelFrameRenderPlan.SecondaryOwnedPartitions,
            secondaryFrameTargets,
            secondaryViewport,
            secondaryScissor,
            benchmarkProfiler,
            &frameGraphTelemetry,
            &secondaryVoxelRenderResults
        };
        renderPipeline.SubmitSecondaryVoxelPass(secondaryGraphicsContext);
        lastSecondaryPartitionGraphicsFenceValue = currentFrameResource->SecondaryRenderFenceValue;
        lastSecondaryPartitionGraphicsFenceOwner = VoxelAdapterOwner::Secondary;

        SecondaryLocalToSharedCopyPassContext secondaryCopyContext{
            secondaryCopyQueue,
            secondaryGraphicsQueue->GetFence(),
            currentFrameResource->SecondaryRenderFenceValue,
            secondCrossAdapterRenderReadyFence,
            crossAdapterRenderReadyFenceValue,
            timestampHeapIndex,
            *currentFrameResource,
            secondaryFrameTargets,
            benchmarkProfiler,
            &frameGraphTelemetry
        };
        renderPipeline.SubmitSecondaryLocalToSharedCopyPass(secondaryCopyContext);

        PrimarySharedToLocalCopyPassContext primaryCopyContext{
            primaryCopyQueue,
            primeCrossAdapterRenderReadyFence,
            currentFrameResource->CrossAdapterRenderReadyFenceValue,
            timestampHeapIndex,
            *currentFrameResource,
            secondaryFrameTargets,
            benchmarkProfiler,
            &frameGraphTelemetry
        };
        renderPipeline.SubmitPrimarySharedToLocalCopyPass(primaryCopyContext);
        secondaryFrameTargets.HasReceivedImage = true;
        secondaryImageReadyThisFrame = true;
    }

    const bool shouldComposeSecondaryImage =
        runSecondaryGraphics &&
        multiGpuVoxelRenderTargets.GetFrames()[currentFrameResourceIndex].HasReceivedImage &&
        voxelCompositePass.IsInitialized();
    frameGraphTelemetry.CompositeSubmitted = shouldComposeSecondaryImage;
    frameGraphTelemetry.CompositeUsedSecondaryImage = shouldComposeSecondaryImage;
    frameGraphTelemetry.CompositeDebugView = voxelCompositeDebugView;

    FinalCompositeAndPresentPassContext finalPassContext{
        renderQueue,
        renderQueue->GetFence(),
        currentFrameResource->PrimaryBaseRenderFenceValue,
        primaryCopyQueue->GetFence(),
        currentFrameResource->PrimarySecondaryImageReadyFenceValue,
        secondaryImageReadyThisFrame,
        timestampHeapIndex,
        *currentFrameResource,
        graphicsPassFenceValue,
        benchmarkProfiler,
        &frameGraphTelemetry,
        [this, shouldComposeSecondaryImage](const std::shared_ptr<GCommandList>& cmdList)
        {
            VoxelRenderPassContext finalPassContext{
                primeDeviceSignature,
                ssaoPrimeRootSignature,
                srvTexturesMemory,
                *currentFrameResource,
                fullViewport,
                fullRect,
                *shadowPath,
                *ambientPrimePath,
                *antiAliasingPrimePath,
                defaultPrimePipelineResources,
                typedRenderer,
                nullptr,
                nullptr,
                nullptr,
                MainWindow->GetCurrentBackBuffer()
            };
            if (shouldComposeSecondaryImage)
            {
                auto& targets = multiGpuVoxelRenderTargets.GetFrames()[currentFrameResourceIndex];
                benchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                             VoxelBenchmarkProfiler::RangeId::Composite);
                VoxelCompositePassContext compositeContext{
                    antiAliasingPrimePath->GetRenderTarget(),
                    antiAliasingPrimePath->GetDepthMap(),
                    targets,
                    antiAliasingPrimePath->GetViewPort(),
                    antiAliasingPrimePath->GetRect(),
                    mainPassCB.NearZ,
                    mainPassCB.FarZ,
                    voxelCompositeDebugView
                };
                voxelCompositePass.Record(cmdList, compositeContext);
                benchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                           VoxelBenchmarkProfiler::RangeId::Composite);
                benchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                               VoxelBenchmarkProfiler::RangeId::Composite);
                finalPassContext.ResolveSourceSrv = &targets.PrimaryCompositeDescriptors;
                finalPassContext.ResolveSourceSrvOffset = 4;
            }
            benchmarkProfiler.BeginRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                         VoxelBenchmarkProfiler::RangeId::FinalResolveUi);
            voxelRenderPasses.RecordFinalPresent(cmdList, finalPassContext);
            if (!benchmarkController.IsAutomaticActive())
                DrawUserInterface(cmdList);
            cmdList->TransitionBarrier(MainWindow->GetCurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT);
            cmdList->FlushResourceBarriers();
            benchmarkProfiler.EndRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                       VoxelBenchmarkProfiler::RangeId::FinalResolveUi);
            benchmarkProfiler.ResolveRange(cmdList, VoxelBenchmarkProfiler::QueueId::PrimaryGraphics,
                                           VoxelBenchmarkProfiler::RangeId::FinalResolveUi);
        }
    };
    renderPipeline.SubmitFinalCompositeAndPresentPass(finalPassContext);
    if (currentFrameResource && currentFrameResource->PrimeRenderFenceValue != 0)
    {
        retainedVoxelFrameRenderPlans.push_back(
            RetainedVoxelFrameRenderPlan{voxelFrameRenderPlan, currentFrameResource->PrimeRenderFenceValue});
    }
    ValidateVoxelFrameDrawResultsCheap(
        voxelFrameRenderPlan,
        primaryVoxelRenderResults,
        secondaryVoxelRenderResults,
        frameGraphTelemetry.SecondaryGraphicsSubmitted);
    frameGraphTelemetry.VisualValidationHasResult = visualValidationMetrics.HasResult;
    frameGraphTelemetry.VisualValidationPassed =
        visualValidationMetrics.HasResult && visualValidationMetrics.Passed;
    frameGraphTelemetry.VisualValidationRunId = visualValidationMetrics.ValidationRunId;
    frameGraphTelemetry.VisualValidationSnapshotHash = visualValidationMetrics.SnapshotHash;
    frameGraphTelemetry.VisualValidationColorMAE = visualValidationMetrics.ColorMAE;
    frameGraphTelemetry.VisualValidationColorRMSE = visualValidationMetrics.ColorRMSE;
    frameGraphTelemetry.VisualValidationPSNR = visualValidationMetrics.ColorPSNR;
    frameGraphTelemetry.VisualValidationMaxError = visualValidationMetrics.MaxColorError;
    frameGraphTelemetry.VisualValidationMismatchedPixelPercent =
        visualValidationMetrics.ColorMismatchPercent;
    frameGraphTelemetry.VisualValidationDepthRMSE = visualValidationMetrics.DepthRMSE;
    frameGraphTelemetry.VisualValidationDepthMismatchPercent =
        visualValidationMetrics.DepthMismatchPercent;
    frameGraphTelemetry.VisualValidationPipelinePrimitiveCount =
        visualValidationMetrics.PipelinePrimitiveCount;
    frameGraphTelemetry.VisualValidationFailReason = visualValidationMetrics.FailReason;

    currentFrameResourceIndex = MainWindow->Present();
    const auto presentTime = std::chrono::steady_clock::now();
    currentPresentToPresentMs = hasSuccessfulPresentTime
                                    ? std::chrono::duration<double, std::milli>(
                                        presentTime - lastSuccessfulPresentTime).count()
                                    : 0.0;
    lastSuccessfulPresentTime = presentTime;
    hasSuccessfulPresentTime = true;
    ++successfulPresentCount;
    ++totalSuccessfulPresentCount;
    frameGraphTelemetry.SuccessfulPresentCount = totalSuccessfulPresentCount;
    if (benchmarkProfiler.IsActive() && benchmarkFrameMetadata)
    {
        RefreshBenchmarkFrameTelemetry(*benchmarkFrameMetadata);
        benchmarkProfiler.UpdateCurrentFrameMetadata(*benchmarkFrameMetadata);
        benchmarkProfiler.EndFrameCpu();
        benchmarkProfiler.ProcessCompletedFrames();
    }
    if (benchmarkWasActive)
    {
        auto benchmarkContext = BuildBenchmarkControllerContext();
        benchmarkController.RestoreVSyncAfterManualCompletion(benchmarkContext, benchmarkWasActive);
    }
    return true;
}

bool VoxelWaterfallApp::Initialize()
{
    InitDevices();
    const auto primaryComputeQueue = primeDevice->GetCommandQueue(GQueueType::Compute);
    const auto primaryGraphicsQueue = primeDevice->GetCommandQueue(GQueueType::Graphics);
    const auto primaryCopyQueue = primeDevice->GetCommandQueue(GQueueType::Copy);
    const auto secondaryComputeQueue = secondDevice
                                           ? secondDevice->GetCommandQueue(GQueueType::Compute)
                                           : primaryComputeQueue;
    const auto secondaryGraphicsQueue = secondDevice
                                            ? secondDevice->GetCommandQueue(GQueueType::Graphics)
                                            : primaryGraphicsQueue;
    const auto secondaryCopyQueue = secondDevice
                                        ? secondDevice->GetCommandQueue(GQueueType::Copy)
                                        : primaryCopyQueue;
    benchmarkProfiler.Initialize(primeDevice, secondDevice,
                                 primaryComputeQueue,
                                 primaryGraphicsQueue,
                                 secondaryComputeQueue,
                                 secondaryGraphicsQueue,
                                 secondaryCopyQueue,
                                 primaryCopyQueue);
    InitializeBenchmarkProvenanceCache();
    InitMainWindow();
    Flush();

    LoadStudyTexture();
    Flush();
    LoadModels();
    Flush();
    CreateMaterials();
    Flush();
    GenerateMipMaps();
    Flush();

    InitRenderPaths();
    Flush();
    InitSRVMemoryAndMaterials();
    Flush();
    InitRootSignature();
    Flush();
    InitPipeLineResource();
    Flush();
    CreateGO();
    Flush();
    SortGO();
    Flush();
    InitFrameResource();
    Flush();

    OnResize();
    InitUserInterface();

    Flush();

    return true;
}

void VoxelWaterfallApp::InitDevices()
{
    auto allDevices = GDeviceFactory::GetAllDevices(false);
    const auto selectedDevices = DeviceSelectionPolicy::Select(allDevices);
    adapterReportLines.clear();
    for (const auto& info : selectedDevices.Adapters)
    {
        std::wstringstream ss;
        ss << L"[MGPU Adapter] index=" << info.AdapterIndex
           << L" name=" << info.Name
           << L" hardware=" << (info.Hardware ? L"true" : L"false")
           << L" graphics=" << (info.GraphicsQueue ? L"true" : L"false")
           << L" compute=" << (info.ComputeQueue ? L"true" : L"false")
           << L" copy=" << (info.CopyQueue ? L"true" : L"false")
           << L" crossAdapterTexture=" << (info.CrossAdapterTexture ? L"true" : L"false")
           << L" selectedPrimary=" << (info.SelectedPrimary ? L"true" : L"false")
           << L" selectedSecondary=" << (info.SelectedSecondary ? L"true" : L"false")
           << L" status=" << info.Status
           << L"\n";

        OutputDebugStringW(ss.str().c_str());
    }

    if (!selectedDevices.MultiGpuUnavailableReason.empty())
    {
        std::wstring reason = L"[MGPU Reason] " + selectedDevices.MultiGpuUnavailableReason + L"\n";
        OutputDebugStringW(reason.c_str());
    }
    for (const auto& adapter : selectedDevices.Adapters)
    {
        adapterReportLines.push_back(
            adapter.Name +
            L" | hardware=" + std::to_wstring(adapter.Hardware) +
            L" graphics=" + std::to_wstring(adapter.GraphicsQueue) +
            L" compute=" + std::to_wstring(adapter.ComputeQueue) +
            L" copy=" + std::to_wstring(adapter.CopyQueue) +
            L" crossAdapterTexture=" + std::to_wstring(adapter.CrossAdapterTexture) +
            L" | " + adapter.Status);
    }

    primeDevice = selectedDevices.Primary ? selectedDevices.Primary :
                  (!allDevices.empty() ? allDevices.front() : nullptr);
    secondDevice = selectedDevices.Secondary;

    if (!primeDevice)
        throw std::runtime_error("No Direct3D 12 hardware adapter is available for MGPU-VoxelWaterfall");

    assets = std::make_shared<AssetsLoader>(primeDevice);


    for (int i = 0; i < static_cast<uint8_t>(RenderMode::Count); ++i)
    {
        typedRenderer.push_back(
            MemoryAllocator::CreateVector<std::shared_ptr<Renderer>>());
    }

    multiGpuAvailable = secondDevice != nullptr && secondDevice != primeDevice;
    crossAdapterTransferMode = multiGpuAvailable
                                   ? selectedDevices.TransferMode
                                   : CrossAdapterTransferMode::Unavailable;
    multiGpuPublicationEligible = selectedDevices.PublicationEligible;

    if (crossAdapterTransferMode == CrossAdapterTransferMode::CopyOnlyCrossAdapter)
    {
        OutputDebugStringW(
            L"[MGPU] Direct cross-adapter RTV/SRV/UAV texture path is not supported. "
            L"Continuing with selected hardware secondary adapter as CopyOnlyCrossAdapter.\n"
        );
    }
    else if (multiGpuAvailable && !secondDevice->GetCommandQueue(GQueueType::Graphics))
    {
        DisableMultiGpu(L"MultiGpu unavailable: secondary adapter graphics queue is not available");
    }

    if (multiGpuAvailable)
    {
        const bool renderReadyFenceReady = primeDevice->TrySharedFence(
            primeCrossAdapterRenderReadyFence,
            secondDevice,
            secondCrossAdapterRenderReadyFence,
            crossAdapterRenderReadyFenceValue);
        if (!renderReadyFenceReady)
        {
            DisableMultiGpu(L"MultiGpu unavailable: shared cross-adapter fence creation/open failed");
        }
        else
        {
            multiGpuStatus = L"MultiGpu hardware available; transfer mode=" +
                std::wstring(CrossAdapterTransferModeNameW(crossAdapterTransferMode)) +
                L"; render target validation pending";
        }
    }
    else
    {
        DisableMultiGpu(selectedDevices.MultiGpuUnavailableReason.empty()
                            ? L"MultiGpu unavailable: secondary hardware adapter was not found"
                            : selectedDevices.MultiGpuUnavailableReason);
    }

    logQueue.Push(L"\nPrime Device: " + (primeDevice->GetName()));
    logQueue.Push(
        L"\t\n Cross Adapter Texture Support: " + std::to_wstring(
            primeDevice->IsCrossAdapterTextureSupported()));
    if (multiGpuAvailable && secondDevice)
    {
        logQueue.Push(L"\nSecond Device: " + (secondDevice->GetName()));
        logQueue.Push(
            L"\t\n Cross Adapter Texture Support: " + std::to_wstring(
                secondDevice->IsCrossAdapterTextureSupported()));
    }
    else
    {
        logQueue.Push(L"\nSecond Device: unavailable; MultiGpu modes are disabled");
    }
}

void VoxelWaterfallApp::InitUserInterface()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    auto& io = ImGui::GetIO();
    io.FontGlobalScale = DebugUiScale;
    ImGui::GetStyle().ScaleAllSizes(DebugUiScale);

    imguiSrvMemory = primeDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);
    ImGui_ImplWin32_Init(MainWindow->GetWindowHandle());
    ImGui_ImplDX12_InitInfo dx12InitInfo{};
    dx12InitInfo.Device = primeDevice->GetDXDevice().Get();
    dx12InitInfo.CommandQueue = primeDevice->GetCommandQueue(GQueueType::Graphics)->GetD3D12CommandQueue().Get();
    dx12InitInfo.NumFramesInFlight = globalCountFrameResources;
    dx12InitInfo.RTVFormat = GetSRGBFormat(BackBufferFormat);
    dx12InitInfo.DSVFormat = DepthStencilFormat;
    dx12InitInfo.SrvDescriptorHeap = imguiSrvMemory.GetDescriptorHeap()->GetDirectxHeap();
    dx12InitInfo.UserData = &imguiSrvMemory;
    dx12InitInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info,
                                           D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
    {
        const auto descriptor = static_cast<GDescriptor*>(info->UserData);
        *outCpuHandle = descriptor->GetCPUHandle();
        *outGpuHandle = descriptor->GetGPUHandle();
    };
    dx12InitInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
                                          D3D12_CPU_DESCRIPTOR_HANDLE,
                                          D3D12_GPU_DESCRIPTOR_HANDLE)
    {
    };
    ImGui_ImplDX12_Init(&dx12InitInfo);
    imguiInitialized = true;
}

void VoxelWaterfallApp::DrawUserInterface(const std::shared_ptr<GCommandList>& cmdList)
{
    VoxelWaterfallDebugPanelContext context{
        imguiInitialized,
        cmdList,
        &imguiSrvMemory,
        voxelWorkload,
        requestedExecutionMode,
        executionMode,
        multiGpuAvailable,
        multiGpuStatus,
        primeDevice ? primeDevice->GetName() : L"unavailable",
        multiGpuAvailable && secondDevice ? secondDevice->GetName() : L"unavailable",
        CrossAdapterTransferModeName(crossAdapterTransferMode),
        multiGpuPublicationEligible,
        &adapterReportLines,
        simulationFrameIndex,
        voxelSimulationAccumulator,
        voxelSimulationStepsThisFrame,
        voxelInterpolationAlpha,
        voxelRecycledCount,
        voxelAliveCount,
        voxelExpectedCount,
        &frameGraphTelemetry,
        voxelCompositeDebugView,
        benchmarkProfiler,
        benchmarkController.WasVSyncEnabled(),
        benchmarkController.IsAutomaticActive(),
        benchmarkController.HasAutomaticConfigs(),
        benchmarkController.GetAutomaticIndex(),
        benchmarkController.GetAutomaticCount(),
        benchmarkController.GetAutomaticSummaryPath(),
        gProvenanceFileHashCount,
        gGitProcessSpawnCount,
        slowFrameValidationCount,
        benchmarkMetadataBuildCount,
        researchRunnerActive,
        researchRunnerPhase,
        researchRunnerBlockedReason,
        researchRunnerOutputPath,
        researchRunnerConfigIndex,
        researchRunnerConfigTotal,
        [this](const VoxelResearchWorkloadProfile profile) { pendingRuntimeChanges.WorkloadProfile = profile; },
        [this](const VoxelResearchCameraMode mode) { pendingRuntimeChanges.CameraMode = mode; },
        [this](const VoxelResearchLightingPreset preset) { pendingRuntimeChanges.LightingPreset = preset; },
        [this](const VoxelRenderResolutionPreset preset) { pendingRuntimeChanges.RenderResolutionPreset = preset; },
        [this](const VoxelExecutionMode mode) { pendingRuntimeChanges.ExecutionMode = mode; },
        [this] { StartManualBenchmark(); },
        [this] { StopManualBenchmark(); },
        [this] { RequestVisualValidation(); },
        [this] { StartAutomaticBenchmark(); },
        [this] { StopAutomaticBenchmark(); },
        [this] { RequestApplyVoxelWorkloadSettings(); },
        [this](const ResearchRunnerRequest& request) { RequestResearchRunner(request); },
        [this] { CancelResearchRunner(); }
    };
    debugPanel.Draw(context);
}

void VoxelWaterfallApp::StartManualBenchmark()
{
    benchmarkController.StartManual(BuildBenchmarkControllerContext());
}

void VoxelWaterfallApp::StopManualBenchmark()
{
    benchmarkController.StopManual(BuildBenchmarkControllerContext());
}

VoxelVisualValidationComparisonInput VoxelWaterfallApp::CaptureVisualValidationCase(
    const VoxelVisualValidationConfig& config,
    const VoxelVisualValidationCase& validationCase,
    const std::filesystem::path& outputDirectory)
{
    using namespace PEPEngine::Graphics;

    VoxelVisualValidationComparisonInput input{};
    input.CaseId = validationCase.CaseId;
    input.ValidationKind = validationCase.ValidationKind;
    input.CheckpointId = validationCase.CheckpointId;
    input.ProtocolHash = validationCase.ProtocolHash;
    input.ReferenceConfigHash = validationCase.ReferenceConfigHash;
    input.CandidateConfigHash = validationCase.CandidateConfigHash;
    input.RequestedReferenceMode = validationCase.ReferenceMode;
    input.RequestedCandidateMode = validationCase.CandidateMode;
    input.ActualSingleMode = validationCase.SingleMode;
    input.ActualMultiMode = validationCase.MultiMode;
    input.RenderWidth = validationCase.RenderWidth;
    input.RenderHeight = validationCase.RenderHeight;
    input.SampleCount = validationCase.SampleCount;
    input.ColorFormat = validationCase.ColorFormat;
    input.LinearDepthFormat = validationCase.LinearDepthFormat;
    input.ActualStaticCount = validationCase.ActualStaticCount;
    input.ActualDynamicCount = validationCase.ActualDynamicCount;
    input.ActualTotalCount = validationCase.ActualTotalCount;
    input.ConfigHash = validationCase.ConfigHash;
    input.CameraHash = validationCase.CameraHash;
    input.AdapterPairIdentity = visualValidationAdapterPairIdentity;
    input.SingleColorReferencePath = RawPath(outputDirectory, validationCase.CaseId, "single_color");
    input.SingleDepthReferencePath = RawPath(outputDirectory, validationCase.CaseId, "single_depth");
    input.MultiColorReferencePath = RawPath(outputDirectory, validationCase.CaseId, "multi_color");
    input.MultiDepthReferencePath = RawPath(outputDirectory, validationCase.CaseId, "multi_depth");
    input.DiffReferencePath = RawPath(outputDirectory, validationCase.CaseId, "diff");

    const auto graphicsQueue = primeDevice ? primeDevice->GetCommandQueue(GQueueType::Graphics) : nullptr;
    const auto computeQueue = primeDevice ? primeDevice->GetCommandQueue(GQueueType::Compute) : nullptr;
    if (!primeDevice || !graphicsQueue || !computeQueue || !antiAliasingPrimePath)
    {
        input.BlockedReason = "GPU capture unavailable: primary device, queues, or render targets are not initialized";
        return input;
    }

    const auto createLinearDepth = [&](GTexture& primaryDepth,
                                       GTexture& secondaryColor,
                                       GTexture& secondaryDepth,
                                       const bool useSecondary,
                                       const std::wstring& name,
                                       std::string& reason) -> GTexture
    {
        const auto desc = antiAliasingPrimePath->GetRenderTarget().GetD3D12ResourceDesc();
        auto output = CreateValidationTexture(
            primeDevice,
            static_cast<uint32_t>(desc.Width),
            desc.Height,
            DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            name);

        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);
        CD3DX12_DESCRIPTOR_RANGE uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        GRootSignature rootSignature;
        rootSignature.AddConstantParameter(sizeof(ValidationDepthConstants) / sizeof(uint32_t), 0);
        rootSignature.AddDescriptorParameter(&srvRange, 1);
        rootSignature.AddDescriptorParameter(&uavRange, 1);
        rootSignature.Initialize(primeDevice, false, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        GShader shader(L"Shaders\\VoxelValidationDepthLinearize.hlsl", ComputeShader, nullptr, "CS", "cs_5_1");
        shader.LoadAndCompile();
        ComputePSO pso(rootSignature);
        pso.SetShader(&shader);
        pso.Initialize(primeDevice);

        auto descriptors = primeDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4);
        auto primaryDepthSrv = Texture2DSrvDesc(DXGI_FORMAT_R32_FLOAT);
        primaryDepth.CreateShaderResourceView(&primaryDepthSrv, &descriptors, 0);
        auto secondaryColorSrv = Texture2DSrvDesc(secondaryColor.GetD3D12ResourceDesc().Format);
        secondaryColor.CreateShaderResourceView(&secondaryColorSrv, &descriptors, 1);
        auto secondaryDepthSrv = Texture2DSrvDesc(DXGI_FORMAT_R32_FLOAT);
        secondaryDepth.CreateShaderResourceView(&secondaryDepthSrv, &descriptors, 2);
        auto outputUav = Texture2DUavDesc(DXGI_FORMAT_R32_FLOAT);
        output.CreateUnorderedAccessView(&outputUav, &descriptors, 3);

        ValidationDepthConstants constants{};
        constants.RenderSize[0] = static_cast<uint32_t>(desc.Width);
        constants.RenderSize[1] = desc.Height;
        constants.NearZ = config.Snapshot.NearZ;
        constants.FarZ = config.Snapshot.FarZ;
        constants.DepthEpsilon = static_cast<float>(config.Tolerances.DepthTolerance);
        constants.UseSecondary = useSecondary ? 1u : 0u;

        const auto cmdList = computeQueue->GetCommandList();
        cmdList->SetPipelineState(pso);
        cmdList->SetDescriptorsHeap(&descriptors);
        cmdList->TransitionBarrier(primaryDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->TransitionBarrier(secondaryColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->TransitionBarrier(secondaryDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->TransitionBarrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->FlushResourceBarriers();
        cmdList->SetRoot32BitConstants(0, sizeof(ValidationDepthConstants) / sizeof(uint32_t), &constants, 0);
        cmdList->SetRootDescriptorTable(1, &descriptors, 0);
        cmdList->SetRootDescriptorTable(2, &descriptors, 3);
        cmdList->Dispatch((constants.RenderSize[0] + 7u) / 8u, (constants.RenderSize[1] + 7u) / 8u, 1);
        cmdList->UAVBarrier(output, true);
        const auto fence = computeQueue->ExecuteCommandList(cmdList);
        computeQueue->WaitForFenceValue(fence);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        reason.clear();
        return output;
    };

    const auto copyTexture = [&](GTexture& source, const std::wstring& name, std::string& reason) -> GTexture
    {
        const auto sourceDesc = source.GetD3D12ResourceDesc();
        auto copy = CreateValidationTexture(
            primeDevice,
            static_cast<uint32_t>(sourceDesc.Width),
            sourceDesc.Height,
            sourceDesc.Format,
            D3D12_RESOURCE_FLAG_NONE,
            name);

        const auto cmdList = graphicsQueue->GetCommandList();
        cmdList->CopyResource(copy, source);
        const auto fence = graphicsQueue->ExecuteCommandList(cmdList);
        graphicsQueue->WaitForFenceValue(fence);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        reason.clear();
        return copy;
    };

    const auto capturePath = [&](const ValidationSide side,
                                 const VoxelExecutionMode mode,
                                 const bool multiPath,
                                 const wchar_t* label) -> ValidationCapturedPath
    {
        ValidationCapturedPath capture{};
        capture.ActualMode = mode;
        try
        {
            ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile::MixedStaticAndDynamic);
            ApplyRenderResolutionPreset(VoxelRenderResolutionPreset::R1920x1080);
            ApplyResearchCameraMode(CameraModeFromName(validationCase.CameraMode));
            ApplyResearchLightingPreset(VoxelResearchLightingPreset::BenchmarkNeutral);
            voxelCompositeDebugView = VoxelCompositeDebugView::FinalComposite;
            const auto resolved = ApplyBenchmarkConfigurationAtomic(
                ConfigForValidationMode(validationCase, mode, side));
            if (!resolved.Passed)
            {
                capture.BlockedReason = "deterministic validation could not apply resolved benchmark config: " +
                    resolved.Reason;
                return capture;
            }
            const std::string expectedHash =
                side == ValidationSide::Reference
                    ? validationCase.ReferenceConfigHash
                    : validationCase.CandidateConfigHash;
            if (resolved.ResolvedConfigHash != expectedHash)
            {
                capture.BlockedReason =
                    "deterministic validation resolved config hash mismatch for " +
                    GetExecutionModeName(mode);
                return capture;
            }
            if (mode == validationCase.CandidateMode &&
                (resolved.ActualStaticCount != validationCase.ActualStaticCount ||
                 resolved.ActualDynamicCount != validationCase.ActualDynamicCount ||
                 resolved.ActualTotalCount != validationCase.ActualTotalCount))
            {
                capture.BlockedReason =
                    "deterministic validation actual workload counts changed after protocol capture";
                return capture;
            }
            ResetBenchmarkDeterministicState();
            Flush();

            auto* timerPtr = GetTimer();
            timerPtr->Reset();
            pumpFrameQuitRequested = false;
            visualValidationFixedStepMode = true;
            visualValidationFixedStepsPerFrame = 1;

            bool presented = false;
            uint64_t executedFixedSteps = 0;
            const uint32_t maxAttempts =
                static_cast<uint32_t>(std::max<uint64_t>(validationCase.FixedStepCount * 120, 240));
            for (uint32_t attempt = 0;
                 attempt < maxAttempts && executedFixedSteps < validationCase.FixedStepCount && !pumpFrameQuitRequested;
                 ++attempt)
            {
                const bool framePresented = PumpOneFrame();
                presented = presented || framePresented;
                if (framePresented)
                    executedFixedSteps += frameGraphTelemetry.ExecutedFixedSteps;
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            visualValidationFixedStepMode = false;
            visualValidationFixedStepsPerFrame = 1;
            Flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

            capture.ActualMode = frameGraphTelemetry.ActualMode;
            capture.FrameIndex = frameGraphTelemetry.FrameResourceIndex;
            if (!presented)
            {
                capture.BlockedReason = "deterministic validation frame was not presented for " +
                    std::string(multiPath ? "Multi" : "Single") + " path";
                return capture;
            }
            if (executedFixedSteps != validationCase.FixedStepCount)
            {
                capture.BlockedReason = "deterministic validation executed " +
                    std::to_string(executedFixedSteps) +
                    " fixed steps, expected " + std::to_string(validationCase.FixedStepCount);
                return capture;
            }
            if (capture.ActualMode != mode)
            {
                capture.BlockedReason = "deterministic validation actual mode " +
                    GetExecutionModeName(capture.ActualMode) + " did not match requested " +
                    GetExecutionModeName(mode);
                return capture;
            }

            GTexture* colorSource = &antiAliasingPrimePath->GetRenderTarget();
            GTexture* secondaryColor = colorSource;
            GTexture* secondaryDepth = &antiAliasingPrimePath->GetDepthMap();
            bool useSecondaryDepth = false;
            if (multiPath)
            {
                if (!multiGpuVoxelRenderTargets.IsInitialized())
                {
                    capture.BlockedReason = "deterministic Multi capture unavailable: multi-GPU render targets are not initialized";
                    return capture;
                }
                auto& frames = multiGpuVoxelRenderTargets.GetFrames();
                if (capture.FrameIndex >= frames.size())
                {
                    capture.BlockedReason = "deterministic Multi capture unavailable: frame target index is out of range";
                    return capture;
                }
                auto& targets = frames[capture.FrameIndex];
                if (!frameGraphTelemetry.CompositeSubmitted || !targets.HasReceivedImage)
                {
                    capture.BlockedReason = "deterministic Multi capture unavailable: final depth-aware composite was not submitted";
                    return capture;
                }
                colorSource = &targets.PrimaryCompositeColor;
                secondaryColor = &targets.PrimaryReceivedSecondaryColor;
                secondaryDepth = &targets.PrimaryReceivedSecondaryLinearDepth;
                useSecondaryDepth = true;
            }

            std::string reason;
            capture.Color = copyTexture(*colorSource, std::wstring(L"VoxelValidation") + label + L"Color", reason);
            if (!reason.empty())
            {
                capture.BlockedReason = reason;
                return capture;
            }
            capture.HardwareDepth = copyTexture(
                antiAliasingPrimePath->GetDepthMap(),
                std::wstring(L"VoxelValidation") + label + L"HardwareDepth",
                reason);
            if (!reason.empty())
            {
                capture.BlockedReason = reason;
                return capture;
            }
            capture.UseSecondaryDepth = useSecondaryDepth;
            if (useSecondaryDepth)
            {
                capture.SecondaryColor = copyTexture(
                    *secondaryColor,
                    std::wstring(L"VoxelValidation") + label + L"SecondaryColor",
                    reason);
                if (!reason.empty())
                {
                    capture.BlockedReason = reason;
                    return capture;
                }
                capture.SecondaryLinearDepth = copyTexture(
                    *secondaryDepth,
                    std::wstring(L"VoxelValidation") + label + L"SecondaryLinearDepth",
                    reason);
                if (!reason.empty())
                {
                    capture.BlockedReason = reason;
                    return capture;
                }
            }
            capture.Available = true;
        }
        catch (const std::exception& ex)
        {
            visualValidationFixedStepMode = false;
            visualValidationFixedStepsPerFrame = 1;
            capture.BlockedReason = "deterministic " + std::string(multiPath ? "Multi" : "Single") +
                " capture failed: " + ex.what();
        }
        catch (const DxException& ex)
        {
            visualValidationFixedStepMode = false;
            visualValidationFixedStepsPerFrame = 1;
            capture.BlockedReason = DxFailureReason(
                multiPath ? "deterministic Multi capture D3D12 failure" : "deterministic Single capture D3D12 failure",
                ex);
        }
        return capture;
    };

    auto single = capturePath(ValidationSide::Reference,
                              validationCase.ReferenceMode,
                              validationCase.ReferenceMode == VoxelExecutionMode::MultiGpuFull ||
                                  validationCase.ReferenceMode == VoxelExecutionMode::MultiGpuTemporalDecimation,
                              L"Reference");
    input.ActualReferenceMode = single.ActualMode;
    if (validationCase.ReferenceMode == validationCase.SingleMode)
        input.ActualSingleMode = single.ActualMode;
    if (!single.Available)
    {
        input.BlockedReason = single.BlockedReason.empty()
                                  ? "deterministic reference capture failed without a detailed reason"
                                  : single.BlockedReason;
        return input;
    }

    auto multi = capturePath(ValidationSide::Candidate,
                             validationCase.CandidateMode,
                             validationCase.CandidateMode == VoxelExecutionMode::MultiGpuFull ||
                                 validationCase.CandidateMode == VoxelExecutionMode::MultiGpuTemporalDecimation,
                             L"Candidate");
    input.ActualCandidateMode = multi.ActualMode;
    if (validationCase.CandidateMode == validationCase.MultiMode)
        input.ActualMultiMode = multi.ActualMode;
    if (!multi.Available)
    {
        input.BlockedReason = multi.BlockedReason.empty()
                                  ? "deterministic candidate capture failed without a detailed reason"
                                  : multi.BlockedReason;
        return input;
    }

    const auto referenceColorDesc = single.Color.GetD3D12ResourceDesc();
    const auto candidateColorDesc = multi.Color.GetD3D12ResourceDesc();
    if (static_cast<uint32_t>(referenceColorDesc.Width) != validationCase.RenderWidth ||
        referenceColorDesc.Height != validationCase.RenderHeight ||
        referenceColorDesc.SampleDesc.Count != validationCase.SampleCount ||
        FormatName(referenceColorDesc.Format) != validationCase.ColorFormat ||
        static_cast<uint32_t>(candidateColorDesc.Width) != validationCase.RenderWidth ||
        candidateColorDesc.Height != validationCase.RenderHeight ||
        candidateColorDesc.SampleDesc.Count != validationCase.SampleCount ||
        FormatName(candidateColorDesc.Format) != validationCase.ColorFormat)
    {
        input.BlockedReason =
            "deterministic validation render target dimensions/formats do not match protocol";
        return input;
    }

    try
    {
        std::string reason;
        single.LinearDepth = createLinearDepth(
            single.HardwareDepth,
            single.Color,
            single.HardwareDepth,
            false,
            L"VoxelValidationSingleLinearDepth",
            reason);
        if (!reason.empty())
        {
            input.BlockedReason = reason;
            return input;
        }

        GTexture& multiSecondaryColor = multi.UseSecondaryDepth ? multi.SecondaryColor : multi.Color;
        GTexture& multiSecondaryDepth = multi.UseSecondaryDepth ? multi.SecondaryLinearDepth : multi.HardwareDepth;
        multi.LinearDepth = createLinearDepth(
            multi.HardwareDepth,
            multiSecondaryColor,
            multiSecondaryDepth,
            multi.UseSecondaryDepth,
            L"VoxelValidationMultiLinearDepth",
            reason);
        if (!reason.empty())
        {
            input.BlockedReason = reason;
            return input;
        }
    }
    catch (const DxException& ex)
    {
        input.BlockedReason = DxFailureReason("deterministic depth linearization D3D12 failure", ex);
        return input;
    }
    catch (const std::exception& ex)
    {
        input.BlockedReason = std::string("deterministic depth linearization failed: ") + ex.what();
        return input;
    }

    try
    {
        const uint32_t width = input.RenderWidth;
        const uint32_t height = input.RenderHeight;
        const uint32_t groupsX = (width + 7u) / 8u;
        const uint32_t groupsY = (height + 7u) / 8u;
        const uint32_t tileCount = groupsX * groupsY;
        const UINT64 statsBytes = static_cast<UINT64>(tileCount) * sizeof(VoxelValidationTileStats);

        GResource tileStats(
            primeDevice,
            CD3DX12_RESOURCE_DESC::Buffer(statsBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            L"VoxelValidationTileStats",
            nullptr,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        GResource statsReadback(
            primeDevice,
            CD3DX12_RESOURCE_DESC::Buffer(statsBytes),
            L"VoxelValidationTileStatsReadback",
            nullptr,
            D3D12_RESOURCE_STATE_COPY_DEST,
            CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK));

        auto diff = CreateValidationTexture(
            primeDevice,
            width,
            height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            L"VoxelValidationDiff");

        CD3DX12_DESCRIPTOR_RANGE srvRange;
        srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0);
        CD3DX12_DESCRIPTOR_RANGE uavRange;
        uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);
        GRootSignature rootSignature;
        rootSignature.AddConstantParameter(sizeof(ValidationCompareConstants) / sizeof(uint32_t), 0);
        rootSignature.AddDescriptorParameter(&srvRange, 1);
        rootSignature.AddDescriptorParameter(&uavRange, 1);
        rootSignature.Initialize(primeDevice, false, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        GShader shader(L"Shaders\\VoxelValidationCompare.hlsl", ComputeShader, nullptr, "CS", "cs_5_1");
        shader.LoadAndCompile();
        ComputePSO pso(rootSignature);
        pso.SetShader(&shader);
        pso.Initialize(primeDevice);

        auto descriptors = primeDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6);
        auto colorSrv = Texture2DSrvDesc(single.Color.GetD3D12ResourceDesc().Format);
        single.Color.CreateShaderResourceView(&colorSrv, &descriptors, 0);
        auto depthSrv = Texture2DSrvDesc(DXGI_FORMAT_R32_FLOAT);
        single.LinearDepth.CreateShaderResourceView(&depthSrv, &descriptors, 1);
        colorSrv = Texture2DSrvDesc(multi.Color.GetD3D12ResourceDesc().Format);
        multi.Color.CreateShaderResourceView(&colorSrv, &descriptors, 2);
        multi.LinearDepth.CreateShaderResourceView(&depthSrv, &descriptors, 3);

        D3D12_UNORDERED_ACCESS_VIEW_DESC statsUav{};
        statsUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        statsUav.Format = DXGI_FORMAT_UNKNOWN;
        statsUav.Buffer.NumElements = tileCount;
        statsUav.Buffer.StructureByteStride = sizeof(VoxelValidationTileStats);
        tileStats.CreateUnorderedAccessView(&statsUav, &descriptors, 4);
        auto diffUav = Texture2DUavDesc(DXGI_FORMAT_R8G8B8A8_UNORM);
        diff.CreateUnorderedAccessView(&diffUav, &descriptors, 5);

        ValidationCompareConstants constants{};
        constants.RenderSize[0] = width;
        constants.RenderSize[1] = height;
        constants.ColorTolerance = static_cast<float>(config.Tolerances.ColorTolerance);
        constants.DepthTolerance = static_cast<float>(config.Tolerances.DepthTolerance);
        constants.ValidDepthMax = static_cast<float>(config.Tolerances.ValidDepthMax);

        const auto cmdList = computeQueue->GetCommandList();
        cmdList->SetPipelineState(pso);
        cmdList->SetDescriptorsHeap(&descriptors);
        cmdList->TransitionBarrier(single.Color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->TransitionBarrier(single.LinearDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->TransitionBarrier(multi.Color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->TransitionBarrier(multi.LinearDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->TransitionBarrier(tileStats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->TransitionBarrier(diff, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->FlushResourceBarriers();
        cmdList->SetRoot32BitConstants(0, sizeof(ValidationCompareConstants) / sizeof(uint32_t), &constants, 0);
        cmdList->SetRootDescriptorTable(1, &descriptors, 0);
        cmdList->SetRootDescriptorTable(2, &descriptors, 4);
        cmdList->Dispatch(groupsX, groupsY, 1);
        cmdList->UAVBarrier(tileStats);
        cmdList->UAVBarrier(diff);
        cmdList->CopyBufferRegion(
            statsReadback.GetD3D12Resource(),
            0,
            tileStats.GetD3D12Resource(),
            0,
            static_cast<UINT>(statsBytes),
            true);
        const auto fence = computeQueue->ExecuteCommandList(cmdList);
        computeQueue->WaitForFenceValue(fence);

        void* mapped = nullptr;
        const D3D12_RANGE readRange{0, static_cast<SIZE_T>(statsBytes)};
        HRESULT hr = statsReadback.GetD3D12Resource()->Map(0, &readRange, &mapped);
        if (FAILED(hr) || !mapped)
        {
            input.BlockedReason = "validation tile statistics readback Map failed with " + HResultToHex(hr);
            return input;
        }
        input.TileStats.resize(tileCount);
        std::memcpy(input.TileStats.data(), mapped, static_cast<size_t>(statsBytes));
        const D3D12_RANGE emptyRange{0, 0};
        statsReadback.GetD3D12Resource()->Unmap(0, &emptyRange);

        input.CaptureAvailable = true;
        input.CompareShaderDispatched = true;
        input.ReadbackComplete = true;

        std::string exportReason;
        if (!ExportTextureRaw(primeDevice, graphicsQueue, single.Color, input.SingleColorReferencePath, exportReason) ||
            !ExportTextureRaw(primeDevice, graphicsQueue, single.LinearDepth, input.SingleDepthReferencePath, exportReason) ||
            !ExportTextureRaw(primeDevice, graphicsQueue, multi.Color, input.MultiColorReferencePath, exportReason) ||
            !ExportTextureRaw(primeDevice, graphicsQueue, multi.LinearDepth, input.MultiDepthReferencePath, exportReason) ||
            !ExportTextureRaw(primeDevice, graphicsQueue, diff, input.DiffReferencePath, exportReason))
        {
            input.BlockedReason = exportReason;
            input.CaptureAvailable = false;
            return input;
        }
    }
    catch (const std::exception& ex)
    {
        input.BlockedReason = std::string("VoxelValidationCompare.hlsl dispatch/readback failed: ") + ex.what();
        input.CompareShaderDispatched = false;
        input.ReadbackComplete = false;
    }
    catch (const DxException& ex)
    {
        input.BlockedReason = DxFailureReason("VoxelValidationCompare.hlsl dispatch/readback D3D12 failure", ex);
        input.CompareShaderDispatched = false;
        input.ReadbackComplete = false;
    }

    return input;
}

void VoxelWaterfallApp::RunVisualValidation(const std::filesystem::path& requestedOutputDirectory,
                                            const BenchmarkSuite suite,
                                            const uint32_t seedOverride)
{
    if (isPumpingFrame || isDrawingFrame)
    {
        ++rejectedRecursiveFrameRequests;
        assert(false && "RunVisualValidation must be started outside PumpOneFrame/DrawFrame");
        visualValidationMetrics = {};
        visualValidationMetrics.HasResult = false;
        visualValidationMetrics.Passed = false;
        visualValidationMetrics.FailReason =
            "RunVisualValidation rejected because it was started during an active frame pump";
        return;
    }

    VoxelVisualValidationConfig config{};
    auto& snapshot = config.Snapshot;
    snapshot.SchemaVersion = 2;
    snapshot.WorkloadProfile = "MixedStaticAndDynamic";
    snapshot.ScenePreset = "MixedVoxelEnvironment";
    snapshot.FixedDeltaTime = 1.0 / 60.0;
    snapshot.WarmupStepCount = 0;
    snapshot.BuildHash = benchmarkProvenanceCache.BuildHash;
    snapshot.ShaderHash = benchmarkProvenanceCache.ShaderHash;
    snapshot.ShaderSetHash = benchmarkProvenanceCache.ShaderSetHash;
    snapshot.AdapterPairIdentity = twoAdapterVerificationHasResult
                                       ? twoAdapterVerificationResult.AdapterPairIdentity
                                       : AdapterLuidPair(benchmarkProvenanceCache.PrimaryAdapterLuid,
                                                         benchmarkProvenanceCache.SecondaryAdapterLuid);
    snapshot.PrimaryDriverVersion =
        AdapterDriverIdentity(benchmarkProvenanceCache.PrimaryAdapterLuid,
                              benchmarkProvenanceCache.PrimaryVendorId,
                              benchmarkProvenanceCache.PrimaryDeviceId);
    snapshot.SecondaryDriverVersion =
        AdapterDriverIdentity(benchmarkProvenanceCache.SecondaryAdapterLuid,
                              benchmarkProvenanceCache.SecondaryVendorId,
                              benchmarkProvenanceCache.SecondaryDeviceId);
    visualValidationBuildHash = snapshot.BuildHash;
    visualValidationShaderHash = snapshot.ShaderSetHash;
    visualValidationAdapterPairIdentity = snapshot.AdapterPairIdentity;
    config.Cases = BuildVisualValidationCasesForSuite(suite, seedOverride);

    const auto fillRuntimeSnapshot = [&](VoxelVisualValidationSnapshot& target,
                                         const VoxelVisualValidationCase& validationCase,
                                         const BenchmarkConfigurationApplyResult& resolved)
    {
        target.StaticSeed = voxelWorkload.StaticGenerationSeed;
        target.DynamicSeed = voxelWorkload.Parameters.Seed;
        target.RequestedStaticCount = resolved.RequestedStaticBudget;
        target.RequestedDynamicCount = resolved.RequestedDynamicBudget;
        target.ActualStaticCount = resolved.ActualStaticCount;
        target.ActualDynamicCount = resolved.ActualDynamicCount;
        target.TotalVoxelCount = resolved.ActualTotalCount;
        target.StaticVoxelSize = voxelWorkload.StaticVoxelSize;
        target.DynamicVoxelSize = voxelWorkload.Parameters.VoxelSize;
        if (antiAliasingPrimePath)
        {
            const auto colorDesc = antiAliasingPrimePath->GetRenderTarget().GetD3D12ResourceDesc();
            const auto depthDesc = antiAliasingPrimePath->GetDepthMap().GetD3D12ResourceDesc();
            target.RenderWidth = static_cast<uint32_t>(colorDesc.Width);
            target.RenderHeight = colorDesc.Height;
            target.SampleCount = colorDesc.SampleDesc.Count;
            target.ColorFormat = FormatName(colorDesc.Format);
            target.LinearDepthFormat = FormatName(DXGI_FORMAT_R32_FLOAT);
            if (depthDesc.SampleDesc.Count != target.SampleCount)
                target.SampleCount = 0;
        }
        target.FixedStepCount = validationCase.FixedStepCount;
        target.SpatialLod = voxelWorkload.SpatialLod;
        target.TemporalPolicy = voxelWorkload.TemporalPolicy;
        target.TemporalInterval = voxelWorkload.TemporalDecimationInterval;
        target.SecondaryShare = voxelWorkload.SecondaryShare;
        target.PartitionStrategy = voxelWorkload.PartitionStrategy;
        target.LoadBalanceScenario = voxelWorkload.LoadBalanceScenario;
        target.ChunkSize = voxelWorkload.ChunkSize;
        target.RequestedExecutionMode = validationCase.CandidateMode;
        target.NearZ = camera ? camera->GetNearZ() : 0.0f;
        target.FarZ = camera ? camera->GetFarZ() : 0.0f;
        target.LightingPreset = LightingPresetName(voxelWorkload.LightingMode);
        target.DynamicShadowsEnabled = voxelWorkload.DynamicShadowsEnabled;
        target.Background = "BenchmarkNeutral";
        if (camera)
        {
            StoreMatrix(camera->GetViewMatrix(), target.View);
            StoreMatrix(camera->GetProjectionMatrix(), target.Projection);
        }
    };

    for (auto& validationCase : config.Cases)
    {
        ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile::MixedStaticAndDynamic);
        ApplyRenderResolutionPreset(VoxelRenderResolutionPreset::R1920x1080);
        ApplyResearchLightingPreset(VoxelResearchLightingPreset::BenchmarkNeutral);
        ApplyResearchCameraMode(CameraModeFromName(validationCase.CameraMode));
        auto referenceConfig = ConfigForValidationMode(
            validationCase,
            validationCase.ReferenceMode,
            ValidationSide::Reference);
        const auto referenceResolved = ApplyBenchmarkConfigurationAtomic(referenceConfig);
        if (!referenceResolved.Passed)
            validationCase.ReferenceConfigHash = "blocked:" + referenceResolved.Reason;
        else
            validationCase.ReferenceConfigHash = referenceResolved.ResolvedConfigHash;

        auto candidateConfig = ConfigForValidationMode(
            validationCase,
            validationCase.CandidateMode,
            ValidationSide::Candidate);
        const auto candidateResolved = ApplyBenchmarkConfigurationAtomic(candidateConfig);
        validationCase.CandidateConfigHash =
            candidateResolved.Passed ? candidateResolved.ResolvedConfigHash : "blocked:" + candidateResolved.Reason;
        validationCase.ConfigHash = validationCase.CandidateConfigHash;
        if (validationCase.ValidationKind == "approximation_fidelity" &&
            (validationCase.SpatialLodEnabled || validationCase.TemporalInterval > 1u))
        {
            if (referenceResolved.Passed && candidateResolved.Passed &&
                validationCase.ReferenceConfigHash == validationCase.CandidateConfigHash)
            {
                validationCase.CandidateConfigHash =
                    "blocked: approximation fidelity reference/candidate resolved hashes are identical";
                validationCase.ConfigHash = validationCase.CandidateConfigHash;
            }
            if (referenceConfig.SpatialLodEnabled || referenceConfig.TemporalInterval != 1u)
            {
                validationCase.ReferenceConfigHash =
                    "blocked: approximation fidelity reference is not LOD-off temporal interval 1";
            }
            if (candidateConfig.SpatialLodEnabled != validationCase.SpatialLodEnabled ||
                candidateConfig.TemporalInterval != validationCase.TemporalInterval)
            {
                validationCase.CandidateConfigHash =
                    "blocked: approximation fidelity candidate does not contain requested approximation policy";
                validationCase.ConfigHash = validationCase.CandidateConfigHash;
            }
        }
        validationCase.ActualStaticCount = candidateResolved.ActualStaticCount;
        validationCase.ActualDynamicCount = candidateResolved.ActualDynamicCount;
        validationCase.ActualTotalCount = candidateResolved.ActualTotalCount;

        VoxelVisualValidationSnapshot caseSnapshot = snapshot;
        fillRuntimeSnapshot(caseSnapshot, validationCase, candidateResolved);
        validationCase.RenderWidth = caseSnapshot.RenderWidth;
        validationCase.RenderHeight = caseSnapshot.RenderHeight;
        validationCase.SampleCount = caseSnapshot.SampleCount;
        validationCase.ColorFormat = caseSnapshot.ColorFormat;
        validationCase.LinearDepthFormat = caseSnapshot.LinearDepthFormat;
        validationCase.NearZ = caseSnapshot.NearZ;
        validationCase.FarZ = caseSnapshot.FarZ;
        std::memcpy(validationCase.View, caseSnapshot.View, sizeof(validationCase.View));
        std::memcpy(validationCase.Projection, caseSnapshot.Projection, sizeof(validationCase.Projection));
        validationCase.CameraHash = BuildValidationCameraHash(caseSnapshot);
        if (snapshot.RenderWidth == 0)
            snapshot = caseSnapshot;
    }

    visualValidationProtocolHash = BuildValidationProtocolHash(config);
    for (auto& validationCase : config.Cases)
        validationCase.ProtocolHash = visualValidationProtocolHash;
    visualValidationCaseConfigHash = BuildValidationCaseSetHash(config.Cases, snapshot);
    {
        std::ostringstream cameraSet;
        cameraSet << "camera_set=mgpu_voxel_visual_validation_cameras.v2\n";
        for (const auto& validationCase : config.Cases)
            cameraSet << validationCase.CaseId << "=" << validationCase.CameraHash << "\n";
        visualValidationCameraHash = ResearchProvenance::Sha256Hex(cameraSet.str());
    }
    snapshot.ProtocolHash = visualValidationProtocolHash;
    snapshot.CaseConfigHash = visualValidationCaseConfigHash;
    snapshot.CameraHash = visualValidationCameraHash;
    snapshot.ValidationRunId = std::string("visual_") +
        visualValidationProtocolHash.substr(0, 16) + "_" +
        FilesystemTimestampToken() + "_" + std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(reinterpret_cast<uintptr_t>(this));

    const auto outputDirectory = requestedOutputDirectory.empty()
                                     ? GetExecutableDirectory() / "VoxelValidation"
                                     : requestedOutputDirectory;
    const auto exportBlockedResults = [&](const std::string& reason)
    {
        for (const auto& validationCase : config.Cases)
        {
            const auto existing = std::find_if(
                config.CompletedComparisons.begin(),
                config.CompletedComparisons.end(),
                [&](const VoxelVisualValidationComparisonInput& comparison)
                {
                    return comparison.CaseId == validationCase.CaseId;
                });
            if (existing != config.CompletedComparisons.end())
                continue;

            VoxelVisualValidationComparisonInput blocked{};
            blocked.CaseId = validationCase.CaseId;
            blocked.ActualSingleMode = validationCase.SingleMode;
            blocked.ActualMultiMode = validationCase.MultiMode;
            blocked.RequestedReferenceMode = validationCase.ReferenceMode;
            blocked.RequestedCandidateMode = validationCase.CandidateMode;
            blocked.RenderWidth = validationCase.RenderWidth;
            blocked.RenderHeight = validationCase.RenderHeight;
            blocked.SampleCount = validationCase.SampleCount;
            blocked.ColorFormat = validationCase.ColorFormat;
            blocked.LinearDepthFormat = validationCase.LinearDepthFormat;
            blocked.ValidationKind = validationCase.ValidationKind;
            blocked.CheckpointId = validationCase.CheckpointId;
            blocked.ProtocolHash = validationCase.ProtocolHash;
            blocked.ReferenceConfigHash = validationCase.ReferenceConfigHash;
            blocked.CandidateConfigHash = validationCase.CandidateConfigHash;
            blocked.ConfigHash = validationCase.ConfigHash;
            blocked.CameraHash = validationCase.CameraHash;
            blocked.AdapterPairIdentity = visualValidationAdapterPairIdentity;
            blocked.BlockedReason =
                "deterministic GPU validation capture could not complete: " + reason;
            config.CompletedComparisons.push_back(std::move(blocked));
        }

        visualValidationMetrics =
            visualValidationRunner.RunDeterministicSuite(config, outputDirectory);
        visualValidationJsonPath = visualValidationMetrics.JsonPath;
        visualValidationCsvPath = visualValidationMetrics.CsvPath;
    };

    try
    {
        config.CompletedComparisons.clear();
        config.CompletedComparisons.reserve(config.Cases.size());
        for (size_t caseIndex = 0; caseIndex < config.Cases.size(); ++caseIndex)
        {
            const auto& validationCase = config.Cases[caseIndex];
            auto comparison = CaptureVisualValidationCase(config, validationCase, outputDirectory);
            const bool blocked = !comparison.BlockedReason.empty() ||
                !comparison.CaptureAvailable ||
                !comparison.CompareShaderDispatched ||
                !comparison.ReadbackComplete ||
                comparison.TileStats.empty();
            const std::string abortReason = comparison.BlockedReason.empty()
                                                ? "deterministic GPU validation capture did not produce complete compare evidence"
                                                : comparison.BlockedReason;
            config.CompletedComparisons.push_back(std::move(comparison));

            if (blocked)
            {
                for (size_t remainingIndex = caseIndex + 1; remainingIndex < config.Cases.size(); ++remainingIndex)
                {
                    const auto& remainingCase = config.Cases[remainingIndex];
                    VoxelVisualValidationComparisonInput remaining{};
                    remaining.CaseId = remainingCase.CaseId;
                    remaining.ActualSingleMode = remainingCase.SingleMode;
                    remaining.ActualMultiMode = remainingCase.MultiMode;
                    remaining.RequestedReferenceMode = remainingCase.ReferenceMode;
                    remaining.RequestedCandidateMode = remainingCase.CandidateMode;
                    remaining.RenderWidth = remainingCase.RenderWidth;
                    remaining.RenderHeight = remainingCase.RenderHeight;
                    remaining.SampleCount = remainingCase.SampleCount;
                    remaining.ColorFormat = remainingCase.ColorFormat;
                    remaining.LinearDepthFormat = remainingCase.LinearDepthFormat;
                    remaining.ValidationKind = remainingCase.ValidationKind;
                    remaining.CheckpointId = remainingCase.CheckpointId;
                    remaining.ProtocolHash = remainingCase.ProtocolHash;
                    remaining.ReferenceConfigHash = remainingCase.ReferenceConfigHash;
                    remaining.CandidateConfigHash = remainingCase.CandidateConfigHash;
                    remaining.ConfigHash = remainingCase.ConfigHash;
                    remaining.CameraHash = remainingCase.CameraHash;
                    remaining.AdapterPairIdentity = visualValidationAdapterPairIdentity;
                    remaining.SingleColorReferencePath = RawPath(outputDirectory, remainingCase.CaseId, "single_color");
                    remaining.SingleDepthReferencePath = RawPath(outputDirectory, remainingCase.CaseId, "single_depth");
                    remaining.MultiColorReferencePath = RawPath(outputDirectory, remainingCase.CaseId, "multi_color");
                    remaining.MultiDepthReferencePath = RawPath(outputDirectory, remainingCase.CaseId, "multi_depth");
                    remaining.DiffReferencePath = RawPath(outputDirectory, remainingCase.CaseId, "diff");
                    remaining.BlockedReason =
                        "deterministic validation aborted after prior capture failure in case " +
                        validationCase.CaseId + ": " + abortReason;
                    config.CompletedComparisons.push_back(std::move(remaining));
                }
                break;
            }
        }

        visualValidationMetrics =
            visualValidationRunner.RunDeterministicSuite(config, outputDirectory);
        visualValidationJsonPath = visualValidationMetrics.JsonPath;
        visualValidationCsvPath = visualValidationMetrics.CsvPath;
        logQueue.Push(L"Visual validation exported to " +
                      visualValidationMetrics.CsvPath.wstring());
    }
    catch (const DxException& ex)
    {
        try
        {
            exportBlockedResults(DxFailureReason("D3D12 failure during deterministic validation capture", ex));
            logQueue.Push(L"Visual validation exported as BLOCKED to " +
                          visualValidationMetrics.CsvPath.wstring());
        }
        catch (...)
        {
            visualValidationMetrics = {};
            visualValidationMetrics.HasResult = false;
            visualValidationMetrics.Passed = false;
            visualValidationMetrics.FailReason = DxFailureReason(
                "failed to export blocked deterministic validation artifacts after D3D12 failure",
                ex);
            logQueue.Push(L"Visual validation failed to export blocked results");
        }
    }
    catch (const std::exception& ex)
    {
        try
        {
            exportBlockedResults(ex.what());
            logQueue.Push(L"Visual validation exported as BLOCKED to " +
                          visualValidationMetrics.CsvPath.wstring());
        }
        catch (...)
        {
            visualValidationMetrics = {};
            visualValidationMetrics.HasResult = false;
            visualValidationMetrics.Passed = false;
            visualValidationMetrics.FailReason =
                std::string("failed to export blocked deterministic validation artifacts: ") + ex.what();
            logQueue.Push(L"Visual validation failed to export blocked results");
        }
    }
}

int VoxelWaterfallApp::RunValidationSuiteOnce(const std::filesystem::path& outputDirectory)
{
    RunVisualValidation(outputDirectory);
    if (!visualValidationMetrics.HasResult)
        return 3;
    return visualValidationMetrics.Passed ? 0 : 2;
}

int VoxelWaterfallApp::RunTwoAdapterVerificationOnce(const std::filesystem::path& requestedOutputDirectory)
{
    const auto outputDirectory = requestedOutputDirectory.empty()
                                     ? GetExecutableDirectory() / L"TwoAdapterVerification"
                                     : requestedOutputDirectory;
    const auto buildHash = benchmarkProvenanceCache.BuildHash;
    const auto allDevices = GDeviceFactory::GetAllDevices(false);
    const auto selectedDevices = DeviceSelectionPolicy::Select(allDevices);

    auto result = twoAdapterVerificationRunner.RunPreflight(
        allDevices,
        selectedDevices,
        buildHash,
        outputDirectory);
    twoAdapterVerificationResult = result;
    twoAdapterVerificationHasResult = true;
    twoAdapterVerificationJsonPath = result.JsonPath;

    const bool preflightPassed =
        result.Status == TwoAdapterVerificationStatus::Pass ||
        result.Status == TwoAdapterVerificationStatus::PassHardwareDirect ||
        result.Status == TwoAdapterVerificationStatus::PassHardwareCopyOnly;
    if (!preflightPassed)
    {
        logQueue.Push(L"Two-GPU runtime verification preflight did not pass; runtime proof was not attempted");
        return result.Status == TwoAdapterVerificationStatus::Blocked ? 3 : 2;
    }

    ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile::MixedStaticAndDynamic);
    ApplyResearchLightingPreset(VoxelResearchLightingPreset::BenchmarkNeutral);
    AutomaticBenchmarkConfig verificationConfig{};
    verificationConfig.Suite = BenchmarkSuite::Smoke;
    verificationConfig.Mode = VoxelExecutionMode::MultiGpuFull;
    verificationConfig.ModeName = "MultiGpuFull";
    verificationConfig.Preset = "TwoAdapterRuntime";
    verificationConfig.RequestedLabelCount = 100000;
    verificationConfig.RequestedStaticBudget = 100000;
    verificationConfig.RequestedDynamicBudget = 25000;
    verificationConfig.TotalCount = 100000;
    verificationConfig.SecondaryShare = 0.5f;
    verificationConfig.SpatialLodEnabled = false;
    verificationConfig.TemporalInterval = 1;
    verificationConfig.ConfigId = "TwoAdapterRuntime:MultiGpuFull:share50:lod_off:temporal1";
    const auto resolvedVerificationConfig = ApplyBenchmarkConfigurationAtomic(verificationConfig);
    if (!resolvedVerificationConfig.Passed)
    {
        result.Status = TwoAdapterVerificationStatus::Fail;
        result.Reasons.push_back("two-adapter runtime verification config did not resolve: " +
                                 resolvedVerificationConfig.Reason);
        twoAdapterVerificationRunner.ExportRuntimeEvidence(result, TwoAdapterRuntimeEvidence{});
        twoAdapterVerificationResult = result;
        twoAdapterVerificationHasResult = true;
        twoAdapterVerificationJsonPath = result.JsonPath;
        return 2;
    }

    const std::string verificationProtocolHash = ResearchProvenance::Sha256Hex(
        "mgpu_two_adapter_runtime_protocol.v2\nbuild=" + benchmarkProvenanceCache.BuildHash +
        "\nshader=" + benchmarkProvenanceCache.ShaderSetHash +
        "\nconfig=" + resolvedVerificationConfig.ResolvedConfigHash +
        "\nadapter_pair=" + result.AdapterPairIdentity +
        "\nmode=MultiGpuFull\n");
    result.VerificationRunId =
        "two_adapter_" + verificationProtocolHash.substr(0, 16) + "_" +
        resolvedVerificationConfig.ResolvedConfigHash.substr(0, 16) + "_" +
        FilesystemTimestampToken() + "_" + std::to_string(GetCurrentProcessId());

    auto* timerPtr = GetTimer();
    timerPtr->Reset();
    pumpFrameQuitRequested = false;

    TwoAdapterRuntimeEvidence runtime{};
    runtime.Attempted = true;
    runtime.FrameConfigId = verificationConfig.ConfigId;
    runtime.ProtocolHash = verificationProtocolHash;
    runtime.ConfigHash = resolvedVerificationConfig.ResolvedConfigHash;
    runtime.BuildHash = benchmarkProvenanceCache.BuildHash;
    runtime.ShaderHash = benchmarkProvenanceCache.ShaderSetHash;
    runtime.PrimaryDriverVersion =
        AdapterDriverIdentity(benchmarkProvenanceCache.PrimaryAdapterLuid,
                              benchmarkProvenanceCache.PrimaryVendorId,
                              benchmarkProvenanceCache.PrimaryDeviceId);
    runtime.SecondaryDriverVersion =
        AdapterDriverIdentity(benchmarkProvenanceCache.SecondaryAdapterLuid,
                              benchmarkProvenanceCache.SecondaryVendorId,
                              benchmarkProvenanceCache.SecondaryDeviceId);
    if (antiAliasingPrimePath)
    {
        const auto desc = antiAliasingPrimePath->GetRenderTarget().GetD3D12ResourceDesc();
        runtime.RenderWidth = static_cast<uint32_t>(desc.Width);
        runtime.RenderHeight = desc.Height;
        runtime.ColorFormat = FormatName(desc.Format);
        runtime.DepthFormat = "R32_FLOAT";
    }
    runtime.RequestedMode = VoxelExecutionMode::MultiGpuFull;
    runtime.ActualMode = VoxelExecutionMode::SingleGpuFull;
    runtime.TransferMode = crossAdapterTransferMode;

    for (const auto& partition : voxelWorkload.Partitions)
    {
        if (partition.AdapterOwner == VoxelAdapterOwner::Secondary)
        {
            runtime.SecondaryPartitionVoxels += partition.VoxelCount();
        }
        else
        {
            runtime.PrimaryPartitionVoxels += partition.VoxelCount();
        }
    }

    const auto isActualMultiMode = [](const VoxelExecutionMode mode)
    {
        return mode == VoxelExecutionMode::MultiGpuFull ||
            mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    };
    const auto accumulateFrameEvidence = [&runtime, &isActualMultiMode, this]()
    {
        ++runtime.FramesObserved;
        runtime.ActualMode = frameGraphTelemetry.ActualMode;
        runtime.AnyActualMultiMode =
            runtime.AnyActualMultiMode || isActualMultiMode(frameGraphTelemetry.ActualMode);
        if (frameGraphTelemetry.SecondarySimulationDispatchedThisFrame ||
            frameGraphTelemetry.SecondaryComputeSubmitted)
        {
            ++runtime.TotalSecondaryComputeDispatchCount;
        }
        runtime.MaxSecondaryGraphicsDrawCount =
            std::max(runtime.MaxSecondaryGraphicsDrawCount, frameGraphTelemetry.SecondaryDrawCalls);
        runtime.MaxSecondaryRenderedVoxelCount =
            std::max(runtime.MaxSecondaryRenderedVoxelCount, frameGraphTelemetry.SecondaryRenderedVoxelCount);
        runtime.SecondaryIndirectDrawCount =
            std::max(runtime.SecondaryIndirectDrawCount, frameGraphTelemetry.SecondaryIndirectDrawCalls);
        runtime.SecondaryGraphicsCommandListSubmissionCount +=
            frameGraphTelemetry.SecondaryGraphicsCommandListSubmissionCount;
        runtime.LocalToSharedCommandListSubmissionCount +=
            frameGraphTelemetry.LocalToSharedCommandListSubmissionCount;
        runtime.SharedToLocalCommandListSubmissionCount +=
            frameGraphTelemetry.SharedToLocalCommandListSubmissionCount;
        runtime.TotalColorTransferBytes += frameGraphTelemetry.ColorBytesTransferred;
        runtime.TotalDepthTransferBytes += frameGraphTelemetry.DepthBytesTransferred;
        runtime.ExpectedColorLocalToSharedBytes += frameGraphTelemetry.ExpectedLocalToSharedColorBytes;
        runtime.ExpectedDepthLocalToSharedBytes += frameGraphTelemetry.ExpectedLocalToSharedDepthBytes;
        runtime.ExpectedColorSharedToLocalBytes += frameGraphTelemetry.ExpectedSharedToLocalColorBytes;
        runtime.ExpectedDepthSharedToLocalBytes += frameGraphTelemetry.ExpectedSharedToLocalDepthBytes;
        runtime.ColorLocalToSharedBytes += frameGraphTelemetry.LocalToSharedColorBytes;
        runtime.DepthLocalToSharedBytes += frameGraphTelemetry.LocalToSharedDepthBytes;
        runtime.ColorSharedToLocalBytes += frameGraphTelemetry.SharedToLocalColorBytes;
        runtime.DepthSharedToLocalBytes += frameGraphTelemetry.SharedToLocalDepthBytes;
        if (!frameGraphTelemetry.LocalToSharedColorSource.empty())
        {
            runtime.ColorLocalToSharedSource = frameGraphTelemetry.LocalToSharedColorSource;
            runtime.ColorLocalToSharedDestination = frameGraphTelemetry.LocalToSharedColorDestination;
            runtime.DepthLocalToSharedSource = frameGraphTelemetry.LocalToSharedDepthSource;
            runtime.DepthLocalToSharedDestination = frameGraphTelemetry.LocalToSharedDepthDestination;
        }
        if (!frameGraphTelemetry.SharedToLocalColorSource.empty())
        {
            runtime.ColorSharedToLocalSource = frameGraphTelemetry.SharedToLocalColorSource;
            runtime.ColorSharedToLocalDestination = frameGraphTelemetry.SharedToLocalColorDestination;
            runtime.DepthSharedToLocalSource = frameGraphTelemetry.SharedToLocalDepthSource;
            runtime.DepthSharedToLocalDestination = frameGraphTelemetry.SharedToLocalDepthDestination;
        }
        runtime.CopyOperation = frameGraphTelemetry.CopyOperation;
        runtime.LocalToSharedPath = frameGraphTelemetry.LocalToSharedPath;
        runtime.SharedToLocalPath = frameGraphTelemetry.SharedToLocalPath;
        runtime.BridgeResourceDimension = frameGraphTelemetry.BridgeResourceDimension;
        runtime.BridgeColorBytes = std::max(runtime.BridgeColorBytes, frameGraphTelemetry.BridgeColorBytes);
        runtime.BridgeDepthBytes = std::max(runtime.BridgeDepthBytes, frameGraphTelemetry.BridgeDepthBytes);
        runtime.BridgeColorRowPitch = std::max(runtime.BridgeColorRowPitch, frameGraphTelemetry.BridgeColorRowPitch);
        runtime.BridgeDepthRowPitch = std::max(runtime.BridgeDepthRowPitch, frameGraphTelemetry.BridgeDepthRowPitch);
        runtime.SecondaryIndirectArgumentMaxCommandCount =
            std::max(runtime.SecondaryIndirectArgumentMaxCommandCount,
                     frameGraphTelemetry.SecondaryIndirectArgumentMaxCommandCount);
        runtime.SecondaryIndirectArgumentResolvedDrawCount =
            std::max(runtime.SecondaryIndirectArgumentResolvedDrawCount,
                     frameGraphTelemetry.SecondaryIndirectArgumentResolvedDrawCount);
        runtime.SecondaryGraphicsTimestampBeginQuery =
            std::max(runtime.SecondaryGraphicsTimestampBeginQuery,
                     frameGraphTelemetry.SecondaryGraphicsTimestampBeginQuery);
        runtime.SecondaryGraphicsTimestampEndQuery =
            std::max(runtime.SecondaryGraphicsTimestampEndQuery,
                     frameGraphTelemetry.SecondaryGraphicsTimestampEndQuery);
        runtime.LocalToSharedTimestampBeginQuery =
            std::max(runtime.LocalToSharedTimestampBeginQuery,
                     frameGraphTelemetry.LocalToSharedTimestampBeginQuery);
        runtime.LocalToSharedTimestampEndQuery =
            std::max(runtime.LocalToSharedTimestampEndQuery,
                     frameGraphTelemetry.LocalToSharedTimestampEndQuery);
        runtime.SharedToLocalTimestampBeginQuery =
            std::max(runtime.SharedToLocalTimestampBeginQuery,
                     frameGraphTelemetry.SharedToLocalTimestampBeginQuery);
        runtime.SharedToLocalTimestampEndQuery =
            std::max(runtime.SharedToLocalTimestampEndQuery,
                     frameGraphTelemetry.SharedToLocalTimestampEndQuery);
        runtime.ParticleTransferBytes += frameGraphTelemetry.ParticleTransferBytes;
        runtime.AnyCompositeSubmitted =
            runtime.AnyCompositeSubmitted || frameGraphTelemetry.CompositeSubmitted;

        runtime.SecondaryComputeFenceValue =
            std::max<uint64_t>(runtime.SecondaryComputeFenceValue,
                               frameGraphTelemetry.SecondaryComputeFenceValue);
        runtime.SecondaryGraphicsFenceValue =
            std::max<uint64_t>(runtime.SecondaryGraphicsFenceValue,
                               frameGraphTelemetry.SecondaryGraphicsFenceValue);
        runtime.SecondaryLocalToSharedCopyFenceValue =
            std::max<uint64_t>(runtime.SecondaryLocalToSharedCopyFenceValue,
                               frameGraphTelemetry.SecondaryLocalToSharedCopyFenceValue);
        runtime.CrossAdapterRenderReadyFenceValue =
            std::max<uint64_t>(runtime.CrossAdapterRenderReadyFenceValue,
                               frameGraphTelemetry.CrossAdapterRenderReadyFenceValue);
        runtime.PrimarySharedToLocalCopyFenceValue =
            std::max<uint64_t>(runtime.PrimarySharedToLocalCopyFenceValue,
                               frameGraphTelemetry.PrimarySharedToLocalCopyFenceValue);
        runtime.PrimarySecondaryImageReadyFenceValue =
            std::max<uint64_t>(runtime.PrimarySecondaryImageReadyFenceValue,
                               frameGraphTelemetry.PrimarySecondaryImageReadyFenceValue);
        runtime.FinalPresentFenceValue =
            std::max<uint64_t>(runtime.FinalPresentFenceValue,
                               frameGraphTelemetry.FinalPresentFenceValue);

        runtime.AnySecondaryComputeFenceValue =
            runtime.AnySecondaryComputeFenceValue || frameGraphTelemetry.SecondaryComputeFenceValue != 0;
        runtime.AnySecondaryGraphicsFenceValue =
            runtime.AnySecondaryGraphicsFenceValue || frameGraphTelemetry.SecondaryGraphicsFenceValue != 0;
        runtime.AnySecondaryLocalToSharedCopyFenceValue =
            runtime.AnySecondaryLocalToSharedCopyFenceValue ||
            frameGraphTelemetry.SecondaryLocalToSharedCopyFenceValue != 0;
        runtime.AnyCrossAdapterRenderReadyFenceValue =
            runtime.AnyCrossAdapterRenderReadyFenceValue ||
            frameGraphTelemetry.CrossAdapterRenderReadyFenceValue != 0;
        runtime.AnyPrimarySharedToLocalCopyFenceValue =
            runtime.AnyPrimarySharedToLocalCopyFenceValue ||
            frameGraphTelemetry.PrimarySharedToLocalCopyFenceValue != 0;
        runtime.AnyPrimarySecondaryImageReadyFenceValue =
            runtime.AnyPrimarySecondaryImageReadyFenceValue ||
            frameGraphTelemetry.PrimarySecondaryImageReadyFenceValue != 0;
        runtime.AnyFinalPresentFenceValue =
            runtime.AnyFinalPresentFenceValue || frameGraphTelemetry.FinalPresentFenceValue != 0;
    };

    constexpr uint32_t verificationPresentFrames = 12;
    constexpr uint32_t maxVerificationPumpAttempts = verificationPresentFrames * 2000;
    const auto targetPresentCount = totalSuccessfulPresentCount + verificationPresentFrames;
    uint32_t verificationPumpAttempts = 0;
    while (totalSuccessfulPresentCount < targetPresentCount &&
           verificationPumpAttempts < maxVerificationPumpAttempts &&
           !pumpFrameQuitRequested)
    {
        ++verificationPumpAttempts;
        AdvanceRuntimeWorkOutsideFrame(false);
        const bool presented = PumpOneFrame();
        AdvanceRuntimeWorkOutsideFrame(presented);
        if (presented)
            accumulateFrameEvidence();
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (totalSuccessfulPresentCount < targetPresentCount)
    {
        runtime.Reasons.push_back("two-adapter runtime verification did not reach target presented frame count");
        runtime.Passed = false;
        skipDestructorGpuFlush = true;
        twoAdapterVerificationRunner.ExportRuntimeEvidence(result, runtime);
        twoAdapterVerificationResult = result;
        twoAdapterVerificationHasResult = true;
        twoAdapterVerificationJsonPath = result.JsonPath;
        logQueue.Push(L"Two-GPU runtime verification FAIL");
        return 2;
    }
    Flush();

    for (const auto& frameTargets : multiGpuVoxelRenderTargets.GetFrames())
    {
        const auto stats = ReadCompletedSecondaryPipelineStatistics(frameTargets);
        if (!stats)
            continue;

        runtime.SecondaryPipelineIAPrimitives =
            std::max(runtime.SecondaryPipelineIAPrimitives, stats->IAPrimitives);
        runtime.SecondaryPipelineVSInvocations =
            std::max(runtime.SecondaryPipelineVSInvocations, stats->VSInvocations);
        runtime.SecondaryPipelinePSInvocations =
            std::max(runtime.SecondaryPipelinePSInvocations, stats->PSInvocations);
        runtime.SecondaryPipelineCInvocations =
            std::max(runtime.SecondaryPipelineCInvocations, stats->CInvocations);
        runtime.SecondaryPipelineCPrimitives =
            std::max(runtime.SecondaryPipelineCPrimitives, stats->CPrimitives);
    }

    runtime.Fallback = !runtime.AnyActualMultiMode;
    runtime.FallbackReason = runtime.Fallback
                                 ? "requested MultiGpuFull was never observed as actual mode during verification interval"
                                 : "";
    runtime.SecondaryComputeDispatchCount = runtime.TotalSecondaryComputeDispatchCount;
    runtime.SecondaryGraphicsDrawCount = runtime.MaxSecondaryGraphicsDrawCount;
    runtime.SecondaryRenderedVoxelCount = runtime.MaxSecondaryRenderedVoxelCount;
    runtime.SecondaryPrimitiveEstimate = static_cast<uint64_t>(runtime.SecondaryRenderedVoxelCount) * 12u;
    runtime.CompositeSubmitted = runtime.AnyCompositeSubmitted;

    runtime.QueueCalibrations.push_back(CollectQueueCalibration(
        "gpu0.compute", primeDevice->GetCommandQueue(GQueueType::Compute)));
    runtime.QueueCalibrations.push_back(CollectQueueCalibration(
        "gpu0.graphics", primeDevice->GetCommandQueue(GQueueType::Graphics)));
    runtime.QueueCalibrations.push_back(CollectQueueCalibration(
        "gpu0.copy", primeDevice->GetCommandQueue(GQueueType::Copy)));
    runtime.QueueCalibrations.push_back(CollectQueueCalibration(
        "gpu1.compute", secondDevice->GetCommandQueue(GQueueType::Compute)));
    runtime.QueueCalibrations.push_back(CollectQueueCalibration(
        "gpu1.graphics", secondDevice->GetCommandQueue(GQueueType::Graphics)));
    runtime.QueueCalibrations.push_back(CollectQueueCalibration(
        "gpu1.copy", secondDevice->GetCommandQueue(GQueueType::Copy)));

    if (runtime.FramesObserved == 0)
        runtime.Reasons.push_back("no presented frames were observed during two-adapter verification interval");
    if (benchmarkProvenanceCache.PrimaryAdapterLuid.empty() ||
        benchmarkProvenanceCache.SecondaryAdapterLuid.empty() ||
        benchmarkProvenanceCache.PrimaryAdapterLuid == benchmarkProvenanceCache.SecondaryAdapterLuid)
    {
        runtime.Reasons.push_back("selected adapters do not have distinct hardware LUIDs");
    }
    if (runtime.Fallback)
        runtime.Reasons.push_back(runtime.FallbackReason);
    if (runtime.RequestedMode != VoxelExecutionMode::MultiGpuFull ||
        runtime.ActualMode != VoxelExecutionMode::MultiGpuFull)
    {
        runtime.Reasons.push_back("requested mode does not exactly match actual MultiGpuFull mode");
    }
    if (!runtime.AnyActualMultiMode)
        runtime.Reasons.push_back("actual MultiGpuFull mode was not observed during verification interval");
    if (runtime.SecondaryPartitionVoxels == 0)
        runtime.Reasons.push_back("secondary partition is empty");
    if (runtime.TotalSecondaryComputeDispatchCount == 0)
        runtime.Reasons.push_back("GPU1 dynamic compute dispatch count is zero");
    if (runtime.MaxSecondaryGraphicsDrawCount == 0)
        runtime.Reasons.push_back("GPU1 graphics draw count is zero");
    if (runtime.SecondaryPipelineIAPrimitives == 0 ||
        runtime.SecondaryPipelineVSInvocations == 0 ||
        runtime.SecondaryPipelinePSInvocations == 0)
    {
        runtime.Reasons.push_back("GPU1 graphics pipeline statistics are zero");
    }
    if (runtime.SecondaryGraphicsCommandListSubmissionCount == 0)
        runtime.Reasons.push_back("GPU1 graphics command list submission count is zero");
    if (runtime.LocalToSharedCommandListSubmissionCount == 0)
        runtime.Reasons.push_back("local-to-shared copy command list submission count is zero");
    if (runtime.SharedToLocalCommandListSubmissionCount == 0)
        runtime.Reasons.push_back("shared-to-local copy command list submission count is zero");
    if (runtime.ColorLocalToSharedBytes == 0 || runtime.DepthLocalToSharedBytes == 0)
        runtime.Reasons.push_back("local-to-shared render output transfer bytes are zero");
    if (runtime.ColorSharedToLocalBytes == 0 || runtime.DepthSharedToLocalBytes == 0)
        runtime.Reasons.push_back("shared-to-local render output transfer bytes are zero");
    if (runtime.ColorLocalToSharedBytes != runtime.ExpectedColorLocalToSharedBytes ||
        runtime.DepthLocalToSharedBytes != runtime.ExpectedDepthLocalToSharedBytes ||
        runtime.ColorSharedToLocalBytes != runtime.ExpectedColorSharedToLocalBytes ||
        runtime.DepthSharedToLocalBytes != runtime.ExpectedDepthSharedToLocalBytes)
    {
        runtime.Reasons.push_back("copy leg byte counts do not match expected D3D12 copyable footprints");
    }
    if (runtime.ColorLocalToSharedSource.empty() || runtime.ColorLocalToSharedDestination.empty() ||
        runtime.DepthLocalToSharedSource.empty() || runtime.DepthLocalToSharedDestination.empty() ||
        runtime.ColorSharedToLocalSource.empty() || runtime.ColorSharedToLocalDestination.empty() ||
        runtime.DepthSharedToLocalSource.empty() || runtime.DepthSharedToLocalDestination.empty())
    {
        runtime.Reasons.push_back("copy pass source/destination resource identity is incomplete");
    }
    if (runtime.ProtocolHash.empty() || runtime.ConfigHash.empty() ||
        runtime.BuildHash != benchmarkProvenanceCache.BuildHash ||
        runtime.ShaderHash != benchmarkProvenanceCache.ShaderSetHash)
    {
        runtime.Reasons.push_back("runtime build/shader/config protocol provenance is incomplete or stale");
    }
    if (runtime.PrimaryDriverVersion.empty() || runtime.SecondaryDriverVersion.empty())
        runtime.Reasons.push_back("adapter driver identity is incomplete");
    if (runtime.ParticleTransferBytes != 0)
        runtime.Reasons.push_back("particle transfer bytes are nonzero");
    if (!runtime.AnySecondaryComputeFenceValue ||
        !runtime.AnySecondaryGraphicsFenceValue ||
        !runtime.AnySecondaryLocalToSharedCopyFenceValue ||
        !runtime.AnyCrossAdapterRenderReadyFenceValue ||
        !runtime.AnyPrimarySharedToLocalCopyFenceValue ||
        !runtime.AnyPrimarySecondaryImageReadyFenceValue ||
        !runtime.AnyFinalPresentFenceValue)
    {
        runtime.Reasons.push_back("one or more required runtime fence values were never observed as nonzero");
    }
    if (!runtime.AnyCompositeSubmitted)
        runtime.Reasons.push_back("final depth-aware composite was not submitted");

    for (const auto& calibration : runtime.QueueCalibrations)
    {
        if (!calibration.Valid)
        {
            runtime.Reasons.push_back("invalid queue timestamp calibration for " + calibration.QueueName);
        }
    }

    runtime.Passed = runtime.Reasons.empty();
    twoAdapterVerificationRunner.ExportRuntimeEvidence(result, runtime);
    twoAdapterVerificationResult = result;
    twoAdapterVerificationHasResult = true;
    twoAdapterVerificationJsonPath = result.JsonPath;
    logQueue.Push(runtime.Passed
                      ? L"Two-GPU runtime verification PASS"
                      : L"Two-GPU runtime verification FAIL");
    return runtime.Passed ? 0 : 2;
}

int VoxelWaterfallApp::RunAutomaticBenchmarkSuiteOnce(
    const BenchmarkSuite suite,
    const uint32_t seedOverride,
    const uint32_t repetitionOverride,
    const std::filesystem::path& outputDirectory)
{
    std::filesystem::path runOutputDirectory = outputDirectory;
    if (runOutputDirectory.empty())
    {
        const auto base = std::filesystem::path(L"VoxelBenchmarkResults");
        for (uint32_t attempt = 0; attempt < 100; ++attempt)
        {
            runOutputDirectory = base /
                (std::string(AutomaticBenchmarkRunner::SuiteName(suite)) + "_" +
                 FilesystemTimestampToken() + "_" + std::to_string(GetCurrentProcessId()) +
                 "_" + std::to_string(attempt));
            if (!std::filesystem::exists(runOutputDirectory))
                break;
        }
    }
    else if (DirectoryHasFiles(runOutputDirectory))
    {
        logQueue.Push(L"Automatic voxel benchmark refused: output directory is not empty");
        return 3;
    }

    std::filesystem::create_directories(runOutputDirectory);
    benchmarkController.SetBenchmarkDirectory(runOutputDirectory);

    RunTwoAdapterVerificationOnce(runOutputDirectory);
    RunVisualValidation(runOutputDirectory, suite, seedOverride);

    const auto structuredEvidenceFailure = [&]() -> std::string
    {
        if (visualValidationJsonPath.empty() || !std::filesystem::exists(visualValidationJsonPath))
            return "validation evidence missing: voxel_visual_validation.json";
        if (visualValidationCsvPath.empty() || !std::filesystem::exists(visualValidationCsvPath))
            return "validation evidence missing: voxel_visual_validation.csv";
        if (twoAdapterVerificationJsonPath.empty() || !std::filesystem::exists(twoAdapterVerificationJsonPath))
            return "two-adapter evidence missing: two_adapter_preflight.json";
        return {};
    }();
    if (!structuredEvidenceFailure.empty())
    {
        logQueue.Push(L"Automatic voxel benchmark evidence precondition failed: " +
                      std::wstring(structuredEvidenceFailure.begin(), structuredEvidenceFailure.end()));
    }

    const bool started = benchmarkController.StartAutomatic(
        BuildBenchmarkControllerContext(),
        suite,
        seedOverride,
        repetitionOverride);
    if (!started)
        return benchmarkController.GetAutomaticTerminalStatus() == ResearchRunStatus::Blocked ? 3 : 2;

    auto* timerPtr = GetTimer();
    timerPtr->Reset();
    pumpFrameQuitRequested = false;
    while (benchmarkController.IsAutomaticActive())
    {
        AdvanceRuntimeWorkOutsideFrame(false);
        const bool presented = PumpOneFrame();
        AdvanceRuntimeWorkOutsideFrame(presented);
        if (pumpFrameQuitRequested)
        {
            benchmarkController.StopAutomatic(BuildBenchmarkControllerContext());
            return 4;
        }
    }

    switch (benchmarkController.GetAutomaticTerminalStatus())
    {
    case ResearchRunStatus::Complete:
        return 0;
    case ResearchRunStatus::Blocked:
        return 3;
    case ResearchRunStatus::Cancelled:
    case ResearchRunStatus::Interrupted:
        return 4;
    case ResearchRunStatus::Invalid:
    default:
        return 2;
    }
}

void VoxelWaterfallApp::RequestResearchRunner(const ResearchRunnerRequest& request)
{
    if (researchRunnerActive)
        return;

    researchRunnerRequest = request;
    if (researchRunnerRequest.OutputDirectory.empty())
    {
        researchRunnerRequest.OutputDirectory =
            std::filesystem::path(L"VoxelResearchRuns") / ResearchRunnerSuiteName(request.Suite);
    }

    researchRunnerActive = true;
    researchRunnerCancelRequested = false;
    researchRunnerHasRequest = true;
    researchRunnerPhase = "Queued";
    researchRunnerBlockedReason.clear();
    researchRunnerOutputPath = researchRunnerRequest.OutputDirectory;
    researchRunnerConfigIndex = 0;
    researchRunnerConfigTotal = 0;
    profileSweepProfileIndex = 0;
    profileSweepModeIndex = 0;
    profileSweepFrameIndex = std::numeric_limits<uint32_t>::max();
    profileSweepRepetition = 0;
    profileSweepStartSimulationFrameIndex = 0;
    if (profileSweepCsvOpen)
    {
        profileSweepCsv.close();
        profileSweepCsvOpen = false;
    }
}

void VoxelWaterfallApp::CancelResearchRunner()
{
    if (!researchRunnerActive)
        return;
    researchRunnerCancelRequested = true;
    if (benchmarkController.IsAutomaticActive() || benchmarkProfiler.IsActive())
        StopAutomaticBenchmark();
}

void VoxelWaterfallApp::AdvanceResearchRunner(const bool presentedFrame)
{
    if (isPumpingFrame)
        return;

    if (!researchRunnerActive || !researchRunnerHasRequest)
        return;

    if (researchRunnerCancelRequested)
    {
        if (profileSweepCsvOpen)
        {
            profileSweepCsv.close();
            profileSweepCsvOpen = false;
        }
        researchRunnerPhase = "CANCELLED";
        researchRunnerActive = false;
        return;
    }

    const auto finishWithStatus = [this](const char* phase, const std::string& reason = std::string{})
    {
        researchRunnerPhase = phase;
        researchRunnerBlockedReason = reason;
        researchRunnerActive = false;
        researchRunnerHasRequest = false;
    };

    const auto startBenchmark = [this, &finishWithStatus](const BenchmarkSuite suite)
    {
        std::filesystem::create_directories(researchRunnerRequest.OutputDirectory);
        benchmarkController.SetBenchmarkDirectory(researchRunnerRequest.OutputDirectory);
        const bool started = benchmarkController.StartAutomatic(
            BuildBenchmarkControllerContext(),
            suite,
            researchRunnerRequest.Seed);
        researchRunnerOutputPath = benchmarkController.GetSuiteStatusPath();
        researchRunnerConfigIndex = static_cast<uint32_t>(benchmarkController.GetAutomaticIndex());
        researchRunnerConfigTotal = static_cast<uint32_t>(benchmarkController.GetAutomaticCount());
        if (!started)
        {
            finishWithStatus("BLOCKED", "Benchmark gates rejected the request; see status JSON");
            return false;
        }
        researchRunnerPhase = "Benchmark running";
        return true;
    };

    if (researchRunnerPhase == "Benchmark running")
    {
        researchRunnerConfigIndex = static_cast<uint32_t>(benchmarkController.GetAutomaticIndex());
        researchRunnerConfigTotal = static_cast<uint32_t>(benchmarkController.GetAutomaticCount());
        researchRunnerOutputPath = benchmarkController.GetAutomaticSummaryPath();
        if (!benchmarkController.IsAutomaticActive())
            finishWithStatus("COMPLETE");
        return;
    }

    if (researchRunnerRequest.Suite == ResearchRunnerSuite::ProfileSweep)
    {
        const std::array<VoxelResearchWorkloadProfile, 6> profiles =
        {
            VoxelResearchWorkloadProfile::StaticRenderOnly,
            VoxelResearchWorkloadProfile::DynamicSimulationAndRender,
            VoxelResearchWorkloadProfile::MixedStaticAndDynamic,
            VoxelResearchWorkloadProfile::OcclusionValidation,
            VoxelResearchWorkloadProfile::SpatialLodDemonstration,
            VoxelResearchWorkloadProfile::DemoMixed
        };
        const std::array<VoxelExecutionMode, 4> modes =
        {
            VoxelExecutionMode::SingleGpuFull,
            VoxelExecutionMode::MultiGpuFull,
            VoxelExecutionMode::SingleGpuTemporalDecimation,
            VoxelExecutionMode::MultiGpuTemporalDecimation
        };

        if (!profileSweepCsvOpen)
        {
            std::filesystem::create_directories(researchRunnerRequest.OutputDirectory);
            researchRunnerOutputPath = researchRunnerRequest.OutputDirectory / L"profile_sweep.csv";
            profileSweepCsv.open(researchRunnerOutputPath, std::ios::out | std::ios::trunc);
            profileSweepCsv
                << "suite,seed,profile,requested_mode,actual_mode,repetition,warmup_frames,"
                << "measured_frames,frame_count,simulation_steps,actual_static_count,"
                << "actual_dynamic_count,actual_total_count,fallback_reason,validation_status,"
                << "validation_run_id,output_dir\n";
            profileSweepCsvOpen = true;
            profileSweepRepetition = 0;
            profileSweepProfileIndex = 0;
            profileSweepModeIndex = 0;
            profileSweepFrameIndex = std::numeric_limits<uint32_t>::max();
            researchRunnerConfigTotal = static_cast<uint32_t>(
                profiles.size() * modes.size() * std::max(1u, researchRunnerRequest.Repetitions));
            researchRunnerConfigIndex = 0;
            researchRunnerPhase = "Profile Sweep";
        }

        if (profileSweepRepetition >= std::max(1u, researchRunnerRequest.Repetitions))
        {
            profileSweepCsv.close();
            profileSweepCsvOpen = false;
            finishWithStatus("COMPLETE");
            return;
        }

        if (profileSweepFrameIndex == std::numeric_limits<uint32_t>::max())
        {
            const auto profile = profiles[profileSweepProfileIndex];
            const auto mode = modes[profileSweepModeIndex];
            ApplyResearchWorkloadProfile(profile);
            ApplyResearchLightingPreset(VoxelResearchLightingPreset::BenchmarkNeutral);
            AutomaticBenchmarkConfig config{};
            config.Mode = mode;
            config.ModeName = ExecutionModeNameLiteral(mode);
            config.TotalCount = voxelWorkload.TotalVoxelCount;
            config.SecondaryShare = UsesMultiGpuExecution(mode) ? 0.5f : 0.0f;
            config.SpatialLodEnabled = false;
            config.TemporalInterval = UsesTemporalExecution(mode) ? 2u : 1u;
            const auto applyResult = ApplyBenchmarkConfigurationAtomic(config);
            researchRunnerBlockedReason = applyResult.Passed ? "" : applyResult.Reason;
            profileSweepStartSimulationFrameIndex = simulationFrameIndex;
            profileSweepFrameIndex = 0;
            return;
        }

        if (!presentedFrame)
            return;

        ++profileSweepFrameIndex;
        const uint32_t targetFrameCount =
            researchRunnerRequest.WarmupFrames + std::max(1u, researchRunnerRequest.MeasuredFrames);
        if (profileSweepFrameIndex < targetFrameCount)
            return;

        const auto profile = profiles[profileSweepProfileIndex];
        const auto requestedMode = modes[profileSweepModeIndex];
        const uint64_t simulationSteps =
            simulationFrameIndex >= profileSweepStartSimulationFrameIndex
                ? simulationFrameIndex - profileSweepStartSimulationFrameIndex
                : 0;
        const std::string requestedModeName = GetExecutionModeName(requestedMode);
        const std::string actualModeName = GetExecutionModeName(executionMode);
        const std::string fallbackReason =
            researchRunnerBlockedReason.empty()
                ? (requestedMode == executionMode ? "" : "requested mode resolved to a different actual mode")
                : researchRunnerBlockedReason;
        const std::string validationStatus =
            !visualValidationMetrics.HasResult ? "NOT_RUN" :
            (visualValidationMetrics.Passed ? "PASS" : "FAIL");

        profileSweepCsv
            << "ProfileSweep,"
            << researchRunnerRequest.Seed << ','
            << ProfileName(profile) << ','
            << requestedModeName << ','
            << actualModeName << ','
            << profileSweepRepetition << ','
            << researchRunnerRequest.WarmupFrames << ','
            << researchRunnerRequest.MeasuredFrames << ','
            << profileSweepFrameIndex << ','
            << simulationSteps << ','
            << voxelWorkload.ActualStaticVoxelCount << ','
            << voxelWorkload.ActualDynamicVoxelCount << ','
            << voxelWorkload.TotalVoxelCount << ','
            << '"' << EscapeJsonString(fallbackReason) << '"' << ','
            << validationStatus << ','
            << visualValidationMetrics.ValidationRunId << ','
            << '"' << EscapeJsonString(researchRunnerRequest.OutputDirectory.string()) << '"'
            << "\n";
        profileSweepCsv.flush();

        ++researchRunnerConfigIndex;
        ++profileSweepModeIndex;
        if (profileSweepModeIndex >= modes.size())
        {
            profileSweepModeIndex = 0;
            ++profileSweepProfileIndex;
        }
        if (profileSweepProfileIndex >= profiles.size())
        {
            profileSweepProfileIndex = 0;
            ++profileSweepRepetition;
        }
        profileSweepFrameIndex = std::numeric_limits<uint32_t>::max();
        return;
    }

    if (researchRunnerPhase != "Queued")
        return;

    std::filesystem::create_directories(researchRunnerRequest.OutputDirectory);
    switch (researchRunnerRequest.Suite)
    {
    case ResearchRunnerSuite::Validation:
        researchRunnerPhase = "Validation";
        RunVisualValidation();
        researchRunnerOutputPath = visualValidationMetrics.CsvPath;
        finishWithStatus(visualValidationMetrics.Passed ? "COMPLETE" : "INVALID",
                         visualValidationMetrics.Passed ? "" : visualValidationMetrics.FailReason);
        return;
    case ResearchRunnerSuite::TwoGpuVerify:
    {
        researchRunnerPhase = "Two-GPU Verify";
        const int result = RunTwoAdapterVerificationOnce();
        researchRunnerOutputPath = GetExecutableDirectory() / L"TwoAdapterVerification";
        finishWithStatus(result == 0 ? "COMPLETE" : "BLOCKED",
                         result == 0 ? "" : "Two-adapter verification did not pass");
        return;
    }
    case ResearchRunnerSuite::Smoke:
    case ResearchRunnerSuite::Full:
    {
        if (researchRunnerRequest.RunPrerequisites)
        {
            researchRunnerPhase = "Two-GPU Verify";
            const int twoGpuResult = RunTwoAdapterVerificationOnce();
            if (twoGpuResult != 0)
            {
                finishWithStatus("BLOCKED", "Two-adapter verification did not pass");
                return;
            }
            researchRunnerPhase = "Validation";
            RunVisualValidation();
            if (!visualValidationMetrics.HasResult || !visualValidationMetrics.Passed)
            {
                finishWithStatus("BLOCKED", visualValidationMetrics.FailReason.empty()
                                                ? "Visual validation did not pass"
                                                : visualValidationMetrics.FailReason);
                return;
            }
        }
        startBenchmark(researchRunnerRequest.Suite == ResearchRunnerSuite::Smoke
                           ? BenchmarkSuite::Smoke
                           : BenchmarkSuite::Full);
        return;
    }
    case ResearchRunnerSuite::MemorySoak:
    {
        researchRunnerPhase = "Memory Soak";
        const uint32_t durationSeconds = std::max(1u, researchRunnerRequest.MeasuredFrames);
        const int result = RunMemorySoakTestOnce(durationSeconds, researchRunnerRequest.OutputDirectory);
        researchRunnerOutputPath = researchRunnerRequest.OutputDirectory / L"leak_report.json";
        finishWithStatus(result == 0 ? "COMPLETE" : "INVALID",
                         result == 0 ? "" : "Memory soak did not pass; see leak_report.json");
        return;
    }
    case ResearchRunnerSuite::RebuildStress:
    {
        researchRunnerPhase = "Rebuild Stress";
        const uint32_t cycles = std::max(1u, researchRunnerRequest.Repetitions);
        const uint32_t stableSeconds = std::max(1u, researchRunnerRequest.MeasuredFrames);
        const int result = RunMemoryRebuildStressTestOnce(cycles, stableSeconds, researchRunnerRequest.OutputDirectory);
        researchRunnerOutputPath = researchRunnerRequest.OutputDirectory / L"leak_report.json";
        finishWithStatus(result == 0 ? "COMPLETE" : "INVALID",
                         result == 0 ? "" : "Rebuild stress did not pass; see leak_report.json");
        return;
    }
    default:
        finishWithStatus("INVALID", "Unknown research runner suite");
        return;
    }
}

int VoxelWaterfallApp::RunProfileSweepOnce(
    const uint32_t seedOverride,
    const uint32_t warmupFrames,
    const uint32_t measuredFrames,
    const uint32_t repetitions,
    const std::filesystem::path& outputDirectory)
{
    ResearchRunnerRequest request{};
    request.Suite = ResearchRunnerSuite::ProfileSweep;
    request.Seed = seedOverride;
    request.WarmupFrames = warmupFrames;
    request.MeasuredFrames = measuredFrames;
    request.Repetitions = repetitions;
    request.OutputDirectory = outputDirectory.empty()
                                  ? std::filesystem::path(L"Artifacts") / L"profile_sweep"
                                  : outputDirectory;
    request.RunPrerequisites = false;
    RequestResearchRunner(request);

    auto* timerPtr = GetTimer();
    timerPtr->Reset();
    pumpFrameQuitRequested = false;
    while (researchRunnerActive && !pumpFrameQuitRequested)
    {
        AdvanceRuntimeWorkOutsideFrame(false);
        const bool presented = PumpOneFrame();
        AdvanceRuntimeWorkOutsideFrame(presented);
    }

    return researchRunnerPhase == "COMPLETE" ? 0 : 2;
}

void VoxelWaterfallApp::RequestApplyVoxelWorkloadSettings()
{
    pendingRuntimeChanges.VoxelWorkloadSettings = true;
}

void VoxelWaterfallApp::RequestVisualValidation(const std::filesystem::path& outputDirectory,
                                                const BenchmarkSuite suite,
                                                const uint32_t seedOverride)
{
    PendingRuntimeChanges::VisualValidationRequest request{};
    request.OutputDirectory = outputDirectory;
    request.Suite = suite;
    request.SeedOverride = seedOverride;
    pendingRuntimeChanges.RunVisualValidation = request;
}

void VoxelWaterfallApp::ApplyPendingRuntimeChangesAtFrameBoundary()
{
    assert(!isDrawingFrame && "Pending runtime changes must only be applied at a frame boundary");
    if (!pendingRuntimeChanges.HasAny())
        return;

    const PendingRuntimeChanges changes = pendingRuntimeChanges;
    pendingRuntimeChanges = {};

    const bool voxelSettingsRequested = changes.VoxelWorkloadSettings.value_or(false);
    bool sceneNeedsRebuild = false;
    bool renderTargetsRebuilt = false;

    if (changes.WorkloadProfile)
    {
        sceneNeedsRebuild =
            voxelResearchSceneManager.RequestPreset(ScenePresetForProfile(*changes.WorkloadProfile));
    }

    if (changes.CameraMode)
    {
        voxelWorkload.CameraMode = *changes.CameraMode;
        voxelWorkload.CameraPath = CameraModeName(*changes.CameraMode);
    }

    if (changes.LightingPreset)
    {
        voxelWorkload.LightingMode = *changes.LightingPreset;
        voxelWorkload.LightingPreset = LightingPresetName(*changes.LightingPreset);
        voxelWorkload.DynamicShadowsEnabled = false;
    }

    bool resolutionChanged = false;
    uint32_t requestedWidth = voxelWorkload.RenderResolutionWidth;
    uint32_t requestedHeight = voxelWorkload.RenderResolutionHeight;
    if (changes.RenderResolutionPreset)
    {
        const auto [width, height] = ResolutionForPreset(*changes.RenderResolutionPreset);
        requestedWidth = width;
        requestedHeight = height;
        resolutionChanged =
            MainWindow->GetClientWidth() != static_cast<int>(width) ||
            MainWindow->GetClientHeight() != static_cast<int>(height);
        voxelWorkload.ResolutionPreset = *changes.RenderResolutionPreset;
        voxelWorkload.RenderResolutionWidth = width;
        voxelWorkload.RenderResolutionHeight = height;
    }

    const VoxelExecutionMode requestedMode = changes.ExecutionMode.value_or(requestedExecutionMode);
    const bool executionRequested = changes.ExecutionMode.has_value();
    if (executionRequested)
        requestedExecutionMode = requestedMode;

    VoxelExecutionMode targetMode = requestedMode;
    const bool requestsMultiGpu = targetMode == VoxelExecutionMode::MultiGpuFull ||
        targetMode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    const bool targetRebuildNeededForMode =
        requestsMultiGpu && multiGpuAvailable && !multiGpuVoxelRenderTargets.IsInitialized();
    const bool commonFlushRequired =
        sceneNeedsRebuild ||
        resolutionChanged ||
        voxelSettingsRequested ||
        targetRebuildNeededForMode ||
        (executionRequested && executionMode != requestedMode);
    if (commonFlushRequired)
        Flush();

    if (resolutionChanged)
    {
        MainWindow->SetWidth(static_cast<int>(requestedWidth));
        MainWindow->SetHeight(static_cast<int>(requestedHeight));
        suppressResizeFlushForPendingRuntimeChanges = true;
        OnResize();
        suppressResizeFlushForPendingRuntimeChanges = false;
        renderTargetsRebuilt = multiGpuAvailable;
    }

    if (requestsMultiGpu && !multiGpuAvailable)
    {
        targetMode = targetMode == VoxelExecutionMode::MultiGpuFull
                         ? VoxelExecutionMode::SingleGpuFull
                         : VoxelExecutionMode::SingleGpuTemporalDecimation;
        if (multiGpuStatus.empty())
            multiGpuStatus = L"MultiGpu unavailable: secondary render target capability validation failed";
    }

    if (requestsMultiGpu && multiGpuAvailable && !multiGpuVoxelRenderTargets.IsInitialized())
    {
        if (!renderTargetsRebuilt)
        {
            RebuildMultiGpuVoxelRenderTargets();
            renderTargetsRebuilt = true;
        }
        if (!multiGpuVoxelRenderTargets.IsInitialized())
        {
            targetMode = targetMode == VoxelExecutionMode::MultiGpuFull
                             ? VoxelExecutionMode::SingleGpuFull
                             : VoxelExecutionMode::SingleGpuTemporalDecimation;
        }
    }

    const bool executionModeChanged = executionMode != targetMode;
    if (executionModeChanged)
    {
        executionMode = targetMode;
        voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::None;
        voxelCompositeDebugView = VoxelCompositeDebugView::FinalComposite;
        if (executionMode == VoxelExecutionMode::MultiGpuFull)
            multiGpuStatus = L"MultiGpuFull adapter-local partitions active; secondary render-output transfer enabled";
        else if (executionMode == VoxelExecutionMode::MultiGpuTemporalDecimation)
            multiGpuStatus = L"MultiGpuTemporalDecimation adapter-local partitions active; secondary render-output transfer enabled";
        else if (executionMode == VoxelExecutionMode::SingleGpuTemporalDecimation)
            multiGpuStatus = L"SingleGpuTemporalDecimation active";
        else
            multiGpuStatus = L"SingleGpuFull active";
        logQueue.Push(L"\nVoxel execution mode changed: " + multiGpuStatus);
    }

    if (sceneNeedsRebuild)
    {
        assert(!isDrawingFrame && "Voxel research scene rebuild must not run during DrawFrame");
        lights.clear();
        camera.reset();
        researchCameraController.reset();
        VoxelResearchSceneContext sceneContext{
            primeDevice,
            secondDevice,
            *assets,
            models,
            srvTexturesMemory,
            gameObjects,
            typedRenderer,
            voxelWorkload,
            AspectRatio()
        };
        voxelResearchSceneManager.RebuildScene(sceneContext);
        ++voxelSceneGeneration;
        SortGO();
    }

    if (voxelSettingsRequested)
    {
        const StaticLayerSnapshot staticLayerBefore = CaptureStaticLayerSnapshot(voxelWorkload);
        const auto activePreset = voxelResearchSceneManager.GetActivePreset();
        if (activePreset == VoxelResearchScenePreset::StaticVoxelEnvironment ||
            activePreset == VoxelResearchScenePreset::MixedVoxelEnvironment ||
            activePreset == VoxelResearchScenePreset::DemoMixed ||
            activePreset == VoxelResearchScenePreset::OcclusionValidation ||
            activePreset == VoxelResearchScenePreset::SpatialLodDemonstration)
        {
            VoxelResearchEnvironmentGenerator::Settings settings{};
            settings.Seed = voxelWorkload.StaticGenerationSeed;
            settings.StaticVoxelBudget = voxelWorkload.StaticVoxelBudget;
            settings.BudgetPreset = voxelWorkload.StaticBudgetPreset;
            settings.StorageMode = voxelWorkload.StaticStorageMode;
            settings.VoxelSize = voxelWorkload.StaticVoxelSize;
            settings.SecondaryShare = voxelWorkload.SecondaryShare;
            settings.PartitionStrategy = voxelWorkload.PartitionStrategy;
            settings.LoadBalanceScenario = voxelWorkload.LoadBalanceScenario;
            settings.ChunkSize = voxelWorkload.ChunkSize;
            settings.SpatialLod = voxelWorkload.SpatialLod;
            auto generated = VoxelResearchEnvironmentGenerator::Generate(settings);
            voxelWorkload.Layers.clear();
            voxelWorkload.Layers.push_back(std::move(generated.Layer));
            voxelWorkload.StaticTelemetry = generated.Telemetry;
        }
        else
        {
            voxelWorkload.Layers.clear();
        }
        if (activePreset == VoxelResearchScenePreset::StaticVoxelEnvironment ||
            activePreset == VoxelResearchScenePreset::SpatialLodDemonstration)
        {
            voxelWorkload.DynamicVoxelBudget = 0;
        }
        voxelWorkload = VoxelSceneWorkloadBuilder::Build(voxelWorkload);
        ++voxelSceneGeneration;
        AssertStaticLayerShapePreserved(staticLayerBefore, voxelWorkload);
    }

    if (sceneNeedsRebuild || voxelSettingsRequested || executionModeChanged)
    {
        RebuildGpuPartitionsForMode();
        voxelSimulationAccumulator = 0.0;
        voxelSimulationTime = 0.0;
        voxelSimulationStepsThisFrame = 0;
        voxelInterpolationAlpha = 0.0f;
        voxelRecycledCount = 0;
        voxelAliveCount = 0;
        voxelExpectedCount = voxelWorkload.TotalVoxelCount;
        simulationFrameIndex = 0;
    }

    if (changes.CameraMode)
        ApplyResearchCameraMode(*changes.CameraMode);
    if (changes.LightingPreset)
        ApplyResearchLightingPreset(*changes.LightingPreset);
    if (sceneNeedsRebuild || voxelSettingsRequested || executionModeChanged || changes.CameraMode)
        spatialLodCameraInitialized = false;

    if (changes.RunVisualValidation)
    {
        ResearchRunnerRequest request{};
        request.Suite = ResearchRunnerSuite::Validation;
        request.Seed = changes.RunVisualValidation->SeedOverride;
        request.OutputDirectory = changes.RunVisualValidation->OutputDirectory;
        request.RunPrerequisites = false;
        RequestResearchRunner(request);
    }
}

void VoxelWaterfallApp::ApplyResearchWorkloadProfile(const VoxelResearchWorkloadProfile profile)
{
    assert(!isDrawingFrame && "ApplyResearchWorkloadProfile must not be called during DrawFrame");
    const VoxelResearchScenePreset preset = ScenePresetForProfile(profile);

    if (!voxelResearchSceneManager.RequestPreset(preset))
        return;

    Flush();
    lights.clear();
    camera.reset();
    researchCameraController.reset();
    VoxelResearchSceneContext sceneContext{
        primeDevice,
        secondDevice,
        *assets,
        models,
        srvTexturesMemory,
        gameObjects,
        typedRenderer,
        voxelWorkload,
        AspectRatio()
    };
    assert(!isDrawingFrame && "Voxel research scene rebuild must not run during DrawFrame");
    voxelResearchSceneManager.RebuildScene(sceneContext);
    ++voxelSceneGeneration;
    SortGO();
    RebuildGpuPartitionsForMode();
    voxelSimulationAccumulator = 0.0;
    voxelSimulationTime = 0.0;
    voxelSimulationStepsThisFrame = 0;
    voxelInterpolationAlpha = 0.0f;
    voxelRecycledCount = 0;
    voxelAliveCount = 0;
    voxelExpectedCount = voxelWorkload.TotalVoxelCount;
    simulationFrameIndex = 0;
    spatialLodCameraInitialized = false;
    pendingRuntimeChanges.VoxelWorkloadSettings.reset();
}

void VoxelWaterfallApp::ApplyResearchCameraMode(const VoxelResearchCameraMode mode)
{
    voxelWorkload.CameraMode = mode;
    voxelWorkload.CameraPath = CameraModeName(mode);
    if (researchCameraController)
    {
        researchCameraController->SetMode(mode);
        researchCameraController->SetInputBlocked(mode != VoxelResearchCameraMode::Interactive ||
                                                  benchmarkProfiler.IsActive() ||
                                                  benchmarkController.IsAutomaticActive());
    }
    spatialLodCameraInitialized = false;
}

void VoxelWaterfallApp::ApplyResearchLightingPreset(const VoxelResearchLightingPreset preset)
{
    voxelWorkload.LightingMode = preset;
    voxelWorkload.LightingPreset = LightingPresetName(preset);
    voxelWorkload.DynamicShadowsEnabled = false;
}

void VoxelWaterfallApp::ApplyRenderResolutionPreset(const VoxelRenderResolutionPreset preset)
{
    assert(!isDrawingFrame && "ApplyRenderResolutionPreset must not be called during DrawFrame");
    const auto [width, height] = ResolutionForPreset(preset);
    if (MainWindow->GetClientWidth() == static_cast<int>(width) &&
        MainWindow->GetClientHeight() == static_cast<int>(height))
    {
        voxelWorkload.ResolutionPreset = preset;
        voxelWorkload.RenderResolutionWidth = width;
        voxelWorkload.RenderResolutionHeight = height;
        return;
    }

    Flush();
    voxelWorkload.ResolutionPreset = preset;
    voxelWorkload.RenderResolutionWidth = width;
    voxelWorkload.RenderResolutionHeight = height;
    MainWindow->SetWidth(static_cast<int>(width));
    MainWindow->SetHeight(static_cast<int>(height));
    OnResize();
}

bool VoxelWaterfallApp::HandleDemoPresetHotkey(const WPARAM key)
{
    if (benchmarkController.IsAutomaticActive())
        return false;

    switch (key)
    {
    case VK_F1:
        pendingRuntimeChanges.WorkloadProfile = VoxelResearchWorkloadProfile::StaticRenderOnly;
        return true;
    case VK_F2:
        pendingRuntimeChanges.WorkloadProfile = VoxelResearchWorkloadProfile::DynamicSimulationAndRender;
        return true;
    case VK_F3:
        pendingRuntimeChanges.WorkloadProfile = VoxelResearchWorkloadProfile::MixedStaticAndDynamic;
        return true;
    case VK_F4:
        pendingRuntimeChanges.WorkloadProfile = VoxelResearchWorkloadProfile::OcclusionValidation;
        return true;
    case VK_F5:
        pendingRuntimeChanges.WorkloadProfile = VoxelResearchWorkloadProfile::SpatialLodDemonstration;
        return true;
    case VK_F9:
        pendingRuntimeChanges.WorkloadProfile = VoxelResearchWorkloadProfile::DemoMixed;
        return true;
    case VK_F6:
        voxelCompositeDebugView = VoxelCompositeDebugView::PartitionOwnershipColors;
        voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::AdapterOwnership;
        return true;
    case VK_F7:
        voxelCompositeDebugView = VoxelCompositeDebugView::SecondaryColorOnly;
        return true;
    case VK_F8:
        voxelCompositeDebugView = VoxelCompositeDebugView::FinalComposite;
        voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::None;
        return true;
    default:
        return false;
    }
}

void VoxelWaterfallApp::ResetBenchmarkDeterministicState()
{
    if (voxelWorkload.CameraMode == VoxelResearchCameraMode::Interactive)
    {
        voxelWorkload.CameraMode = VoxelResearchCameraMode::FixedOverview;
        voxelWorkload.CameraPath = CameraModeName(voxelWorkload.CameraMode);
    }
    voxelSimulationAccumulator = 0.0;
    voxelSimulationTime = 0.0;
    voxelSimulationStepsThisFrame = 0;
    voxelInterpolationAlpha = 0.0f;
    voxelRecycledCount = 0;
    voxelAliveCount = 0;
    simulationFrameIndex = 0;
    voxelWorkload.SpatialLod.FreezeCamera = false;
    spatialLodCameraInitialized = false;
    if (researchCameraController)
    {
        researchCameraController->SetMode(voxelWorkload.CameraMode);
        researchCameraController->SetInputBlocked(true);
        researchCameraController->SetDeterministicTime(0.0);
        researchCameraController->ResetRoute();
    }
}

std::string VoxelWaterfallApp::GetExecutionModeName() const
{
    return GetExecutionModeName(executionMode);
}

std::string VoxelWaterfallApp::GetExecutionModeName(const VoxelExecutionMode mode) const
{
    switch (mode)
    {
    case VoxelExecutionMode::SingleGpuFull:
        return "SingleGpuFull";
    case VoxelExecutionMode::MultiGpuFull:
        return "MultiGpuFull";
    case VoxelExecutionMode::SingleGpuTemporalDecimation:
        return "SingleGpuTemporalDecimation";
    case VoxelExecutionMode::MultiGpuTemporalDecimation:
        return "MultiGpuTemporalDecimation";
    }
    return "SingleGpuFull";
}

void VoxelWaterfallApp::InitializeBenchmarkProvenanceCache()
{
    benchmarkProvenanceCache.BuildHash =
        HashFileOrUnknown(GetExecutableDirectory() / L"MGPU-VoxelWaterfall.exe");
    benchmarkProvenanceCache.ShaderSetHash =
        HashShaderBytecodeSetOrUnknown(GetExecutableDirectory());
    benchmarkProvenanceCache.ShaderHash = benchmarkProvenanceCache.ShaderSetHash;
    const auto gitStatus = RunCommandTrimmed("git status --porcelain=v2 --branch");
    if (gitStatus.rfind("Unknown:", 0) == 0)
    {
        benchmarkProvenanceCache.GitCommit = gitStatus;
        benchmarkProvenanceCache.GitDirtyState = gitStatus;
    }
    else
    {
        const auto [commit, dirtyState] = ParseGitStatusPorcelainV2(gitStatus);
        benchmarkProvenanceCache.GitCommit = commit;
        benchmarkProvenanceCache.GitDirtyState = dirtyState;
    }
    benchmarkProvenanceCache.OperatingSystem = "Windows";
#ifdef _DEBUG
    benchmarkProvenanceCache.BuildConfiguration = "Debug";
#else
    benchmarkProvenanceCache.BuildConfiguration = "Release";
#endif
#if defined(DEBUG) || defined(_DEBUG)
    benchmarkProvenanceCache.D3D12DebugLayerEnabled = true;
#else
    benchmarkProvenanceCache.D3D12DebugLayerEnabled = false;
#endif
    benchmarkProvenanceCache.PrimaryAdapterName = primeDevice ? primeDevice->GetName() : L"unavailable";
    benchmarkProvenanceCache.SecondaryAdapterName = multiGpuAvailable && secondDevice
                                                       ? secondDevice->GetName()
                                                       : L"unavailable";
    FillAdapterMetadata(primeDevice,
                        benchmarkProvenanceCache.PrimaryVendorId,
                        benchmarkProvenanceCache.PrimaryDeviceId,
                        benchmarkProvenanceCache.PrimaryDedicatedVideoMemory,
                        benchmarkProvenanceCache.PrimaryAdapterLuid);
    FillAdapterMetadata(multiGpuAvailable ? secondDevice : nullptr,
                        benchmarkProvenanceCache.SecondaryVendorId,
                        benchmarkProvenanceCache.SecondaryDeviceId,
                        benchmarkProvenanceCache.SecondaryDedicatedVideoMemory,
                        benchmarkProvenanceCache.SecondaryAdapterLuid);
    benchmarkProvenanceCache.Initialized = true;
}

void VoxelWaterfallApp::RefreshBenchmarkFrameTelemetry(
    VoxelBenchmarkProfiler::FrameMetadata& metadata) const
{
    metadata.FrameIndex = simulationFrameIndex;
    metadata.RequestedMode = GetExecutionModeName(requestedExecutionMode);
    metadata.ActualMode = GetExecutionModeName(frameGraphTelemetry.ActualMode);
    metadata.TransferMode = CrossAdapterTransferModeName(crossAdapterTransferMode);
    metadata.FallbackReason.clear();
    if (metadata.RequestedMode != metadata.ActualMode)
        metadata.FallbackReason = "requested mode unavailable; actual mode selected by runtime capability checks";

    uint32_t primaryVoxelCount = 0;
    uint32_t secondaryVoxelCount = 0;
    uint32_t updatedVoxelCount = 0;
    for (const auto& partition : voxelWorkload.Partitions)
    {
        if (partition.AdapterOwner == VoxelAdapterOwner::Secondary)
            secondaryVoxelCount += partition.VoxelCount();
        else
            primaryVoxelCount += partition.VoxelCount();
        updatedVoxelCount += partition.UpdatedVoxelCount;
    }
    metadata.PrimaryPartitionVoxelCount = primaryVoxelCount;
    metadata.SecondaryPartitionVoxelCount = secondaryVoxelCount;
    metadata.UpdatedVoxelCount = updatedVoxelCount;
    metadata.SimulationStepsThisFrame = voxelSimulationStepsThisFrame;
    metadata.SimulationDispatchCount = frameGraphTelemetry.SimulationDispatchCount;
    metadata.SecondaryComputeSubmitted = frameGraphTelemetry.SecondaryComputeSubmitted;
    metadata.SchedulerMode = VoxelSimulationScheduler::SchedulerModeName(frameGraphTelemetry.SchedulerMode);
    metadata.RequestedFixedSteps = frameGraphTelemetry.RequestedFixedSteps;
    metadata.ExecutedFixedSteps = frameGraphTelemetry.ExecutedFixedSteps;
    metadata.DroppedSteps = frameGraphTelemetry.DroppedSimulationSteps;
    metadata.DroppedSimulationTime = frameGraphTelemetry.DroppedSimulationTime;
    metadata.LogicalUpdatedVoxelCount = frameGraphTelemetry.LogicalUpdatedVoxelCount;
    metadata.WallDeltaMs = frameGraphTelemetry.WallDeltaMs;
    metadata.AcceptedSimulationDeltaMs = frameGraphTelemetry.AcceptedSimulationDeltaMs;
    metadata.FrameResourceBackpressurePollCount = frameGraphTelemetry.FrameResourceBackpressurePollCount;
    metadata.DrainedMessageCount = frameGraphTelemetry.DrainedMessageCount;
    metadata.SuccessfulPresentCount = frameGraphTelemetry.SuccessfulPresentCount;
    metadata.SimulationStepsPerWallSecond = frameGraphTelemetry.SimulationStepsPerWallSecond;
    metadata.CpuWaitMs = currentPrimaryWaitMs;
    metadata.PresentToPresentMs = currentPresentToPresentMs;
    metadata.TotalCrossAdapterBytes = frameGraphTelemetry.TotalCrossAdapterBytes;
    metadata.ColorTransferBytes = frameGraphTelemetry.ColorBytesTransferred;
    metadata.DepthTransferBytes = frameGraphTelemetry.DepthBytesTransferred;
    metadata.ParticleTransferBytes = frameGraphTelemetry.ParticleTransferBytes;
    metadata.RenderOutputTransferBytes = frameGraphTelemetry.RenderOutputTransferBytes;
    metadata.SecondaryDrawCalls = frameGraphTelemetry.SecondaryDrawCalls;
    metadata.PrimaryRenderedVoxelCount = frameGraphTelemetry.PrimarySpatialLodStats.TotalRendered();
    metadata.SecondaryRenderedVoxelCount = frameGraphTelemetry.SecondarySpatialLodStats.TotalRendered();
    metadata.PrimarySubmittedVoxelCount = metadata.PrimaryRenderedVoxelCount;
    metadata.SecondarySubmittedVoxelCount = metadata.SecondaryRenderedVoxelCount;
    metadata.PrimaryLod0Count = frameGraphTelemetry.PrimarySpatialLodStats.Lod0Rendered;
    metadata.PrimaryLod1Count = frameGraphTelemetry.PrimarySpatialLodStats.Lod1Rendered;
    metadata.PrimaryLod2Count = frameGraphTelemetry.PrimarySpatialLodStats.Lod2Rendered;
    metadata.SecondaryLod0Count = frameGraphTelemetry.SecondarySpatialLodStats.Lod0Rendered;
    metadata.SecondaryLod1Count = frameGraphTelemetry.SecondarySpatialLodStats.Lod1Rendered;
    metadata.SecondaryLod2Count = frameGraphTelemetry.SecondarySpatialLodStats.Lod2Rendered;
    const auto totalHashCapacity =
        frameGraphTelemetry.PrimarySpatialLodStats.HashCapacity +
        frameGraphTelemetry.SecondarySpatialLodStats.HashCapacity;
    const auto totalEmittedGroups =
        frameGraphTelemetry.PrimarySpatialLodStats.EmittedGroups +
        frameGraphTelemetry.SecondarySpatialLodStats.EmittedGroups;
    metadata.NoLodFastPath = voxelWorkload.SpatialLod.Mode == VoxelSpatialLodMode::Off;
    metadata.LodHashCapacity = totalHashCapacity;
    metadata.LodHashLoadFactor =
        totalHashCapacity > 0
            ? static_cast<float>(totalEmittedGroups) / static_cast<float>(totalHashCapacity)
            : 0.0f;
    metadata.LodDuplicateCount =
        frameGraphTelemetry.PrimarySpatialLodStats.Aggregated +
        frameGraphTelemetry.SecondarySpatialLodStats.Aggregated;
    metadata.LodProbeOverflowCount =
        frameGraphTelemetry.PrimarySpatialLodStats.ProbeOverflow +
        frameGraphTelemetry.SecondarySpatialLodStats.ProbeOverflow;
    metadata.LodMaxProbeCount =
        std::max(frameGraphTelemetry.PrimarySpatialLodStats.MaxProbeCount,
                 frameGraphTelemetry.SecondarySpatialLodStats.MaxProbeCount);
    const auto totalProbeCount =
        frameGraphTelemetry.PrimarySpatialLodStats.TotalProbeCount +
        frameGraphTelemetry.SecondarySpatialLodStats.TotalProbeCount;
    metadata.LodAverageProbeCount =
        totalEmittedGroups > 0
            ? static_cast<float>(totalProbeCount) / static_cast<float>(totalEmittedGroups)
            : 0.0f;
    metadata.VisualValidationHasResult = frameGraphTelemetry.VisualValidationHasResult;
    metadata.VisualValidationPassed = frameGraphTelemetry.VisualValidationPassed;
    metadata.VisualValidationRunId = frameGraphTelemetry.VisualValidationRunId;
    metadata.VisualValidationSnapshotHash = frameGraphTelemetry.VisualValidationSnapshotHash;
    metadata.VisualValidationColorMAE = frameGraphTelemetry.VisualValidationColorMAE;
    metadata.VisualValidationColorRMSE = frameGraphTelemetry.VisualValidationColorRMSE;
    metadata.VisualValidationPSNR = frameGraphTelemetry.VisualValidationPSNR;
    metadata.VisualValidationMaxError = frameGraphTelemetry.VisualValidationMaxError;
    metadata.VisualValidationMismatchedPixelPercent =
        frameGraphTelemetry.VisualValidationMismatchedPixelPercent;
    metadata.VisualValidationDepthRMSE = frameGraphTelemetry.VisualValidationDepthRMSE;
    metadata.VisualValidationDepthMismatchPercent =
        frameGraphTelemetry.VisualValidationDepthMismatchPercent;
    metadata.VisualValidationPipelinePrimitiveCount =
        frameGraphTelemetry.VisualValidationPipelinePrimitiveCount;
    metadata.VisualValidationFailReason = frameGraphTelemetry.VisualValidationFailReason;
    metadata.VisualValidationProtocolHash = visualValidationProtocolHash;
    metadata.VisualValidationConfigHash = currentBenchmarkResolvedConfigHash;
    metadata.VisualValidationCameraHash = visualValidationCameraHash;
    if (visualValidationMetrics.HasResult)
    {
        const VoxelVisualValidationCaseResult* selectedCase = nullptr;
        for (const auto& result : visualValidationMetrics.CaseResults)
        {
            const bool matchesCandidate = result.CandidateConfigHash == currentBenchmarkResolvedConfigHash;
            const bool matchesReference = result.ReferenceConfigHash == currentBenchmarkResolvedConfigHash;
            if (!matchesCandidate && !matchesReference)
                continue;
            if (!selectedCase ||
                (result.ValidationKind == "approximation_fidelity" && matchesCandidate))
            {
                selectedCase = &result;
                if (result.ValidationKind == "approximation_fidelity" && matchesCandidate)
                    break;
            }
        }
        if (selectedCase)
        {
            metadata.VisualValidationCaseId = selectedCase->CaseId;
            metadata.VisualValidationProtocolHash = selectedCase->ProtocolHash;
            metadata.VisualValidationConfigHash =
                selectedCase->CandidateConfigHash == currentBenchmarkResolvedConfigHash
                    ? selectedCase->CandidateConfigHash
                    : selectedCase->ReferenceConfigHash;
            metadata.VisualValidationCameraHash = selectedCase->CameraHash;
            metadata.VisualValidationPassed = selectedCase->Passed;
            metadata.VisualValidationColorMAE = selectedCase->ColorMAE;
            metadata.VisualValidationColorRMSE = selectedCase->ColorRMSE;
            metadata.VisualValidationPSNR = selectedCase->ColorPSNR;
            metadata.VisualValidationMaxError = selectedCase->MaxColorError;
            metadata.VisualValidationMismatchedPixelPercent = selectedCase->ColorMismatchPercent;
            metadata.VisualValidationDepthRMSE = selectedCase->DepthRMSE;
            metadata.VisualValidationDepthMismatchPercent = selectedCase->DepthMismatchPercent;
            metadata.VisualValidationFailReason = selectedCase->Passed
                                                     ? ""
                                                     : selectedCase->Reason;
        }
        else
        {
            metadata.VisualValidationPassed = false;
            metadata.VisualValidationCaseId.clear();
            metadata.VisualValidationFailReason =
                "no matching PASS visual validation case for resolved benchmark config hash";
        }
    }
    metadata.CpuFrameStart = cpuFrameStart;
}

VoxelBenchmarkProfiler::FrameMetadata VoxelWaterfallApp::BuildBenchmarkMetadata() const
{
    ++benchmarkMetadataBuildCount;
    VoxelBenchmarkProfiler::FrameMetadata metadata{};
    metadata.ProfileName = ProfileName(voxelWorkload.Profile);
    metadata.ScenePreset = voxelWorkload.ScenePreset;
    metadata.TemporalPolicy = TemporalPolicyName(voxelWorkload.TemporalPolicy);
    metadata.SpatialLodPolicy =
        voxelWorkload.SpatialLod.Mode == VoxelSpatialLodMode::ThreeLevel
            ? "ThreeLevel"
            : "Off";
    metadata.PartitionStrategy = PartitionStrategyName(voxelWorkload.PartitionStrategy);
    metadata.LoadBalanceScenario = LoadBalanceScenarioName(voxelWorkload.LoadBalanceScenario);
    metadata.CameraPath =
        researchCameraController ? researchCameraController->GetCameraPathName() : voxelWorkload.CameraPath;
    metadata.LightingPreset = LightingPresetName(voxelWorkload.LightingMode);
    metadata.RenderResolutionPreset = ResolutionPresetName(voxelWorkload.ResolutionPreset);
    metadata.ResolvedConfigHash = currentBenchmarkResolvedConfigHash;
    metadata.CameraFovDegrees = camera ? camera->GetFov() : 0.0f;
    metadata.CameraNearPlane = camera ? camera->GetNearZ() : 0.0f;
    metadata.CameraFarPlane = camera ? camera->GetFarZ() : 0.0f;
    metadata.DynamicShadowsEnabled = voxelWorkload.DynamicShadowsEnabled;
    const auto [configClass, configReason] = ClassifyBenchmarkConfig(voxelWorkload, requestedExecutionMode);
    metadata.BenchmarkConfigClass = BenchmarkConfigClassName(configClass);
    metadata.BenchmarkConfigReason = configReason;
    metadata.TotalVoxelCount = voxelWorkload.TotalVoxelCount;
    metadata.ActualStaticVoxelCount = voxelWorkload.ActualStaticVoxelCount;
    metadata.ActualDynamicVoxelCount = voxelWorkload.ActualDynamicVoxelCount;
    metadata.StaticVoxelBudget = voxelWorkload.StaticVoxelBudget;
    metadata.DynamicVoxelBudget = voxelWorkload.DynamicVoxelBudget;
    metadata.VoxelSize = voxelWorkload.Parameters.VoxelSize;
    metadata.ChunkSizeX = voxelWorkload.ChunkSize.Width;
    metadata.ChunkSizeY = voxelWorkload.ChunkSize.Height;
    metadata.ChunkSizeZ = voxelWorkload.ChunkSize.Depth;
    metadata.Seed =
        voxelWorkload.ActualStaticVoxelCount > 0
            ? voxelWorkload.StaticGenerationSeed
            : voxelWorkload.Parameters.Seed;
    metadata.SecondaryShare = voxelWorkload.SecondaryShare;
    metadata.TemporalDecimationInterval = voxelWorkload.TemporalDecimationInterval;
    if (antiAliasingPrimePath)
    {
        const auto desc = antiAliasingPrimePath->GetRenderTarget().GetD3D12ResourceDesc();
        metadata.RenderWidth = static_cast<uint32_t>(desc.Width);
        metadata.RenderHeight = desc.Height;
    }
    metadata.PrimaryAdapterName = benchmarkProvenanceCache.PrimaryAdapterName;
    metadata.SecondaryAdapterName = benchmarkProvenanceCache.SecondaryAdapterName;
    metadata.PrimaryVendorId = benchmarkProvenanceCache.PrimaryVendorId;
    metadata.PrimaryDeviceId = benchmarkProvenanceCache.PrimaryDeviceId;
    metadata.PrimaryDedicatedVideoMemory = benchmarkProvenanceCache.PrimaryDedicatedVideoMemory;
    metadata.PrimaryAdapterLuid = benchmarkProvenanceCache.PrimaryAdapterLuid;
    metadata.SecondaryVendorId = benchmarkProvenanceCache.SecondaryVendorId;
    metadata.SecondaryDeviceId = benchmarkProvenanceCache.SecondaryDeviceId;
    metadata.SecondaryDedicatedVideoMemory = benchmarkProvenanceCache.SecondaryDedicatedVideoMemory;
    metadata.SecondaryAdapterLuid = benchmarkProvenanceCache.SecondaryAdapterLuid;
    metadata.OperatingSystem = benchmarkProvenanceCache.OperatingSystem;
    metadata.BuildConfiguration = benchmarkProvenanceCache.BuildConfiguration;
    metadata.GitCommit = benchmarkProvenanceCache.GitCommit;
    metadata.GitDirtyState = benchmarkProvenanceCache.GitDirtyState;
    metadata.D3D12DebugLayerEnabled = benchmarkProvenanceCache.D3D12DebugLayerEnabled;
    RefreshBenchmarkFrameTelemetry(metadata);
    return metadata;
}

BenchmarkControllerContext VoxelWaterfallApp::BuildBenchmarkControllerContext()
{
    const bool visualPassed = visualValidationMetrics.HasResult && visualValidationMetrics.Passed;
    std::string twoAdapterReason = "two-adapter runtime verification has not been run";
    if (twoAdapterVerificationHasResult)
    {
        if (!twoAdapterVerificationResult.Reasons.empty())
            twoAdapterReason = twoAdapterVerificationResult.Reasons.front();
        else
            twoAdapterReason = TwoAdapterVerificationRunner::StatusName(twoAdapterVerificationResult.Status);
    }
    const auto metadata = BuildBenchmarkMetadata();
    auto currentProvenance = BuildResearchProvenanceRecord(
        metadata,
        benchmarkProvenanceCache.BuildHash,
        benchmarkProvenanceCache.ShaderSetHash,
        visualValidationProtocolHash,
        visualValidationCaseConfigHash,
        visualValidationCameraHash);
    auto validationProvenance = currentProvenance;
    validationProvenance.Fields["build.executable_sha256"] = visualValidationBuildHash;
    validationProvenance.Fields["build.shader_bytecode_set_sha256"] = visualValidationShaderHash;
    validationProvenance.Fields["validation.protocol_sha256"] = visualValidationProtocolHash;
    validationProvenance.Fields["validation.case_config_sha256"] = visualValidationCaseConfigHash;
    validationProvenance.Fields["validation.camera_sha256"] = visualValidationCameraHash;
    auto twoAdapterProvenance = currentProvenance;
    twoAdapterProvenance.Fields["build.executable_sha256"] = twoAdapterVerificationResult.BuildHash;
    std::vector<BenchmarkValidationCoverage> validationCoverage;
    for (const auto& result : visualValidationMetrics.CaseResults)
    {
        if (!result.Passed)
            continue;
        if (!result.ReferenceConfigHash.empty())
        {
            validationCoverage.push_back({
                result.ConfigKey,
                ExecutionModeNameLiteral(result.RequestedReferenceMode),
                result.CaseId,
                result.ProtocolHash,
                result.ReferenceConfigHash,
                result.CameraHash,
                true
            });
        }
        if (!result.CandidateConfigHash.empty())
        {
            validationCoverage.push_back({
                result.ConfigKey,
                ExecutionModeNameLiteral(result.RequestedCandidateMode),
                result.CaseId,
                result.ProtocolHash,
                result.CandidateConfigHash,
                result.CameraHash,
                true
            });
        }
    }

    return BenchmarkControllerContext{
        benchmarkProfiler,
        [this] { return BuildBenchmarkMetadata(); },
        [this](const std::wstring& message) { logQueue.Push(message); },
        [this] { return MainWindow->IsVSync(); },
        [this](const bool enabled) { MainWindow->SetVSync(enabled); },
        [this] { Flush(); },
        [this](const VoxelExecutionMode mode) { ApplyExecutionMode(mode); },
        [this](const int totalCount) { ApplyBenchmarkVoxelCount(totalCount); },
        [this](const float secondaryShare) { ApplyBenchmarkSecondaryShare(secondaryShare); },
        [this](const bool enabled) { ApplyBenchmarkSpatialLodEnabled(enabled); },
        [this](const uint32_t interval) { ApplyBenchmarkTemporalInterval(interval); },
        [this](const AutomaticBenchmarkConfig& config) { return ApplyBenchmarkConfigurationAtomic(config); },
        [this] { ResetBenchmarkDeterministicState(); },
        multiGpuAvailable,
        visualPassed,
        visualValidationMetrics.ValidationRunId,
        visualValidationMetrics.FailReason,
        benchmarkProvenanceCache.BuildHash,
        benchmarkProvenanceCache.ShaderSetHash,
        visualValidationBuildHash,
        visualValidationShaderHash,
        visualValidationAdapterPairIdentity,
        visualValidationProtocolHash,
        visualValidationCaseConfigHash,
        visualValidationCameraHash,
        visualValidationJsonPath,
        visualValidationCsvPath,
        validationCoverage,
        currentProvenance,
        validationProvenance,
        twoAdapterProvenance,
        twoAdapterVerificationHasResult &&
            twoAdapterVerificationResult.Status == TwoAdapterVerificationStatus::Pass,
        twoAdapterVerificationResult.VerificationRunId,
        twoAdapterVerificationResult.AdapterPairIdentity,
        twoAdapterVerificationResult.BuildHash,
        twoAdapterReason,
        twoAdapterVerificationJsonPath
    };
}

void VoxelWaterfallApp::StartAutomaticBenchmark()
{
    StartAutomaticBenchmark(BenchmarkSuite::Full);
}

void VoxelWaterfallApp::StartAutomaticBenchmark(const BenchmarkSuite suite,
                                                const uint32_t seedOverride,
                                                const uint32_t repetitionOverride)
{
    benchmarkController.StartAutomatic(BuildBenchmarkControllerContext(), suite, seedOverride, repetitionOverride);
}

void VoxelWaterfallApp::StopAutomaticBenchmark()
{
    benchmarkController.StopAutomatic(BuildBenchmarkControllerContext());
}

void VoxelWaterfallApp::ApplyBenchmarkVoxelCount(const int totalCount)
{
    const auto activePreset = voxelResearchSceneManager.GetActivePreset();
    if (activePreset == VoxelResearchScenePreset::StaticVoxelEnvironment)
        voxelWorkload.StaticVoxelBudget = static_cast<uint32_t>(std::max(1, totalCount));
    else if (activePreset == VoxelResearchScenePreset::MixedVoxelEnvironment ||
             activePreset == VoxelResearchScenePreset::DemoMixed)
    {
        voxelWorkload.StaticVoxelBudget = static_cast<uint32_t>(std::max(1, totalCount));
        if (totalCount <= 100000)
            voxelWorkload.DynamicVoxelBudget = 25000;
        else if (totalCount <= 250000)
            voxelWorkload.DynamicVoxelBudget = 100000;
        else if (totalCount <= 500000)
            voxelWorkload.DynamicVoxelBudget = 250000;
        else
            voxelWorkload.DynamicVoxelBudget = 500000;
    }
    else if (activePreset == VoxelResearchScenePreset::DynamicWaterfall)
        voxelWorkload.DynamicVoxelBudget = static_cast<uint32_t>(std::max(0, totalCount));
    else if (activePreset == VoxelResearchScenePreset::SpatialLodDemonstration)
        voxelWorkload.StaticVoxelBudget = static_cast<uint32_t>(std::max(1, totalCount));
    else
        voxelWorkload.TotalVoxelCount = static_cast<uint32_t>(std::max(0, totalCount));
    pendingRuntimeChanges.VoxelWorkloadSettings = true;
    ApplyPendingVoxelSettings();
}

void VoxelWaterfallApp::ApplyBenchmarkSecondaryShare(const float secondaryShare)
{
    voxelWorkload.SecondaryShare = std::clamp(secondaryShare, 0.0f, 1.0f);
    pendingRuntimeChanges.VoxelWorkloadSettings = true;
    ApplyPendingVoxelSettings();
}

void VoxelWaterfallApp::ApplyBenchmarkSpatialLodEnabled(const bool enabled)
{
    voxelWorkload.SpatialLod.Mode = enabled
                                        ? VoxelSpatialLodMode::ThreeLevel
                                        : VoxelSpatialLodMode::Off;
    voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::None;
    voxelCompositeDebugView = VoxelCompositeDebugView::FinalComposite;
}

void VoxelWaterfallApp::ApplyBenchmarkTemporalInterval(const uint32_t interval)
{
    voxelWorkload.TemporalPolicy = interval <= 1 ? VoxelTemporalPolicy::Full : VoxelTemporalPolicy::Decimated;
    voxelWorkload.TemporalDecimationInterval =
        voxelWorkload.TemporalPolicy == VoxelTemporalPolicy::Decimated
            ? std::clamp<uint32_t>(interval, 2, 16)
            : 1;
    pendingRuntimeChanges.VoxelWorkloadSettings = true;
    ApplyPendingVoxelSettings();
}

BenchmarkConfigurationApplyResult VoxelWaterfallApp::ApplyBenchmarkConfigurationAtomic(
    const AutomaticBenchmarkConfig& config)
{
    BenchmarkConfigurationApplyResult result{};
    result.ActualMode = GetExecutionModeName(executionMode);

    Flush();

    auto defaultDynamicBudgetForStaticBudget = [](const uint32_t staticBudget)
    {
        if (staticBudget <= 100000)
            return 25000u;
        if (staticBudget <= 250000)
            return 100000u;
        if (staticBudget <= 500000)
            return 250000u;
        return 500000u;
    };
    result.RequestedLabelCount =
        config.RequestedLabelCount != 0 ? config.RequestedLabelCount : config.TotalCount;
    result.RequestedStaticBudget =
        config.RequestedStaticBudget != 0 ? config.RequestedStaticBudget : config.TotalCount;
    const auto activePreset = voxelResearchSceneManager.GetActivePreset();
    if (config.RequestedDynamicBudget != 0)
    {
        result.RequestedDynamicBudget = config.RequestedDynamicBudget;
    }
    else if (activePreset == VoxelResearchScenePreset::MixedVoxelEnvironment ||
             activePreset == VoxelResearchScenePreset::DemoMixed)
    {
        result.RequestedDynamicBudget = defaultDynamicBudgetForStaticBudget(result.RequestedStaticBudget);
    }
    else if (activePreset == VoxelResearchScenePreset::DynamicWaterfall)
    {
        result.RequestedDynamicBudget = config.TotalCount;
    }

    const int requestedStaticBudget = static_cast<int>(result.RequestedStaticBudget);
    const int requestedDynamicBudget = static_cast<int>(result.RequestedDynamicBudget);
    if (activePreset == VoxelResearchScenePreset::StaticVoxelEnvironment)
        voxelWorkload.StaticVoxelBudget = static_cast<uint32_t>(std::max(1, requestedStaticBudget));
    else if (activePreset == VoxelResearchScenePreset::MixedVoxelEnvironment ||
             activePreset == VoxelResearchScenePreset::DemoMixed)
    {
        voxelWorkload.StaticVoxelBudget = static_cast<uint32_t>(std::max(1, requestedStaticBudget));
        voxelWorkload.DynamicVoxelBudget = static_cast<uint32_t>(std::max(0, requestedDynamicBudget));
    }
    else if (activePreset == VoxelResearchScenePreset::DynamicWaterfall)
        voxelWorkload.DynamicVoxelBudget = static_cast<uint32_t>(std::max(0, requestedDynamicBudget));
    else if (activePreset == VoxelResearchScenePreset::SpatialLodDemonstration)
        voxelWorkload.StaticVoxelBudget = static_cast<uint32_t>(std::max(1, requestedStaticBudget));
    else
        voxelWorkload.TotalVoxelCount =
            static_cast<uint32_t>(std::max(0, static_cast<int>(result.RequestedLabelCount)));

    voxelWorkload.SecondaryShare = std::clamp(config.SecondaryShare, 0.0f, 1.0f);
    voxelWorkload.SpatialLod.Mode =
        config.SpatialLodEnabled ? VoxelSpatialLodMode::ThreeLevel : VoxelSpatialLodMode::Off;
    voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::None;
    voxelCompositeDebugView = VoxelCompositeDebugView::FinalComposite;
    const bool temporalMode = config.Mode == VoxelExecutionMode::SingleGpuTemporalDecimation ||
        config.Mode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    voxelWorkload.TemporalPolicy = temporalMode ? VoxelTemporalPolicy::Decimated : VoxelTemporalPolicy::Full;
    voxelWorkload.TemporalDecimationInterval = std::clamp<uint32_t>(config.TemporalInterval, 1, 16);

    requestedExecutionMode = config.Mode;
    executionMode = config.Mode;
    if ((config.Mode == VoxelExecutionMode::MultiGpuFull ||
         config.Mode == VoxelExecutionMode::MultiGpuTemporalDecimation) &&
        (!multiGpuAvailable || !multiGpuVoxelRenderTargets.IsInitialized()))
    {
        if (multiGpuAvailable && !multiGpuVoxelRenderTargets.IsInitialized())
            RebuildMultiGpuVoxelRenderTargets();
        if (!multiGpuAvailable || !multiGpuVoxelRenderTargets.IsInitialized())
        {
            executionMode = config.Mode == VoxelExecutionMode::MultiGpuFull
                                ? VoxelExecutionMode::SingleGpuFull
                                : VoxelExecutionMode::SingleGpuTemporalDecimation;
        }
    }

    pendingRuntimeChanges.VoxelWorkloadSettings = true;
    ApplyPendingVoxelSettings();
    ResetBenchmarkDeterministicState();
    Flush();

    result.ActualMode = GetExecutionModeName(executionMode);
    result.ActualStaticCount = voxelWorkload.ActualStaticVoxelCount;
    result.ActualDynamicCount = voxelWorkload.ActualDynamicVoxelCount;
    result.ActualTotalCount = voxelWorkload.TotalVoxelCount;

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "{\"actual_dynamic_count\":" << result.ActualDynamicCount
           << ",\"actual_mode\":\"" << result.ActualMode
           << "\",\"actual_static_count\":" << result.ActualStaticCount
           << ",\"actual_total_count\":" << result.ActualTotalCount
           << ",\"requested_dynamic_budget\":" << result.RequestedDynamicBudget
           << ",\"requested_mode\":\"" << config.ModeName
           << "\",\"requested_static_budget\":" << result.RequestedStaticBudget
           << ",\"requested_static_budget_label\":" << result.RequestedLabelCount
           << ",\"secondary_share\":" << std::fixed << std::setprecision(8) << config.SecondaryShare
           << ",\"spatial_lod_enabled\":" << (config.SpatialLodEnabled ? "true" : "false")
           << ",\"temporal_interval\":" << config.TemporalInterval << "}";
    result.ResolvedConfigHash = ResearchProvenance::Sha256Hex(stream.str());
    currentBenchmarkResolvedConfigHash = result.ResolvedConfigHash;

    if (result.ActualMode != config.ModeName)
    {
        result.Reason = "requested mode does not match actual mode";
        return result;
    }
    if (result.ActualTotalCount != result.ActualStaticCount + result.ActualDynamicCount)
    {
        result.Reason = "actual total/static/dynamic counts are inconsistent";
        return result;
    }
    if (result.ActualTotalCount == 0)
    {
        result.Reason = "resolved configuration contains zero voxels";
        return result;
    }

    result.Passed = true;
    return result;
}

void VoxelWaterfallApp::ApplyExecutionMode(const VoxelExecutionMode requestedMode)
{
    assert(!isDrawingFrame && "ApplyExecutionMode must not be called during DrawFrame");
    requestedExecutionMode = requestedMode;
    VoxelExecutionMode targetMode = requestedMode;
    const bool requestsMultiGpu = targetMode == VoxelExecutionMode::MultiGpuFull ||
        targetMode == VoxelExecutionMode::MultiGpuTemporalDecimation;
    if (requestsMultiGpu && !multiGpuAvailable)
    {
        targetMode = targetMode == VoxelExecutionMode::MultiGpuFull
                         ? VoxelExecutionMode::SingleGpuFull
                         : VoxelExecutionMode::SingleGpuTemporalDecimation;
        if (multiGpuStatus.empty())
            multiGpuStatus = L"MultiGpu unavailable: secondary render target capability validation failed";
    }

    if (requestsMultiGpu && multiGpuAvailable && !multiGpuVoxelRenderTargets.IsInitialized())
    {
        RebuildMultiGpuVoxelRenderTargets();
        if (!multiGpuVoxelRenderTargets.IsInitialized())
        {
            targetMode = targetMode == VoxelExecutionMode::MultiGpuFull
                             ? VoxelExecutionMode::SingleGpuFull
                             : VoxelExecutionMode::SingleGpuTemporalDecimation;
        }
    }

    if (executionMode == targetMode)
        return;

    executionMode = targetMode;
    voxelWorkload.SpatialLod.DebugMode = VoxelSpatialLodDebugMode::None;
    voxelCompositeDebugView = VoxelCompositeDebugView::FinalComposite;
    if (executionMode == VoxelExecutionMode::MultiGpuFull)
    {
        multiGpuStatus = L"MultiGpuFull adapter-local partitions active; secondary render-output transfer enabled";
    }
    else if (executionMode == VoxelExecutionMode::MultiGpuTemporalDecimation)
    {
        multiGpuStatus = L"MultiGpuTemporalDecimation adapter-local partitions active; secondary render-output transfer enabled";
    }
    else if (executionMode == VoxelExecutionMode::SingleGpuTemporalDecimation)
    {
        multiGpuStatus = L"SingleGpuTemporalDecimation active";
    }
    else
    {
        multiGpuStatus = L"SingleGpuFull active";
    }
    RebuildGpuPartitionsForMode();
    voxelSimulationAccumulator = 0.0;
    voxelSimulationTime = 0.0;
    voxelSimulationStepsThisFrame = 0;
    voxelInterpolationAlpha = 0.0f;
    voxelRecycledCount = 0;
    voxelAliveCount = 0;
    voxelExpectedCount = voxelWorkload.TotalVoxelCount;
    simulationFrameIndex = 0;
    spatialLodCameraInitialized = false;
    logQueue.Push(L"\nVoxel execution mode changed: " + multiGpuStatus);
}

void VoxelWaterfallApp::ApplyPendingVoxelSettings()
{
    assert(!isDrawingFrame && "ApplyPendingVoxelSettings must not be called during DrawFrame");
    if (!pendingRuntimeChanges.VoxelWorkloadSettings.value_or(false))
        return;

    const StaticLayerSnapshot staticLayerBefore = CaptureStaticLayerSnapshot(voxelWorkload);

    Flush();
    const auto activePreset = voxelResearchSceneManager.GetActivePreset();
    if (activePreset == VoxelResearchScenePreset::StaticVoxelEnvironment ||
        activePreset == VoxelResearchScenePreset::MixedVoxelEnvironment ||
        activePreset == VoxelResearchScenePreset::DemoMixed ||
        activePreset == VoxelResearchScenePreset::OcclusionValidation ||
        activePreset == VoxelResearchScenePreset::SpatialLodDemonstration)
    {
        VoxelResearchEnvironmentGenerator::Settings settings{};
        settings.Seed = voxelWorkload.StaticGenerationSeed;
        settings.StaticVoxelBudget = voxelWorkload.StaticVoxelBudget;
        settings.BudgetPreset = voxelWorkload.StaticBudgetPreset;
        settings.StorageMode = voxelWorkload.StaticStorageMode;
        settings.VoxelSize = voxelWorkload.StaticVoxelSize;
        settings.SecondaryShare = voxelWorkload.SecondaryShare;
        settings.PartitionStrategy = voxelWorkload.PartitionStrategy;
        settings.LoadBalanceScenario = voxelWorkload.LoadBalanceScenario;
        settings.ChunkSize = voxelWorkload.ChunkSize;
        settings.SpatialLod = voxelWorkload.SpatialLod;
        auto generated = VoxelResearchEnvironmentGenerator::Generate(settings);
        voxelWorkload.Layers.clear();
        voxelWorkload.Layers.push_back(std::move(generated.Layer));
        voxelWorkload.StaticTelemetry = generated.Telemetry;
    }
    else
    {
        voxelWorkload.Layers.clear();
    }
    if (activePreset == VoxelResearchScenePreset::StaticVoxelEnvironment ||
        activePreset == VoxelResearchScenePreset::SpatialLodDemonstration)
        voxelWorkload.DynamicVoxelBudget = 0;
    voxelWorkload = VoxelSceneWorkloadBuilder::Build(voxelWorkload);
    ++voxelSceneGeneration;
    AssertStaticLayerShapePreserved(staticLayerBefore, voxelWorkload);
    RebuildGpuPartitionsForMode();
    voxelSimulationAccumulator = 0.0;
    voxelSimulationTime = 0.0;
    voxelSimulationStepsThisFrame = 0;
    voxelInterpolationAlpha = 0.0f;
    voxelRecycledCount = 0;
    voxelAliveCount = 0;
    voxelExpectedCount = voxelWorkload.TotalVoxelCount;
    simulationFrameIndex = 0;
    spatialLodCameraInitialized = false;
    pendingRuntimeChanges.VoxelWorkloadSettings.reset();
}

void VoxelWaterfallApp::RebuildGpuPartitionsForMode()
{
    assert(!isDrawingFrame && "Voxel GPU partition rebuild must not run during DrawFrame");
    ++voxelPartitionGeneration;
    auto voxelObjectIt = std::find_if(
        gameObjects.begin(),
        gameObjects.end(),
        [](const std::shared_ptr<GameObject>& object)
        {
            return object && object->GetName() == VoxelSceneGpuObjectName;
        });

    if (voxelObjectIt == gameObjects.end())
    {
        assert(voxelWorkload.TotalVoxelCount == 0 &&
               "Dynamic voxel object must exist before rebuilding non-empty GPU partitions");
        for (auto& partition : voxelWorkload.Partitions)
        {
            partition.AdapterOwner = AdapterOwnerForPartition(executionMode, multiGpuAvailable, partition);
            partition.GpuPartition.reset();
        }
        return;
    }

    (*voxelObjectIt)->RemoveComponentsOfType<VoxelGpuPartition>();
    for (auto& partition : voxelWorkload.Partitions)
        partition.GpuPartition.reset();

    for (auto& partition : voxelWorkload.Partitions)
    {
        if (partition.VoxelCount() == 0)
            continue;

        partition.AdapterOwner = AdapterOwnerForPartition(executionMode, multiGpuAvailable, partition);
        const auto ownerDevice = DeviceForAdapterOwner(partition.AdapterOwner, primeDevice, secondDevice);
        assert(ownerDevice && "VoxelGpuPartition owner device must be valid");

        auto gpuPartition = std::make_shared<VoxelGpuPartition>(
            ownerDevice,
            partition.DrawStreams,
            partition.AdapterOwner);
        gpuPartition->SetSimulationEnabled(partition.HasDynamicVoxels() && partition.SimulationPolicy.Enabled);
        gpuPartition->SetRenderEnabled(partition.VoxelCount() > 0);
        (*voxelObjectIt)->AddComponent(gpuPartition);

        partition.GpuPartition = std::move(gpuPartition);
    }

    const auto modeNameUtf8 = GetExecutionModeName(executionMode);
    logQueue.Push(L"\nVoxel GPU partitions rebuilt for " +
        std::wstring(modeNameUtf8.begin(), modeNameUtf8.end()));
    if (SlowVoxelValidationEnabled())
        ValidateVoxelWorkloadGlobalIdsSlow();
}

VoxelFrameRenderPlan VoxelWaterfallApp::BuildVoxelFrameRenderPlan() const
{
    VoxelFrameRenderPlan renderPlan{};
    renderPlan.SceneGeneration = voxelSceneGeneration;
    renderPlan.PartitionGeneration = voxelPartitionGeneration;
    renderPlan.Profile = voxelWorkload.Profile;
    renderPlan.ExecutionMode = executionMode;
    for (const auto& partition : voxelWorkload.Partitions)
    {
        VoxelFramePartitionRenderPlan partitionPlan{};
        partitionPlan.PartitionId = partition.PartitionId;
        partitionPlan.AdapterOwner = partition.AdapterOwner;
        partitionPlan.LogicalVoxelCount = partition.VoxelCount();
        partitionPlan.DrawStreams = partition.DrawStreams;
        partitionPlan.GpuPartition = partition.GpuPartition;
        partitionPlan.SceneGeneration = voxelSceneGeneration;
        partitionPlan.PartitionGeneration = voxelPartitionGeneration;

        renderPlan.LogicalVoxelCount += partitionPlan.LogicalVoxelCount;
        if (partition.AdapterOwner == VoxelAdapterOwner::Secondary)
            renderPlan.SecondaryOwnedPartitions.push_back(std::move(partitionPlan));
        else
            renderPlan.PrimaryOwnedPartitions.push_back(std::move(partitionPlan));
    }
    return renderPlan;
}

void VoxelWaterfallApp::ValidateVoxelFrameRenderPlan(const VoxelFrameRenderPlan& renderPlan) const
{
    if (typedRenderer.size() > static_cast<size_t>(RenderMode::Particle))
    {
        for (const auto& renderer : typedRenderer[static_cast<int>(RenderMode::Particle)])
            assert(dynamic_cast<VoxelGpuPartition*>(renderer.get()) == nullptr &&
                   "VoxelGpuPartition must not be registered in the generic renderer registry");
    }

    for (size_t i = 0; i < renderPlan.PrimaryOwnedPartitions.size(); ++i)
    {
        const auto& partition = renderPlan.PrimaryOwnedPartitions[i];
        assert(partition.AdapterOwner == VoxelAdapterOwner::Primary);
        for (size_t j = i + 1; j < renderPlan.PrimaryOwnedPartitions.size(); ++j)
        {
            assert(partition.PartitionId != renderPlan.PrimaryOwnedPartitions[j].PartitionId &&
                   "Duplicate primary voxel partition draw entry");
        }
    }

    for (const auto& partition : renderPlan.SecondaryOwnedPartitions)
    {
        assert(partition.AdapterOwner == VoxelAdapterOwner::Secondary);
        for (const auto& primaryPartition : renderPlan.PrimaryOwnedPartitions)
        {
            assert(partition.PartitionId != primaryPartition.PartitionId &&
                   "Voxel partition is present in both primary and secondary draw lists");
        }
    }

    auto validatePartition = [this, &renderPlan](const VoxelFramePartitionRenderPlan& partition)
    {
        assert(partition.LogicalVoxelCount == 0 || partition.GpuPartition &&
               "Non-empty voxel partition must have adapter-local GPU object");
        if (!partition.GpuPartition)
            return;
        assert(partition.GpuPartition->GetAdapterOwner() == partition.AdapterOwner);
        const bool hasDynamicVoxels = std::any_of(
            partition.DrawStreams.begin(),
            partition.DrawStreams.end(),
            [](const VoxelPartitionDrawStream& stream)
            {
                return stream.LayerType == VoxelSceneLayerType::Dynamic && stream.VoxelCount() > 0;
            });
        assert(hasDynamicVoxels || !partition.GpuPartition->IsSimulationEnabled());

        const auto expectedDevice = DeviceForAdapterOwner(partition.AdapterOwner, primeDevice, secondDevice);
        const auto actualDevice = partition.GpuPartition->GetOwningDevice();
        assert(expectedDevice && actualDevice);
        assert(SameAdapterLuid(expectedDevice->GetDesc().AdapterLuid, actualDevice->GetDesc().AdapterLuid) &&
               "Voxel partition GPU object owner device does not match adapter owner");
        assert(partition.SceneGeneration == renderPlan.SceneGeneration);
        assert(partition.PartitionGeneration == renderPlan.PartitionGeneration);
    };
    for (const auto& partition : renderPlan.PrimaryOwnedPartitions)
        validatePartition(partition);
    for (const auto& partition : renderPlan.SecondaryOwnedPartitions)
        validatePartition(partition);

    if (executionMode == VoxelExecutionMode::MultiGpuFull ||
        executionMode == VoxelExecutionMode::MultiGpuTemporalDecimation)
    {
        for (const auto& partition : renderPlan.PrimaryOwnedPartitions)
            assert(partition.PartitionId != VoxelAdapterPartitionId::SecondaryPartition &&
                   "Multi-GPU primary draw list must not contain SecondaryPartition");
        for (const auto& partition : renderPlan.SecondaryOwnedPartitions)
            assert(partition.PartitionId != VoxelAdapterPartitionId::PrimaryPartition &&
                   "Multi-GPU secondary draw list must not contain PrimaryPartition");
    }

    if ((executionMode == VoxelExecutionMode::SingleGpuFull ||
        executionMode == VoxelExecutionMode::SingleGpuTemporalDecimation)
        && renderPlan.LogicalVoxelCount > 0)
    {
        assert(renderPlan.SecondaryOwnedPartitions.empty() &&
               "Single-GPU mode must not submit secondary graphics draw list");
        assert(renderPlan.PrimaryOwnedPartitions.size() == VoxelAdapterPartitionCount &&
               "Single-GPU primary draw list must contain both logical voxel partitions");
    }
}

void VoxelWaterfallApp::ValidateVoxelFrameDrawResultsCheap(
    const VoxelFrameRenderPlan& renderPlan,
    const std::vector<VoxelPartitionRenderResult>& primaryResults,
    const std::vector<VoxelPartitionRenderResult>& secondaryResults,
    const bool secondaryGraphicsSubmitted) const
{
    uint32_t expectedPartitionVoxelCount = 0;
    for (const auto& partition : renderPlan.PrimaryOwnedPartitions)
        expectedPartitionVoxelCount += partition.LogicalVoxelCount;
    for (const auto& partition : renderPlan.SecondaryOwnedPartitions)
        expectedPartitionVoxelCount += partition.LogicalVoxelCount;

    uint32_t logicalDrawListVoxelCount = 0;
    uint32_t submittedVoxelCount = 0;
    auto accumulateResultCounts = [&logicalDrawListVoxelCount, &submittedVoxelCount](
        const std::vector<VoxelPartitionRenderResult>& results)
    {
        for (const auto& result : results)
        {
            logicalDrawListVoxelCount += result.LogicalVoxelCount;
            submittedVoxelCount += result.SubmittedVoxelCount;
        }
    };
    accumulateResultCounts(primaryResults);
    accumulateResultCounts(secondaryResults);

    auto emitValidationDiagnostics = [&](const char* reason)
    {
        std::ostringstream stream;
        stream << "[VoxelFrameDrawValidation] " << reason
               << " frame_index=" << simulationFrameIndex
               << " profile=" << ProfileName(renderPlan.Profile)
               << " mode=" << ExecutionModeNameLiteral(renderPlan.ExecutionMode)
               << " expected_count=" << expectedPartitionVoxelCount
               << " logical_result_count=" << logicalDrawListVoxelCount
               << " submitted_count=" << submittedVoxelCount
               << " frame_scene_generation=" << renderPlan.SceneGeneration
               << " frame_partition_generation=" << renderPlan.PartitionGeneration
               << "\n  plan_partitions=";
        auto appendPlanPartition = [&stream](const char* queueName, const VoxelFramePartitionRenderPlan& partition)
        {
            stream << queueName << '{'
                   << "id=" << PartitionIdName(partition.PartitionId)
                   << ",owner=" << AdapterOwnerName(partition.AdapterOwner)
                   << ",expected=" << partition.LogicalVoxelCount
                   << ",sceneGen=" << partition.SceneGeneration
                   << ",partitionGen=" << partition.PartitionGeneration
                   << "} ";
        };
        for (const auto& partition : renderPlan.PrimaryOwnedPartitions)
            appendPlanPartition("primary", partition);
        for (const auto& partition : renderPlan.SecondaryOwnedPartitions)
            appendPlanPartition("secondary", partition);

        stream << "\n  draw_results=";
        auto appendResult = [&stream](const char* queueName, const VoxelPartitionRenderResult& result)
        {
            stream << queueName << '{'
                   << "id=" << PartitionIdName(result.PartitionId)
                   << ",owner=" << AdapterOwnerName(result.OwnerAdapter)
                   << ",logical=" << result.LogicalVoxelCount
                   << ",submitted=" << result.SubmittedVoxelCount
                   << ",draws=" << result.DrawCallCount
                   << ",sceneGen=" << result.SceneGeneration
                   << ",partitionGen=" << result.PartitionGeneration
                   << "} ";
        };
        for (const auto& result : primaryResults)
            appendResult("primary", result);
        for (const auto& result : secondaryResults)
            appendResult("secondary", result);
        stream << '\n';
        OutputDebugStringA(stream.str().c_str());
    };

    auto resultHasMixedGeneration = [&renderPlan](const VoxelPartitionRenderResult& result)
    {
        return result.SceneGeneration != renderPlan.SceneGeneration ||
            result.PartitionGeneration != renderPlan.PartitionGeneration;
    };
    const bool mixedFrameGenerations =
        std::any_of(primaryResults.begin(), primaryResults.end(), resultHasMixedGeneration) ||
        std::any_of(secondaryResults.begin(), secondaryResults.end(), resultHasMixedGeneration);
    if (mixedFrameGenerations)
        emitValidationDiagnostics("mixed frame generations");
    assert(!mixedFrameGenerations && "mixed frame generations");

    if (logicalDrawListVoxelCount != expectedPartitionVoxelCount)
        emitValidationDiagnostics("Frame voxel draw lists do not cover the current partition workload count");
    assert(logicalDrawListVoxelCount == expectedPartitionVoxelCount &&
           "Frame voxel draw lists do not cover the current partition workload count");

    uint32_t secondaryDrawCallCount = 0;

    size_t primaryResultIndex = 0;
    for (const auto& partition : renderPlan.PrimaryOwnedPartitions)
    {
        if (partition.LogicalVoxelCount == 0)
            continue;
        if (primaryResultIndex >= primaryResults.size())
            emitValidationDiagnostics("Primary voxel command list did not record an expected partition");
        assert(primaryResultIndex < primaryResults.size() &&
               "Primary voxel command list did not record an expected partition");
        const auto& result = primaryResults[primaryResultIndex++];
        if (result.PartitionId != partition.PartitionId)
            emitValidationDiagnostics("Primary voxel render result does not match the expected partition id");
        assert(result.PartitionId == partition.PartitionId &&
               "Primary voxel render result does not match the expected partition id");
        if (result.LogicalVoxelCount != partition.LogicalVoxelCount)
            emitValidationDiagnostics("Primary voxel render result does not match the expected logical voxel count");
        assert(result.LogicalVoxelCount == partition.LogicalVoxelCount &&
               "Primary voxel render result does not match the expected logical voxel count");
    }
    if (primaryResultIndex != primaryResults.size())
        emitValidationDiagnostics("Primary voxel command list recorded unexpected extra partition results");
    assert(primaryResultIndex == primaryResults.size() &&
           "Primary voxel command list recorded unexpected extra partition results");

    size_t secondaryResultIndex = 0;
    for (const auto& partition : renderPlan.SecondaryOwnedPartitions)
    {
        if (partition.LogicalVoxelCount == 0)
            continue;
        if (secondaryResultIndex >= secondaryResults.size())
            emitValidationDiagnostics("Secondary voxel command list did not record an expected partition");
        assert(secondaryResultIndex < secondaryResults.size() &&
               "Secondary voxel command list did not record an expected partition");
        const auto& result = secondaryResults[secondaryResultIndex++];
        if (result.PartitionId != partition.PartitionId)
            emitValidationDiagnostics("Secondary voxel render result does not match the expected partition id");
        assert(result.PartitionId == partition.PartitionId &&
               "Secondary voxel render result does not match the expected partition id");
        if (result.LogicalVoxelCount != partition.LogicalVoxelCount)
            emitValidationDiagnostics("Secondary voxel render result does not match the expected logical voxel count");
        assert(result.LogicalVoxelCount == partition.LogicalVoxelCount &&
               "Secondary voxel render result does not match the expected logical voxel count");
        secondaryDrawCallCount += result.DrawCallCount;
    }
    if (secondaryResultIndex != secondaryResults.size())
        emitValidationDiagnostics("Secondary voxel command list recorded unexpected extra partition results");
    assert(secondaryResultIndex == secondaryResults.size() &&
           "Secondary voxel command list recorded unexpected extra partition results");

    if (submittedVoxelCount > expectedPartitionVoxelCount)
        emitValidationDiagnostics("Submitted voxel draw count exceeds logical workload count");
    assert(submittedVoxelCount <= expectedPartitionVoxelCount &&
           "Submitted voxel draw count exceeds logical workload count");

    if (executionMode == VoxelExecutionMode::MultiGpuFull ||
        executionMode == VoxelExecutionMode::MultiGpuTemporalDecimation)
    {
        for (const auto& result : primaryResults)
            assert(result.PartitionId != VoxelAdapterPartitionId::SecondaryPartition &&
                   "Multi-GPU primary command list recorded SecondaryPartition");
        for (const auto& result : secondaryResults)
            assert(result.PartitionId != VoxelAdapterPartitionId::PrimaryPartition &&
                   "Multi-GPU secondary command list recorded PrimaryPartition");

        const bool hasSecondaryLogicalVoxels = std::any_of(
            renderPlan.SecondaryOwnedPartitions.begin(),
            renderPlan.SecondaryOwnedPartitions.end(),
            [](const VoxelFramePartitionRenderPlan& partition)
            {
                return partition.LogicalVoxelCount > 0;
            });
        if (hasSecondaryLogicalVoxels && simulationFrameIndex > 0)
        {
            assert(secondaryGraphicsSubmitted &&
                   "Multi-GPU secondary graphics queue did not receive a command list");
            assert(secondaryDrawCallCount > 0 &&
                   "Multi-GPU secondary graphics command list did not issue a draw command");
        }
    }

    if (executionMode == VoxelExecutionMode::SingleGpuFull ||
        executionMode == VoxelExecutionMode::SingleGpuTemporalDecimation)
    {
        assert(!secondaryGraphicsSubmitted &&
               "Single-GPU mode must not submit secondary graphics queue");
        assert(secondaryResults.empty() &&
               "Single-GPU mode must not record secondary voxel draw results");
    }
}

void VoxelWaterfallApp::InitFrameResource()
{
    for (int i = 0; i < globalCountFrameResources; ++i)
    {
        frameResources.push_back(std::make_unique<FrameResource>(primeDevice,
                                                                  secondDevice ? secondDevice : primeDevice, 2,
                                                                  static_cast<UINT>(assets->GetMaterials().size())));
    }
    logQueue.Push(std::wstring(L"\nInit FrameResource "));
}

void VoxelWaterfallApp::InitRootSignature()
{
    auto rootSignature = std::make_shared<GRootSignature>();
    CD3DX12_DESCRIPTOR_RANGE texParam[4];
    texParam[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, StandardShaderSlot::SkyMap - 3, 0); //SkyMap
    texParam[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, StandardShaderSlot::ShadowMap - 3, 0); //ShadowMap
    texParam[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, StandardShaderSlot::AmbientMap - 3, 0); //SsaoMap
    texParam[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                     static_cast<UINT>(assets->GetLoadTexturesCount() > 0 ? assets->GetLoadTexturesCount() : 1),
                     StandardShaderSlot::TexturesMap - 3, 0);


    rootSignature->AddConstantBufferParameter(0);
    rootSignature->AddConstantBufferParameter(1);
    rootSignature->AddShaderResourceView(0, 1);
    rootSignature->AddDescriptorParameter(&texParam[0], 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rootSignature->AddDescriptorParameter(&texParam[1], 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rootSignature->AddDescriptorParameter(&texParam[2], 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rootSignature->AddDescriptorParameter(&texParam[3], 1, D3D12_SHADER_VISIBILITY_PIXEL);
    rootSignature->Initialize(primeDevice);

    primeDeviceSignature = rootSignature;


    logQueue.Push(std::wstring(L"\nInit RootSignature for " + primeDevice->GetName()));

    ssaoPrimeRootSignature = std::make_shared<GRootSignature>();

    CD3DX12_DESCRIPTOR_RANGE texTable0;
    texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0);

    CD3DX12_DESCRIPTOR_RANGE texTable1;
    texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0);

    ssaoPrimeRootSignature->AddConstantBufferParameter(0);
    ssaoPrimeRootSignature->AddConstantParameter(1, 1);
    ssaoPrimeRootSignature->AddDescriptorParameter(&texTable0, 1, D3D12_SHADER_VISIBILITY_PIXEL);
    ssaoPrimeRootSignature->AddDescriptorParameter(&texTable1, 1, D3D12_SHADER_VISIBILITY_PIXEL);

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        0, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        1, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC depthMapSam(
        2, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, // addressW
        0.0f,
        0,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        3, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> staticSamplers =
    {
        pointClamp, linearClamp, depthMapSam, linearWrap
    };

    for (auto&& sampler : staticSamplers)
    {
        ssaoPrimeRootSignature->AddStaticSampler(sampler);
    }

    ssaoPrimeRootSignature->Initialize(primeDevice);
}

void VoxelWaterfallApp::InitPipeLineResource()
{
    defaultInputLayout =
    {
        {
            "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        },
        {
            "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        },
        {
            "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        },
        {
            "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        },
    };

    const D3D12_INPUT_LAYOUT_DESC desc = {
        defaultInputLayout.data(), static_cast<UINT>(defaultInputLayout.size())
    };

    defaultPrimePipelineResources = RenderModeFactory();
    defaultPrimePipelineResources.LoadDefaultShaders();
    defaultPrimePipelineResources.LoadDefaultPSO(primeDevice, primeDeviceSignature, desc,
                                                 BackBufferFormat, DepthStencilFormat, ssaoPrimeRootSignature,
                                                 NormalMapFormat, AmbientMapFormat);

    ambientPrimePath->SetPipelineData(*defaultPrimePipelineResources.GetPSO(RenderMode::Ssao),
                                      *defaultPrimePipelineResources.GetPSO(RenderMode::SsaoBlur));

    voxelCompositePass.Initialize(primeDevice, GetSRGBFormat(BackBufferFormat));

    logQueue.Push(std::wstring(L"\nInit PSO for " + primeDevice->GetName()));

    const auto primeDeviceShadowMapPso = defaultPrimePipelineResources.GetPSO(RenderMode::ShadowMapOpaque);
}

void VoxelWaterfallApp::CreateMaterials()
{
    auto seamless = std::make_shared<Material>(L"seamless", RenderMode::Opaque);
    seamless->FresnelR0 = Vector3(0.02f, 0.02f, 0.02f);
    seamless->Roughness = 0.1f;

    auto tex = assets->GetTextureIndex(L"seamless");
    seamless->SetDiffuseTexture(assets->GetTexture(tex), tex);

    tex = assets->GetTextureIndex(L"defaultNormalMap");

    seamless->SetNormalMap(assets->GetTexture(tex), tex);
    assets->AddMaterial(seamless);

    auto matteOccluder = std::make_shared<Material>(L"matteOccluder", RenderMode::Opaque);
    matteOccluder->DiffuseAlbedo = Vector4(0.36f, 0.35f, 0.33f, 1.0f);
    matteOccluder->FresnelR0 = Vector3(0.01f, 0.01f, 0.01f);
    matteOccluder->Roughness = 0.9f;
    tex = assets->GetTextureIndex(L"seamless");
    matteOccluder->SetDiffuseTexture(assets->GetTexture(tex), tex);
    tex = assets->GetTextureIndex(L"defaultNormalMap");
    matteOccluder->SetNormalMap(assets->GetTexture(tex), tex);
    assets->AddMaterial(matteOccluder);

    models[L"quad"]->SetMeshMaterial(0, assets->GetMaterial(assets->GetMaterialIndex(L"seamless")));
    if (models.find(L"box") != models.end())
        models[L"box"]->SetMeshMaterial(0, assets->GetMaterial(assets->GetMaterialIndex(L"matteOccluder")));

    logQueue.Push(std::wstring(L"\nCreate Materials"));
}

void VoxelWaterfallApp::InitSRVMemoryAndMaterials()
{
    srvTexturesMemory =
        primeDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         static_cast<uint32_t>(assets->GetTextures().size()));

    auto materials = assets->GetMaterials();

    for (size_t j = 0; j < materials.size(); ++j)
    {
        auto material = materials[j];

        material->InitMaterial(&srvTexturesMemory);
    }

    logQueue.Push(std::wstring(L"\nInit Views for " + primeDevice->GetName()));
    ambientPrimePath->BuildDescriptors();
}

void VoxelWaterfallApp::InitRenderPaths()
{
    auto commandQueue = primeDevice->GetCommandQueue(GQueueType::Graphics);
    auto cmdList = commandQueue->GetCommandList();

    ambientPrimePath = (std::make_shared<SSAO>(
        primeDevice,
        cmdList,
        MainWindow->GetClientWidth(), MainWindow->GetClientHeight()));

    antiAliasingPrimePath = (std::make_shared<SSAA>(primeDevice, 4, MainWindow->GetClientWidth(),
                                                    MainWindow->GetClientHeight()));
    antiAliasingPrimePath->OnResize(MainWindow->GetClientWidth(), MainWindow->GetClientHeight());

    commandQueue->WaitForFenceValue(commandQueue->ExecuteCommandList(cmdList));

    logQueue.Push(std::wstring(L"\nInit Render path data for " + primeDevice->GetName()));

    shadowPath = (std::make_shared<ShadowMap>(primeDevice, 2048, 2048));
    RebuildMultiGpuVoxelRenderTargets();
}

MultiGpuVoxelRenderTargetDesc VoxelWaterfallApp::BuildMultiGpuVoxelRenderTargetDesc() const
{
    MultiGpuVoxelRenderTargetDesc desc{};
    desc.FrameCount = globalCountFrameResources;
    desc.ColorFormat = GetSRGBFormat(BackBufferFormat);
    desc.LinearDepthFormat = DXGI_FORMAT_R32_FLOAT;
    desc.DepthStencilFormat = DepthStencilFormat;
    desc.TransferMode = crossAdapterTransferMode;

    if (antiAliasingPrimePath)
    {
        const auto renderTargetDesc = antiAliasingPrimePath->GetRenderTarget().GetD3D12ResourceDesc();
        desc.Width = static_cast<UINT>(renderTargetDesc.Width);
        desc.Height = renderTargetDesc.Height;
        desc.ColorFormat = renderTargetDesc.Format;
    }
    else if (MainWindow)
    {
        desc.Width = MainWindow->GetClientWidth();
        desc.Height = MainWindow->GetClientHeight();
    }

    return desc;
}

void VoxelWaterfallApp::RebuildMultiGpuVoxelRenderTargets()
{
    assert(!isDrawingFrame && "Multi-GPU voxel render target rebuild must not run during DrawFrame");
    multiGpuVoxelRenderTargets.Reset();

    if (!multiGpuAvailable)
        return;

    const auto desc = BuildMultiGpuVoxelRenderTargetDesc();
    if (!multiGpuVoxelRenderTargets.Initialize(primeDevice, secondDevice, desc))
    {
        DisableMultiGpu(multiGpuVoxelRenderTargets.GetFailureMessage());
        return;
    }

    multiGpuStatus = L"MultiGpu hardware available; secondary voxel render targets allocated; transfer mode=" +
        std::wstring(CrossAdapterTransferModeNameW(crossAdapterTransferMode));
    logQueue.Push(L"\n" + multiGpuStatus);
    logQueue.Push(L"\nMultiGpu voxel render target frame sets: " +
        std::to_wstring(multiGpuVoxelRenderTargets.GetFrameCount()));
}

void VoxelWaterfallApp::DisableMultiGpu(const std::wstring& reason)
{
    multiGpuAvailable = false;
    crossAdapterTransferMode = CrossAdapterTransferMode::Unavailable;
    multiGpuPublicationEligible = false;
    executionMode = VoxelExecutionMode::SingleGpuFull;
    multiGpuStatus = reason.empty() ? L"MultiGpu unavailable: unknown capability failure" : reason;
    multiGpuVoxelRenderTargets.Reset();
    logQueue.Push(L"\n" + multiGpuStatus);
}

void VoxelWaterfallApp::LoadStudyTexture()
{
    auto queue = primeDevice->GetCommandQueue(GQueueType::Compute);

    const auto cmdList = queue->GetCommandList();

    for (const auto& entry : SampleAssetManifest::Textures())
    {
        auto texture = entry.IsNormalMap
                           ? GTexture::LoadTextureFromFile(ResolveVoxelWaterfallAssetPathW(entry.RelativePath.c_str()),
                                                           cmdList, TextureUsage::Normalmap)
                           : GTexture::LoadTextureFromFile(ResolveVoxelWaterfallAssetPathW(entry.RelativePath.c_str()),
                                                           cmdList);
        texture->SetName(entry.Name);
        assets->AddTexture(texture);
    }

    queue->WaitForFenceValue(queue->ExecuteCommandList(cmdList));

    logQueue.Push(std::wstring(L"\nLoad DDS Texture"));
}

void VoxelWaterfallApp::LoadModels()
{
    auto queue = primeDevice->GetCommandQueue(GQueueType::Compute);
    const auto cmdList = queue->GetCommandList();

    for (const auto& entry : SampleAssetManifest::Models())
    {
        auto model = assets->CreateModelFromFile(cmdList, ResolveVoxelWaterfallAssetPathA(entry.RelativePath.c_str()));
        model->scaleMatrix = entry.Scale;
        models[entry.Name] = std::move(model);
    }

    auto quad = assets->GenerateQuad(cmdList, -15.0f, -15.0f, 30.0f, 30.0f, 0.0f);
    models[L"quad"] = std::move(quad);
    auto box = assets->GenerateBox(cmdList, 1.0f, 1.0f, 1.0f);
    models[L"box"] = std::move(box);

    queue->WaitForFenceValue(queue->ExecuteCommandList(cmdList));
    queue->Flush();

    logQueue.Push(std::wstring(L"\nLoad Models Data"));
}

void VoxelWaterfallApp::GenerateMipMaps()
{
    try
    {
        std::vector<GTexture*> generatedMipTextures;

        auto textures = assets->GetTextures();

        for (auto&& texture : textures)
        {
            texture->ClearTrack();

            if (texture->GetD3D12Resource()->GetDesc().Flags != D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                continue;

            if (!texture->HasMipMap)
            {
                generatedMipTextures.push_back(texture.get());
            }
        }

        const auto computeQueue = primeDevice->GetCommandQueue(GQueueType::Compute);
        auto computeList = computeQueue->GetCommandList();
        GTexture::GenerateMipMaps(computeList, generatedMipTextures.data(), generatedMipTextures.size());
        computeQueue->WaitForFenceValue(computeQueue->ExecuteCommandList(computeList));
        logQueue.Push(std::wstring(L"\nMip Map Generation for " + primeDevice->GetName()));

        computeList = computeQueue->GetCommandList();
        for (auto&& texture : generatedMipTextures)
            computeList->TransitionBarrier(texture->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
        computeList->FlushResourceBarriers();
        logQueue.Push(std::wstring(L"\nTexture Barrier Generation for " + primeDevice->GetName()));
        computeQueue->WaitForFenceValue(computeQueue->ExecuteCommandList(computeList));

        logQueue.Push(std::wstring(L"\nMipMap Generation cmd list executing " + primeDevice->GetName()));
        for (auto&& pair : textures)
            pair->ClearTrack();
        logQueue.Push(std::wstring(L"\nFinish Mip Map Generation for " + primeDevice->GetName()));
    }
    catch (DxException& e)
    {
        logQueue.Push(L"\n" + e.Filename + L" " + e.FunctionName + L" " + std::to_wstring(e.LineNumber));
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
    }
    catch (...)
    {
        logQueue.Push(L"\nUnexpected error during mip-map generation");
    }
}

void VoxelWaterfallApp::SortGO()
{
    researchCameraController.reset();
    for (auto&& item : gameObjects)
    {
        auto light = item->GetComponent<Light>();
        if (light != nullptr)
        {
            lights.push_back(light.get());
        }

        auto cam = item->GetComponent<Camera>();
        if (cam != nullptr)
        {
            camera = (cam);
        }
        auto researchController = item->GetComponent<VoxelResearchCameraController>();
        if (researchController != nullptr)
        {
            researchCameraController = researchController;
        }
    }
}

void VoxelWaterfallApp::ValidateVoxelWorkloadGlobalIdsSlow() const
{
    ++slowFrameValidationCount;
    std::unordered_set<VoxelGlobalId> globalIds;
    globalIds.reserve(voxelWorkload.TotalVoxelCount);
    for (const auto& partition : voxelWorkload.Partitions)
    {
        for (const VoxelGlobalId globalVoxelId : partition.GlobalVoxelIds)
        {
            assert(globalIds.insert(globalVoxelId).second &&
                   "GlobalVoxelId appears in more than one logical partition");
        }
    }
    assert(globalIds.size() == voxelWorkload.TotalVoxelCount &&
           "Logical voxel partitions do not cover the workload exactly once");
}

void VoxelWaterfallApp::CreateGO()
{
    logQueue.Push(std::wstring(L"\nStart Create GO"));

    lights.clear();
    camera.reset();
    researchCameraController.reset();
    voxelResearchSceneManager.RequestPreset(VoxelResearchScenePreset::MixedVoxelEnvironment);
    VoxelResearchSceneContext sceneContext{
        primeDevice,
        secondDevice,
        *assets,
        models,
        srvTexturesMemory,
        gameObjects,
        typedRenderer,
        voxelWorkload,
        AspectRatio()
    };
    assert(!isDrawingFrame && "Voxel research scene rebuild must not run during DrawFrame");
    voxelResearchSceneManager.RebuildScene(sceneContext);
    ++voxelSceneGeneration;
    RebuildGpuPartitionsForMode();

#if defined(DEBUG) || defined(_DEBUG)
    RunSceneOwnershipSettingsSelfTest();
#endif

    logQueue.Push(std::wstring(L"\nFinish create GO"));
}

#if defined(DEBUG) || defined(_DEBUG)
void VoxelWaterfallApp::RunSceneOwnershipSettingsSelfTest()
{
    RunVoxelSpatialLodReferenceTests();
    RunVoxelSimulationSchedulerReferenceTests();
    RunVoxelWaterfallDynamicReferenceTests();

    if (voxelResearchSceneManager.GetActivePreset() != VoxelResearchScenePreset::MixedVoxelEnvironment)
        return;

    const VoxelExecutionMode originalRequestedMode = requestedExecutionMode;
    const float originalSecondaryShare = voxelWorkload.SecondaryShare;
    const VoxelSpatialLodSettings originalSpatialLod = voxelWorkload.SpatialLod;
    const VoxelTemporalPolicy originalTemporalPolicy = voxelWorkload.TemporalPolicy;
    const uint32_t originalTemporalInterval = voxelWorkload.TemporalDecimationInterval;
    const VoxelCompositeDebugView originalCompositeDebugView = voxelCompositeDebugView;
    const StaticLayerSnapshot before = CaptureStaticLayerSnapshot(voxelWorkload);

    assert(before.Valid);
    assert(voxelWorkload.SpatialLod.Mode == VoxelSpatialLodMode::Off);

    ApplyExecutionMode(VoxelExecutionMode::SingleGpuTemporalDecimation);
    ApplyBenchmarkSecondaryShare(0.25f);
    ApplyBenchmarkSpatialLodEnabled(true);
    pendingRuntimeChanges.VoxelWorkloadSettings = true;
    ApplyPendingVoxelSettings();
    ApplyBenchmarkTemporalInterval(4);
    AssertStaticLayerShapePreserved(before, voxelWorkload);

    ApplyExecutionMode(originalRequestedMode);
    voxelWorkload.SecondaryShare = originalSecondaryShare;
    voxelWorkload.SpatialLod = originalSpatialLod;
    voxelWorkload.TemporalPolicy = originalTemporalPolicy;
    voxelWorkload.TemporalDecimationInterval = originalTemporalInterval;
    voxelCompositeDebugView = originalCompositeDebugView;
    pendingRuntimeChanges.VoxelWorkloadSettings = true;
    ApplyPendingVoxelSettings();

    AssertStaticLayerShapePreserved(before, voxelWorkload);
    assert(voxelWorkload.SpatialLod.Mode == VoxelSpatialLodMode::Off);
}
#endif

void VoxelWaterfallApp::CalculateFrameStats()
{
    static float minFps = std::numeric_limits<float>::max();
    static float minMspf = std::numeric_limits<float>::max();
    static float maxFps = std::numeric_limits<float>::min();
    static float maxMspf = std::numeric_limits<float>::min();
    static UINT writeStatisticCount = 0;
    ++mainLoopIterationCount;

    if ((timer.TotalTime() - timeElapsed) >= 1.0f)
    {
        const uint64_t loopIterationsThisSecond = mainLoopIterationCount;
        const float fps = static_cast<float>(successfulPresentCount);
        const float mspf = fps > 0.0f ? 1000.0f / fps : 0.0f;

        minFps = std::min(fps, minFps);
        if (mspf > 0.0f)
            minMspf = std::min(mspf, minMspf);
        maxFps = std::max(fps, maxFps);
        maxMspf = std::max(mspf, maxMspf);

        frameCount = successfulPresentCount;
        successfulPresentCount = 0;
        mainLoopIterationCount = 0;
        timeElapsed += 1.0f;


        const std::string modeNameUtf8 = GetExecutionModeName();
        const std::wstring modeName(modeNameUtf8.begin(), modeNameUtf8.end());
        const std::wstring title = L"FPS " + std::to_wstring(fps) + L" Mode:" + modeName + L" Progress: " + std::to_wstring(
                (static_cast<float>(writeStatisticCount) / StatisticsStepSecondsCount) * 100.0f) + L"/" +
            std::to_wstring(100);

        if (writeStatisticCount >= static_cast<UINT>(StatisticsStepSecondsCount))
        {
            const std::wstring statisticsText =
                L"\nMode: " + modeName
                + L"\nMultiGpu status: " + multiGpuStatus
                + L"\n\tMin FPS:" + std::to_wstring(minFps)
                + L"\n\tMin MSPF:" + std::to_wstring(minMspf)
                + L"\n\tMax FPS:" + std::to_wstring(maxFps)
                + L"\n\tMax MSPF:" + std::to_wstring(maxMspf);

            logQueue.Push(statisticsText);


            writeStatisticCount = 0;
            minFps = std::numeric_limits<float>::max();
            minMspf = std::numeric_limits<float>::max();
            maxFps = std::numeric_limits<float>::min();
            maxMspf = std::numeric_limits<float>::min();

        }
        else
        {
            const std::string actualModeNameUtf8 = GetExecutionModeName(frameGraphTelemetry.ActualMode);
            const std::wstring actualModeName(actualModeNameUtf8.begin(), actualModeNameUtf8.end());
            const std::wstring statisticsText =
                L"\n\tFPS:" + std::to_wstring(fps)
                + L"\n\tMain loop iterations:" + std::to_wstring(loopIterationsThisSecond)
                + L"\n\tMSPF:" + std::to_wstring(mspf)
                + L"\n\tActual mode:" + actualModeName;

            logQueue.Push(statisticsText);

            writeStatisticCount++;
        }


        MainWindow->SetWindowTitle(title);
    }
}

void VoxelWaterfallApp::LogWriting()
{
    const std::filesystem::path filePath(
        L"VoxelWaterfall " + primeDevice->GetName() + L"+" +
        (secondDevice ? secondDevice->GetName() : L"no-secondary") + L".txt");

    const auto path = std::filesystem::current_path().wstring() + L"\\" + filePath.wstring();

    OutputDebugStringW(path.c_str());

    std::wofstream fileSteam;
    fileSteam.open(path.c_str(), std::ios::out | std::ios::in | std::ios::binary | std::ios::trunc);
    if (fileSteam.is_open())
    {
        fileSteam << L"Information" << std::endl << L"Statistic step seconds:" << std::to_wstring(
            StatisticsStepSecondsCount) << std::endl;
    }

    std::wstring line;

    while (logQueue.Size() > 0)
    {
        while (logQueue.TryPop(line))
        {
            fileSteam << line;
        }
    }

    fileSteam << L"\nFinish Logs" << std::endl;

    fileSteam.flush();
    fileSteam.close();
}

int VoxelWaterfallApp::Run()
{
    timer.Reset();
    pumpFrameQuitRequested = false;

    while (!pumpFrameQuitRequested)
    {
        AdvanceRuntimeWorkOutsideFrame(false);
        const bool presented = PumpOneFrame();
        AdvanceRuntimeWorkOutsideFrame(presented);
    }

    benchmarkController.Shutdown(BuildBenchmarkControllerContext());
    return 0;
}

uint32_t VoxelWaterfallApp::DrainPendingWin32Messages()
{
    MSG msg{};
    uint32_t drained = 0;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        ++drained;
        if (msg.message == WM_QUIT)
        {
            pumpFrameQuitRequested = true;
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return drained;
}

bool VoxelWaterfallApp::TryAcquireCurrentFrameResource()
{
    currentFrameResourceReady = false;
    currentFrameResource = frameResources[currentFrameResourceIndex];

    const auto commandQueue = primeDevice->GetCommandQueue(GQueueType::Graphics);
    if (currentFrameResource->PrimeRenderFenceValue != 0 &&
        !commandQueue->IsFinish(currentFrameResource->PrimeRenderFenceValue))
    {
        if (!frameResourceBackpressureActive)
        {
            frameResourceBackpressureActive = true;
            frameResourceBackpressureStart = std::chrono::steady_clock::now();
        }
        ++totalFrameResourceBackpressurePollCount;
        ++currentFrameResourceBackpressurePollCount;
        return false;
    }

    if (frameResourceBackpressureActive)
    {
        const auto now = std::chrono::steady_clock::now();
        currentPrimaryWaitMs =
            std::chrono::duration<double, std::milli>(now - frameResourceBackpressureStart).count();
        frameResourceBackpressureActive = false;
    }
    else
    {
        currentPrimaryWaitMs = 0.0;
    }

    currentFrameResourceReady = true;
    return true;
}

bool VoxelWaterfallApp::AdvanceRuntimeWorkOutsideFrame(const bool presentedFrame)
{
    if (isPumpingFrame)
        return false;

    const bool wasActive = researchRunnerActive;
    AdvanceResearchRunner(presentedFrame);
    return wasActive || researchRunnerActive;
}

bool VoxelWaterfallApp::PumpOneFrame()
{
    if (isPumpingFrame)
    {
        ++rejectedRecursiveFrameRequests;
        assert(false && "PumpOneFrame reentrancy is forbidden: recursive frame pump would nest DrawFrame");
        return false;
    }

    struct PumpFrameScope
    {
        VoxelWaterfallApp& App;
        explicit PumpFrameScope(VoxelWaterfallApp& app) : App(app)
        {
            App.isPumpingFrame = true;
            ++App.currentFramePumpDepth;
            App.maximumObservedFramePumpDepth =
                std::max(App.maximumObservedFramePumpDepth, App.currentFramePumpDepth);
        }
        ~PumpFrameScope()
        {
            assert(App.currentFramePumpDepth > 0);
            if (App.currentFramePumpDepth > 0)
                --App.currentFramePumpDepth;
            App.isPumpingFrame = false;
        }
    } pumpFrameScope(*this);

    currentFrameDrainedMessageCount = DrainPendingWin32Messages();
    if (pumpFrameQuitRequested)
        return false;

    if (benchmarkController.IsAutomaticActive())
    {
        auto benchmarkContext = BuildBenchmarkControllerContext();
        benchmarkController.UpdateAutomatic(benchmarkContext);
    }

    ApplyPendingRuntimeChangesAtFrameBoundary();

    if (isStopRequested)
    {
        MainWindow->SetWindowTitle(MainWindow->GetWindowName() + L" Finished. Wait...");
        LogWriting();
        Quit();
        pumpFrameQuitRequested = true;
        return false;
    }

    if (!TryAcquireCurrentFrameResource())
    {
        std::this_thread::yield();
        return false;
    }

    timer.Tick();
    CalculateFrameStats();
    UpdateAfterFrameResourceAcquire(timer);
    const bool presented = DrawFrame(timer);
    if (presented)
    {
        ++frameSerial;
        currentFrameResourceBackpressurePollCount = 0;
        ServiceDeferredResourceLifetime();
    }
    return presented;
}

void VoxelWaterfallApp::ServiceDeferredResourceLifetime()
{
    if (primeDevice)
    {
        primeDevice->ResetAllocators(frameSerial);
        const auto renderQueue = primeDevice->GetCommandQueue(GQueueType::Graphics);
        if (renderQueue)
        {
            retainedVoxelFrameRenderPlans.erase(
                std::remove_if(
                    retainedVoxelFrameRenderPlans.begin(),
                    retainedVoxelFrameRenderPlans.end(),
                    [&renderQueue](const RetainedVoxelFrameRenderPlan& retainedPlan)
                    {
                        return retainedPlan.PrimaryRenderFenceValue == 0 ||
                            renderQueue->IsFinish(retainedPlan.PrimaryRenderFenceValue);
                    }),
                retainedVoxelFrameRenderPlans.end());
        }
    }
    if (secondDevice && secondDevice != primeDevice)
        secondDevice->ResetAllocators(frameSerial);
}

void VoxelWaterfallApp::PumpOneMemoryAuditFrame()
{
    PumpOneFrame();
}

int VoxelWaterfallApp::RunMemorySoakTestOnce(
    const uint32_t durationSeconds,
    const std::filesystem::path& outputDirectory)
{
    const auto outDir = outputDirectory.empty()
                            ? std::filesystem::path(L"Artifacts") / L"memory_soak"
                            : outputDirectory;
    std::filesystem::create_directories(outDir);
    const auto timelinePath = outDir / L"memory_timeline.csv";
    const auto reportPath = outDir / L"leak_report.json";

    ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile::DynamicSimulationAndRender);
    AutomaticBenchmarkConfig config{};
    config.Mode = VoxelExecutionMode::SingleGpuFull;
    config.ModeName = "SingleGpuFull";
    config.TotalCount = 123556;
    config.SecondaryShare = 0.0f;
    config.SpatialLodEnabled = false;
    config.TemporalInterval = 1;
    const auto applyResult = ApplyBenchmarkConfigurationAtomic(config);
    if (!applyResult.Passed)
    {
        std::ofstream report(reportPath);
        report << "{\n"
               << "  \"status\":\"FAILED\",\n"
               << "  \"scenario\":\"stable_workload_soak\",\n"
               << "  \"reason\":\"" << EscapeJsonString(applyResult.Reason) << "\"\n"
               << "}\n";
        return 1;
    }

    std::ofstream timeline(timelinePath);
    WriteMemoryTimelineHeader(timeline);
    std::vector<MemoryAuditSnapshot> snapshots;
    timer.Reset();
    const auto start = std::chrono::steady_clock::now();
    auto nextSample = start;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(durationSeconds))
    {
        PumpOneMemoryAuditFrame();
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextSample)
        {
            const double elapsed = std::chrono::duration<double>(now - start).count();
            auto snapshot = CaptureMemoryAuditSnapshot(frameSerial, elapsed, primeDevice, secondDevice);
            WriteMemoryTimelineRow(timeline, snapshot);
            snapshots.push_back(snapshot);
            nextSample = now + std::chrono::seconds(1);
        }
    }
    Flush();
    ServiceDeferredResourceLifetime();
    const auto finalSnapshot = CaptureMemoryAuditSnapshot(
        frameSerial,
        static_cast<double>(durationSeconds),
        primeDevice,
        secondDevice);
    WriteMemoryTimelineRow(timeline, finalSnapshot);
    snapshots.push_back(finalSnapshot);
    timeline.close();

    const size_t warmupCount = std::min<size_t>(30, snapshots.size() / 3);
    const bool privateTrend = HasPositivePrivateUsageTrend(snapshots, warmupCount);
    const bool hasStabilityWindow = snapshots.size() >= 10 && snapshots.size() > warmupCount + 2;
    const bool allocatorStable = !hasStabilityWindow ||
        snapshots[warmupCount].CommandListsCreated == snapshots.back().CommandListsCreated;
    const bool descriptorStable = !hasStabilityWindow ||
        snapshots[warmupCount].DescriptorCapacity == snapshots.back().DescriptorCapacity;
    const bool liveResourcesStable = !hasStabilityWindow ||
        snapshots[warmupCount].LiveResources == snapshots.back().LiveResources;
    const bool passed = !privateTrend && allocatorStable && descriptorStable && liveResourcesStable;

    std::ofstream report(reportPath);
    report << "{\n"
           << "  \"status\":\"" << (passed ? "PASS" : "FAIL") << "\",\n"
           << "  \"scenario\":\"stable_workload_soak\",\n"
           << "  \"duration_seconds\":" << durationSeconds << ",\n"
           << "  \"workload\":{\"mode\":\"SingleGpuFull\",\"spatial_lod\":\"Off\",\"requested_voxels\":123556,"
           << "\"actual_total\":" << applyResult.ActualTotalCount << "},\n"
           << "  \"artifacts\":{\"memory_timeline\":\"" << EscapeJsonString(timelinePath.string()) << "\"},\n"
           << "  \"checks\":{\n"
           << "    \"command_allocators_stable\":" << (allocatorStable ? "true" : "false") << ",\n"
           << "    \"descriptor_capacity_stable\":" << (descriptorStable ? "true" : "false") << ",\n"
           << "    \"live_resources_stable\":" << (liveResourcesStable ? "true" : "false") << ",\n"
           << "    \"private_usage_positive_trend\":" << (privateTrend ? "true" : "false") << "\n"
           << "  },\n"
           << "  \"final_snapshot\":{\"private_usage_bytes\":" << finalSnapshot.PrivateUsageBytes
           << ",\"working_set_bytes\":" << finalSnapshot.WorkingSetBytes
           << ",\"primary_local_vram_bytes\":" << finalSnapshot.PrimaryLocalUsageBytes
           << ",\"secondary_local_vram_bytes\":" << finalSnapshot.SecondaryLocalUsageBytes
           << ",\"descriptor_capacity\":" << finalSnapshot.DescriptorCapacity
           << ",\"descriptor_active\":" << finalSnapshot.DescriptorActive
           << ",\"live_d3d12_resources\":" << finalSnapshot.LiveResources << "}\n"
           << "}\n";

#if defined(DEBUG) || defined(_DEBUG)
    if (primeDevice)
        primeDevice->ReportLiveDeviceObjects();
    if (secondDevice && secondDevice != primeDevice)
        secondDevice->ReportLiveDeviceObjects();
#endif

    return passed ? 0 : 2;
}

int VoxelWaterfallApp::RunMemoryRebuildStressTestOnce(
    const uint32_t rebuildCycles,
    const uint32_t stableSeconds,
    const std::filesystem::path& outputDirectory)
{
    const auto outDir = outputDirectory.empty()
                            ? std::filesystem::path(L"Artifacts") / L"memory_rebuild_stress"
                            : outputDirectory;
    std::filesystem::create_directories(outDir);
    const auto timelinePath = outDir / L"memory_timeline.csv";
    const auto reportPath = outDir / L"leak_report.json";

    ApplyResearchWorkloadProfile(VoxelResearchWorkloadProfile::DynamicSimulationAndRender);

    std::ofstream timeline(timelinePath);
    WriteMemoryTimelineHeader(timeline);
    std::vector<MemoryAuditSnapshot> snapshots;
    bool blocked = false;
    std::string blockReason;

    const std::array<uint32_t, 4> voxelCounts = {50000u, 123556u, 250000u, 500000u};
    timer.Reset();
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t cycle = 0; cycle < rebuildCycles; ++cycle)
    {
        const bool requestMulti = (cycle % 2u) == 1u;
        if (requestMulti && !multiGpuAvailable)
        {
            blocked = true;
            blockReason = "MultiGpu rebuild cycle requested but no compatible secondary adapter is available";
            break;
        }
        if (requestMulti && !multiGpuVoxelRenderTargets.IsInitialized())
        {
            RebuildMultiGpuVoxelRenderTargets();
            if (!multiGpuVoxelRenderTargets.IsInitialized())
            {
                blocked = true;
                blockReason = WideToUtf8Local(multiGpuVoxelRenderTargets.GetFailureMessage());
                if (blockReason.empty())
                    blockReason = "MultiGpu rebuild cycle requested but cross-adapter render targets are not initialized";
                break;
            }
        }

        AutomaticBenchmarkConfig config{};
        config.Mode = requestMulti ? VoxelExecutionMode::MultiGpuFull : VoxelExecutionMode::SingleGpuFull;
        config.ModeName = requestMulti ? "MultiGpuFull" : "SingleGpuFull";
        config.TotalCount = voxelCounts[cycle % voxelCounts.size()];
        config.SecondaryShare = requestMulti ? 0.5f : 0.0f;
        config.SpatialLodEnabled = (cycle % 4u) >= 2u;
        config.TemporalInterval = 1;
        const auto applyResult = ApplyBenchmarkConfigurationAtomic(config);
        if (!applyResult.Passed)
        {
            blocked = true;
            blockReason = applyResult.Reason;
            break;
        }

        for (uint32_t frame = 0; frame < 5; ++frame)
            PumpOneMemoryAuditFrame();
        Flush();
        ServiceDeferredResourceLifetime();

        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        auto snapshot = CaptureMemoryAuditSnapshot(frameSerial, elapsed, primeDevice, secondDevice);
        WriteMemoryTimelineRow(timeline, snapshot);
        snapshots.push_back(snapshot);
    }

    const auto stableStart = std::chrono::steady_clock::now();
    if (!blocked)
    {
        AutomaticBenchmarkConfig config{};
        config.Mode = VoxelExecutionMode::SingleGpuFull;
        config.ModeName = "SingleGpuFull";
        config.TotalCount = 123556;
        config.SecondaryShare = 0.0f;
        config.SpatialLodEnabled = false;
        config.TemporalInterval = 1;
        const auto applyResult = ApplyBenchmarkConfigurationAtomic(config);
        if (!applyResult.Passed)
        {
            blocked = true;
            blockReason = applyResult.Reason;
        }
    }

    auto nextSample = std::chrono::steady_clock::now();
    while (!blocked &&
           std::chrono::steady_clock::now() - stableStart < std::chrono::seconds(stableSeconds))
    {
        PumpOneMemoryAuditFrame();
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextSample)
        {
            const double elapsed = std::chrono::duration<double>(now - start).count();
            auto snapshot = CaptureMemoryAuditSnapshot(frameSerial, elapsed, primeDevice, secondDevice);
            WriteMemoryTimelineRow(timeline, snapshot);
            snapshots.push_back(snapshot);
            nextSample = now + std::chrono::seconds(1);
        }
    }
    Flush();
    ServiceDeferredResourceLifetime();
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    const auto finalSnapshot = CaptureMemoryAuditSnapshot(frameSerial, elapsed, primeDevice, secondDevice);
    WriteMemoryTimelineRow(timeline, finalSnapshot);
    timeline.close();
    snapshots.push_back(finalSnapshot);

    const size_t warmupCount = std::min<size_t>(10, snapshots.size() / 3);
    const bool privateTrend = HasPositivePrivateUsageTrend(snapshots, warmupCount);
    const bool hasStabilityWindow = snapshots.size() >= 10 && snapshots.size() > warmupCount + 2;
    const bool allocatorStable = !hasStabilityWindow ||
        snapshots[warmupCount].CommandListsCreated == snapshots.back().CommandListsCreated;
    const bool descriptorStable = !hasStabilityWindow ||
        snapshots[warmupCount].DescriptorCapacity == snapshots.back().DescriptorCapacity;
    const bool liveResourcesStable = !hasStabilityWindow ||
        snapshots[warmupCount].LiveResources == snapshots.back().LiveResources;
    const bool passed = !blocked && !privateTrend && allocatorStable && descriptorStable && liveResourcesStable;

    std::ofstream report(reportPath);
    report << "{\n"
           << "  \"status\":\"" << (blocked ? "BLOCKED" : (passed ? "PASS" : "FAIL")) << "\",\n"
           << "  \"scenario\":\"rebuild_stress\",\n"
           << "  \"rebuild_cycles\":" << rebuildCycles << ",\n"
           << "  \"stable_seconds\":" << stableSeconds << ",\n"
           << "  \"block_reason\":\"" << EscapeJsonString(blockReason) << "\",\n"
           << "  \"artifacts\":{\"memory_timeline\":\"" << EscapeJsonString(timelinePath.string()) << "\"},\n"
           << "  \"checks\":{\n"
           << "    \"command_allocators_stable\":" << (allocatorStable ? "true" : "false") << ",\n"
           << "    \"descriptor_capacity_stable\":" << (descriptorStable ? "true" : "false") << ",\n"
           << "    \"live_resources_stable\":" << (liveResourcesStable ? "true" : "false") << ",\n"
           << "    \"private_usage_positive_trend\":" << (privateTrend ? "true" : "false") << "\n"
           << "  },\n"
           << "  \"final_snapshot\":{\"private_usage_bytes\":" << finalSnapshot.PrivateUsageBytes
           << ",\"working_set_bytes\":" << finalSnapshot.WorkingSetBytes
           << ",\"primary_local_vram_bytes\":" << finalSnapshot.PrimaryLocalUsageBytes
           << ",\"secondary_local_vram_bytes\":" << finalSnapshot.SecondaryLocalUsageBytes
           << ",\"descriptor_capacity\":" << finalSnapshot.DescriptorCapacity
           << ",\"descriptor_active\":" << finalSnapshot.DescriptorActive
           << ",\"live_d3d12_resources\":" << finalSnapshot.LiveResources << "}\n"
           << "}\n";

#if defined(DEBUG) || defined(_DEBUG)
    if (primeDevice)
        primeDevice->ReportLiveDeviceObjects();
    if (secondDevice && secondDevice != primeDevice)
        secondDevice->ReportLiveDeviceObjects();
#endif

    return blocked ? 3 : (passed ? 0 : 2);
}

void VoxelWaterfallApp::UpdateMaterials()
{
    {
        auto currentMaterialBuffer = currentFrameResource->MaterialBuffer;

        for (auto&& material : assets->GetMaterials())
        {
            material->Update();
            auto constantData = material->GetMaterialConstantData();
            currentMaterialBuffer->CopyData(material->GetIndex(), constantData);
        }
    }
}

void VoxelWaterfallApp::UpdateShadowTransform(const GameTimer& gt)
{
    // Only the first "main" light casts a shadow.
    Vector3 lightDir = mBaseLightDirections[0];
    Vector3 lightPos = -2.0f * mSceneBounds.Radius * lightDir;
    Vector3 targetPos = mSceneBounds.Center;
    Vector3 lightUp = Vector3::Up;
    Matrix lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

    mLightPosW = lightPos;


    // Transform bounding sphere to light space.
    Vector3 sphereCenterLS = Vector3::Transform(targetPos, lightView);


    // Ortho frustum in light space encloses scene.
    float l = sphereCenterLS.x - mSceneBounds.Radius;
    float b = sphereCenterLS.y - mSceneBounds.Radius;
    float n = sphereCenterLS.z - mSceneBounds.Radius;
    float r = sphereCenterLS.x + mSceneBounds.Radius;
    float t = sphereCenterLS.y + mSceneBounds.Radius;
    float f = sphereCenterLS.z + mSceneBounds.Radius;

    mLightNearZ = n;
    mLightFarZ = f;
    Matrix lightProj = DirectX::XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

    // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    Matrix T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);

    Matrix S = lightView * lightProj * T;
    mLightView = lightView;
    mLightProj = lightProj;
    mShadowTransform = S;
}

void VoxelWaterfallApp::UpdateShadowPassCB(const GameTimer& gt)
{
    auto view = mLightView;
    auto proj = mLightProj;

    auto viewProj = (view * proj);
    auto invView = view.Invert();
    auto invProj = proj.Invert();
    auto invViewProj = viewProj.Invert();

    shadowPassCB.View = view.Transpose();
    shadowPassCB.InvView = invView.Transpose();
    shadowPassCB.Proj = proj.Transpose();
    shadowPassCB.InvProj = invProj.Transpose();
    shadowPassCB.ViewProj = viewProj.Transpose();
    shadowPassCB.InvViewProj = invViewProj.Transpose();
    shadowPassCB.EyePosW = mLightPosW;
    shadowPassCB.NearZ = mLightNearZ;
    shadowPassCB.FarZ = mLightFarZ;

    UINT w = shadowPath->Width();
    UINT h = shadowPath->Height();
    shadowPassCB.RenderTargetSize = Vector2(static_cast<float>(w), static_cast<float>(h));
    shadowPassCB.InvRenderTargetSize = Vector2(1.0f / w, 1.0f / h);

    auto currPassCB = currentFrameResource->PrimePassConstantUploadBuffer;
    currPassCB->CopyData(1, shadowPassCB);
}

void VoxelWaterfallApp::UpdateMainPassCB(const GameTimer& gt)
{
    auto view = camera->GetViewMatrix();
    auto proj = camera->GetProjectionMatrix();

    auto viewProj = (view * proj);
    auto invView = view.Invert();
    auto invProj = proj.Invert();
    auto invViewProj = viewProj.Invert();
    auto shadowTransform = mShadowTransform;

    // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    Matrix T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);
    Matrix viewProjTex = XMMatrixMultiply(viewProj, T);
    mainPassCB.View = view.Transpose();
    mainPassCB.InvView = invView.Transpose();
    mainPassCB.Proj = proj.Transpose();
    mainPassCB.InvProj = invProj.Transpose();
    mainPassCB.ViewProj = viewProj.Transpose();
    mainPassCB.InvViewProj = invViewProj.Transpose();
    mainPassCB.ViewProjTex = viewProjTex.Transpose();
    mainPassCB.ShadowTransform = shadowTransform.Transpose();
    mainPassCB.EyePosW = camera->gameObject->GetTransform()->GetWorldPosition();
    mainPassCB.debugMap = voxelWorkload.DynamicShadowsEnabled ? 1.0f : 0.0f;
    mainPassCB.RenderTargetSize = Vector2(static_cast<float>(MainWindow->GetClientWidth()),
                                          static_cast<float>(MainWindow->GetClientHeight()));
    mainPassCB.InvRenderTargetSize = Vector2(1.0f / mainPassCB.RenderTargetSize.x,
                                             1.0f / mainPassCB.RenderTargetSize.y);
    mainPassCB.NearZ = camera->GetNearZ();
    mainPassCB.FarZ = camera->GetFarZ();
    mainPassCB.TotalTime = gt.TotalTime();
    mainPassCB.DeltaTime = gt.DeltaTime();
    mainPassCB.AmbientLight = Vector4{0.20f, 0.22f, 0.24f, 1.0f};

    for (int i = 0; i < MaxLights; ++i)
    {
        if (i < lights.size())
        {
            mainPassCB.Lights[i] = lights[i]->GetData();
        }
        else
        {
            break;
        }
    }

    mainPassCB.Lights[0].Direction = mBaseLightDirections[0];
    mainPassCB.Lights[1].Direction = mBaseLightDirections[1];
    mainPassCB.Lights[2].Direction = mBaseLightDirections[2];
    switch (voxelWorkload.LightingMode)
    {
    case VoxelResearchLightingPreset::OcclusionValidationLighting:
        mainPassCB.AmbientLight = Vector4{0.10f, 0.10f, 0.12f, 1.0f};
        mainPassCB.Lights[0].Strength = Vector3{1.05f, 1.0f, 0.9f};
        mainPassCB.Lights[1].Strength = Vector3{0.12f, 0.16f, 0.22f};
        mainPassCB.Lights[2].Strength = Vector3{0.0f, 0.0f, 0.0f};
        break;
    case VoxelResearchLightingPreset::DemoStaticSky:
        mainPassCB.AmbientLight = Vector4{0.30f, 0.34f, 0.38f, 1.0f};
        mainPassCB.Lights[0].Strength = Vector3{0.95f, 0.92f, 0.84f};
        mainPassCB.Lights[1].Strength = Vector3{0.20f, 0.26f, 0.34f};
        mainPassCB.Lights[2].Strength = Vector3{0.08f, 0.10f, 0.12f};
        break;
    case VoxelResearchLightingPreset::BenchmarkNeutral:
    default:
        mainPassCB.AmbientLight = Vector4{0.20f, 0.22f, 0.24f, 1.0f};
        mainPassCB.Lights[0].Strength = Vector3{0.78f, 0.78f, 0.74f};
        mainPassCB.Lights[1].Strength = Vector3{0.12f, 0.14f, 0.16f};
        mainPassCB.Lights[2].Strength = Vector3{0.0f, 0.0f, 0.0f};
        break;
    }

    auto currentPassCB = currentFrameResource->PrimePassConstantUploadBuffer;
    currentPassCB->CopyData(0, mainPassCB);
    if (currentFrameResource->SecondaryPassConstantUploadBuffer)
        currentFrameResource->SecondaryPassConstantUploadBuffer->CopyData(0, mainPassCB);
}

void VoxelWaterfallApp::UpdateSsaoCB(const GameTimer& gt)
{
    SsaoConstants ssaoCB;

    auto P = camera->GetProjectionMatrix();

    // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    Matrix T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);

    ssaoCB.Proj = mainPassCB.Proj;
    ssaoCB.InvProj = mainPassCB.InvProj;
    XMStoreFloat4x4(&ssaoCB.ProjTex, XMMatrixTranspose(P * T));

    {
        ambientPrimePath->GetOffsetVectors(ssaoCB.OffsetVectors);

        auto blurWeights = ambientPrimePath->CalcGaussWeights(2.5f);
        ssaoCB.BlurWeights[0] = Vector4(&blurWeights[0]);
        ssaoCB.BlurWeights[1] = Vector4(&blurWeights[4]);
        ssaoCB.BlurWeights[2] = Vector4(&blurWeights[8]);

        ssaoCB.InvRenderTargetSize = Vector2(1.0f / ambientPrimePath->SsaoMapWidth(),
                                             1.0f / ambientPrimePath->SsaoMapHeight());

        // Coordinates given in view space.
        ssaoCB.OcclusionRadius = 0.5f;
        ssaoCB.OcclusionFadeStart = 0.2f;
        ssaoCB.OcclusionFadeEnd = 1.0f;
        ssaoCB.SurfaceEpsilon = 0.05f;

        auto currSsaoCB = currentFrameResource->SsaoConstantUploadBuffer;
        currSsaoCB->CopyData(0, ssaoCB);
    }
}

bool VoxelWaterfallApp::InitMainWindow()
{
    MainWindow = CreateRenderWindow(primeDevice, mainWindowCaption, 1920, 1080, false);

    logQueue.Push(std::wstring(L"\nInit Window"));
    return true;
}

void VoxelWaterfallApp::OnResize()
{
    D3DApp::OnResize();
    voxelWorkload.RenderResolutionWidth = static_cast<uint32_t>(MainWindow->GetClientWidth());
    voxelWorkload.RenderResolutionHeight = static_cast<uint32_t>(MainWindow->GetClientHeight());

    fullViewport.Height = static_cast<float>(MainWindow->GetClientHeight());
    fullViewport.Width = static_cast<float>(MainWindow->GetClientWidth());
    fullViewport.MinDepth = 0.0f;
    fullViewport.MaxDepth = 1.0f;
    fullViewport.TopLeftX = 0;
    fullViewport.TopLeftY = 0;
    fullRect = D3D12_RECT{0, 0, MainWindow->GetClientWidth(), MainWindow->GetClientHeight()};


    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = GetSRGBFormat(BackBufferFormat);
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    for (int i = 0; i < globalCountFrameResources; ++i)
    {
        MainWindow->GetBackBuffer(i).CreateRenderTargetView(&rtvDesc, &frameResources[i]->BackBufferRTVMemory);
    }


    if (camera != nullptr)
    {
        camera->SetAspectRatio(AspectRatio());
    }

    if (ambientPrimePath != nullptr)
    {
        ambientPrimePath->OnResize(MainWindow->GetClientWidth(), MainWindow->GetClientHeight());
        ambientPrimePath->RebuildDescriptors();
    }

    if (antiAliasingPrimePath != nullptr)
    {
        antiAliasingPrimePath->OnResize(MainWindow->GetClientWidth(), MainWindow->GetClientHeight());
    }

    if (multiGpuAvailable)
    {
        if (!suppressResizeFlushForPendingRuntimeChanges)
            Flush();
        RebuildMultiGpuVoxelRenderTargets();
    }

    currentFrameResourceIndex = MainWindow->GetCurrentBackBufferIndex();
}

void VoxelWaterfallApp::Flush()
{
    if (primeDevice)
        primeDevice->Flush();
    if (secondDevice && secondDevice != primeDevice)
        secondDevice->Flush();
    retainedVoxelFrameRenderPlans.clear();
    lastPrimaryPartitionGraphicsFenceValue = 0;
    lastSecondaryPartitionGraphicsFenceValue = 0;
    lastSecondaryPartitionGraphicsFenceOwner = VoxelAdapterOwner::Primary;
}

LRESULT VoxelWaterfallApp::MsgProc(const HWND hwnd, const UINT msg, const WPARAM wParam, const LPARAM lParam)
{
    if (benchmarkController.IsAutomaticActive())
        return D3DApp::MsgProc(hwnd, msg, wParam, lParam);

    if (msg == WM_KEYDOWN && HandleDemoPresetHotkey(wParam))
        return 0;

    if (imguiInitialized && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    bool inputHandled = false;
    const LRESULT inputResult = inputRouter.Route(hwnd, msg, wParam, lParam, keyboard, mouse, inputHandled);
    if (inputHandled)
        return inputResult;

    return D3DApp::MsgProc(hwnd, msg, wParam, lParam);
}
