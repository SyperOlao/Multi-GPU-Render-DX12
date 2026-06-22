#include "Source/Benchmark/VoxelBenchmarkProfiler.h"

#include "d3dUtil.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <sstream>

using Microsoft::WRL::ComPtr;
using namespace PEPEngine::Graphics;

namespace
{
    std::string TimestampForFile()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm localTime{};
        localtime_s(&localTime, &nowTime);

        std::ostringstream stream;
        stream << std::put_time(&localTime, "%Y%m%d_%H%M%S");
        return stream.str();
    }

    const char* BoolText(const bool value)
    {
        return value ? "true" : "false";
    }

    std::string Hex64(const uint64_t value)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
        return stream.str();
    }

    std::string HResultHex(const HRESULT hr)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::setw(8) << std::setfill('0')
               << static_cast<uint32_t>(hr);
        return stream.str();
    }

    void WriteQueueCalibrationHeader(std::ostream& stream, const char* prefix)
    {
        stream << prefix << "_timestamp_frequency,"
               << prefix << "_clock_calibration_hresult,"
               << prefix << "_calibration_cpu_qpc,"
               << prefix << "_calibration_gpu_timestamp,"
               << prefix << "_calibration_valid,"
               << prefix << "_calibration_monotonic,";
    }

    void WriteQueueCalibrationValues(std::ostream& stream,
                                     const uint64_t frequency,
                                     const HRESULT hresult,
                                     const uint64_t cpuQpc,
                                     const uint64_t gpuTimestamp,
                                     const bool valid,
                                     const bool monotonic)
    {
        stream << frequency << ','
               << HResultHex(hresult) << ','
               << cpuQpc << ','
               << gpuTimestamp << ','
               << BoolText(valid) << ','
               << BoolText(monotonic) << ',';
    }
}

void VoxelBenchmarkProfiler::Initialize(const std::shared_ptr<GDevice>& primaryDevice,
                                        const std::shared_ptr<GDevice>& secondaryDevice,
                                        const std::shared_ptr<GCommandQueue>& primaryComputeQueue,
                                        const std::shared_ptr<GCommandQueue>& primaryGraphicsQueue,
                                        const std::shared_ptr<GCommandQueue>& secondaryComputeQueue,
                                        const std::shared_ptr<GCommandQueue>& secondaryGraphicsQueue,
                                        const std::shared_ptr<GCommandQueue>& secondaryCopyQueue,
                                        const std::shared_ptr<GCommandQueue>& primaryCopyQueue)
{
    queues[ToIndex(QueueId::PrimaryCompute)] = {primaryDevice, primaryComputeQueue};
    queues[ToIndex(QueueId::PrimaryGraphics)] = {primaryDevice, primaryGraphicsQueue};
    queues[ToIndex(QueueId::SecondaryCompute)] = {secondaryDevice, secondaryComputeQueue};
    queues[ToIndex(QueueId::SecondaryGraphics)] = {secondaryDevice, secondaryGraphicsQueue};
    queues[ToIndex(QueueId::SecondaryCopy)] = {secondaryDevice, secondaryCopyQueue};
    queues[ToIndex(QueueId::PrimaryCopy)] = {primaryDevice, primaryCopyQueue};

    QueryPerformanceFrequency(&qpcFrequency);

    for (auto& queue : queues)
    {
        if (queue.Device && queue.Queue)
        {
            queue.Frequency = std::max<uint64_t>(1, queue.Queue->GetTimestampFreq());
            CreateQueueResources(queue);
            queue.Valid = true;
        }
    }

    CalibrateQueues();
    initialized = queues[ToIndex(QueueId::PrimaryCompute)].Valid &&
        queues[ToIndex(QueueId::PrimaryGraphics)].Valid &&
        queues[ToIndex(QueueId::PrimaryCopy)].Valid;
}

float VoxelBenchmarkProfiler::GetProgress() const
{
    if (!active && rowsWritten >= recordedFrameCount)
        return 1.0f;

    const float total = static_cast<float>(warmupFrameCount + recordedFrameCount);
    return std::min(1.0f, static_cast<float>(framesSeen + rowsWritten) / total);
}

bool VoxelBenchmarkProfiler::Start(const std::filesystem::path& outputDirectory, const FrameMetadata& metadata)
{
    const std::string modeToken = SanitizeFileToken(metadata.RequestedMode);
    return Start(outputDirectory, metadata,
                 "VoxelBenchmark_" + modeToken + "_" + TimestampForFile() + "_" +
                 std::to_string(metadata.TotalVoxelCount) + ".csv",
                 "", 0);
}

