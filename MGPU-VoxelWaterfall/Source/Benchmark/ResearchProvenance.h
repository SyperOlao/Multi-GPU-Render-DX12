#pragma once

#include "Source/Benchmark/AutomaticBenchmarkRunner.h"
#include "Source/Benchmark/VoxelBenchmarkProfiler.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

enum class ResearchRunStatus
{
    Pending,
    Running,
    Complete,
    Blocked,
    Invalid,
    Cancelled,
    Interrupted
};

struct ResearchProvenanceRecord
{
    uint32_t SchemaVersion = 1;
    std::map<std::string, std::string> Fields;

    bool Empty() const { return Fields.empty(); }
};

struct ResearchStateValidationInput
{
    BenchmarkSuite Suite = BenchmarkSuite::Smoke;
    std::vector<AutomaticBenchmarkConfig> Configs;
    std::vector<std::string> ExecutionConfigIds;
    std::vector<std::string> ExecutionStatuses;
    std::vector<uint32_t> ExecutionWarmupFrames;
    std::vector<uint32_t> ExecutionMeasuredFrames;
    std::vector<uint32_t> ExecutionRepetitions;
    std::vector<VoxelBenchmarkProfiler::BenchmarkSummary> Summaries;
};

struct ResearchStateValidationResult
{
    ResearchRunStatus Status = ResearchRunStatus::Invalid;
    std::string Reason;
};

namespace ResearchProvenance
{
    std::string StatusName(ResearchRunStatus status);
    bool IsTerminal(ResearchRunStatus status);
    bool CanTransition(ResearchRunStatus from, ResearchRunStatus to);

    std::string Sha256Hex(const std::string& text);
    std::string Sha256File(const std::filesystem::path& path);
    std::string CanonicalSerialize(const ResearchProvenanceRecord& record);
    std::string ProtocolHash(const ResearchProvenanceRecord& record);
    std::string ToJson(const ResearchProvenanceRecord& record, uint32_t indent = 2);
    std::string CompareCompatible(const ResearchProvenanceRecord& expected,
                                  const ResearchProvenanceRecord& actual,
                                  const std::vector<std::string>& requiredFields);

    bool WriteTextFileAtomic(const std::filesystem::path& path,
                             const std::string& contents,
                             std::string* reason = nullptr);

    ResearchStateValidationResult ValidateCompleteSuite(
        const ResearchStateValidationInput& input,
        uint32_t expectedWarmupFrames,
        uint32_t expectedMeasuredFrames);
}
