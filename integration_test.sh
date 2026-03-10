#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

if ! command -v nc >/dev/null 2>&1; then
  echo "nc is required"
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required"
  exit 1
fi

if nc -h 2>&1 | grep -q -- ' -N'; then
  NC_ARGS=(-N)
else
  NC_ARGS=(-q 1)
fi

send_json() {
  local payload="$1"
  printf '%s\n' "$payload" | nc "${NC_ARGS[@]}" 127.0.0.1 8080 | head -n1
}

json_get() {
  local json="$1"
  local path="$2"
  python3 - "$json" "$path" <<'PY'
import json
import sys

obj = json.loads(sys.argv[1])
path = sys.argv[2]

value = obj
for part in path.split('.'):
    if part == '':
        continue
    if isinstance(value, list):
        value = value[int(part)]
    elif isinstance(value, dict):
        value = value.get(part)
    else:
        value = None

if value is None:
    print("null")
elif isinstance(value, bool):
    print("true" if value else "false")
elif isinstance(value, (int, float)):
    if isinstance(value, float) and value.is_integer():
        print(int(value))
    else:
        print(value)
elif isinstance(value, str):
    print(value)
else:
    print(json.dumps(value, separators=(',', ':')))
PY
}

assert_eq() {
  local actual="$1"
  local expected="$2"
  local message="$3"
  if [[ "$actual" != "$expected" ]]; then
    echo "ASSERT FAILED: $message"
    echo "  expected: $expected"
    echo "  actual:   $actual"
    exit 1
  fi
}

assert_file_contains() {
  local file="$1"
  local needle="$2"
  local message="$3"
  if ! grep -q "$needle" "$file"; then
    echo "ASSERT FAILED: $message"
    echo "  needle: $needle"
    echo "  file: $file"
    exit 1
  fi
}

echo "Compiling server..."
if ! gcc -std=c11 -Wall -Wextra -O2 -o server server.c game_logic.c -lcjson; then
  gcc -std=c11 -Wall -Wextra -O2 -o server server.c game_logic.c \
    -L/lib/x86_64-linux-gnu -Wl,-rpath,/lib/x86_64-linux-gnu -Wl,-l:libcjson.so.1
fi

echo "Compiling game_logic API check helper..."
cat > /tmp/game_logic_api_check.c <<'C'
#include <stdio.h>
#include "game_logic.h"

static int check(bool cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
  }
  return 0;
}

int main(void) {
  int rc = 0;
  int board[9] = {0};
  board[4] = CELL_X;

  GameLogicError err = GAME_LOGIC_ERR_NONE;
  rc |= check(!game_logic_apply_move(NULL, 3, CELL_X, DIR_UP, &err), "NULL board should fail");
  rc |= check(err == GAME_LOGIC_ERR_INVALID_ARGS, "NULL board error code");

  err = GAME_LOGIC_ERR_NONE;
  rc |= check(!game_logic_apply_move(board, 2, CELL_X, DIR_UP, &err), "size < 3 should fail");
  rc |= check(err == GAME_LOGIC_ERR_INVALID_ARGS, "invalid size error code");

  err = GAME_LOGIC_ERR_NONE;
  rc |= check(!game_logic_apply_move(board, 3, 7, DIR_UP, &err), "invalid player should fail");
  rc |= check(err == GAME_LOGIC_ERR_INVALID_PLAYER, "invalid player error code");

  err = GAME_LOGIC_ERR_NONE;
  rc |= check(!game_logic_apply_move(board, 3, CELL_X, 9, &err), "invalid direction should fail");
  rc |= check(err == GAME_LOGIC_ERR_INVALID_DIRECTION, "invalid direction error code");

  err = GAME_LOGIC_ERR_NONE;
  rc |= check(game_logic_apply_move(board, 3, CELL_X, DIR_UP, &err), "valid move should succeed");
  rc |= check(err == GAME_LOGIC_ERR_NONE, "success should set no error");

  return rc;
}
C
gcc -std=c11 -Wall -Wextra -O2 -I. -o /tmp/game_logic_api_check /tmp/game_logic_api_check.c game_logic.c
/tmp/game_logic_api_check

echo "Resetting data dir..."
rm -rf data
mkdir -p data

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "$SERVER_PID" >/dev/null 2>&1 || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "Starting server..."
./server > /tmp/xo_catch_server.log 2>&1 &
SERVER_PID=$!
sleep 0.3

echo "[1/9] NEW_GAME size validation and seed check"
resp=$(send_json '{"cmd":"NEW_GAME","size":2}')
assert_eq "$(json_get "$resp" "ok")" "false" "NEW_GAME size<3 should fail"
assert_eq "$(json_get "$resp" "error.code")" "INVALID_SIZE" "Expected INVALID_SIZE"

