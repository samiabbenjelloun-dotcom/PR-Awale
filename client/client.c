#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "client.h"
#include "../awale.h"

static void init(void);
static void end(void);
static void app(const char *address, const char *name);
static int connect_to_server(const char *address);
static int send_all(SOCKET sock, const char *message);
static int send_chat_message(SOCKET sock, const char *input);

#ifdef WIN32
#include <conio.h>
#include <windows.h>
#endif

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
         perror("send()");
         return -1;
      }
      sent += (size_t)n;
   }
   return 0;
}

static int read_line(SOCKET sock, char *buffer, size_t capacity)
{
   size_t length = 0;
   while (1)
   {
      char character;
      int n = recv(sock, &character, 1, 0);
      if (n <= 0)
      {
         if (n < 0)
         {
            perror("recv()");
         }
         return n;
      }
      if (character == '\r')
      {
         continue;
      }
      if (character == '\n')
      {
         buffer[length] = '\0';
         return 1;
      }
      if (length >= capacity - 1)
      {
         fprintf(stderr, "Message received from server is too long.\n");
         return -1;
      }
      buffer[length++] = character;
   }
}

#ifdef WIN32
static int stdin_is_readable(void)
{
   HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
   DWORD mode;
   DWORD available = 0;
   if (GetConsoleMode(input, &mode))
   {
      return _kbhit();
   }
   if (PeekNamedPipe(input, NULL, 0, NULL, &available, NULL))
   {
      return available > 0;
   }
   return WaitForSingleObject(input, 0) == WAIT_OBJECT_0;
}
#endif

static int wait_for_input(SOCKET sock, int allow_stdin, int *socket_ready,
                          int *stdin_ready)
{
   while (1)
   {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(sock, &readable);
#ifdef WIN32
      struct timeval timeout = {0, 100000};
      int result = select(0, &readable, NULL, NULL, &timeout);
      *stdin_ready = allow_stdin && stdin_is_readable();
#else
      int max_fd = (int)sock;
      if (allow_stdin)
      {
         FD_SET(STDIN_FILENO, &readable);
         if (STDIN_FILENO > max_fd)
         {
            max_fd = STDIN_FILENO;
         }
      }
      int result = select(max_fd + 1, &readable, NULL, NULL, NULL);
      *stdin_ready = allow_stdin && result > 0 &&
                     FD_ISSET(STDIN_FILENO, &readable);
#endif
      if (result == SOCKET_ERROR)
      {
         perror("select()");
         return -1;
      }
      *socket_ready = result > 0 && FD_ISSET(sock, &readable);
      if (*socket_ready || *stdin_ready)
      {
         return 0;
      }
   }
}

static int send_lobby_command(SOCKET sock, const char *input)
{
   char name[32];
   char extra;
   char message[BUF_SIZE];

   if (strcmp(input, "list") == 0)
   {
      return send_all(sock, "LIST\n");
   }
   if (strncmp(input, "chat ", 5) == 0)
   {
      return send_chat_message(sock, input);
   }
   if (sscanf(input, "challenge %31s %c", name, &extra) == 1)
   {
      snprintf(message, sizeof message, "CHALLENGE %s\n", name);
      return send_all(sock, message);
   }
   if (sscanf(input, "accept %31s %c", name, &extra) == 1)
   {
      snprintf(message, sizeof message, "ACCEPT %s\n", name);
      return send_all(sock, message);
   }
   if (sscanf(input, "reject %31s %c", name, &extra) == 1)
   {
      snprintf(message, sizeof message, "REJECT %s\n", name);
      return send_all(sock, message);
   }
   if (sscanf(input, "observe %31s %c", name, &extra) == 1)
   {
      snprintf(message, sizeof message, "OBSERVE %s\n", name);
      return send_all(sock, message);
   }
   if (strcmp(input, "quit") == 0)
   {
      return 1;
   }

   puts("Commands: list, chat <message>, challenge <name>, accept <name>, reject <name>, observe <name>, quit");
   return 0;
}

