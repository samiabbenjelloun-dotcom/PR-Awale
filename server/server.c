#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "server.h"
#include "client.h"
#include "../awale.h"

static void init(void);
static void end(void);
static void app(int rotation);
static SOCKET init_connection(void);
static void end_connection(int sock);

typedef struct
{
   int player;
   int hole;
} MoveRecord;

typedef struct
{
   MoveRecord *moves;
   size_t count;
   size_t capacity;
} GameHistory;

typedef struct
{
   unsigned long id;
   int rotation;
   char player0[32];
   char player1[32];
   unsigned long move_count;
} SavedGame;

#define GAMES_FILE "games.txt"

static void init(void)
{
#ifdef WIN32
   WSADATA wsa;
   int err = WSAStartup(MAKEWORD(2, 2), &wsa);
   if (err != 0)
   {
      fprintf(stderr, "WSAStartup failed: %d\n", err);
      exit(EXIT_FAILURE);
   }
#endif
}

static void end(void)
{
#ifdef WIN32
   WSACleanup();
#endif
}

static int send_all(SOCKET sock, const char *message)
{
   size_t length = strlen(message);
   size_t sent = 0;
#ifdef MSG_NOSIGNAL
   const int flags = MSG_NOSIGNAL;
#else
   const int flags = 0;
#endif

   while (sent < length)
   {
      int n = send(sock, message + sent, (int)(length - sent), flags);
      if (n <= 0)
      {
         return -1;
      }
      sent += (size_t)n;
   }
   return 0;
}

static int find_player(Client *clients, int *active, const char *name)
{
   for (int i = 0; i < MAX_CLIENTS; i++)
   {
      if (active[i] && strcmp(clients[i].name, name) == 0)
      {
         return i;
      }
   }
   return -1;
}

static int find_name(char names[MAX_CLIENTS][32], int count, const char *name)
{
   for (int i = 0; i < count; i++)
   {
      if (strcmp(names[i], name) == 0)
      {
         return i;
      }
   }
   return -1;
}

static void remove_name(char names[MAX_CLIENTS][32], int *count,
                        const char *name)
{
   int index = find_name(names, *count, name);
   if (index < 0)
   {
      return;
   }
   for (int i = index; i + 1 < *count; i++)
   {
      strcpy(names[i], names[i + 1]);
   }
   (*count)--;
}

static int is_friend(const Client *player, const char *name)
{
   for (int i = 0; i < player->friend_count; i++)
   {
      if (strcmp(player->friends[i], name) == 0)
      {
         return 1;
      }
   }
   return 0;
}

static int can_observe_game(Client *clients, int observer, int player_a,
                            int player_b)
{
   if (!clients[player_a].private_mode && !clients[player_b].private_mode)
   {
      return 1;
   }
   return is_friend(&clients[player_a], clients[observer].name) ||
          is_friend(&clients[player_b], clients[observer].name);
}

static void revoke_disallowed_spectators(Client *clients, int *active,
                                         int *game_peer, int *spectating)
{
   for (int i = 0; i < MAX_CLIENTS; i++)
   {
      int game_owner = spectating[i];
      if (active[i] && game_owner >= 0 && game_peer[game_owner] >= 0 &&
          !can_observe_game(clients, i, game_owner, game_peer[game_owner]))
      {
         spectating[i] = -1;
         send_all(clients[i].sock, "SPECTATE_REVOKED\n");
      }
   }
}

static int valid_bio_line(const char *line)
{
   size_t length = strlen(line);
   if (length > BIO_LINE_MAX)
   {
      return 0;
   }
   for (size_t i = 0; i < length; i++)
   {
      unsigned char character = (unsigned char)line[i];
      if (character < 0x20 || character > 0x7e)
      {
         return 0;
      }
   }
   return 1;
}

static int record_move(GameHistory *history, int player, int hole)
{
   if (history->count == history->capacity)
   {
      size_t capacity = history->capacity == 0 ? 16 : history->capacity * 2;
      if (capacity < history->capacity ||
          capacity > SIZE_MAX / sizeof *history->moves)
      {
         return -1;
      }
      MoveRecord *moves = realloc(history->moves,
                                  capacity * sizeof *history->moves);
      if (moves == NULL)
      {
         return -1;
      }
      history->moves = moves;
      history->capacity = capacity;
   }
   history->moves[history->count].player = player;
   history->moves[history->count].hole = hole;
   history->count++;
   return 0;
}

static void clear_history(GameHistory *history)
{
   free(history->moves);
   history->moves = NULL;
   history->count = 0;
   history->capacity = 0;
}

