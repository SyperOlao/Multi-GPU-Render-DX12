#include "Source/Benchmark/ResearchProvenance.h"

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

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

    std::string HexBytes(const uint8_t* bytes, const size_t byteCount)
    {
        std::ostringstream stream;
        stream << std::hex << std::setfill('0');
        for (size_t i = 0; i < byteCount; ++i)
            stream << std::setw(2) << static_cast<uint32_t>(bytes[i]);
        return stream.str();
    }

    std::string ConfigKey(const AutomaticBenchmarkConfig& config)
    {
        return config.ConfigId + "#rep" + std::to_string(config.Repetition);
    }
}

std::string ResearchProvenance::StatusName(const ResearchRunStatus status)
{
    switch (status)
    {
    case ResearchRunStatus::Pending: return "PENDING";
    case ResearchRunStatus::Running: return "RUNNING";
    case ResearchRunStatus::Complete: return "COMPLETE";
    case ResearchRunStatus::Blocked: return "BLOCKED";
    case ResearchRunStatus::Invalid: return "INVALID";
    case ResearchRunStatus::Cancelled: return "CANCELLED";
    case ResearchRunStatus::Interrupted: return "INTERRUPTED";
    default: return "INVALID";
    }
}

bool ResearchProvenance::IsTerminal(const ResearchRunStatus status)
{
    return status == ResearchRunStatus::Complete ||
        status == ResearchRunStatus::Blocked ||
        status == ResearchRunStatus::Invalid ||
        status == ResearchRunStatus::Cancelled ||
        status == ResearchRunStatus::Interrupted;
}

bool ResearchProvenance::CanTransition(const ResearchRunStatus from, const ResearchRunStatus to)
{
    if (from == to)
        return true;
    if (IsTerminal(from))
        return false;
    if (from == ResearchRunStatus::Pending)
    {
        return to == ResearchRunStatus::Running ||
            to == ResearchRunStatus::Blocked ||
            to == ResearchRunStatus::Invalid ||
            to == ResearchRunStatus::Cancelled ||
            to == ResearchRunStatus::Interrupted;
    }
    if (from == ResearchRunStatus::Running)
    {
        return to == ResearchRunStatus::Complete ||
            to == ResearchRunStatus::Blocked ||
            to == ResearchRunStatus::Invalid ||
            to == ResearchRunStatus::Cancelled ||
            to == ResearchRunStatus::Interrupted;
    }
    return false;
}

std::string ResearchProvenance::Sha256Hex(const std::string& text)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD dataLength = 0;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return "unknown: BCryptOpenAlgorithmProvider(SHA256) failed";
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &dataLength, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return "unknown: BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed";
    }

    std::vector<uint8_t> object(objectLength);
    std::array<uint8_t, 32> digest{};
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash,
                       reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
                       static_cast<ULONG>(text.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
    {
        if (hash)
            BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return "unknown: BCrypt SHA256 hash failed";
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return HexBytes(digest.data(), digest.size());
}

std::string ResearchProvenance::Sha256File(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return "unknown: file not readable";

    std::ostringstream bytes;
    bytes << file.rdbuf();
    return Sha256Hex(bytes.str());
}

std::string ResearchProvenance::CanonicalSerialize(const ResearchProvenanceRecord& record)
{
    std::ostringstream stream;
    stream << "schema_version=" << record.SchemaVersion << "\n";
    for (const auto& [key, value] : record.Fields)
        stream << key << "=" << value << "\n";
    return stream.str();
}

std::string ResearchProvenance::ProtocolHash(const ResearchProvenanceRecord& record)
{
    return Sha256Hex(CanonicalSerialize(record));
}

std::string ResearchProvenance::ToJson(const ResearchProvenanceRecord& record, const uint32_t indent)
{
    const std::string pad(indent, ' ');
    const std::string child(indent + 2, ' ');
    std::ostringstream json;
    json << pad << "\"provenance\":{\n"
         << child << "\"schema\":\"mgpu_research_provenance.v1\",\n"
         << child << "\"schema_version\":" << record.SchemaVersion << ",\n"
         << child << "\"canonical_sha256\":\"" << EscapeJson(ProtocolHash(record)) << "\",\n"
         << child << "\"fields\":{";
    bool first = true;
    for (const auto& [key, value] : record.Fields)
    {
        json << (first ? "\n" : ",\n")
             << child << "  \"" << EscapeJson(key) << "\":\"" << EscapeJson(value) << "\"";
        first = false;
    }
    json << (first ? "}" : "\n" + child + "}") << "\n"
         << pad << "}";
    return json.str();
}

std::string ResearchProvenance::CompareCompatible(
    const ResearchProvenanceRecord& expected,
    const ResearchProvenanceRecord& actual,
    const std::vector<std::string>& requiredFields)
{
    if (expected.Empty())
        return "current provenance record is missing";
    if (actual.Empty())
        return "evidence provenance record is missing";

    for (const auto& field : requiredFields)
    {
        const auto expectedIt = expected.Fields.find(field);
        const auto actualIt = actual.Fields.find(field);
        if (expectedIt == expected.Fields.end() || expectedIt->second.empty() ||
            expectedIt->second.rfind("unknown", 0) == 0 || expectedIt->second.rfind("Unknown", 0) == 0)
        {
            return "current provenance field is missing: " + field;
        }
        if (actualIt == actual.Fields.end() || actualIt->second.empty() ||
            actualIt->second.rfind("unknown", 0) == 0 || actualIt->second.rfind("Unknown", 0) == 0)
        {
            return "evidence provenance field is missing: " + field;
        }
        if (expectedIt->second != actualIt->second)
            return "stale evidence provenance field mismatch: " + field;
    }
    return {};
}

bool ResearchProvenance::WriteTextFileAtomic(const std::filesystem::path& path,
                                             const std::string& contents,
                                             std::string* reason)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        if (reason)
            *reason = "could not create parent directory: " + ec.message();
        return false;
    }

    const auto temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out.is_open())
        {
            if (reason)
                *reason = "could not open temp file for atomic write";
            return false;
        }
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!out.good())
        {
            if (reason)
                *reason = "failed to write temp file";
            return false;
        }
    }

    std::filesystem::rename(temp, path, ec);
    if (ec)
    {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temp, path, ec);
        if (ec)
        {
            if (reason)
                *reason = "atomic rename failed: " + ec.message();
            return false;
        }
    }
    return true;
}

