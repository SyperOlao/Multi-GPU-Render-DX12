#include "VoxelBenchmarkProfiler.h"

#include "d3dUtil.h"

#include <algorithm>
#include <iomanip>
#include <locale>
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
}

void VoxelBenchmarkProfiler::Initialize(const std::shared_ptr<GDevice>& primaryDevice,
                                        const std::shared_ptr<GDevice>& secondaryDevice,
                                        const std::shared_ptr<GCommandQueue>& primaryComputeQueue,
                                        const std::shared_ptr<GCommandQueue>& secondaryComputeQueue,
                                        const std::shared_ptr<GCommandQueue>& transferQueue,
                                        const std::shared_ptr<GCommandQueue>& graphicsQueue)
{
    queues[ToIndex(QueueId::PrimaryCompute)] = {primaryDevice, primaryComputeQueue};
    queues[ToIndex(QueueId::SecondaryCompute)] = {secondaryDevice, secondaryComputeQueue};
    queues[ToIndex(QueueId::Transfer)] = {primaryDevice, transferQueue};
    queues[ToIndex(QueueId::Graphics)] = {primaryDevice, graphicsQueue};

    for (auto& queue : queues)
    {
        if (queue.Device && queue.Queue)
        {
            queue.Frequency = std::max<uint64_t>(1, queue.Queue->GetTimestampFreq());
            CreateQueueResources(queue);
            queue.Valid = true;
        }
    }

    initialized = queues[ToIndex(QueueId::PrimaryCompute)].Valid &&
        queues[ToIndex(QueueId::Graphics)].Valid;
}

float VoxelBenchmarkProfiler::GetProgress() const
{
    if (!active && rowsWritten >= RecordedFrameCount)
        return 1.0f;

    const float total = static_cast<float>(WarmupFrameCount + RecordedFrameCount);
    return std::min(1.0f, static_cast<float>(framesSeen + rowsWritten) / total);
}

bool VoxelBenchmarkProfiler::Start(const std::filesystem::path& outputDirectory, const FrameMetadata& metadata)
{
    const std::string modeToken = SanitizeFileToken(metadata.ExecutionMode);
    return Start(outputDirectory, metadata,
                 "VoxelBenchmark_" + modeToken + "_" + TimestampForFile() + "_" +
                 std::to_string(metadata.TotalVoxelCount) + ".csv",
                 "");
}

bool VoxelBenchmarkProfiler::Start(const std::filesystem::path& outputDirectory, const FrameMetadata& metadata,
                                   const std::string& fileName, const std::string& presetName)
{
    if (!initialized)
        return false;

    Stop();

    std::filesystem::create_directories(outputDirectory);
    csvPath = outputDirectory / fileName;

    csv.open(csvPath, std::ios::out | std::ios::trunc);
    if (!csv.is_open())
        return false;

    ResetSamples();
    currentPresetName = presetName;
    completedSummaryReady = false;
    csv.imbue(std::locale::classic());
    csv << "frame_index,execution_mode,near_voxel_count,medium_voxel_count,far_voxel_count,"
        << "total_voxel_count,updated_voxel_count,medium_update_interval,far_update_interval,"
        << "primary_adapter_name,secondary_adapter_name,primary_compute_ms,secondary_compute_ms,"
        << "cross_adapter_transfer_ms,graphics_ms,primary_wait_ms,secondary_wait_ms,"
        << "synchronization_ms,cpu_frame_ms,gpu_frame_ms\n";

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
    if (!active || rowsWritten >= RecordedFrameCount)
        return;

    ProcessCompletedFrames();

    const uint32_t slot = static_cast<uint32_t>(metadata.FrameIndex % RingFrameCount);
    auto& frame = frames[slot];
    if (frame.Active && frame.CsvEligible && !frame.Written && IsFrameReady(frame))
        WriteFrame(frame);

    frame = {};
    frame.Active = true;
    frame.CsvEligible = framesSeen >= WarmupFrameCount;
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

    if (rowsWritten >= RecordedFrameCount)
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

    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};

    std::string result(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr);
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

double VoxelBenchmarkProfiler::ReadRangeMs(const FrameRecord& frame, const RangeId range) const
{
    const auto& rangeRecord = frame.Ranges[ToIndex(range)];
    if (!rangeRecord.Active)
        return 0.0;

    const auto& queue = queues[ToIndex(rangeRecord.Queue)];
    if (!queue.Valid)
        return 0.0;

    const uint64_t offset = QueryOffset(frame.Slot, range);
    D3D12_RANGE readRange{offset, offset + 2 * sizeof(uint64_t)};
    void* mappedData = nullptr;
    ThrowIfFailed(queue.ReadbackBuffer->Map(0, &readRange, &mappedData));

    const auto* timestamps = reinterpret_cast<const uint64_t*>(
        static_cast<const uint8_t*>(mappedData) + offset);
    const uint64_t delta = timestamps[1] >= timestamps[0] ? timestamps[1] - timestamps[0] : 0;

    const D3D12_RANGE emptyRange{0, 0};
    queue.ReadbackBuffer->Unmap(0, &emptyRange);

    return static_cast<double>(delta) * 1000.0 / static_cast<double>(queue.Frequency);
}