bool VoxelBenchmarkProfiler::Start(const std::filesystem::path& outputDirectory,
                                   const FrameMetadata& metadata,
                                   const std::string& fileName,
                                   const std::string& presetName,
                                   const uint32_t repetition,
                                   const uint32_t warmupFrames,
                                   const uint32_t measuredFrames,
                                   const std::string& suiteName,
                                   const std::string& runId,
                                   const std::string& configId,
                                   const std::string& pairId,
                                   const uint32_t orderIndex,
                                   const uint32_t randomizationSeed)
{
    if (!initialized)
        return false;

    Stop();
    CalibrateQueues();

    std::filesystem::create_directories(outputDirectory);
    csvPath = outputDirectory / fileName;

    csv.open(csvPath, std::ios::out | std::ios::trunc);
    if (!csv.is_open())
        return false;

    ResetSamples();
    warmupFrameCount = warmupFrames;
    recordedFrameCount = measuredFrames;
    latestTimingSnapshot = {};
    currentPresetName = presetName;
    currentRepetition = repetition;
    currentSuiteName = suiteName;
    currentRunId = runId;
    currentConfigId = configId;
    currentPairId = pairId;
    currentOrderIndex = orderIndex;
    currentRandomizationSeed = randomizationSeed;
    completedSummaryReady = false;
    csv.imbue(std::locale::classic());
    csv << "frame_index,suite,run_id,config_id,pair_id,repetition,randomized_order_index,randomization_seed,"
        << "profile,scene_preset,requested_mode,actual_mode,fallback_reason,"
        << "benchmark_config_class,benchmark_config_reason,temporal_policy,spatial_lod_policy,"
        << "partition_strategy,load_balance_scenario,total_voxels,actual_static_voxels,actual_dynamic_voxels,"
        << "static_budget,dynamic_budget,voxel_size,chunk_size_x,chunk_size_y,chunk_size_z,secondary_share,"
        << "primary_partition_voxels,secondary_partition_voxels,visible_primary_count,visible_secondary_count,"
        << "updated_voxels,simulation_steps,simulation_dispatches,seed,render_width,render_height,"
        << "render_resolution_preset,camera_path,camera_fov_degrees,camera_near_plane,camera_far_plane,"
        << "lighting_preset,dynamic_shadows_enabled,"
        << "primary_adapter,secondary_adapter,primary_vendor_id,primary_device_id,"
        << "primary_luid,primary_dedicated_memory,secondary_vendor_id,secondary_device_id,secondary_luid,"
        << "secondary_dedicated_memory,operating_system,build_configuration,git_commit,d3d12_debug_layer,";
    WriteQueueCalibrationHeader(csv, "primary_compute_queue");
    WriteQueueCalibrationHeader(csv, "primary_graphics_queue");
    WriteQueueCalibrationHeader(csv, "secondary_compute_queue");
    WriteQueueCalibrationHeader(csv, "secondary_graphics_queue");
    WriteQueueCalibrationHeader(csv, "secondary_copy_queue");
    WriteQueueCalibrationHeader(csv, "primary_copy_queue");
    csv << "cpu_wait_ms,"
        << "cpu_frame_ms,critical_path_gpu_ms,gpu_work_sum_ms,primary_compute_ms,"
        << "primary_lod_compaction_ms,primary_base_graphics_ms,secondary_compute_ms,"
        << "secondary_lod_compaction_ms,secondary_graphics_ms,"
        << "secondary_local_to_shared_copy_ms,primary_shared_to_local_copy_ms,transfer_ms,"
        << "composite_ms,final_resolve_ui_ms,present_ready_gpu_ms,total_cross_adapter_bytes,"
        << "color_transfer_bytes,depth_transfer_bytes,particle_transfer_bytes,render_output_transfer_bytes,"
        << "secondary_draw_calls,primary_submitted_voxels,secondary_submitted_voxels,"
        << "primary_rendered_voxels,secondary_rendered_voxels,primary_lod0,primary_lod1,primary_lod2,"
        << "secondary_lod0,secondary_lod1,secondary_lod2,"
        << "visual_validation_has_result,visual_validation_passed,visual_validation_run_id,"
        << "visual_validation_snapshot_hash,visual_color_mae,visual_color_rmse,"
        << "visual_psnr,visual_max_error,visual_mismatched_pixel_percent,visual_depth_rmse,"
        << "visual_depth_mismatched_pixel_percent,visual_pipeline_primitive_count,visual_fail_reason,"
        << "frame_valid,invalid_reason\n";

    framesSeen = 0;
    rowsWritten = 0;
    for (auto& frame : frames)
        frame = {};
    currentFrame = nullptr;
    active = true;
    return true;
}

void VoxelBenchmarkProfiler::Stop()
{
    if (csv.is_open())
    {
        csv.flush();
        csv.close();
    }
    active = false;
    currentFrame = nullptr;
}

void VoxelBenchmarkProfiler::BeginFrame(const FrameMetadata& metadata)
{
    if (!active || rowsWritten >= recordedFrameCount)
        return;

    ProcessCompletedFrames();

    const uint32_t slot = static_cast<uint32_t>(metadata.FrameIndex % RingFrameCount);
    auto& frame = frames[slot];
    if (frame.Active && !frame.Written)
    {
        if (frame.CsvEligible && IsFrameReady(frame))
            WriteFrame(frame);
        if (frame.Active && !frame.Written)
        {
            currentFrame = nullptr;
            return;
        }
    }

    frame = {};
    frame.Active = true;
    frame.CsvEligible = framesSeen >= warmupFrameCount;
    frame.Written = false;
    frame.Slot = slot;
    frame.Metadata = metadata;
    currentFrame = &frame;
    ++framesSeen;
}

void VoxelBenchmarkProfiler::EndFrameCpu()
{
    if (!active || !currentFrame)
        return;

    const auto now = std::chrono::steady_clock::now();
    currentFrame->CpuFrameMs = std::chrono::duration<double, std::milli>(
        now - currentFrame->Metadata.CpuFrameStart).count();
}

void VoxelBenchmarkProfiler::UpdateCurrentFrameMetadata(const FrameMetadata& metadata)
{
    if (!active || !currentFrame)
        return;

    const auto cpuStart = currentFrame->Metadata.CpuFrameStart;
    currentFrame->Metadata = metadata;
    currentFrame->Metadata.CpuFrameStart = cpuStart;
}

