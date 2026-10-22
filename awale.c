#include <stdio.h>
#include <stdlib.h>

#include "awale.h"

void init_game(AwaleGame *game) {
    for (int i = 0; i < NB_HOLES; i++) {
        game->board[i] = INITIAL_SEEDS;
    }
    for (int i = 0; i < 2; i++) {
        game->players[i].id = i;
        game->players[i].score = 0;
        game->players[i].order = i;
    }
    game->current_player = 0; // Player 0 starts
    printf("Rotation (0 for clockwise, 1 for anticlockwise): ");
    if (scanf("%d", &game->rotation) != 1 || (game->rotation != 0 && game->rotation != 1)) {
        fprintf(stderr, "Invalid rotation input. Enter 0 or 1.\n");
        exit(1);
    }
}

int is_valid_move(AwaleGame *game, int hole) {
    if (hole < 0 || hole >= NB_HOLES) {
        return 0; // Invalid hole index
    }
    if (game->board[hole] == 0) {
        return 0; // Cannot play from an empty hole
    }
    if (hole / (NB_HOLES / 2) != game->current_player) {
        return 0; // Players can only play from their own side
    }

    return 1; // Valid move
}

int play_move(AwaleGame *game, int hole) {
    if (!is_valid_move(game, hole)) {
        return -1; // Invalid move
    }

    int seeds = game->board[hole];
    game->board[hole] = 0;
    int index = hole;
    const int anticlockwise_next[NB_HOLES] = {
        6, 0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 5
    };

    while (seeds > 0) {
        if (game->rotation == 0) { // Clockwise
            index = (index + 1) % NB_HOLES;
        } else { // Anticlockwise
            index = anticlockwise_next[index];
        }

        if (index != hole) { // Skip the original hole
            game->board[index]++;
            seeds--;
        }
    }

    // Check for captures
    if(game->rotation == 0) { // Clockwise
        while (index >= NB_HOLES / 2 && index < NB_HOLES && (game->board[index] == 2 || game->board[index] == 3)) {
            game->players[game->current_player].score += game->board[index];
            game->board[index] = 0;
            index--;
        }
    } else { // Anticlockwise
        while (index < NB_HOLES / 2 && index >= 0 && (game->board[index] == 2 || game->board[index] == 3)) {
            game->players[game->current_player].score += game->board[index];
            game->board[index] = 0;
            index++;
        }
    }

    return 0; // Move played successfully
}

int is_game_over(AwaleGame *game) {
    int total_seeds = 0;
    for (int i = 0; i < NB_HOLES; i++) {
        total_seeds += game->board[i];
    }
    return total_seeds == 0; // Game is over if there are no seeds left
}

void print_board(AwaleGame *game) {
    printf("Current board:\n");
    for (int i = 0; i < NB_HOLES; i++) {
        printf("%d ", game->board[i]);
        
        if(i == NB_HOLES / 2 - 1) {
            printf("\n"); // New line after half the holes
        }
    }
    printf("\nScores:\n   - Player 0: %d\n   - Player 1: %d\n", game->players[0].score, game->players[1].score);
}