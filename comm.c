/*
 * comm.c — interface utilisateur (un par machine, tourne en parallèle du driver)
 *
 * Rôle : permet à l'utilisateur d'envoyer des messages, diffuser, voir la table.
 *        Communique avec driver.c via une socket Unix locale (/tmp/ring_<port>)
 *        Repris de unix/clienttcp.c pour la connexion Unix.
 *
 * Usage : ./comm <port_driver>
 *   port_driver = port d'écoute du driver sur cette machine (= son ID)
 *
 * Communication avec le driver :
 *   Comm → Driver : msg_t (TEXT, BROADCAST, TABLE_REQ)
 *   Driver → Comm : msg_t (TEXT, BROADCAST, TABLE_UPDATE)
 *
 * Boucle principale : select() sur stdin + socket unix
 *   → permet de recevoir des messages entrants PENDANT que l'utilisateur tape
 *
 * Fonctions candidates à extraire :
 *   - connect_to_driver()  : bloc de connexion Unix
 *   - afficher_message()   : affichage TEXT/BROADCAST
 *   - afficher_table()     : affichage TABLE_UPDATE
 *   - menu_emettre()       : saisie + envoi TEXT
 *   - menu_diffuser()      : saisie + envoi BROADCAST
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>

#include "protocole.h"
#include "utils.h"

/* Etat de reception fichier — global pour etre partage entre on_file_* et main */
static FILE *recv_file      = NULL;
static char  recv_name[256] = {0};
static long  recv_total     = 0;
static long  recv_got       = 0;

/* resout une IP saisie en port destination via la table locale
   retourne -1 si IP inconnue (et affiche un message d'erreur) */
int resolve_dest_port(machine_t *table, int nb, const char *dest_ip) {
    for (int i = 0; i < nb; i++) {
        if (strcmp(table[i].ip, dest_ip) == 0) return table[i].port;
    }
    printf("Erreur: IP '%s' inconnue (faites Recuperer d'abord)\n", dest_ip);
    return -1;
}

/* envoie un fichier sur le socket driver : FILE_START + chunks FILE_DATA + FILE_END
   inspire de transfer() du TD2 (transfer fichier client) */
void send_file(int sock, int my_port, int dest, const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) { printf("Erreur: fichier '%s' introuvable\n", filename); return; }

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);

    /* FILE_START porte le nom dans data et la taille totale dans size */
    msg_t fmsg = create_file_start(my_port, dest, filename);
    fmsg.size  = (int)fsz;
    send_msg_t(sock, &fmsg);

    /* chunks FILE_DATA (max SMAX octets par envoi) */
    char buf[SMAX];
    int n, chunks = 0;
    while ((n = (int)fread(buf, 1, SMAX, f)) > 0) {
        fmsg = create_file_data(my_port, dest, buf, n);
        send_msg_t(sock, &fmsg);
        chunks++;
    }
    fclose(f);

    fmsg = create_file_end(my_port, dest);
    send_msg_t(sock, &fmsg);

    printf("Transfert de '%s' lance (%ld octets, %d blocs, dest=%d)\n",
           filename, fsz, chunks, dest);
}

/* handlers de reception fichier — appeles depuis la boucle main quand FILE_* arrive */
void on_file_start(msg_t *msg) {
    strncpy(recv_name, msg->data, sizeof(recv_name) - 1);
    recv_total = msg->size;
    recv_got   = 0;
    recv_file  = fopen(recv_name, "wb");
    if (!recv_file)
        printf("\n[FICHIER] Impossible de creer '%s'\n", recv_name);
    else
        printf("\n[FICHIER] Reception de '%s' (%ld octets)...\n",
               recv_name, recv_total);
}

void on_file_data(msg_t *msg) {
    if (recv_file) {
        fwrite(msg->data, 1, msg->size, recv_file);
        recv_got += msg->size;
    }
}

void on_file_end(void) {
    if (recv_file) {
        fclose(recv_file);
        recv_file = NULL;
        printf("\n[FICHIER] '%s' recu completement (%ld octets)\n",
               recv_name, recv_got);
    }
}

