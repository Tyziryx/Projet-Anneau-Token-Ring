/*
 * utils.c — fonctions utilitaires partagées entre driver.c et comm.c
 *
 * Contient :
 *   - FATAL()                   : affiche erreur + quitte
 *   - send_all() / recv_all()   : envoi/réception garantis (boucle TCP)
 *   - send_msg_t() / recv_msg_t(): envoi/réception d'un msg_t complet
 *   - socket_create_inet()      : crée une socket TCP client (connexion vers un serveur)
 *   - socket_create_server()    : crée une socket TCP serveur (écoute sur un port)
 *   - socket_create_unix_server(): crée une socket Unix (comm locale Driver↔Comm)
 *   - table_add/remove/serialize/deserialize/print : gestion table des machines
 *
 * Pour ajouter une nouvelle fonction socket : l'ajouter ici + déclarer dans utils.h
 */

#include "utils.h"

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/un.h>

/* ------------------------------------------------------------------
   FATAL — affiche l'erreur système et quitte immédiatement
   Utilisé partout comme raccourci pour perror() + exit()
   ------------------------------------------------------------------ */
void FATAL(char *msg) {
    perror(msg);
    exit(1);
}

/* ------------------------------------------------------------------
   send_all — envoie exactement `size` octets sur `sock`
   Nécessaire car send() peut envoyer moins que demandé (TCP fragmentation)
   Retourne le nb total envoyé, ou 0 si connexion fermée
   ------------------------------------------------------------------ */
int send_all(int sock, void *buffer, int size) {
    int total = 0;
    int n;
    while (total < size) {
        n = send(sock, (char *)buffer + total, size - total, 0);
        if (n < 0) FATAL("send");
        if (n == 0) return 0;   /* connexion fermée */
        total += n;
    }
    return total;
}

/* ------------------------------------------------------------------
   recv_all — reçoit exactement `size` octets depuis `sock`
   Même raison que send_all : recv() peut recevoir moins que demandé
   Retourne le nb total reçu, ou 0 si connexion fermée
   ------------------------------------------------------------------ */
int recv_all(int sock, char *buffer, int size) {
    int total = 0;
    int n;
    while (total < size) {
        n = recv(sock, (char *)buffer + total, size - total, 0);
        if (n < 0) FATAL("recv");
        if (n == 0) return 0;   /* connexion fermée */
        total += n;
    }
    return total;
}

/* ------------------------------------------------------------------
   send_msg_t — envoie un msg_t complet (sizeof(msg_t) octets)
   Utilisé pour TOUS les échanges : anneau TCP + socket unix Driver↔Comm
   ------------------------------------------------------------------ */
int send_msg_t(int sock, msg_t *msg) {
    return send_all(sock, msg, sizeof(msg_t));
}

/* ------------------------------------------------------------------
   recv_msg_t — reçoit un msg_t complet
   Retourne <= 0 si la connexion est fermée (voisin déconnecté)
   ------------------------------------------------------------------ */
int recv_msg_t(int sock, msg_t *msg) {
    return recv_all(sock, (char *)msg, sizeof(msg_t));
}

/* ------------------------------------------------------------------
   socket_create_inet — crée une socket TCP et se connecte à hostname:port
   Utilisé par :
     - driver.c choix 2 : Mn se connecte à M1 (futur sock_droite)
     - driver.c JOIN_CMD : M(n-1) se reconnecte vers Mn
   ------------------------------------------------------------------ */
int socket_create_inet(const char *hostname, int port) {
    struct hostent *hp;
    struct sockaddr_in serveur;
    int sock, cc;

    hp = gethostbyname(hostname);
    if (hp == NULL) { FATAL("gethostbyname"); return -1; }

    memset(&serveur, 0, sizeof(serveur));
    bcopy(hp->h_addr_list[0], (char *)&serveur.sin_addr, hp->h_length);
    serveur.sin_family = AF_INET;
    serveur.sin_port   = htons(port);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { FATAL("socket"); return -1; }

    printf("Connexion à %s:%d...\n", hostname, port);
    cc = connect(sock, (struct sockaddr *)&serveur, sizeof(serveur));
    if (cc == -1) { FATAL("connect"); close(sock); return -1; }
    printf("Connecté !\n");

    return sock;
}

/* ------------------------------------------------------------------
   socket_create_server — crée une socket TCP en écoute sur `port`
   Utilisé par :
     - driver.c choix 1 : M1 attend M2 (et futurs joins)
     - driver.c choix 2 : Mn crée son serveur pour recevoir M(n-1)
   Retourne le fd de la socket d'écoute (server_sock dans driver.c)
   ------------------------------------------------------------------ */
int socket_create_server(int port) {
    struct sockaddr_in serveur;
    int sock;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { FATAL("socket"); return -1; }

    /* SO_REUSEADDR évite "Address already in use" après un crash */
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&serveur, 0, sizeof(serveur));
    serveur.sin_family      = AF_INET;
    serveur.sin_port        = htons(port);
    serveur.sin_addr.s_addr = INADDR_ANY;   /* accepte sur toutes les interfaces */

    if (bind(sock, (struct sockaddr *)&serveur, sizeof(serveur)) < 0) {
        FATAL("bind"); close(sock); return -1;
    }
    if (listen(sock, 5) < 0) {
        FATAL("listen"); close(sock); return -1;
    }

    printf("Serveur en écoute sur port %d...\n", port);
    return sock;
}