void VoxelBenchmarkProfiler::ProcessCompletedFrames()
{
    if (!active)
        return;

    for (auto& frame : frames)
    {
        if (!frame.Active || !frame.CsvEligible || frame.Written)
            continue;

        if (IsFrameReady(frame))
            WriteFrame(frame);
    }

    if (rowsWritten >= recordedFrameCount)
    {
        FinalizeCompletedSummary();
        Stop();
    }
}

void VoxelBenchmarkProfiler::BeginRange(const std::shared_ptr<GCommandList>& cmdList,
                                        const QueueId queue,
                                        const RangeId range)
{
    if (!active || !currentFrame || !cmdList)
        return;

    const auto queueIndex = ToIndex(queue);
    if (!queues[queueIndex].Valid)
        return;

    const uint32_t queryIndex = QueryIndex(currentFrame->Slot, range, false);
    cmdList->EndQuery(queues[queueIndex].QueryHeap.Get(), queryIndex);
    currentFrame->Ranges[ToIndex(range)] = {true, queue};
}

void VoxelBenchmarkProfiler::EndRange(const std::shared_ptr<GCommandList>& cmdList,
                                      const QueueId queue,
                                      const RangeId range)
{
    if (!active || !currentFrame || !cmdList)
        return;

    const auto queueIndex = ToIndex(queue);
    if (!queues[queueIndex].Valid)
        return;

    const uint32_t queryIndex = QueryIndex(currentFrame->Slot, range, true);
    cmdList->EndQuery(queues[queueIndex].QueryHeap.Get(), queryIndex);
}

void VoxelBenchmarkProfiler::ResolveRange(const std::shared_ptr<GCommandList>& cmdList,
                                          const QueueId queue,
                                          const RangeId range)
{
    if (!active || !currentFrame || !cmdList)
        return;

    const auto queueIndex = ToIndex(queue);
    if (!queues[queueIndex].Valid)
        return;

    const uint32_t firstQuery = QueryIndex(currentFrame->Slot, range, false);
    cmdList->ResolveQuery(queues[queueIndex].QueryHeap.Get(),
                          queues[queueIndex].ReadbackBuffer.Get(),
                          firstQuery, 2, QueryOffset(currentFrame->Slot, range));
}

void VoxelBenchmarkProfiler::SetQueueFence(const QueueId queue, const uint64_t fenceValue)
{
    if (!active || !currentFrame)
        return;

    currentFrame->FenceValues[ToIndex(queue)] = fenceValue;
}

std::string VoxelBenchmarkProfiler::ToUtf8(const std::wstring& value)
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

std::string VoxelBenchmarkProfiler::EscapeCsv(const std::string& value)
{
    if (value.find_first_of(",\"\n\r") == std::string::npos)
        return value;

    std::string escaped = "\"";
    for (const char ch : value)
    {
        if (ch == '"')
            escaped += "\"\"";
        else
            escaped += ch;
    }
    escaped += '"';
    return escaped;
}

std::string VoxelBenchmarkProfiler::EscapeCsv(const std::wstring& value)
{
    return EscapeCsv(ToUtf8(value));
}

std::string VoxelBenchmarkProfiler::SanitizeFileToken(const std::string& value)
{
    std::string result = value;
    for (char& ch : result)
    {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_')
            ch = '_';
    }
    return result.empty() ? "Unknown" : result;
}

uint32_t VoxelBenchmarkProfiler::QueryIndex(const uint32_t slot, const RangeId range, const bool endQuery) const
{
    return slot * QueryCountPerFrame + ToIndex(range) * 2 + (endQuery ? 1u : 0u);
}

uint64_t VoxelBenchmarkProfiler::QueryOffset(const uint32_t slot, const RangeId range) const
{
    return static_cast<uint64_t>(slot * QueryCountPerFrame + ToIndex(range) * 2) * sizeof(uint64_t);
}

bool VoxelBenchmarkProfiler::IsQueueComplete(const QueueContext& queue, const uint64_t fenceValue) const
{
    return fenceValue == 0 || (queue.Queue && queue.Queue->IsFinish(fenceValue));
}

bool VoxelBenchmarkProfiler::IsFrameReady(const FrameRecord& frame) const
{
    for (uint32_t range = 0; range < RangeCount; ++range)
    {
        if (!frame.Ranges[range].Active)
            continue;

        const auto queue = frame.Ranges[range].Queue;
        const auto queueIndex = ToIndex(queue);
        if (!queues[queueIndex].Valid || !IsQueueComplete(queues[queueIndex], frame.FenceValues[queueIndex]))
            return false;
    }

    return true;
}

VoxelBenchmarkProfiler::RangeTiming VoxelBenchmarkProfiler::ReadRangeTiming(
    const FrameRecord& frame,
    const RangeId range) const
{
    const auto& rangeRecord = frame.Ranges[ToIndex(range)];
    if (!rangeRecord.Active)
        return {};

    const auto& queue = queues[ToIndex(rangeRecord.Queue)];
    if (!queue.Valid)
        return {};

    const uint64_t offset = QueryOffset(frame.Slot, range);
    D3D12_RANGE readRange{offset, offset + 2 * sizeof(uint64_t)};
    void* mappedData = nullptr;
    ThrowIfFailed(queue.ReadbackBuffer->Map(0, &readRange, &mappedData));

    const auto* timestamps = reinterpret_cast<const uint64_t*>(
        static_cast<const uint8_t*>(mappedData) + offset);
    const uint64_t start = timestamps[0];
    const uint64_t end = timestamps[1];

    const D3D12_RANGE emptyRange{0, 0};
    queue.ReadbackBuffer->Unmap(0, &emptyRange);

    const uint64_t delta = end >= start ? end - start : 0;
    const double ms = static_cast<double>(delta) * 1000.0 / static_cast<double>(queue.Frequency);
    const double qpcPerGpuTick = static_cast<double>(qpcFrequency.QuadPart) /
        static_cast<double>(queue.Frequency);

    return {
        ms,
        static_cast<double>(queue.CalibrationCpuQpc) +
        (static_cast<double>(start) - static_cast<double>(queue.CalibrationGpuTimestamp)) * qpcPerGpuTick,
        static_cast<double>(queue.CalibrationCpuQpc) +
        (static_cast<double>(end) - static_cast<double>(queue.CalibrationGpuTimestamp)) * qpcPerGpuTick
    };
}

