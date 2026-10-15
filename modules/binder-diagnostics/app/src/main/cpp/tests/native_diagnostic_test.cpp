#include "native_session.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class FakeBackend final : public diagnostics::Backend {
public:
    diagnostics::OpenResult openDevice(const char* path) noexcept override {
        ++openCalls;
        const bool isBinder = std::string(path) == "/dev/binder";
        if ((isBinder && binderOpenError != 0) || (!isBinder && hwbinderOpenError != 0)) {
            return {-1, isBinder ? binderOpenError : hwbinderOpenError};
        }
        const int fd = nextFd++;
        activeDescriptors.insert(fd);
        return {fd, 0};
    }

    diagnostics::VersionResult probeVersion(int fd) noexcept override {
        ++probeCalls;
        if (activeDescriptors.count(fd) == 0) return {false, 0, EBADF};
        if (probeError != 0) return {false, 0, probeError};
        return {true, actualProtocol, 0};
    }

    void closeDevice(int fd) noexcept override {
        ++closeCalls;
        if (activeDescriptors.erase(fd) != 1) invalidClose = true;
    }

    diagnostics::Identity identity() const noexcept override { return currentIdentity; }
    int expectedProtocolVersion() const noexcept override { return expectedProtocol; }

    diagnostics::Identity currentIdentity{101, 10234, 10234};
    int expectedProtocol = 8;
    int actualProtocol = 8;
    int binderOpenError = 0;
    int hwbinderOpenError = 0;
    int probeError = 0;
    int openCalls = 0;
    int closeCalls = 0;
    int probeCalls = 0;
    bool invalidClose = false;
    std::set<int> activeDescriptors;

private:
    int nextFd = 7;
};

bool contains(const std::string& value, const std::string& text) {
    return value.find(text) != std::string::npos;
}

void checkOpenFallbackReopenAndLifetime() {
    FakeBackend backend;
    {
        diagnostics::DeviceModel device(backend);
        auto state = device.snapshot();
        require(!state.deviceOpen && state.identity.uid == 10234 && state.identity.euid == 10234,
                "Initial identity must be real, even when device is closed");
        backend.binderOpenError = EACCES;
        require(device.initialize(), "hwbinder fallback should connect");
        state = device.snapshot();
        require(state.devicePath == "/dev/hwbinder" && state.deviceOpen, "Actual fallback path was lost");
        require(state.protocolVersion == 8 && state.protocolAvailable && state.probeCount == 1 &&
                state.successfulProbeCount == 1 && state.lastError.code == 0, "Initial probe state is incorrect");
        require(backend.openCalls == 2 && backend.activeDescriptors.size() == 1, "Wrong descriptor ownership");
        require(device.initialize(), "Connected initialization should reprobe");
        require(backend.openCalls == 2 && device.snapshot().probeCount == 2, "Repeated initialization opened another fd");
        backend.currentIdentity = {202, 20234, 20235};
        require(device.probe(), "Second explicit probe failed");
        device.resetCounters();
        state = device.snapshot();
        require(state.deviceOpen && state.devicePath == "/dev/hwbinder" && state.protocolVersion == 8 &&
                state.lastProbeSucceeded && state.probeCount == 0 && state.successfulProbeCount == 0,
                "Reset damaged a live connection or retained counters");
        require(state.identity.pid == 202 && state.identity.uid == 20234 && state.identity.euid == 20235,
                "Snapshot did not refresh real process credentials");
        device.close();
        device.close();
        state = device.snapshot();
        require(!state.deviceOpen && state.devicePath.empty() && !state.protocolAvailable &&
                state.probeCount == 0 && state.lastError.code == 0, "Close did not restore diagnostic baseline");
        require(backend.activeDescriptors.empty() && backend.closeCalls == 1, "Close leaked or double-closed fd");
        require(device.initialize(), "Reopen failed");
        require(device.snapshot().probeCount == 1, "Reopen retained previous probe counters");
    }
    require(backend.activeDescriptors.empty() && backend.closeCalls == 2 && !backend.invalidClose,
            "Destructor leaked or double-closed a descriptor");
}

void checkOpenAndProbeFailures() {
    FakeBackend backend;
    diagnostics::DeviceModel device(backend);
    backend.binderOpenError = EACCES;
    backend.hwbinderOpenError = ENOENT;
    require(!device.initialize(), "Denied devices must not initialize");
    auto state = device.snapshot();
    require(!state.deviceOpen && state.lastError.code == ENOENT &&
            contains(state.lastError.message, "/dev/hwbinder"), "Open errno and path were not retained");
    require(!device.probe() && device.snapshot().lastError.code == ENOENT,
            "Closed probe erased the initialization failure");
    require(backend.closeCalls == 0 && backend.probeCalls == 0, "Failure used an invalid descriptor");

    backend.binderOpenError = backend.hwbinderOpenError = 0;
    backend.actualProtocol = 7;
    require(!device.initialize(), "Unexpected Binder protocol was accepted");
    state = device.snapshot();
    require(state.lastError.code == EPROTO && !state.lastProbeSucceeded && state.probeCount == 2 &&
            state.successfulProbeCount == 0, "Protocol mismatch was not reported");
    require(backend.activeDescriptors.empty() && backend.closeCalls == 2, "Protocol mismatch leaked descriptors");

    backend.actualProtocol = 8;
    backend.probeError = EIO;
    require(!device.initialize(), "Failed BINDER_VERSION must not initialize");
    require(device.snapshot().lastError.code == EIO && backend.activeDescriptors.empty(),
            "Probe errno was lost or its descriptor leaked");
    backend.probeError = 0;
    require(device.initialize(), "Initialization did not recover after failure");
    backend.probeError = EBADF;
    require(!device.initialize(), "Failed reinitialization must close the old connection");
    state = device.snapshot();
    require(!state.deviceOpen && state.devicePath.empty() && state.lastError.code == EBADF &&
            backend.activeDescriptors.empty(), "Failed reinitialization kept an invalid connection");
}

