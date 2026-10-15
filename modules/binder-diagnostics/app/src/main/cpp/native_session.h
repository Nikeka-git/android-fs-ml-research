#ifndef NATIVE_DIAGNOSTIC_SESSION_H
#define NATIVE_DIAGNOSTIC_SESSION_H

#include "real_binder_model.h"
#include "simulation_model.h"

#include <cinttypes>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace diagnostics {

inline std::string jsonString(const std::string& value) {
    std::string result = "\"";
    for (const unsigned char character : value) {
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20) {
                    char escape[7];
                    std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned int>(character));
                    result += escape;
                } else {
                    result += static_cast<char>(character);
                }
        }
    }
    result += '"';
    return result;
}

inline std::string deviceJsonFields(const DeviceState& state) {
    return "\"deviceOpen\":" + std::string(state.deviceOpen ? "true" : "false") +
           ",\"devicePath\":" + jsonString(state.devicePath) +
           ",\"protocolVersion\":" + (state.protocolAvailable ? std::to_string(state.protocolVersion) : "null") +
           ",\"expectedProtocolVersion\":" + std::to_string(state.expectedProtocolVersion) +
           ",\"pid\":" + std::to_string(state.identity.pid) +
           ",\"uid\":" + std::to_string(state.identity.uid) +
           ",\"euid\":" + std::to_string(state.identity.euid) +
           ",\"probeCount\":" + std::to_string(state.probeCount) +
           ",\"successfulProbeCount\":" + std::to_string(state.successfulProbeCount) +
           ",\"lastProbeSucceeded\":" + (state.lastProbeSucceeded ? "true" : "false") +
           ",\"lastError\":{\"code\":" + std::to_string(state.lastError.code) +
           ",\"message\":" + jsonString(state.lastError.message) + "}" +
           ",\"kernelAddressAvailable\":false,\"rootElevationSupported\":false";
}

// One native monitor makes each complete JNI call, including mode changes and
// JSON snapshots, atomic. Kotlin additionally holds its session monitor across
// workflows containing multiple calls. Neither model accesses kernel memory.
class Session {
public:
    explicit Session(Backend& backend) : device_(backend) {}

    std::string status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deviceMode_
            ? "Binder device diagnostics active; BINDER_VERSION only; kernel addresses and root elevation unavailable"
            : "Local userspace simulation active; optional Binder device diagnostics; no kernel memory or privilege changes";
    }

    bool initializeDevice() {
        std::lock_guard<std::mutex> lock(mutex_);
        // Reset the previous mode before attempting initialization, so any error
        // or allocation exception leaves a clean simulation and an owned fd.
        deviceMode_ = false;
        simulation_.reset();
        try {
            deviceMode_ = device_.initialize();
        } catch (...) {
            // For example, a diagnostic string allocation may fail after an
            // existing connection was probed. Simulation mode must own no fd.
            device_.close();
            throw;
        }
        return deviceMode_;
    }

    bool isDeviceMode() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deviceMode_;
    }

    std::uint64_t procInfo() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deviceMode_ ? 0 : simulation::kProcAddress;
    }

    bool execute(std::int64_t target) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deviceMode_) {
            device_.unsupported("executeBadSpin");
            return false;
        }
        return target > 0 && simulation_.execute(static_cast<std::uint64_t>(target));
    }

    bool checkSimulationHealth() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deviceMode_) {
            device_.unsupported("checkSystemHealth (use probeDevice for device diagnostics)");
            return false;
        }
        return simulation_.checkHealth();
    }

    bool probeDevice() {
        std::lock_guard<std::mutex> lock(mutex_);
        return device_.probe();
    }

    bool elevateToRoot() {
        std::lock_guard<std::mutex> lock(mutex_);
        device_.unsupported("elevateToRoot");
        return false;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deviceMode_) device_.resetCounters(); else simulation_.reset();
    }

    void closeDevice() {
        std::lock_guard<std::mutex> lock(mutex_);
        device_.close();
        deviceMode_ = false;
        simulation_.reset();
    }

    std::string detailedState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string diagnosticFields = deviceJsonFields(device_.snapshot());
        if (deviceMode_) {
            return "{\"mode\":\"device_diagnostics\",\"simulation\":false," + diagnosticFields + "}";
        }
        const simulation::State state = simulation_.snapshot();
        char buffer[512];
        const int length = std::snprintf(
            buffer, sizeof(buffer),
            "{\"mode\":\"simulation\",\"procAddress\":\"0x%016" PRIX64 "\","
            "\"innerLockState\":\"0x%016" PRIX64 "\",\"basePtr\":\"0x%016" PRIX64 "\","
            "\"transactionCount\":%" PRIu64 ",\"flags\":\"0x%016" PRIX64 "\","
            "\"referenceCount\":%" PRId32 ",\"simulation\":true,\"expectedOffset\":%" PRIu64 ","
            "\"searchRange\":%" PRIu64 ",\"addressAlignment\":%" PRIu64 ",\"deviceDiagnostics\":{",
            simulation::kProcAddress, state.innerLockState, state.basePtr,
            state.transactionCount, state.flags, state.referenceCount,
            simulation::kExpectedOffset, simulation::kSearchRange, simulation::kAddressAlignment);
        if (length < 0 || static_cast<std::size_t>(length) >= sizeof(buffer)) {
            throw std::runtime_error("Could not serialize simulation state");
        }
        return std::string(buffer) + diagnosticFields + "}}";
    }

private:
    mutable std::mutex mutex_;
    DeviceModel device_;
    simulation::Model simulation_;
    bool deviceMode_ = false;
};

}  // namespace diagnostics

#endif
