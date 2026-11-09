# PR-Awale

A terminal-based Awale game written in C for two players.

## Prerequisites

You need a C compiler, such as GCC.

## Build and run

From the project directory, compile the two source files:

```bash
gcc -Wall -Wextra -o awale main.c awale.c
```

Then run the program:

```bash
./awale
```

On Windows with MinGW, compilation creates `awale.exe`. Run it with:

```powershell
.\awale.exe
```

At startup, choose player 0's direction: `0` for clockwise or `1` for anticlockwise. Player 1 plays in the opposite direction. Then enter the number of the hole to play when prompted.

## Main files

- `main.c`: game loop and move input.
- `awale.c`: game rules and logic.
- `awale.h`: shared structures and declarations.

## Network play

The server hosts up to 32 clients in a lobby. Each client receives a list of the other connected players; players currently in a game are listed as "in game" and cannot be challenged. Spectators are listed as "spectating". Players in a game and spectators no longer receive lobby updates. The `chat <message>` command lets players in the lobby exchange messages; during a game, it lets the two players chat, and spectators also receive those messages. Spectators cannot send chat messages.

The `bio` command lets you save up to 10 lines of printable ASCII text, with a maximum of 80 characters per line. Press Enter to add a blank line, or type `/done` to finish before the tenth line; input ends automatically after 10 lines. The `bio <name>` command displays a player's bio or indicates that they have not saved one.

To add a friend, use `friend add <name>`. The other player must accept with `friend accept <name>` or reject with `friend reject <name>`. Use `friend requests` to view incoming requests and `friends` to view your friends. `friend remove <name>` removes the friendship for both players. Friendships and requests are stored in memory and removed for both sides when a player disconnects.

The `private on` command restricts spectating to friends of at least one of the two players; being friends with Alice or Bob is sufficient. `private off` disables this restriction. Spectators who lose access after a privacy change are returned to the lobby.

Completed games are archived in `games.txt` in the directory from which the server was started. Use `games` to list saved games and `review <id>` to replay a game move by move. Only games completed normally are archived; games interrupted by a disconnection are not saved.

The lobby commands are `list`, `bio`, `bio <name>`, `friend add <name>`, `friend accept <name>`, `friend reject <name>`, `friend requests`, `friend remove <name>`, `friends`, `private on`, `private off`, `games`, `review <id>`, `chat <message>`, `challenge <name>`, `accept <name>`, `reject <name>`, `observe <name>`, and `quit`. Names must be unique, contain no spaces, and be no longer than 31 characters.

Once a challenge is accepted, clients validate and apply the rules locally using `awale.c`, while the server relays moves and checks whose turn it is and whether a move uses a hole on the correct side. The server does not validate every rule, so this prototype is intended for trusted players.

Private game replays are accessible only to the two players and to people who are friends with at least one of them. Friends at the time the game ends are saved with the replay so they retain access after a disconnection; current friendships are also taken into account when the relevant player is connected.

On Windows with MinGW, compile the server and client from the project root:

```powershell
gcc -Wall -Wextra -o server.exe server/server.c -lws2_32
gcc -Wall -Wextra -o client.exe client/client.c awale.c -lws2_32
```

Start the server, using `0` for clockwise or `1` for anticlockwise:

```powershell
.\server.exe 0
```

Then start a client in each terminal:

```powershell
.\client.exe 127.0.0.1 Alice
.\client.exe 127.0.0.1 Bob
```

To play across two machines, replace `127.0.0.1` with the IP address of the machine hosting the server and allow TCP port `1977` through its firewall. Stop the server with Ctrl+C.
