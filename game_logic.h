#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

#include <stdbool.h>

#define CELL_EMPTY 0
#define CELL_X 1
#define CELL_O 2

#define DIR_UP 0
#define DIR_DOWN 1
#define DIR_LEFT 2
#define DIR_RIGHT 3

bool game_logic_is_valid_direction(int direction);

void game_logic_seed_board(int *board, int size);

void game_logic_apply_move(int *board, int size, int player_cell, int direction);

void game_logic_count_cells(const int *board, int size, int *x_count, int *o_count, int *empty_count);

#endif