resp=$(send_json '{"cmd":"NEW_GAME","size":5}')
assert_eq "$(json_get "$resp" "ok")" "true" "NEW_GAME size=5 should pass"
assert_eq "$(json_get "$resp" "role")" "X" "Creator role should be X"
GAME_ID="$(json_get "$resp" "state.game_id")"
X_TOKEN="$(json_get "$resp" "token")"
assert_eq "$(json_get "$resp" "state.board.11")" "1" "Odd seed X position mismatch"
assert_eq "$(json_get "$resp" "state.board.13")" "2" "Odd seed O position mismatch"

GAME_FILE="data/game_${GAME_ID}.json"
[[ -f "$GAME_FILE" ]] || { echo "Expected game file $GAME_FILE"; exit 1; }

echo "[2/9] JOIN role assignment and spectator fallback"
resp=$(send_json "{\"cmd\":\"JOIN_GAME\",\"game_id\":${GAME_ID}}")
assert_eq "$(json_get "$resp" "ok")" "true" "JOIN_GAME should pass"
assert_eq "$(json_get "$resp" "role")" "O" "Second join should become O"
O_TOKEN="$(json_get "$resp" "token")"

resp=$(send_json "{\"cmd\":\"JOIN_GAME\",\"game_id\":${GAME_ID}}")
assert_eq "$(json_get "$resp" "ok")" "true" "Spectator join should pass"
assert_eq "$(json_get "$resp" "role")" "SPECTATOR" "Third join should be spectator"
assert_eq "$(json_get "$resp" "token")" "null" "Spectator should not get token"

assert_file_contains "$GAME_FILE" '"status":"playing"' "Game should persist as playing after O joins"

echo "[3/9] MOVE auth and turn checks"
resp=$(send_json "{\"cmd\":\"MOVE\",\"game_id\":${GAME_ID},\"token\":\"badtoken\",\"direction\":0}")
assert_eq "$(json_get "$resp" "ok")" "false" "Bad token move should fail"
assert_eq "$(json_get "$resp" "error.code")" "UNAUTHORIZED" "Expected UNAUTHORIZED"

resp=$(send_json "{\"cmd\":\"MOVE\",\"game_id\":${GAME_ID},\"token\":\"${O_TOKEN}\",\"direction\":0}")
assert_eq "$(json_get "$resp" "ok")" "false" "Wrong-turn move should fail"
assert_eq "$(json_get "$resp" "error.code")" "NOT_YOUR_TURN" "Expected NOT_YOUR_TURN"

resp=$(send_json "{\"cmd\":\"MOVE\",\"game_id\":${GAME_ID},\"token\":\"${X_TOKEN}\",\"direction\":0}")
assert_eq "$(json_get "$resp" "ok")" "true" "Valid X move should succeed"
assert_eq "$(json_get "$resp" "state.active_player")" "O" "Turn should switch to O"

resp=$(send_json "{\"cmd\":\"MOVE\",\"game_id\":${GAME_ID},\"token\":\"${X_TOKEN}\",\"direction\":0}")
assert_eq "$(json_get "$resp" "ok")" "false" "Back-to-back X move should fail"
assert_eq "$(json_get "$resp" "error.code")" "NOT_YOUR_TURN" "Expected NOT_YOUR_TURN"

echo "[4/9] Capture-removal deterministic scenario"
cat > data/game_777.json <<'JSON'
{"game_id":777,"size":3,"board":[1,1,1,1,2,1,1,1,0],"active_player":"X","x_token":"1111111111111111","o_token":"2222222222222222","x_disconnected":false,"o_disconnected":false,"status":"playing","winner":null,"finish_reason":null,"created_at":1,"updated_at":1}
JSON

resp=$(send_json '{"cmd":"MOVE","game_id":777,"token":"1111111111111111","direction":3}')
assert_eq "$(json_get "$resp" "ok")" "true" "Custom move should succeed"
assert_eq "$(json_get "$resp" "state.status")" "finished" "Custom game should finish"
assert_eq "$(json_get "$resp" "state.winner")" "X" "Captured O should vanish; X wins by elimination"
assert_eq "$(json_get "$resp" "state.finish_reason")" "elimination" "Expected elimination reason"
assert_eq "$(json_get "$resp" "state.board.4")" "0" "Captured surrounded O should be removed"
assert_eq "$(json_get "$resp" "state.board.8")" "1" "New cloned X should remain on board"

echo "[5/9] GET_STATE polling"
resp=$(send_json "{\"cmd\":\"GET_STATE\",\"game_id\":${GAME_ID}}")
assert_eq "$(json_get "$resp" "ok")" "true" "GET_STATE should succeed"
assert_eq "$(json_get "$resp" "state.game_id")" "$GAME_ID" "GET_STATE game_id mismatch"

echo "[6/9] OFFER_DRAW agreement behavior"
resp=$(send_json '{"cmd":"NEW_GAME","size":5}')
assert_eq "$(json_get "$resp" "ok")" "true" "NEW_GAME for draw test should pass"
DRAW_GAME_ID="$(json_get "$resp" "state.game_id")"
DRAW_X_TOKEN="$(json_get "$resp" "token")"

