#include "Source/Validation/TwoAdapterVerification.h"

#include "GCommandQueue.h"
#include "GDevice.h"
#include "d3dx12.h"

#include <Windows.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

using Microsoft::WRL::ComPtr;
using PEPEngine::Graphics::GDevice;
using PEPEngine::Graphics::GQueueType;

namespace
{
    constexpr UINT VerificationWidth = 320;
    constexpr UINT VerificationHeight = 180;

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

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                             nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(std::max(size, 0)), '\0');
        if (size > 0)
        {
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                result.data(), size, nullptr, nullptr);
        }
        return result;
    }

    std::string Hex32(const uint32_t value)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
        return stream.str();
    }

    std::string Hex64(const uint64_t value)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
        return stream.str();
    }

    std::string HResultText(const HRESULT hr)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::setw(8) << std::setfill('0')
               << static_cast<uint32_t>(hr);
        return stream.str();
    }

    bool SameLuid(const DXGI_ADAPTER_DESC2& left, const DXGI_ADAPTER_DESC2& right)
    {
        return left.AdapterLuid.HighPart == right.AdapterLuid.HighPart &&
            left.AdapterLuid.LowPart == right.AdapterLuid.LowPart;
    }

    bool HasQueue(const std::shared_ptr<GDevice>& device, const GQueueType type)
    {
        return device && device->GetCommandQueue(type) != nullptr;
    }

    std::string QueueCapabilitiesJson(const std::shared_ptr<GDevice>& device)
    {
        std::ostringstream json;
        json << "{\"graphics\":" << (HasQueue(device, GQueueType::Graphics) ? "true" : "false")
             << ",\"compute\":" << (HasQueue(device, GQueueType::Compute) ? "true" : "false")
             << ",\"copy\":" << (HasQueue(device, GQueueType::Copy) ? "true" : "false") << "}";
        return json.str();
    }

    std::string FormatName(const DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        default: return "UNKNOWN";
        }
    }

    std::string ModeName(const VoxelExecutionMode mode)
    {
        switch (mode)
        {
        case VoxelExecutionMode::SingleGpuFull: return "SingleGpuFull";
        case VoxelExecutionMode::MultiGpuFull: return "MultiGpuFull";
        case VoxelExecutionMode::SingleGpuTemporalDecimation: return "SingleGpuTemporalDecimation";
        case VoxelExecutionMode::MultiGpuTemporalDecimation: return "MultiGpuTemporalDecimation";
        default: return "Unknown";
        }
    }

    std::string HighestFeatureLevel(const std::shared_ptr<GDevice>& device)
    {
        if (!device)
            return "unavailable";

        std::array<D3D_FEATURE_LEVEL, 5> levels = {
            D3D_FEATURE_LEVEL_12_2,
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };
        D3D12_FEATURE_DATA_FEATURE_LEVELS data{};
        data.NumFeatureLevels = static_cast<UINT>(levels.size());
        data.pFeatureLevelsRequested = levels.data();
        if (FAILED(device->GetDXDevice()->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &data, sizeof(data))))
            return "unknown";

        switch (data.MaxSupportedFeatureLevel)
        {
        case D3D_FEATURE_LEVEL_12_2: return "12_2";
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        default: return "unknown";
        }
    }

    bool SupportsFormat(const std::shared_ptr<GDevice>& device,
                        const DXGI_FORMAT format,
                        const D3D12_FORMAT_SUPPORT1 required)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
        support.Format = format;
        if (!device || FAILED(device->GetDXDevice()->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
        {
            return false;
        }
        return (support.Support1 & required) == required;
    }

    UINT64 Align64K(const UINT64 value)
    {
        constexpr UINT64 alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        return (value + alignment - 1u) & ~(alignment - 1u);
    }

    D3D12_RESOURCE_DESC SharedTextureDesc(const UINT width, const UINT height, const DXGI_FORMAT format)
    {
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return desc;
    }

    struct CreationRecord
    {
        std::string Name;
        std::string Format;
        UINT Width = 0;
        UINT Height = 0;
        UINT64 RowPitch = 0;
        UINT64 HeapBytes = 0;
        std::string CreateHResult = HResultText(E_FAIL);
        std::string SharedHandleHResult = HResultText(E_FAIL);
        std::string OpenHandleHResult = HResultText(E_FAIL);
        std::string PrimaryPlacedResourceHResult = HResultText(E_FAIL);
        std::string SecondaryPlacedResourceHResult = HResultText(E_FAIL);
        bool Attempted = false;
        bool Passed = false;
    };

    CreationRecord VerifySharedTexture(const std::shared_ptr<GDevice>& primary,
                                       const std::shared_ptr<GDevice>& secondary,
                                       const std::string& name,
                                       const DXGI_FORMAT format)
    {
        CreationRecord record{};
        record.Name = name;
        record.Format = FormatName(format);
        record.Width = VerificationWidth;
        record.Height = VerificationHeight;
        record.Attempted = primary != nullptr && secondary != nullptr;
        if (!record.Attempted)
            return record;

        auto desc = SharedTextureDesc(VerificationWidth, VerificationHeight, format);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        primary->GetDXDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, nullptr);
        record.RowPitch = layout.Footprint.RowPitch;
        record.HeapBytes = Align64K(static_cast<UINT64>(layout.Footprint.RowPitch) * layout.Footprint.Height);

        CD3DX12_HEAP_DESC heapDesc(record.HeapBytes,
                                   D3D12_HEAP_TYPE_DEFAULT,
                                   0,
                                   D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);
        ComPtr<ID3D12Heap> primaryHeap;
        HRESULT hr = primary->GetDXDevice()->CreateHeap(&heapDesc, IID_PPV_ARGS(&primaryHeap));
        record.CreateHResult = HResultText(hr);
        if (FAILED(hr))
            return record;

        HANDLE sharedHandle = nullptr;
        hr = primary->GetDXDevice()->CreateSharedHandle(primaryHeap.Get(), nullptr, GENERIC_ALL, nullptr,
                                                        &sharedHandle);
        record.SharedHandleHResult = HResultText(hr);
        if (FAILED(hr) || sharedHandle == nullptr)
            return record;

        ComPtr<ID3D12Heap> secondaryHeap;
        hr = secondary->GetDXDevice()->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&secondaryHeap));
        CloseHandle(sharedHandle);
        record.OpenHandleHResult = HResultText(hr);
        if (FAILED(hr))
            return record;

        ComPtr<ID3D12Resource> primaryResource;
        hr = primary->GetDXDevice()->CreatePlacedResource(primaryHeap.Get(), 0, &desc,
                                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                          IID_PPV_ARGS(&primaryResource));
        record.PrimaryPlacedResourceHResult = HResultText(hr);
        if (FAILED(hr))
            return record;

        ComPtr<ID3D12Resource> secondaryResource;
        hr = secondary->GetDXDevice()->CreatePlacedResource(secondaryHeap.Get(), 0, &desc,
                                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                            IID_PPV_ARGS(&secondaryResource));
        record.SecondaryPlacedResourceHResult = HResultText(hr);
        record.Passed = SUCCEEDED(hr);
        return record;
    }

    CreationRecord VerifySharedFence(const std::shared_ptr<GDevice>& primary,
                                     const std::shared_ptr<GDevice>& secondary)
    {
        CreationRecord record{};
        record.Name = "shared_fence";
        record.Format = "D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER";
        record.Attempted = primary != nullptr && secondary != nullptr;
        if (!record.Attempted)
            return record;

        ComPtr<ID3D12Fence> primaryFence;
        HRESULT hr = primary->GetDXDevice()->CreateFence(
            0,
            D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
            IID_PPV_ARGS(&primaryFence));
        record.CreateHResult = HResultText(hr);
        if (FAILED(hr))
            return record;

        HANDLE sharedHandle = nullptr;
        hr = primary->GetDXDevice()->CreateSharedHandle(primaryFence.Get(), nullptr, GENERIC_ALL, nullptr,
                                                        &sharedHandle);
        record.SharedHandleHResult = HResultText(hr);
        if (FAILED(hr) || sharedHandle == nullptr)
            return record;

        ComPtr<ID3D12Fence> secondaryFence;
        hr = secondary->GetDXDevice()->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&secondaryFence));
        CloseHandle(sharedHandle);
        record.OpenHandleHResult = HResultText(hr);
        record.Passed = SUCCEEDED(hr);
        return record;
    }

    std::string CreationRecordJson(const CreationRecord& record)
    {
        std::ostringstream json;
        json << "{"
             << "\"name\":\"" << EscapeJson(record.Name) << "\","
             << "\"attempted\":" << (record.Attempted ? "true" : "false") << ","
             << "\"format\":\"" << record.Format << "\","
             << "\"width\":" << record.Width << ","
             << "\"height\":" << record.Height << ","
             << "\"row_pitch\":" << record.RowPitch << ","
             << "\"heap_flags\":\"SHARED|SHARED_CROSS_ADAPTER\","
             << "\"heap_bytes\":" << record.HeapBytes << ","
             << "\"create_hresult\":\"" << record.CreateHResult << "\","
             << "\"shared_handle_hresult\":\"" << record.SharedHandleHResult << "\","
             << "\"open_handle_hresult\":\"" << record.OpenHandleHResult << "\","
             << "\"primary_placed_resource_hresult\":\"" << record.PrimaryPlacedResourceHResult << "\","
             << "\"secondary_placed_resource_hresult\":\"" << record.SecondaryPlacedResourceHResult << "\","
             << "\"passed\":" << (record.Passed ? "true" : "false")
             << "}";
        return json.str();
    }

    std::string AdapterJson(const AdapterSelectionInfo& info)
    {
        std::ostringstream json;
        json << "{"
             << "\"adapter_index\":" << info.AdapterIndex << ","
             << "\"name\":\"" << EscapeJson(WideToUtf8(info.Name)) << "\","
             << "\"vendor_id\":\"" << Hex32(info.VendorId) << "\","
             << "\"device_id\":\"" << Hex32(info.DeviceId) << "\","
             << "\"subsys_id\":\"" << Hex32(info.SubSysId) << "\","
             << "\"revision\":" << info.Revision << ","
             << "\"dedicated_video_memory\":" << info.DedicatedVideoMemory << ","
             << "\"dedicated_system_memory\":" << info.DedicatedSystemMemory << ","
             << "\"shared_system_memory\":" << info.SharedSystemMemory << ","
             << "\"luid_high\":" << info.LuidHighPart << ","
             << "\"luid_low\":" << info.LuidLowPart << ","
             << "\"d3d_feature_level\":\"" << EscapeJson(info.FeatureLevel) << "\","
             << "\"hardware\":" << (info.Hardware ? "true" : "false") << ","
             << "\"queues\":{\"graphics\":" << (info.GraphicsQueue ? "true" : "false")
             << ",\"compute\":" << (info.ComputeQueue ? "true" : "false")
             << ",\"copy\":" << (info.CopyQueue ? "true" : "false") << "},"
             << "\"cross_adapter_row_major_texture_supported\":"
             << (info.CrossAdapterTexture ? "true" : "false") << ","
             << "\"selected_role\":\""
             << (info.SelectedPrimary ? "primary" : (info.SelectedSecondary ? "secondary" : "none")) << "\","
             << "\"status\":\"" << EscapeJson(WideToUtf8(info.Status)) << "\""
             << "}";
        return json.str();
    }

    std::string ReasonsJson(const std::vector<std::string>& reasons)
    {
        std::ostringstream json;
        json << "[";
        for (size_t i = 0; i < reasons.size(); ++i)
            json << (i == 0 ? "" : ",") << "\"" << EscapeJson(reasons[i]) << "\"";
        json << "]";
        return json.str();
    }

    void WriteOutputs(const TwoAdapterVerificationResult& result)
    {
        std::filesystem::create_directories(result.JsonPath.parent_path());

        std::ofstream json(result.JsonPath, std::ios::out | std::ios::trunc);
        json.imbue(std::locale::classic());
        json << "{\n"
             << "  \"verification_run_id\":\"" << EscapeJson(result.VerificationRunId) << "\",\n"
             << "  \"build_hash\":\"" << EscapeJson(result.BuildHash) << "\",\n"
             << "  \"adapter_pair_identity\":\"" << EscapeJson(result.AdapterPairIdentity) << "\",\n"
             << "  \"status\":\"" << TwoAdapterVerificationRunner::StatusName(result.Status) << "\",\n"
             << "  \"reasons\":" << ReasonsJson(result.Reasons) << ",\n"
             << result.PreflightJsonFragment << ",\n"
             << "  \"runtime\":{\n"
             << "    \"attempted\":" << (result.Runtime.Attempted ? "true" : "false") << ",\n"
             << "    \"frames_observed\":" << result.Runtime.FramesObserved << ",\n"
             << "    \"frame_config_id\":\"" << EscapeJson(result.Runtime.FrameConfigId) << "\",\n"
             << "    \"requested_mode\":\"" << ModeName(result.Runtime.RequestedMode) << "\",\n"
             << "    \"actual_mode\":\"" << ModeName(result.Runtime.ActualMode) << "\",\n"
             << "    \"fallback\":" << (result.Runtime.Fallback ? "true" : "false") << ",\n"
             << "    \"fallback_reason\":\"" << EscapeJson(result.Runtime.FallbackReason) << "\",\n"
             << "    \"any_actual_multi_mode\":" << (result.Runtime.AnyActualMultiMode ? "true" : "false") << ",\n"
             << "    \"primary_partition_voxels\":" << result.Runtime.PrimaryPartitionVoxels << ",\n"
             << "    \"secondary_partition_voxels\":" << result.Runtime.SecondaryPartitionVoxels << ",\n"
             << "    \"total_secondary_compute_dispatch_count\":"
             << result.Runtime.TotalSecondaryComputeDispatchCount << ",\n"
             << "    \"max_secondary_graphics_draw_count\":"
             << result.Runtime.MaxSecondaryGraphicsDrawCount << ",\n"
             << "    \"max_secondary_rendered_voxel_count\":"
             << result.Runtime.MaxSecondaryRenderedVoxelCount << ",\n"
             << "    \"total_color_transfer_bytes\":" << result.Runtime.TotalColorTransferBytes << ",\n"
             << "    \"total_depth_transfer_bytes\":" << result.Runtime.TotalDepthTransferBytes << ",\n"
             << "    \"any_composite_submitted\":"
             << (result.Runtime.AnyCompositeSubmitted ? "true" : "false") << ",\n"
             << "    \"nonzero_fence_values_seen\":{"
             << "\"secondary_compute\":" << (result.Runtime.AnySecondaryComputeFenceValue ? "true" : "false")
             << ",\"secondary_graphics\":" << (result.Runtime.AnySecondaryGraphicsFenceValue ? "true" : "false")
             << ",\"secondary_local_to_shared_copy\":"
             << (result.Runtime.AnySecondaryLocalToSharedCopyFenceValue ? "true" : "false")
             << ",\"cross_adapter_render_ready\":"
             << (result.Runtime.AnyCrossAdapterRenderReadyFenceValue ? "true" : "false")
             << ",\"primary_shared_to_local_copy\":"
             << (result.Runtime.AnyPrimarySharedToLocalCopyFenceValue ? "true" : "false")
             << ",\"primary_secondary_image_ready\":"
             << (result.Runtime.AnyPrimarySecondaryImageReadyFenceValue ? "true" : "false")
             << ",\"final_present\":" << (result.Runtime.AnyFinalPresentFenceValue ? "true" : "false")
             << "},\n"
             << "    \"secondary_compute_dispatch_count\":" << result.Runtime.SecondaryComputeDispatchCount << ",\n"
             << "    \"secondary_graphics_draw_count\":" << result.Runtime.SecondaryGraphicsDrawCount << ",\n"
             << "    \"secondary_indirect_draw_count\":" << result.Runtime.SecondaryIndirectDrawCount << ",\n"
             << "    \"secondary_rendered_voxel_count\":" << result.Runtime.SecondaryRenderedVoxelCount << ",\n"
             << "    \"secondary_primitive_estimate\":" << result.Runtime.SecondaryPrimitiveEstimate << ",\n"
             << "    \"color_local_to_shared_bytes\":" << result.Runtime.ColorLocalToSharedBytes << ",\n"
             << "    \"depth_local_to_shared_bytes\":" << result.Runtime.DepthLocalToSharedBytes << ",\n"
             << "    \"color_shared_to_local_bytes\":" << result.Runtime.ColorSharedToLocalBytes << ",\n"
             << "    \"depth_shared_to_local_bytes\":" << result.Runtime.DepthSharedToLocalBytes << ",\n"
             << "    \"particle_transfer_bytes\":" << result.Runtime.ParticleTransferBytes << ",\n"
             << "    \"secondary_compute_fence_value\":" << result.Runtime.SecondaryComputeFenceValue << ",\n"
             << "    \"secondary_graphics_fence_value\":" << result.Runtime.SecondaryGraphicsFenceValue << ",\n"
             << "    \"secondary_local_to_shared_copy_fence_value\":"
             << result.Runtime.SecondaryLocalToSharedCopyFenceValue << ",\n"
             << "    \"cross_adapter_render_ready_fence_value\":"
             << result.Runtime.CrossAdapterRenderReadyFenceValue << ",\n"
             << "    \"primary_shared_to_local_copy_fence_value\":"
             << result.Runtime.PrimarySharedToLocalCopyFenceValue << ",\n"
             << "    \"primary_secondary_image_ready_fence_value\":"
             << result.Runtime.PrimarySecondaryImageReadyFenceValue << ",\n"
             << "    \"final_present_fence_value\":" << result.Runtime.FinalPresentFenceValue << ",\n"
             << "    \"composite_submitted\":" << (result.Runtime.CompositeSubmitted ? "true" : "false") << ",\n"
             << "    \"passed\":" << (result.Runtime.Passed ? "true" : "false") << ",\n"
             << "    \"reasons\":" << ReasonsJson(result.Runtime.Reasons) << ",\n"
             << "    \"queue_calibrations\":[";
        for (size_t i = 0; i < result.Runtime.QueueCalibrations.size(); ++i)
        {
            const auto& queue = result.Runtime.QueueCalibrations[i];
            json << (i == 0 ? "" : ",") << "{"
                 << "\"queue\":\"" << EscapeJson(queue.QueueName) << "\","
                 << "\"hresult\":\"" << queue.HResult << "\","
                 << "\"frequency\":" << queue.Frequency << ","
                 << "\"gpu_timestamp\":" << queue.GpuTimestamp << ","
                 << "\"cpu_timestamp\":" << queue.CpuTimestamp << ","
                 << "\"valid\":" << (queue.Valid ? "true" : "false") << "}";
        }
        json << "]\n"
             << "  }\n"
             << "}\n";

        std::ofstream text(result.TextPath, std::ios::out | std::ios::trunc);
        text << "verificationRunId=" << result.VerificationRunId << "\n"
             << "status=" << TwoAdapterVerificationRunner::StatusName(result.Status) << "\n"
             << "buildHash=" << result.BuildHash << "\n"
             << "adapterPair=" << result.AdapterPairIdentity << "\n";
        for (const auto& reason : result.Reasons)
            text << "reason=" << reason << "\n";
        text << result.PreflightText;
        if (result.Runtime.Attempted)
        {
            text << "runtime.frameConfigId=" << result.Runtime.FrameConfigId << "\n"
                 << "runtime.framesObserved=" << result.Runtime.FramesObserved << "\n"
                 << "runtime.anyActualMultiMode="
                 << (result.Runtime.AnyActualMultiMode ? "true" : "false") << "\n"
                 << "runtime.totalSecondaryDispatches="
                 << result.Runtime.TotalSecondaryComputeDispatchCount << "\n"
                 << "runtime.maxSecondaryDraws="
                 << result.Runtime.MaxSecondaryGraphicsDrawCount << "\n"
                 << "runtime.maxSecondaryRenderedVoxels="
                 << result.Runtime.MaxSecondaryRenderedVoxelCount << "\n"
                 << "runtime.totalColorTransferBytes="
                 << result.Runtime.TotalColorTransferBytes << "\n"
                 << "runtime.totalDepthTransferBytes="
                 << result.Runtime.TotalDepthTransferBytes << "\n"
                 << "runtime.anyCompositeSubmitted="
                 << (result.Runtime.AnyCompositeSubmitted ? "true" : "false") << "\n"
                 << "runtime.secondaryDispatches=" << result.Runtime.SecondaryComputeDispatchCount << "\n"
                 << "runtime.secondaryDraws=" << result.Runtime.SecondaryGraphicsDrawCount << "\n"
                 << "runtime.renderOutputBytes="
                 << (result.Runtime.ColorLocalToSharedBytes + result.Runtime.DepthLocalToSharedBytes) << "\n"
                 << "runtime.particleBytes=" << result.Runtime.ParticleTransferBytes << "\n"
                 << "runtime.passed=" << (result.Runtime.Passed ? "true" : "false") << "\n";
            for (const auto& reason : result.Runtime.Reasons)
                text << "runtime.reason=" << reason << "\n";
        }
    }
}