void VoxelBenchmarkProfiler::WriteFrame(const FrameRecord& frame)
{
    if (!csv.is_open() || rowsWritten >= recordedFrameCount)
        return;

    const auto primaryCompute = ReadRangeTiming(frame, RangeId::PrimaryCompute);
    const auto primaryLodCompaction = ReadRangeTiming(frame, RangeId::PrimaryLodCompaction);
    const auto primaryBaseGraphics = ReadRangeTiming(frame, RangeId::PrimaryBaseGraphics);
    const auto secondaryCompute = ReadRangeTiming(frame, RangeId::SecondaryCompute);
    const auto secondaryLodCompaction = ReadRangeTiming(frame, RangeId::SecondaryLodCompaction);
    const auto secondaryGraphics = ReadRangeTiming(frame, RangeId::SecondaryGraphics);
    const auto secondaryCopy = ReadRangeTiming(frame, RangeId::SecondaryLocalToSharedCopy);
    const auto primaryCopy = ReadRangeTiming(frame, RangeId::PrimarySharedToLocalCopy);
    const auto composite = ReadRangeTiming(frame, RangeId::Composite);
    const auto finalResolveUi = ReadRangeTiming(frame, RangeId::FinalResolveUi);

    const std::array timings = {
        primaryCompute, primaryLodCompaction, primaryBaseGraphics,
        secondaryCompute, secondaryLodCompaction, secondaryGraphics,
        secondaryCopy, primaryCopy, composite, finalResolveUi
    };

    double firstStart = std::numeric_limits<double>::max();
    double finalEnd = 0.0;
    double gpuWorkSum = 0.0;
    bool timestampsValid = false;
    bool activeQueueCalibrationValid = true;
    for (uint32_t i = 0; i < RangeCount; ++i)
    {
        if (!frame.Ranges[i].Active)
            continue;
        timestampsValid = true;
        const auto& queue = queues[ToIndex(frame.Ranges[i].Queue)];
        activeQueueCalibrationValid = activeQueueCalibrationValid &&
            queue.CalibrationValid && queue.CalibrationMonotonic && queue.Frequency > 0;
        firstStart = std::min(firstStart, timings[i].StartQpc);
        finalEnd = std::max(finalEnd, timings[i].EndQpc);
        gpuWorkSum += timings[i].Ms;
    }
    timestampsValid = timestampsValid && activeQueueCalibrationValid;

    if (frame.Ranges[ToIndex(RangeId::FinalResolveUi)].Active)
        finalEnd = finalResolveUi.EndQpc;

    const double criticalPathGpuMs =
        firstStart < std::numeric_limits<double>::max() && finalEnd >= firstStart
            ? (finalEnd - firstStart) * 1000.0 / static_cast<double>(qpcFrequency.QuadPart)
            : 0.0;
    const double transferMs = secondaryCopy.Ms + primaryCopy.Ms;
    const double presentReadyGpuMs = criticalPathGpuMs;
    const std::string invalidReason = ValidateFrameRecord(frame, timestampsValid);

    cpuFrameMsSamples.push_back(frame.CpuFrameMs);
    criticalPathGpuMsSamples.push_back(criticalPathGpuMs);
    gpuWorkSumMsSamples.push_back(gpuWorkSum);
    primaryComputeMsSamples.push_back(primaryCompute.Ms);
    primaryLodCompactionMsSamples.push_back(primaryLodCompaction.Ms);
    primaryGraphicsMsSamples.push_back(primaryBaseGraphics.Ms);
    secondaryComputeMsSamples.push_back(secondaryCompute.Ms);
    secondaryLodCompactionMsSamples.push_back(secondaryLodCompaction.Ms);
    secondaryGraphicsMsSamples.push_back(secondaryGraphics.Ms);
    transferMsSamples.push_back(transferMs);
    compositeMsSamples.push_back(composite.Ms);
    transferBytesSamples.push_back(frame.Metadata.TotalCrossAdapterBytes);
    colorTransferBytesSamples.push_back(frame.Metadata.ColorTransferBytes);
    depthTransferBytesSamples.push_back(frame.Metadata.DepthTransferBytes);
    particleTransferBytesSamples.push_back(frame.Metadata.ParticleTransferBytes);
    renderOutputTransferBytesSamples.push_back(frame.Metadata.RenderOutputTransferBytes);
    secondaryDrawCallSamples.push_back(static_cast<double>(frame.Metadata.SecondaryDrawCalls));
    primarySubmittedVoxelSamples.push_back(static_cast<double>(frame.Metadata.PrimarySubmittedVoxelCount));
    secondarySubmittedVoxelSamples.push_back(static_cast<double>(frame.Metadata.SecondarySubmittedVoxelCount));
    primaryLod0Samples.push_back(static_cast<double>(frame.Metadata.PrimaryLod0Count));
    primaryLod1Samples.push_back(static_cast<double>(frame.Metadata.PrimaryLod1Count));
    primaryLod2Samples.push_back(static_cast<double>(frame.Metadata.PrimaryLod2Count));
    secondaryLod0Samples.push_back(static_cast<double>(frame.Metadata.SecondaryLod0Count));
    secondaryLod1Samples.push_back(static_cast<double>(frame.Metadata.SecondaryLod1Count));
    secondaryLod2Samples.push_back(static_cast<double>(frame.Metadata.SecondaryLod2Count));
    if (!invalidReason.empty())
        invalidReasons.push_back(invalidReason);

    latestTimingSnapshot = {
        true,
        primaryCompute.Ms,
        primaryLodCompaction.Ms,
        primaryBaseGraphics.Ms,
        secondaryCompute.Ms,
        secondaryLodCompaction.Ms,
        secondaryGraphics.Ms,
        transferMs,
        composite.Ms,
        finalResolveUi.Ms,
        criticalPathGpuMs,
        gpuWorkSum,
        frame.Metadata.TotalCrossAdapterBytes,
        frame.Metadata.ParticleTransferBytes,
        frame.Metadata.SecondaryDrawCalls,
        frame.Metadata.VisualValidationPassed
    };

    csv << frame.Metadata.FrameIndex << ','
        << EscapeCsv(currentSuiteName) << ','
        << EscapeCsv(currentRunId) << ','
        << EscapeCsv(currentConfigId) << ','
        << EscapeCsv(currentPairId) << ','
        << currentRepetition << ','
        << currentOrderIndex << ','
        << currentRandomizationSeed << ','
        << EscapeCsv(frame.Metadata.ProfileName) << ','
        << EscapeCsv(frame.Metadata.ScenePreset) << ','
        << EscapeCsv(frame.Metadata.RequestedMode) << ','
        << EscapeCsv(frame.Metadata.ActualMode) << ','
        << EscapeCsv(frame.Metadata.FallbackReason) << ','
        << EscapeCsv(frame.Metadata.BenchmarkConfigClass) << ','
        << EscapeCsv(frame.Metadata.BenchmarkConfigReason) << ','
        << EscapeCsv(frame.Metadata.TemporalPolicy) << ','
        << EscapeCsv(frame.Metadata.SpatialLodPolicy) << ','
        << EscapeCsv(frame.Metadata.PartitionStrategy) << ','
        << EscapeCsv(frame.Metadata.LoadBalanceScenario) << ','
        << frame.Metadata.TotalVoxelCount << ','
        << frame.Metadata.ActualStaticVoxelCount << ','
        << frame.Metadata.ActualDynamicVoxelCount << ','
        << frame.Metadata.StaticVoxelBudget << ','
        << frame.Metadata.DynamicVoxelBudget << ','
        << frame.Metadata.VoxelSize << ','
        << frame.Metadata.ChunkSizeX << ','
        << frame.Metadata.ChunkSizeY << ','
        << frame.Metadata.ChunkSizeZ << ','
        << frame.Metadata.SecondaryShare << ','
        << frame.Metadata.PrimaryPartitionVoxelCount << ','
        << frame.Metadata.SecondaryPartitionVoxelCount << ','
        << frame.Metadata.PrimaryRenderedVoxelCount << ','
        << frame.Metadata.SecondaryRenderedVoxelCount << ','
        << frame.Metadata.UpdatedVoxelCount << ','
        << frame.Metadata.SimulationStepsThisFrame << ','
        << frame.Metadata.SimulationDispatchCount << ','
        << frame.Metadata.Seed << ','
        << frame.Metadata.RenderWidth << ','
        << frame.Metadata.RenderHeight << ','
        << EscapeCsv(frame.Metadata.RenderResolutionPreset) << ','
        << EscapeCsv(frame.Metadata.CameraPath) << ','
        << frame.Metadata.CameraFovDegrees << ','
        << frame.Metadata.CameraNearPlane << ','
        << frame.Metadata.CameraFarPlane << ','
        << EscapeCsv(frame.Metadata.LightingPreset) << ','
        << BoolText(frame.Metadata.DynamicShadowsEnabled) << ','
        << EscapeCsv(frame.Metadata.PrimaryAdapterName) << ','
        << EscapeCsv(frame.Metadata.SecondaryAdapterName) << ','
        << frame.Metadata.PrimaryVendorId << ','
        << frame.Metadata.PrimaryDeviceId << ','
        << EscapeCsv(frame.Metadata.PrimaryAdapterLuid) << ','
        << frame.Metadata.PrimaryDedicatedVideoMemory << ','
        << frame.Metadata.SecondaryVendorId << ','
        << frame.Metadata.SecondaryDeviceId << ','
        << EscapeCsv(frame.Metadata.SecondaryAdapterLuid) << ','
        << frame.Metadata.SecondaryDedicatedVideoMemory << ','
        << EscapeCsv(frame.Metadata.OperatingSystem) << ','
        << EscapeCsv(frame.Metadata.BuildConfiguration) << ','
        << EscapeCsv(frame.Metadata.GitCommit) << ','
        << BoolText(frame.Metadata.D3D12DebugLayerEnabled) << ',';
    for (const auto& queue : queues)
    {
        WriteQueueCalibrationValues(csv, queue.Frequency,
                                    queue.CalibrationHResult,
                                    queue.CalibrationCpuQpc,
                                    queue.CalibrationGpuTimestamp,
                                    queue.CalibrationValid,
                                    queue.CalibrationMonotonic);
    }
    csv
        << std::fixed << std::setprecision(6)
        << frame.Metadata.CpuWaitMs << ','
        << frame.CpuFrameMs << ','
        << criticalPathGpuMs << ','
        << gpuWorkSum << ','
        << primaryCompute.Ms << ','
        << primaryLodCompaction.Ms << ','
        << primaryBaseGraphics.Ms << ','
        << secondaryCompute.Ms << ','
        << secondaryLodCompaction.Ms << ','
        << secondaryGraphics.Ms << ','
        << secondaryCopy.Ms << ','
        << primaryCopy.Ms << ','
        << transferMs << ','
        << composite.Ms << ','
        << finalResolveUi.Ms << ','
        << presentReadyGpuMs << ','
        << frame.Metadata.TotalCrossAdapterBytes << ','
        << frame.Metadata.ColorTransferBytes << ','
        << frame.Metadata.DepthTransferBytes << ','
        << frame.Metadata.ParticleTransferBytes << ','
        << frame.Metadata.RenderOutputTransferBytes << ','
        << frame.Metadata.SecondaryDrawCalls << ','
        << frame.Metadata.PrimarySubmittedVoxelCount << ','
        << frame.Metadata.SecondarySubmittedVoxelCount << ','
        << frame.Metadata.PrimaryRenderedVoxelCount << ','
        << frame.Metadata.SecondaryRenderedVoxelCount << ','
        << frame.Metadata.PrimaryLod0Count << ','
        << frame.Metadata.PrimaryLod1Count << ','
        << frame.Metadata.PrimaryLod2Count << ','
        << frame.Metadata.SecondaryLod0Count << ','
        << frame.Metadata.SecondaryLod1Count << ','
        << frame.Metadata.SecondaryLod2Count << ','
        << BoolText(frame.Metadata.VisualValidationHasResult) << ','
        << BoolText(frame.Metadata.VisualValidationPassed) << ','
        << EscapeCsv(frame.Metadata.VisualValidationRunId) << ','
        << EscapeCsv(Hex64(frame.Metadata.VisualValidationSnapshotHash)) << ','
        << frame.Metadata.VisualValidationColorMAE << ','
        << frame.Metadata.VisualValidationColorRMSE << ','
        << frame.Metadata.VisualValidationPSNR << ','
        << frame.Metadata.VisualValidationMaxError << ','
        << frame.Metadata.VisualValidationMismatchedPixelPercent << ','
        << frame.Metadata.VisualValidationDepthRMSE << ','
        << frame.Metadata.VisualValidationDepthMismatchPercent << ','
        << frame.Metadata.VisualValidationPipelinePrimitiveCount << ','
        << EscapeCsv(frame.Metadata.VisualValidationFailReason) << ','
        << BoolText(invalidReason.empty()) << ','
        << EscapeCsv(invalidReason) << '\n';

    ++rowsWritten;
    if (rowsWritten % 32 == 0)
        csv.flush();

    const_cast<FrameRecord&>(frame).Written = true;
}