resp=$(send_json "{\"cmd\":\"JOIN_GAME\",\"game_id\":${DRAW_GAME_ID}}")
assert_eq "$(json_get "$resp" "ok")" "true" "JOIN_GAME for draw test should pass"
DRAW_O_TOKEN="$(json_get "$resp" "token")"

resp=$(send_json "{\"cmd\":\"OFFER_DRAW\",\"game_id\":${DRAW_GAME_ID},\"token\":\"${DRAW_X_TOKEN}\"}")
assert_eq "$(json_get "$resp" "ok")" "true" "OFFER_DRAW from X should succeed"
assert_eq "$(json_get "$resp" "state.x_agree_draw")" "true" "X draw agree should be true"
assert_eq "$(json_get "$resp" "state.status")" "playing" "Game should still be playing after one offer"

resp=$(send_json "{\"cmd\":\"OFFER_DRAW\",\"game_id\":${DRAW_GAME_ID},\"token\":\"${DRAW_X_TOKEN}\"}")
assert_eq "$(json_get "$resp" "ok")" "true" "OFFER_DRAW from X should toggle off"
assert_eq "$(json_get "$resp" "state.x_agree_draw")" "false" "X draw agree should be false after toggle"
assert_eq "$(json_get "$resp" "state.status")" "playing" "Game should still be playing after toggle"

resp=$(send_json "{\"cmd\":\"OFFER_DRAW\",\"game_id\":${DRAW_GAME_ID},\"token\":\"${DRAW_X_TOKEN}\"}")
assert_eq "$(json_get "$resp" "ok")" "true" "OFFER_DRAW from X should toggle on"
assert_eq "$(json_get "$resp" "state.x_agree_draw")" "true" "X draw agree should be true after re-offer"

resp=$(send_json "{\"cmd\":\"OFFER_DRAW\",\"game_id\":${DRAW_GAME_ID},\"token\":\"${DRAW_O_TOKEN}\"}")
assert_eq "$(json_get "$resp" "ok")" "true" "OFFER_DRAW from O should succeed"
assert_eq "$(json_get "$resp" "state.o_agree_draw")" "true" "O draw agree should be true"
assert_eq "$(json_get "$resp" "state.status")" "finished" "Game should finish after both agree"
assert_eq "$(json_get "$resp" "state.winner")" "DRAW" "Draw agreement should end in DRAW"
assert_eq "$(json_get "$resp" "state.finish_reason")" "draw_agreed" "Draw reason should be persisted"

echo "[7/9] RESIGN finish behavior"
resp=$(send_json "{\"cmd\":\"RESIGN\",\"game_id\":${GAME_ID},\"token\":\"${O_TOKEN}\"}")
assert_eq "$(json_get "$resp" "ok")" "true" "RESIGN should succeed"
assert_eq "$(json_get "$resp" "state.status")" "finished" "Game should be finished after resign"
assert_eq "$(json_get "$resp" "state.winner")" "X" "If O resigns, winner should be X"
assert_eq "$(json_get "$resp" "state.finish_reason")" "resign" "Resign reason should be persisted"

assert_file_contains "$GAME_FILE" '"status":"finished"' "Finished status should persist"
assert_file_contains "$GAME_FILE" '"finish_reason":"resign"' "Resign reason should persist"

echo "[8/9] Finished-game access restrictions"
resp=$(send_json "{\"cmd\":\"MOVE\",\"game_id\":${GAME_ID},\"token\":\"${X_TOKEN}\",\"direction\":1}")
assert_eq "$(json_get "$resp" "ok")" "false" "MOVE on finished game should fail"
assert_eq "$(json_get "$resp" "error.code")" "GAME_FINISHED" "Expected GAME_FINISHED"

resp=$(send_json "{\"cmd\":\"JOIN_GAME\",\"game_id\":${GAME_ID}}")
assert_eq "$(json_get "$resp" "ok")" "false" "JOIN on finished game should fail"
assert_eq "$(json_get "$resp" "error.code")" "GAME_FINISHED" "Expected GAME_FINISHED"

resp=$(send_json "{\"cmd\":\"GET_STATE\",\"game_id\":${GAME_ID}}")
assert_eq "$(json_get "$resp" "ok")" "true" "GET_STATE should work for finished game"

echo "[9/9] Lazy-load from disk"
cat > data/game_778.json <<'JSON'
{"game_id":778,"size":3,"board":[1,0,2,0,1,0,2,0,1],"active_player":"O","x_token":"aaaaaaaaaaaaaaaa","o_token":"bbbbbbbbbbbbbbbb","x_disconnected":true,"o_disconnected":false,"status":"playing","winner":null,"finish_reason":null,"created_at":2,"updated_at":2}
JSON

resp=$(send_json '{"cmd":"GET_STATE","game_id":778}')
assert_eq "$(json_get "$resp" "ok")" "true" "Lazy load GET_STATE should succeed"
assert_eq "$(json_get "$resp" "state.active_player")" "O" "Lazy loaded active player mismatch"
assert_eq "$(json_get "$resp" "state.board.2")" "2" "Lazy loaded board mismatch"

echo "All integration checks passed."
