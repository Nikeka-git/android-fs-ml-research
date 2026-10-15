#ifndef NATIVE_BINDER_DIAGNOSTICS_H
#define NATIVE_BINDER_DIAGNOSTICS_H

#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

// The historical filename is kept for source compatibility. This code performs
// connection diagnostics only. It does not obtain kernel addresses or privileges.
namespace diagnostics {

struct Identity {
    std::uint32_t pid = 0;
    std::uint32_t uid = 0;
    std::uint32_t euid = 0;
};

struct OpenResult {
    int fd = -1;
    int error = 0;
};

struct VersionResult {
    bool success = false;
    int version = 0;
    int error = 0;
};

// Syscall results carry errno captured at the call site. Tests supply a backend
// without opening any device. Backend lifetime must exceed the owning model's.
class Backend {
public:
    virtual ~Backend() = default;
    virtual OpenResult openDevice(const char* path) noexcept = 0;
    virtual VersionResult probeVersion(int fd) noexcept = 0;
    virtual void closeDevice(int fd) noexcept = 0;
    virtual Identity identity() const noexcept = 0;
    virtual int expectedProtocolVersion() const noexcept = 0;
};

class Descriptor {
public:
    explicit Descriptor(Backend& backend, int fd = -1) noexcept
        : backend_(backend), fd_(fd) {}
    ~Descriptor() { reset(); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;

    int get() const noexcept { return fd_; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) backend_.closeDevice(fd_);
        fd_ = fd;
    }

private:
    Backend& backend_;
    int fd_;
};

struct Error {
    int code = 0;
    std::string message;
};

struct DeviceState {
    bool deviceOpen = false;
    std::string devicePath;
    bool protocolAvailable = false;
    int protocolVersion = 0;
    int expectedProtocolVersion = 0;
    Identity identity;
    std::uint64_t probeCount = 0;
    std::uint64_t successfulProbeCount = 0;
    bool lastProbeSucceeded = false;
    Error lastError;
};

class DeviceModel {
public:
    explicit DeviceModel(Backend& backend) : backend_(backend), descriptor_(backend) {}

    // Reinitializing a connected model probes the same descriptor. New attempts
    // fall back to hwbinder if binder cannot be opened or its protocol is unusable.
    bool initialize() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (descriptor_.get() >= 0) {
            if (probeLocked(descriptor_.get())) return true;
            descriptor_.reset();
            state_.devicePath.clear();
            return false;
        }

        constexpr const char* paths[] = {"/dev/binder", "/dev/hwbinder"};
        for (const char* path : paths) {
            std::string candidatePath(path);
            state_.protocolAvailable = false;
            state_.protocolVersion = 0;
            state_.lastProbeSucceeded = false;
            const OpenResult opened = backend_.openDevice(path);
            if (opened.fd < 0) {
                setError(opened.error != 0 ? opened.error : EIO, "open " + candidatePath);
                continue;
            }
            Descriptor pending(backend_, opened.fd);
            if (!probeLocked(pending.get())) continue;
            state_.devicePath = std::move(candidatePath);
            descriptor_.reset(pending.release());
            return true;
        }
        state_.devicePath.clear();
        return false;
    }

    bool probe() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (descriptor_.get() < 0) {
            // Keep a failed initialization's more useful open/protocol error.
            if (state_.lastError.code == 0) setError(EBADF, "Binder device is not open");
            return false;
        }
        return probeLocked(descriptor_.get());
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        descriptor_.reset();
        state_ = DeviceState{};
    }

    // A diagnostic reset does not pretend to reset the Binder driver. The live
    // connection, latest protocol observation and actual process identity remain.
    void resetCounters() {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.probeCount = 0;
        state_.successfulProbeCount = 0;
    }

    void unsupported(const char* operation) {
        std::lock_guard<std::mutex> lock(mutex_);
        setError(ENOTSUP, std::string(operation) +
                 " is unavailable in device diagnostics; no kernel addresses or privilege operations are provided");
    }

    DeviceState snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        DeviceState result = state_;
        result.deviceOpen = descriptor_.get() >= 0;
        result.expectedProtocolVersion = backend_.expectedProtocolVersion();
        result.identity = backend_.identity();
        return result;
    }

private:
    void setError(int code, const std::string& operation) {
        Error error{code, operation + ": " + std::generic_category().message(code)};
        state_.lastError = std::move(error);
    }

    bool probeLocked(int fd) {
        state_.lastProbeSucceeded = false;
        if (state_.probeCount == std::numeric_limits<std::uint64_t>::max()) {
            setError(EOVERFLOW, "Binder diagnostic probe counter exhausted");
            return false;
        }
        ++state_.probeCount;
        const VersionResult result = backend_.probeVersion(fd);
        state_.protocolAvailable = result.success;
        state_.protocolVersion = result.success ? result.version : 0;
        if (!result.success) {
            setError(result.error != 0 ? result.error : EIO, "BINDER_VERSION");
            return false;
        }
        if (result.version != backend_.expectedProtocolVersion()) {
            setError(EPROTO, "Binder protocol " + std::to_string(result.version) +
                     " does not match expected " + std::to_string(backend_.expectedProtocolVersion()));
            return false;
        }
        ++state_.successfulProbeCount;
        state_.lastProbeSucceeded = true;
        state_.lastError = Error{};
        return true;
    }

    Backend& backend_;
    Descriptor descriptor_;
    mutable std::mutex mutex_;
    DeviceState state_;
};

}  // namespace diagnostics

#endif
