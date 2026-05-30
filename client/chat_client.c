/*
 * chat_client.c — Chat Hub Client
 * TCP  : auth, privés, commandes, keepalive
 * UDP  : réception broadcasts (port aléatoire, communiqué au serveur)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common/protocol.h"

static int   tcp_fd  = -1;
static int   udp_fd  = -1;
static char  my_pseudo[MAX_PSEUDO];
static int   running = 1;
static int   udp_local_port = 0;
static struct sockaddr_in srv_udp_addr;

static void print_public(const char *from, const char *body) {
    printf("\r\033[K\033[1;36m%s\033[0m: %s\n%s> ", from, body, my_pseudo);
    fflush(stdout);
}
static void print_private(const char *from, const char *body) {
    printf("\r\033[K\033[1;35m[PRIVE de %s]\033[0m %s\n%s> ", from, body, my_pseudo);
    fflush(stdout);
}
static void print_system(const char *body) {
    printf("\r\033[K\033[1;32m[INFO]\033[0m %s\n%s> ", body, my_pseudo);
    fflush(stdout);
}

static int send_tcp(const Message *m) {
    return (send(tcp_fd, m, MSG_SIZE, 0) == (ssize_t)MSG_SIZE) ? 0 : -1;
}

/* ─── Thread réception ───────────────────────────────── */
static void *recv_thread(void *arg) {
    (void)arg;
    int maxfd = (tcp_fd > udp_fd) ? tcp_fd : udp_fd;
    fd_set fds;

    while (running) {
        FD_ZERO(&fds);
        FD_SET(tcp_fd, &fds);
        FD_SET(udp_fd, &fds);
        struct timeval tv = {1, 0};
        int r = select(maxfd + 1, &fds, NULL, NULL, &tv);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) continue;

        /* TCP */
        if (FD_ISSET(tcp_fd, &fds)) {
            Message m;
            ssize_t n = recv(tcp_fd, &m, MSG_SIZE, MSG_WAITALL);
            if (n <= 0) { print_system("Connexion perdue."); running = 0; break; }
            switch (m.type) {
            case MSG_CONNECT_OK:  print_system(m.body); break;
            case MSG_CONNECT_ERR: print_system(m.body); running = 0; break;
            case MSG_SYSTEM:      print_system(m.body); break;
            case MSG_PRIVATE:     print_private(m.from, m.body); break;
            case MSG_PING: {
                Message pong; memset(&pong, 0, sizeof(pong));
                pong.type = MSG_PONG;
                strncpy(pong.from, my_pseudo, MAX_PSEUDO - 1);
                send_tcp(&pong);
                break;
            }
            default: break;
            }
        }

        /* UDP — messages publics */
        if (FD_ISSET(udp_fd, &fds)) {
            Message m;
            struct sockaddr_in src; socklen_t slen = sizeof(src);
            ssize_t n = recvfrom(udp_fd, &m, MSG_SIZE, 0,
                                 (struct sockaddr *)&src, &slen);
            if (n > 0 && m.type == MSG_PUBLIC)
                if (strcmp(m.from, my_pseudo) != 0)
                    print_public(m.from, m.body);
        }
    }
    return NULL;
}

/* ─── Connexion ──────────────────────────────────────── */
static int connect_to_server(const char *host, int tcp_port) {
    /* TCP */
    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) { perror("socket TCP"); return -1; }
    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET;
    srv.sin_port   = htons((uint16_t)tcp_port);
    if (inet_pton(AF_INET, host, &srv.sin_addr) <= 0) {
        fprintf(stderr, "IP invalide : %s\n", host); return -1;
    }
    if (connect(tcp_fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("connect TCP"); return -1;
    }

    /* UDP — port aléatoire (0) pour éviter les conflits */
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) { perror("socket UDP"); return -1; }
    struct sockaddr_in local = {0};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = 0; /* OS choisit le port */
    if (bind(udp_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind UDP"); return -1;
    }
    /* Récupérer le port attribué */
    socklen_t slen = sizeof(local);
    getsockname(udp_fd, (struct sockaddr *)&local, &slen);
    udp_local_port = ntohs(local.sin_port);
    printf("UDP local port : %d\n", udp_local_port);

    /* Adresse UDP serveur */
    memset(&srv_udp_addr, 0, sizeof(srv_udp_addr));
    srv_udp_addr.sin_family = AF_INET;
    srv_udp_addr.sin_port   = htons((uint16_t)(tcp_port + 1));
    inet_pton(AF_INET, host, &srv_udp_addr.sin_addr);

    return 0;
}

