/*
 * driver.c — processus Driver de l'anneau (un par machine)
 *
 * Le driver gere la topologie, fait circuler le token et route les messages.
 * Il parle avec comm.c via une socket Unix locale (/tmp/ring_<port>).
 *
 * Structure du fichier (style inspire du td5) :
 *   - variables globales pour l'etat de l'anneau (comme sock_diffuseur, recepteurs)
 *   - init_as_M1 / init_as_Mn : creation ou insertion dans l'anneau
 *   - handle_* : une fonction par fd surveille et par type de message
 *   - main() : boucle select() qui dispatche vers les handlers
 *
 * Sockets utilises :
 *   sock_gauche  <- reception depuis le voisin gauche (anneausockg)
 *   sock_droite  -> envoi vers le voisin droit (anneausockd)
 *   server_sock  -> ecoute des nouvelles connexions (joins, reconnexions leave/election)
 *   unix_listen  -> ecoute de comm.c
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

/* ----- etat global partage entre main() et handlers ----- */

int choix;
int port_ecoute        = -1;
int server_sock        = -1;
int sock_gauche        = -1;
int sock_droite        = -1;
int port_voisin_droite = -1;
char ip_voisin_droite[100]    = {0};
char self_ip[INET_ADDRSTRLEN] = {0};
char self_hostname[HOSTNAME_LEN] = {0};

machine_t table[MAX_MACHINES];
int nb_machines = 0;

int token_seq       = 0;
int has_leaving     = 0;
int election_pending = 0;

/* Variables de join — M1 uniquement */
int    join_pending = 0;
int    join_waiting = 0;
int    join_sock    = -1;
char   join_ip[INET_ADDRSTRLEN]    = {0};
char   join_hostname[HOSTNAME_LEN] = {0};
int    join_port    = -1;
time_t join_start   = 0;

/* Message Comm en attente du token */
msg_t pending_msg;
int   has_pending = 0;

int  unix_listen = -1;
int  unix_client = -1;
char unix_path[64];

int should_exit = 0;   /* set a 1 pour sortir de la boucle main */


/* detecte l'IP reelle (evite 127.0.1.1 sur Ubuntu) et le hostname local */
void detect_self_ip_and_host(void) {
    char tmp_host[256];
    gethostname(tmp_host, sizeof(tmp_host));
    strncpy(self_hostname, tmp_host, HOSTNAME_LEN - 1);

    struct ifaddrs *ifas, *ifa;
    if (getifaddrs(&ifas) == 0) {
        for (ifa = ifas; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
            if (strncmp(buf, "127.", 4) != 0) {
                strncpy(self_ip, buf, INET_ADDRSTRLEN - 1);
                break;
            }
        }
        freeifaddrs(ifas);
    }
    if (self_ip[0] == '\0') strcpy(self_ip, "127.0.0.1");
}


/* cherche le hostname et l'IP d'une machine dans la table a partir de son port */
void lookup_host_ip(int port, const char **host, const char **ip) {
    *host = "?"; *ip = "?";
    for (int i = 0; i < nb_machines; i++) {
        if (table[i].port == port) {
            *host = table[i].hostname;
            *ip   = table[i].ip;
            return;
        }
    }
}

/* factorisation : met a jour mon port_s, diffuse la nouvelle table et relance le token
   appele apres un join / leave / repair / election (anneau rebouclé) */
void diffuse_table_et_relance_token(const char *tag) {
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

    msg_t tok = create_msg_TOKEN(port_ecoute, -1);
    tok.size = ++token_seq;
    send_msg_t(sock_droite, &tok);
    printf("[%s] Token relance (seq=%d)\n", tag, token_seq);
}


