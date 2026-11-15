# PR-Awale

A terminal-based Awale game for two players, with online matches, spectators, chat, friends, and game replays.

## Requirements

- GCC (for example, MinGW on Windows)
- Two terminals to play locally; the server listens on TCP port `1977`

## Build the client and server

From the project directory, compile both programs with MinGW:

```powershell
gcc -Wall -Wextra -o server.exe server/server.c -lws2_32
gcc -Wall -Wextra -o client.exe client/client.c awale.c -lws2_32
```

## Start a game

1. In one terminal, start the server. Choose `0` for clockwise or `1` for anticlockwise:

   ```powershell
   .\server.exe 0
   ```

2. In separate terminals, connect each player to the server. Each player needs a unique name (no spaces, maximum 31 characters):

   ```powershell
   .\client.exe 127.0.0.1 Alice
   .\client.exe 127.0.0.1 Bob
   ```

3. In a client, challenge another player with `challenge <name>`. The other player can enter `accept <name>` or `reject <name>`. Once the game starts, follow the prompts to choose a hole on your side of the board.

To play across two machines, replace `127.0.0.1` with the server machine's IP address and allow TCP port `1977` through its firewall. Stop the server with Ctrl+C.

## Client commands

Most commands are available in the lobby; `chat <message>` also works during a game. The client prints the command list when it starts.

| Command | Description |
| --- | --- |
| `list` | List connected players and their status. |
| `challenge <name>` | Challenge a player. |
| `accept <name>` / `reject <name>` | Accept or reject an incoming challenge. |
| `observe <name>` | Watch a player's game, if spectating is allowed. |
| `chat <message>` | Send a message in the lobby or, during a game, to the players. Spectators can receive game chat but cannot send it. |
| `quit` | Disconnect from the server. |
| `bio` | Save a bio of up to 10 lines, with at most 80 printable ASCII characters per line. Press Enter for a blank line or enter `/done` to finish early. |
| `bio <name>` | Display a player's bio. |
| `friend add <name>` | Send a friend request. |
| `friend accept <name>` / `friend reject <name>` | Accept or reject a friend request. |
| `friend requests` | Show incoming friend requests. |
| `friends` | List your friends. |
| `friend remove <name>` | Remove a friend. |
| `private on` / `private off` | Restrict spectating to friends of at least one player, or allow everyone to observe. |
| `games` | List completed games available to you. |
| `review <id>` | Replay a completed game move by move. |

Friend requests and friendships are kept in memory and removed when a player disconnects. Spectators who lose access after a privacy change are returned to the lobby.

Completed games are saved in `games.txt` in the directory where the server was started. Only games completed normally are archived; games interrupted by a disconnection are not saved. Private replays are accessible to the two players and people who were friends with at least one of them when the game ended. Current friendships are also taken into account while a player is connected.

## Notes

The server hosts up to 32 clients. Players in a game or watching a game do not receive lobby updates. During a match, clients apply and validate the Awale rules locally using `awale.c`; the server relays moves and checks whose turn it is and whether a move uses a hole on the correct side. The server does not validate every rule, so this prototype is intended for trusted players.
