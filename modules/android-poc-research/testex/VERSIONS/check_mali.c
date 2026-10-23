#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>

#define KBASE_IOCTL_TYPE 0x80

struct kbase_ioctl_version_check {
    uint16_t major;
    uint16_t minor;
};

#define KBASE_IOCTL_VERSION_CHECK_52 _IOWR(KBASE_IOCTL_TYPE, 52, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_VERSION_CHECK_0  _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)

int main() {
    printf("[*] Testing /dev/mali0...\n");

    // 1. Проверяем open с O_RDONLY
    int fd = open("/dev/mali0", O_RDONLY);
    if (fd < 0) {
        printf("[-] open(O_RDONLY) failed: %s (errno=%d)\n", strerror(errno), errno);
        // Пробуем O_RDWR
        fd = open("/dev/mali0", O_RDWR);
        if (fd < 0) {
            printf("[-] open(O_RDWR) failed: %s (errno=%d)\n", strerror(errno), errno);
            return 1;
        } else {
            printf("[+] open(O_RDWR) succeeded. fd=%d\n", fd);
        }
    } else {
        printf("[+] open(O_RDONLY) succeeded. fd=%d\n", fd);
    }

    struct kbase_ioctl_version_check version = {0, 0};
    int ret;

    // 2. Пробуем ioctl(52)
    errno = 0;
    ret = ioctl(fd, KBASE_IOCTL_VERSION_CHECK_52, &version);
    printf("[*] ioctl(52) ret=%d, errno=%d (%s)\n", ret, errno, strerror(errno));
    if (ret == 0) {
        printf("[+] Mali Driver Version: r%u p%u\n", version.major, version.minor);
        close(fd);
        return 0;
    }

    // 3. Пробуем ioctl(0)
    version.major = 0; version.minor = 0;
    errno = 0;
    ret = ioctl(fd, KBASE_IOCTL_VERSION_CHECK_0, &version);
    printf("[*] ioctl(0) ret=%d, errno=%d (%s)\n", ret, errno, strerror(errno));
    if (ret == 0) {
        printf("[+] Mali Driver Version: r%u p%u\n", version.major, version.minor);
        close(fd);
        return 0;
    }

    printf("[-] All ioctls failed.\n");
    close(fd);
    return 1;
}