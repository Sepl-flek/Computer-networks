#ifndef MESSAGE_EX_H
#define MESSAGE_EX_H

#include <stdint.h>
#include <time.h>

#define MAX_NAME 32
#define MAX_PAYLOAD 256

typedef struct {
    uint32_t length;
    uint8_t type;
    uint32_t msg_id;
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    time_t timestamp;
    char payload[MAX_PAYLOAD];
} MessageEx;

#endif