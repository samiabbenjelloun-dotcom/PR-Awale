#ifndef CLIENT_H
#define CLIENT_H

#include "server.h"

typedef struct
{
   SOCKET sock;
   char name[BUF_SIZE];
   char bio[BIO_MAX_LINES][BIO_LINE_MAX + 1];
   int bio_line_count;
   char pending_bio[BIO_MAX_LINES][BIO_LINE_MAX + 1];
   int pending_bio_line_count;
   int bio_editing;
   char friends[MAX_CLIENTS][32];
   int friend_count;
   char friend_requests[MAX_CLIENTS][32];
   int friend_request_count;
   int private_mode;
} Client;

#endif /* guard */