#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* ─── Ports ─────────────────────────────────────────── */
#define DEFAULT_TCP_PORT  5555
#define DEFAULT_UDP_PORT  5556
#define MAX_PSEUDO        32
#define MAX_MSG           512
#define MAX_CLIENTS       64

/* ─── Types de messages ─────────────────────────────── */
typedef enum {
    MSG_CONNECT      = 1,   /* client → serveur : demande de connexion  */
    MSG_CONNECT_OK   = 2,   /* serveur → client : pseudo accepté        */
    MSG_CONNECT_ERR  = 3,   /* serveur → client : pseudo refusé         */
    MSG_PUBLIC       = 4,   /* broadcast UDP : message salon général    */
    MSG_PRIVATE      = 5,   /* TCP : message privé                      */
    MSG_SYSTEM       = 6,   /* TCP : notification système               */
    MSG_USERS        = 7,   /* TCP : liste des connectés                */
    MSG_DISCONNECT   = 8,   /* TCP : déconnexion propre                 */
    MSG_PING         = 9,   /* TCP : keepalive ping                     */
    MSG_PONG         = 10   /* TCP : keepalive pong                     */
} MsgType;

/* ─── Structure d'un message (format réseau) ─────────── */
/*
 * Format binaire fixe :
 *   [type:1][from:32][to:32][body:512]  → 577 octets max
 *   'to' vide = broadcast / système
 */
typedef struct {
    uint8_t  type;
    char     from[MAX_PSEUDO];
    char     to[MAX_PSEUDO];
    char     body[MAX_MSG];
} Message;

#define MSG_SIZE sizeof(Message)

#endif /* PROTOCOL_H */