void VoxelBenchmarkProfiler::CreateQueueResources(QueueContext& context)
{
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = QueryCountPerQueue;
    heapDesc.NodeMask = context.Device->GetNodeMask();
    ThrowIfFailed(context.Device->GetDXDevice()->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&context.QueryHeap)));

    const auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(QueryCountPerQueue * sizeof(uint64_t));
    const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    ThrowIfFailed(context.Device->GetDXDevice()->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &readbackDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&context.ReadbackBuffer)));
}

std::string VoxelBenchmarkProfiler::ValidateFrameRecord(
    const FrameRecord& frame,
    const bool timestampsValid) const
{
    const auto& metadata = frame.Metadata;
    const bool requestedMultiGpu = metadata.RequestedMode == "MultiGpuFull" ||
        metadata.RequestedMode == "MultiGpuTemporalDecimation";
    if (!timestampsValid)
        return "GPU timestamp query failed";
    if (metadata.BenchmarkConfigClass == "Invalid mixed-quality configuration")
        return metadata.BenchmarkConfigReason.empty()
                   ? "invalid mixed-quality configuration"
                   : metadata.BenchmarkConfigReason;
    if (metadata.ProfileName == "StaticRenderOnly" && metadata.SimulationDispatchCount != 0)
        return "StaticRenderOnly recorded simulation dispatch";
    if (metadata.ProfileName == "DynamicSimulationAndRender" && metadata.ActualStaticVoxelCount != 0)
        return "DynamicSimulationAndRender contains static voxels";
    if (metadata.ProfileName == "MixedStaticAndDynamic" &&
        (metadata.ActualStaticVoxelCount == 0 || metadata.ActualDynamicVoxelCount == 0))
        return "MixedStaticAndDynamic does not contain both static and dynamic voxels";
    if (requestedMultiGpu && metadata.ActualMode != metadata.RequestedMode)
        return "requested multi-GPU mode fell back to a different actual mode";
    if (requestedMultiGpu &&
        metadata.SecondaryPartitionVoxelCount > 0 &&
        metadata.SecondaryDrawCalls == 0)
        return "secondary graphics draw count is zero for a non-empty secondary partition";
    if (metadata.ParticleTransferBytes > 0)
        return "particle transfer bytes are non-zero";
    if (requestedMultiGpu && metadata.RenderOutputTransferBytes == 0)
        return "render-output transfer bytes are zero";
    if (!metadata.VisualValidationHasResult)
        return "visual validation metrics are not available";
    if (!metadata.VisualValidationPassed)
        return metadata.VisualValidationFailReason.empty()
                   ? "visual validation failed"
                   : metadata.VisualValidationFailReason;
    return {};
}

