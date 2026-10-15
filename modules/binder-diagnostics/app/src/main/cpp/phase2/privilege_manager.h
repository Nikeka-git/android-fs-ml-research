#ifndef PHASE2_PRIVILEGE_MANAGER_H
#define PHASE2_PRIVILEGE_MANAGER_H

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef __ANDROID__
#include <sys/capability.h>
#include <unistd.h>
#elif defined(__linux__)
#include <linux/capability.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace phase2 {

inline constexpr std::uint32_t kUnknownCredential = std::numeric_limits<std::uint32_t>::max();

struct PrivilegeObservation {
    bool credentialsAvailable = false;
    std::uint32_t realUid = kUnknownCredential;
    std::uint32_t realGid = kUnknownCredential;
    std::uint32_t effectiveUid = kUnknownCredential;
    std::uint32_t effectiveGid = kUnknownCredential;
    bool capabilitiesAvailable = false;
    std::uint64_t effectiveCapabilityMask = 0;
    int credentialsError = ENOTSUP;
    int capabilitiesError = ENOTSUP;
};

// A backend observes the calling thread only. It has no operation for changing
// identities, capabilities, SELinux policies, or other process credentials.
class PrivilegeBackend {
public:
    virtual ~PrivilegeBackend() = default;
    virtual PrivilegeObservation observe() const noexcept = 0;
};

class PlatformPrivilegeBackend final : public PrivilegeBackend {
public:
    PrivilegeObservation observe() const noexcept override {
        PrivilegeObservation result;
#if defined(__ANDROID__) || defined(__linux__)
        result.credentialsAvailable = true;
        result.realUid = static_cast<std::uint32_t>(::getuid());
        result.realGid = static_cast<std::uint32_t>(::getgid());
        result.effectiveUid = static_cast<std::uint32_t>(::geteuid());
        result.effectiveGid = static_cast<std::uint32_t>(::getegid());
        result.credentialsError = 0;
        struct __user_cap_header_struct header{};
        header.version = _LINUX_CAPABILITY_VERSION_3;
        header.pid = 0;  // capget with pid=0 observes this calling thread.
        struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3]{};
        int status;
        do {
#ifdef __ANDROID__
            status = ::capget(&header, data);
#else
            status = static_cast<int>(::syscall(SYS_capget, &header, data));
#endif
        } while (status < 0 && errno == EINTR);
        if (status == 0) {
            result.capabilitiesAvailable = true;
            result.effectiveCapabilityMask = static_cast<std::uint64_t>(data[0].effective) |
                (static_cast<std::uint64_t>(data[1].effective) << 32);
            result.capabilitiesError = 0;
        } else {
            result.capabilitiesError = errno != 0 ? errno : EIO;
        }
#endif
        return result;
    }
};

struct PrivilegeState {
    // Historical original* names now mean observed real IDs, not cached startup
    // credentials. Unavailable IDs use a sentinel instead of falsely reporting 0.
    std::uint32_t originalUid = kUnknownCredential;
    std::uint32_t originalGid = kUnknownCredential;
    std::uint32_t effectiveUid = kUnknownCredential;
    std::uint32_t effectiveGid = kUnknownCredential;
    bool rootElevated = false;
    bool capabilitiesPreserved = false;
    // No capabilities are acquired; this compatibility field stays empty.
    std::vector<int> acquiredCapabilities;
    int lastErrorCode = 0;
    std::string lastErrorMessage;
    bool credentialsAvailable = false;
    bool capabilitiesAvailable = false;
    bool identityAvailable = false;
    bool alreadyRoot = false;
    std::vector<int> effectiveCapabilities;
    int inspectionErrorCode = 0;
    std::string inspectionErrorMessage;
};

