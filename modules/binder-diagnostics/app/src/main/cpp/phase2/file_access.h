#ifndef PHASE2_FILE_ACCESS_H
#define PHASE2_FILE_ACCESS_H

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

// Ordinary POSIX file I/O under the caller's existing permissions and security policy.
namespace phase2 {

/**
 * Owns exactly one descriptor. Copying is forbidden; moving transfers ownership.
 * Shared use of a handle and its descriptor offset requires caller synchronization.
 */
class FileHandle {
public:
    FileHandle() noexcept = default;
    explicit FileHandle(int fd) noexcept : fd_(fd >= 0 ? fd : -1) {}

    // Ownership is accepted before copying path, so allocation failure cannot leak fd.
    FileHandle(int fd, const std::string& path, int error = 0)
        : fd_(fd >= 0 ? fd : -1), lastError_(error) {
        try {
            path_ = path;
        } catch (...) {
            closeQuietly();
            throw;
        }
    }

    ~FileHandle() noexcept { closeQuietly(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept
        : fd_(other.release()), path_(std::move(other.path_)), lastError_(other.lastError_) {
        other.lastError_ = 0;
    }
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            closeQuietly();
            fd_ = other.release();
            path_ = std::move(other.path_);
            lastError_ = other.lastError_;
            other.lastError_ = 0;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool isOpen() const noexcept { return fd_ >= 0; }
    const std::string& getPath() const noexcept { return path_; }
    int lastError() const noexcept { return lastError_; }
    std::string errorMessage() const {
        if (lastError_ == 0) return {};
        return path_ + ": " + std::strerror(lastError_);
    }
    int release() noexcept { return std::exchange(fd_, -1); }

    bool close() noexcept {
        if (!isOpen()) return true;
        const int fd = release();
        // On Linux/Android close releases the descriptor even on EINTR; never retry it.
        if (::close(fd) < 0) {
            lastError_ = errno;
            return false;
        }
        lastError_ = 0;
        return true;
    }

private:
    friend class FileAccessManager;
    void setError(int error) const noexcept { lastError_ = error; }
    void closeQuietly() noexcept {
        const int savedError = errno;
        if (isOpen()) ::close(release());
        errno = savedError;
    }
    int fd_ = -1;
    std::string path_;
    mutable int lastError_ = 0;
};

struct FileDescriptorInfo {
    int fd = -1;
    std::string path;
    // fileInfo performs lstat, not open: fd stays -1 and isOpen stays false.
    bool isOpen = false;
    std::uint64_t size = 0;
    mode_t permissions = 0;
    uid_t ownerId = 0;
    gid_t groupId = 0;
    int lastError = 0;
    std::string lastErrorMessage;
    bool metadataAvailable = false;
};

struct DirectoryListing {
    std::string directoryPath;
    std::vector<std::string> entries;
    std::vector<std::string> files;
    std::vector<std::string> directories;
    // Sum of regular-file logical sizes; directories and symlinks are not file contents.
    std::uint64_t totalSize = 0;
    int errorCount = 0;
    std::string errorMessage;
    int lastError = 0;
};

class FileAccessManager {
public:
    static constexpr std::size_t kMaximumReadSize = 10 * 1024 * 1024;

    FileHandle openFileForRead(const std::string& path) {
        if (hasNul(path)) return failedHandle(path, EINVAL);
        int fd;
        do { fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
        const int error = fd < 0 ? errno : 0;
        FileHandle result(fd, path, error);
        if (error != 0) errno = error;
        return result;
    }

    /** Creates if missing, respects umask, and writes from offset zero without truncating. */
    FileHandle openFileForWrite(const std::string& path, mode_t permissions = 0644) {
        if (hasNul(path) || (permissions & ~static_cast<mode_t>(07777)) != 0) {
            return failedHandle(path, EINVAL);
        }
        int fd;
        do { fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, permissions); }
        while (fd < 0 && errno == EINTR);
        const int error = fd < 0 ? errno : 0;
        FileHandle result(fd, path, error);
        if (error != 0) errno = error;
        return result;
    }

    /**
     * Reads a prefix of at most maxSize bytes (0..10 MiB), never an extra buffer.
     * Rewinds seekable files; consumes non-seekable streams from their current position.
     * EOF/empty contents return an empty string; failures throw system_error, even after
     * a partial read. A limit-sized prefix does not assert that EOF has been reached.
     */
    std::string readFileContent(const FileHandle& handle,
                                std::size_t maxSize = kMaximumReadSize) {
        if (maxSize > kMaximumReadSize) throwIo(handle, EINVAL, "read limit");
        if (!handle.isOpen()) throwIo(handle, handle.lastError() ? handle.lastError() : EBADF, "read");
        handle.setError(0);
        if (maxSize == 0) return {};
        off_t position;
        do { position = ::lseek(handle.get(), 0, SEEK_SET); } while (position < 0 && errno == EINTR);
        if (position < 0 && errno != ESPIPE) throwIo(handle, errno, "seek");

        std::string content;
        char buffer[4096];
        while (content.size() < maxSize) {
            const std::size_t requested = std::min(sizeof(buffer), maxSize - content.size());
            ssize_t count;
            do { count = ::read(handle.get(), buffer, requested); } while (count < 0 && errno == EINTR);
            if (count < 0) throwIo(handle, errno, "read");
            if (count == 0) break;
            content.append(buffer, static_cast<std::size_t>(count));
        }
        return content;
    }

    /** Writes at the current descriptor offset, without truncation or rollback on failure. */
    bool writeFileContent(const FileHandle& handle, const std::string& content) {
        if (!handle.isOpen()) return failWrite(handle, handle.lastError() ? handle.lastError() : EBADF);
        handle.setError(0);
        std::size_t written = 0;
        while (written < content.size()) {
            const std::size_t requested = std::min(
                content.size() - written,
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t count = ::write(handle.get(), content.data() + written, requested);
            if (count < 0) {
                const int error = errno;
                if (error == EINTR) continue;
                return failWrite(handle, error);
            }
            if (count == 0) return failWrite(handle, EIO);
            written += static_cast<std::size_t>(count);
        }
        return true;
    }

    DirectoryListing listDirectory(const std::string& path) {
        DirectoryListing result;
        result.directoryPath = path;
        if (hasNul(path)) {
            recordDirectoryError(result, EINVAL, path);
            return result;
        }
        DIR* opened;
        do { opened = ::opendir(path.c_str()); } while (!opened && errno == EINTR);
        if (!opened) {
            recordDirectoryError(result, errno, path);
            return result;
        }
        std::unique_ptr<DIR, int (*)(DIR*)> directory(opened, ::closedir);
        for (;;) {
            errno = 0;
            dirent* entry = ::readdir(directory.get());
            if (!entry) {
                if (errno == EINTR) continue;
                if (errno != 0) recordDirectoryError(result, errno, path);
                break;
            }
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            result.entries.push_back(name);
            struct stat info {};
            int statResult;
            do { statResult = ::fstatat(::dirfd(directory.get()), name.c_str(), &info, AT_SYMLINK_NOFOLLOW); }
            while (statResult < 0 && errno == EINTR);
            if (statResult < 0) {
                recordDirectoryError(result, errno, name);
                continue;
            }
            if (S_ISDIR(info.st_mode)) result.directories.push_back(name);
            if (S_ISREG(info.st_mode)) {
                result.files.push_back(name);
                if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) >
                        std::numeric_limits<std::uint64_t>::max() - result.totalSize) {
                    recordDirectoryError(result, EOVERFLOW, name);
                } else {
                    result.totalSize += static_cast<std::uint64_t>(info.st_size);
                }
            }
        }
        if (::closedir(directory.release()) < 0) recordDirectoryError(result, errno, path);
        std::sort(result.entries.begin(), result.entries.end());
        std::sort(result.files.begin(), result.files.end());
        std::sort(result.directories.begin(), result.directories.end());
        return result;
    }

    FileDescriptorInfo fileInfo(const std::string& path) {
        FileDescriptorInfo info;
        info.path = path;
        struct stat metadata {};
        int status = -1;
        if (hasNul(path)) {
            errno = EINVAL;
        } else {
            do { status = ::lstat(path.c_str(), &metadata); } while (status < 0 && errno == EINTR);
        }
        if (status < 0) {
            info.lastError = errno;
            info.lastErrorMessage = std::strerror(info.lastError);
        } else if (metadata.st_size < 0) {
            info.lastError = EOVERFLOW;
            info.lastErrorMessage = std::strerror(info.lastError);
        } else {
            info.metadataAvailable = true;
            info.size = static_cast<std::uint64_t>(metadata.st_size);
            info.permissions = metadata.st_mode;
            info.ownerId = metadata.st_uid;
            info.groupId = metadata.st_gid;
        }
        return info;
    }

    bool chmodFile(const std::string& path, mode_t permissions) {
        if (hasNul(path) || (permissions & ~static_cast<mode_t>(07777)) != 0) return failPath();
        int result;
        do { result = ::chmod(path.c_str(), permissions); } while (result < 0 && errno == EINTR);
        return result == 0;
    }

    bool chownFile(const std::string& path, uid_t ownerId, gid_t groupId) {
        if (hasNul(path)) return failPath();
        int result;
        do { result = ::chown(path.c_str(), ownerId, groupId); } while (result < 0 && errno == EINTR);
        return result == 0;
    }

    bool createSymlink(const std::string& target, const std::string& linkPath) {
        if (hasNul(target) || hasNul(linkPath)) return failPath();
        int result;
        do { result = ::symlink(target.c_str(), linkPath.c_str()); } while (result < 0 && errno == EINTR);
        return result == 0;
    }

    /** Returns the complete link target or throws, rather than silently truncating it. */
    std::string readSymlink(const std::string& linkPath) {
        if (hasNul(linkPath)) throw std::system_error(EINVAL, std::generic_category(), "readlink path");
        std::vector<char> buffer(256);
        for (;;) {
            ssize_t count;
            do { count = ::readlink(linkPath.c_str(), buffer.data(), buffer.size()); }
            while (count < 0 && errno == EINTR);
            if (count < 0) {
                const int error = errno;
                throw std::system_error(error, std::generic_category(), "readlink " + linkPath);
            }
            if (static_cast<std::size_t>(count) < buffer.size()) {
                return std::string(buffer.data(), static_cast<std::size_t>(count));
            }
            if (buffer.size() > kMaximumReadSize) {
                throw std::system_error(EFBIG, std::generic_category(), "readlink target exceeds limit");
            }
            buffer.resize(std::min(buffer.size() * 2, kMaximumReadSize + 1));
        }
    }

    std::string readProcInfo(const std::string& processId) {
        if (!validProcessId(processId)) {
            throw std::system_error(EINVAL, std::generic_category(), "invalid process id");
        }
        const auto handle = openFileForRead("/proc/" + processId + "/status");
        return readFileContent(handle);
    }

    /** Respects ordinary access denial; this does not change SELinux policy or permissions. */
    std::string readSELinuxPolicy() {
        const auto handle = openFileForRead("/sys/fs/selinux/policy");
        return readFileContent(handle);
    }

private:
    static bool hasNul(const std::string& path) noexcept { return path.find('\0') != std::string::npos; }
    static FileHandle failedHandle(const std::string& path, int error) {
        FileHandle result(-1, path, error);
        errno = error;
        return result;
    }
    [[noreturn]] static void throwIo(const FileHandle& handle, int error, const char* operation) {
        handle.setError(error);
        throw std::system_error(error, std::generic_category(), std::string(operation) + " " + handle.getPath());
    }
    static bool failWrite(const FileHandle& handle, int error) noexcept {
        handle.setError(error);
        errno = error;
        return false;
    }
    static bool failPath() noexcept { errno = EINVAL; return false; }
    static void recordDirectoryError(DirectoryListing& listing, int error, const std::string& path) {
        if (listing.errorCount < std::numeric_limits<int>::max()) ++listing.errorCount;
        if (listing.lastError == 0) {
            listing.lastError = error;
            listing.errorMessage = path + ": " + std::strerror(error);
        }
    }
    static bool validProcessId(const std::string& id) noexcept {
        if (id == "self" || id == "thread-self") return true;
        std::uint64_t value = 0;
        if (id.empty()) return false;
        for (char c : id) {
            if (c < '0' || c > '9') return false;
            const auto digit = static_cast<std::uint64_t>(c - '0');
            const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max());
            if (value > (maximum - digit) / 10) return false;
            value = value * 10 + digit;
        }
        return value > 0;
    }
};

}  // namespace phase2
#endif  // PHASE2_FILE_ACCESS_H
