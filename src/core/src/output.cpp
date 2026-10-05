#include "tc/core/output.hpp"

#include "tc/core/error.hpp"
#include "tc/core/metainfo.hpp"
#include "tc/core/native_fs.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace tc::core {

namespace {

[[noreturn]] void write_failed(std::string message, int os_error)
{
    throw CoreError(ErrorCode::OutputWriteFailed, std::move(message), os_error);
}

std::string same_path_key(fs::path const& p)
{
    std::string const s = to_utf8(p.lexically_normal());
#ifdef _WIN32
    return fold_case(s);
#else
    return s;
#endif
}

#ifdef _WIN32

void write_new_file(fs::path const& path, std::string_view bytes, bool& created)
{
    HANDLE h = CreateFileW(native::extended_path(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD const err = GetLastError();
        write_failed("Cannot create a temporary file in the destination folder", static_cast<int>(err));
    }
    created = true;
    std::size_t done = 0;
    DWORD err = 0;
    while (done < bytes.size() && err == 0) {
        DWORD const chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(h, bytes.data() + done, chunk, &written, nullptr)) err = GetLastError();
        else if (written == 0) err = ERROR_WRITE_FAULT;
        done += written;
    }
    if (err == 0 && !FlushFileBuffers(h)) err = GetLastError();
    if (!CloseHandle(h) && err == 0) err = GetLastError();
    if (err != 0) write_failed("Cannot write the torrent file", static_cast<int>(err));
}

void remove_file(fs::path const& path) { DeleteFileW(native::extended_path(path).c_str()); }

void move_into_place(fs::path const& from, fs::path const& to, bool replace)
{
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replace) flags |= MOVEFILE_REPLACE_EXISTING;
    if (MoveFileExW(native::extended_path(from).c_str(), native::extended_path(to).c_str(), flags)) return;
    DWORD const err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS)
        throw CoreError(ErrorCode::OutputConflict, "Another program created the output file while the torrent was being made",
            static_cast<int>(err));
    write_failed("Cannot move the torrent file into place", static_cast<int>(err));
}

bool is_network_path(fs::path const& p)
{
    std::wstring const& s = p.native();
    if (s.starts_with(L"\\\\?\\UNC\\") || (s.starts_with(L"\\\\") && !s.starts_with(L"\\\\?\\"))) return true;
    std::wstring const root = p.root_path().native();
    return !root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
}

#else

void write_new_file(fs::path const& path, std::string_view bytes, bool& created)
{
    int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        int const err = errno;
        write_failed(std::string("Cannot create a temporary file in the destination folder: ") + std::strerror(err), err);
    }
    created = true;
    std::size_t done = 0;
    int err = 0;
    while (done < bytes.size()) {
        ssize_t const n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = errno;
            break;
        }
        if (n == 0) {
            err = EIO;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    if (err == 0 && ::fsync(fd) != 0) err = errno;
    if (::close(fd) != 0 && err == 0) err = errno;
    if (err != 0) write_failed(std::string("Cannot write the torrent file: ") + std::strerror(err), err);
}

void remove_file(fs::path const& path) { ::unlink(path.c_str()); }

void sync_directory(fs::path const& dir)
{
    int const fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

void move_into_place(fs::path const& from, fs::path const& to, bool replace)
{
    if (replace) {
        if (::rename(from.c_str(), to.c_str()) != 0) {
            int const err = errno;
            write_failed(std::string("Cannot move the torrent file into place: ") + std::strerror(err), err);
        }
    } else if (::link(from.c_str(), to.c_str()) == 0) {
        // link() fails with EEXIST instead of replacing: no race window.
        ::unlink(from.c_str());
    } else {
        int const err = errno;
        if (err == EEXIST)
            throw CoreError(ErrorCode::OutputConflict, "Another program created the output file while the torrent was being made", err);
        // Filesystems without hard links: check, then rename (weaker guarantee).
        if (native::stat_entry(to))
            throw CoreError(ErrorCode::OutputConflict, "Another program created the output file while the torrent was being made");
        if (::rename(from.c_str(), to.c_str()) != 0) {
            int const rerr = errno;
            write_failed(std::string("Cannot move the torrent file into place: ") + std::strerror(rerr), rerr);
        }
    }
    sync_directory(to.parent_path());
}

bool is_network_path(fs::path const&) { return false; }

#endif

std::string read_back(fs::path const& path)
{
    std::error_code ec;
    auto const size = fs::file_size(path, ec);
    if (ec) write_failed("Cannot reopen the written torrent file", ec.value());
    std::string data(static_cast<std::size_t>(size), '\0');
    FILE* f = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&f, native::extended_path(path).c_str(), L"rb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) write_failed("Cannot reopen the written torrent file", errno);
    std::size_t const n = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (n != data.size()) write_failed("Cannot read back the written torrent file", 0);
    return data;
}

} // namespace