int main(int argc, char *argv[]) {

    int port;

    if (argc == 2) {
        port = atoi(argv[1]);
    } else if (argc == 1) {
        /* Auto-détection : lit le port depuis /tmp/ring_local (écrit par driver.c) */
        FILE *fp = fopen("/tmp/ring_local", "r");
        if (!fp) {
            fprintf(stderr, "Erreur: driver non demarre (pas de /tmp/ring_local)\n");
            fprintf(stderr, "Usage: %s <port_driver>\n", argv[0]);
            exit(1);
        }
        if (fscanf(fp, "%d", &port) != 1) {
            fclose(fp); fprintf(stderr, "Erreur lecture /tmp/ring_local\n"); exit(1);
        }
        fclose(fp);
        printf("(port auto-detecte: %d)\n", port);
    } else {
        fprintf(stderr, "Usage: %s [port_driver]\n", argv[0]);
        exit(1);
    }

    /* ==================================================================
       CONNEXION AU DRIVER — socket Unix AF_UNIX SOCK_STREAM
       Chemin : /tmp/ring_<port> (créé par driver.c au démarrage)
       Repris de unix/clienttcp.c
       ================================================================== */
    char unix_path[64];
    sprintf(unix_path, "/tmp/ring_%d", port);

    int sock;
    struct sockaddr_un addr;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) FATAL("socket");

    bzero(&addr, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, unix_path);

    printf("Connexion au driver (port %d)...\n", port);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        FATAL("connect — driver lance ? (./driver doit tourner d'abord)");
    printf("Connecte !\n");

    /* ==================================================================
       BOUCLE PRINCIPALE — select() sur stdin + socket driver
       Pattern repris de td5 : FD_ZERO, FD_SET, select, FD_ISSET
       Permet de recevoir des messages entrants sans bloquer sur stdin
       ================================================================== */
    fd_set readfds;
    int max_fd;
    msg_t msg;

    /* ------------------------------------------------------------------
       Table locale des machines — mise à jour automatique à chaque TABLE_UPDATE
       Permet d'envoyer des messages par IP plutôt que par port
       ------------------------------------------------------------------ */
    machine_t local_table[MAX_MACHINES];
    int local_nb = 0;

    /* L'etat de reception fichier (recv_file/name/total/got) est en global,
       voir le haut du fichier — manipule par on_file_start / on_file_data / on_file_end */

    printf("\n===== COMM ANNEAU (ID=%d) =====\n", port);

    while (1) {

        printf("\n1. Emettre\n2. Diffuser\n3. Recuperer\n4. Transferer fichier\n5. Quitter\nChoix: ");
        fflush(stdout);

        FD_ZERO(&readfds);
        FD_SET(0, &readfds);      /* stdin : saisie utilisateur */
        FD_SET(sock, &readfds);   /* socket unix : messages du driver */
        max_fd = sock;

        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) { perror("select"); break; }

        /* ----------------------------------------------------------------
           Message reçu depuis le driver (vient de l'anneau)
           - TEXT        : message direct d'une autre machine
           - BROADCAST   : message diffusé par une autre machine
           - TABLE_UPDATE: réponse à un TABLE_REQ (option 3)
           ---------------------------------------------------------------- */
        if (FD_ISSET(sock, &readfds)) {
            int n = recv_msg_t(sock, &msg);
            if (n <= 0) { printf("\nDriver deconnecte\n"); break; }

            if (msg.type == TEXT) {
                printf("\n[MESSAGE de %d]: %s\n", msg.source, msg.data);

            } else if (msg.type == BROADCAST) {
                printf("\n[BROADCAST de %d]: %s\n", msg.source, msg.data);

            } else if (msg.type == TABLE_UPDATE) {
                /* Met à jour la table locale et l'affiche */
                char dummy[INET_ADDRSTRLEN];
                table_deserialize(msg.data, local_table, &local_nb, dummy);
                printf("\n");
                table_print(local_table, local_nb);

            /* ----------------------------------------------------------------
               Transfert de fichier entrant — handlers extraits en haut du fichier
               (style transfer fichier TD : un on_file_start / data / end)
               ---------------------------------------------------------------- */
            } else if (msg.type == FILE_START) {
                on_file_start(&msg);
            } else if (msg.type == FILE_DATA) {
                on_file_data(&msg);
            } else if (msg.type == FILE_END) {
                on_file_end();
            }
        }

        /* ----------------------------------------------------------------
           Saisie utilisateur depuis stdin
           ---------------------------------------------------------------- */
        if (FD_ISSET(0, &readfds)) {
            int choix;
            if (scanf("%d", &choix) != 1) {
                /* Entrée invalide : vide le buffer et réaffiche le menu */
                int c; while ((c = getchar()) != '\n' && c != EOF);
                continue;
            }

            if (choix == 1) {
                /* --------------------------------------------------------
                   Emettre — envoie un TEXT vers une machine par IP
                   Si la table locale est disponible : cherche le port par IP
                   Sinon : saisie directe du port (fallback)
                   -------------------------------------------------------- */
                char texte[SMAX];
                int dest = -1;

                if (local_nb > 0) {
                    char dest_ip[INET_ADDRSTRLEN];
                    printf("Destination (IP): ");
                    scanf("%s", dest_ip);
                    dest = resolve_dest_port(local_table, local_nb, dest_ip);
                    if (dest == -1) continue;
                    printf("(port %d)\n", dest);
                } else {
                    printf("Destination (port): ");
                    scanf("%d", &dest);
                }

                printf("Message: ");
                scanf(" %[^\n]", texte);

                msg = create_msg_TEXT(port, dest, texte);
                send_msg_t(sock, &msg);
                printf("Message envoye vers %d (en attente du token...)\n", dest);

            } else if (choix == 2) {
                /* --------------------------------------------------------
                   Diffuser — envoie un BROADCAST à toutes les machines
                   Le message fait le tour complet de l'anneau
                   -------------------------------------------------------- */
                char texte[SMAX];
                printf("Message: ");
                scanf(" %[^\n]", texte);

                msg = create_msg_BROADCAST(port, texte);
                send_msg_t(sock, &msg);
                printf("Broadcast envoye (en attente du token...)\n");

            } else if (choix == 3) {
                /* --------------------------------------------------------
                   Récupérer — demande la table au driver local (TABLE_REQ)
                   Le driver répond directement sans passer par l'anneau
                   La réponse TABLE_UPDATE sera reçue dans FD_ISSET(sock)
                   -------------------------------------------------------- */
                msg_t req;
                req.type    = TABLE_REQ;
                req.source  = port;
                req.dest    = -1;
                req.size    = 0;
                req.data[0] = '\0';
                send_msg_t(sock, &req);

            } else if (choix == 4) {
                /* --------------------------------------------------------
                   Transferer un fichier — delegue a send_file() en haut du fichier
                   Le driver tient le token pendant tout le transfert (Token Ring)
                   -------------------------------------------------------- */
                int dest = -1;
                char filename[256];

                if (local_nb > 0) {
                    char dest_ip[INET_ADDRSTRLEN];
                    printf("Destination (IP): ");
                    scanf("%s", dest_ip);
                    dest = resolve_dest_port(local_table, local_nb, dest_ip);
                    if (dest == -1) continue;
                    printf("(port %d)\n", dest);
                } else {
                    printf("Destination (port): ");
                    scanf("%d", &dest);
                }
                printf("Fichier a envoyer: ");
                scanf(" %255s", filename);

                send_file(sock, port, dest, filename);

            } else if (choix == 5) {
                /* --------------------------------------------------------
                   Quitter — envoie LEAVE_CMD au driver pour sortir proprement
                   Le driver gère le protocole LEAVE sur l'anneau
                   Comm se déconnecte immédiatement après l'envoi
                   -------------------------------------------------------- */
                msg_t lmsg;
                lmsg.type    = LEAVE_CMD;
                lmsg.source  = port;
                lmsg.dest    = -1;
                lmsg.size    = 0;
                lmsg.data[0] = '\0';
                send_msg_t(sock, &lmsg);
                printf("Deconnexion en cours...\n");
                break;
            }
        }
    }

    close(sock);
    return 0;
}
