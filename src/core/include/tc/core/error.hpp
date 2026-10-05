#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tc::core {

// Stable error codes (specification section 14.2). The string form returned by
// to_string() is part of the external contract and must not change.
enum class ErrorCode {
    SourceChanged,
    SourceMissing,
    SourceUnreadable,
    PathCollision,
    InvalidPath,
    EmptyPayload,
    UnsupportedFormat,
    InvalidMetainfo,
    ResourceLimit,
    InvalidArgument,
    OutputConflict,
    OutputWriteFailed,
    Cancelled,
    EngineError,
    // A cloud placeholder would have to be downloaded before it can be hashed
    // and the selected policy does not allow that (section 5.2).
    HydrationRequired,
};

std::string_view to_string(ErrorCode code) noexcept;

// True when repeating the same operation can succeed without changing the
// configuration (for example after the user closes a program that holds a
// file open, or frees disk space).
bool is_retryable(ErrorCode code) noexcept;

// Processing phase in which an error occurred (section 9.1 pipeline).
enum class Phase {
    Unspecified,
    Scanning,
    Preflight,
    Hashing,
    Building,
    Validating,
    Committing,
    Verifying,
};

std::string_view to_string(Phase phase) noexcept;

// Exception type used by the core. Every failure that crosses a module
// boundary carries a stable code and a message that is safe to show to the
// user (no secrets, no raw authenticated URLs).
class CoreError : public std::runtime_error {
public:
    CoreError(ErrorCode code, std::string message, std::optional<int> os_error = std::nullopt);

    ErrorCode code() const noexcept { return code_; }
    std::optional<int> os_error() const noexcept { return os_error_; }
    bool retryable() const noexcept { return is_retryable(code_); }

    Phase phase() const noexcept { return phase_; }
    // Manifest source ID the error refers to, when there is one.
    std::string const& source_id() const noexcept { return source_id_; }

    CoreError& with_phase(Phase phase) noexcept
    {
        phase_ = phase;
        return *this;
    }
    CoreError& with_source(std::string source_id)
    {
        source_id_ = std::move(source_id);
        return *this;
    }

private:
    ErrorCode code_;
    std::optional<int> os_error_;
    Phase phase_ = Phase::Unspecified;
    std::string source_id_;
};

} // namespace tc::core
