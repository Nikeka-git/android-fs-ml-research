#include "../phase2/phase2_session.h"

#include <cerrno>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class BinderBackend final : public diagnostics::Backend {
public:
    int openError = 0;
    int versionError = 0;
    int live = 0;
    int closes = 0;
    bool invalidUse = false;

    diagnostics::OpenResult openDevice(const char*) noexcept override {
        if (openError != 0) return {-1, openError};
        if (live != 0) invalidUse = true;
        ++live;
        return {5, 0};
    }
    diagnostics::VersionResult probeVersion(int fd) noexcept override {
        if (fd != 5 || live != 1) invalidUse = true;
        return versionError == 0 ? diagnostics::VersionResult{true, 8, 0}
                                 : diagnostics::VersionResult{false, 0, versionError};
    }
    void closeDevice(int fd) noexcept override {
        if (fd != 5 || live != 1) invalidUse = true;
        --live;
        ++closes;
    }
    diagnostics::Identity identity() const noexcept override { return {12, 10001, 10001}; }
    int expectedProtocolVersion() const noexcept override { return 8; }
};

class PrivilegeBackend final : public phase2::PrivilegeBackend {
public:
    phase2::PrivilegeObservation observation;
    PrivilegeBackend() {
        observation.credentialsAvailable = true;
        observation.realUid = 10001;
        observation.realGid = 10002;
        observation.effectiveUid = 10003;
        observation.effectiveGid = 10004;
        observation.credentialsError = 0;
        observation.capabilitiesAvailable = true;
        observation.effectiveCapabilityMask = UINT64_C(1) << 7;
        observation.capabilitiesError = 0;
    }
    phase2::PrivilegeObservation observe() const noexcept override { return observation; }
};

struct Fixture {
    std::string directory;
    std::string file;
    Fixture() {
        const char* environment = std::getenv("TMPDIR");
        const std::string root = environment != nullptr ? environment : "/data/local/tmp";
        const std::string pattern = root + "/phase2-session-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        char* created = ::mkdtemp(name.data());
        require(created != nullptr, "mkdtemp failed");
        directory = created;
        file = directory + "/data.txt";
    }
    ~Fixture() { ::unlink(file.c_str()); ::rmdir(directory.c_str()); }
};

void initialStateIsDeterministic() {
    BinderBackend binder;
    PrivilegeBackend privilege;
    phase2::Phase2Session session(binder, privilege);
    const phase2::Phase2State state = session.currentState();
    require(!state.phase2Active && !state.fileAccessEnabled, "Initial session was active");
    require(!state.privilegeElevated && !state.rootAccess, "Initial session invented root");
    require(!state.deviceInitialized && !state.cveExploitationActive, "Initial device/CVE flags set");
    require(state.totalTransactions == 0 && state.lastError == 0 && state.lastMessage.empty(), "Uninitialized counters/errors");
    require(binder.live == 0 && binder.closes == 0, "Constructor opened a device");
    require(session.getDetailedReport().find("\"phase2Active\":false") != std::string::npos, "Report hardcoded active");
}

