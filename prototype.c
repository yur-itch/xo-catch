/*
raylib_grid_clone_game.c

Clone + Surround prototype using raylib.

Implemented:
- Menu to select board size
- Arrow-key moves (global directional clone move)
- Player switching after each half-move
- Flood-fill liberty detection
- Surrounded groups convert to the player who just moved

Controls:
Arrow Keys  : Perform move
ESC         : Return to menu
Mouse click : "New Game" button

Compile (Linux example):
  gcc -o raylib_grid_clone_game raylib_grid_clone_game.c -lraylib -lm -lpthread -ldl -lrt -lX11
*/

#include "raylib.h"
#include <stdlib.h>
#include <stdbool.h>

#define SCREEN_W 900
#define SCREEN_H 800

typedef enum { MENU = 0, PLAYING } GameState;
typedef enum { EMPTY = 0, XCELL, OCELL } Cell;
typedef enum { PLAYER_X = 0, PLAYER_O } Player;

typedef struct {
    int x;
    int y;
} Point;

static bool IsMouseOver(Rectangle r) {
    Vector2 m = GetMousePosition();
    return (m.x >= r.x && m.x <= r.x + r.width &&
            m.y >= r.y && m.y <= r.y + r.height);
}

static Cell PlayerCell(Player p) {
    return (p == PLAYER_X) ? XCELL : OCELL;
}

static Cell OppCell(Player p) {
    return (p == PLAYER_X) ? OCELL : XCELL;
}

/* ------------------------------------------------------- */
/* CLONE MOVE */
/* ------------------------------------------------------- */

static void DoMove(Cell *board, int size, Player player, int dx, int dy)
{
    Cell me  = PlayerCell(player);
    Cell opp = OppCell(player);

    int max = size * size;
    Point *placements = malloc(sizeof(Point) * max);
    int count = 0;

    for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++)
    {
        if (board[y*size + x] != me)
            continue;

        int cx = x + dx;
        int cy = y + dy;

        while (cx >= 0 && cy >= 0 && cx < size && cy < size &&
               board[cy*size + cx] == opp)
        {
            cx += dx;
            cy += dy;
        }

        if (cx < 0 || cy < 0 || cx >= size || cy >= size)
            continue;

        if (board[cy*size + cx] == EMPTY)
        {
            placements[count++] = (Point){cx,cy};
        }
    }

    for (int i=0;i<count;i++)
    {
        Point p = placements[i];
        board[p.y*size + p.x] = me;
    }

    free(placements);
}

/* ------------------------------------------------------- */
/* FLOOD FILL CAPTURE */
/* ------------------------------------------------------- */

static void CheckCaptures(Cell *board, int size, Player capturer)
{
    bool *visited = calloc(size*size, sizeof(bool));

    Point *stack = malloc(sizeof(Point) * size * size);
    Point *group = malloc(sizeof(Point) * size * size);

    Cell capturerCell = PlayerCell(capturer);

    for (int y=0;y<size;y++)
    for (int x=0;x<size;x++)
    {
        int idx = y*size + x;

        if (visited[idx] || board[idx] == EMPTY)
            continue;

        Cell color = board[idx];

        int stackTop = 0;
        int groupSize = 0;
        bool hasLiberty = false;

        stack[stackTop++] = (Point){x,y};
        visited[idx] = true;

        while (stackTop > 0)
        {
            Point p = stack[--stackTop];
            group[groupSize++] = p;

            const int dirs[4][2] = {
                {1,0},{-1,0},{0,1},{0,-1}
            };

            for (int d=0; d<4; d++)
            {
                int nx = p.x + dirs[d][0];
                int ny = p.y + dirs[d][1];

                if (nx < 0 || ny < 0 || nx >= size || ny >= size)
                    continue;

                int nidx = ny*size + nx;

                if (board[nidx] == EMPTY)
                {
                    hasLiberty = true;
                }
                else if (board[nidx] == color && !visited[nidx])
                {
                    visited[nidx] = true;
                    stack[stackTop++] = (Point){nx,ny};
                }
            }
        }

        if (!hasLiberty)
        {
            for (int i=0;i<groupSize;i++)
            {
                Point p = group[i];
                board[p.y*size + p.x] = capturerCell;
            }
        }
    }

    free(visited);
    free(stack);
    free(group);
}

/* ------------------------------------------------------- */
/* MAIN */
/* ------------------------------------------------------- */