void checkErrnoMessagesUseGenericCategory() {
    FakeBackend backend;
    backend.binderOpenError = backend.hwbinderOpenError = EACCES;
    diagnostics::DeviceModel device(backend);
    require(!device.initialize(), "Denied devices must not initialize");
    const auto error = device.snapshot().lastError;
    require(error.code == EACCES, "Permission-denied errno was not preserved");
    const std::string expected = "open /dev/hwbinder: " +
        std::make_error_code(std::errc::permission_denied).message();
    if (error.message != expected) {
        throw std::runtime_error("Wrong errno message mapping; expected '" + expected +
                                 "', received '" + error.message + "'");
    }
}

void checkModesAndUnsupportedOperations() {
    FakeBackend backend;
    diagnostics::Session session(backend);
    require(!session.isDeviceMode() && session.procInfo() == simulation::kProcAddress, "Wrong initial mode");
    require(session.execute(simulation::kProcAddress + simulation::kExpectedOffset) && session.checkSimulationHealth(),
            "Local simulation did not work");
    std::string json = session.detailedState();
    require(contains(json, "\"transactionCount\":1") && contains(json, "\"simulation\":true"),
            "Simulation JSON contract changed or counters became strings");
    require(session.initializeDevice() && session.isDeviceMode(), "Diagnostic mode did not start");
    require(session.procInfo() == 0, "Device diagnostics invented a kernel address");
    require(!session.execute(simulation::kProcAddress + 64) && !session.checkSimulationHealth(),
            "Device mode accepted simulation operations");
    json = session.detailedState();
    require(contains(json, "\"mode\":\"device_diagnostics\"") && contains(json, "\"deviceOpen\":true") &&
            contains(json, "\"code\":" + std::to_string(ENOTSUP)) && contains(json, "\"uid\":10234"),
            "Device state failed to report unsupported operation and actual credentials");
    require(!session.elevateToRoot(), "Unsupported elevation claimed success");
    backend.currentIdentity = {101, 0, 0};
    require(!session.elevateToRoot(), "Already-root process falsely claimed an elevation feature");
    require(session.probeDevice(), "Device probe did not recover from unsupported operation");
    require(contains(session.detailedState(), "\"code\":0"), "Successful probe retained stale error");
    session.closeDevice();
    require(!session.isDeviceMode() && session.procInfo() == simulation::kProcAddress &&
            !session.checkSimulationHealth(), "Close did not return to a clean simulation");
    json = session.detailedState();
    require(contains(json, "\"transactionCount\":0") && contains(json, "\"flags\":\"0x0000000000000001\"") &&
            contains(json, "\"probeCount\":0") && contains(json, "\"code\":0"), "Mode close kept stale state");

    backend.binderOpenError = backend.hwbinderOpenError = EACCES;
    require(!session.initializeDevice() && !session.isDeviceMode(), "Failed initialization changed mode");
    json = session.detailedState();
    require(contains(json, "\"simulation\":true") && contains(json, "\"deviceOpen\":false") &&
            contains(json, "\"code\":" + std::to_string(EACCES)), "Initialization failure was not exposed to simulation UI");
    require(session.execute(simulation::kProcAddress + 64) && session.checkSimulationHealth(),
            "Diagnostic failure disabled the local model");
}

void checkJsonEscaping() {
    require(diagnostics::jsonString(std::string("\"\\\n\r\t\b\f") + char(1)) ==
                "\"\\\"\\\\\\n\\r\\t\\b\\f\\u0001\"", "Diagnostic strings are not JSON escaped");
}

void checkConcurrentModeTransitions() {
    FakeBackend backend;
    diagnostics::Session session(backend);
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            for (int iteration = 0; iteration < 2000; ++iteration) {
                if (worker == 0) {
                    if (!session.initializeDevice()) failed = true;
                } else if (worker == 1) {
                    session.closeDevice();
                } else if (worker == 2) {
                    session.probeDevice();
                    session.reset();
                }
                const std::string json = session.detailedState();
                const bool deviceMode = contains(json, "\"mode\":\"device_diagnostics\"");
                if (deviceMode != contains(json, "\"deviceOpen\":true")) failed = true;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    session.closeDevice();
    require(!failed && backend.activeDescriptors.empty() && !backend.invalidClose,
            "Concurrent mode transition produced an inconsistent snapshot or descriptor leak");
}

}  // namespace

int main() {
    try {
        checkOpenFallbackReopenAndLifetime();
        checkOpenAndProbeFailures();
        checkErrnoMessagesUseGenericCategory();
        checkModesAndUnsupportedOperations();
        checkJsonEscaping();
        checkConcurrentModeTransitions();
        std::cout << "All native diagnostic tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