static int send_chat_message(SOCKET sock, const char *input)
{
   const char *text = input + 5;
   char message[BUF_SIZE];
   int length;

   while (*text == ' ')
   {
      text++;
   }
   if (*text == '\0')
   {
      puts("Usage: chat <message>");
      return 0;
   }
   length = snprintf(message, sizeof message, "CHAT %s\n", text);
   if (length < 0 || (size_t)length >= sizeof message)
   {
      puts("Chat message is too long.");
      return 0;
   }
   return send_all(sock, message);
}

static int handle_server_message(const char *line, AwaleGame *game,
                                 int *game_active, int *player_id,
                                 int *move_pending, int *end_sent,
                                 int *spectating)
{
   if (strcmp(line, "PLAYERS_BEGIN") == 0)
   {
      puts("\nConnected players:");
      return 0;
   }
   if (strncmp(line, "PLAYER ", 7) == 0)
   {
      char name[32];
      char status[16];
      char extra;
      if (sscanf(line, "PLAYER %31s %15s %c", name, status, &extra) != 2)
      {
         fprintf(stderr, "Invalid player-list message from server.\n");
         return -1;
      }
      if (strcmp(status, "IN_GAME") == 0)
      {
         printf("  %s (in game)\n", name);
      }
      else if (strcmp(status, "AVAILABLE") == 0)
      {
         printf("  %s\n", name);
      }
      else if (strcmp(status, "SPECTATING") == 0)
      {
         printf("  %s (spectating)\n", name);
      }
      else
      {
         fprintf(stderr, "Invalid player status from server: %s\n", status);
         return -1;
      }
      return 0;
   }
   if (strcmp(line, "PLAYERS_END") == 0)
   {
      puts("Commands: list, chat <message>, challenge <name>, accept <name>, reject <name>, observe <name>, quit");
      return 0;
   }
   if (strncmp(line, "CHAT ", 5) == 0)
   {
      printf("\n[Chat] %s\n", line + 5);
      return 0;
   }
   if (strncmp(line, "CHALLENGE ", 10) == 0)
   {
      printf("\n%s challenges you. Type: accept %s or reject %s\n",
             line + 10, line + 10, line + 10);
      return 0;
   }
   if (strncmp(line, "CHALLENGE_SENT ", 15) == 0)
   {
      printf("Challenge sent to %s.\n", line + 15);
      return 0;
   }
   if (strncmp(line, "CHALLENGE_REJECTED ", 19) == 0)
   {
      printf("%s declined your challenge.\n", line + 19);
      return 0;
   }
   if (strncmp(line, "CHALLENGE_CANCELLED ", 20) == 0)
   {
      printf("Challenge from %s was cancelled.\n", line + 20);
      return 0;
   }
   if (strncmp(line, "ERROR ", 6) == 0)
   {
      *move_pending = 0;
      printf("Error: %s\n", line + 6);
      return 0;
   }
   if (strncmp(line, "START ", 6) == 0)
   {
      int rotation;
      char opponent[32];
      char extra;
      if (sscanf(line, "START %d %d %31s %c", player_id, &rotation,
                 opponent, &extra) != 3 ||
          (*player_id != 0 && *player_id != 1) ||
          (rotation != 0 && rotation != 1))
      {
         fprintf(stderr, "Invalid START message from server.\n");
         return -1;
      }
      init_game_with_rotation(game, rotation);
      *game_active = 1;
      *spectating = 0;
      *move_pending = 0;
      *end_sent = 0;
      printf("\nChallenge accepted. Game against %s; you are player %d.\n",
             opponent, *player_id);
      puts("During the game, type 'chat <message>' to chat at any time.");
      print_board(game);
      return 0;
   }
   if (strncmp(line, "SPECTATE_START ", 15) == 0)
   {
      int rotation;
      char player0[32];
      char player1[32];
      char extra;
      if (sscanf(line, "SPECTATE_START %d %31s %31s %c", &rotation,
                 player0, player1, &extra) != 3 ||
          (rotation != 0 && rotation != 1))
      {
         fprintf(stderr, "Invalid SPECTATE_START message from server.\n");
         return -1;
      }
      init_game_with_rotation(game, rotation);
      *game_active = 1;
      *spectating = 1;
      *player_id = -1;
      *move_pending = 0;
      *end_sent = 0;
      printf("\nWatching %s vs %s. Type 'stop' to return to the lobby.\n",
             player0, player1);
      print_board(game);
      return 0;
   }
   if (strncmp(line, "MOVE ", 5) == 0)
   {
      int move_player;
      int hole;
      char extra;
      if (!*game_active ||
          sscanf(line, "MOVE %d %d %c", &move_player, &hole, &extra) != 2 ||
          move_player != game->current_player ||
          hole < 1 || hole > NB_HOLES ||
          play_move(game, hole - 1) != 0)
      {
         fprintf(stderr, "Invalid move received from server: %s\n", line);
         return -1;
      }
      game->current_player = 1 - game->current_player;
      *move_pending = 0;
      putchar('\n');
      print_board(game);
      if (is_game_over(game) && *player_id == 0 && !*end_sent)
      {
         *end_sent = 1;
         return 2;
      }
      return 0;
   }
   if (strcmp(line, "GAME_OVER") == 0 ||
       strncmp(line, "GAME_OVER ", 10) == 0)
   {
      if (strncmp(line, "GAME_OVER ", 10) == 0)
      {
         printf("Game ended: %s\n", line + 10);
      }
      else
      {
         puts("Game ended.");
      }
      *game_active = 0;
      *spectating = 0;
      *move_pending = 0;
      return 0;
   }
   if (strcmp(line, "SPECTATE_STOPPED") == 0)
   {
      *game_active = 0;
      *spectating = 0;
      *move_pending = 0;
      puts("Stopped watching; returned to the lobby.");
      return 0;
   }

   fprintf(stderr, "Unknown server message: %s\n", line);
   return 0;
}

