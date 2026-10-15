#include "phase2/binder_exploit.h"
#include "phase2/privilege_manager.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class FakeBinderBackend final : public diagnostics::Backend {
public:
    diagnostics::OpenResult openDevice(const char* path) noexcept override {
        ++openCount;
        const bool primary = std::string(path) == "/dev/binder";
        const int error = primary ? binderError : hwbinderError;
        if (error != 0) return {-1, error};
        if (active) ownershipError = true;
        active = true;
        return {9, 0};
    }
    diagnostics::VersionResult probeVersion(int fd) noexcept override {
        ++probeCount;
        if (!active || fd != 9) return {false, 0, EBADF};
        if (probeError != 0) return {false, 0, probeError};
        return {true, protocol, 0};
    }
    void closeDevice(int fd) noexcept override {
        ++closeCount;
        if (!active || fd != 9) ownershipError = true;
        active = false;
    }
    diagnostics::Identity identity() const noexcept override { return {111, 12345, 12345}; }
    int expectedProtocolVersion() const noexcept override { return 8; }
    int binderError = 0;
    int hwbinderError = 0;
    int probeError = 0;
    int protocol = 8;
    int openCount = 0;
    int probeCount = 0;
    int closeCount = 0;
    bool active = false;
    bool ownershipError = false;
};

void checkBinderDiagnosticsAndUnsupportedOperations() {
    FakeBinderBackend backend;
    {
        phase2::BinderExploitManager manager(backend);
        require(!manager.executeCVEExploitation() && manager.currentState().lastError == ENOTSUP,
                "Disconnected CVE call must be explicitly unsupported");
        require(backend.openCount == 0 && backend.probeCount == 0, "Unsupported method accessed a device");
        backend.binderError = EACCES;
        require(manager.initialize(), "Diagnostic hwbinder fallback failed");
        auto state = manager.currentState();
        require(state.deviceInitialized && state.devicePath == "/dev/hwbinder" && state.protocolVersion == 8,
                "Connected diagnostic state is incorrect");
        require(state.binderFd == -1, "Snapshot exposed an owned descriptor");
        require(!state.uafVulnerable && !state.vulnerabilityAssessmentAvailable && !state.exploitationSupported,
                "Protocol version was misrepresented as vulnerability evidence");
        require(manager.initialize() && backend.openCount == 2 && backend.probeCount == 2,
                "Repeated initialize leaked/replaced the live descriptor");
        const int probesBefore = backend.probeCount;
        require(!manager.executeCVEExploitation() && !manager.generateRaceCondition() &&
                !manager.executeBadSpinAttack(111, UINT64_C(0xfffffffffffffff0)),
                "Unsupported attack operation claimed success");
        state = manager.currentState();
        require(state.lastError == ENOTSUP && !state.raceConditionGenerated &&
                state.transactionCount == 0 && state.successfulExploitationCount == 0 &&
                backend.probeCount == probesBefore, "Unsupported operation performed I/O or fabricated counters");
        require(manager.monitorDeviceState() && manager.probeVersion(), "Diagnostic version probe failed");
        require(manager.currentState().lastError == 0, "Successful probe retained old unsupported error");
        const auto json = manager.getDetailedReport();
        require(json.find("\"diagnosticsOnly\":true") != std::string::npos &&
                json.find("\"vulnerabilityAssessmentAvailable\":false") != std::string::npos &&
                json.find("\"transactionCount\":0") != std::string::npos,
                "Binder JSON lost its honest diagnostic contract");
        manager.close();
        manager.close();
        state = manager.currentState();
        require(!state.deviceInitialized && state.devicePath.empty() && state.lastError == 0 &&
                state.probeCount == 0 && !backend.active && backend.closeCount == 1,
                "Close did not clean the local state or descriptor");
        require(manager.initialize(), "Diagnostic reopen failed");
    }
    require(!backend.active && !backend.ownershipError && backend.closeCount == 2,
            "Destructor leaked or double-closed a Binder descriptor");
}