void VoxelBenchmarkProfiler::WriteFrame(const FrameRecord& frame)
{
    if (!csv.is_open() || rowsWritten >= RecordedFrameCount)
        return;

    double primaryComputeMs = 0.0;
    double secondaryComputeMs = 0.0;
    const std::array computeRanges = {
        RangeId::NearCompute,
        RangeId::MediumCompute,
        RangeId::FarCompute
    };

    for (const auto range : computeRanges)
    {
        const auto& rangeRecord = frame.Ranges[ToIndex(range)];
        const double ms = ReadRangeMs(frame, range);
        if (rangeRecord.Active && rangeRecord.Queue == QueueId::SecondaryCompute)
            secondaryComputeMs += ms;
        else
            primaryComputeMs += ms;
    }

    const double transferMs = ReadRangeMs(frame, RangeId::CrossAdapterTransfer);
    const double graphicsMs = ReadRangeMs(frame, RangeId::Graphics);
    const double synchronizationMs = frame.Metadata.PrimaryWaitMs + frame.Metadata.SecondaryWaitMs;
    const double gpuFrameMs = primaryComputeMs + secondaryComputeMs + transferMs + graphicsMs;

    frameMsSamples.push_back(frame.CpuFrameMs);
    primaryComputeMsSamples.push_back(primaryComputeMs);
    secondaryComputeMsSamples.push_back(secondaryComputeMs);
    transferMsSamples.push_back(transferMs);
    syncMsSamples.push_back(synchronizationMs);
    graphicsMsSamples.push_back(graphicsMs);

    csv << frame.Metadata.FrameIndex << ','
        << EscapeCsv(frame.Metadata.ExecutionMode) << ','
        << frame.Metadata.NearVoxelCount << ','
        << frame.Metadata.MediumVoxelCount << ','
        << frame.Metadata.FarVoxelCount << ','
        << frame.Metadata.TotalVoxelCount << ','
        << frame.Metadata.UpdatedVoxelCount << ','
        << frame.Metadata.MediumUpdateInterval << ','
        << frame.Metadata.FarUpdateInterval << ','
        << EscapeCsv(frame.Metadata.PrimaryAdapterName) << ','
        << EscapeCsv(frame.Metadata.SecondaryAdapterName) << ','
        << std::fixed << std::setprecision(6)
        << primaryComputeMs << ','
        << secondaryComputeMs << ','
        << transferMs << ','
        << graphicsMs << ','
        << frame.Metadata.PrimaryWaitMs << ','
        << frame.Metadata.SecondaryWaitMs << ','
        << synchronizationMs << ','
        << frame.CpuFrameMs << ','
        << gpuFrameMs << '\n';

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

void VoxelBenchmarkProfiler::ResetSamples()
{
    frameMsSamples.clear();
    primaryComputeMsSamples.clear();
    secondaryComputeMsSamples.clear();
    transferMsSamples.clear();
    syncMsSamples.clear();
    graphicsMsSamples.clear();
}

void VoxelBenchmarkProfiler::FinalizeCompletedSummary()
{
    if (completedSummaryReady || rowsWritten < RecordedFrameCount || frameMsSamples.empty())
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
    completedSummary.Mode = lastWritten ? lastWritten->Metadata.ExecutionMode : "";
    completedSummary.Preset = currentPresetName;
    completedSummary.TotalVoxelCount = lastWritten ? lastWritten->Metadata.TotalVoxelCount : 0;
    completedSummary.AverageFrameMs = Average(frameMsSamples);
    completedSummary.MedianFrameMs = Percentile(frameMsSamples, 0.50);
    completedSummary.P95FrameMs = Percentile(frameMsSamples, 0.95);
    completedSummary.AveragePrimaryComputeMs = Average(primaryComputeMsSamples);
    completedSummary.AverageSecondaryComputeMs = Average(secondaryComputeMsSamples);
    completedSummary.AverageTransferMs = Average(transferMsSamples);
    completedSummary.AverageSyncMs = Average(syncMsSamples);
    completedSummary.AverageGraphicsMs = Average(graphicsMsSamples);
    completedSummary.Target60FpsReached = completedSummary.AverageFrameMs <= 16.67;
    completedSummary.CsvPath = csvPath;
    completedSummaryReady = true;
}

double VoxelBenchmarkProfiler::Average(const std::vector<double>& values)
{
    if (values.empty())
        return 0.0;

    double sum = 0.0;
    for (const double value : values)
        sum += value;
    return sum / static_cast<double>(values.size());
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
