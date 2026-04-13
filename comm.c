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

int main(int argc, char *argv[]) {

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port_driver>\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);   /* ID de cette machine = port du driver local */

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
       Etat de réception de fichier (un seul transfert entrant à la fois)
       Rempli par FILE_START, utilisé par FILE_DATA/FILE_END
       ------------------------------------------------------------------ */
    FILE *recv_file         = NULL;
    char  recv_name[256]    = {0};
    long  recv_total        = 0;
    long  recv_got          = 0;

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
                /* Réponse au TABLE_REQ : désérialise et affiche la table */
                machine_t t[MAX_MACHINES];
                int nb = 0;
                char dummy[INET_ADDRSTRLEN];
                table_deserialize(msg.data, t, &nb, dummy);
                printf("\n");
                table_print(t, nb);

            /* ----------------------------------------------------------------
               Transfert de fichier entrant (poussé par le driver quand dest==moi)
               FILE_START : ouvre le fichier local en écriture
               FILE_DATA  : écrit le chunk reçu
               FILE_END   : ferme le fichier et confirme
               ---------------------------------------------------------------- */
            } else if (msg.type == FILE_START) {
                strncpy(recv_name, msg.data, sizeof(recv_name) - 1);
                recv_total = msg.size;   /* taille totale mise dans FILE_START.size */
                recv_got   = 0;
                recv_file  = fopen(recv_name, "wb");
                if (!recv_file)
                    printf("\n[FICHIER] Impossible de creer '%s'\n", recv_name);
                else
                    printf("\n[FICHIER] Reception de '%s' (%ld octets)...\n",
                           recv_name, recv_total);

            } else if (msg.type == FILE_DATA) {
                if (recv_file) {
                    fwrite(msg.data, 1, msg.size, recv_file);
                    recv_got += msg.size;
                }

            } else if (msg.type == FILE_END) {
                if (recv_file) {
                    fclose(recv_file);
                    recv_file = NULL;
                    printf("\n[FICHIER] '%s' recu completement (%ld octets)\n",
                           recv_name, recv_got);
                }
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
                   Emettre — envoie un TEXT vers une machine spécifique
                   Le driver stocke le message et l'envoie au prochain token
                   -------------------------------------------------------- */
                int dest;
                char texte[SMAX];
                printf("Destination (port): ");
                scanf("%d", &dest);
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
                   Transferer un fichier — envoie FILE_START + chunks + FILE_END
                   Le driver tient le token pendant tout le transfert (Token Ring)
                   msg.size dans FILE_START = taille totale du fichier
                   -------------------------------------------------------- */
                int dest;
                char filename[256];
                printf("Destination (port): ");
                scanf("%d", &dest);
                printf("Fichier a envoyer: ");
                scanf(" %255s", filename);

                FILE *f = fopen(filename, "rb");
                if (!f) {
                    printf("Erreur: fichier '%s' introuvable\n", filename);
                    continue;
                }

                /* Calcule la taille du fichier */
                fseek(f, 0, SEEK_END);
                long fsz = ftell(f);
                rewind(f);

                /* FILE_START : filename dans data, taille totale dans msg.size */
                msg_t fmsg = create_file_start(port, dest, filename);
                fmsg.size = (int)fsz;
                send_msg_t(sock, &fmsg);

                /* Envoie les chunks FILE_DATA (taille max SMAX par chunk) */
                char buf[SMAX];
                int n, chunks = 0;
                while ((n = (int)fread(buf, 1, SMAX, f)) > 0) {
                    fmsg = create_file_data(port, dest, buf, n);
                    send_msg_t(sock, &fmsg);
                    chunks++;
                }
                fclose(f);

                /* FILE_END : signale la fin du transfert */
                fmsg = create_file_end(port, dest);
                send_msg_t(sock, &fmsg);

                printf("Transfert de '%s' lance (%ld octets, %d blocs, dest=%d)\n",
                       filename, fsz, chunks, dest);

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
