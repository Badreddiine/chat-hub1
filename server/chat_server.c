/*
 * chat_server.c — Chat Hub Server
 * TCP  : connexions, auth, privés, notifications, keepalive
 * UDP  : unicast vers chaque client (port communiqué à la connexion)
 * TCP fallback : pour clients sur loopback (même machine)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common/protocol.h"

#define PING_INTERVAL 15
#define PING_TIMEOUT  30
#define BACKLOG       16

typedef struct {
    int            fd;
    char           pseudo[MAX_PSEUDO];
    struct sockaddr_in udp_addr;
    time_t         last_pong;
    int            active;
    int            udp_ready;
} Client;

static Client  clients[MAX_CLIENTS];
static int     tcp_sock = -1;
static int     udp_sock = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void log_event(const char *fmt, ...) {
    va_list ap; time_t t = time(NULL);
    struct tm *ti = localtime(&t); char ts[20];
    strftime(ts, sizeof(ts), "%H:%M:%S", ti);
    printf("[%s] ", ts); va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n"); fflush(stdout);
}

static int send_tcp(int fd, const Message *m) {
    return (send(fd, m, MSG_SIZE, MSG_NOSIGNAL) == (ssize_t)MSG_SIZE) ? 0 : -1;
}

static void send_system(int fd, const char *text) {
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_SYSTEM;
    strncpy(m.from, "SERVER", MAX_PSEUDO - 1);
    strncpy(m.body, text, MAX_MSG - 1);
    send_tcp(fd, &m);
}

/* UDP unicast vers chaque client + TCP fallback */
static void broadcast_public(const Message *m, int sender_fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) continue;
        if (clients[i].fd == sender_fd) continue;

        /* Essai UDP unicast */
        if (clients[i].udp_ready) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clients[i].udp_addr.sin_addr, ip, sizeof(ip));
            log_event("[UDP] -> %s:%d (%s)", ip,
                      ntohs(clients[i].udp_addr.sin_port), clients[i].pseudo);
            sendto(udp_sock, m, MSG_SIZE, 0,
                   (struct sockaddr *)&clients[i].udp_addr,
                   sizeof(clients[i].udp_addr));
        }

        /* TCP fallback (garantit la réception dans tous les cas) */
        send_tcp(clients[i].fd, m);
    }
}

static Client *find_client(const char *pseudo) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active && strcmp(clients[i].pseudo, pseudo) == 0)
            return &clients[i];
    return NULL;
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!clients[i].active && clients[i].fd <= 0) return i;
    return -1;
}

static void build_users(char *buf, size_t sz) {
    buf[0] = '\0';
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) continue;
        strncat(buf, clients[i].pseudo, sz - strlen(buf) - 2);
        strncat(buf, " ", sz - strlen(buf) - 1);
    }
}

static void disconnect_client(int idx) {
    if (clients[idx].fd <= 0) return;
    char notif[MAX_MSG] = "";
    if (clients[idx].active)
        snprintf(notif, sizeof(notif), "%s a quitte le chat.", clients[idx].pseudo);
    close(clients[idx].fd);
    clients[idx].fd        = -1;
    clients[idx].active    = 0;
    clients[idx].udp_ready = 0;
    if (!notif[0]) return;
    log_event("%s", notif);
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_SYSTEM;
    strncpy(m.from, "SERVER", MAX_PSEUDO - 1);
    strncpy(m.body, notif, MAX_MSG - 1);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active) send_tcp(clients[i].fd, &m);
}

static void *ping_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(PING_INTERVAL);
        time_t now = time(NULL);
        Message ping; memset(&ping, 0, sizeof(ping)); ping.type = MSG_PING;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) continue;
            if (now - clients[i].last_pong > PING_TIMEOUT) {
                log_event("Timeout : %s", clients[i].pseudo);
                disconnect_client(i); continue;
            }
            send_tcp(clients[i].fd, &ping);
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static void handle_message(int idx, const Message *m) {
    Client *c = &clients[idx];
    switch (m->type) {

    case MSG_CONNECT: {
        pthread_mutex_lock(&lock);
        int taken = (find_client(m->from) != NULL);
        if (!taken) {
            strncpy(c->pseudo, m->from, MAX_PSEUDO - 1);
            struct sockaddr_in peer; socklen_t plen = sizeof(peer);
            getpeername(c->fd, (struct sockaddr *)&peer, &plen);
            int udp_port = atoi(m->body);
            if (udp_port > 0) {
                c->udp_addr          = peer;
                c->udp_addr.sin_port = htons((uint16_t)udp_port);
                c->udp_ready         = 1;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                log_event("UDP client %s : %s:%d", m->from, ip, udp_port);
            }
            c->active    = 1;
            c->last_pong = time(NULL);
        }
        pthread_mutex_unlock(&lock);

        if (taken) {
            Message err; memset(&err, 0, sizeof(err));
            err.type = MSG_CONNECT_ERR;
            strncpy(err.body, "Pseudo deja utilise.", MAX_MSG - 1);
            send_tcp(c->fd, &err); return;
        }
        Message ok; memset(&ok, 0, sizeof(ok));
        ok.type = MSG_CONNECT_OK;
        snprintf(ok.body, MAX_MSG, "Bienvenue %s !", c->pseudo);
        send_tcp(c->fd, &ok);

        char notif[MAX_MSG];
        snprintf(notif, sizeof(notif), "%s a rejoint le chat.", c->pseudo);
        log_event("%s", notif);
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].active && clients[i].fd != c->fd)
                send_system(clients[i].fd, notif);
        pthread_mutex_unlock(&lock);
        break;
    }

    case MSG_PUBLIC:
        log_event("[PUBLIC] %s: %s", c->pseudo, m->body);
        pthread_mutex_lock(&lock);
        broadcast_public(m, c->fd);
        pthread_mutex_unlock(&lock);
        break;

    case MSG_PRIVATE: {
        pthread_mutex_lock(&lock);
        Client *dest = find_client(m->to);
        if (dest) {
            send_tcp(dest->fd, m);
            log_event("[PRIVE] %s -> %s", c->pseudo, m->to);
        } else {
            char e[MAX_MSG];
            snprintf(e, sizeof(e), "Utilisateur '%s' introuvable.", m->to);
            send_system(c->fd, e);
        }
        pthread_mutex_unlock(&lock);
        break;
    }

    case MSG_USERS: {
        char list[MAX_MSG] = "Connectes : ";
        pthread_mutex_lock(&lock);
        build_users(list + strlen(list), sizeof(list) - strlen(list));
        pthread_mutex_unlock(&lock);
        send_system(c->fd, list);
        break;
    }

    case MSG_PONG:
        pthread_mutex_lock(&lock);
        c->last_pong = time(NULL);
        pthread_mutex_unlock(&lock);
        break;

    case MSG_DISCONNECT:
        pthread_mutex_lock(&lock);
        disconnect_client(idx);
        pthread_mutex_unlock(&lock);
        break;

    default: break;
    }
}