void checkBinderFailuresAndConcurrency() {
    FakeBinderBackend backend;
    phase2::BinderExploitManager manager(backend);
    backend.protocol = 7;
    require(!manager.initialize(), "Protocol mismatch must not initialize");
    auto state = manager.currentState();
    require(state.lastError == EPROTO && !state.deviceInitialized && !state.uafVulnerable &&
            !backend.active && backend.closeCount == 2, "Mismatch fabricated vulnerability or leaked a descriptor");
    backend.protocol = 8;
    backend.probeError = EIO;
    require(!manager.initialize() && manager.currentState().lastError == EIO && !backend.active,
            "Failed version probe was not reported/closed");
    backend.probeError = 0;
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            for (int iteration = 0; iteration < 1000; ++iteration) {
                if (worker == 0) manager.initialize();
                else if (worker == 1) manager.close();
                else if (worker == 2) manager.executeCVEExploitation();
                else manager.probeVersion();
                const auto snapshot = manager.currentState();
                if (snapshot.deviceInitialized != !snapshot.devicePath.empty() || snapshot.uafVulnerable ||
                    snapshot.raceConditionGenerated || snapshot.transactionCount != 0 ||
                    snapshot.successfulExploitationCount != 0 || snapshot.binderFd != -1) failed = true;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    manager.close();
    require(!failed && !backend.active && !backend.ownershipError,
            "Concurrent Binder operations corrupted state or descriptor ownership");
    require(phase2::detail::binderJsonString(std::string("\"\\") + char(1) + '\n') ==
                "\"\\\"\\\\\\u0001\\u000a\"", "Binder report strings are not fully JSON escaped");
}

class FakePrivilegeBackend final : public phase2::PrivilegeBackend {
public:
    phase2::PrivilegeObservation observe() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return observation_;
    }
    void set(const phase2::PrivilegeObservation& observation) {
        std::lock_guard<std::mutex> lock(mutex_);
        observation_ = observation;
    }
private:
    mutable std::mutex mutex_;
    phase2::PrivilegeObservation observation_;
};

phase2::PrivilegeObservation observedIdentity(std::uint32_t uid, std::uint64_t caps) {
    phase2::PrivilegeObservation observation;
    observation.credentialsAvailable = true;
    observation.realUid = uid;
    observation.realGid = uid + 1;
    observation.effectiveUid = uid;
    observation.effectiveGid = uid + 1;
    observation.credentialsError = 0;
    observation.capabilitiesAvailable = true;
    observation.effectiveCapabilityMask = caps;
    observation.capabilitiesError = 0;
    return observation;
}

void checkActualCapabilityBitsAndFreshIdentity() {
    FakePrivilegeBackend backend;
    backend.set(observedIdentity(12345, (UINT64_C(1) << 7) | (UINT64_C(1) << 40) | (UINT64_C(1) << 63)));
    phase2::PrivilegeManager manager(backend);
    require(manager.hasCapability(7) && manager.hasCapability(40) && manager.hasCapability(63) &&
            !manager.hasCapability(0) && !manager.hasCapability(6), "Capability result does not match effective bits");
    require(!manager.hasCapability(-1) && manager.currentState().lastErrorCode == EINVAL &&
            !manager.hasCapability(64), "Invalid capability index was accepted");
    manager.reset();
    auto state = manager.currentState();
    require(state.identityAvailable && state.capabilitiesAvailable && state.originalUid == 12345 &&
            state.originalGid == 12346 && state.effectiveUid == 12345 && state.effectiveGid == 12346 &&
            state.lastErrorCode == 0 && !state.alreadyRoot, "Reset fabricated credentials or kept an old error");
    require(state.effectiveCapabilities == std::vector<int>({7, 40, 63}) && state.acquiredCapabilities.empty() &&
            !state.capabilitiesPreserved && !state.rootElevated, "Capability inspection claimed acquisition/preservation");
    require(!manager.elevateToRoot(), "Privilege inspection claimed elevation");
    state = manager.currentState();
    require(state.lastErrorCode == ENOTSUP && state.effectiveUid == 12345 && !state.rootElevated,
            "Unsupported operation did not preserve actual identity");
    backend.set(observedIdentity(0, 0));
    state = manager.currentState();
    require(state.identityAvailable && state.alreadyRoot && state.effectiveUid == 0 &&
            !state.rootElevated && state.effectiveCapabilities.empty(), "Already-root observation is stale or fabricated");
    require(!manager.elevateToRoot(), "Already-root observation claimed an elevation feature");
    manager.acquireRootCapabilities();
    manager.acquireRootCapabilities();
    state = manager.currentState();
    require(state.lastErrorCode == ENOTSUP && state.acquiredCapabilities.empty() &&
            state.effectiveCapabilities.empty() && !state.capabilitiesPreserved,
            "Repeated acquisition placeholder fabricated capabilities");

    auto unavailable = observedIdentity(23456, UINT64_C(1) << 7);
    unavailable.capabilitiesAvailable = false;
    unavailable.capabilitiesError = EACCES;
    backend.set(unavailable);
    manager.reset();
    state = manager.currentState();
    require(state.identityAvailable && state.effectiveUid == 23456 && !state.capabilitiesAvailable &&
            state.inspectionErrorCode == EACCES && state.effectiveCapabilities.empty() && !manager.hasCapability(7),
            "Failed capget returned stale or invented capabilities");
    backend.set(phase2::PrivilegeObservation{});
    state = manager.currentState();
    require(!state.identityAvailable && state.effectiveUid == phase2::kUnknownCredential && !state.alreadyRoot &&
            state.inspectionErrorCode == ENOTSUP, "Unavailable identity falsely reported UID 0");
}