/* ─── Auth — envoie le port UDP au serveur ───────────── */
static int authenticate(void) {
    printf("Pseudo : "); fflush(stdout);
    if (!fgets(my_pseudo, sizeof(my_pseudo), stdin)) return -1;
    my_pseudo[strcspn(my_pseudo, "\n")] = '\0';
    if (!my_pseudo[0]) return -1;
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_CONNECT;
    strncpy(m.from, my_pseudo, MAX_PSEUDO - 1);
    /* Port UDP local dans body */
    snprintf(m.body, MAX_MSG, "%d", udp_local_port);
    return send_tcp(&m);
}

/* ─── Boucle stdin ───────────────────────────────────── */
static void input_loop(void) {
    char line[MAX_MSG + MAX_PSEUDO + 10];
    printf("\nCommandes : /msg <pseudo> <texte>  |  /users  |  /quit\n");
    printf("────────────────────────────────────────────────────\n");

    while (running) {
        printf("%s> ", my_pseudo); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = '\0';
        if (!line[0]) continue;

        Message m; memset(&m, 0, sizeof(m));
        strncpy(m.from, my_pseudo, MAX_PSEUDO - 1);

        if (strcmp(line, "/quit") == 0) {
            m.type = MSG_DISCONNECT; send_tcp(&m); running = 0;

        } else if (strcmp(line, "/users") == 0) {
            m.type = MSG_USERS; send_tcp(&m);

        } else if (strncmp(line, "/msg ", 5) == 0) {
            char *rest = line + 5, *sp = strchr(rest, ' ');
            if (!sp) { printf("Usage : /msg <pseudo> <texte>\n"); continue; }
            *sp = '\0';
            strncpy(m.to, rest, MAX_PSEUDO - 1);
            strncpy(m.body, sp + 1, MAX_MSG - 1);
            m.type = MSG_PRIVATE; send_tcp(&m);
            printf("\033[1;35m[PRIVE -> %s]\033[0m %s\n", m.to, m.body);

        } else {
            /* Message public → UDP vers serveur */
            m.type = MSG_PUBLIC;
            strncpy(m.body, line, MAX_MSG - 1);
            sendto(udp_fd, &m, MSG_SIZE, 0,
                   (struct sockaddr *)&srv_udp_addr, sizeof(srv_udp_addr));
            printf("\033[1;36m%s\033[0m: %s\n", my_pseudo, line);
        }
    }
}

int main(int argc, char *argv[]) {
    const char *host = "127.0.0.1";
    int tcp_port = DEFAULT_TCP_PORT;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--server") == 0) host     = argv[i + 1];
        if (strcmp(argv[i], "--port")   == 0) tcp_port = atoi(argv[i + 1]);
    }
    printf("Connexion a %s:%d ...\n", host, tcp_port);
    if (connect_to_server(host, tcp_port) < 0) { fprintf(stderr, "Echec connexion.\n"); exit(1); }
    printf("Connecte.\n");
    if (authenticate() < 0) { fprintf(stderr, "Erreur auth.\n"); exit(1); }

    pthread_t rt;
    pthread_create(&rt, NULL, recv_thread, NULL);
    usleep(300000);
    if (!running) { pthread_join(rt, NULL); close(tcp_fd); close(udp_fd); exit(1); }

    input_loop();
    running = 0;
    pthread_join(rt, NULL);
    close(tcp_fd); close(udp_fd);
    printf("Deconnecte. A bientot !\n");
    return 0;
}