void check_output_target(fs::path const& output, Manifest const& manifest)
{
    std::error_code ec;
    fs::path const abs = fs::absolute(output, ec);
    std::string const key = same_path_key(ec ? output : abs);
    std::optional<native::FileObservation> const existing = native::observe(output);
    for (auto const& e : manifest.entries) {
        bool const same_path = same_path_key(e.source_path) == key;
        bool const same_identity = existing && existing->identity.valid && e.observed.identity.valid
            && existing->identity == e.observed.identity;
        if (same_path || same_identity) {
            CoreError err(ErrorCode::OutputConflict,
                (same_path ? "The output file is one of the selected source files: "
                           : "The output file is a hard link to a selected source file: ")
                    + manifest.torrent_path_string(e) + ". Choose another output path, or exclude that file from the source.");
            err.with_phase(Phase::Preflight).with_source(e.source_id);
            throw err;
        }
    }
}

fs::path temp_path_for(fs::path const& output)
{
    std::random_device rd;
    std::uniform_int_distribution<unsigned> dist(0, 15);
    std::string suffix;
    for (int i = 0; i < 16; ++i) suffix += "0123456789abcdef"[dist(rd)];
    return output.parent_path() / path_from_utf8("." + to_utf8(output.filename()) + ".tc-" + suffix + ".tmp");
}

CommitResult commit_output(fs::path const& output, std::string_view bytes, CommitOptions const& options)
{
    std::error_code ec;
    fs::path const target = fs::absolute(output, ec).lexically_normal();
    if (ec || target.filename().empty())
        throw CoreError(ErrorCode::InvalidArgument, "Invalid output file name").with_phase(Phase::Committing);

    try {
        auto const dir = native::stat_entry(target.parent_path());
        if (!dir || dir->kind != native::EntryKind::Directory)
            throw CoreError(ErrorCode::OutputWriteFailed, "The destination folder does not exist");
        if (options.manifest != nullptr) check_output_target(target, *options.manifest);

        auto const existing = native::stat_entry(target);
        if (existing && existing->kind != native::EntryKind::Regular)
            throw CoreError(ErrorCode::OutputConflict, "The output path exists and is not an ordinary file");
        if (existing && !options.replace_existing)
            throw CoreError(ErrorCode::OutputConflict, "The output file already exists; choose Replace to overwrite it");

        // Validate the bytes before touching the destination folder.
        Metainfo const expected = Metainfo::parse(std::string(bytes));

        fs::path const temp = temp_path_for(target);
        bool created = false;
        try {
            write_new_file(temp, bytes, created);
            if (options.on_stage) options.on_stage(CommitStage::TempWritten);
            std::string const written = read_back(temp);
            Metainfo const reopened = Metainfo::parse(written);
            if (written != bytes || reopened.raw_info() != expected.raw_info()
                || reopened.info_hashes().v1 != expected.info_hashes().v1
                || reopened.info_hashes().v2 != expected.info_hashes().v2)
                throw CoreError(ErrorCode::OutputWriteFailed, "The written torrent file does not match what was generated");
            if (options.on_stage) options.on_stage(CommitStage::BeforeCommit);
            move_into_place(temp, target, options.replace_existing);
        } catch (...) {
            // A failed exclusive create must never delete somebody else's file.
            if (created) remove_file(temp);
            throw;
        }

        CommitResult result;
        result.path = target;
        result.replaced_existing = existing.has_value();
        if (is_network_path(target))
            result.guarantee_note = "The destination is a network location; atomic replacement depends on the server.";
        return result;
    } catch (CoreError& e) {
        if (e.phase() == Phase::Unspecified) e.with_phase(Phase::Committing);
        throw;
    }
}

} // namespace tc::core
