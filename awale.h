#ifndef AWALE_H
#define AWALE_H

#define NB_HOLES 12
#define INITIAL_SEEDS 4

typedef struct {
    int id;
    int score;
    int order; // 0 for first player, 1 for second player
} Player;

typedef struct {
    int board[NB_HOLES];
    Player players[2];
    int current_player;
    int rotation; // 0 for normal, 1 for rotated
} AwaleGame;

void init_game(AwaleGame *game);
int is_valid_move(AwaleGame *game, int hole);
int play_move(AwaleGame *game, int hole);
int must_feed_opponent(AwaleGame *game);
int is_game_over(AwaleGame *game);
void print_board(AwaleGame *game);

#endif // AWALE_H