static int read_saved_game(FILE *file, SavedGame *game, GameHistory *history)
{
   char line[BUF_SIZE];
   int version;
   char extra;
   unsigned long move_count;

   if (fgets(line, sizeof line, file) == NULL)
   {
      return 0;
   }
   if (sscanf(line, "GAME %d %lu %d %31s %31s %lu %c", &version,
              &game->id, &game->rotation, game->player0, game->player1,
              &move_count, &extra) != 6 ||
       version != 1 || (game->rotation != 0 && game->rotation != 1))
   {
      return -1;
   }
   game->move_count = move_count;

   for (size_t i = 0; i < game->move_count; i++)
   {
      int player;
      int hole;
      if (fgets(line, sizeof line, file) == NULL ||
          sscanf(line, "MOVE %d %d %c", &player, &hole, &extra) != 2 ||
          (player != 0 && player != 1) || hole < 1 || hole > NB_HOLES ||
          record_move(history, player, hole) != 0)
      {
         return -1;
      }
   }
   if (fgets(line, sizeof line, file) == NULL ||
       (strcmp(line, "END_GAME\n") != 0 &&
        strcmp(line, "END_GAME\r\n") != 0 &&
        strcmp(line, "END_GAME") != 0))
   {
      return -1;
   }
   return 1;
}

static unsigned long load_next_game_id(void)
{
   FILE *file = fopen(GAMES_FILE, "r");
   unsigned long max_id = 0;
   if (file == NULL)
   {
      return 1;
   }

   while (1)
   {
      SavedGame game = {0};
      GameHistory history = {0};
      int result = read_saved_game(file, &game, &history);
      clear_history(&history);
      if (result <= 0)
      {
         break;
      }
      if (game.id > max_id)
      {
         max_id = game.id;
      }
   }
   fclose(file);
   return max_id == ULONG_MAX ? 0 : max_id + 1;
}

static int save_game(unsigned long id, int rotation, const char *player0,
                     const char *player1, const GameHistory *history)
{
   FILE *file = fopen(GAMES_FILE, "a");
   if (file == NULL)
   {
      return -1;
   }

   int failed = fprintf(file, "GAME 1 %lu %d %s %s %lu\n", id, rotation,
                       player0, player1,
                       (unsigned long)history->count) < 0;
   for (size_t i = 0; !failed && i < history->count; i++)
   {
      failed = fprintf(file, "MOVE %d %d\n", history->moves[i].player,
                       history->moves[i].hole) < 0;
   }
   if (!failed && fputs("END_GAME\n", file) == EOF)
   {
      failed = 1;
   }
   if (fclose(file) != 0)
   {
      failed = 1;
   }
   return failed ? -1 : 0;
}

static int send_player_list(SOCKET sock, int recipient, Client *clients,
                            int *active, int *game_peer, int *spectating)
{
   if (send_all(sock, "PLAYERS_BEGIN\n") != 0)
   {
      return -1;
   }
   for (int i = 0; i < MAX_CLIENTS; i++)
   {
      char message[BUF_SIZE];
      if (i != recipient && active[i] && clients[i].name[0] != '\0')
      {
         const char *status = game_peer[i] >= 0 ? "IN_GAME" : spectating[i] >= 0 ? "SPECTATING"
                                                                                 : "AVAILABLE";
         snprintf(message, sizeof message, "PLAYER %s %s\n", clients[i].name,
                  status);
         if (send_all(sock, message) != 0)
         {
            return -1;
         }
      }
   }
   return send_all(sock, "PLAYERS_END\n");
}

static void broadcast_player_lists(Client *clients, int *active,
                                   int *game_peer, int *spectating)
{
   for (int i = 0; i < MAX_CLIENTS; i++)
   {
      if (active[i] && clients[i].name[0] != '\0' && game_peer[i] < 0 &&
          spectating[i] < 0 &&
          send_player_list(clients[i].sock, i, clients, active, game_peer,
                           spectating) != 0)
      {
         fprintf(stderr, "Could not update the player list for %s.\n",
                 clients[i].name);
      }
   }
}

static int write_error(Client *clients, int player, const char *message)
{
   char response[BUF_SIZE];
   snprintf(response, sizeof response, "ERROR %s\n", message);
   return send_all(clients[player].sock, response);
}

static int send_saved_games(Client *clients, int sender)
{
   FILE *file = fopen(GAMES_FILE, "r");
   int found_game = 0;
   if (send_all(clients[sender].sock, "GAMES_BEGIN\n") != 0)
   {
      if (file != NULL)
      {
         fclose(file);
      }
      return -1;
   }
   if (file == NULL)
   {
      return send_all(clients[sender].sock, "GAMES_EMPTY\nGAMES_END\n");
   }

   while (1)
   {
      SavedGame game = {0};
      GameHistory history = {0};
      int result = read_saved_game(file, &game, &history);
      if (result <= 0)
      {
         clear_history(&history);
         if (result < 0 &&
             send_all(clients[sender].sock,
                      "ERROR saved game archive is invalid\n") != 0)
         {
            fclose(file);
            return -1;
         }
         break;
      }
      found_game = 1;
      char message[BUF_SIZE];
      snprintf(message, sizeof message, "SAVED_GAME %lu %d %s %s %lu\n",
               game.id, game.rotation, game.player0, game.player1,
               game.move_count);
      clear_history(&history);
      if (send_all(clients[sender].sock, message) != 0)
      {
         fclose(file);
         return -1;
      }
   }
   fclose(file);
   if (!found_game &&
       send_all(clients[sender].sock, "GAMES_EMPTY\n") != 0)
   {
      return -1;
   }
   return send_all(clients[sender].sock, "GAMES_END\n");
}

