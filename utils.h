#ifndef UTILS_H
#define UTILS_H

#include "protocole.h"

void FATAL(char *msg);

int send_all(int sock, void *buffer, int size);
int recv_all(int sock, char *buffer, int size);

int send_msg_t(int sock, msg_t *msg);
int recv_msg_t(int sock, msg_t *msg);

int socket_create_inet(const char *hostname, int port);
int socket_create_server(int port);
int socket_create_unix_server(const char *path);

/* Helpers table des machines */
void table_add(machine_t *t, int *nb, int port, const char *ip, const char *hostname, int is_master);
void table_remove(machine_t *t, int *nb, int port);
int  table_serialize(machine_t *t, int nb, const char *self_ip, char *buf);
void table_deserialize(const char *buf, machine_t *t, int *nb, char *self_ip);
void table_print(machine_t *t, int nb);

#endif