/* M1 cree l'anneau : attend M2, lui envoie la table, se reconnecte a lui pour fermer l'anneau */
void init_as_M1(void) {
    printf("Port d'ecoute: ");
    scanf("%d", &port_ecoute);

    table_add(table, &nb_machines, port_ecoute, self_ip, self_hostname, 1);

    while ((server_sock = socket_create_server(port_ecoute)) < 0) {
        printf("Port %d deja utilise. Choisissez un autre port: ", port_ecoute);
        scanf("%d", &port_ecoute);
    }

    printf("En attente du premier voisin...\n");
    struct sockaddr_in cli;
    socklen_t lg = sizeof(cli);
    sock_gauche = accept(server_sock, (struct sockaddr *)&cli, &lg);
    if (sock_gauche < 0) FATAL("accept");

    int port_m2;
    char hostname_m2[HOSTNAME_LEN];
    char iflag;
    if (recv_all(sock_gauche, &iflag, 1) <= 0) FATAL("recv flag init");
    if (recv_all(sock_gauche, (char *)&port_m2, sizeof(int)) <= 0) FATAL("recv port_m2");
    port_m2 = ntohl(port_m2);
    if (recv_all(sock_gauche, hostname_m2, HOSTNAME_LEN) <= 0) FATAL("recv hostname_m2");

    char ip_m2[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cli.sin_addr, ip_m2, sizeof(ip_m2));
    printf("Connexion recue de %s (%s:%d)\n", hostname_m2, ip_m2, port_m2);

    msg_t tmsg;
    tmsg.type   = TABLE_UPDATE;
    tmsg.source = port_ecoute;
    tmsg.dest   = port_m2;
    tmsg.size   = table_serialize(table, nb_machines, ip_m2, tmsg.data);
    send_msg_t(sock_gauche, &tmsg);

    table_add(table, &nb_machines, port_m2, ip_m2, hostname_m2, 0);
    table_print(table, nb_machines);

    printf("Connexion retour vers %s:%d...\n", ip_m2, port_m2);
    sock_droite = socket_create_inet(ip_m2, port_m2);
    if (sock_droite < 0) FATAL("socket_create_inet retour");

    port_voisin_droite = port_m2;
    strcpy(ip_voisin_droite, ip_m2);
}


/* Mn rejoint un anneau : se connecte a M1, recoit la table, attend M(n-1) */
void init_as_Mn(void) {
    char ip_m1[100];
    int  port_m1;

    printf("IP de M1 (coordinateur): ");
    scanf("%s", ip_m1);
    printf("Port de M1: ");
    scanf("%d", &port_m1);
    printf("Mon port d'ecoute: ");
    scanf("%d", &port_ecoute);

    while ((server_sock = socket_create_server(port_ecoute)) < 0) {
        printf("Port %d deja utilise. Choisissez un autre port: ", port_ecoute);
        scanf("%d", &port_ecoute);
    }

    sock_droite = socket_create_inet(ip_m1, port_m1);
    if (sock_droite < 0) FATAL("socket_create_inet");

    char jflag = 'J';
    if (send_all(sock_droite, &jflag, 1) <= 0) FATAL("send flag");
    int port_net = htonl(port_ecoute);
    if (send_all(sock_droite, &port_net, sizeof(int)) <= 0) FATAL("send port_ecoute");
    if (send_all(sock_droite, self_hostname, HOSTNAME_LEN) <= 0) FATAL("send hostname");

    msg_t tmsg;
    if (recv_msg_t(sock_droite, &tmsg) <= 0) FATAL("recv table");
    char mn_self_ip[INET_ADDRSTRLEN];
    table_deserialize(tmsg.data, table, &nb_machines, mn_self_ip);
    strcpy(self_ip, mn_self_ip);

    table_add(table, &nb_machines, port_ecoute, self_ip, self_hostname, 0);
    table_print(table, nb_machines);

    port_voisin_droite = port_m1;
    strcpy(ip_voisin_droite, ip_m1);

    printf("En attente que M(n-1) se connecte...\n");
    sock_gauche = accept(server_sock, NULL, NULL);
    if (sock_gauche < 0) FATAL("accept");
    printf("Anneau ferme !\n");
}


/* timeout M1 : regenere le token perdu ou rollback un join bloque */
void handle_timeout(void) {
    if (choix != 1) return;
    if (join_waiting) {
        printf("[JOIN] Timeout — JOIN_DONE non recu, rollback\n");
        close(join_sock); join_sock = -1;
        join_pending = 0; join_waiting = 0;
        msg_t token = create_msg_TOKEN(port_ecoute, -1);
        token.size = ++token_seq;
        send_msg_t(sock_droite, &token);
        printf("[TOKEN] Token relance apres rollback join (seq=%d)\n", token_seq);
    } else {
        token_seq++;
        msg_t token = create_msg_TOKEN(port_ecoute, -1);
        token.size = token_seq;
        send_msg_t(sock_droite, &token);
        printf("[TOKEN] Timeout — token regenere (seq=%d, %d machines)\n",
               token_seq, nb_machines);
    }
}


/* comm.c se connecte sur la socket unix : on pousse la table courante */
void handle_unix_accept(void) {
    unix_client = accept(unix_listen, NULL, NULL);
    printf("[UNIX] Comm connecte\n");
    if (nb_machines > 0) {
        msg_t tresp;
        tresp.type   = TABLE_UPDATE;
        tresp.source = port_ecoute;
        tresp.dest   = -1;
        tresp.size   = table_serialize(table, nb_machines, self_ip, tresp.data);
        send_msg_t(unix_client, &tresp);
        printf("[UNIX] Table poussee vers comm (%d machines)\n", nb_machines);
    }
}