static void server_loop(void) {
    fd_set fds; int max_fd;
    while (1) {
        FD_ZERO(&fds); FD_SET(tcp_sock, &fds); max_fd = tcp_sock;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd > 0) {
                FD_SET(clients[i].fd, &fds);
                if (clients[i].fd > max_fd) max_fd = clients[i].fd;
            }
        pthread_mutex_unlock(&lock);

        struct timeval tv = {1, 0};
        int ready = select(max_fd + 1, &fds, NULL, NULL, &tv);
        if (ready < 0) { if (errno == EINTR) continue; perror("select"); break; }

        if (FD_ISSET(tcp_sock, &fds)) {
            struct sockaddr_in ca; socklen_t cl = sizeof(ca);
            int nfd = accept(tcp_sock, (struct sockaddr *)&ca, &cl);
            if (nfd >= 0) {
                pthread_mutex_lock(&lock);
                int s = find_free_slot();
                if (s >= 0) {
                    memset(&clients[s], 0, sizeof(Client));
                    clients[s].fd        = nfd;
                    clients[s].last_pong = time(NULL);
                    log_event("Connexion depuis %s:%d (slot %d)",
                              inet_ntoa(ca.sin_addr), ntohs(ca.sin_port), s);
                } else { close(nfd); }
                pthread_mutex_unlock(&lock);
            }
        }

        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd <= 0 || !FD_ISSET(clients[i].fd, &fds)) continue;
            Message m;
            ssize_t n = recv(clients[i].fd, &m, MSG_SIZE, MSG_WAITALL);
            if (n <= 0) { disconnect_client(i); }
            else {
                pthread_mutex_unlock(&lock);
                handle_message(i, &m);
                pthread_mutex_lock(&lock);
            }
        }
        pthread_mutex_unlock(&lock);
    }
}

int main(int argc, char *argv[]) {
    int tcp_port = DEFAULT_TCP_PORT;
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--port") == 0) tcp_port = atoi(argv[i + 1]);
    int udp_port = tcp_port + 1;

    tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) { perror("socket TCP"); exit(1); }
    int opt = 1;
    setsockopt(tcp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in srv = {0};
    srv.sin_family = AF_INET; srv.sin_addr.s_addr = INADDR_ANY;
    srv.sin_port   = htons((uint16_t)tcp_port);
    if (bind(tcp_sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) { perror("bind TCP"); exit(1); }
    if (listen(tcp_sock, BACKLOG) < 0) { perror("listen"); exit(1); }

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) { perror("socket UDP"); exit(1); }
    struct sockaddr_in usrv = {0};
    usrv.sin_family = AF_INET; usrv.sin_addr.s_addr = INADDR_ANY;
    usrv.sin_port   = htons((uint16_t)udp_port);
    if (bind(udp_sock, (struct sockaddr *)&usrv, sizeof(usrv)) < 0) { perror("bind UDP"); exit(1); }

    memset(clients, 0, sizeof(clients));
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    pthread_t pt;
    pthread_create(&pt, NULL, ping_thread, NULL);
    pthread_detach(pt);

    printf("╔══════════════════════════════════════╗\n");
    printf("║       Chat Hub Server demarre        ║\n");
    printf("╠══════════════════════════════════════╣\n");
    printf("║  TCP : %-5d   UDP : %-5d           ║\n", tcp_port, udp_port);
    printf("╚══════════════════════════════════════╝\n");
    fflush(stdout);

    server_loop();
    close(tcp_sock);
    close(udp_sock);
    return 0;
}