static int send_game_review(Client *clients, int sender, unsigned long id)
{
   FILE *file = fopen(GAMES_FILE, "r");
   if (file == NULL)
   {
      return write_error(clients, sender, "no saved games were found");
   }

   while (1)
   {
      SavedGame game = {0};
      GameHistory history = {0};
      int result = read_saved_game(file, &game, &history);
      if (result <= 0)
      {
         clear_history(&history);
         fclose(file);
         return write_error(clients, sender,
                            result < 0 ? "saved game archive is invalid"
                                       : "saved game not found");
      }
      if (game.id == id)
      {
         char message[BUF_SIZE];
         snprintf(message, sizeof message,
                  "REVIEW_BEGIN %lu %d %s %s %lu\n", game.id,
                  game.rotation, game.player0, game.player1, game.move_count);
         if (send_all(clients[sender].sock, message) != 0)
         {
            clear_history(&history);
            fclose(file);
            return -1;
         }
         for (size_t i = 0; i < history.count; i++)
         {
            snprintf(message, sizeof message, "REVIEW_MOVE %d %d\n",
                     history.moves[i].player, history.moves[i].hole);
            if (send_all(clients[sender].sock, message) != 0)
            {
               clear_history(&history);
               fclose(file);
               return -1;
            }
         }
         clear_history(&history);
         fclose(file);
         return send_all(clients[sender].sock, "REVIEW_END\n");
      }
      clear_history(&history);
   }
}