void failedBinderDoesNotDisableNormalFiles() {
    Fixture fixture;
    const int fd = ::open(fixture.file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    require(fd >= 0, "Fixture open failed");
    phase2::FileHandle output(fd, fixture.file);
    phase2::FileAccessManager files;
    require(files.writeFileContent(output, "ordinary process access"), "Fixture write failed");
    require(output.close(), "Fixture close failed");

    BinderBackend binder;
    binder.openError = EACCES;
    PrivilegeBackend privilege;
    phase2::Phase2Session session(binder, privilege);
    require(!session.initialize(), "Denied Binder initialization succeeded");
    const phase2::Phase2State denied = session.currentState();
    require(denied.lastError == EACCES, "Session replaced the actual Binder error");
    require(denied.phase2Active && denied.fileAccessEnabled && !denied.deviceInitialized, "File support incorrectly depended on Binder");
    phase2::FileHandle input = session.openFile(fixture.file);
    require(input.isOpen(), "Normal file access failed after Binder denial");
    require(files.readFileContent(input) == "ordinary process access", "File data changed");
    require(session.currentState().lastError == 0, "Successful file access retained stale error");
    session.close();
    require(::fcntl(input.get(), F_GETFD) >= 0, "Session closed a caller-owned handle");
    require(!session.currentState().phase2Active, "close did not deactivate session");
}

void actualRootIsDifferentFromElevation() {
    BinderBackend binder;
    PrivilegeBackend privilege;
    privilege.observation.effectiveUid = 0;
    phase2::Phase2Session session(binder, privilege);
    require(session.currentState().rootAccess, "Actual root identity was hidden");
    require(!session.elevateToRoot(), "Unsupported elevation returned success");
    const phase2::Phase2State state = session.currentState();
    require(state.rootAccess && !state.privilegeElevated && state.lastError == ENOTSUP, "Root was conflated with elevation");
    privilege.observation.effectiveUid = 10003;
    require(!session.currentState().rootAccess, "Root observation remained stale");
    privilege.observation.credentialsAvailable = false;
    privilege.observation.credentialsError = EIO;
    require(!session.currentState().rootAccess, "Unknown identity was reported as root");
    const std::string report = session.getDetailedReport();
    require(report.find("\"euid\":null") != std::string::npos, "Unknown identity serialized as a numeric UID");
    std::cout << "REPORT_JSON=" << report << '\n';
}

void probeErrorsAndCloseArePropagated() {
    BinderBackend binder;
    PrivilegeBackend privilege;
    {
        phase2::Phase2Session session(binder, privilege);
        require(session.initialize(), "Mock Binder connection failed");
        require(session.initialize() && binder.live == 1, "Repeated initialize leaked a descriptor");
        require(!session.currentState().cveExploitationActive, "Connection claimed CVE exploitation");
        require(session.probeDevice(), "Probe failed");
        require(!session.executeCVEExploitation(), "Unsupported CVE operation succeeded");
        require(session.currentState().totalTransactions == 0, "Probe/exploit attempt fabricated transactions");
        binder.versionError = EIO;
        require(!session.monitorDevice(), "Broken device monitor succeeded");
        require(session.currentState().lastError == EIO, "Monitor failed to propagate errno");
        session.close();
        session.close();
        require(binder.live == 0 && binder.closes == 1 && !binder.invalidUse, "Close was not idempotent");
        const phase2::Phase2State closed = session.currentState();
        require(!closed.phase2Active && !closed.deviceInitialized && closed.lastError == 0, "Close retained session state");
        binder.versionError = 0;
        require(session.initialize(), "Could not reopen after close");
    }
    require(binder.live == 0 && binder.closes == 2 && !binder.invalidUse, "Destructor leaked its device");
}

void metadataErrorsAndJsonEscaping() {
    Fixture fixture;
    BinderBackend binder;
    PrivilegeBackend privilege;
    phase2::Phase2Session session(binder, privilege);
    const std::string missing = fixture.directory + "/missing\"\\\r\t\b\f" + '\x01';
    phase2::FileHandle handle = session.openFile(missing);
    require(!handle.isOpen() && session.currentState().lastError == ENOENT, "Open error was lost");
    const std::string report = session.getDetailedReport();
    for (const unsigned char character : report) require(character >= 0x20, "Raw control character in JSON");
    require(report.find("\\u0001") != std::string::npos, "JSON did not escape a control byte");
    std::cout << "REPORT_JSON=" << report << '\n';
    const auto info = session.fileInfo(fixture.directory);
    require(info.metadataAvailable && !info.isOpen && info.fd == -1, "Metadata falsely claimed an open descriptor");
    require(session.currentState().lastError == 0, "Successful metadata retained old error");
    const auto listing = session.listDirectory(missing);
    require(listing.lastError == ENOENT && session.currentState().lastError == ENOENT, "Directory error was lost");

    const std::string unicode = u8"\u041f\u0440\u0438\u0432\u0435\u0442 \U0001f600";
    const std::string malformed = unicode + static_cast<char>(0xff) + "\xed\xa0\x80";
    const std::string encoded = phase2::detail::jsonString(malformed);
    require(encoded.find(unicode) != std::string::npos, "Valid UTF-8 was corrupted");
    require(encoded.find("\\ufffd") != std::string::npos, "Malformed UTF-8 was not replaced");
    session.openFile(fixture.directory + "/" + malformed);
    std::cout << "REPORT_JSON=" << session.getDetailedReport() << '\n';
}

void concurrentSnapshotsRemainConsistent() {
    BinderBackend binder;
    PrivilegeBackend privilege;
    phase2::Phase2Session session(binder, privilege);
    std::exception_ptr error;
    std::mutex errorMutex;
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&, worker] {
            try {
                for (int iteration = 0; iteration < 64; ++iteration) {
                    switch ((iteration + worker) % 4) {
                        case 0: session.initialize(); break;
                        case 1: session.close(); break;
                        case 2: require(!session.executeCVEExploitation(), "Concurrent CVE operation succeeded"); break;
                        default: session.probeDevice(); break;
                    }
                    const auto state = session.currentState();
                    require(state.phase2Active == state.fileAccessEnabled, "Torn activity/file snapshot");
                    require(!state.deviceInitialized || state.phase2Active, "Inactive session owned a device");
                    require(!state.privilegeElevated && !state.cveExploitationActive && state.totalTransactions == 0, "Snapshot fabricated privilege/exploit results");
                    require(session.getDetailedReport().find("\"exploitationSupported\":false") != std::string::npos, "Unsupported capability changed");
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (!error) error = std::current_exception();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    if (error) std::rethrow_exception(error);
    session.close();
    require(binder.live == 0 && !binder.invalidUse, "Concurrent operations leaked/reused a descriptor");
}

void platformIdentityIsObservedWithoutChanges() {
    const uid_t uid = ::getuid();
    const uid_t euid = ::geteuid();
    const gid_t gid = ::getgid();
    const gid_t egid = ::getegid();
    phase2::Phase2Session session;
    require(session.currentState().rootAccess == (euid == 0), "Platform root observation disagreed with OS");
    require(!session.elevateToRoot(), "Platform elevation should be unsupported");
    require(::getuid() == uid && ::geteuid() == euid && ::getgid() == gid && ::getegid() == egid, "Inspection changed process credentials");
    std::cout << "REPORT_JSON=" << session.getDetailedReport() << '\n';
}
}  // namespace

int main() {
    try {
        initialStateIsDeterministic();
        failedBinderDoesNotDisableNormalFiles();
        actualRootIsDifferentFromElevation();
        probeErrorsAndCloseArePropagated();
        metadataErrorsAndJsonEscaping();
        concurrentSnapshotsRemainConsistent();
        platformIdentityIsObservedWithoutChanges();
        std::cout << "Phase 2 session tests passed (7 groups, 256 concurrent iterations)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Phase 2 session tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