ResearchStateValidationResult ResearchProvenance::ValidateCompleteSuite(
    const ResearchStateValidationInput& input,
    const uint32_t expectedWarmupFrames,
    const uint32_t expectedMeasuredFrames)
{
    ResearchStateValidationResult result{};
    result.Status = ResearchRunStatus::Complete;

    if (input.Configs.empty())
    {
        result.Status = ResearchRunStatus::Invalid;
        result.Reason = "suite manifest contains no configs";
        return result;
    }
    if (input.ExecutionConfigIds.size() != input.ExecutionStatuses.size() ||
        input.ExecutionConfigIds.size() != input.ExecutionWarmupFrames.size() ||
        input.ExecutionConfigIds.size() != input.ExecutionMeasuredFrames.size() ||
        input.ExecutionConfigIds.size() != input.ExecutionRepetitions.size())
    {
        result.Status = ResearchRunStatus::Invalid;
        result.Reason = "execution manifest vectors are inconsistent";
        return result;
    }

    std::map<std::string, const AutomaticBenchmarkConfig*> required;
    for (const auto& config : input.Configs)
    {
        const auto key = ConfigKey(config);
        if (!required.emplace(key, &config).second)
        {
            result.Status = ResearchRunStatus::Invalid;
            result.Reason = "duplicate config in suite manifest: " + config.ConfigId;
            return result;
        }
    }

    std::set<std::string> seen;
    for (size_t i = 0; i < input.ExecutionConfigIds.size(); ++i)
    {
        const auto key = input.ExecutionConfigIds[i] + "#rep" + std::to_string(input.ExecutionRepetitions[i]);
        if (required.find(key) == required.end())
        {
            result.Status = ResearchRunStatus::Invalid;
            result.Reason = "execution manifest does not map to exactly one suite config: " + input.ExecutionConfigIds[i];
            return result;
        }
        if (!seen.insert(key).second)
        {
            result.Status = ResearchRunStatus::Invalid;
            result.Reason = "duplicate execution manifest for config: " + input.ExecutionConfigIds[i];
            return result;
        }
        if (input.ExecutionStatuses[i] != "COMPLETE")
        {
            result.Status = input.ExecutionStatuses[i] == "RUNNING" || input.ExecutionStatuses[i] == "PENDING"
                                ? ResearchRunStatus::Interrupted
                                : ResearchRunStatus::Invalid;
            result.Reason = "execution is not COMPLETE: " + input.ExecutionConfigIds[i];
            return result;
        }
        if (input.ExecutionWarmupFrames[i] != expectedWarmupFrames ||
            input.ExecutionMeasuredFrames[i] != expectedMeasuredFrames)
        {
            result.Status = ResearchRunStatus::Invalid;
            result.Reason = "execution frame counts do not match suite manifest: " + input.ExecutionConfigIds[i];
            return result;
        }
    }

    if (seen.size() != required.size())
    {
        result.Status = ResearchRunStatus::Invalid;
        result.Reason = "missing execution manifest for one or more suite configs";
        return result;
    }
    if (input.Summaries.size() != required.size())
    {
        result.Status = ResearchRunStatus::Invalid;
        result.Reason = "runs.csv summary count does not match suite config count";
        return result;
    }
    for (const auto& summary : input.Summaries)
    {
        if (!summary.Valid || !summary.SkipReason.empty())
        {
            result.Status = ResearchRunStatus::Invalid;
            result.Reason = "one or more benchmark executions are invalid";
            return result;
        }
        if (summary.MeasuredFrameCount != expectedMeasuredFrames ||
            summary.ValidFrameCount != expectedMeasuredFrames ||
            summary.InvalidFrameCount != 0)
        {
            result.Status = ResearchRunStatus::Invalid;
            result.Reason = "measured/valid frame counts do not match suite manifest";
            return result;
        }
    }

    return result;
}