static int handle_line(Client *clients, int *active, int *pending_from,
                       int *pending_to, int *game_peer, int *role, int *turn,
                       int *spectating, GameHistory *history, int sender,
                       int rotation, unsigned long *next_game_id,
                       const char *line)
{
   char name[32];
   char extra;
   unsigned long game_id;

   if (clients[sender].name[0] == '\0')
   {
      if (sscanf(line, "HELLO %31s %c", name, &extra) != 1 ||
          strlen(name) >= sizeof clients[sender].name)
      {
         return write_error(clients, sender,
                            "send HELLO followed by a name (maximum 31 characters)");
      }
      if (find_player(clients, active, name) >= 0)
      {
         return write_error(clients, sender, "that name is already in use");
      }
      strcpy(clients[sender].name, name);
      printf("%s connected.\n", name);
      broadcast_player_lists(clients, active, game_peer, spectating);
      return 0;
   }

   if (strcmp(line, "BIO_SET_BEGIN") == 0)
   {
      if (game_peer[sender] >= 0 || spectating[sender] >= 0)
      {
         return write_error(clients, sender, "you must be in the lobby to edit your bio");
      }
      clients[sender].bio_editing = 1;
      clients[sender].pending_bio_line_count = 0;
      return 0;
   }
   if (clients[sender].bio_editing)
   {
      if (strncmp(line, "BIO_LINE ", 9) == 0)
      {
         const char *bio_line = line + 9;
         if (clients[sender].pending_bio_line_count >= BIO_MAX_LINES ||
             !valid_bio_line(bio_line))
         {
            clients[sender].bio_editing = 0;
            clients[sender].pending_bio_line_count = 0;
            return write_error(clients, sender,
                               "bio must have at most 10 printable ASCII lines of 80 characters each");
         }
         strcpy(clients[sender].pending_bio[
                    clients[sender].pending_bio_line_count++], bio_line);
         return 0;
      }
      if (strcmp(line, "BIO_SET_END") == 0)
      {
         if (clients[sender].pending_bio_line_count == 0)
         {
            clients[sender].bio_editing = 0;
            return write_error(clients, sender, "a bio must contain at least one line");
         }
         clients[sender].bio_line_count = clients[sender].pending_bio_line_count;
         memcpy(clients[sender].bio, clients[sender].pending_bio,
                sizeof clients[sender].bio);
         clients[sender].bio_editing = 0;
         clients[sender].pending_bio_line_count = 0;
         return send_all(clients[sender].sock, "BIO_SAVED\n");
      }
      return write_error(clients, sender, "finish editing your bio first");
   }
   if (strncmp(line, "BIO_LINE ", 9) == 0 ||
       strcmp(line, "BIO_SET_END") == 0)
   {
      return write_error(clients, sender, "no bio update is in progress");
   }
   if (sscanf(line, "BIO_GET %31s %c", name, &extra) == 1)
   {
      int target = find_player(clients, active, name);
      if (target < 0)
      {
         return write_error(clients, sender, "player not found");
      }
      if (clients[target].bio_line_count == 0)
      {
         char message[BUF_SIZE];
         snprintf(message, sizeof message, "BIO_EMPTY %s\n", name);
         return send_all(clients[sender].sock, message);
      }
      char message[BUF_SIZE];
      snprintf(message, sizeof message, "BIO_BEGIN %s\n", name);
      if (send_all(clients[sender].sock, message) != 0)
      {
         return -1;
      }
      for (int i = 0; i < clients[target].bio_line_count; i++)
      {
         snprintf(message, sizeof message, "BIO_LINE %s\n",
                  clients[target].bio[i]);
         if (send_all(clients[sender].sock, message) != 0)
         {
            return -1;
         }
      }
      return send_all(clients[sender].sock, "BIO_END\n");
   }

   if (strcmp(line, "FRIENDS") == 0)
   {
      if (send_all(clients[sender].sock, "FRIENDS_BEGIN\n") != 0)
      {
         return -1;
      }
      for (int i = 0; i < clients[sender].friend_count; i++)
      {
         char message[BUF_SIZE];
         snprintf(message, sizeof message, "FRIEND %s\n",
                  clients[sender].friends[i]);
         if (send_all(clients[sender].sock, message) != 0)
         {
            return -1;
         }
      }
      return send_all(clients[sender].sock, "FRIENDS_END\n");
   }
   if (strcmp(line, "FRIEND_REQUESTS") == 0)
   {
      if (send_all(clients[sender].sock, "FRIEND_REQUESTS_BEGIN\n") != 0)
      {
         return -1;
      }
      for (int i = 0; i < clients[sender].friend_request_count; i++)
      {
         char message[BUF_SIZE];
         snprintf(message, sizeof message, "FRIEND_REQUEST_FROM %s\n",
                  clients[sender].friend_requests[i]);
         if (send_all(clients[sender].sock, message) != 0)
         {
            return -1;
         }
      }
      return send_all(clients[sender].sock, "FRIEND_REQUESTS_END\n");
   }
   if (sscanf(line, "FRIEND_ADD %31s %c", name, &extra) == 1)
   {
      int target = find_player(clients, active, name);
      char message[BUF_SIZE];
      if (target < 0)
      {
         return write_error(clients, sender, "that player is not connected");
      }
      if (target == sender)
      {
         return write_error(clients, sender, "you cannot add yourself as a friend");
      }
      if (is_friend(&clients[sender], name))
      {
         return write_error(clients, sender, "that player is already your friend");
      }
      if (find_name(clients[sender].friend_requests,
              clients[sender].friend_request_count, name) >= 0)
      {
         return write_error(clients, sender,
                            "this player already sent you a request; accept it instead");
      }
      if (find_name(clients[target].friend_requests,
                    clients[target].friend_request_count,
                    clients[sender].name) >= 0)
      {
         return write_error(clients, sender, "a friend request is already pending");
      }
      if (clients[target].friend_request_count >= MAX_CLIENTS)
      {
         return write_error(clients, sender,
                            "that player's friend request list is full");
      }
      strcpy(clients[target].friend_requests[
                 clients[target].friend_request_count++], clients[sender].name);
      snprintf(message, sizeof message, "FRIEND_REQUEST %s\n",
               clients[sender].name);
      if (send_all(clients[target].sock, message) != 0)
      {
         return -1;
      }
      snprintf(message, sizeof message, "FRIEND_REQUEST_SENT %s\n", name);
      return send_all(clients[sender].sock, message);
   }
   if (sscanf(line, "FRIEND_ACCEPT %31s %c", name, &extra) == 1)
   {
      int requester = find_player(clients, active, name);
      if (find_name(clients[sender].friend_requests,
                    clients[sender].friend_request_count, name) < 0)
      {
         return write_error(clients, sender,
                            "there is no friend request from that player");
      }
      if (requester < 0)
      {
         remove_name(clients[sender].friend_requests,
                     &clients[sender].friend_request_count, name);
         return write_error(clients, sender,
                            "that player is no longer connected");
      }
      if (clients[sender].friend_count >= MAX_CLIENTS ||
          clients[requester].friend_count >= MAX_CLIENTS)
      {
         return write_error(clients, sender, "a friend list is full");
      }
      remove_name(clients[sender].friend_requests,
                  &clients[sender].friend_request_count, name);
      strcpy(clients[sender].friends[clients[sender].friend_count++], name);
      strcpy(clients[requester].friends[
                 clients[requester].friend_count++], clients[sender].name);
      char message[BUF_SIZE];
      snprintf(message, sizeof message, "FRIEND_ADDED %s\n", name);
      if (send_all(clients[sender].sock, message) != 0)
      {
         return -1;
      }
      snprintf(message, sizeof message, "FRIEND_ADDED %s\n",
               clients[sender].name);
      return send_all(clients[requester].sock, message);
   }
   if (sscanf(line, "FRIEND_REJECT %31s %c", name, &extra) == 1)
   {
      char message[BUF_SIZE];
      int requester = find_player(clients, active, name);
      if (find_name(clients[sender].friend_requests,
                    clients[sender].friend_request_count, name) < 0)
      {
         return write_error(clients, sender,
                            "there is no friend request from that player");
      }
      remove_name(clients[sender].friend_requests,
                  &clients[sender].friend_request_count, name);
      if (requester >= 0)
      {
         snprintf(message, sizeof message, "FRIEND_REQUEST_REJECTED %s\n",
                  clients[sender].name);
         if (send_all(clients[requester].sock, message) != 0)
         {
            return -1;
         }
      }
      snprintf(message, sizeof message, "FRIEND_REQUEST_DECLINED %s\n",
            requester >= 0 ? clients[requester].name : name);
      return send_all(clients[sender].sock, message);
   }
   if (sscanf(line, "FRIEND_REMOVE %31s %c", name, &extra) == 1)
   {
      char message[BUF_SIZE];
      if (find_name(clients[sender].friends, clients[sender].friend_count,
                    name) < 0)
      {
         return write_error(clients, sender, "that player is not your friend");
      }
      int target = find_player(clients, active, name);
      remove_name(clients[sender].friends, &clients[sender].friend_count, name);
      if (target >= 0)
      {
         remove_name(clients[target].friends, &clients[target].friend_count,
                     clients[sender].name);
      }
      revoke_disallowed_spectators(clients, active, game_peer, spectating);
      broadcast_player_lists(clients, active, game_peer, spectating);
      snprintf(message, sizeof message, "FRIEND_REMOVED %s\n", name);
      if (send_all(clients[sender].sock, message) != 0)
      {
         return -1;
      }
      if (target >= 0)
      {
         snprintf(message, sizeof message, "FRIEND_REMOVED %s\n",
                  clients[sender].name);
         return send_all(clients[target].sock, message);
      }
      return 0;
   }
   if (sscanf(line, "SET_PRIVATE %3s %c", name, &extra) == 1)
   {
      if (strcmp(name, "ON") != 0 && strcmp(name, "OFF") != 0)
      {
         return write_error(clients, sender, "use private on or private off");
      }
      clients[sender].private_mode = strcmp(name, "ON") == 0;
      revoke_disallowed_spectators(clients, active, game_peer, spectating);
      broadcast_player_lists(clients, active, game_peer, spectating);
      char message[BUF_SIZE];
      snprintf(message, sizeof message, "PRIVATE %s\n", name);
      return send_all(clients[sender].sock, message);
   }

   if (strcmp(line, "GAMES") == 0)
   {
      if (game_peer[sender] >= 0 || spectating[sender] >= 0)
      {
         return write_error(clients, sender, "you must be in the lobby");
      }
      return send_saved_games(clients, sender);
   }
   if (sscanf(line, "REVIEW %lu %c", &game_id, &extra) == 1)
   {
      if (game_peer[sender] >= 0 || spectating[sender] >= 0)
      {
         return write_error(clients, sender, "you must be in the lobby");
      }
      return send_game_review(clients, sender, game_id);
   }

   if (strcmp(line, "LIST") == 0)
   {
      if (game_peer[sender] >= 0 || spectating[sender] >= 0)
      {
         return write_error(clients, sender, "you are currently busy");
      }
      return send_player_list(clients[sender].sock, sender, clients, active,
                              game_peer, spectating);
   }

   if (strncmp(line, "CHAT ", 5) == 0)
   {
      const char *text = line + 5;
      char message[BUF_SIZE];
      int length;
      int game_owner = -1;
      while (*text == ' ')
      {
         text++;
      }
      if (*text == '\0')
      {
         return write_error(clients, sender, "chat message cannot be empty");
      }
      if (spectating[sender] >= 0)
      {
         return write_error(clients, sender,
                            "spectators cannot send chat messages");
      }
      if (game_peer[sender] >= 0)
      {
         game_owner = role[sender] == 0 ? sender : game_peer[sender];
      }
      length = snprintf(message, sizeof message, "CHAT %s: %s\n",
                        clients[sender].name, text);
      if (length < 0 || (size_t)length >= sizeof message)
      {
         return write_error(clients, sender, "chat message is too long");
      }
      for (int i = 0; i < MAX_CLIENTS; i++)
      {
         int recipient_in_lobby = game_owner < 0 && game_peer[i] < 0 &&
                                  spectating[i] < 0;
         int recipient_in_game = game_owner >= 0 &&
                                 (i == game_owner ||
                                  i == game_peer[game_owner] ||
                                  spectating[i] == game_owner);
         if (active[i] && clients[i].name[0] != '\0' &&
             (recipient_in_lobby || recipient_in_game) &&
             send_all(clients[i].sock, message) != 0)
         {
            fprintf(stderr, "Could not deliver a chat message to %s.\n",
                    clients[i].name);
         }
      }
      return 0;
   }

   if (sscanf(line, "CHALLENGE %31s %c", name, &extra) == 1)
   {
      int target = find_player(clients, active, name);
      char message[BUF_SIZE];
      if (target < 0)
      {
         return write_error(clients, sender, "player not found");
      }
      if (target == sender)
      {
         return write_error(clients, sender, "you cannot challenge yourself");
      }
      if (game_peer[sender] >= 0 || game_peer[target] >= 0 ||
          spectating[sender] >= 0 || spectating[target] >= 0 ||
          pending_from[sender] >= 0 || pending_to[sender] >= 0 ||
          pending_from[target] >= 0 || pending_to[target] >= 0)
      {
         return write_error(clients, sender, "one of these players is already busy");
      }
      pending_from[target] = sender;
      pending_to[sender] = target;
      snprintf(message, sizeof message, "CHALLENGE %s\n",
               clients[sender].name);
      if (send_all(clients[target].sock, message) != 0)
      {
         return -1;
      }
      snprintf(message, sizeof message, "CHALLENGE_SENT %s\n", name);
      return send_all(clients[sender].sock, message);
   }

   if (sscanf(line, "ACCEPT %31s %c", name, &extra) == 1 ||
       sscanf(line, "REJECT %31s %c", name, &extra) == 1)
   {
      int is_accept = strncmp(line, "ACCEPT ", 7) == 0;
      int challenger = find_player(clients, active, name);
      char message[BUF_SIZE];
      if (challenger < 0 || pending_from[sender] != challenger)
      {
         return write_error(clients, sender, "no pending challenge from that player");
      }
      pending_from[sender] = -1;
      pending_to[challenger] = -1;
      if (!is_accept)
      {
         snprintf(message, sizeof message, "CHALLENGE_REJECTED %s\n",
                  clients[sender].name);
         return send_all(clients[challenger].sock, message);
      }

      pending_from[challenger] = -1;
      game_peer[sender] = challenger;
      game_peer[challenger] = sender;
      clear_history(&history[challenger]);
      role[challenger] = 0;
      role[sender] = 1;
      turn[challenger] = 0;
      turn[sender] = 0;

      snprintf(message, sizeof message, "START 0 %d %s\n", rotation,
               clients[sender].name);
      if (send_all(clients[challenger].sock, message) != 0)
      {
         return -1;
      }
      snprintf(message, sizeof message, "START 1 %d %s\n", rotation,
               clients[challenger].name);
      if (send_all(clients[sender].sock, message) != 0)
      {
         return -1;
      }
      broadcast_player_lists(clients, active, game_peer, spectating);
      printf("%s challenged %s: game started.\n",
             clients[challenger].name, clients[sender].name);
      return 0;
   }

   int player;
   int hole;
   if (game_peer[sender] >= 0 &&
       sscanf(line, "MOVE %d %d %c", &player, &hole, &extra) == 2)
   {
      int peer = game_peer[sender];
      char message[BUF_SIZE];
      if (player != role[sender] || hole < 1 || hole > NB_HOLES ||
          (role[sender] == 0 && hole > NB_HOLES / 2) ||
          (role[sender] == 1 && hole <= NB_HOLES / 2))
      {
         return write_error(clients, sender, "invalid player or hole");
      }
      if (player != turn[sender])
      {
         return write_error(clients, sender, "it is not your turn");
      }

      int history_owner = role[sender] == 0 ? sender : peer;
      if (record_move(&history[history_owner], player, hole) != 0)
      {
         return write_error(clients, sender,
                            "server could not record the move for observers");
      }
      snprintf(message, sizeof message, "MOVE %d %d\n", player, hole);
      if (send_all(clients[sender].sock, message) != 0 ||
          send_all(clients[peer].sock, message) != 0)
      {
         return -1;
      }
      for (int i = 0; i < MAX_CLIENTS; i++)
      {
         if (active[i] && spectating[i] == history_owner &&
             send_all(clients[i].sock, message) != 0)
         {
            fprintf(stderr, "Could not relay a move to observer %s.\n",
                    clients[i].name);
         }
      }
      turn[sender] = 1 - player;
      turn[peer] = 1 - player;
      return 0;
   }

   if (sscanf(line, "OBSERVE %31s %c", name, &extra) == 1)
   {
      int target = find_player(clients, active, name);
      if (game_peer[sender] >= 0 || spectating[sender] >= 0 ||
          pending_from[sender] >= 0 || pending_to[sender] >= 0)
      {
         return write_error(clients, sender, "you must be available to observe");
      }
      if (target < 0 || game_peer[target] < 0)
      {
         return write_error(clients, sender, "that player is not in a game");
      }

      int history_owner = role[target] == 0 ? target : game_peer[target];
      int history_opponent = game_peer[history_owner];
      if (!can_observe_game(clients, sender, history_owner, history_opponent))
      {
         return write_error(clients, sender, "this game is private");
      }
      char message[BUF_SIZE];
      spectating[sender] = history_owner;
      snprintf(message, sizeof message, "SPECTATE_START %d %s %s\n",
               rotation, clients[history_owner].name,
               clients[history_opponent].name);
      if (send_all(clients[sender].sock, message) != 0)
      {
         return -1;
      }
      for (size_t i = 0; i < history[history_owner].count; i++)
      {
         snprintf(message, sizeof message, "MOVE %d %d\n",
                  history[history_owner].moves[i].player,
                  history[history_owner].moves[i].hole);
         if (send_all(clients[sender].sock, message) != 0)
         {
            return -1;
         }
      }
      broadcast_player_lists(clients, active, game_peer, spectating);
      return 0;
   }

   if (strcmp(line, "STOP_OBSERVING") == 0 && spectating[sender] >= 0)
   {
      spectating[sender] = -1;
      if (send_all(clients[sender].sock, "SPECTATE_STOPPED\n") != 0)
      {
         return -1;
      }
      broadcast_player_lists(clients, active, game_peer, spectating);
      return 0;
   }

   if (strcmp(line, "END") == 0 && game_peer[sender] >= 0)
   {
      int peer = game_peer[sender];
      int history_owner = role[sender] == 0 ? sender : peer;
      unsigned long saved_game_id = *next_game_id;
      int saved = saved_game_id != 0 &&
                  save_game(saved_game_id, rotation,
                            clients[history_owner].name,
                            clients[game_peer[history_owner]].name,
                            &history[history_owner]) == 0;
      if (saved)
      {
         *next_game_id = saved_game_id == ULONG_MAX ? 0 : saved_game_id + 1;
      }
      else
      {
         fprintf(stderr, "Could not save game %lu to %s.\n", saved_game_id,
                 GAMES_FILE);
      }
      if (send_all(clients[sender].sock, "GAME_OVER\n") != 0 ||
          send_all(clients[peer].sock, "GAME_OVER\n") != 0)
      {
         return -1;
      }
      for (int i = 0; i < MAX_CLIENTS; i++)
      {
         if (active[i] && spectating[i] == history_owner)
         {
            if (send_all(clients[i].sock, "GAME_OVER\n") != 0)
            {
               fprintf(stderr, "Could not notify observer %s that the game ended.\n",
                       clients[i].name);
            }
            spectating[i] = -1;
         }
      }
      char save_message[BUF_SIZE];
      if (saved)
      {
         snprintf(save_message, sizeof save_message, "GAME_SAVED %lu\n",
                  saved_game_id);
      }
      else
      {
         strcpy(save_message, "GAME_SAVE_FAILED\n");
      }
      send_all(clients[sender].sock, save_message);
      send_all(clients[peer].sock, save_message);
      game_peer[sender] = -1;
      game_peer[peer] = -1;
      clear_history(&history[history_owner]);
      broadcast_player_lists(clients, active, game_peer, spectating);
      return 0;
   }

   return write_error(clients, sender,
                      "commands: LIST, GAMES, REVIEW <id>, CHALLENGE <name>, ACCEPT <name>, REJECT <name>, OBSERVE <name>, STOP_OBSERVING");
}

