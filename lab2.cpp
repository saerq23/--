#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static volatile sig_atomic_t g_sighup = 0;

static void on_sighup(int) { g_sighup = 1; }

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(int argc, char** argv) {
    int port = 12345;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port\n");
            return 1;
        }
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    if (set_nonblock(listen_fd) < 0) {
        perror("fcntl(O_NONBLOCK)");
        close(listen_fd);
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sighup;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGHUP, &sa, nullptr) < 0) {
        perror("sigaction");
        close(listen_fd);
        return 1;
    }

    sigset_t blockmask, origmask, waitmask;
    sigemptyset(&blockmask);
    sigaddset(&blockmask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &blockmask, &origmask) < 0) {
        perror("sigprocmask");
        close(listen_fd);
        return 1;
    }
    waitmask = origmask;
    sigdelset(&waitmask, SIGHUP);

    int client_fd = -1;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;

        if (client_fd >= 0) {
            FD_SET(client_fd, &rfds);
            if (client_fd > maxfd) maxfd = client_fd;
        }

        int r = pselect(maxfd + 1, &rfds, nullptr, nullptr, nullptr, &waitmask);

        if (g_sighup) {
            printf("SIGHUP received\n");
            fflush(stdout);
            g_sighup = 0;
        }

        if (r < 0) {
            if (errno == EINTR) continue;
            perror("pselect");
            break;
        }

        if (FD_ISSET(listen_fd, &rfds)) {
            for (;;) {
                int fd = accept(listen_fd, nullptr, nullptr);
                if (fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    perror("accept");
                    break;
                }

                printf("New connection\n");
                fflush(stdout);

                if (client_fd < 0) {
                    client_fd = fd;
                } else {
                    close(fd);
                }
            }
        }

        if (client_fd >= 0 && FD_ISSET(client_fd, &rfds)) {
            char buf[4096];
            ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
            if (n > 0) {
                printf("Received %zd bytes\n", n);
                fflush(stdout);
            } else if (n == 0) {
                close(client_fd);
                client_fd = -1;
            } else {
                if (errno != EINTR) {
                    perror("recv");
                    close(client_fd);
                    client_fd = -1;
                }
            }
        }
    }

    if (client_fd >= 0) close(client_fd);
    close(listen_fd);
    return 0;
}
