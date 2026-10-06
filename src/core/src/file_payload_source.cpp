#include "tc/core/payload_source.hpp"

#include "tc/core/error.hpp"
#include "windows_payload_reader.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tc::core {

namespace {
void cancelled(std::stop_token stop)
{
    if (stop.stop_requested()) throw CoreError(ErrorCode::Cancelled, "Payload read was cancelled");
}
} // namespace

std::size_t PayloadReader::read(std::span<std::byte> buffer, std::stop_token stop)
{
    cancelled(stop);
    auto const n = read(buffer);
    cancelled(stop);
    return n;
}

std::unique_ptr<PayloadReader> PayloadSource::open(ManifestEntry const& entry, std::stop_token stop)
{
    cancelled(stop);
    auto reader = open(entry);
    cancelled(stop);
    return reader;
}

namespace {

#ifdef _WIN32
struct OwnedHandle {
    HANDLE value;
    ~OwnedHandle() { if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    HANDLE release() { auto const result = value; value = nullptr; return result; }
};

class Win32Reader final : public PayloadReader {
public:
    Win32Reader(HANDLE h, std::string label) : handle_(h), label_(std::move(label)) {}
    ~Win32Reader() override { CloseHandle(handle_); }
    Win32Reader(Win32Reader const&) = delete;
    Win32Reader& operator=(Win32Reader const&) = delete;

    std::size_t read(std::span<std::byte> buffer) override { return read(buffer, {}); }

    std::size_t read(std::span<std::byte> buffer, std::stop_token stop) override
    {
        cancelled(stop);
        if (buffer.empty()) return 0;
        OwnedHandle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (event.value == nullptr)
            throw CoreError(ErrorCode::SourceUnreadable, "Cannot create payload read event", static_cast<int>(GetLastError()));
        OVERLAPPED operation{};
        operation.hEvent = event.value;
        operation.Offset = static_cast<DWORD>(position_);
        operation.OffsetHigh = static_cast<DWORD>(position_ >> 32);
        // Registration precedes submission; a second cancel after ReadFile
        // covers a stop that raced with registering the kernel operation.
        std::stop_callback cancel(stop, [&] { CancelIoEx(handle_, &operation); });
        DWORD const want = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), 1u << 30));
        BOOL const immediate = ReadFile(handle_, buffer.data(), want, nullptr, &operation);
        DWORD error = immediate ? ERROR_SUCCESS : GetLastError();
        if (error != ERROR_SUCCESS && error != ERROR_IO_PENDING && error != ERROR_HANDLE_EOF) {
            cancelled(stop);
            throw CoreError(ErrorCode::SourceUnreadable, "Read failed: " + label_, static_cast<int>(error));
        }
        DWORD got = 0;
        if (error != ERROR_HANDLE_EOF) {
            if (stop.stop_requested()) CancelIoEx(handle_, &operation);
            // Always settle the operation before its event, OVERLAPPED or
            // caller-owned buffer can be released, even after cancellation.
            if (!GetOverlappedResult(handle_, &operation, &got, TRUE)) error = GetLastError();
            else error = ERROR_SUCCESS;
        }
        cancelled(stop);
        if (error == ERROR_OPERATION_ABORTED)
            throw CoreError(ErrorCode::Cancelled, "Payload read was cancelled", static_cast<int>(error));
        if (error == ERROR_HANDLE_EOF) return 0;
        if (error != ERROR_SUCCESS)
            throw CoreError(ErrorCode::SourceUnreadable, "Read failed: " + label_, static_cast<int>(error));
        position_ += got;
        return got;
    }

    std::optional<native::FileObservation> observe() override { return native::observe_handle(handle_); }

private:
    HANDLE handle_;
    std::string label_;
    std::uint64_t position_ = 0;
};

class Win32Source final : public PayloadSource {
public:
    std::unique_ptr<PayloadReader> open(ManifestEntry const& entry) override
    {
        std::string const label = to_utf8(entry.source_path.filename());
        HANDLE h = CreateFileW(native::extended_path(entry.source_path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD const err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                throw CoreError(ErrorCode::SourceMissing, "Source file not found: " + label, static_cast<int>(err));
            if (err == ERROR_SHARING_VIOLATION)
                throw CoreError(ErrorCode::SourceUnreadable,
                    "Source file is open for writing by another process: " + label, static_cast<int>(err));
            throw CoreError(ErrorCode::SourceUnreadable, "Cannot open source file: " + label, static_cast<int>(err));
        }
        return detail::take_windows_payload_reader(h, label);
    }
};

#else

class PosixReader final : public PayloadReader {
public:
    PosixReader(int fd, std::string label) : fd_(fd), label_(std::move(label)) {}
    ~PosixReader() override { ::close(fd_); }
    PosixReader(PosixReader const&) = delete;
    PosixReader& operator=(PosixReader const&) = delete;

    std::size_t read(std::span<std::byte> buffer) override
    {
        while (true) {
            ssize_t const n = ::read(fd_, buffer.data(), std::min<std::size_t>(buffer.size(), INT_MAX));
            if (n >= 0) return static_cast<std::size_t>(n);
            if (errno == EINTR) continue;
            int const err = errno;
            throw CoreError(ErrorCode::SourceUnreadable, "Read failed: " + label_ + ": " + std::strerror(err), err);
        }
    }

    std::optional<native::FileObservation> observe() override { return native::observe_fd(fd_); }

private:
    int fd_;
    std::string label_;
};

class PosixSource final : public PayloadSource {
public:
    std::unique_ptr<PayloadReader> open(ManifestEntry const& entry) override
    {
        std::string const label = to_utf8(entry.source_path.filename());
        int const fd = ::open(entry.source_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            int const err = errno;
            if (err == ENOENT || err == ENOTDIR)
                throw CoreError(ErrorCode::SourceMissing, "Source file not found: " + label, err);
            throw CoreError(ErrorCode::SourceUnreadable, "Cannot open source file: " + label + ": " + std::strerror(err), err);
        }
        return std::make_unique<PosixReader>(fd, label);
    }
};

#endif

} // namespace

#ifdef _WIN32
std::unique_ptr<PayloadReader> detail::take_windows_payload_reader(void* handle, std::string_view label)
{
    OwnedHandle owned{handle};
    auto reader = std::make_unique<Win32Reader>(handle, std::string(label));
    owned.release();
    return reader;
}
#endif

std::unique_ptr<PayloadSource> make_file_payload_source()
{
#ifdef _WIN32
    return std::make_unique<Win32Source>();
#else
    return std::make_unique<PosixSource>();
#endif
}

} // namespace tc::core
