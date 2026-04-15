/*
 * protocole.h — définitions partagées entre driver.c et comm.c
 *
 * Contient :
 *   - les constantes SMAX, HOSTNAME_LEN, MAX_MACHINES
 *   - les types de messages (#define TEXT, TOKEN, ...)
 *   - la struct msg_t  : format de TOUS les messages échangés sur l'anneau
 *   - la struct machine_t : entrée de la table des machines
 *   - les prototypes des fonctions de création de messages (implémentées dans protocole.c)
 */

#ifndef PROTOCOLE_H
#define PROTOCOLE_H

#include <stddef.h>
#include <netinet/in.h>

#define SMAX         1024   /* taille max du champ data dans msg_t          */
#define HOSTNAME_LEN 32     /* taille max d'un nom de machine               */
#define MAX_MACHINES 18     /* max machines dans l'anneau (calculé pour tenir dans SMAX) */
                            /* vérif : 4 + 18*(4+16+32) + 16 = 956 < SMAX  */

/* ------------------------------------------------------------------
   Types de messages — utilisés dans msg_t.type
   ------------------------------------------------------------------ */
#define TEXT         1   /* message texte d'une machine vers une autre      */
#define TOKEN        2   /* jeton de l'anneau (circule en permanence)       */
#define BROADCAST    3   /* message texte vers toutes les machines          */
#define MSG_FILE     4   /* (futur) annonce transfert fichier               */
#define FILE_START   5   /* (futur) début d'un transfert fichier            */
#define FILE_DATA    6   /* (futur) chunk de données d'un fichier           */
#define FILE_END     7   /* (futur) fin d'un transfert fichier              */
#define JOIN_CMD     8   /* M1 → anneau : "insère cette machine après M(n-1)" */
#define JOIN_DONE    9   /* M(n-1) → Mn → M1 : "insertion terminée"        */
#define TABLE_UPDATE 10  /* M1 → anneau : table des machines mise à jour    */
#define TABLE_REQ    11  /* Comm → Driver (unix) : "donne-moi la table"     */
#define LEAVE_CMD    12  /* M_L → anneau : "je pars, voisin gauche reconnecte-toi" */
#define LEAVE_DONE   13  /* M(n-1) → M_next... → M1 : "anneau rebouclé, retire port_L" */
#define REPAIR_CMD   14  /* Mi → anneau : "Mj mort, M(j-1) reconnecte-toi à moi"   */
#define REPAIR_DONE  15  /* M(j-1) → Mi : "reconnexion faite, anneau réparé"        */
#define ELECTION     16  /* M2 → anneau : "M1 mort, je suis nouveau maître"         */

/* ------------------------------------------------------------------
   Format de TOUS les messages échangés (anneau ET socket unix)
   ------------------------------------------------------------------ */
typedef struct {
    int  type;          /* type du message (voir #define ci-dessus)         */
    int  source;        /* port de la machine émettrice (= son ID)          */
    int  dest;          /* port de la machine destinataire (-1 = broadcast) */
    int  size;          /* nb d'octets significatifs dans data              */
    char data[SMAX];    /* contenu du message (texte, table sérialisée...)  */
} msg_t;

/* ------------------------------------------------------------------
   Entrée de la table des machines (utils.c : table_add/remove/...)
   ------------------------------------------------------------------ */
typedef struct {
    int  port;                   /* port E : port d'écoute = ID de la machine   */
    char ip[INET_ADDRSTRLEN];    /* adresse IPv4 (16 octets max)                */
    char hostname[HOSTNAME_LEN]; /* nom de la machine (gethostname)             */
    int  is_master;              /* 1 si cette machine est le coordinateur (M1) */
    int  port_s;                 /* port S : port du voisin droit (anneausockd) */
                                 /* rempli lors de la circulation TABLE_UPDATE  */
} machine_t;

/* ------------------------------------------------------------------
   Fonctions de création de messages — implémentées dans protocole.c
   Si on veut ajouter un nouveau type de message, ajouter ici + dans protocole.c
   ------------------------------------------------------------------ */
msg_t create_msg_TEXT(int source, int dest, char *data);
msg_t create_msg_TOKEN(int source, int dest);
msg_t create_msg_BROADCAST(int source, char *data);
msg_t create_file_start(int source, int dest, const char *filename);
msg_t create_file_data(int source, int dest, const char *part, int size);
msg_t create_file_end(int source, int dest);

#endif
