#ifndef NATIVE_ANDROID_BINDER_BACKEND_H
#define NATIVE_ANDROID_BINDER_BACKEND_H

#include "real_binder_model.h"

#include <fcntl.h>
#include <linux/android/binder.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace diagnostics {

class AndroidBinderBackend final : public Backend {
public:
    OpenResult openDevice(const char* path) noexcept override {
        int fd;
        do {
            fd = ::open(path, O_RDWR | O_CLOEXEC);
        } while (fd < 0 && errno == EINTR);
        return {fd, fd < 0 ? errno : 0};
    }

    VersionResult probeVersion(int fd) noexcept override {
        struct binder_version version{};
        int result;
        do {
            result = ::ioctl(fd, BINDER_VERSION, &version);
        } while (result < 0 && errno == EINTR);
        return {result == 0, version.protocol_version, result < 0 ? errno : 0};
    }

    void closeDevice(int fd) noexcept override {
        // On Linux close releases the descriptor even when reporting EINTR;
        // retrying could close a descriptor that another thread has since reused.
        ::close(fd);
    }

    Identity identity() const noexcept override {
        return {static_cast<std::uint32_t>(::getpid()),
                static_cast<std::uint32_t>(::getuid()),
                static_cast<std::uint32_t>(::geteuid())};
    }

    int expectedProtocolVersion() const noexcept override {
        return BINDER_CURRENT_PROTOCOL_VERSION;
    }
};

}  // namespace diagnostics

#endif
