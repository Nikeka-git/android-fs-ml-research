// Standalone POSIX test. Link with --wrap=read,write,readdir,fstatat and c++_static.
// Fixtures live in a unique mkdtemp directory beneath TMPDIR and require no elevated access.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include "phase2/file_access.h"

namespace {
bool failNextAllocation = false;
enum class ReadFault { None, InterruptOnce, PartialThenError };
enum class WriteFault { None, InterruptAndPartial, ZeroProgress, PartialThenError };
enum class DirectoryFault { None, InterruptOnce, ErrorAfterEntry, AllocationAfterEntry };
ReadFault readFault = ReadFault::None;
WriteFault writeFault = WriteFault::None;
DirectoryFault directoryFault = DirectoryFault::None;
int readCalls = 0;
int writeCalls = 0;
int directoryCalls = 0;
int directoryFdAtFailure = -1;
bool directoryReturnedEntry = false;
bool failEntryStat = false;

void resetFaults() {
    failNextAllocation = false;
    readFault = ReadFault::None;
    writeFault = WriteFault::None;
    directoryFault = DirectoryFault::None;
    readCalls = writeCalls = directoryCalls = 0;
    directoryReturnedEntry = false;
    failEntryStat = false;
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Operation> void expectError(int expected, Operation operation) {
    bool caught = false;
    try { operation(); }
    catch (const std::system_error& error) {
        caught = true;
        require(error.code().value() == expected, "wrong system_error code");
    }
    require(caught, "expected system_error");
}

void removeFixture(const std::string& path) noexcept {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) return;
    if (!S_ISDIR(info.st_mode)) { ::unlink(path.c_str()); return; }
    DIR* directory = ::opendir(path.c_str());
    if (!directory) return;
    while (dirent* entry = ::readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        try { removeFixture(path + "/" + entry->d_name); } catch (...) {}
    }
    ::closedir(directory);
    ::rmdir(path.c_str());
}
struct Fixture {
    std::string path;
    phase2::FileAccessManager files;
    Fixture() {
        const char* temporary = std::getenv("TMPDIR");
#ifdef __ANDROID__
        if (!temporary || !*temporary) temporary = "/data/local/tmp";
#else
        if (!temporary || !*temporary) temporary = "/tmp";
#endif
        require(temporary[0] == '/', "TMPDIR must be absolute");
        std::string pattern = std::string(temporary) + "/phase2-file-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        char* directory = ::mkdtemp(buffer.data());
        require(directory != nullptr, "mkdtemp failed");
        path = directory;
    }
    ~Fixture() {
        resetFaults();
        removeFixture(path);
    }
    std::string name(const std::string& name) const { return path + "/" + name; }
    std::string put(const std::string& name, const std::string& contents) {
        const std::string target = this->name(name);
        auto handle = files.openFileForWrite(target, 0600);
        require(handle.isOpen(), "fixture open failed");
        require(files.writeFileContent(handle, contents), "fixture write failed");
        return target;
    }
    std::string contents(const std::string& target) {
        const auto handle = files.openFileForRead(target);
        return files.readFileContent(handle);
    }
};

void ownershipAndMove() {
    static_assert(!std::is_copy_constructible<phase2::FileHandle>::value);
    static_assert(!std::is_copy_assignable<phase2::FileHandle>::value);
    static_assert(std::is_nothrow_move_constructible<phase2::FileHandle>::value);
    static_assert(std::is_nothrow_move_assignable<phase2::FileHandle>::value);
    Fixture fixture;
    const std::string path = fixture.put("owned", "abc");
    auto first = fixture.files.openFileForRead(path);
    auto second = fixture.files.openFileForRead(path);
    const int original = first.get(), replaced = second.get();
    second = std::move(first);
    require(!first.isOpen() && second.get() == original, "move assignment did not transfer ownership");
    require(::fcntl(replaced, F_GETFD) == -1 && errno == EBADF, "move assignment leaked destination fd");
    phase2::FileHandle moved(std::move(second));
    require(!second.isOpen() && moved.get() == original, "move constructor did not transfer ownership");
    auto* self = &moved;
    moved = std::move(*self);
    require(moved.get() == original, "self move invalidated fd");
    const int released = moved.release();
    require(!moved.isOpen() && ::fcntl(released, F_GETFD) >= 0, "release closed fd");
    require(::close(released) == 0, "release cleanup failed");
    int owned = -1;
    {
        auto handle = fixture.files.openFileForRead(path);
        owned = handle.get();
        errno = EDOM;
    }
    require(errno == EDOM, "destructor changed errno");
    require(::fcntl(owned, F_GETFD) == -1 && errno == EBADF, "destructor leaked fd");
}

void constructorAllocationFailureClosesRawFd() {
    Fixture fixture;
    const std::string path = fixture.put("allocation", "abc");
    const std::string longPath(512, 'p');
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    require(fd >= 0, "raw open failed");
    bool caught = false;
    failNextAllocation = true;
    try { phase2::FileHandle handle(fd, longPath); }
    catch (const std::bad_alloc&) { caught = true; }
    failNextAllocation = false;
    require(caught, "path copy did not exercise allocation failure");
    require(::fcntl(fd, F_GETFD) == -1 && errno == EBADF, "constructor leaked adopted fd on exception");

    const auto existing = fixture.files.openFileForRead(path);
    const int nextFd = ::dup(existing.get());
    require(nextFd >= 0 && ::close(nextFd) == 0, "could not reserve next fd");
    caught = false;
    failNextAllocation = true;
    try { auto handle = fixture.files.openFileForRead(path); }
    catch (const std::bad_alloc&) { caught = true; }
    failNextAllocation = false;
    require(caught, "open path copy did not exercise allocation failure");
    require(::fcntl(nextFd, F_GETFD) == -1 && errno == EBADF, "manager leaked newly opened fd on exception");
}

void openErrorsAndCloseOnExec() {
    Fixture fixture;
    const std::string path = fixture.put("cloexec", "");
    auto read = fixture.files.openFileForRead(path);
    auto write = fixture.files.openFileForWrite(path);
    require((::fcntl(read.get(), F_GETFD) & FD_CLOEXEC) != 0, "read fd missing CLOEXEC");
    require((::fcntl(write.get(), F_GETFD) & FD_CLOEXEC) != 0, "write fd missing CLOEXEC");
    require(read.close() && read.close() && !read.isOpen(), "explicit close not idempotent");
    const auto missing = fixture.files.openFileForRead(fixture.name("absent"));
    require(!missing.isOpen() && missing.lastError() == ENOENT, "missing file error lost");
    require(!missing.errorMessage().empty(), "missing file message absent");
    expectError(ENOENT, [&] { fixture.files.readFileContent(missing); });
}

void boundedReads() {
    Fixture fixture;
    const std::string data(12'000, 'a');
    const auto handle = fixture.files.openFileForRead(fixture.put("bounded", data));
    for (std::size_t limit : {1U, 37U, 4095U, 4096U, 4097U}) {
        const auto content = fixture.files.readFileContent(handle, limit);
        require(content == data.substr(0, limit), "read exceeded or missed exact limit");
        require(::lseek(handle.get(), 0, SEEK_CUR) == static_cast<off_t>(limit), "read consumed bytes beyond limit");
    }
    require(::lseek(handle.get(), 17, SEEK_SET) == 17, "could not seek fixture");
    readCalls = 0;
    require(fixture.files.readFileContent(handle, 0).empty(), "zero limit returned bytes");
    require(readCalls == 0 && ::lseek(handle.get(), 0, SEEK_CUR) == 17, "zero limit performed I/O");
    expectError(EINVAL, [&] { fixture.files.readFileContent(handle, phase2::FileAccessManager::kMaximumReadSize + 1); });
    expectError(EINVAL, [&] { fixture.files.readFileContent(handle, std::numeric_limits<std::size_t>::max()); });
    require(fixture.files.readFileContent(handle, phase2::FileAccessManager::kMaximumReadSize) == data, "EOF below limit lost bytes");
}

void hardReadCap() {
    Fixture fixture;
    const std::string data(phase2::FileAccessManager::kMaximumReadSize + 123, 'b');
    const auto handle = fixture.files.openFileForRead(fixture.put("large", data));
    const auto prefix = fixture.files.readFileContent(handle);
    require(prefix.size() == phase2::FileAccessManager::kMaximumReadSize, "default cap exceeded");
    require(::lseek(handle.get(), 0, SEEK_CUR) == static_cast<off_t>(prefix.size()), "cap consumed extra bytes");
}

void emptyContentIsDifferentFromReadFailure() {
    Fixture fixture;
    const auto empty = fixture.files.openFileForRead(fixture.put("empty", ""));
    require(fixture.files.readFileContent(empty).empty() && empty.lastError() == 0, "empty file treated as error");
    const phase2::FileHandle closed;
    expectError(EBADF, [&] { fixture.files.readFileContent(closed); });
    const phase2::FileHandle unknown(std::numeric_limits<int>::max());
    expectError(EBADF, [&] { fixture.files.readFileContent(unknown); });
    require(!fixture.files.writeFileContent(unknown, "x") && errno == EBADF, "unknown descriptor write reported success");
    auto writeOnly = fixture.files.openFileForWrite(fixture.name("write-only"));
    expectError(EBADF, [&] { fixture.files.readFileContent(writeOnly); });
    const auto directory = fixture.files.openFileForRead(fixture.path);
    expectError(EISDIR, [&] { fixture.files.readFileContent(directory); });
}

void interruptedReadAndPartialFailure() {
    Fixture fixture;
    const auto handle = fixture.files.openFileForRead(fixture.put("read-errors", "abcdef"));
    readFault = ReadFault::InterruptOnce;
    readCalls = 0;
    require(fixture.files.readFileContent(handle, 6) == "abcdef", "read did not retry EINTR");
    require(readCalls == 2, "EINTR scenario was not exercised");
    readFault = ReadFault::PartialThenError;
    readCalls = 0;
    expectError(EIO, [&] { fixture.files.readFileContent(handle); });
    require(handle.lastError() == EIO && ::lseek(handle.get(), 0, SEEK_CUR) == 2, "partial read error was hidden");
    resetFaults();
    require(fixture.files.readFileContent(handle) == "abcdef" && handle.lastError() == 0, "read could not recover after error");
}

void boundedPipeRead() {
    Fixture fixture;
    int descriptors[2];
    require(::pipe(descriptors) == 0, "pipe failed");
    const phase2::FileHandle reader(descriptors[0]);
    phase2::FileHandle writer(descriptors[1]);
    require(fixture.files.writeFileContent(writer, "stream"), "pipe write failed");
    require(writer.close(), "pipe close failed");
    require(fixture.files.readFileContent(reader, 3) == "str", "nonseekable read failed");
    require(fixture.files.readFileContent(reader, 3) == "eam", "nonseekable read reset stream");
    require(fixture.files.readFileContent(reader, 3).empty(), "pipe EOF not detected");
}

void partialAndInterruptedWrites() {
    Fixture fixture;
    const std::string path = fixture.name("write-errors");
    auto handle = fixture.files.openFileForWrite(path);
    writeFault = WriteFault::InterruptAndPartial;
    writeCalls = 0;
    const std::string binary("ab\0cdefghi", 10);
    require(fixture.files.writeFileContent(handle, binary), "partial write/EINTR did not complete");
    require(writeCalls >= 3, "partial write scenario not exercised");
    resetFaults();
    require(fixture.contents(path) == binary, "binary partial write corrupted content");
    const auto readOnly = fixture.files.openFileForRead(path);
    require(!fixture.files.writeFileContent(readOnly, "x") && errno == EBADF, "read-only fd write not rejected");
}

void zeroProgressWriteFailsWithoutLooping() {
    Fixture fixture;
    const auto handle = fixture.files.openFileForWrite(fixture.name("zero-write"));
    writeFault = WriteFault::ZeroProgress;
    writeCalls = 0;
    require(!fixture.files.writeFileContent(handle, "abc"), "zero progress reported success");
    require(errno == EIO && handle.lastError() == EIO && writeCalls == 1, "zero progress did not terminate with EIO");
    resetFaults();
}

void partialWriteFailureIsReportedWithoutClaimingRollback() {
    Fixture fixture;
    const std::string path = fixture.name("partial-write");
    const auto handle = fixture.files.openFileForWrite(path);
    writeFault = WriteFault::PartialThenError;
    writeCalls = 0;
    require(!fixture.files.writeFileContent(handle, "abcdef"), "partial write failure reported success");
    require(errno == ENOSPC && handle.lastError() == ENOSPC, "write errno lost");
    resetFaults();
    require(fixture.contents(path) == "ab", "partial write prefix unexpectedly rolled back or completed");
    require(fixture.files.writeFileContent(handle, "z") && handle.lastError() == 0, "write did not recover");
    require(fixture.contents(path) == "abz", "write current-offset behavior changed");
}

void writingDoesNotSilentlyTruncateOrAppend() {
    Fixture fixture;
    const std::string path = fixture.put("existing", "ABCDE");
    const auto handle = fixture.files.openFileForWrite(path);
    require(fixture.files.writeFileContent(handle, "xy"), "overwrite prefix failed");
    require(fixture.contents(path) == "xyCDE", "write silently truncated or appended");
    require(fixture.files.writeFileContent(handle, ""), "empty write failed");
    require(fixture.contents(path) == "xyCDE", "empty write changed file");
}

void initializedMetadataAndDirectoryClassification() {
    Fixture fixture;
    const std::string file = fixture.put("data", "abcdef");
    fixture.put("other", "xyz");
    require(::mkdir(fixture.name("child").c_str(), 0700) == 0, "mkdir failed");
    require(fixture.files.createSymlink("data", fixture.name("link")), "symlink failed");
    const auto info = fixture.files.fileInfo(file);
    require(info.metadataAvailable && info.size == 6 && S_ISREG(info.permissions), "metadata missing");
    require(info.fd == -1 && !info.isOpen && info.lastError == 0 && info.lastErrorMessage.empty(), "stat fabricated open descriptor");
    const auto missing = fixture.files.fileInfo(fixture.name("missing"));
    require(!missing.metadataAvailable && !missing.isOpen && missing.fd == -1, "failed metadata claims success");
    require(missing.size == 0 && missing.permissions == 0 && missing.ownerId == 0 && missing.groupId == 0, "failed metadata uninitialized");
    require(missing.lastError == ENOENT && !missing.lastErrorMessage.empty(), "metadata failure diagnostic missing");
    const auto listing = fixture.files.listDirectory(fixture.path);
    require(listing.entries == std::vector<std::string>({"child", "data", "link", "other"}), "directory entries incorrect");
    require(listing.files == std::vector<std::string>({"data", "other"}), "symlink or directory classified as regular file");
    require(listing.directories == std::vector<std::string>({"child"}) && listing.totalSize == 9, "directory size includes metadata or link bytes");
    require(listing.lastError == 0 && listing.errorCount == 0 && listing.errorMessage.empty(), "successful directory reports error");
    require(fixture.files.chmodFile(file, 0600), "chmod own fixture failed");
    require((fixture.files.fileInfo(file).permissions & 0777) == 0600, "chmod did not apply requested permissions");
}

void directoryFailuresRemainVisible() {
    Fixture fixture;
    const auto missing = fixture.files.listDirectory(fixture.name("missing"));
    require(missing.errorCount == 1 && missing.lastError == ENOENT && !missing.errorMessage.empty(), "opendir failure hidden");
    const std::string regular = fixture.put("regular", "");
    const auto notDirectory = fixture.files.listDirectory(regular);
    require(notDirectory.errorCount == 1 && notDirectory.lastError == ENOTDIR, "non-directory reported as empty directory");
    fixture.put("lost-entry", "present but simulated concurrent metadata failure");
    failEntryStat = true;
    const auto partial = fixture.files.listDirectory(fixture.path);
    resetFaults();
    require(std::find(partial.entries.begin(), partial.entries.end(), "lost-entry") != partial.entries.end(), "metadata failure erased observed entry");
    require(std::find(partial.files.begin(), partial.files.end(), "lost-entry") == partial.files.end(), "failed metadata fabricated type");
    require(partial.errorCount == 1 && partial.lastError == ENOENT && partial.errorMessage.find("lost-entry") != std::string::npos, "entry stat failure hidden");
}

void directoryIterationFailureAndInterruption() {
    Fixture fixture;
    fixture.put("entry", "x");
    directoryFault = DirectoryFault::InterruptOnce;
    directoryCalls = 0;
    auto listing = fixture.files.listDirectory(fixture.path);
    resetFaults();
    require(listing.errorCount == 0 && listing.files.size() == 1, "readdir EINTR was not retried");
    directoryFault = DirectoryFault::ErrorAfterEntry;
    listing = fixture.files.listDirectory(fixture.path);
    resetFaults();
    require(listing.entries.size() == 1 && listing.errorCount == 1 && listing.lastError == EIO, "readdir partial failure hidden");
    require(!listing.errorMessage.empty(), "readdir error message absent");
}

void directoryAllocationFailureClosesItsDescriptor() {
    Fixture fixture;
    fixture.put("entry", "x");
    directoryFdAtFailure = -1;
    directoryFault = DirectoryFault::AllocationAfterEntry;
    bool caught = false;
    try { fixture.files.listDirectory(fixture.path); }
    catch (const std::bad_alloc&) { caught = true; }
    resetFaults();
    require(caught && directoryFdAtFailure >= 0, "directory allocation failure not exercised");
    require(::fcntl(directoryFdAtFailure, F_GETFD) == -1 && errno == EBADF, "directory leaked after allocation failure");
}

void nulPathsAreRejectedBeforeAnyPrefixSideEffect() {
    Fixture fixture;
    const std::string path = fixture.put("protected-prefix", "unchanged");
    const std::string nulPath = path + std::string("\0ignored", 8);
    require(!fixture.files.openFileForRead(nulPath).isOpen(), "NUL read path accepted");
    const auto failedWrite = fixture.files.openFileForWrite(nulPath);
    require(!failedWrite.isOpen() && failedWrite.lastError() == EINVAL, "NUL write path accepted");
    require(!fixture.files.chmodFile(nulPath, 0000) && errno == EINVAL, "NUL chmod applied prefix");
    require(!fixture.files.chownFile(nulPath, ::getuid(), ::getgid()) && errno == EINVAL, "NUL chown accepted");
    require(!fixture.files.createSymlink(nulPath, fixture.name("bad-target")) && errno == EINVAL, "NUL link target accepted");
    require(!fixture.files.createSymlink("target", nulPath) && errno == EINVAL, "NUL link path accepted");
    require(fixture.files.fileInfo(nulPath).lastError == EINVAL, "NUL metadata path accepted");
    const auto directory = fixture.files.listDirectory(nulPath);
    require(directory.lastError == EINVAL && directory.errorCount == 1, "NUL directory path accepted");
    expectError(EINVAL, [&] { fixture.files.readSymlink(nulPath); });
    require(fixture.contents(path) == "unchanged", "NUL path modified existing prefix");
    require((fixture.files.fileInfo(path).permissions & 0777) == 0600, "NUL path changed prefix permissions");
}

void symlinkTargetsAreCompleteAndErrorsDistinct() {
    Fixture fixture;
    const std::string target(1500, 'x');
    const std::string link = fixture.name("long-link");
    require(fixture.files.createSymlink(target, link), "long symlink creation failed");
    require(fixture.files.readSymlink(link) == target, "symlink target silently truncated");
    require(S_ISLNK(fixture.files.fileInfo(link).permissions), "metadata followed symlink");
    const auto listing = fixture.files.listDirectory(fixture.path);
    require(listing.entries.size() == 1 && listing.files.empty() && listing.totalSize == 0, "broken symlink metadata lost or counted as file");
    expectError(ENOENT, [&] { fixture.files.readSymlink(fixture.name("missing")); });
    const std::string regular = fixture.put("regular", "");
    expectError(EINVAL, [&] { fixture.files.readSymlink(regular); });
}

void procIdentifiersCannotChangeTheRequestedPath() {
    Fixture fixture;
    for (const char* id : {"", "0", "-1", "../self", "/self", "self/status", "9999999999999999999999999"}) {
        expectError(EINVAL, [&] { fixture.files.readProcInfo(id); });
    }
    expectError(EINVAL, [&] { fixture.files.readProcInfo(std::string("self\0other", 10)); });
    require(fixture.files.readProcInfo("self").find("Pid:") != std::string::npos, "self process status unavailable");
    require(fixture.files.readProcInfo(std::to_string(::getpid())).find("Pid:") != std::string::npos, "numeric process status unavailable");
}
}  // namespace

// Deterministic fault injection only in this test executable; real calls are used otherwise.
extern "C" ssize_t __real_read(int fd, void* buffer, size_t count);
extern "C" ssize_t __real_write(int fd, const void* buffer, size_t count);
extern "C" dirent* __real_readdir(DIR* directory);
extern "C" int __real_fstatat(int directory, const char* path, struct stat* info, int flags);
extern "C" ssize_t __wrap_read(int fd, void* buffer, size_t count) {
    ++readCalls;
    if (readFault == ReadFault::InterruptOnce && readCalls == 1) { errno = EINTR; return -1; }
    if (readFault == ReadFault::PartialThenError) {
        if (readCalls == 1) return __real_read(fd, buffer, std::min(count, std::size_t(2)));
        errno = EIO;
        return -1;
    }
    return __real_read(fd, buffer, count);
}
extern "C" ssize_t __wrap_write(int fd, const void* buffer, size_t count) {
    ++writeCalls;
    if (writeFault == WriteFault::ZeroProgress) return 0;
    if (writeFault == WriteFault::InterruptAndPartial) {
        if (writeCalls == 1) { errno = EINTR; return -1; }
        return __real_write(fd, buffer, std::min(count, std::size_t(2)));
    }
    if (writeFault == WriteFault::PartialThenError) {
        if (writeCalls == 1) return __real_write(fd, buffer, std::min(count, std::size_t(2)));
        errno = ENOSPC;
        return -1;
    }
    return __real_write(fd, buffer, count);
}
extern "C" dirent* __wrap_readdir(DIR* directory) {
    ++directoryCalls;
    if (directoryFault == DirectoryFault::InterruptOnce && directoryCalls == 1) { errno = EINTR; return nullptr; }
    if (directoryFault == DirectoryFault::ErrorAfterEntry && directoryReturnedEntry) { errno = EIO; return nullptr; }
    dirent* entry = __real_readdir(directory);
    if (entry && std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) {
        directoryReturnedEntry = true;
        if (directoryFault == DirectoryFault::AllocationAfterEntry) {
            directoryFdAtFailure = ::dirfd(directory);
            failNextAllocation = true;
        }
    }
    return entry;
}
extern "C" int __wrap_fstatat(int directory, const char* path, struct stat* info, int flags) {
    if (failEntryStat && std::strcmp(path, "lost-entry") == 0) { errno = ENOENT; return -1; }
    return __real_fstatat(directory, path, info, flags);
}

void* operator new(std::size_t size) {
    if (failNextAllocation) { failNextAllocation = false; throw std::bad_alloc(); }
    if (void* allocated = std::malloc(size ? size : 1)) return allocated;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main() {
    const struct { const char* name; void (*run)(); } tests[] = {
        {"ownership and move", ownershipAndMove},
        {"allocation failure closes raw and newly opened fd", constructorAllocationFailureClosesRawFd},
        {"open errors and close-on-exec", openErrorsAndCloseOnExec},
        {"exact bounded reads", boundedReads},
        {"default read hard cap", hardReadCap},
        {"empty contents versus read failures", emptyContentIsDifferentFromReadFailure},
        {"read EINTR and partial failure", interruptedReadAndPartialFailure},
        {"bounded stream reads", boundedPipeRead},
        {"partial writes and EINTR", partialAndInterruptedWrites},
        {"zero progress write", zeroProgressWriteFailsWithoutLooping},
        {"partial write failure", partialWriteFailureIsReportedWithoutClaimingRollback},
        {"explicit non-truncating write policy", writingDoesNotSilentlyTruncateOrAppend},
        {"metadata and directory types", initializedMetadataAndDirectoryClassification},
        {"directory open and entry errors", directoryFailuresRemainVisible},
        {"directory iteration errors", directoryIterationFailureAndInterruption},
        {"directory exception cleanup", directoryAllocationFailureClosesItsDescriptor},
        {"embedded NUL rejection", nulPathsAreRejectedBeforeAnyPrefixSideEffect},
        {"complete symlink targets", symlinkTargetsAreCompleteAndErrorsDistinct},
        {"proc identifier validation", procIdentifiersCannotChangeTheRequestedPath},
    };
    unsigned failed = 0;
    for (const auto& test : tests) {
        resetFaults();
        try {
            test.run();
            std::printf("PASS %s\n", test.name);
        } catch (const std::exception& error) {
            resetFaults();
            ++failed;
            std::fprintf(stderr, "FAIL %s: %s\n", test.name, error.what());
        }
    }
    std::printf("File access: %zu tests, %u failures\n", sizeof(tests) / sizeof(tests[0]), failed);
    return failed ? 1 : 0;
}