/* message recu de comm (TEXT/BROADCAST : stocke en attente du token ; LEAVE/TABLE_REQ : traite direct) */
void handle_unix_client(void) {
    int n = recv_msg_t(unix_client, &pending_msg);
    if (n <= 0) {
        printf("[UNIX] Comm deconnecte\n");
        close(unix_client); unix_client = -1; has_pending = 0;
        return;
    }

    if (pending_msg.type == LEAVE_CMD) {
        if (nb_machines <= 2) {
            printf("[LEAVE] Refuse : anneau de taille minimale (2 machines)\n");
            return;
        }
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

    } else if (pending_msg.type == TABLE_REQ) {
        msg_t tresp;
        tresp.type   = TABLE_UPDATE;
        tresp.source = port_ecoute;
        tresp.dest   = -1;
        tresp.size   = table_serialize(table, nb_machines, self_ip, tresp.data);
        send_msg_t(unix_client, &tresp);

    } else {
        if (has_pending) {
            printf("[UNIX] Refuse — message deja en attente (dest=%d)\n",
                   pending_msg.dest);
            msg_t err;
            err.type   = TEXT;
            err.source = port_ecoute;
            err.dest   = port_ecoute;
            err.size   = snprintf(err.data, SMAX,
                "[ERREUR] Un message est deja en attente du token, reessayez");
            send_msg_t(unix_client, &err);
        } else {
            has_pending = 1;
            const char *dh = "?", *di = "?";
            for (int i = 0; i < nb_machines; i++) {
                if (table[i].port == pending_msg.dest) {
                    dh = table[i].hostname; di = table[i].ip; break;
                }
            }
            if (pending_msg.type == BROADCAST)
                printf("[MSG] BROADCAST en attente du token\n");
            else
                printf("[MSG] → %s (%s:%d) — en attente du token\n",
                       dh, di, pending_msg.dest);
        }
    }
}


/* nouvelle connexion TCP : le premier byte indique le type
   'J' = join, 'L' = reconnexion apres leave, 'E' = reconnexion apres election */
void handle_server_accept(void) {
    struct sockaddr_in scli;
    socklen_t slg = sizeof(scli);
    int new_sock = accept(server_sock, (struct sockaddr *)&scli, &slg);
    if (new_sock < 0) return;

    char conn_flag;
    if (recv_all(new_sock, &conn_flag, 1) <= 0) { close(new_sock); return; }

    if (conn_flag == 'L') {
        /* Reconnexion LEAVE : M(n-1) devient le nouveau voisin gauche */
        msg_t ldone;
        if (recv_msg_t(new_sock, &ldone) <= 0 || ldone.type != LEAVE_DONE) {
            close(new_sock); return;
        }
        int leaving_port = ldone.dest;
        int leaving_was_master = 0;
        for (int i = 0; i < nb_machines; i++) {
            if (table[i].port == leaving_port) {
                leaving_was_master = table[i].is_master; break;
            }
        }
        const char *new_h, *new_ip;
        lookup_host_ip(ldone.source, &new_h, &new_ip);
        printf("[LEAVE] Nouveau voisin gauche : %s (%s:%d)\n",
               new_h, new_ip, ldone.source);
        close(sock_gauche);
        sock_gauche = new_sock;

        if (choix == 1 || leaving_was_master) {
            if (leaving_was_master && choix != 1) {
                const char *dh, *dip;
                lookup_host_ip(leaving_port, &dh, &dip);
                printf("[LEAVE] M1 %s (%s:%d) a quitte — je deviens le nouveau maitre\n",
                       dh, dip, leaving_port);
                choix = 1;
            }
            table_remove(table, &nb_machines, leaving_port);
            for (int i = 0; i < nb_machines; i++)
                table[i].is_master = (table[i].port == port_ecoute) ? 1 : 0;
            printf("[LEAVE_DONE] Machine %d retiree\n", leaving_port);
            diffuse_table_et_relance_token("LEAVE");
        } else {
            send_msg_t(sock_droite, &ldone);
        }

    } else if (conn_flag == 'E') {
        /* Reconnexion ELECTION : Mn rebranche vers moi (nouveau M1) */
        msg_t elec;
        if (recv_msg_t(new_sock, &elec) <= 0 || elec.type != ELECTION) {
            close(new_sock); return;
        }
        printf("[ELECTION] Mn reconnecte — anneau referme, je suis M1\n");
        election_pending = 0;
        if (sock_gauche >= 0) close(sock_gauche);
        sock_gauche = new_sock;
        diffuse_table_et_relance_token("ELECTION");

    } else if (conn_flag == 'J' && choix == 1) {
        /* JOIN : nouvelle machine — M1 uniquement */
        join_sock = new_sock;
        inet_ntop(AF_INET, &scli.sin_addr, join_ip, sizeof(join_ip));
        int jport_net;
        if (recv_all(join_sock, (char *)&jport_net, sizeof(int)) <= 0
         || recv_all(join_sock, join_hostname, HOSTNAME_LEN) <= 0) {
            close(join_sock); join_sock = -1;
            return;
        }
        join_port = ntohl(jport_net);

        /* Verif : l'ID machine = port, donc un port deja pris casse la table */
        for (int i = 0; i < nb_machines; i++) {
            if (table[i].port == join_port) {
                printf("[JOIN] Refuse — port %d deja utilise par %s (%s)\n",
                       join_port, table[i].hostname, table[i].ip);
                close(join_sock); join_sock = -1;
                return;
            }
        }

        msg_t tmsg;
        tmsg.type   = TABLE_UPDATE;
        tmsg.source = port_ecoute;
        tmsg.dest   = join_port;
        tmsg.size   = table_serialize(table, nb_machines, join_ip, tmsg.data);
        send_msg_t(join_sock, &tmsg);
        join_pending = 1;
        join_start   = time(NULL);
        printf("[JOIN] %s (%s:%d) veut rejoindre — attente token\n",
               join_hostname, join_ip, join_port);

    } else {
        close(new_sock);
    }
}