void VoxelBenchmarkProfiler::CalibrateQueues()
{
    for (auto& queue : queues)
    {
        if (!queue.Valid || !queue.Queue)
            continue;

        UINT64 frequency = 0;
        HRESULT hr = queue.Queue->GetD3D12CommandQueue()->GetTimestampFrequency(&frequency);
        queue.CalibrationHResult = hr;
        if (SUCCEEDED(hr) && frequency > 0)
        {
            queue.Frequency = frequency;
        }
        else
        {
            queue.CalibrationValid = false;
            queue.CalibrationMonotonic = false;
            continue;
        }

        const auto previousGpu = queue.CalibrationGpuTimestamp;
        const auto previousCpu = queue.CalibrationCpuQpc;
        uint64_t gpuTimestamp = 0;
        uint64_t cpuTimestamp = 0;
        hr = queue.Queue->GetD3D12CommandQueue()->GetClockCalibration(&gpuTimestamp, &cpuTimestamp);
        queue.CalibrationHResult = hr;
        if (SUCCEEDED(hr))
        {
            queue.CalibrationGpuTimestamp = gpuTimestamp;
            queue.CalibrationCpuQpc = cpuTimestamp;
            queue.CalibrationMonotonic =
                previousGpu == 0 || (gpuTimestamp >= previousGpu && cpuTimestamp >= previousCpu);
            queue.CalibrationValid = queue.Frequency > 0 && queue.CalibrationMonotonic;
        }
        else
        {
            queue.CalibrationValid = false;
            queue.CalibrationMonotonic = false;
        }
    }
}

