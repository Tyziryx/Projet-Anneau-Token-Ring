#include "protocole.h"

#include <string.h>   
#include <stdio.h>

msg_t create_msg_TEXT(int source, int dest, char *data){
    msg_t msg;
    msg.type = TEXT;
    msg.source = source;
    msg.dest = dest;
    strncpy(msg.data, data, SMAX);
    msg.data[SMAX-1] = '\0'; 
    msg.size = strlen(msg.data);
    return msg;
}

msg_t create_msg_TOKEN(int source, int dest){
    msg_t msg;
    msg.type = TOKEN;
    msg.source = source;
    msg.dest = -1; // pas besoin;
    msg.data[0] = '\0'; 
    msg.size = 0;
    return msg;
}

msg_t create_msg_BROADCAST(int source, char *data){
    msg_t msg;
    msg.type = BROADCAST;
    msg.source = source;
    msg.dest = -1; // pas besoin
    strncpy(msg.data, data, SMAX);
    msg.data[SMAX-1] = '\0'; 
    msg.size = strlen(msg.data);
    return msg;
}

msg_t create_file_start(int source, int dest, const char *filename) {
    msg_t msg;

    msg.type = FILE_START;
    msg.source = source;
    msg.dest = dest;

    strncpy(msg.data, filename, SMAX);
    msg.data[SMAX - 1] = '\0';

    msg.size = strlen(msg.data);

    return msg;
}

msg_t create_file_data(int source, int dest, const char *part, int size) {
    msg_t msg;

    msg.type = FILE_DATA;
    msg.source = source;
    msg.dest = dest;

    if (size > SMAX) size = SMAX;

    memcpy(msg.data, part, size);
    msg.size = size;

    return msg;
}

msg_t create_file_end(int source, int dest) {
    msg_t msg;

    msg.type = FILE_END;
    msg.source = source;
    msg.dest = dest;
    msg.size = 0;

    return msg;
}