/* M1 recoit JOIN_DONE : anneau rebouclé, on diffuse la nouvelle table et on relance le token */
void handle_join_done_on_joinsock(void) {
    msg_t msg;
    int n = recv_msg_t(join_sock, &msg);
    if (n <= 0 || msg.type != JOIN_DONE) return;

    printf("[JOIN_DONE] Anneau mis a jour !\n");
    close(sock_gauche);
    sock_gauche  = join_sock;
    join_sock    = -1;
    join_pending = 0;
    join_waiting = 0;

    table_add(table, &nb_machines, join_port, join_ip, join_hostname, 0);
    diffuse_table_et_relance_token("JOIN");
}


/* ----- handlers par type de message recu sur sock_gauche ----- */

/* token : si j'ai un msg en attente je l'envoie avant de faire tourner le token
   M1 verifie en plus le numero de sequence (absorbe les tokens obsoletes) */
void handle_token(msg_t *msg) {
    if (choix == 1) {
        if (msg->size < token_seq) {
            printf("[TOKEN] Obsolete (seq=%d < %d) — absorbe\n", msg->size, token_seq);
            return;
        }
        token_seq = msg->size;
    }

    if (choix == 1 && join_pending && !join_waiting) {
        printf("[TOKEN] Bloque — JOIN_CMD vers %s:%d\n", join_ip, join_port);
        msg_t jcmd;
        jcmd.type   = JOIN_CMD;
        jcmd.source = port_ecoute;
        jcmd.dest   = -1;
        jcmd.size   = snprintf(jcmd.data, SMAX, "%s %d", join_ip, join_port);
        send_msg_t(sock_droite, &jcmd);
        join_waiting = 1;

    } else if (has_pending && pending_msg.type == FILE_START) {
        printf("[TOKEN] Transfert fichier vers %d — token tenu\n", pending_msg.dest);
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
        msg->source = port_ecoute;
        send_msg_t(sock_droite, msg);

    } else if (has_pending) {
        if (pending_msg.type == BROADCAST) {
            printf("[TOKEN] Envoi BROADCAST\n");
        } else {
            const char *sh = "?", *si = "?";
            for (int i = 0; i < nb_machines; i++) {
                if (table[i].port == pending_msg.dest) {
                    sh = table[i].hostname; si = table[i].ip; break;
                }
            }
            printf("[TOKEN] Envoi → %s (%s:%d)\n", sh, si, pending_msg.dest);
        }
        send_msg_t(sock_droite, &pending_msg);
        has_pending = 0;
        sleep(2);
        msg->source = port_ecoute;
        send_msg_t(sock_droite, msg);

    } else {
        printf("[TOKEN] Recu (source=%d) → circulation\n", msg->source);
        sleep(2);
        msg->source = port_ecoute;
        send_msg_t(sock_droite, msg);
    }
}