std::string TwoAdapterVerificationRunner::StatusName(const TwoAdapterVerificationStatus status)
{
    switch (status)
    {
    case TwoAdapterVerificationStatus::Pass: return "PASS";
    case TwoAdapterVerificationStatus::Fail: return "FAIL";
    case TwoAdapterVerificationStatus::Blocked: return "BLOCKED";
    default: return "BLOCKED";
    }
}

TwoAdapterVerificationResult TwoAdapterVerificationRunner::RunPreflight(
    const std::vector<std::shared_ptr<GDevice>>& devices,
    const SelectedDevices& selectedDevices,
    const std::string& buildHash,
    const std::filesystem::path& outputDirectory) const
{
    TwoAdapterVerificationResult result{};
    result.BuildHash = buildHash;
    result.JsonPath = outputDirectory / "two_adapter_preflight.json";
    result.TextPath = outputDirectory / "two_adapter_preflight.txt";

    uint64_t identityHash = 1469598103934665603ull;
    auto append = [&identityHash](const uint64_t value)
    {
        for (uint32_t i = 0; i < 8; ++i)
        {
            identityHash ^= static_cast<uint8_t>((value >> (i * 8)) & 0xffu);
            identityHash *= 1099511628211ull;
        }
    };
    append(static_cast<uint64_t>(devices.size()));
    if (selectedDevices.Primary)
    {
        append(static_cast<uint32_t>(selectedDevices.Primary->GetDesc().VendorId));
        append(static_cast<uint32_t>(selectedDevices.Primary->GetDesc().DeviceId));
        append(static_cast<uint32_t>(selectedDevices.Primary->GetDesc().AdapterLuid.LowPart));
        append(static_cast<uint32_t>(selectedDevices.Primary->GetDesc().AdapterLuid.HighPart));
    }
    if (selectedDevices.Secondary)
    {
        append(static_cast<uint32_t>(selectedDevices.Secondary->GetDesc().VendorId));
        append(static_cast<uint32_t>(selectedDevices.Secondary->GetDesc().DeviceId));
        append(static_cast<uint32_t>(selectedDevices.Secondary->GetDesc().AdapterLuid.LowPart));
        append(static_cast<uint32_t>(selectedDevices.Secondary->GetDesc().AdapterLuid.HighPart));
    }
    result.VerificationRunId = Hex64(identityHash);

    std::ostringstream adaptersJson;
    std::ostringstream text;
    adaptersJson << "  \"adapters\":[";
    for (size_t i = 0; i < selectedDevices.Adapters.size(); ++i)
    {
        adaptersJson << (i == 0 ? "" : ",") << AdapterJson(selectedDevices.Adapters[i]);
        text << "adapter[" << selectedDevices.Adapters[i].AdapterIndex << "] "
             << WideToUtf8(selectedDevices.Adapters[i].Name)
             << " vendor=" << Hex32(selectedDevices.Adapters[i].VendorId)
             << " device=" << Hex32(selectedDevices.Adapters[i].DeviceId)
             << " luidHigh=" << selectedDevices.Adapters[i].LuidHighPart
             << " luidLow=" << selectedDevices.Adapters[i].LuidLowPart
             << " featureLevel=" << selectedDevices.Adapters[i].FeatureLevel
             << " role="
             << (selectedDevices.Adapters[i].SelectedPrimary
                     ? "primary"
                     : (selectedDevices.Adapters[i].SelectedSecondary ? "secondary" : "none"))
             << " status=" << WideToUtf8(selectedDevices.Adapters[i].Status) << "\n";
    }
    adaptersJson << "]";

    bool preconditionMet = selectedDevices.Primary != nullptr && selectedDevices.Secondary != nullptr;
    if (!preconditionMet)
    {
        result.Status = TwoAdapterVerificationStatus::Blocked;
        result.Reasons.push_back(WideToUtf8(selectedDevices.MultiGpuUnavailableReason.empty()
                                                ? L"Two compatible hardware D3D12 adapters were not found"
                                                : selectedDevices.MultiGpuUnavailableReason));
    }

    bool distinctLuid = false;
    bool colorFormatSupported = false;
    bool depthFormatSupported = false;
    if (preconditionMet)
    {
        const auto& primaryDesc = selectedDevices.Primary->GetDesc();
        const auto& secondaryDesc = selectedDevices.Secondary->GetDesc();
        distinctLuid = !SameLuid(primaryDesc, secondaryDesc);
        colorFormatSupported =
            SupportsFormat(selectedDevices.Primary, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_FORMAT_SUPPORT1_RENDER_TARGET) &&
            SupportsFormat(selectedDevices.Secondary, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
        depthFormatSupported =
            SupportsFormat(selectedDevices.Primary, DXGI_FORMAT_R32_FLOAT, D3D12_FORMAT_SUPPORT1_RENDER_TARGET) &&
            SupportsFormat(selectedDevices.Secondary, DXGI_FORMAT_R32_FLOAT, D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
        result.AdapterPairIdentity =
            Hex32(primaryDesc.VendorId) + ":" + Hex32(primaryDesc.DeviceId) + ":" +
            std::to_string(primaryDesc.AdapterLuid.HighPart) + ":" +
            std::to_string(primaryDesc.AdapterLuid.LowPart) + "->" +
            Hex32(secondaryDesc.VendorId) + ":" + Hex32(secondaryDesc.DeviceId) + ":" +
            std::to_string(secondaryDesc.AdapterLuid.HighPart) + ":" +
            std::to_string(secondaryDesc.AdapterLuid.LowPart);
        if (!distinctLuid)
            result.Reasons.push_back("Primary and secondary adapters have identical LUID");
        if (!colorFormatSupported)
            result.Reasons.push_back("Required color format is not supported as render target on both adapters");
        if (!depthFormatSupported)
            result.Reasons.push_back("Required linear depth format is not supported as render target on both adapters");
    }
    else
    {
        result.AdapterPairIdentity = "unavailable";
    }

    const auto fenceRecord = VerifySharedFence(selectedDevices.Primary, selectedDevices.Secondary);
    const auto colorRecord = VerifySharedTexture(selectedDevices.Primary, selectedDevices.Secondary,
                                                 "shared_color", DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto depthRecord = VerifySharedTexture(selectedDevices.Primary, selectedDevices.Secondary,
                                                 "shared_linear_depth", DXGI_FORMAT_R32_FLOAT);

    if (preconditionMet)
    {
        if (!selectedDevices.Primary->IsCrossAdapterTextureSupported() ||
            !selectedDevices.Secondary->IsCrossAdapterTextureSupported())
        {
            result.Reasons.push_back("CrossAdapterRowMajorTextureSupported is false on at least one selected adapter");
        }
        if (!fenceRecord.Passed)
            result.Reasons.push_back("Shared fence create/open verification failed");
        if (!colorRecord.Passed)
            result.Reasons.push_back("Shared color resource create/open verification failed");
        if (!depthRecord.Passed)
            result.Reasons.push_back("Shared linear depth resource create/open verification failed");

        const bool pass = distinctLuid &&
            colorFormatSupported &&
            depthFormatSupported &&
            selectedDevices.Primary->IsCrossAdapterTextureSupported() &&
            selectedDevices.Secondary->IsCrossAdapterTextureSupported() &&
            fenceRecord.Passed &&
            colorRecord.Passed &&
            depthRecord.Passed;
        result.Status = pass ? TwoAdapterVerificationStatus::Pass : TwoAdapterVerificationStatus::Fail;
    }

    std::ostringstream pairJson;
    pairJson << "  \"pair\":{"
             << "\"primary_feature_level\":\"" << HighestFeatureLevel(selectedDevices.Primary) << "\","
             << "\"secondary_feature_level\":\"" << HighestFeatureLevel(selectedDevices.Secondary) << "\","
             << "\"primary_queues\":" << QueueCapabilitiesJson(selectedDevices.Primary) << ","
             << "\"secondary_queues\":" << QueueCapabilitiesJson(selectedDevices.Secondary) << ","
             << "\"distinct_luid\":" << (distinctLuid ? "true" : "false") << ","
             << "\"primary_cross_adapter_row_major_texture_supported\":"
             << (selectedDevices.Primary && selectedDevices.Primary->IsCrossAdapterTextureSupported() ? "true" : "false")
             << ","
             << "\"secondary_cross_adapter_row_major_texture_supported\":"
             << (selectedDevices.Secondary && selectedDevices.Secondary->IsCrossAdapterTextureSupported()
                     ? "true"
                     : "false")
             << ","
             << "\"color_format\":\"R8G8B8A8_UNORM\","
             << "\"depth_format\":\"R32_FLOAT\","
             << "\"color_format_supported\":" << (colorFormatSupported ? "true" : "false") << ","
             << "\"depth_format_supported\":" << (depthFormatSupported ? "true" : "false") << "}";

    std::ostringstream creationJson;
    creationJson << "  \"creation_open_records\":["
                 << CreationRecordJson(fenceRecord) << ","
                 << CreationRecordJson(colorRecord) << ","
                 << CreationRecordJson(depthRecord) << "]";

    result.PreflightJsonFragment = adaptersJson.str() + ",\n" + pairJson.str() + ",\n" + creationJson.str();
    auto recordStatus = [](const CreationRecord& record)
    {
        if (!record.Attempted)
            return "SKIPPED";
        return record.Passed ? "PASS" : "FAIL";
    };
    text << "pair=" << result.AdapterPairIdentity << "\n"
         << "creation.sharedFence=" << recordStatus(fenceRecord)
         << " attempted=" << (fenceRecord.Attempted ? "true" : "false") << " create="
         << fenceRecord.CreateHResult << " handle=" << fenceRecord.SharedHandleHResult
         << " open=" << fenceRecord.OpenHandleHResult << "\n"
         << "creation.sharedColor=" << recordStatus(colorRecord)
         << " attempted=" << (colorRecord.Attempted ? "true" : "false") << " create="
         << colorRecord.CreateHResult << " handle=" << colorRecord.SharedHandleHResult
         << " open=" << colorRecord.OpenHandleHResult << " rowPitch=" << colorRecord.RowPitch << "\n"
         << "creation.sharedDepth=" << recordStatus(depthRecord)
         << " attempted=" << (depthRecord.Attempted ? "true" : "false") << " create="
         << depthRecord.CreateHResult << " handle=" << depthRecord.SharedHandleHResult
         << " open=" << depthRecord.OpenHandleHResult << " rowPitch=" << depthRecord.RowPitch << "\n";
    result.PreflightText = text.str();

    WriteOutputs(result);
    return result;
}

void TwoAdapterVerificationRunner::ExportRuntimeEvidence(TwoAdapterVerificationResult& result,
                                                         const TwoAdapterRuntimeEvidence& runtime) const
{
    result.Runtime = runtime;
    if (result.Status == TwoAdapterVerificationStatus::Pass)
    {
        result.Status = runtime.Passed
                            ? TwoAdapterVerificationStatus::Pass
                            : TwoAdapterVerificationStatus::Fail;
        if (!runtime.Passed)
        {
            for (const auto& reason : runtime.Reasons)
                result.Reasons.push_back(reason);
        }
    }
    WriteOutputs(result);
}