int main(void)
{
    InitWindow(SCREEN_W, SCREEN_H, "Clone + Surround Prototype");
    SetTargetFPS(60);

    GameState state = MENU;
    Player activePlayer = PLAYER_X;

    int boardSize = 9;
    const int MIN_SIZE = 3;
    const int MAX_SIZE = 25;

    Rectangle minusBtn = {120,140,60,60};
    Rectangle plusBtn  = {320,140,60,60};
    Rectangle startBtn = {120,220,260,60};

    Rectangle newGameBtn = {20,20,140,40};

    Cell *board = NULL;

    while (!WindowShouldClose())
    {
        /* ---------------- MENU ---------------- */

        if (state == MENU)
        {
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
            {
                if (IsMouseOver(minusBtn) && boardSize > MIN_SIZE)
                    boardSize--;

                else if (IsMouseOver(plusBtn) && boardSize < MAX_SIZE)
                    boardSize++;

                else if (IsMouseOver(startBtn))
                {
                    if (board) free(board);

                    board = malloc(sizeof(Cell)*boardSize*boardSize);

                    for (int i=0;i<boardSize*boardSize;i++)
                        board[i] = EMPTY;

                    int cx = boardSize/2;
                    int cy = boardSize/2;

                    board[cy*boardSize + cx] = XCELL;

                    if (cx+1 < boardSize)
                        board[cy*boardSize + cx+1] = OCELL;
                    else
                        board[cy*boardSize + cx-1] = OCELL;

                    activePlayer = PLAYER_X;
                    state = PLAYING;
                }
            }
        }

        /* ---------------- GAME ---------------- */

        else if (state == PLAYING)
        {
            bool moved = false;

            if (IsKeyPressed(KEY_UP))
            {
                DoMove(board, boardSize, activePlayer, 0,-1);
                moved = true;
            }
            else if (IsKeyPressed(KEY_DOWN))
            {
                DoMove(board, boardSize, activePlayer, 0,1);
                moved = true;
            }
            else if (IsKeyPressed(KEY_LEFT))
            {
                DoMove(board, boardSize, activePlayer, -1,0);
                moved = true;
            }
            else if (IsKeyPressed(KEY_RIGHT))
            {
                DoMove(board, boardSize, activePlayer, 1,0);
                moved = true;
            }

            if (moved)
            {
                CheckCaptures(board, boardSize, activePlayer);

                activePlayer = (activePlayer == PLAYER_X)
                    ? PLAYER_O
                    : PLAYER_X;
            }

            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && IsMouseOver(newGameBtn))
            {
                state = MENU;
                free(board);
                board = NULL;
            }

            if (IsKeyPressed(KEY_ESCAPE))
            {
                state = MENU;
                free(board);
                board = NULL;
            }
        }

        /* ---------------- DRAW ---------------- */

        BeginDrawing();
        ClearBackground(RAYWHITE);

        if (state == MENU)
        {
            DrawText("Clone + Surround",40,30,30,DARKGRAY);
            DrawText("Board Size",120,100,20,DARKGRAY);

            DrawRectangleRec(minusBtn,LIGHTGRAY);
            DrawText("-",minusBtn.x+20,minusBtn.y+10,40,BLACK);

            DrawText(TextFormat("%dx%d",boardSize,boardSize),200,150,30,BLACK);

            DrawRectangleRec(plusBtn,LIGHTGRAY);
            DrawText("+",plusBtn.x+18,plusBtn.y+10,40,BLACK);

            DrawRectangleRec(startBtn,GREEN);
            DrawText("Start Game",startBtn.x+40,startBtn.y+18,20,WHITE);
        }

        else if (state == PLAYING)
        {
            Color ngCol = IsMouseOver(newGameBtn) ? MAROON : RED;
            DrawRectangleRec(newGameBtn, ngCol);
            DrawText("New Game", newGameBtn.x+18, newGameBtn.y+10,20,WHITE);

            DrawText(TextFormat("Turn: %s", activePlayer==PLAYER_X?"X":"O"),
                     220,25,24,BLACK);

            int marginTop = 90;
            int marginSides = 40;

            int availW = SCREEN_W - marginSides*2;
            int availH = SCREEN_H - marginTop - 40;

            int cellSize = availW / boardSize;
            if (cellSize * boardSize > availH)
                cellSize = availH / boardSize;

            int boardW = cellSize * boardSize;
            int boardH = cellSize * boardSize;

            int boardX = (SCREEN_W - boardW)/2;
            int boardY = marginTop;

            for (int y=0;y<boardSize;y++)
            for (int x=0;x<boardSize;x++)
            {
                Rectangle cell = {
                    boardX + x*cellSize,
                    boardY + y*cellSize,
                    cellSize,
                    cellSize
                };

                DrawRectangleRec(cell, BEIGE);
                DrawRectangleLinesEx(cell,1,LIGHTGRAY);

                Cell c = board[y*boardSize + x];

                if (c == XCELL)
                {
                    int pad = cellSize/6;

                    DrawLine(cell.x+pad, cell.y+pad,
                             cell.x+cell.width-pad,
                             cell.y+cell.height-pad,
                             MAROON);

                    DrawLine(cell.x+pad,
                             cell.y+cell.height-pad,
                             cell.x+cell.width-pad,
                             cell.y+pad,
                             MAROON);
                }
                else if (c == OCELL)
                {
                    int cx = cell.x + cell.width/2;
                    int cy = cell.y + cell.height/2;

                    int r = cell.width/2 - cell.width/6;

                    DrawCircleLines(cx,cy,r,DARKBLUE);
                }
            }

            DrawText("Use arrow keys to move",20,SCREEN_H-24,16,DARKGRAY);
        }

        EndDrawing();
    }

    if (board) free(board);

    CloseWindow();

    return 0;
}