class PrivilegeManager {
public:
    PrivilegeManager() : backend_(defaultBackend_) { refreshLocked(false); }
    // The injected backend must outlive this manager.
    explicit PrivilegeManager(PrivilegeBackend& backend) : backend_(backend) { refreshLocked(false); }
    PrivilegeManager(const PrivilegeManager&) = delete;
    PrivilegeManager& operator=(const PrivilegeManager&) = delete;

    bool elevateToRoot() {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshLocked(false);
        setOperationError(ENOTSUP, "Privilege elevation is unsupported; current credentials were only inspected");
        return false;
    }

    // Compatibility method: refresh the observation, then report that capability
    // acquisition is unsupported. It never calls capset, prctl or setuid/setgid.
    void acquireRootCapabilities() {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshLocked(false);
        setOperationError(ENOTSUP, "Capability acquisition is unsupported; effective capabilities were only inspected");
    }

    bool hasCapability(int capability) {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshLocked(false);
        if (capability < 0 || capability >= 64) {
            setOperationError(EINVAL, "Capability index must be between 0 and 63");
            return false;
        }
        if (!state_.capabilitiesAvailable) return false;
        return std::find(state_.effectiveCapabilities.begin(), state_.effectiveCapabilities.end(), capability) !=
               state_.effectiveCapabilities.end();
    }

    // Every snapshot observes actual current identities and current thread caps.
    // A previous unsupported-operation error is retained alongside fresh read errors.
    PrivilegeState currentState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshLocked(true);
        return state_;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshLocked(false);
    }

private:
    static std::string errnoMessage(int code, const char* operation) {
        return std::string(operation) + ": " + std::generic_category().message(code);
    }

    void setOperationError(int code, const char* message) {
        std::string text(message);
        state_.lastErrorCode = code;
        state_.lastErrorMessage = std::move(text);
        operationError_ = true;
    }

    void refreshLocked(bool keepOperationError) const {
        const PrivilegeObservation observation = backend_.observe();
        PrivilegeState fresh;
        fresh.credentialsAvailable = observation.credentialsAvailable;
        fresh.identityAvailable = observation.credentialsAvailable;
        fresh.capabilitiesAvailable = observation.capabilitiesAvailable;
        if (observation.credentialsAvailable) {
            fresh.originalUid = observation.realUid;
            fresh.originalGid = observation.realGid;
            fresh.effectiveUid = observation.effectiveUid;
            fresh.effectiveGid = observation.effectiveGid;
            fresh.alreadyRoot = observation.effectiveUid == 0;
        }
        if (observation.capabilitiesAvailable) {
            for (int bit = 0; bit < 64; ++bit) {
                if ((observation.effectiveCapabilityMask & (UINT64_C(1) << bit)) != 0) {
                    fresh.effectiveCapabilities.push_back(bit);
                }
            }
        }
        if (!observation.credentialsAvailable) {
            fresh.inspectionErrorCode = observation.credentialsError != 0 ? observation.credentialsError : EIO;
            fresh.inspectionErrorMessage = errnoMessage(fresh.inspectionErrorCode, "Credential inspection");
        } else if (!observation.capabilitiesAvailable) {
            fresh.inspectionErrorCode = observation.capabilitiesError != 0 ? observation.capabilitiesError : EIO;
            fresh.inspectionErrorMessage = errnoMessage(fresh.inspectionErrorCode, "capget");
        }
        if (keepOperationError && operationError_) {
            fresh.lastErrorCode = state_.lastErrorCode;
            fresh.lastErrorMessage = state_.lastErrorMessage;
        } else {
            fresh.lastErrorCode = fresh.inspectionErrorCode;
            fresh.lastErrorMessage = fresh.inspectionErrorMessage;
            operationError_ = false;
        }
        state_ = std::move(fresh);
    }

    PlatformPrivilegeBackend defaultBackend_;
    PrivilegeBackend& backend_;
    mutable std::mutex mutex_;
    mutable PrivilegeState state_;
    mutable bool operationError_ = false;
};

}  // namespace phase2

#endif