static void disconnect_player(Client *clients, int *active, int *pending_from,
                              int *pending_to, int *game_peer, int *role,
                              int *spectating, GameHistory *history, int player)
{
   char message[BUF_SIZE];
   int peer = game_peer[player];
   int challenger = pending_from[player];
   int history_owner = -1;

   if (peer >= 0 && active[peer])
   {
      game_peer[peer] = -1;
      send_all(clients[peer].sock, "GAME_OVER opponent disconnected\n");
      history_owner = role[player] == 0 ? player : peer;
   }
   if (history_owner >= 0)
   {
      for (int i = 0; i < MAX_CLIENTS; i++)
      {
         if (active[i] && spectating[i] == history_owner)
         {
            send_all(clients[i].sock, "GAME_OVER opponent disconnected\n");
            spectating[i] = -1;
         }
      }
      clear_history(&history[history_owner]);
   }
   if (challenger >= 0 && active[challenger])
   {
      snprintf(message, sizeof message, "CHALLENGE_CANCELLED %s\n",
               clients[player].name);
      send_all(clients[challenger].sock, message);
      pending_to[challenger] = -1;
   }
   int target = pending_to[player];
   if (target >= 0 && active[target])
   {
      snprintf(message, sizeof message, "CHALLENGE_CANCELLED %s\n",
               clients[player].name);
      send_all(clients[target].sock, message);
      pending_from[target] = -1;
   }
   for (int i = 0; i < MAX_CLIENTS; i++)
   {
      if (pending_from[i] == player)
      {
         pending_from[i] = -1;
      }
   }

   for (int i = 0; i < MAX_CLIENTS; i++)
   {
      if (i == player || !active[i])
      {
         continue;
      }
      if (is_friend(&clients[i], clients[player].name))
      {
         remove_name(clients[i].friends, &clients[i].friend_count,
                     clients[player].name);
         snprintf(message, sizeof message, "FRIEND_REMOVED %s\n",
                  clients[player].name);
         send_all(clients[i].sock, message);
      }
      if (find_name(clients[player].friend_requests,
                    clients[player].friend_request_count,
                    clients[i].name) >= 0)
      {
         snprintf(message, sizeof message, "FRIEND_REQUEST_CANCELLED %s\n",
                  clients[player].name);
         send_all(clients[i].sock, message);
      }
      if (find_name(clients[i].friend_requests,
                    clients[i].friend_request_count,
                    clients[player].name) >= 0)
      {
         remove_name(clients[i].friend_requests,
                     &clients[i].friend_request_count, clients[player].name);
         snprintf(message, sizeof message, "FRIEND_REQUEST_CANCELLED %s\n",
                  clients[player].name);
         send_all(clients[i].sock, message);
      }
   }
   clients[player].friend_request_count = 0;

   printf("%s disconnected.\n", clients[player].name);
   closesocket(clients[player].sock);
   active[player] = 0;
   clients[player].name[0] = '\0';
   pending_from[player] = -1;
   pending_to[player] = -1;
   game_peer[player] = -1;
   spectating[player] = -1;
   broadcast_player_lists(clients, active, game_peer, spectating);
}