void checkPrivilegeConcurrencyAndPlatformObservation() {
    FakePrivilegeBackend backend;
    backend.set(observedIdentity(111, UINT64_C(1) << 7));
    phase2::PrivilegeManager manager(backend);
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            for (int iteration = 0; iteration < 1000; ++iteration) {
                if (worker == 0) backend.set(observedIdentity(111 + (iteration % 2), UINT64_C(1) << 7));
                else if (worker == 1) manager.reset();
                else if (worker == 2) manager.elevateToRoot();
                else manager.hasCapability(7);
                const auto snapshot = manager.currentState();
                if (!snapshot.identityAvailable || snapshot.originalUid != snapshot.effectiveUid ||
                    snapshot.originalGid != snapshot.effectiveGid || snapshot.originalGid != snapshot.originalUid + 1 ||
                    snapshot.effectiveCapabilities != std::vector<int>({7}) || snapshot.rootElevated ||
                    !snapshot.acquiredCapabilities.empty()) failed = true;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    require(!failed, "Concurrent privilege observations corrupted state");

    phase2::PrivilegeManager platform;
    auto before = platform.currentState();
#if defined(__ANDROID__) || defined(__linux__)
    require(before.identityAvailable && before.originalUid == static_cast<std::uint32_t>(::getuid()) &&
            before.originalGid == static_cast<std::uint32_t>(::getgid()) &&
            before.effectiveUid == static_cast<std::uint32_t>(::geteuid()) &&
            before.effectiveGid == static_cast<std::uint32_t>(::getegid()), "Platform identity did not match actual syscalls");
    const phase2::PlatformPrivilegeBackend actualBackend;
    const auto actual = actualBackend.observe();
    for (int capability = 0; capability < 64; ++capability) {
        const bool expected = actual.capabilitiesAvailable &&
            (actual.effectiveCapabilityMask & (UINT64_C(1) << capability)) != 0;
        require(platform.hasCapability(capability) == expected, "Platform capability inspection differs from capget");
    }
#else
    require(!before.identityAvailable && before.effectiveUid == phase2::kUnknownCredential && !before.alreadyRoot,
            "Unsupported host platform fabricated identity");
#endif
    require(!platform.elevateToRoot(), "Platform manager attempted to elevate privileges");
    platform.acquireRootCapabilities();
    platform.reset();
    const auto after = platform.currentState();
    require(before.originalUid == after.originalUid && before.originalGid == after.originalGid &&
            before.effectiveUid == after.effectiveUid && before.effectiveGid == after.effectiveGid &&
            before.effectiveCapabilities == after.effectiveCapabilities && !after.rootElevated &&
            after.acquiredCapabilities.empty(), "Read-only privilege API changed observed identity/capabilities");
}

static_assert(!std::is_copy_constructible<phase2::BinderExploitManager>::value, "Binder ownership must not be copyable");
static_assert(!std::is_copy_constructible<phase2::PrivilegeManager>::value, "Privilege state mutex must not be copyable");

}  // namespace

int main() {
    try {
        checkBinderDiagnosticsAndUnsupportedOperations();
        checkBinderFailuresAndConcurrency();
        checkActualCapabilityBitsAndFreshIdentity();
        checkPrivilegeConcurrencyAndPlatformObservation();
        std::cout << "All phase2 security tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
