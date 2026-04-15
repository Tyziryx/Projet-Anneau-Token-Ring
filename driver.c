/*
 * driver.c — processus de gestion de l'anneau (un par machine)
 *
 * Rôle : gère la topologie de l'anneau, fait circuler le token,
 *        route les messages, coordonne les joins dynamiques.
 *        Communique avec comm.c via une socket Unix locale.
 *
 * Architecture des sockets :
 *
 *   sock_gauche  ← reçoit les messages du voisin gauche (depuis l'anneau)
 *   sock_droite  → envoie les messages vers le voisin droit (vers l'anneau)
 *   server_sock  → écoute les nouvelles connexions :
 *                    - M1 : attend M2 puis les joins dynamiques
 *                    - Mn : attend M(n-1) pendant le protocole JOIN
 *   unix_listen  → écoute comm.c (socket locale /tmp/ring_<port>)
 *   unix_client  → socket connectée avec comm.c (une seule à la fois)
 *   join_sock    → connexion temporaire de Mn vers M1 pendant un join
 *                  (devient le nouveau sock_gauche de M1 après JOIN_DONE)
 *
 * Flux du token :
 *   M1 génère le token → sock_droite → M2 → ... → Mn → sock_gauche de M1
 *   À chaque passage : si message en attente de Comm → l'envoyer d'abord
 *
 * Protocole de join (N machines) :
 *   voir protocole.h et les commentaires dans la boucle principale
 *
 * Fonctions candidates à extraire (si le fichier grossit) :
 *   - init_as_M1()       : tout le bloc choix==1
 *   - init_as_Mn()       : tout le bloc choix==2
 *   - handle_token()     : gestion TOKEN dans la boucle
 *   - handle_join_cmd()  : gestion JOIN_CMD dans la boucle
 *   - handle_join_done() : gestion JOIN_DONE côté M1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/un.h>
#include <ifaddrs.h>
#include <time.h>

#include "protocole.h"
#include "utils.h"

int main(int argc, char *argv[]) {

    int choix;
    int port_ecoute        = -1;   /* ID de cette machine = son port d'écoute      */
    int server_sock        = -1;   /* socket d'écoute TCP (reste ouvert pour joins) */
    int sock_gauche        = -1;   /* reçoit depuis le voisin gauche               */
    int sock_droite        = -1;   /* envoie vers le voisin droit                  */
    int port_voisin_droite = -1;   /* port du voisin actuel à droite               */
                                   /* sert à identifier M(n-1) dans JOIN_CMD       */
    char ip_voisin_droite[100]       = {0};
    char self_ip[INET_ADDRSTRLEN]    = {0};
    char self_hostname[HOSTNAME_LEN] = {0};

    /* Table locale des machines de l'anneau */
    machine_t table[MAX_MACHINES];
    int nb_machines = 0;

    /* ------------------------------------------------------------------
       Numéro de séquence du token — M1 uniquement
       token_seq : incrémenté à chaque création/recréation du token
       Sert à absorber les tokens obsolètes si deux tokens circulent
       (ex: M1 régénère après timeout, l'ancien token revient aussi)
       Champ utilisé : msg.size dans les messages TOKEN (data = vide)
       ------------------------------------------------------------------ */
    int token_seq  = 0;
    int has_leaving = 0;    /* 1 = cette machine est en train de quitter l'anneau */

    /* ------------------------------------------------------------------
       Variables de join — utilisées par M1 uniquement
       join_pending : une machine a demandé à joindre, on attend le token
       join_waiting : JOIN_CMD envoyé, on attend JOIN_DONE sur join_sock
       join_sock    : connexion de Mn → deviendra le nouveau sock_gauche
       ------------------------------------------------------------------ */
    int    join_pending  = 0;
    int    join_waiting  = 0;
    int    join_sock     = -1;
    char   join_ip[INET_ADDRSTRLEN]    = {0};
    char   join_hostname[HOSTNAME_LEN] = {0};
    int    join_port     = -1;
    time_t join_start    = 0;     /* timestamp du début du join — pour timeout 10s */

    /* Récupère le hostname */
    char tmp_host[256];
    gethostname(tmp_host, sizeof(tmp_host));
    strncpy(self_hostname, tmp_host, HOSTNAME_LEN - 1);

    /* Récupère la vraie IP réseau via getifaddrs (évite 127.0.1.1 sur Ubuntu)
       On prend la première interface non-loopback avec une adresse IPv4 */
    struct ifaddrs *ifas, *ifa;
    if (getifaddrs(&ifas) == 0) {
        for (ifa = ifas; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
            if (strncmp(buf, "127.", 4) != 0) {   /* ignore loopback */
                strncpy(self_ip, buf, INET_ADDRSTRLEN - 1);
                break;
            }
        }
        freeifaddrs(ifas);
    }
    if (self_ip[0] == '\0') strcpy(self_ip, "127.0.0.1");

    printf("===== DRIVER ANNEAU =====\n");
    printf("1. Creer anneau (premier PC)\n");
    printf("2. Rejoindre anneau\n");
    printf("Choix: ");
    scanf("%d", &choix);

    /* ==================================================================
       INITIALISATION — choix 1 : M1 (coordinateur, crée le premier anneau)
       ==================================================================
       M1 attend M2, reçoit son port+hostname, lui envoie la table,
       puis se reconnecte à M2 pour fermer l'anneau.
       server_sock RESTE OUVERT après l'init pour les joins dynamiques.
       Si on voulait extraire : init_as_M1()
    */
    if (choix == 1) {

        printf("Port d'ecoute: ");
        scanf("%d", &port_ecoute);

        /* M1 s'ajoute à sa propre table (is_master=1) */
        table_add(table, &nb_machines, port_ecoute, self_ip, self_hostname, 1);

        while ((server_sock = socket_create_server(port_ecoute)) < 0) {
            printf("Port %d deja utilise. Choisissez un autre port: ", port_ecoute);
            scanf("%d", &port_ecoute);
        }

        printf("En attente du premier voisin...\n");

        /* Accepte la connexion de M2, récupère son IP via accept() */
        struct sockaddr_in cli;
        socklen_t lg = sizeof(cli);
        sock_gauche = accept(server_sock, (struct sockaddr *)&cli, &lg);
        if (sock_gauche < 0) FATAL("accept");
        printf("Connexion recue !\n");

        /* Handshake : M2 envoie flag 'J' + port + hostname */
        int port_m2;
        char hostname_m2[HOSTNAME_LEN];
        char iflag;
        if (recv_all(sock_gauche, &iflag, 1) <= 0) FATAL("recv flag init");
        if (recv_all(sock_gauche, (char *)&port_m2, sizeof(int)) <= 0)
            FATAL("recv port_m2");
        port_m2 = ntohl(port_m2);
        if (recv_all(sock_gauche, hostname_m2, HOSTNAME_LEN) <= 0)
            FATAL("recv hostname_m2");

        char ip_m2[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip_m2, sizeof(ip_m2));

        /* Envoie à M2 : table courante + IP propre de M2 (vue par M1) */
        msg_t tmsg;
        tmsg.type   = TABLE_UPDATE;
        tmsg.source = port_ecoute;
        tmsg.dest   = port_m2;
        tmsg.size   = table_serialize(table, nb_machines, ip_m2, tmsg.data);
        send_msg_t(sock_gauche, &tmsg);

        /* Ajoute M2 à la table locale */
        table_add(table, &nb_machines, port_m2, ip_m2, hostname_m2, 0);
        table_print(table, nb_machines);

        /* Ferme l'anneau : M1 se connecte EN RETOUR à M2 */
        printf("Connexion retour vers %s:%d...\n", ip_m2, port_m2);
        sock_droite = socket_create_inet(ip_m2, port_m2);
        if (sock_droite < 0) FATAL("socket_create_inet retour");

        port_voisin_droite = port_m2;
        strcpy(ip_voisin_droite, ip_m2);

    /* ==================================================================
       INITIALISATION — choix 2 : Mn (rejoindre un anneau existant)
       ==================================================================
       Mn se connecte à M1, lui envoie son port+hostname,
       reçoit la table en retour, puis attend que M(n-1) se connecte.
       L'ordre est important : server_sock créé AVANT l'envoi du port
       (évite une race condition si M1 lance JOIN_CMD trop vite)
       Si on voulait extraire : init_as_Mn()
    */
    } else if (choix == 2) {

        char ip_m1[100];
        int  port_m1;

        printf("IP de M1 (coordinateur): ");
        scanf("%s", ip_m1);
        printf("Port de M1: ");
        scanf("%d", &port_m1);
        printf("Mon port d'ecoute: ");
        scanf("%d", &port_ecoute);

        /* Étape 1 : créer le server AVANT d'envoyer le port à M1
           → M1 ne peut pas lancer JOIN_CMD avant que notre server soit prêt */
        while ((server_sock = socket_create_server(port_ecoute)) < 0) {
            printf("Port %d deja utilise. Choisissez un autre port: ", port_ecoute);
            scanf("%d", &port_ecoute);
        }

        /* Étape 2 : connexion vers M1 (ce socket deviendra sock_droite) */
        sock_droite = socket_create_inet(ip_m1, port_m1);
        if (sock_droite < 0) FATAL("socket_create_inet");

        /* Étape 3 : handshake → flag 'J' + port + hostname à M1
           Le flag permet à M1 de distinguer un JOIN d'une reconnexion LEAVE */
        char jflag = 'J';
        if (send_all(sock_droite, &jflag, 1) <= 0) FATAL("send flag");
        int port_net = htonl(port_ecoute);
        if (send_all(sock_droite, &port_net, sizeof(int)) <= 0)
            FATAL("send port_ecoute");
        if (send_all(sock_droite, self_hostname, HOSTNAME_LEN) <= 0)
            FATAL("send hostname");

        /* Étape 4 : reçoit la table depuis M1 (et notre propre IP vue par M1) */
        msg_t tmsg;
        if (recv_msg_t(sock_droite, &tmsg) <= 0) FATAL("recv table");
        char mn_self_ip[INET_ADDRSTRLEN];
        table_deserialize(tmsg.data, table, &nb_machines, mn_self_ip);
        strcpy(self_ip, mn_self_ip);   /* notre IP réelle vue par M1 */

        /* S'ajoute à la table locale (is_master=0, on est Mn) */
        table_add(table, &nb_machines, port_ecoute, self_ip, self_hostname, 0);
        table_print(table, nb_machines);

        port_voisin_droite = port_m1;
        strcpy(ip_voisin_droite, ip_m1);

        /* Étape 5 : attend que M(n-1) se connecte (bloquant)
           → M(n-1) se connecte après avoir reçu JOIN_CMD de M1 */
        printf("En attente que M(n-1) se connecte...\n");
        sock_gauche = accept(server_sock, NULL, NULL);
        if (sock_gauche < 0) FATAL("accept");
        printf("Anneau ferme !\n");

    } else {
        printf("Choix invalide\n");
        exit(EXIT_FAILURE);
    }

    printf("Anneau initialise ! (ID=%d  IP=%s  host=%s)\n",
           port_ecoute, self_ip, self_hostname);

    /* ==================================================================
       SOCKET UNIX — communication locale avec comm.c
       Chemin : /tmp/ring_<port> (unique par machine sur la même machine)
       Driver = serveur Unix, Comm = client Unix (repris de unix/serveurtcp.c)
       ================================================================== */
    char unix_path[64];
    sprintf(unix_path, "/tmp/ring_%d", port_ecoute);
    int unix_listen = socket_create_unix_server(unix_path);
    if (unix_listen < 0) FATAL("socket_create_unix_server");
    printf("[UNIX] En attente de Comm sur %s\n", unix_path);

    /* Ecrit le port dans /tmp/ring_local → comm peut s'y connecter sans argument */
    { FILE *fp = fopen("/tmp/ring_local", "w");
      if (fp) { fprintf(fp, "%d", port_ecoute); fclose(fp); } }

    /* Seul M1 génère le token initial (un seul token dans l'anneau)
       seq=1 : premier token, les tokens avec seq < token_seq seront absorbes */
    if (choix == 1) {
        msg_t token = create_msg_TOKEN(port_ecoute, -1);
        token.size = ++token_seq;   /* seq=1 au démarrage */
        send_msg_t(sock_droite, &token);
        printf("[TOKEN] Jeton genere et envoye (seq=%d)\n", token_seq);
    }

    /* Message en attente d'être envoyé (reçu de Comm, attend le token) */
    msg_t pending_msg;
    int has_pending = 0;
    int unix_client = -1;   /* fd de comm.c une fois connecté (-1 = pas connecté) */

    /* ==================================================================
       BOUCLE PRINCIPALE — select() sur tous les fds actifs
       Pattern repris de td5/serveur.c (FD_ZERO, FD_SET, select, FD_ISSET)

       Fds surveillés :
         sock_gauche  : messages venant de l'anneau
         unix_listen  : nouvelle connexion de Comm
         unix_client  : messages de Comm (après connexion)
         server_sock  : M1 uniquement — nouvelle machine qui veut joindre
         join_sock    : M1 uniquement, pendant un join — attend JOIN_DONE de Mn
       ================================================================== */
    fd_set readfds;
    int max_fd;
    msg_t msg;

    while (1) {

        /* Reconstruit le fd_set à chaque itération (select() le modifie) */
        FD_ZERO(&readfds);

        FD_SET(sock_gauche, &readfds);
        max_fd = sock_gauche;

        FD_SET(unix_listen, &readfds);
        if (unix_listen > max_fd) max_fd = unix_listen;

        if (unix_client > 0) {
            /* Même logique que td2serveur/client : on lit les chunks en boucle directe.
               Si FILE_START attend le token, on NE relit PAS unix_client via select()
               → FILE_DATA/FILE_END restent dans le buffer unix jusqu'au token,
               puis sont lus en synchrone dans le handler TOKEN (comme fread boucle TP). */
            if (!(has_pending && pending_msg.type == FILE_START)) {
                FD_SET(unix_client, &readfds);
                if (unix_client > max_fd) max_fd = unix_client;
            }
        }

        /* Tous les drivers surveillent server_sock :
           - M1 : pour les nouveaux joins (si pas de join en cours)
           - Mn : pour les reconnexions LEAVE (M(n-1) qui se reconnecte après un départ) */
        if (!join_waiting && !has_leaving) {
            FD_SET(server_sock, &readfds);
            if (server_sock > max_fd) max_fd = server_sock;
        }

        /* M1 surveille join_sock pendant qu'on attend JOIN_DONE */
        if (join_waiting && join_sock > 0) {
            FD_SET(join_sock, &readfds);
            if (join_sock > max_fd) max_fd = join_sock;
        }

        /* M1 : timeout pour détecter token perdu (N*3 secondes)
           Pendant join_waiting : timeout 10s pour détecter JOIN_DONE jamais reçu
           Si activity==0 → token perdu ou join bloqué → action corrective */
        struct timeval  tv;
        struct timeval *ptv = NULL;
        if (choix == 1) {
            if (join_waiting) {
                /* Timeout join : combien de temps reste-t-il avant les 10s ? */
                time_t elapsed = time(NULL) - join_start;
                long remaining = 10 - (long)elapsed;
                if (remaining <= 0) remaining = 1;
                tv.tv_sec  = remaining;
                tv.tv_usec = 0;
            } else {
                int n_sec = (nb_machines > 1 ? nb_machines : 2) * 3;
                tv.tv_sec  = n_sec;
                tv.tv_usec = 0;
            }
            ptv = &tv;
        }

        int activity = select(max_fd + 1, &readfds, NULL, NULL, ptv);
        if (activity < 0) { perror("select"); break; }

        /* ----------------------------------------------------------------
           Timeout M1
           - join_waiting : JOIN_DONE pas reçu dans les 10s → rollback
           - sinon : token perdu → régénérer
           ---------------------------------------------------------------- */
        if (activity == 0) {
            if (choix == 1 && join_waiting) {
                /* Rollback join : ferme join_sock, relance le token */
                printf("[JOIN] Timeout — JOIN_DONE non recu, rollback\n");
                close(join_sock); join_sock = -1;
                join_pending = 0; join_waiting = 0;
                msg_t token = create_msg_TOKEN(port_ecoute, -1);
                token.size = ++token_seq;
                send_msg_t(sock_droite, &token);
                printf("[TOKEN] Token relance apres rollback join (seq=%d)\n",
                       token_seq);
            } else {
                token_seq++;
                msg_t token = create_msg_TOKEN(port_ecoute, -1);
                token.size = token_seq;
                send_msg_t(sock_droite, &token);
                printf("[TOKEN] Timeout — token regenere (seq=%d, %d machines)\n",
                       token_seq, nb_machines);
            }
            continue;
        }

        /* ----------------------------------------------------------------
           Comm se connecte sur le socket unix
           ---------------------------------------------------------------- */
        if (FD_ISSET(unix_listen, &readfds)) {
            unix_client = accept(unix_listen, NULL, NULL);
            printf("[UNIX] Comm connecte\n");
            /* Pousse immédiatement la table courante vers comm
               → comm affiche l'état du ring sans que l'utilisateur
               ait besoin de faire "3. Récupérer" manuellement */
            if (nb_machines > 0) {
                msg_t tresp;
                tresp.type   = TABLE_UPDATE;
                tresp.source = port_ecoute;
                tresp.dest   = -1;
                tresp.size   = table_serialize(table, nb_machines,
                                               self_ip, tresp.data);
                send_msg_t(unix_client, &tresp);
                printf("[UNIX] Table poussee vers comm (%d machines)\n",
                       nb_machines);
            }
        }

        /* ----------------------------------------------------------------
           Message reçu de Comm via socket unix
           - TABLE_REQ → répondre directement avec la table (pas via l'anneau)
           - Autre (TEXT/BROADCAST) → stocker, sera envoyé au prochain token
           ---------------------------------------------------------------- */
        if (unix_client > 0 && FD_ISSET(unix_client, &readfds)) {
            int n = recv_msg_t(unix_client, &pending_msg);
            if (n <= 0) {
                printf("[UNIX] Comm deconnecte\n");
                close(unix_client); unix_client = -1; has_pending = 0;
            } else if (pending_msg.type == LEAVE_CMD) {
                /* --------------------------------------------------------
                   Comm veut quitter l'anneau proprement
                   Refuse si anneau à 2 machines (minimum requis)
                   Sinon : envoie LEAVE_CMD sur l'anneau avec l'adresse du
                   voisin droit (pour que M(n-1) sache où se reconnecter)
                   -------------------------------------------------------- */
                if (nb_machines <= 2) {
                    printf("[LEAVE] Refuse : anneau de taille minimale (2 machines)\n");
                } else {
                    has_leaving = 1;
                    msg_t lcmd;
                    lcmd.type   = LEAVE_CMD;
                    lcmd.source = port_ecoute;
                    lcmd.dest   = -1;
                    lcmd.size   = snprintf(lcmd.data, SMAX, "%s %d",
                                          ip_voisin_droite, port_voisin_droite);
                    send_msg_t(sock_droite, &lcmd);
                    close(unix_client); unix_client = -1;
                    printf("[LEAVE] LEAVE_CMD envoye, attente deconnexion...\n");
                }
            } else if (pending_msg.type == TABLE_REQ) {
                /* Répond directement sans passer par l'anneau */
                msg_t tresp;
                tresp.type   = TABLE_UPDATE;
                tresp.source = port_ecoute;
                tresp.dest   = -1;
                tresp.size   = table_serialize(table, nb_machines,
                                               self_ip, tresp.data);
                send_msg_t(unix_client, &tresp);
            } else {
                if (has_pending) {
                    /* Refuse : un message est déjà en file d'attente */
                    printf("[UNIX] Refuse — message deja en attente (dest=%d)\n",
                           pending_msg.dest);
                    /* Notifie comm pour qu'il puisse afficher un message d'erreur */
                    msg_t err;
                    err.type   = TEXT;
                    err.source = port_ecoute;
                    err.dest   = port_ecoute;
                    err.size   = snprintf(err.data, SMAX,
                        "[ERREUR] Un message est deja en attente du token, reessayez");
                    send_msg_t(unix_client, &err);
                } else {
                    /* Stocke le message, sera envoyé quand le token passe */
                    has_pending = 1;
                    printf("[UNIX] Message en attente du token (dest=%d)\n",
                           pending_msg.dest);
                }
            }
        }

        /* ----------------------------------------------------------------
           Nouvelle connexion sur server_sock — JOIN ou reconnexion LEAVE
           Premier byte = flag : 'J' → nouveau membre  'L' → reconnexion LEAVE
           Sans ce flag, M1 lisait type=LEAVE_DONE(13) comme port → garbage
           Pattern repris de td5/serveur.c : accept → recv → dispatch
           ---------------------------------------------------------------- */
        if (!join_waiting && !has_leaving && FD_ISSET(server_sock, &readfds)) {
            struct sockaddr_in scli;
            socklen_t slg = sizeof(scli);
            int new_sock = accept(server_sock, (struct sockaddr *)&scli, &slg);
            if (new_sock >= 0) {
                char conn_flag;
                if (recv_all(new_sock, &conn_flag, 1) <= 0) {
                    close(new_sock);
                } else if (conn_flag == 'L') {
                    /* Reconnexion LEAVE : M(n-1) devient le nouveau voisin gauche */
                    msg_t ldone;
                    if (recv_msg_t(new_sock, &ldone) > 0 && ldone.type == LEAVE_DONE) {
                        printf("[LEAVE] Nouveau voisin gauche (port=%d)\n", ldone.source);
                        close(sock_gauche);
                        sock_gauche = new_sock;
                        if (choix == 1) {
                            /* M1 est M_next : traite directement sans passer par l'anneau */
                            int leaving_port = ldone.dest;
                            table_remove(table, &nb_machines, leaving_port);
                            for (int i = 0; i < nb_machines; i++) {
                                if (table[i].port == port_ecoute) {
                                    table[i].port_s = port_voisin_droite; break;
                                }
                            }
                            table_print(table, nb_machines);
                            printf("[LEAVE_DONE] Machine %d retiree\n", leaving_port);
                            msg_t tupd;
                            tupd.type   = TABLE_UPDATE;
                            tupd.source = port_ecoute;
                            tupd.dest   = -1;
                            tupd.size   = table_serialize(table, nb_machines,
                                                          self_ip, tupd.data);
                            send_msg_t(sock_droite, &tupd);
                            msg_t tok = create_msg_TOKEN(port_ecoute, -1);
                            tok.size = ++token_seq;
                            send_msg_t(sock_droite, &tok);
                            printf("[TOKEN] Token relance apres depart (seq=%d)\n",
                                   token_seq);
                        } else {
                            /* Non-M1 : forward LEAVE_DONE vers M1 via l'anneau */
                            send_msg_t(sock_droite, &ldone);
                        }
                    } else {
                        close(new_sock);
                    }
                } else if (conn_flag == 'J' && choix == 1) {
                    /* JOIN : nouvelle machine — M1 uniquement */
                    join_sock = new_sock;
                    inet_ntop(AF_INET, &scli.sin_addr, join_ip, sizeof(join_ip));
                    int jport_net;
                    if (recv_all(join_sock, (char *)&jport_net, sizeof(int)) <= 0
                     || recv_all(join_sock, join_hostname, HOSTNAME_LEN) <= 0) {
                        close(join_sock); join_sock = -1;
                    } else {
                        join_port = ntohl(jport_net);
                        msg_t tmsg;
                        tmsg.type   = TABLE_UPDATE;
                        tmsg.source = port_ecoute;
                        tmsg.dest   = join_port;
                        tmsg.size   = table_serialize(table, nb_machines,
                                                      join_ip, tmsg.data);
                        send_msg_t(join_sock, &tmsg);
                        join_pending = 1;
                        join_start   = time(NULL);
                        printf("[JOIN] %s (%s:%d) veut rejoindre — attente token\n",
                               join_hostname, join_ip, join_port);
                    }
                } else {
                    close(new_sock);
                }
            }
        }

        /* ----------------------------------------------------------------
           M1 : reçoit JOIN_DONE sur join_sock → finalise le join
           JOIN_DONE signifie : M(n-1) s'est connecté à Mn, anneau rebouclé
           M1 remplace son ancien sock_gauche par join_sock (connexion de Mn)
           Puis diffuse la table mise à jour et relance le token
           → Extraire en handle_join_done() si le code grossit
           ---------------------------------------------------------------- */
        if (join_waiting && join_sock > 0 && FD_ISSET(join_sock, &readfds)) {
            int n = recv_msg_t(join_sock, &msg);
            if (n > 0 && msg.type == JOIN_DONE) {
                printf("[JOIN_DONE] Anneau mis a jour !\n");

                close(sock_gauche);          /* ferme l'ancienne connexion (M(n-1)) */
                sock_gauche  = join_sock;    /* Mn devient le nouveau voisin gauche */
                join_sock    = -1;
                join_pending = 0;
                join_waiting = 0;

                /* Ajoute Mn à la table et diffuse sur tout l'anneau.
                   M1 remplit son propre port_s = port_voisin_droite (= port de M2)
                   Les autres machines rempliront le leur au passage du TABLE_UPDATE */
                table_add(table, &nb_machines, join_port, join_ip, join_hostname, 0);
                for (int i = 0; i < nb_machines; i++) {
                    if (table[i].port == port_ecoute) {
                        table[i].port_s = port_voisin_droite;
                        break;
                    }
                }
                table_print(table, nb_machines);

                msg_t tupd;
                tupd.type   = TABLE_UPDATE;
                tupd.source = port_ecoute;
                tupd.dest   = -1;
                tupd.size   = table_serialize(table, nb_machines, self_ip, tupd.data);
                send_msg_t(sock_droite, &tupd);

                /* Relance le token dans le nouvel anneau avec seq incrémenté
                   → invalide tout éventuel token en transit lors du join */
                msg_t token = create_msg_TOKEN(port_ecoute, -1);
                token.size = ++token_seq;
                send_msg_t(sock_droite, &token);
                printf("[TOKEN] Nouveau token apres join (seq=%d)\n", token_seq);
            }
        }

        /* ================================================================
           MESSAGES REÇUS DEPUIS L'ANNEAU (sock_gauche)
           Dispatcher selon msg.type — extraire en handle_ring_msg() si besoin
           ================================================================ */
        if (FD_ISSET(sock_gauche, &readfds)) {
            int n = recv_msg_t(sock_gauche, &msg);
            if (n <= 0) {
                /* Déconnexion du voisin gauche.
                   1. join en cours → M(n-1) a fermé volontairement, normal
                   2. has_leaving → c'est MOI qui pars → sortie propre
                   3. sinon → déconnexion brutale → lancer REPAIR */
                if (join_waiting) continue;
                if (has_leaving) {
                    printf("[LEAVE] Deconnexion confirmee par M(n-1), sortie propre\n");
                    break;
                }
                /* Déconnexion brutale : on cherche quel port est mort.
                   Le voisin gauche mort = celui dont port_s == port_ecoute dans la table
                   (il avait port_ecoute comme voisin droit = nous).
                   On envoie REPAIR_CMD sur sock_droite : data = "ip_moi port_moi port_mort" */
                int dead_port = -1;
                for (int i = 0; i < nb_machines; i++) {
                    if (table[i].port_s == port_ecoute && table[i].port != port_ecoute) {
                        dead_port = table[i].port; break;
                    }
                }
                printf("[REPAIR] Voisin gauche mort (port=%d) — envoi REPAIR_CMD\n",
                       dead_port);
                close(sock_gauche); sock_gauche = -1;
                /* Ouvre server_sock pour recevoir la reconnexion de M(j-1) */
                msg_t rcmd;
                rcmd.type   = REPAIR_CMD;
                rcmd.source = port_ecoute;
                rcmd.dest   = -1;
                rcmd.size   = snprintf(rcmd.data, SMAX, "%s %d %d",
                                       self_ip, port_ecoute, dead_port);
                send_msg_t(sock_droite, &rcmd);
                /* Attend la reconnexion de M(j-1) sur server_sock */
                printf("[REPAIR] Attente reconnexion sur port %d...\n", port_ecoute);
                struct sockaddr_in rcli; socklen_t rlg = sizeof(rcli);
                sock_gauche = accept(server_sock, (struct sockaddr *)&rcli, &rlg);
                if (sock_gauche < 0) { perror("accept repair"); break; }
                /* Lit le flag de connexion puis REPAIR_DONE */
                char rflag;
                msg_t rdone;
                recv_all(sock_gauche, &rflag, 1);
                recv_msg_t(sock_gauche, &rdone);
                /* M1 retire la machine morte et diffuse la table mise à jour */
                if (dead_port > 0) {
                    table_remove(table, &nb_machines, dead_port);
                    table_print(table, nb_machines);
                }
                if (choix == 1) {
                    msg_t tupd;
                    tupd.type   = TABLE_UPDATE;
                    tupd.source = port_ecoute; tupd.dest = -1;
                    tupd.size   = table_serialize(table, nb_machines, self_ip, tupd.data);
                    send_msg_t(sock_droite, &tupd);
                    msg_t tok = create_msg_TOKEN(port_ecoute, -1);
                    tok.size = ++token_seq;
                    send_msg_t(sock_droite, &tok);
                    printf("[REPAIR] Anneau repare, token relance (seq=%d)\n", token_seq);
                }
                continue;
            }

            /* ------------------------------------------------------------
               TOKEN — le jeton circule dans l'anneau
               Priorité :
                 1. Si M1 a un join en attente → bloquer, lancer JOIN_CMD
                 2. Si Comm a un message en attente → l'envoyer puis le token
                 3. Sinon → faire circuler normalement
               → Extraire en handle_token() si le code grossit
               ------------------------------------------------------------ */
            if (msg.type == TOKEN) {

                /* M1 vérifie le numéro de séquence du token
                   Si seq < token_seq : token obsolète (ex: ancien token qui revient
                   après qu'on ait régénéré) → on l'absorbe silencieusement
                   Si seq == token_seq : token courant, on le traite normalement */
                if (choix == 1) {
                    if (msg.size < token_seq) {
                        printf("[TOKEN] Obsolete (seq=%d < %d) — absorbe\n",
                               msg.size, token_seq);
                        continue;   /* ignore ce token, le token courant circule encore */
                    }
                    /* Synchronise token_seq (cas normal : msg.size == token_seq) */
                    token_seq = msg.size;
                }

                if (choix == 1 && join_pending && !join_waiting) {
                    /* M1 bloque le token et lance le protocole d'insertion */
                    printf("[TOKEN] Bloque — JOIN_CMD vers %s:%d\n",
                           join_ip, join_port);
                    msg_t jcmd;
                    jcmd.type   = JOIN_CMD;
                    jcmd.source = port_ecoute;   /* M1_port : identifie M(n-1) */
                    jcmd.dest   = -1;
                    jcmd.size   = snprintf(jcmd.data, SMAX, "%s %d",
                                           join_ip, join_port);
                    send_msg_t(sock_droite, &jcmd);
                    join_waiting = 1;

                } else if (has_pending && pending_msg.type == FILE_START) {
                    /* Transfert de fichier : on tient le token pendant tout le transfert
                       (Token Ring : on garde le jeton tant qu'on émet)
                       1. Envoie FILE_START
                       2. Lit depuis unix_client et envoie FILE_DATA jusqu'à FILE_END
                       3. Relâche le token */
                    printf("[TOKEN] Transfert fichier vers %d — token tenu\n",
                           pending_msg.dest);
                    send_msg_t(sock_droite, &pending_msg);
                    has_pending = 0;

                    msg_t fmsg;
                    int chunks = 0;
                    while (recv_msg_t(unix_client, &fmsg) > 0) {
                        send_msg_t(sock_droite, &fmsg);
                        if (fmsg.type == FILE_DATA) chunks++;
                        if (fmsg.type == FILE_END)  break;
                    }
                    printf("[TOKEN] Transfert termine (%d blocs) — token relache\n", chunks);
                    msg.source = port_ecoute;
                    send_msg_t(sock_droite, &msg);

                } else if (has_pending) {
                    /* On a le token + un message Comm en attente : on envoie d'abord le message */
                    printf("[TOKEN] Envoi msg (dest=%d) puis circulation\n",
                           pending_msg.dest);
                    send_msg_t(sock_droite, &pending_msg);
                    has_pending = 0;
                    sleep(2);
                    msg.source = port_ecoute;
                    send_msg_t(sock_droite, &msg);

                } else {
                    /* Token libre → circulation normale */
                    printf("[TOKEN] Recu (source=%d) → circulation\n", msg.source);
                    sleep(2);
                    msg.source = port_ecoute;
                    send_msg_t(sock_droite, &msg);
                }

            /* ------------------------------------------------------------
               JOIN_CMD — M1 demande l'insertion d'une nouvelle machine
               Chaque machine vérifie si elle est M(n-1) :
                 → port_voisin_droite == msg.source (M1_port) : OUI, je suis M(n-1)
                 → sinon : forward
               M(n-1) ferme son sock_droite vers M1 et se connecte à Mn
               puis envoie JOIN_DONE pour signaler que l'anneau est rebouclé
               → Extraire en handle_join_cmd() si le code grossit
               ------------------------------------------------------------ */
            } else if (msg.type == JOIN_CMD) {

                if (port_voisin_droite == msg.source) {
                    /* Je suis M(n-1) : mon voisin de droite actuel = M1 */
                    char mn_ip[100]; int mn_port;
                    sscanf(msg.data, "%s %d", mn_ip, &mn_port);
                    printf("[JOIN_CMD] Je suis M(n-1) → connexion vers %s:%d\n",
                           mn_ip, mn_port);

                    close(sock_droite);
                    sock_droite = socket_create_inet(mn_ip, mn_port);
                    if (sock_droite < 0) FATAL("socket_create_inet join");

                    port_voisin_droite = mn_port;
                    strcpy(ip_voisin_droite, mn_ip);

                    /* Confirme l'insertion → Mn relaiera vers M1 */
                    msg_t done;
                    done.type = JOIN_DONE; done.source = port_ecoute;
                    done.dest = -1;        done.size   = 0;
                    send_msg_t(sock_droite, &done);

                } else {
                    /* Pas M(n-1) → simple forward */
                    send_msg_t(sock_droite, &msg);
                }

            /* ------------------------------------------------------------
               JOIN_DONE — Mn relaie vers M1 (Mn n'a rien d'autre à faire)
               ------------------------------------------------------------ */
            } else if (msg.type == JOIN_DONE) {
                send_msg_t(sock_droite, &msg);

            /* ------------------------------------------------------------
               LEAVE_CMD — une machine M_L veut quitter l'anneau
               Même mécanisme d'identification que JOIN_CMD :
                 port_voisin_droite == msg.source → je suis M(n-1)
               M(n-1) ferme sock_droite (vers M_L) et se connecte à M_next
               Envoie LEAVE_DONE : msg.dest = port_L (pour que M1 retire de table)
               ------------------------------------------------------------ */
            } else if (msg.type == LEAVE_CMD) {

                if (port_voisin_droite == msg.source) {
                    /* Je suis M(n-1) : mon voisin de droite actuel = M_L */
                    char ip_next[100]; int port_next;
                    sscanf(msg.data, "%s %d", ip_next, &port_next);
                    printf("[LEAVE_CMD] Je suis M(n-1) → connexion vers %s:%d\n",
                           ip_next, port_next);

                    int old_droite = sock_droite;
                    sock_droite = socket_create_inet(ip_next, port_next);
                    if (sock_droite < 0) FATAL("socket_create_inet leave");
                    close(old_droite);    /* M_L voit recv==0 sur sock_gauche */

                    port_voisin_droite = port_next;
                    strcpy(ip_voisin_droite, ip_next);

                    /* Envoie flag 'L' avant LEAVE_DONE pour que M_next distingue
                       cette connexion d'un JOIN (correction du bug port=218103808) */
                    char lflag = 'L';
                    send_all(sock_droite, &lflag, 1);

                    /* LEAVE_DONE : msg.dest = port_L (pour que M1 retire de table) */
                    msg_t done;
                    done.type   = LEAVE_DONE;
                    done.source = port_ecoute;
                    done.dest   = msg.source;   /* port_L */
                    done.size   = 0;
                    done.data[0] = '\0';
                    send_msg_t(sock_droite, &done);

                } else {
                    send_msg_t(sock_droite, &msg);
                }

            /* ------------------------------------------------------------
               LEAVE_DONE — anneau rebouclé après départ de msg.dest (= port_L)
               - Non-M1 : forward (déjà traité dans le handler server_sock pour
                 M_next, mais les machines entre M_next et M1 le reçoivent ici)
               - M1 : retire port_L de la table + diffuse TABLE_UPDATE
               ------------------------------------------------------------ */
            } else if (msg.type == LEAVE_DONE) {

                if (choix == 1) {
                    int leaving_port = msg.dest;
                    table_remove(table, &nb_machines, leaving_port);
                    table_print(table, nb_machines);
                    printf("[LEAVE_DONE] Machine %d retiree de la table\n", leaving_port);

                    /* Diffuse la table mise à jour + relance token */
                    msg_t tupd;
                    tupd.type   = TABLE_UPDATE;
                    tupd.source = port_ecoute;
                    tupd.dest   = -1;
                    for (int i = 0; i < nb_machines; i++) {
                        if (table[i].port == port_ecoute) {
                            table[i].port_s = port_voisin_droite; break;
                        }
                    }
                    tupd.size = table_serialize(table, nb_machines, self_ip, tupd.data);
                    send_msg_t(sock_droite, &tupd);

                    msg_t token = create_msg_TOKEN(port_ecoute, -1);
                    token.size = ++token_seq;
                    send_msg_t(sock_droite, &token);
                    printf("[TOKEN] Token relance apres depart (seq=%d)\n", token_seq);
                } else {
                    send_msg_t(sock_droite, &msg);
                }

            /* ------------------------------------------------------------
               TABLE_UPDATE — table des machines mise à jour par M1
               - Si source == moi : tour complet → absorber
               - Sinon : mettre à jour la table locale et forward
               ------------------------------------------------------------ */
            } else if (msg.type == TABLE_UPDATE) {

                if (msg.source == port_ecoute) {
                    /* Tour complet : met à jour la table locale avec les port_s */
                    char dummy[INET_ADDRSTRLEN];
                    table_deserialize(msg.data, table, &nb_machines, dummy);
                    printf("[TABLE] Tour complet, absorbe\n");
                    /* Notifie comm de la mise à jour */
                    if (unix_client > 0) send_msg_t(unix_client, &msg);
                } else {
                    char dummy[INET_ADDRSTRLEN];
                    table_deserialize(msg.data, table, &nb_machines, dummy);
                    /* Se réinsère dans la table (au cas où elle aurait été écrasée)
                       is_master = (choix==1) : M1 reste maître même après désérialisation */
                    table_add(table, &nb_machines, port_ecoute, self_ip, self_hostname,
                              choix == 1 ? 1 : 0);

                    /* Remplit son propre port_s = port du voisin droit actuel
                       Ainsi quand M1 absorbe le TABLE_UPDATE, tous les port_s sont remplis */
                    for (int i = 0; i < nb_machines; i++) {
                        if (table[i].port == port_ecoute) {
                            table[i].port_s = port_voisin_droite;
                            break;
                        }
                    }

                    /* Re-sérialise avec le port_s mis à jour avant de forwarder */
                    msg.size = table_serialize(table, nb_machines, self_ip, msg.data);
                    table_print(table, nb_machines);
                    send_msg_t(sock_droite, &msg);
                }

            /* ------------------------------------------------------------
               TEXT — message unicast d'une machine vers une autre
               - dest == moi → livrer à Comm via socket unix
               - dest != moi → forward
               ------------------------------------------------------------ */
            } else if (msg.type == TEXT) {

                if (msg.dest == port_ecoute) {
                    printf("[MSG] Pour moi ! de %d: %s\n", msg.source, msg.data);
                    if (unix_client > 0) send_msg_t(unix_client, &msg);
                } else {
                    send_msg_t(sock_droite, &msg);
                }

            /* ------------------------------------------------------------
               BROADCAST — message vers toutes les machines
               - source == moi : tour complet → absorber
               - Sinon : livrer à Comm ET forward
               ------------------------------------------------------------ */
            } else if (msg.type == BROADCAST) {

                if (msg.source == port_ecoute) {
                    printf("[BROADCAST] Tour complet, absorbe\n");
                } else {
                    printf("[BROADCAST] de %d: %s\n", msg.source, msg.data);
                    if (unix_client > 0) send_msg_t(unix_client, &msg);
                    send_msg_t(sock_droite, &msg);
                }

            /* ------------------------------------------------------------
               FILE_START / FILE_DATA / FILE_END — transfert de fichier
               Routage identique à TEXT (unicast vers dest)
               - dest == moi → livrer à Comm via unix (elle assemble le fichier)
               - dest != moi → forward sur l'anneau
               ------------------------------------------------------------ */
            } else if (msg.type == FILE_START ||
                       msg.type == FILE_DATA  ||
                       msg.type == FILE_END) {

                if (msg.dest == port_ecoute) {
                    if (msg.type == FILE_START)
                        printf("[FICHIER] Reception depuis %d : '%s' (%d octets)\n",
                               msg.source, msg.data, msg.size);
                    if (unix_client > 0) send_msg_t(unix_client, &msg);
                } else {
                    send_msg_t(sock_droite, &msg);
                }

            /* ------------------------------------------------------------
               REPAIR_CMD — un voisin droit Mi signale que Mj (son voisin gauche) est mort
               data = "ip_Mi port_Mi port_mort"
               Si port_voisin_droite == port_mort → je suis M(j-1), je me reconnecte à Mi
               Sinon → forward
               ------------------------------------------------------------ */
            } else if (msg.type == REPAIR_CMD) {
                char ip_mi[INET_ADDRSTRLEN]; int port_mi, port_mort;
                sscanf(msg.data, "%s %d %d", ip_mi, &port_mi, &port_mort);

                if (port_voisin_droite == port_mort) {
                    /* Je suis M(j-1) : mon voisin droit est mort */
                    printf("[REPAIR_CMD] Mon voisin droit %d est mort — reconnexion vers %s:%d\n",
                           port_mort, ip_mi, port_mi);
                    close(sock_droite);
                    sock_droite = socket_create_inet(ip_mi, port_mi);
                    if (sock_droite < 0) FATAL("socket_create_inet repair");
                    port_voisin_droite = port_mi;
                    strcpy(ip_voisin_droite, ip_mi);
                    /* Envoie flag 'R' + REPAIR_DONE */
                    char rflag = 'R';
                    send_all(sock_droite, &rflag, 1);
                    msg_t rdone;
                    rdone.type   = REPAIR_DONE;
                    rdone.source = port_ecoute;
                    rdone.dest   = port_mort;   /* port de la machine morte */
                    rdone.size   = 0;
                    rdone.data[0] = '\0';
                    send_msg_t(sock_droite, &rdone);
                    printf("[REPAIR_CMD] REPAIR_DONE envoye\n");
                } else {
                    send_msg_t(sock_droite, &msg);
                }

            /* ------------------------------------------------------------
               REPAIR_DONE — anneau réparé, forward jusqu'à Mi (déjà traité
               dans le handler accept + boucle repair au-dessus)
               ------------------------------------------------------------ */
            } else if (msg.type == REPAIR_DONE) {
                /* Normalement absorbé par Mi dans le accept() bloquant ci-dessus.
                   Si on arrive ici c'est un forward d'une machine intermédiaire. */
                send_msg_t(sock_droite, &msg);
            }
        }
    }

    /* Nettoyage */
    close(sock_gauche);
    close(sock_droite);
    close(server_sock);
    close(unix_listen);
    if (unix_client > 0) close(unix_client);
    if (join_sock > 0)   close(join_sock);
    unlink(unix_path);
    return 0;
}
