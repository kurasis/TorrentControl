#include "tc/core/error.hpp"

namespace tc::core {

std::string_view to_string(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::SourceChanged: return "SOURCE_CHANGED";
    case ErrorCode::SourceMissing: return "SOURCE_MISSING";
    case ErrorCode::SourceUnreadable: return "SOURCE_UNREADABLE";
    case ErrorCode::PathCollision: return "PATH_COLLISION";
    case ErrorCode::InvalidPath: return "INVALID_PATH";
    case ErrorCode::EmptyPayload: return "EMPTY_PAYLOAD";
    case ErrorCode::UnsupportedFormat: return "UNSUPPORTED_FORMAT";
    case ErrorCode::InvalidMetainfo: return "INVALID_METAINFO";
    case ErrorCode::ResourceLimit: return "RESOURCE_LIMIT";
    case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
    case ErrorCode::OutputConflict: return "OUTPUT_CONFLICT";
    case ErrorCode::OutputWriteFailed: return "OUTPUT_WRITE_FAILED";
    case ErrorCode::Cancelled: return "CANCELLED";
    case ErrorCode::EngineError: return "ENGINE_ERROR";
    case ErrorCode::HydrationRequired: return "HYDRATION_REQUIRED";
    }
    return "UNKNOWN";
}

bool is_retryable(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::SourceChanged:
    case ErrorCode::SourceMissing:
    case ErrorCode::SourceUnreadable:
    case ErrorCode::OutputConflict:
    case ErrorCode::OutputWriteFailed:
    case ErrorCode::Cancelled:
        return true;
    default:
        return false;
    }
}

std::string_view to_string(Phase phase) noexcept
{
    switch (phase) {
    case Phase::Unspecified: return "unspecified";
    case Phase::Scanning: return "scanning";
    case Phase::Preflight: return "preflight";
    case Phase::Hashing: return "hashing";
    case Phase::Building: return "building";
    case Phase::Validating: return "validating";
    case Phase::Committing: return "committing";
    case Phase::Verifying: return "verifying";
    }
    return "unspecified";
}

CoreError::CoreError(ErrorCode code, std::string message, std::optional<int> os_error)
    : std::runtime_error(std::move(message)), code_(code), os_error_(os_error)
{
}

} // namespace tc::core
