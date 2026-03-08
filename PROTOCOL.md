# Clone+Surround Server Protocol (v1)

## Build and Run

Compile with manual gcc commands:

```bash
gcc -std=c11 -Wall -Wextra -O2 -o server server.c game_logic.c -lcjson
```

If your system has `libcjson.so.1` but no `libcjson.so` linker symlink, use:

```bash
gcc -std=c11 -Wall -Wextra -O2 -o server server.c game_logic.c \
  -L/lib/x86_64-linux-gnu -Wl,-rpath,/lib/x86_64-linux-gnu -Wl,-l:libcjson.so.1
```

Run server:

```bash
./server
```

Default server bind is `127.0.0.1:8080` (local-machine only).

Optional bind override:

```bash
./server [bind_ip] [port]
```

Examples:

```bash
./server 127.0.0.1 8080     # local-only (default behavior)
./server 0.0.0.0 8080       # LAN-accessible on all interfaces
./server 192.168.1.50 8080  # bind to one specific LAN interface
```

The protocol transport is newline-delimited JSON (one JSON object per line).

Compile client:

```bash
gcc -std=c11 -Wall -Wextra -O2 -o client client.c $(pkg-config --cflags --libs raylib) -lcjson
```

If your system has `libcjson.so.1` but no `libcjson.so` linker symlink, use:

```bash
gcc -std=c11 -Wall -Wextra -O2 -o client client.c $(pkg-config --cflags --libs raylib) \
  -L/lib/x86_64-linux-gnu -Wl,-rpath,/lib/x86_64-linux-gnu -Wl,-l:libcjson.so.1
```

If your raylib is installed as a static library and `pkg-config --libs raylib` is not enough,
add common Linux dependencies explicitly:

```bash
gcc -std=c11 -Wall -Wextra -O2 -o client client.c -I/usr/local/include -L/usr/local/lib -lraylib \
  -L/lib/x86_64-linux-gnu -Wl,-rpath,/lib/x86_64-linux-gnu -Wl,-l:libcjson.so.1 \
  -lm -lpthread -ldl -lrt -lX11 -lGL -lXrandr -lXi -lXcursor -lXinerama
```

Run client:

```bash
./client
```

Optional host/port override:

```bash
./client 127.0.0.1 8080
```

For LAN play, point clients to the server machine's LAN IP:

```bash
./client 192.168.1.50 8080
```

## Transport and Envelopes

- Request: flat JSON object with `cmd`.
- Response success:

```json
{"ok":true,"state":{...}}
```

- Response error:

```json
{"ok":false,"error":{"code":"...","message":"..."},"state":{...optional...}}
```

## Command Schemas

### 1. NEW_GAME

Request:

```json
{"cmd":"NEW_GAME","size":9}
```

Rules:
- `size` must be integer from `3` to `25`.
- Creator is assigned role `X`.

Response fields (top-level extras):
- `role`: `"X"`
- `token`: player token for X

### 2. JOIN_GAME

Request:

```json
{"cmd":"JOIN_GAME","game_id":1}
```

Optional reconnect token:

```json
{"cmd":"JOIN_GAME","game_id":1,"token":"0123abcd..."}
```

Behavior:
- If token matches existing X/O, reconnect that player and clear disconnected flag.
- Else if player slot available, assign it and issue token.
- Else join as spectator.

Response top-level fields:
- `role`: `"X" | "O" | "SPECTATOR"`
- `token`: included only for `X`/`O`

### 3. MOVE

Request:

```json
{"cmd":"MOVE","game_id":1,"token":"...","direction":0}
```

Direction enum:
- `0 = UP`
- `1 = DOWN`
- `2 = LEFT`
- `3 = RIGHT`

Rules:
- Requires valid player token.
- Requires correct turn.
- Rejected if game is not `playing`.

### 4. GET_STATE

Request:

```json
{"cmd":"GET_STATE","game_id":1}
```

Optional token is allowed but not required.

### 5. QUIT_GAME

Request:

```json
{"cmd":"QUIT_GAME","game_id":1,"token":"..."}
```

Rules:
- Valid player token required.
- Marks game finished with `finish_reason="quit"`.

## State Object

`state` contains:
- `game_id` (int)
- `size` (int)
- `board` (1D int array, `0=empty`, `1=X`, `2=O`)
- `active_player` (`"X"` or `"O"`)
- `x_token` (string or null)
- `o_token` (string or null)
- `x_disconnected` (bool)
- `o_disconnected` (bool)
- `status` (`"waiting" | "playing" | "finished"`)
- `winner` (`"X" | "O" | "DRAW" | null`)
- `finish_reason` (string or null)
- `created_at` (unix epoch seconds)
- `updated_at` (unix epoch seconds)

## Persistence

- Files are written to `./data/game_<id>.json` after each state-changing action.
- Files are retained after game completion.
- If a referenced game is not loaded in memory, server lazy-loads from disk.

## Move Resolution Order

For each valid MOVE:
1. Clone along direction (skipping opponent cells, place only on first empty landing cell).
2. Convert opponent groups with no liberties to mover color.
3. Remove mover groups with no liberties to empty.
4. Switch turn.
5. Evaluate end conditions (elimination, full board, both-zero fail-safe draw).

## Manual Smoke Checklist (Client)

1. Start `./server`, then `./client`; confirm the status bar shows target `127.0.0.1:8080`.
2. Create a game from client A and verify immediate board entry as role `X`, with waiting banner until opponent joins.
3. Join same game from client B by game id and verify role `O`, turn display, and board updates on both clients.
4. Join same game from client C and verify role `SPECTATOR`, read-only behavior, and spectator banner.
5. Submit moves with both Arrow keys and WASD from active player and verify server-authoritative turn enforcement.
6. Stop server during gameplay; verify inline connection error and automatic reconnect attempts. Restart server and verify in-session reclaim via `JOIN_GAME` with token.
7. Press ESC as player and verify `QUIT_GAME` + menu return; press ESC as spectator and verify local menu return without quit call.
8. Finish a game and verify finished overlay shows winner/reason and "Return to Menu" action.
