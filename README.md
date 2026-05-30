# Chat Hub — Application de chat centralisée

Architecture client-serveur en C avec TCP + UDP.

## Dépendances

- GCC ≥ 7
- POSIX threads (`-pthread`)
- Linux / macOS (ou WSL sous Windows)

## Compilation

```bash
make          # compile server + client dans bin/
make clean    # supprime les binaires
```

## Exécution

**Serveur** (terminal 1) :
```bash
./bin/chat_serverd --port 5555
```

**Client** (terminal 2, 3, …) :
```bash
./bin/chat_client --server 127.0.0.1 --port 5555
```

## Commandes client

| Commande | Description |
|---|---|
| `<texte>` | Message public (salon général) |
| `/msg <pseudo> <texte>` | Message privé |
| `/users` | Liste des connectés |
| `/quit` | Déconnexion propre |

## Protocole de communication

### Format des messages

Structure binaire fixe de **577 octets** :

```
| type (1o) | from (32o) | to (32o) | body (512o) |
```

### Types de messages

| Type | Valeur | Transport | Description |
|---|---|---|---|
| `MSG_CONNECT` | 1 | TCP | Demande de connexion avec pseudo |
| `MSG_CONNECT_OK` | 2 | TCP | Pseudo accepté |
| `MSG_CONNECT_ERR` | 3 | TCP | Pseudo refusé |
| `MSG_PUBLIC` | 4 | UDP | Message salon général |
| `MSG_PRIVATE` | 5 | TCP | Message privé |
| `MSG_SYSTEM` | 6 | TCP | Notification système |
| `MSG_USERS` | 7 | TCP | Liste des connectés |
| `MSG_DISCONNECT` | 8 | TCP | Déconnexion propre |
| `MSG_PING` | 9 | TCP | Keepalive ping |
| `MSG_PONG` | 10 | TCP | Keepalive pong |

### Ports

- **TCP 5555** : connexions, auth, privés, keepalive
- **UDP 5556** : broadcasts salon général (port TCP + 1)

## Architecture

```
┌─────────────────────────────────────────────┐
│                  SERVEUR                    │
│                                             │
│  select() ──► TCP 5555 (connexions)         │
│           ──► clients[i].fd (données TCP)   │
│                                             │
│  Thread keepalive : ping toutes les 15s     │
│  Timeout déconnexion : 30s sans pong        │
└──────────────┬──────────────┬───────────────┘
               │ TCP          │ UDP broadcast
    ┌──────────▼───┐    ┌─────▼──────────┐
    │   Client 1   │    │   Client 2     │
    │  (Kamal)     │    │   (Said)       │
    │              │    │                │
    │ Thread recv  │    │  Thread recv   │
    │ + stdin loop │    │  + stdin loop  │
    └──────────────┘    └────────────────┘
```

## Choix techniques

- **C** : contrôle bas niveau des sockets, performances, adapté aux cours réseau
- **TCP** pour tout ce qui nécessite fiabilité (auth, privés, notifs)
- **UDP** pour les broadcasts du salon général (latence réduite, perte acceptable)
- **select()** côté serveur : pas de thread par client, pas de deadlock possible
- **2 threads côté client** : stdin non bloquant + réception simultanée
- **Ping/Pong** : détection des déconnexions brutales toutes les 15 s

## Scénarios de test

1. **Pseudo déjà pris** : deux clients tentent le même pseudo → le second reçoit une erreur
2. **Message public** : Kamal envoie "Bonjour" → Said le reçoit en UDP
3. **Message privé** : `/msg Said Salut` → seul Said reçoit le message
4. **Déconnexion propre** : `/quit` → les autres reçoivent une notification
5. **Déconnexion brutale** : `kill -9` du client → le serveur détecte le timeout après 30 s
6. **Serveur plein** : 64 clients connectés → le 65e est refusé
