#include "game_logic.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int x;
    int y;
} Point;

static int idx_of(int x, int y, int size) {
    return y * size + x;
}

static bool in_bounds(int x, int y, int size) {
    return x >= 0 && y >= 0 && x < size && y < size;
}

static int opponent_cell(int player_cell) {
    return player_cell == CELL_X ? CELL_O : CELL_X;
}

bool game_logic_is_valid_direction(int direction) {
    return direction >= DIR_UP && direction <= DIR_RIGHT;
}

void game_logic_seed_board(int *board, int size) {
    int total = size * size;
    memset(board, 0, sizeof(int) * (size_t)total);

    int y = (size - 1) / 2;
    int x_x;
    int x_o;

    if (size % 2 == 1) {
        int center = size / 2;
        x_x = center - 1;
        x_o = center + 1;
    } else {
        x_x = (size / 2) - 1;
        x_o = size / 2;
    }

    board[idx_of(x_x, y, size)] = CELL_X;
    board[idx_of(x_o, y, size)] = CELL_O;
}

static void clone_move(int *board, int size, int player_cell, int dx, int dy) {
    int total = size * size;
    Point *placements = malloc(sizeof(Point) * (size_t)total);
    int placement_count = 0;
    int opp = opponent_cell(player_cell);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (board[idx_of(x, y, size)] != player_cell) {
                continue;
            }

            int cx = x + dx;
            int cy = y + dy;

            while (in_bounds(cx, cy, size) && board[idx_of(cx, cy, size)] == opp) {
                cx += dx;
                cy += dy;
            }

            if (!in_bounds(cx, cy, size)) {
                continue;
            }

            if (board[idx_of(cx, cy, size)] == CELL_EMPTY) {
                placements[placement_count].x = cx;
                placements[placement_count].y = cy;
                ++placement_count;
            }
        }
    }

    for (int i = 0; i < placement_count; ++i) {
        board[idx_of(placements[i].x, placements[i].y, size)] = player_cell;
    }

    free(placements);
}

static bool flood_group(
    const int *board,
    int size,
    int start_x,
    int start_y,
    bool *visited,
    Point *stack,
    Point *group,
    int *group_size
) {
    int color = board[idx_of(start_x, start_y, size)];
    int stack_top = 0;
    bool has_liberty = false;

    stack[stack_top++] = (Point){start_x, start_y};
    visited[idx_of(start_x, start_y, size)] = true;
    *group_size = 0;

    while (stack_top > 0) {
        Point p = stack[--stack_top];
        group[(*group_size)++] = p;

        const int dirs[4][2] = {
            {1, 0}, {-1, 0}, {0, 1}, {0, -1}
        };

        for (int d = 0; d < 4; ++d) {
            int nx = p.x + dirs[d][0];
            int ny = p.y + dirs[d][1];

            if (!in_bounds(nx, ny, size)) {
                continue;
            }

            int nidx = idx_of(nx, ny, size);
            int value = board[nidx];

            if (value == CELL_EMPTY) {
                has_liberty = true;
            } else if (value == color && !visited[nidx]) {
                visited[nidx] = true;
                stack[stack_top++] = (Point){nx, ny};
            }
        }
    }

    return has_liberty;
}

static void convert_captured_opponent_groups(int *board, int size, int mover_cell) {
    int total = size * size;
    bool *visited = calloc((size_t)total, sizeof(bool));
    Point *stack = malloc(sizeof(Point) * (size_t)total);
    Point *group = malloc(sizeof(Point) * (size_t)total);
    int opponent = opponent_cell(mover_cell);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int id = idx_of(x, y, size);
            if (visited[id] || board[id] != opponent) {
                continue;
            }

            int group_size = 0;
            bool has_liberty = flood_group(board, size, x, y, visited, stack, group, &group_size);

            if (!has_liberty) {
                for (int i = 0; i < group_size; ++i) {
                    board[idx_of(group[i].x, group[i].y, size)] = mover_cell;
                }
            }
        }
    }

    free(visited);
    free(stack);
    free(group);
}

static void remove_dead_groups_of_color(int *board, int size, int color) {
    int total = size * size;
    bool *visited = calloc((size_t)total, sizeof(bool));
    Point *stack = malloc(sizeof(Point) * (size_t)total);
    Point *group = malloc(sizeof(Point) * (size_t)total);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int id = idx_of(x, y, size);
            if (visited[id] || board[id] != color) {
                continue;
            }

            int group_size = 0;
            bool has_liberty = flood_group(board, size, x, y, visited, stack, group, &group_size);

            if (!has_liberty) {
                for (int i = 0; i < group_size; ++i) {
                    board[idx_of(group[i].x, group[i].y, size)] = CELL_EMPTY;
                }
            }
        }
    }

    free(visited);
    free(stack);
    free(group);
}

void game_logic_apply_move(int *board, int size, int player_cell, int direction) {
    int dx = 0;
    int dy = 0;

    switch (direction) {
        case DIR_UP:
            dy = -1;
            break;
        case DIR_DOWN:
            dy = 1;
            break;
        case DIR_LEFT:
            dx = -1;
            break;
        case DIR_RIGHT:
            dx = 1;
            break;
        default:
            return;
    }

    clone_move(board, size, player_cell, dx, dy);
    convert_captured_opponent_groups(board, size, player_cell);
    remove_dead_groups_of_color(board, size, player_cell);
}

void game_logic_count_cells(const int *board, int size, int *x_count, int *o_count, int *empty_count) {
    int x = 0;
    int o = 0;
    int e = 0;
    int total = size * size;

    for (int i = 0; i < total; ++i) {
        if (board[i] == CELL_X) {
            ++x;
        } else if (board[i] == CELL_O) {
            ++o;
        } else {
            ++e;
        }
    }

    if (x_count != NULL) {
        *x_count = x;
    }
    if (o_count != NULL) {
        *o_count = o;
    }
    if (empty_count != NULL) {
        *empty_count = e;
    }
}