/* JOIN_CMD : si je suis M(n-1) je me reconnecte vers Mn, sinon je fais suivre */
void handle_join_cmd(msg_t *msg) {
    if (port_voisin_droite == msg->source) {
        char mn_ip[100]; int mn_port;
        sscanf(msg->data, "%s %d", mn_ip, &mn_port);
        printf("[JOIN_CMD] Je suis M(n-1) → connexion vers %s:%d\n", mn_ip, mn_port);
        close(sock_droite);
        sock_droite = socket_create_inet(mn_ip, mn_port);
        if (sock_droite < 0) FATAL("socket_create_inet join");
        port_voisin_droite = mn_port;
        strcpy(ip_voisin_droite, mn_ip);
        msg_t done;
        done.type = JOIN_DONE; done.source = port_ecoute;
        done.dest = -1;        done.size   = 0;
        send_msg_t(sock_droite, &done);
    } else {
        send_msg_t(sock_droite, msg);
    }
}

void handle_join_done_msg(msg_t *msg) {
    send_msg_t(sock_droite, msg);
}

/* LEAVE_CMD : si je suis le voisin gauche de la machine qui part, je me reconnecte a son voisin droit */
void handle_leave_cmd(msg_t *msg) {
    if (port_voisin_droite == msg->source) {
        char ip_next[100]; int port_next;
        sscanf(msg->data, "%s %d", ip_next, &port_next);
        const char *nh, *ni;
        lookup_host_ip(port_next, &nh, &ni);
        (void)ni;
        printf("[LEAVE_CMD] Je suis M(n-1) → connexion vers %s (%s:%d)\n",
               nh, ip_next, port_next);
        int old_droite = sock_droite;
        sock_droite = socket_create_inet(ip_next, port_next);
        if (sock_droite < 0) FATAL("socket_create_inet leave");
        close(old_droite);
        port_voisin_droite = port_next;
        strcpy(ip_voisin_droite, ip_next);
        char lflag = 'L';
        send_all(sock_droite, &lflag, 1);
        msg_t done;
        done.type   = LEAVE_DONE;
        done.source = port_ecoute;
        done.dest   = msg->source;
        done.size   = 0;
        done.data[0] = '\0';
        send_msg_t(sock_droite, &done);
    } else {
        send_msg_t(sock_droite, msg);
    }
}

void handle_leave_done_msg(msg_t *msg) {
    if (choix == 1) {
        int leaving_port = msg->dest;
        table_remove(table, &nb_machines, leaving_port);
        printf("[LEAVE_DONE] Machine %d retiree de la table\n", leaving_port);
        diffuse_table_et_relance_token("LEAVE");
    } else {
        send_msg_t(sock_droite, msg);
    }
}

/* TABLE_UPDATE : 2 tours pour que tout le monde ait les port_s complets
   1er tour : chacun remplit son port_s ; 2eme tour (dest=-2) : sync de la table finale */
void handle_table_update(msg_t *msg) {
    if (msg->source == port_ecoute && msg->dest == -2) {
        printf("[TABLE] Sync complet, absorbe\n");
    } else if (msg->source == port_ecoute) {
        char dummy[INET_ADDRSTRLEN];
        table_deserialize(msg->data, table, &nb_machines, dummy);
        printf("[TABLE] Tour complet — sync vers toutes les machines\n");
        if (unix_client > 0) send_msg_t(unix_client, msg);
        msg_t sync;
        sync.type   = TABLE_UPDATE;
        sync.source = port_ecoute;
        sync.dest   = -2;
        sync.size   = table_serialize(table, nb_machines, self_ip, sync.data);
        send_msg_t(sock_droite, &sync);
    } else if (msg->dest == -2) {
        char dummy[INET_ADDRSTRLEN];
        table_deserialize(msg->data, table, &nb_machines, dummy);
        table_add(table, &nb_machines, port_ecoute, self_ip, self_hostname,
                  choix == 1 ? 1 : 0);
        table_print(table, nb_machines);
        send_msg_t(sock_droite, msg);
    } else {
        char dummy[INET_ADDRSTRLEN];
        table_deserialize(msg->data, table, &nb_machines, dummy);
        table_add(table, &nb_machines, port_ecoute, self_ip, self_hostname,
                  choix == 1 ? 1 : 0);
        for (int i = 0; i < nb_machines; i++) {
            if (table[i].port == port_ecoute) {
                table[i].port_s = port_voisin_droite; break;
            }
        }
        msg->size = table_serialize(table, nb_machines, self_ip, msg->data);
        table_print(table, nb_machines);
        send_msg_t(sock_droite, msg);
    }
}