static int connect_to_server(const char *address)
{
   SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
   SOCKADDR_IN sin = {0};
   struct hostent *hostinfo;

   if (sock == INVALID_SOCKET)
   {
      perror("socket()");
      return INVALID_SOCKET;
   }
   hostinfo = gethostbyname(address);
   if (hostinfo == NULL || hostinfo->h_addr_list[0] == NULL)
   {
      fprintf(stderr, "Unknown host %s.\n", address);
      closesocket(sock);
      return INVALID_SOCKET;
   }
   memcpy(&sin.sin_addr, hostinfo->h_addr_list[0], (size_t)hostinfo->h_length);
   sin.sin_port = htons(PORT);
   sin.sin_family = AF_INET;

   if (connect(sock, (SOCKADDR *)&sin, sizeof sin) == SOCKET_ERROR)
   {
      perror("connect()");
      closesocket(sock);
      return INVALID_SOCKET;
   }
   return sock;
}

static void app(const char *address, const char *name)
{
   SOCKET sock = connect_to_server(address);
   if (sock == INVALID_SOCKET)
   {
      return;
   }

   char message[BUF_SIZE];
   snprintf(message, sizeof message, "HELLO %s\n", name);
   if (send_all(sock, message) != 0)
   {
      closesocket(sock);
      return;
   }

   AwaleGame game;
   int player_id = -1;
   int game_active = 0;
   int move_pending = 0;
   int end_sent = 0;
   int prompt_state = 0;
   int spectating = 0;
   int stop_pending = 0;

   puts("Connected to lobby. Use 'list', 'chat <message>', 'challenge <name>', or 'observe <name>'.");
   while (1)
   {
      int socket_ready;
      int stdin_ready;
      int can_enter_move = game_active && player_id == game.current_player &&
                           !move_pending;
      int allow_stdin = !game_active || !spectating || !stop_pending;
      if (wait_for_input(sock, allow_stdin, &socket_ready, &stdin_ready) != 0)
      {
         break;
      }

      if (socket_ready)
      {
         char line[BUF_SIZE];
         int result = read_line(sock, line, sizeof line);
         if (result <= 0)
         {
            puts("Server disconnected.");
            break;
         }
         int handled = handle_server_message(line, &game, &game_active,
                                             &player_id, &move_pending,
                                             &end_sent, &spectating);
         if (handled < 0)
         {
            break;
         }
         fflush(stdout);
         if (strncmp(line, "START ", 6) == 0 ||
             strncmp(line, "SPECTATE_START ", 15) == 0 ||
             strncmp(line, "MOVE ", 5) == 0 ||
             strncmp(line, "GAME_OVER", 9) == 0)
         {
            prompt_state = 0;
         }
         else if (strncmp(line, "CHAT ", 5) == 0)
         {
            prompt_state = 0;
         }
         if (strcmp(line, "SPECTATE_STOPPED") == 0 ||
             strncmp(line, "GAME_OVER", 9) == 0)
         {
            stop_pending = 0;
         }
         if (handled == 2 && send_all(sock, "END\n") != 0)
         {
            break;
         }
      }

      can_enter_move = game_active && player_id == game.current_player &&
                       !move_pending;
      if (can_enter_move && prompt_state != 1)
      {
         printf("Your turn (hole %d-%d or chat <message>): ",
                player_id * (NB_HOLES / 2) + 1,
                (player_id + 1) * (NB_HOLES / 2));
         fflush(stdout);
         prompt_state = 1;
      }
      else if (game_active && !spectating && !move_pending && !can_enter_move &&
               prompt_state != 2)
      {
         fputs("Waiting for opponent (chat <message>): ", stdout);
         fflush(stdout);
         prompt_state = 2;
      }

      if (stdin_ready && spectating)
      {
         char input[BUF_SIZE];
         if (fgets(input, sizeof input, stdin) == NULL)
         {
            break;
         }
         char *newline = strchr(input, '\n');
         if (newline != NULL)
         {
            *newline = '\0';
         }
         if (strcmp(input, "stop") == 0)
         {
            if (send_all(sock, "STOP_OBSERVING\n") != 0)
            {
               break;
            }
            stop_pending = 1;
         }
         else if (strcmp(input, "quit") == 0)
         {
            break;
         }
         else
         {
            puts("You are spectating. Type 'stop' to return to the lobby.");
         }
      }
      else if (stdin_ready && !game_active)
      {
         char input[BUF_SIZE];
         if (fgets(input, sizeof input, stdin) == NULL)
         {
            break;
         }
         char *newline = strchr(input, '\n');
         if (newline != NULL)
         {
            *newline = '\0';
         }
         else if (!feof(stdin))
         {
            int character;
            while ((character = getchar()) != '\n' && character != EOF)
            {
            }
            puts("Command is too long.");
            continue;
         }
         if (strcmp(input, "quit") == 0)
         {
            break;
         }
         if (send_lobby_command(sock, input) < 0)
         {
            break;
         }
      }
      else if (stdin_ready && game_active && !spectating)
      {
         char input[BUF_SIZE];
         int hole;
         char extra;
         if (fgets(input, sizeof input, stdin) == NULL)
         {
            break;
         }
         char *newline = strchr(input, '\n');
         if (newline != NULL)
         {
            *newline = '\0';
         }
         else if (!feof(stdin))
         {
            int character;
            while ((character = getchar()) != '\n' && character != EOF)
            {
            }
            puts("Input is too long.");
            continue;
         }

         if (strncmp(input, "chat ", 5) == 0)
         {
            if (send_chat_message(sock, input) != 0)
            {
               break;
            }
         }
         else if (!can_enter_move)
         {
            puts("Wait for your turn or enter 'chat <message>'.");
            prompt_state = 0;
         }
         else if (sscanf(input, "%d %c", &hole, &extra) != 1 ||
                  !is_valid_move(&game, hole - 1))
         {
            puts("Invalid move. Check the hole and feeding rule.");
            prompt_state = 0;
         }
         else
         {
            snprintf(message, sizeof message, "MOVE %d %d\n", player_id, hole);
            if (send_all(sock, message) != 0)
            {
               break;
            }
            move_pending = 1;
            prompt_state = 0;
         }
      }
   }

   closesocket(sock);
}

int main(int argc, char **argv)
{
   if (argc != 3 || strlen(argv[2]) > 31 || strchr(argv[2], ' ') != NULL)
   {
      fprintf(stderr, "Usage: %s <server-address> <name-without-spaces>\n",
              argv[0]);
      return EXIT_FAILURE;
   }

   init();
   app(argv[1], argv[2]);
   end();
   return EXIT_SUCCESS;
}