/* ------------------------------------------------------------------
   socket_create_unix_server — crée une socket Unix (AF_UNIX SOCK_STREAM)
   Utilisé par driver.c pour la communication locale avec comm.c
   Le chemin est /tmp/ring_<port> (unique par machine)
   Repris de unix/serveurtcp.c du TD
   ------------------------------------------------------------------ */
int socket_create_unix_server(const char *path) {
    int sock;
    struct sockaddr_un addr;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { FATAL("socket unix"); return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    unlink(path);   /* supprime le fichier socket s'il existait déjà */

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        FATAL("bind unix"); close(sock); return -1;
    }
    if (listen(sock, 5) < 0) {
        FATAL("listen unix"); close(sock); return -1;
    }

    return sock;
}

/* ==================================================================
   HELPERS TABLE DES MACHINES
   Utilisés dans driver.c (mise à jour) et comm.c (affichage)
   Si on voulait les isoler : créer table.h / table.c
   ================================================================== */

/* ------------------------------------------------------------------
   table_add — ajoute une machine si elle n'est pas déjà présente
   Identifié par port (= ID unique)
   ------------------------------------------------------------------ */
void table_add(machine_t *t, int *nb, int port,
               const char *ip, const char *hostname, int is_master) {
    if (*nb >= MAX_MACHINES) return;
    for (int i = 0; i < *nb; i++)
        if (t[i].port == port) return;   /* déjà présente, rien à faire */
    t[*nb].port      = port;
    t[*nb].is_master = is_master;
    strncpy(t[*nb].ip,       ip,       INET_ADDRSTRLEN);
    strncpy(t[*nb].hostname, hostname, HOSTNAME_LEN);
    (*nb)++;
}

/* ------------------------------------------------------------------
   table_remove — supprime une machine par son port
   Utilisé lors d'une déconnexion (à venir)
   ------------------------------------------------------------------ */
void table_remove(machine_t *t, int *nb, int port) {
    for (int i = 0; i < *nb; i++) {
        if (t[i].port == port) {
            /* décale tout ce qui suit d'une case vers la gauche */
            memmove(&t[i], &t[i+1], (*nb - i - 1) * sizeof(machine_t));
            (*nb)--;
            return;
        }
    }
}

/* ------------------------------------------------------------------
   table_serialize — sérialise la table dans buf (format binaire)
   Format : [int nb | machine_t x nb | char self_ip INET_ADDRSTRLEN]
   self_ip = IP de la machine destinataire, vue par l'émetteur
   Retourne le nombre d'octets écrits dans buf
   ------------------------------------------------------------------ */
int table_serialize(machine_t *t, int nb, const char *self_ip, char *buf) {
    int off = 0;
    memcpy(buf + off, &nb, sizeof(int));
    off += sizeof(int);
    memcpy(buf + off, t, nb * sizeof(machine_t));
    off += nb * sizeof(machine_t);
    strncpy(buf + off, self_ip, INET_ADDRSTRLEN);
    off += INET_ADDRSTRLEN;
    return off;
}

/* ------------------------------------------------------------------
   table_deserialize — reconstruit la table depuis buf
   self_ip (optionnel, peut être NULL) : IP propre de cette machine
   ------------------------------------------------------------------ */
void table_deserialize(const char *buf, machine_t *t, int *nb, char *self_ip) {
    int off = 0;
    memcpy(nb, buf + off, sizeof(int));
    off += sizeof(int);
    if (*nb < 0 || *nb > MAX_MACHINES) *nb = 0;
    memcpy(t, buf + off, *nb * sizeof(machine_t));
    off += *nb * sizeof(machine_t);
    if (self_ip)
        strncpy(self_ip, buf + off, INET_ADDRSTRLEN);
}

/* ------------------------------------------------------------------
   table_print — affiche la table sous forme de tableau
   Appelé dans driver.c à chaque mise à jour, et dans comm.c (Récupérer)
   ------------------------------------------------------------------ */
void table_print(machine_t *t, int nb) {
    printf("\n+--------------------------------+------------------+--------+--------+----------+\n");
    printf("| %-30s | %-16s | %-6s | %-6s | %-8s |\n",
           "MACHINE", "ADRESSE", "PORT E", "PORT S", "ROLE");
    printf("+--------------------------------+------------------+--------+--------+----------+\n");
    for (int i = 0; i < nb; i++) {
        /* port_s == 0 → pas encore connu (avant le premier TABLE_UPDATE complet) */
        char port_s_str[8];
        if (t[i].port_s > 0) snprintf(port_s_str, sizeof(port_s_str), "%d", t[i].port_s);
        else                  snprintf(port_s_str, sizeof(port_s_str), "?");

        printf("| %-30s | %-16s | %-6d | %-6s | %-8s |\n",
               t[i].hostname, t[i].ip, t[i].port,
               port_s_str,
               t[i].is_master ? "MAITRE" : "");
    }
    printf("+--------------------------------+------------------+--------+--------+----------+\n");
    printf("  %d machine(s) dans l'anneau\n\n", nb);
}