/* TEXT : si pour moi je livre a comm, sinon je fais suivre sur l'anneau */
void handle_text(msg_t *msg) {
    const char *sh, *si;
    lookup_host_ip(msg->source, &sh, &si);
    if (msg->dest == port_ecoute) {
        printf("[MSG] Pour moi ! de %s (%s:%d): %s\n", sh, si, msg->source, msg->data);
        if (unix_client > 0) send_msg_t(unix_client, msg);
    } else {
        const char *dh, *di;
        lookup_host_ip(msg->dest, &dh, &di);
        printf("[MSG] Pas pour moi : %s → %s (dest=%d) — je fais suivre\n",
               sh, dh, msg->dest);
        (void)si; (void)di;
        send_msg_t(sock_droite, msg);
    }
}

/* BROADCAST : je lis et je fais suivre, sauf si c'est moi la source (tour complet) */
void handle_broadcast(msg_t *msg) {
    if (msg->source == port_ecoute) {
        printf("[BROADCAST] Tour complet, absorbe\n");
    } else {
        const char *sh, *si;
        lookup_host_ip(msg->source, &sh, &si);
        printf("[BROADCAST] de %s (%s:%d): %s — je fais suivre\n",
               sh, si, msg->source, msg->data);
        if (unix_client > 0) send_msg_t(unix_client, msg);
        send_msg_t(sock_droite, msg);
    }
}

/* FILE_START/DATA/END : meme routage que TEXT, comm.c assemble le fichier */
void handle_file(msg_t *msg) {
    if (msg->dest == port_ecoute) {
        if (msg->type == FILE_START) {
            const char *sh, *si;
            lookup_host_ip(msg->source, &sh, &si);
            printf("[FICHIER] Reception depuis %s (%s:%d) : '%s' (%d octets)\n",
                   sh, si, msg->source, msg->data, msg->size);
        }
        if (unix_client > 0) send_msg_t(unix_client, msg);
    } else {
        if (msg->type == FILE_START) {
            const char *sh, *si, *dh, *di;
            lookup_host_ip(msg->source, &sh, &si);
            lookup_host_ip(msg->dest,   &dh, &di);
            printf("[FICHIER] Pas pour moi : %s → %s (dest=%d) — je fais suivre '%s'\n",
                   sh, dh, msg->dest, msg->data);
            (void)si; (void)di;
        }
        send_msg_t(sock_droite, msg);
    }
}

/* REPAIR_CMD : qqn a detecte une machine morte, si c'etait mon voisin droit je me reconnecte */
void handle_repair_cmd(msg_t *msg) {
    char ip_mi[INET_ADDRSTRLEN]; int port_mi, port_mort;
    sscanf(msg->data, "%s %d %d", ip_mi, &port_mi, &port_mort);
    if (port_voisin_droite == port_mort) {
        const char *mh, *mip;
        lookup_host_ip(port_mi, &mh, &mip);
        (void)mip;
        printf("[REPAIR_CMD] Mon voisin droit %d est mort — reconnexion vers %s (%s:%d)\n",
               port_mort, mh, ip_mi, port_mi);
        close(sock_droite);
        sock_droite = socket_create_inet(ip_mi, port_mi);
        if (sock_droite < 0) FATAL("socket_create_inet repair");
        port_voisin_droite = port_mi;
        strcpy(ip_voisin_droite, ip_mi);
        char rflag = 'R';
        send_all(sock_droite, &rflag, 1);
        msg_t rdone;
        rdone.type   = REPAIR_DONE;
        rdone.source = port_ecoute;
        rdone.dest   = port_mort;
        rdone.size   = 0;
        rdone.data[0] = '\0';
        send_msg_t(sock_droite, &rdone);
        printf("[REPAIR_CMD] REPAIR_DONE envoye\n");
    } else {
        send_msg_t(sock_droite, msg);
    }
}

void handle_repair_done(msg_t *msg) {
    send_msg_t(sock_droite, msg);
}

/* ELECTION : M1 est mort, nouveau maitre annonce. Si j'etais son voisin gauche je me reconnecte */
void handle_election(msg_t *msg) {
    char ip_new[INET_ADDRSTRLEN]; int port_new, port_dead;
    sscanf(msg->data, "%s %d %d", ip_new, &port_new, &port_dead);
    table_remove(table, &nb_machines, port_dead);
    for (int i = 0; i < nb_machines; i++)
        table[i].is_master = (table[i].port == port_new) ? 1 : 0;

    if (port_voisin_droite == port_dead) {
        const char *nh, *nip;
        lookup_host_ip(port_new, &nh, &nip);
        (void)nip;
        printf("[ELECTION] Je suis Mn — reconnexion vers %s (%s:%d)\n",
               nh, ip_new, port_new);
        close(sock_droite);
        sock_droite = socket_create_inet(ip_new, port_new);
        if (sock_droite < 0) FATAL("connect election");
        port_voisin_droite = port_new;
        strcpy(ip_voisin_droite, ip_new);
        char eflag = 'E';
        send_all(sock_droite, &eflag, 1);
        send_msg_t(sock_droite, msg);
        printf("[ELECTION] Reconnexion faite, ELECTION transmise\n");
    } else {
        send_msg_t(sock_droite, msg);
    }
}