static void app(int rotation)
{
   SOCKET listener = init_connection();
   Client clients[MAX_CLIENTS] = {0};
   int active[MAX_CLIENTS] = {0};
   int pending_from[MAX_CLIENTS];
   int pending_to[MAX_CLIENTS];
   int game_peer[MAX_CLIENTS];
   int spectating[MAX_CLIENTS];
   GameHistory history[MAX_CLIENTS] = {{0}};
   int role[MAX_CLIENTS] = {0};
   int turn[MAX_CLIENTS] = {0};
   char input[MAX_CLIENTS][BUF_SIZE] = {{0}};
   size_t input_length[MAX_CLIENTS] = {0};
   unsigned long next_game_id = load_next_game_id();

   for (int i = 0; i < MAX_CLIENTS; i++)
   {
      pending_from[i] = -1;
      pending_to[i] = -1;
      game_peer[i] = -1;
      spectating[i] = -1;
   }
   printf("Lobby listening on port %d. Rotation: %s.\n", PORT,
          rotation == 0 ? "clockwise" : "anticlockwise");

   while (1)
   {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(listener, &readable);
      SOCKET max_socket = listener;
      for (int i = 0; i < MAX_CLIENTS; i++)
      {
         if (active[i])
         {
            FD_SET(clients[i].sock, &readable);
            if (clients[i].sock > max_socket)
            {
               max_socket = clients[i].sock;
            }
         }
      }
#ifdef WIN32
      int ready = select(0, &readable, NULL, NULL, NULL);
#else
      int ready = select((int)max_socket + 1, &readable, NULL, NULL, NULL);
#endif
      if (ready == SOCKET_ERROR)
      {
         perror("select()");
         break;
      }

      if (FD_ISSET(listener, &readable))
      {
         SOCKADDR_IN address = {0};
#ifdef WIN32
         int address_length = sizeof address;
#else
         socklen_t address_length = sizeof address;
#endif
         SOCKET client_sock = accept(listener, (SOCKADDR *)&address,
                                     &address_length);
         if (client_sock == INVALID_SOCKET)
         {
            perror("accept()");
         }
         else
         {
            int slot = -1;
            for (int i = 0; i < MAX_CLIENTS; i++)
            {
               if (!active[i])
               {
                  slot = i;
                  break;
               }
            }
            if (slot < 0)
            {
               send_all(client_sock, "ERROR server is full\n");
               closesocket(client_sock);
            }
            else
            {
               clients[slot].sock = client_sock;
               clients[slot].name[0] = '\0';
               clients[slot].bio_line_count = 0;
               clients[slot].pending_bio_line_count = 0;
               clients[slot].bio_editing = 0;
               clients[slot].friend_count = 0;
               clients[slot].friend_request_count = 0;
               clients[slot].private_mode = 0;
               active[slot] = 1;
               input_length[slot] = 0;
            }
         }
      }

      for (int i = 0; i < MAX_CLIENTS; i++)
      {
         if (!active[i] || !FD_ISSET(clients[i].sock, &readable))
         {
            continue;
         }

         char chunk[BUF_SIZE];
         int received = recv(clients[i].sock, chunk, sizeof chunk, 0);
         if (received <= 0)
         {
            if (received < 0)
            {
               perror("recv()");
            }
            disconnect_player(clients, active, pending_from, pending_to,
                              game_peer, role, spectating, history, i);
            continue;
         }

         for (int j = 0; j < received; j++)
         {
            char character = chunk[j];
            if (character == '\r')
            {
               continue;
            }
            if (character != '\n')
            {
               if (input_length[i] >= sizeof input[i] - 1)
               {
                  send_all(clients[i].sock, "ERROR message too long\n");
                  disconnect_player(clients, active, pending_from, pending_to,
                                    game_peer, role, spectating, history, i);
                  break;
               }
               input[i][input_length[i]++] = character;
               continue;
            }

            input[i][input_length[i]] = '\0';
            if (handle_line(clients, active, pending_from, pending_to,
                            game_peer, role, turn, spectating, history, i,
                            rotation, &next_game_id, input[i]) != 0)
            {
               disconnect_player(clients, active, pending_from, pending_to,
                                 game_peer, role, spectating, history, i);
               break;
            }
            input_length[i] = 0;
            if (!active[i])
            {
               break;
            }
         }
      }
   }

   for (int i = 0; i < MAX_CLIENTS; i++)
   {
      clear_history(&history[i]);
      if (active[i])
      {
         closesocket(clients[i].sock);
      }
   }
   end_connection(listener);
}

