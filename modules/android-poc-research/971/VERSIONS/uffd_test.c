#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/userfaultfd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

int main() {
    printf("[*] Testing userfaultfd...\n");
    int fd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        printf("[-] userfaultfd blocked: %s\n", strerror(errno));
    } else {
        printf("[+] userfaultfd works! FD: %d\n", fd);
        close(fd);
    }
    return 0;
}