/* voisin gauche deconnecte brutalement : lance REPAIR (voisin normal) ou ELECTION (M1 mort) */
void handle_ring_disconnect(void) {
    /* Cas normal pendant un join : M(n-1) a ferme volontairement */
    if (join_waiting) return;

    /* Sortie propre : c'est MOI qui pars */
    if (has_leaving) {
        printf("[LEAVE] Deconnexion confirmee par M(n-1), sortie propre\n");
        should_exit = 1;
        return;
    }

    /* Deconnexion brutale : cherche la machine morte dans la table */
    int dead_port = -1, dead_is_master = 0;
    for (int i = 0; i < nb_machines; i++) {
        if (table[i].port_s == port_ecoute && table[i].port != port_ecoute) {
            dead_port      = table[i].port;
            dead_is_master = table[i].is_master;
            break;
        }
    }
    close(sock_gauche); sock_gauche = -1;

    if (dead_is_master) {
        /* ELECTION : je deviens le nouveau maitre */
        printf("[ELECTION] M1 (%d) mort — je deviens maitre\n", dead_port);
        choix = 1;
        election_pending = 1;
        table_remove(table, &nb_machines, dead_port);
        for (int i = 0; i < nb_machines; i++) {
            table[i].is_master = (table[i].port == port_ecoute) ? 1 : 0;
        }
        msg_t elec;
        elec.type   = ELECTION;
        elec.source = port_ecoute;
        elec.dest   = -1;
        elec.size   = snprintf(elec.data, SMAX, "%s %d %d",
                               self_ip, port_ecoute, dead_port);
        send_msg_t(sock_droite, &elec);
        printf("[ELECTION] ELECTION envoyee — attente reconnexion Mn\n");
        /* La reco de Mn arrivera dans handle_server_accept ('E') */
    } else {
        /* REPAIR : envoie REPAIR_CMD, attend reconnexion de M(j-1) */
        printf("[REPAIR] Voisin gauche mort (port=%d) — envoi REPAIR_CMD\n", dead_port);
        msg_t rcmd;
        rcmd.type   = REPAIR_CMD;
        rcmd.source = port_ecoute;
        rcmd.dest   = -1;
        rcmd.size   = snprintf(rcmd.data, SMAX, "%s %d %d",
                               self_ip, port_ecoute, dead_port);
        send_msg_t(sock_droite, &rcmd);
        printf("[REPAIR] Attente reconnexion sur port %d...\n", port_ecoute);
        struct sockaddr_in rcli; socklen_t rlg = sizeof(rcli);
        sock_gauche = accept(server_sock, (struct sockaddr *)&rcli, &rlg);
        if (sock_gauche < 0) { perror("accept repair"); should_exit = 1; return; }
        char rflag; msg_t rdone;
        recv_all(sock_gauche, &rflag, 1);
        recv_msg_t(sock_gauche, &rdone);
        if (dead_port > 0) table_remove(table, &nb_machines, dead_port);
        if (choix == 1) {
            /* M1 repare : diffuse table + relance token en une passe */
            diffuse_table_et_relance_token("REPAIR");
        } else {
            /* non-M1 : juste diffuse la table (le token sera relance par M1) */
            for (int i = 0; i < nb_machines; i++) {
                if (table[i].port == port_ecoute) {
                    table[i].port_s = port_voisin_droite; break;
                }
            }
            table_print(table, nb_machines);
            msg_t tupd;
            tupd.type   = TABLE_UPDATE;
            tupd.source = port_ecoute; tupd.dest = -1;
            tupd.size   = table_serialize(table, nb_machines, self_ip, tupd.data);
            send_msg_t(sock_droite, &tupd);
        }
    }
}


/* lit un message sur sock_gauche et dispatche vers le bon handler */
void handle_ring_msg(void) {
    msg_t msg;
    int n = recv_msg_t(sock_gauche, &msg);
    if (n <= 0) { handle_ring_disconnect(); return; }

    switch (msg.type) {
        case TOKEN:        handle_token(&msg);         break;
        case JOIN_CMD:     handle_join_cmd(&msg);      break;
        case JOIN_DONE:    handle_join_done_msg(&msg); break;
        case LEAVE_CMD:    handle_leave_cmd(&msg);     break;
        case LEAVE_DONE:   handle_leave_done_msg(&msg);break;
        case TABLE_UPDATE: handle_table_update(&msg);  break;
        case TEXT:         handle_text(&msg);          break;
        case BROADCAST:    handle_broadcast(&msg);     break;
        case FILE_START:
        case FILE_DATA:
        case FILE_END:     handle_file(&msg);          break;
        case REPAIR_CMD:   handle_repair_cmd(&msg);    break;
        case REPAIR_DONE:  handle_repair_done(&msg);   break;
        case ELECTION:     handle_election(&msg);      break;
    }
}


