#pragma once

// Output commit (specification section 14.1).
//
// Created metainfo is written to a uniquely named temporary file in the
// destination folder, flushed, reopened and validated, and only then moved
// into place with a same-volume rename. An existing destination is replaced
// only with an explicit choice; a destination created by another process
// while the job ran is reported as a conflict, never overwritten silently.

#include "tc/core/manifest.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace tc::core {
class Metainfo;

// Throws CoreError(OutputConflict) when `output` is one of the manifest's
// source files, by path or by file identity (hard-link aliases included).
void check_output_target(std::filesystem::path const& output, Manifest const& manifest);

// Name of a new temporary file next to `output`. It matches the default
// exclusion rule for TorrentControl temporary files.
std::filesystem::path temp_path_for(std::filesystem::path const& output);

enum class CommitStage {
    // The temporary file is written, flushed and closed.
    TempWritten,
    // Validation passed; the rename is next.
    BeforeCommit,
    // Windows rename was blocked; the same atomic operation will be retried.
    RenameRetry,
};

struct CommitOptions {
    bool replace_existing = false;
    // Editor only: tolerate legacy v1 key ordering solely when the candidate
    // contains this validated import's exact original raw info slice.
    Metainfo const* preserved_info = nullptr;
    // When set, the output may not be one of these sources (section 14.1 step 1).
    Manifest const* manifest = nullptr;
    // Test seam: called at each stage. Throwing aborts the commit, which must
    // then leave the previous output untouched and remove the temporary file.
    std::function<void(CommitStage)> on_stage = {};
};

struct CommitResult {
    std::filesystem::path path;
    bool replaced_existing = false;
    // Set when the destination cannot promise an atomic replace (for example
    // a network share); shown to the user instead of claiming atomicity.
    std::string guarantee_note;
};

// Writes `bytes` (metainfo) to `output`. Throws CoreError with
// OutputConflict (exists without replace, output is a source, or a race),
// OutputWriteFailed (I/O errors such as a full disk or denied access), or
// InvalidMetainfo when the reopened file does not validate.
CommitResult commit_output(std::filesystem::path const& output, std::string_view bytes, CommitOptions const& options = {});

} // namespace tc::core