static SOCKET init_connection(void)
{
   SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
   SOCKADDR_IN address = {0};

   if (sock == INVALID_SOCKET)
   {
      perror("socket()");
      exit(EXIT_FAILURE);
   }
   address.sin_addr.s_addr = htonl(INADDR_ANY);
   address.sin_port = htons(PORT);
   address.sin_family = AF_INET;

   if (bind(sock, (SOCKADDR *)&address, sizeof address) == SOCKET_ERROR)
   {
      perror("bind()");
      closesocket(sock);
      exit(EXIT_FAILURE);
   }
   if (listen(sock, MAX_CLIENTS) == SOCKET_ERROR)
   {
      perror("listen()");
      closesocket(sock);
      exit(EXIT_FAILURE);
   }
   return sock;
}

static void end_connection(int sock)
{
   closesocket(sock);
}

int main(int argc, char **argv)
{
   int rotation = 0;
   if (argc > 2 ||
       (argc == 2 && strcmp(argv[1], "0") != 0 && strcmp(argv[1], "1") != 0))
   {
      fprintf(stderr, "Usage: %s [rotation: 0 clockwise, 1 anticlockwise]\n",
              argv[0]);
      return EXIT_FAILURE;
   }
   if (argc == 2)
   {
      rotation = argv[1][0] - '0';
   }

   init();
   app(rotation);
   end();
   return EXIT_SUCCESS;
}