void VoxelBenchmarkProfiler::ResetSamples()
{
    cpuFrameMsSamples.clear();
    criticalPathGpuMsSamples.clear();
    gpuWorkSumMsSamples.clear();
    primaryComputeMsSamples.clear();
    primaryLodCompactionMsSamples.clear();
    primaryGraphicsMsSamples.clear();
    secondaryComputeMsSamples.clear();
    secondaryLodCompactionMsSamples.clear();
    secondaryGraphicsMsSamples.clear();
    transferMsSamples.clear();
    compositeMsSamples.clear();
    transferBytesSamples.clear();
    colorTransferBytesSamples.clear();
    depthTransferBytesSamples.clear();
    particleTransferBytesSamples.clear();
    renderOutputTransferBytesSamples.clear();
    secondaryDrawCallSamples.clear();
    primarySubmittedVoxelSamples.clear();
    secondarySubmittedVoxelSamples.clear();
    primaryLod0Samples.clear();
    primaryLod1Samples.clear();
    primaryLod2Samples.clear();
    secondaryLod0Samples.clear();
    secondaryLod1Samples.clear();
    secondaryLod2Samples.clear();
    invalidReasons.clear();
}

void VoxelBenchmarkProfiler::FinalizeCompletedSummary()
{
    if (completedSummaryReady || rowsWritten < recordedFrameCount || cpuFrameMsSamples.empty())
        return;

    FrameRecord* lastWritten = nullptr;
    for (auto& frame : frames)
    {
        if (frame.Written)
        {
            if (!lastWritten || frame.Metadata.FrameIndex > lastWritten->Metadata.FrameIndex)
                lastWritten = &frame;
        }
    }

    completedSummary = {};
    completedSummary.RequestedMode = lastWritten ? lastWritten->Metadata.RequestedMode : "";
    completedSummary.ActualMode = lastWritten ? lastWritten->Metadata.ActualMode : "";
    completedSummary.Preset =
        !currentPresetName.empty()
            ? currentPresetName
            : (lastWritten ? lastWritten->Metadata.ScenePreset : "");
    completedSummary.ProfileName = lastWritten ? lastWritten->Metadata.ProfileName : "";
    completedSummary.PartitionStrategy = lastWritten ? lastWritten->Metadata.PartitionStrategy : "";
    completedSummary.LoadBalanceScenario = lastWritten ? lastWritten->Metadata.LoadBalanceScenario : "";
    completedSummary.BenchmarkConfigClass = lastWritten ? lastWritten->Metadata.BenchmarkConfigClass : "";
    completedSummary.TemporalPolicy = lastWritten ? lastWritten->Metadata.TemporalPolicy : "";
    completedSummary.SpatialLodPolicy = lastWritten ? lastWritten->Metadata.SpatialLodPolicy : "";
    completedSummary.PairId = currentPairId;
    completedSummary.PrimaryAdapterName = lastWritten ? lastWritten->Metadata.PrimaryAdapterName : L"";
    completedSummary.SecondaryAdapterName = lastWritten ? lastWritten->Metadata.SecondaryAdapterName : L"";
    completedSummary.TotalVoxelCount = lastWritten ? lastWritten->Metadata.TotalVoxelCount : 0;
    completedSummary.ActualStaticVoxelCount = lastWritten ? lastWritten->Metadata.ActualStaticVoxelCount : 0;
    completedSummary.ActualDynamicVoxelCount = lastWritten ? lastWritten->Metadata.ActualDynamicVoxelCount : 0;
    completedSummary.SecondaryShare = lastWritten ? lastWritten->Metadata.SecondaryShare : 0.0f;
    completedSummary.RenderWidth = lastWritten ? lastWritten->Metadata.RenderWidth : 0;
    completedSummary.RenderHeight = lastWritten ? lastWritten->Metadata.RenderHeight : 0;
    completedSummary.Repetition = currentRepetition;
    completedSummary.RepetitionCount = 1;
    completedSummary.MeasuredFrameCount = static_cast<uint32_t>(cpuFrameMsSamples.size());
    completedSummary.InvalidFrameCount = static_cast<uint32_t>(invalidReasons.size());
    completedSummary.ValidFrameCount =
        completedSummary.MeasuredFrameCount >= completedSummary.InvalidFrameCount
            ? completedSummary.MeasuredFrameCount - completedSummary.InvalidFrameCount
            : 0;
    completedSummary.Valid = invalidReasons.empty();
    completedSummary.ValidityReason = invalidReasons.empty() ? "" : invalidReasons.front();
    completedSummary.AverageCpuFrameMs = Average(cpuFrameMsSamples);
    completedSummary.MedianCpuFrameMs = Percentile(cpuFrameMsSamples, 0.50);
    completedSummary.P95CpuFrameMs = Percentile(cpuFrameMsSamples, 0.95);
    completedSummary.P99CpuFrameMs = Percentile(cpuFrameMsSamples, 0.99);
    completedSummary.StdDevCpuFrameMs = StdDev(cpuFrameMsSamples);
    completedSummary.CpuFrameCi95HalfWidthMs = 0.0;
    completedSummary.SpeedupStatistic = "descriptive_frame_distribution_only";
    completedSummary.CriticalPathGpuMs = Average(criticalPathGpuMsSamples);
    completedSummary.GpuWorkSumMs = Average(gpuWorkSumMsSamples);
    completedSummary.PrimaryComputeMs = Average(primaryComputeMsSamples);
    completedSummary.PrimaryLodCompactionMs = Average(primaryLodCompactionMsSamples);
    completedSummary.PrimaryGraphicsMs = Average(primaryGraphicsMsSamples);
    completedSummary.SecondaryComputeMs = Average(secondaryComputeMsSamples);
    completedSummary.SecondaryLodCompactionMs = Average(secondaryLodCompactionMsSamples);
    completedSummary.SecondaryGraphicsMs = Average(secondaryGraphicsMsSamples);
    completedSummary.TransferMs = Average(transferMsSamples);
    completedSummary.CompositeMs = Average(compositeMsSamples);
    completedSummary.AverageTransferBytes = static_cast<uint64_t>(AverageUint64(transferBytesSamples));
    completedSummary.AverageColorTransferBytes =
        static_cast<uint64_t>(AverageUint64(colorTransferBytesSamples));
    completedSummary.AverageDepthTransferBytes =
        static_cast<uint64_t>(AverageUint64(depthTransferBytesSamples));
    completedSummary.AverageParticleTransferBytes =
        static_cast<uint64_t>(AverageUint64(particleTransferBytesSamples));
    completedSummary.AverageRenderOutputTransferBytes =
        static_cast<uint64_t>(AverageUint64(renderOutputTransferBytesSamples));
    completedSummary.AverageSecondaryDrawCalls = Average(secondaryDrawCallSamples);
    completedSummary.AveragePrimarySubmittedVoxelCount = Average(primarySubmittedVoxelSamples);
    completedSummary.AverageSecondarySubmittedVoxelCount = Average(secondarySubmittedVoxelSamples);
    completedSummary.AveragePrimaryLod0Count = Average(primaryLod0Samples);
    completedSummary.AveragePrimaryLod1Count = Average(primaryLod1Samples);
    completedSummary.AveragePrimaryLod2Count = Average(primaryLod2Samples);
    completedSummary.AverageSecondaryLod0Count = Average(secondaryLod0Samples);
    completedSummary.AverageSecondaryLod1Count = Average(secondaryLod1Samples);
    completedSummary.AverageSecondaryLod2Count = Average(secondaryLod2Samples);
    completedSummary.VisualValidationPassed =
        lastWritten ? lastWritten->Metadata.VisualValidationPassed : false;
    completedSummary.CsvPath = csvPath;
    completedSummaryReady = true;
}

