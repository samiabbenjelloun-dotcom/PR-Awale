#include <stdio.h>
#include <stdlib.h>

#include "awale.h"

int main()
{
    AwaleGame game;
    init_game(&game);

    while (!is_game_over(&game))
    {
        print_board(&game);
        if (game.current_player == 0)
        {
            printf("%s's turn (top row, holes 1-%d): ",
                   game.players[0].name, NB_HOLES / 2);
        }
        else
        {
            printf("%s's turn (bottom row, holes %d-%d): ",
                   game.players[1].name, NB_HOLES / 2 + 1, NB_HOLES);
        }
        int hole;
        if (scanf("%d", &hole) != 1)
        {
            fprintf(stderr, "Invalid input. Enter a hole number.\n");
            return 1;
        }

        /* The prompt uses holes numbered from 1, while the board uses zero-based indexes. */
        int selected_hole = hole - 1;
        if (play_move(&game, selected_hole) == -1)
        {
            int start = game.current_player * (NB_HOLES / 2);
            int end = start + NB_HOLES / 2;
            if (must_feed_opponent(&game) &&
                selected_hole >= start && selected_hole < end &&
                game.board[selected_hole] > 0)
            {
                printf("You must choose a move that feeds the opponent.\n");
            }
            else
            {
                printf("Invalid move. Try again.\n");
            }
            continue;
        }

        // Switch to the other player
        game.current_player = 1 - game.current_player;
    }

    print_board(&game);
    printf("Game over! %s score: %d, %s score: %d\n",
           game.players[0].name, game.players[0].score,
           game.players[1].name, game.players[1].score);
    return 0;
}