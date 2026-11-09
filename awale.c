#include <stdio.h>
#include <stdlib.h>

#include "awale.h"

/* These tables encode the board traversal order, including the turn between rows. */
static const int clockwise_next[NB_HOLES] = {
    1, 2, 3, 4, 5, 11, 0, 6, 7, 8, 9, 10};
static const int anticlockwise_next[NB_HOLES] = {
    6, 0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 5};

static int has_seeds_on_side(const AwaleGame *game, int player)
{
    int start = player * (NB_HOLES / 2);
    int end = start + NB_HOLES / 2;

    for (int hole = start; hole < end; hole++)
    {
        if (game->board[hole] > 0)
        {
            return 1;
        }
    }
    return 0;
}

static int next_hole(const AwaleGame *game, int hole)
{
    return game->rotation == 0
               ? clockwise_next[hole]
               : anticlockwise_next[hole];
}

static int previous_hole(const AwaleGame *game, int hole)
{
    return game->rotation == 0
               ? anticlockwise_next[hole]
               : clockwise_next[hole];
}

static int sow_seeds(AwaleGame *game, int hole)
{
    int seeds = game->board[hole];
    int index = hole;
    game->board[hole] = 0;

    while (seeds > 0)
    {
        index = next_hole(game, index);

        /* A sowing lap skips the starting hole, as required by Awale rules. */
        if (index == hole)
        {
            continue;
        }

        game->board[index]++;
        seeds--;
    }
    return index;
}

static void capture_seeds(AwaleGame *game, int index)
{
    int opponent = 1 - game->current_player;

    /* Capture backwards from the last sown hole while opponent pits hold 2 or 3 seeds. */
    while (index / (NB_HOLES / 2) == opponent &&
           (game->board[index] == 2 || game->board[index] == 3))
    {
        game->players[game->current_player].score += game->board[index];
        game->board[index] = 0;
        index = previous_hole(game, index);
    }
}

static int move_feeds_opponent(const AwaleGame *game, int hole, int player)
{
    /* Simulate the move on a copy so checking the feeding rule never changes the real game. */
    AwaleGame test = *game;
    int last_hole = sow_seeds(&test, hole);
    capture_seeds(&test, last_hole);
    return has_seeds_on_side(&test, 1 - player);
}

static int has_feeding_move(AwaleGame *game, int player)
{
    int start = player * (NB_HOLES / 2);
    int end = start + NB_HOLES / 2;

    for (int hole = start; hole < end; hole++)
    {
        if (game->board[hole] > 0 && move_feeds_opponent(game, hole, player))
        {
            return 1;
        }
    }
    return 0;
}

int must_feed_opponent(AwaleGame *game)
{
    int opponent = 1 - game->current_player;
    /* Feeding is mandatory only when the opponent's side is empty and a feeding move exists. */
    return !has_seeds_on_side(game, opponent) &&
           has_feeding_move(game, game->current_player);
}

void init_game_with_rotation(AwaleGame *game, int rotation)
{
    for (int i = 0; i < NB_HOLES; i++)
    {
        game->board[i] = INITIAL_SEEDS;
    }
    for (int i = 0; i < 2; i++)
    {
        game->players[i].id = i;
        game->players[i].score = 0;
        game->players[i].order = i;
    }
    game->current_player = 0; // Player 0 starts
    game->rotation = rotation;
}

void init_game(AwaleGame *game)
{
    printf("Rotation (0 for clockwise, 1 for anticlockwise): ");
    int rotation;
    if (scanf("%d", &rotation) != 1 || (rotation != 0 && rotation != 1))
    {
        fprintf(stderr, "Invalid rotation input. Enter 0 or 1.\n");
        exit(1);
    }
    init_game_with_rotation(game, rotation);
}

int is_valid_move(AwaleGame *game, int hole)
{
    if (hole < 0 || hole >= NB_HOLES)
    {
        return 0; // Invalid hole index
    }
    if (game->board[hole] == 0)
    {
        return 0; // Cannot play from an empty hole
    }
    if (hole / (NB_HOLES / 2) != game->current_player)
    {
        return 0; // Players can only play from their own side
    }

    if (must_feed_opponent(game) &&
        !move_feeds_opponent(game, hole, game->current_player))
    {
        return 0;
    }

    return 1; // Valid move
}

int play_move(AwaleGame *game, int hole)
{
    if (!is_valid_move(game, hole))
    {
        return -1; // Invalid move
    }

    int index = sow_seeds(game, hole);
    capture_seeds(game, index);

    return 0; // Move played successfully
}

int is_game_over(AwaleGame *game)
{
    int total_seeds = 0;
    for (int i = 0; i < NB_HOLES; i++)
    {
        total_seeds += game->board[i];
    }

    /* A player reaching 25 wins; 24-24 is also a terminal draw. */
    if (game->players[0].score >= 25 || game->players[1].score >= 25)
    {
        return 1; // Game is over if a player has 25 or more seeds
    }

    if (game->players[0].score == 24 && game->players[1].score == 24)
    {
        return 1; // Game is over if both players have 24 seeds
    }

    int valid_move_found = 0;
    int start = game->current_player * (NB_HOLES / 2);
    int end = start + (NB_HOLES / 2);

    for (int i = start; i < end; i++)
    {
        if (game->board[i] > 0)
        {
            valid_move_found = 1;
            break;
        }
    }

    if (!valid_move_found)
    {
        return 1; // Game is over if the current player has no valid moves
    }

    /* Stop if the player cannot feed an empty opponent side. */
    if (!has_seeds_on_side(game, 1 - game->current_player) &&
        !has_feeding_move(game, game->current_player))
    {
        return 1; // Game is over if the current player cannot feed the opponent
    }

    return total_seeds == 0; // Game is over if there are no seeds left
}

void print_board(AwaleGame *game)
{
    printf("Current board:\n");
    for (int i = 0; i < NB_HOLES; i++)
    {
        printf("%d ", game->board[i]);

        if (i == NB_HOLES / 2 - 1)
        {
            printf("\n"); // New line after half the holes
        }
    }
    printf("\nScores:\n   - Player 0: %d\n   - Player 1: %d\n", game->players[0].score, game->players[1].score);
}