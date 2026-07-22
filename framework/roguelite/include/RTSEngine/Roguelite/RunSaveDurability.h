#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rts::roguelite {

enum class RunSaveDurabilityOperation : std::uint8_t {
    None,
    SyncFile,
    ReplaceFile,
    RemoveFile,
    SyncDirectory
};

enum class RunSaveDurabilityError : std::uint8_t {
    None,
    OpenFailed,
    SyncFailed,
    ReplaceFailed,
    RemoveFailed,
    DirectoryOpenFailed,
    DirectorySyncFailed
};

struct RunSaveDurabilityResult final {
    RunSaveDurabilityOperation operation{RunSaveDurabilityOperation::None};
    RunSaveDurabilityError error{RunSaveDurabilityError::None};
    std::int64_t nativeCode{};

    explicit operator bool() const noexcept {
        return error == RunSaveDurabilityError::None;
    }
};

class IRunSaveDurability {
public:
    virtual ~IRunSaveDurability() = default;
    virtual RunSaveDurabilityResult syncFile(
        const std::filesystem::path& path) noexcept = 0;
    virtual RunSaveDurabilityResult replaceFile(
        const std::filesystem::path& source,
        const std::filesystem::path& destination) noexcept = 0;
    virtual RunSaveDurabilityResult removeFile(
        const std::filesystem::path& path) noexcept = 0;
    virtual RunSaveDurabilityResult syncDirectory(
        const std::filesystem::path& path) noexcept = 0;
};

class PlatformRunSaveDurability final : public IRunSaveDurability {
public:
    RunSaveDurabilityResult syncFile(
        const std::filesystem::path& path) noexcept override {
#if defined(_WIN32)
        const HANDLE handle = ::CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return failure(RunSaveDurabilityOperation::SyncFile,
                           RunSaveDurabilityError::OpenFailed,
                           ::GetLastError());
        }
        const bool synced = ::FlushFileBuffers(handle) != 0;
        const auto native = synced ? 0u : ::GetLastError();
        ::CloseHandle(handle);
        return synced
            ? success(RunSaveDurabilityOperation::SyncFile)
            : failure(RunSaveDurabilityOperation::SyncFile,
                      RunSaveDurabilityError::SyncFailed, native);
#else
        const int descriptor = ::open(path.c_str(), O_RDONLY);
        if (descriptor < 0) {
            return failure(RunSaveDurabilityOperation::SyncFile,
                           RunSaveDurabilityError::OpenFailed, errno);
        }
        bool synced = false;
#if defined(__APPLE__) && defined(F_FULLFSYNC)
        synced = ::fcntl(descriptor, F_FULLFSYNC) == 0;
        if (!synced) synced = ::fsync(descriptor) == 0;
#else
        synced = ::fsync(descriptor) == 0;
#endif
        const int native = synced ? 0 : errno;
        ::close(descriptor);
        return synced
            ? success(RunSaveDurabilityOperation::SyncFile)
            : failure(RunSaveDurabilityOperation::SyncFile,
                      RunSaveDurabilityError::SyncFailed, native);
#endif
    }

    RunSaveDurabilityResult replaceFile(
        const std::filesystem::path& source,
        const std::filesystem::path& destination) noexcept override {
#if defined(_WIN32)
        if (::MoveFileExW(
                source.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
            return failure(RunSaveDurabilityOperation::ReplaceFile,
                           RunSaveDurabilityError::ReplaceFailed,
                           ::GetLastError());
        }
        return success(RunSaveDurabilityOperation::ReplaceFile);
#else
        if (::rename(source.c_str(), destination.c_str()) != 0) {
            return failure(RunSaveDurabilityOperation::ReplaceFile,
                           RunSaveDurabilityError::ReplaceFailed, errno);
        }
        auto directory = destination.parent_path();
        if (directory.empty()) directory = ".";
        return syncDirectory(directory);
#endif
    }

    RunSaveDurabilityResult removeFile(
        const std::filesystem::path& path) noexcept override {
#if defined(_WIN32)
        if (::DeleteFileW(path.c_str()) == 0) {
            const auto native = ::GetLastError();
            if (native != ERROR_FILE_NOT_FOUND && native != ERROR_PATH_NOT_FOUND) {
                return failure(RunSaveDurabilityOperation::RemoveFile,
                               RunSaveDurabilityError::RemoveFailed, native);
            }
        }
        return success(RunSaveDurabilityOperation::RemoveFile);
#else
        if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
            return failure(RunSaveDurabilityOperation::RemoveFile,
                           RunSaveDurabilityError::RemoveFailed, errno);
        }
        auto directory = path.parent_path();
        if (directory.empty()) directory = ".";
        return syncDirectory(directory);
#endif
    }

    RunSaveDurabilityResult syncDirectory(
        const std::filesystem::path& path) noexcept override {
#if defined(_WIN32)
        // MoveFileExW(..., MOVEFILE_WRITE_THROUGH) supplies the platform's
        // durable rename primitive. Windows does not provide a portable
        // directory FlushFileBuffers equivalent.
        (void)path;
        return success(RunSaveDurabilityOperation::SyncDirectory);
#else
        int flags = O_RDONLY;
#ifdef O_DIRECTORY
        flags |= O_DIRECTORY;
#endif
        const int descriptor = ::open(path.c_str(), flags);
        if (descriptor < 0) {
            return failure(RunSaveDurabilityOperation::SyncDirectory,
                           RunSaveDurabilityError::DirectoryOpenFailed, errno);
        }
        const bool synced = ::fsync(descriptor) == 0;
        const int native = synced ? 0 : errno;
        ::close(descriptor);
        return synced
            ? success(RunSaveDurabilityOperation::SyncDirectory)
            : failure(RunSaveDurabilityOperation::SyncDirectory,
                      RunSaveDurabilityError::DirectorySyncFailed, native);
#endif
    }

private:
    static RunSaveDurabilityResult success(
        RunSaveDurabilityOperation operation) noexcept {
        return {operation, RunSaveDurabilityError::None, 0};
    }

    template<class Code>
    static RunSaveDurabilityResult failure(
        RunSaveDurabilityOperation operation,
        RunSaveDurabilityError error,
        Code nativeCode) noexcept {
        return {operation, error, static_cast<std::int64_t>(nativeCode)};
    }
};

inline IRunSaveDurability& DefaultRunSaveDurability() noexcept {
    static PlatformRunSaveDurability durability;
    return durability;
}

} // namespace rts::roguelite
