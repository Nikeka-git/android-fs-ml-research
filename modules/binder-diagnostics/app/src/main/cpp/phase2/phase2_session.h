#ifndef PHASE2_SESSION_H
#define PHASE2_SESSION_H

#include <cstdint>
#include <mutex>
#include <string>
#include <system_error>

#include "privilege_manager.h"
#include "file_access.h"
#include "binder_exploit.h"
#include "json_utils.h"

namespace phase2 {

struct Phase2State {
    bool privilegeElevated = false;
    bool rootAccess = false;
    bool fileAccessEnabled = false;
    bool cveExploitationActive = false;
    std::string devicePath;
    std::uint64_t totalTransactions = 0;
    int lastError = 0;
    std::string lastMessage;
    bool phase2Active = false;
    bool deviceInitialized = false;
};

// A C++ utility session, independent of the APK's existing JNI session.
// File helpers use ordinary OS permissions. initialize() returns the optional
// Binder connection result; file access remains available when Binder fails.
// No operation exploits a vulnerability or changes process credentials.
class Phase2Session {
public:
    Phase2Session() = default;

    // Injected backends must outlive this session.
    Phase2Session(diagnostics::Backend& binderBackend, PrivilegeBackend& privilegeBackend)
        : privilegeManager_(privilegeBackend), exploitManager_(binderBackend) {}

    bool initialize() {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.phase2Active = true;
        const bool connected = exploitManager_.initialize();
        adoptBinderResult();
        return connected;
    }

    // Unsupported even for an already-root caller. rootAccess separately
    // reports the actual identity; it is never a claim of privilege elevation.
    bool elevateToRoot() {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.phase2Active = true;
        const bool elevated = privilegeManager_.elevateToRoot();
        const PrivilegeState privilege = privilegeManager_.currentState();
        state_.lastError = privilege.lastErrorCode;
        state_.lastMessage = privilege.lastErrorMessage;
        return elevated;
    }

    // Returned handles belong to the caller; close() cannot invalidate them.
    FileHandle openFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.phase2Active = true;
        FileHandle handle = fileAccess_.openFileForRead(path);
        state_.lastError = handle.lastError();
        state_.lastMessage = handle.isOpen() ? "" : handle.errorMessage();
        return handle;
    }

    DirectoryListing listDirectory(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.phase2Active = true;
        DirectoryListing listing = fileAccess_.listDirectory(path);
        state_.lastError = listing.lastError;
        state_.lastMessage = listing.errorMessage;
        return listing;
    }

    FileDescriptorInfo fileInfo(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.phase2Active = true;
        FileDescriptorInfo info = fileAccess_.fileInfo(path);
        state_.lastError = info.lastError;
        state_.lastMessage = info.lastErrorMessage;
        return info;
    }

    bool executeCVEExploitation() {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.phase2Active = true;
        const bool result = exploitManager_.executeCVEExploitation();
        adoptBinderResult();
        return result;
    }

    bool monitorDevice() {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.phase2Active = true;
        const bool result = exploitManager_.monitorDeviceState();
        adoptBinderResult();
        return result;
    }

    bool probeDevice() {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.phase2Active = true;
        const bool result = exploitManager_.probeVersion();
        adoptBinderResult();
        return result;
    }

    Phase2State currentState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot(exploitManager_.currentState(), privilegeManager_.currentState());
    }

    std::string getDetailedReport() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const ExploitState binder = exploitManager_.currentState();
        const PrivilegeState privilege = privilegeManager_.currentState();
        const Phase2State state = snapshot(binder, privilege);
        return "{\"phase2Active\":" + boolean(state.phase2Active) +
            ",\"privilegeElevated\":" + boolean(state.privilegeElevated) +
            ",\"rootAccess\":" + boolean(state.rootAccess) +
            ",\"rootElevationSupported\":false,\"fileAccessEnabled\":" + boolean(state.fileAccessEnabled) +
            ",\"fileAccessScope\":\"current_process_permissions\",\"cveExploitationActive\":false," +
            "\"exploitationSupported\":false,\"deviceInitialized\":" + boolean(state.deviceInitialized) +
            ",\"devicePath\":" + detail::jsonString(state.devicePath) +
            ",\"totalTransactions\":" + std::to_string(state.totalTransactions) +
            ",\"privileges\":{\"identityAvailable\":" + boolean(privilege.identityAvailable) +
            ",\"uid\":" + identityValue(privilege.originalUid, privilege.identityAvailable) +
            ",\"gid\":" + identityValue(privilege.originalGid, privilege.identityAvailable) +
            ",\"euid\":" + identityValue(privilege.effectiveUid, privilege.identityAvailable) +
            ",\"egid\":" + identityValue(privilege.effectiveGid, privilege.identityAvailable) +
            ",\"alreadyRoot\":" + boolean(privilege.alreadyRoot) +
            ",\"capabilitiesAvailable\":" + boolean(privilege.capabilitiesAvailable) +
            ",\"effectiveCapabilities\":" + capabilitiesJson(privilege) +
            ",\"inspectionError\":{\"code\":" + std::to_string(privilege.inspectionErrorCode) +
            ",\"message\":" + detail::jsonString(privilege.inspectionErrorMessage) + "}}," +
            "\"binderExploit\":" + exploitManager_.getDetailedReport() +
            ",\"lastError\":{\"code\":" + std::to_string(state.lastError) +
            ",\"message\":" + detail::jsonString(state.lastMessage) + "}}";
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        exploitManager_.close();
        privilegeManager_.reset();
        state_ = Phase2State{};
    }

private:
    static std::string boolean(bool value) { return value ? "true" : "false"; }

    static std::string identityValue(std::uint32_t value, bool available) {
        return available ? std::to_string(value) : "null";
    }

    static std::string capabilitiesJson(const PrivilegeState& privilege) {
        if (!privilege.capabilitiesAvailable) return "null";
        std::string result = "[";
        bool first = true;
        for (const int capability : privilege.effectiveCapabilities) {
            if (!first) result += ',';
            result += std::to_string(capability);
            first = false;
        }
        return result + ']';
    }

    Phase2State snapshot(const ExploitState& binder, const PrivilegeState& privilege) const {
        Phase2State state = state_;
        state.privilegeElevated = false;
        state.rootAccess = privilege.identityAvailable && privilege.alreadyRoot;
        state.fileAccessEnabled = state.phase2Active;
        state.cveExploitationActive = false;
        state.deviceInitialized = binder.deviceInitialized;
        state.devicePath = binder.devicePath;
        // A cumulative counter is copied, never added repeatedly as a delta.
        state.totalTransactions = binder.transactionCount;
        return state;
    }

    void adoptBinderResult() {
        const ExploitState binder = exploitManager_.currentState();
        state_.lastError = binder.lastError;
        state_.lastMessage = binder.lastErrorMessage;
    }

    mutable std::mutex mutex_;
    PrivilegeManager privilegeManager_;
    FileAccessManager fileAccess_;
    BinderExploitManager exploitManager_;
    Phase2State state_{};
};

}  // namespace phase2

#endif