double VoxelBenchmarkProfiler::Average(const std::vector<double>& values)
{
    if (values.empty())
        return 0.0;

    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

double VoxelBenchmarkProfiler::AverageUint64(const std::vector<uint64_t>& values)
{
    if (values.empty())
        return 0.0;

    long double sum = 0.0;
    for (const auto value : values)
        sum += static_cast<long double>(value);
    return static_cast<double>(sum / static_cast<long double>(values.size()));
}

double VoxelBenchmarkProfiler::StdDev(const std::vector<double>& values)
{
    if (values.size() < 2)
        return 0.0;

    const double mean = Average(values);
    double sumSquares = 0.0;
    for (const double value : values)
    {
        const double delta = value - mean;
        sumSquares += delta * delta;
    }
    return std::sqrt(sumSquares / static_cast<double>(values.size() - 1));
}

double VoxelBenchmarkProfiler::Percentile(std::vector<double> values, const double percentile)
{
    if (values.empty())
        return 0.0;

    std::sort(values.begin(), values.end());
    const double clamped = std::clamp(percentile, 0.0, 1.0);
    const size_t index = static_cast<size_t>(std::round(clamped * static_cast<double>(values.size() - 1)));
    return values[index];
}

VoxelBenchmarkProfiler::BenchmarkSummary VoxelBenchmarkProfiler::ConsumeCompletedSummary()
{
    completedSummaryReady = false;
    return completedSummary;
}
