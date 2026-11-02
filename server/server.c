#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

static int handle_line(Client *clients, int *active, int *pending_from,
                       int *pending_to, int *game_peer, int *role, int *turn,
                       int *spectating, GameHistory *history, int sender,
                       int rotation, const char *line)
{
   char name[32];
   char extra;

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
      game_peer[sender] = -1;
      game_peer[peer] = -1;
      clear_history(&history[history_owner]);
      broadcast_player_lists(clients, active, game_peer, spectating);
      return 0;
   }

   return write_error(clients, sender,
                      "commands: LIST, CHALLENGE <name>, ACCEPT <name>, REJECT <name>, OBSERVE <name>, STOP_OBSERVING");
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
                            rotation, input[i]) != 0)
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