/* main : init + boucle select() (meme pattern que td5/serveur.c : FD_ZERO / FD_SET / select / FD_ISSET) */
int main(void) {
    detect_self_ip_and_host();

    printf("===== DRIVER ANNEAU =====\n");
    printf("1. Creer anneau (premier PC)\n");
    printf("2. Rejoindre anneau\n");
    printf("Choix: ");
    scanf("%d", &choix);

    if      (choix == 1) init_as_M1();
    else if (choix == 2) init_as_Mn();
    else { printf("Choix invalide\n"); exit(EXIT_FAILURE); }

    printf("Anneau initialise ! (ID=%d  IP=%s  host=%s)\n",
           port_ecoute, self_ip, self_hostname);

    /* Socket unix Driver↔Comm */
    sprintf(unix_path, "/tmp/ring_%d", port_ecoute);
    unix_listen = socket_create_unix_server(unix_path);
    if (unix_listen < 0) FATAL("socket_create_unix_server");
    printf("[UNIX] En attente de Comm sur %s\n", unix_path);

    /* Port local pour que comm s'y connecte sans argument */
    { FILE *fp = fopen("/tmp/ring_local", "w");
      if (fp) { fprintf(fp, "%d", port_ecoute); fclose(fp); } }

    /* M1 genere le token initial */
    if (choix == 1) {
        msg_t token = create_msg_TOKEN(port_ecoute, -1);
        token.size = ++token_seq;
        send_msg_t(sock_droite, &token);
        printf("[TOKEN] Jeton genere et envoye (seq=%d)\n", token_seq);
    }

    /* Boucle principale : select() sur tous les fds actifs */
    fd_set readfds;
    int max_fd;

    while (!should_exit) {
        FD_ZERO(&readfds);

        if (sock_gauche >= 0) {
            FD_SET(sock_gauche, &readfds);
            max_fd = sock_gauche;
        } else max_fd = 0;

        FD_SET(unix_listen, &readfds);
        if (unix_listen > max_fd) max_fd = unix_listen;

        if (unix_client > 0 && !(has_pending && pending_msg.type == FILE_START)) {
            FD_SET(unix_client, &readfds);
            if (unix_client > max_fd) max_fd = unix_client;
        }

        if (!join_waiting && !has_leaving) {
            FD_SET(server_sock, &readfds);
            if (server_sock > max_fd) max_fd = server_sock;
        }

        if (join_waiting && join_sock > 0) {
            FD_SET(join_sock, &readfds);
            if (join_sock > max_fd) max_fd = join_sock;
        }

        struct timeval tv;
        struct timeval *ptv = NULL;
        if (choix == 1 && !election_pending) {
            if (join_waiting) {
                time_t elapsed = time(NULL) - join_start;
                long remaining = 10 - (long)elapsed;
                if (remaining <= 0) remaining = 1;
                tv.tv_sec = remaining; tv.tv_usec = 0;
            } else {
                int n_sec = (nb_machines > 1 ? nb_machines : 2) * 3;
                tv.tv_sec = n_sec; tv.tv_usec = 0;
            }
            ptv = &tv;
        }

        int activity = select(max_fd + 1, &readfds, NULL, NULL, ptv);
        if (activity < 0) { perror("select"); break; }
        if (activity == 0) { handle_timeout(); continue; }

        if (FD_ISSET(unix_listen, &readfds))                   handle_unix_accept();
        if (unix_client > 0 && FD_ISSET(unix_client, &readfds))handle_unix_client();
        if (!join_waiting && !has_leaving
            && FD_ISSET(server_sock, &readfds))                handle_server_accept();
        if (join_waiting && join_sock > 0
            && FD_ISSET(join_sock, &readfds))                  handle_join_done_on_joinsock();
        if (sock_gauche >= 0 && FD_ISSET(sock_gauche, &readfds))handle_ring_msg();
    }

    close(sock_gauche);
    close(sock_droite);
    close(server_sock);
    close(unix_listen);
    if (unix_client > 0) close(unix_client);
    if (join_sock > 0)   close(join_sock);
    unlink(unix_path);
    return 0;
}
