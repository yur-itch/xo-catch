#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if __has_include("cJSON.h")
#include "cJSON.h"
#elif __has_include(<cjson/cJSON.h>)
#include <cjson/cJSON.h>
#elif __has_include(<cJSON.h>)
#include <cJSON.h>
#else
#error "cJSON headers not found. Install libcjson development package."
#endif

#include "game_logic.h"

#define DEFAULT_LISTEN_IP "127.0.0.1"
#define DEFAULT_LISTEN_PORT 8080
#define DATA_DIR "./data"
#define CLIENT_BUFFER_SIZE 32768
#define LINE_BUFFER_SIZE 32768
#define MAX_CLIENTS FD_SETSIZE

enum Role {
    ROLE_NONE = 0,
    ROLE_X = 1,
    ROLE_O = 2,
    ROLE_SPECTATOR = 3
};

enum Winner {
    WINNER_NONE = 0,
    WINNER_X = 1,
    WINNER_O = 2,
    WINNER_DRAW = 3
};

enum GameStatus {
    STATUS_WAITING = 0,
    STATUS_PLAYING = 1,
    STATUS_FINISHED = 2
};

typedef struct Game {
    int game_id;
    int size;
    int *board;

    int active_player;

    bool has_x;
    bool has_o;
    char x_token[17];
    char o_token[17];

    bool x_disconnected;
    bool o_disconnected;
    int x_conn_fd;
    int o_conn_fd;

    int status;
    int winner;
    char finish_reason[32];

    time_t created_at;
    time_t updated_at;

    struct Game *next;
} Game;

typedef struct {
    int fd;
    char buffer[CLIENT_BUFFER_SIZE];
    size_t len;
} ClientConn;

static Game *g_games = NULL;
static int g_next_game_id = 1;
static char g_listen_ip[64] = DEFAULT_LISTEN_IP;
static int g_listen_port = DEFAULT_LISTEN_PORT;

static const char *role_to_string(int role) {
    switch (role) {
        case ROLE_X:
            return "X";
        case ROLE_O:
            return "O";
        case ROLE_SPECTATOR:
            return "SPECTATOR";
        default:
            return "NONE";
    }
}

static const char *status_to_string(int status) {
    switch (status) {
        case STATUS_WAITING:
            return "waiting";
        case STATUS_PLAYING:
            return "playing";
        case STATUS_FINISHED:
            return "finished";
        default:
            return "unknown";
    }
}

static int string_to_status(const char *value) {
    if (value == NULL) {
        return STATUS_WAITING;
    }
    if (strcmp(value, "waiting") == 0) {
        return STATUS_WAITING;
    }
    if (strcmp(value, "playing") == 0) {
        return STATUS_PLAYING;
    }
    if (strcmp(value, "finished") == 0) {
        return STATUS_FINISHED;
    }
    return STATUS_WAITING;
}

static const char *winner_to_string(int winner) {
    switch (winner) {
        case WINNER_X:
            return "X";
        case WINNER_O:
            return "O";
        case WINNER_DRAW:
            return "DRAW";
        default:
            return NULL;
    }
}

static int string_to_winner(const char *value) {
    if (value == NULL) {
        return WINNER_NONE;
    }
    if (strcmp(value, "X") == 0) {
        return WINNER_X;
    }
    if (strcmp(value, "O") == 0) {
        return WINNER_O;
    }
    if (strcmp(value, "DRAW") == 0) {
        return WINNER_DRAW;
    }
    return WINNER_NONE;
}

static void touch_game(Game *game) {
    game->updated_at = time(NULL);
}

static void ensure_data_dir(void) {
    struct stat st;
    if (stat(DATA_DIR, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "%s exists and is not a directory\n", DATA_DIR);
            exit(1);
        }
        return;
    }

    if (mkdir(DATA_DIR, 0755) != 0) {
        perror("mkdir data dir");
        exit(1);
    }
}

static void game_file_path(int game_id, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/game_%d.json", DATA_DIR, game_id);
}

static void game_tmp_file_path(int game_id, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/game_%d.json.tmp", DATA_DIR, game_id);
}

static void generate_token(char out[17]) {
    uint64_t value = ((uint64_t)(unsigned)rand() << 48)
                   ^ ((uint64_t)(unsigned)rand() << 32)
                   ^ ((uint64_t)(unsigned)rand() << 16)
                   ^ (uint64_t)(unsigned)rand();
    snprintf(out, 17, "%016llx", (unsigned long long)value);
}

static Game *find_loaded_game(int game_id) {
    Game *cur = g_games;
    while (cur != NULL) {
        if (cur->game_id == game_id) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

static void add_loaded_game(Game *game) {
    game->next = g_games;
    g_games = game;
}

static cJSON *game_to_json_state(const Game *game) {
    cJSON *state = cJSON_CreateObject();
    cJSON *board = cJSON_CreateArray();
    int total = game->size * game->size;

    cJSON_AddNumberToObject(state, "game_id", game->game_id);
    cJSON_AddNumberToObject(state, "size", game->size);

    for (int i = 0; i < total; ++i) {
        cJSON_AddItemToArray(board, cJSON_CreateNumber(game->board[i]));
    }
    cJSON_AddItemToObject(state, "board", board);

    cJSON_AddStringToObject(state, "active_player", role_to_string(game->active_player));

    if (game->has_x) {
        cJSON_AddStringToObject(state, "x_token", game->x_token);
    } else {
        cJSON_AddNullToObject(state, "x_token");
    }

    if (game->has_o) {
        cJSON_AddStringToObject(state, "o_token", game->o_token);
    } else {
        cJSON_AddNullToObject(state, "o_token");
    }

    cJSON_AddBoolToObject(state, "x_disconnected", game->x_disconnected);
    cJSON_AddBoolToObject(state, "o_disconnected", game->o_disconnected);

    cJSON_AddStringToObject(state, "status", status_to_string(game->status));

    const char *winner = winner_to_string(game->winner);
    if (winner != NULL) {
        cJSON_AddStringToObject(state, "winner", winner);
    } else {
        cJSON_AddNullToObject(state, "winner");
    }

    if (strcmp(game->finish_reason, "none") == 0) {
        cJSON_AddNullToObject(state, "finish_reason");
    } else {
        cJSON_AddStringToObject(state, "finish_reason", game->finish_reason);
    }

    cJSON_AddNumberToObject(state, "created_at", (double)game->created_at);
    cJSON_AddNumberToObject(state, "updated_at", (double)game->updated_at);

    return state;
}

static bool save_game(const Game *game) {
    char path[256];
    char tmp_path[256];
    game_file_path(game->game_id, path, sizeof(path));
    game_tmp_file_path(game->game_id, tmp_path, sizeof(tmp_path));

    cJSON *json = game_to_json_state(game);
    char *encoded = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (encoded == NULL) {
        return false;
    }

    FILE *fp = fopen(tmp_path, "w");
    if (fp == NULL) {
        free(encoded);
        return false;
    }

    size_t expected = strlen(encoded);
    size_t written = fwrite(encoded, 1, expected, fp);
    free(encoded);

    int close_rc = fclose(fp);
    if (written != expected || close_rc != 0) {
        return false;
    }

    if (rename(tmp_path, path) != 0) {
        return false;
    }

    return true;
}

static int load_file_into_memory(const char *path, char **buffer_out) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return -1;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    char *buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(fp);
        return -1;
    }

    size_t read_size = fread(buffer, 1, (size_t)size, fp);
    fclose(fp);

    if (read_size != (size_t)size) {
        free(buffer);
        return -1;
    }

    buffer[size] = '\0';
    *buffer_out = buffer;
    return (int)size;
}

static bool parse_json_string_field(cJSON *obj, const char *name, char *out, size_t out_size, bool *present) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (item == NULL || cJSON_IsNull(item)) {
        if (present != NULL) {
            *present = false;
        }
        if (out_size > 0) {
            out[0] = '\0';
        }
        return true;
    }

    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }

    snprintf(out, out_size, "%s", item->valuestring);
    if (present != NULL) {
        *present = true;
    }
    return true;
}

static Game *load_game_from_disk(int game_id) {
    char path[256];
    game_file_path(game_id, path, sizeof(path));

    char *data = NULL;
    if (load_file_into_memory(path, &data) < 0) {
        return NULL;
    }

    cJSON *json = cJSON_Parse(data);
    free(data);
    if (json == NULL) {
        return NULL;
    }

    cJSON *size_item = cJSON_GetObjectItemCaseSensitive(json, "size");
    cJSON *board_item = cJSON_GetObjectItemCaseSensitive(json, "board");
    cJSON *active_item = cJSON_GetObjectItemCaseSensitive(json, "active_player");

    if (!cJSON_IsNumber(size_item) || !cJSON_IsArray(board_item) || !cJSON_IsString(active_item)) {
        cJSON_Delete(json);
        return NULL;
    }

    int size = size_item->valueint;
    if (size < 3 || size > 25) {
        cJSON_Delete(json);
        return NULL;
    }

    int total = size * size;
    if (cJSON_GetArraySize(board_item) != total) {
        cJSON_Delete(json);
        return NULL;
    }

    Game *game = calloc(1, sizeof(Game));
    if (game == NULL) {
        cJSON_Delete(json);
        return NULL;
    }

    game->board = malloc(sizeof(int) * (size_t)total);
    if (game->board == NULL) {
        free(game);
        cJSON_Delete(json);
        return NULL;
    }

    game->game_id = game_id;
    game->size = size;
    game->x_conn_fd = -1;
    game->o_conn_fd = -1;

    for (int i = 0; i < total; ++i) {
        cJSON *entry = cJSON_GetArrayItem(board_item, i);
        if (!cJSON_IsNumber(entry)) {
            free(game->board);
            free(game);
            cJSON_Delete(json);
            return NULL;
        }
        game->board[i] = entry->valueint;
    }

    game->active_player = strcmp(active_item->valuestring, "O") == 0 ? ROLE_O : ROLE_X;

    if (!parse_json_string_field(json, "x_token", game->x_token, sizeof(game->x_token), &game->has_x) ||
        !parse_json_string_field(json, "o_token", game->o_token, sizeof(game->o_token), &game->has_o)) {
        free(game->board);
        free(game);
        cJSON_Delete(json);
        return NULL;
    }

    cJSON *x_disc_item = cJSON_GetObjectItemCaseSensitive(json, "x_disconnected");
    cJSON *o_disc_item = cJSON_GetObjectItemCaseSensitive(json, "o_disconnected");
    game->x_disconnected = cJSON_IsTrue(x_disc_item);
    game->o_disconnected = cJSON_IsTrue(o_disc_item);

    cJSON *status_item = cJSON_GetObjectItemCaseSensitive(json, "status");
    if (cJSON_IsString(status_item) && status_item->valuestring != NULL) {
        game->status = string_to_status(status_item->valuestring);
    } else {
        game->status = STATUS_WAITING;
    }

    cJSON *winner_item = cJSON_GetObjectItemCaseSensitive(json, "winner");
    if (cJSON_IsString(winner_item) && winner_item->valuestring != NULL) {
        game->winner = string_to_winner(winner_item->valuestring);
    } else {
        game->winner = WINNER_NONE;
    }

    bool has_reason = false;
    if (!parse_json_string_field(json, "finish_reason", game->finish_reason, sizeof(game->finish_reason), &has_reason)) {
        free(game->board);
        free(game);
        cJSON_Delete(json);
        return NULL;
    }
    if (!has_reason) {
        snprintf(game->finish_reason, sizeof(game->finish_reason), "none");
    }

    cJSON *created_item = cJSON_GetObjectItemCaseSensitive(json, "created_at");
    cJSON *updated_item = cJSON_GetObjectItemCaseSensitive(json, "updated_at");
    game->created_at = cJSON_IsNumber(created_item) ? (time_t)created_item->valuedouble : time(NULL);
    game->updated_at = cJSON_IsNumber(updated_item) ? (time_t)updated_item->valuedouble : game->created_at;

    cJSON_Delete(json);
    return game;
}

static Game *get_game_or_load(int game_id) {
    Game *loaded = find_loaded_game(game_id);
    if (loaded != NULL) {
        return loaded;
    }

    Game *from_disk = load_game_from_disk(game_id);
    if (from_disk != NULL) {
        add_loaded_game(from_disk);
    }
    return from_disk;
}

static cJSON *make_error_response(const char *code, const char *message, const Game *game_optional) {
    cJSON *response = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();

    cJSON_AddBoolToObject(response, "ok", false);
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(response, "error", error);

    if (game_optional != NULL) {
        cJSON_AddItemToObject(response, "state", game_to_json_state(game_optional));
    }

    return response;
}

static cJSON *err_invalid_number_field(const char *field) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Field '%s' must be a number", field);
    return make_error_response("INVALID_REQUEST", msg, NULL);
}

static cJSON *err_invalid_string_field(const char *field) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Field '%s' must be a string", field);
    return make_error_response("INVALID_REQUEST", msg, NULL);
}

static cJSON *err_game_not_found(void) {
    return make_error_response("GAME_NOT_FOUND", "No game exists with that game_id", NULL);
}

static cJSON *err_game_finished(const Game *game) {
    return make_error_response("GAME_FINISHED", "Finished games accept only GET_STATE", game);
}

static cJSON *err_persist_failed(const Game *game) {
    return make_error_response("SERVER_ERROR", "Failed to persist game", game);
}

static cJSON *make_state_ok_response(const Game *game) {
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddItemToObject(response, "state", game_to_json_state(game));
    return response;
}

static bool persist_touched_game(Game *game) {
    touch_game(game);
    if (!save_game(game)) {
        fprintf(stderr, "Failed to persist game #%d\n", game->game_id);
        return false;
    }
    return true;
}

static bool send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static void send_json_response(int fd, cJSON *response) {
    char *encoded = cJSON_PrintUnformatted(response);
    if (encoded == NULL) {
        return;
    }

    send_all(fd, encoded, strlen(encoded));
    send_all(fd, "\n", 1);
    free(encoded);
}

static bool get_required_int(cJSON *json, const char *name, int *out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valueint;
    return true;
}

static bool get_required_string(cJSON *json, const char *name, const char **out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    *out = item->valuestring;
    return true;
}

static const char *get_optional_string(cJSON *json, const char *name) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return NULL;
    }
    return item->valuestring;
}

static int role_from_token(const Game *game, const char *token) {
    if (token == NULL) {
        return ROLE_NONE;
    }

    if (game->has_x && strcmp(token, game->x_token) == 0) {
        return ROLE_X;
    }
    if (game->has_o && strcmp(token, game->o_token) == 0) {
        return ROLE_O;
    }
    return ROLE_NONE;
}

static void set_game_finished(Game *game, int winner, const char *reason) {
    game->status = STATUS_FINISHED;
    game->winner = winner;
    snprintf(game->finish_reason, sizeof(game->finish_reason), "%s", reason);
}

static void evaluate_end_conditions(Game *game) {
    int x_count = 0;
    int o_count = 0;
    int empty_count = 0;

    game_logic_count_cells(game->board, game->size, &x_count, &o_count, &empty_count);

    if (x_count == 0 && o_count == 0) {
        set_game_finished(game, WINNER_DRAW, "both_zero");
        return;
    }

    if (x_count == 0) {
        set_game_finished(game, WINNER_O, "elimination");
        return;
    }

    if (o_count == 0) {
        set_game_finished(game, WINNER_X, "elimination");
        return;
    }

    if (empty_count == 0) {
        if (x_count > o_count) {
            set_game_finished(game, WINNER_X, "full_board");
        } else if (o_count > x_count) {
            set_game_finished(game, WINNER_O, "full_board");
        } else {
            set_game_finished(game, WINNER_DRAW, "full_board");
        }
    }
}

static cJSON *make_game_logic_error_response(GameLogicError err, const Game *game) {
    switch (err) {
        case GAME_LOGIC_ERR_INVALID_ARGS:
            return make_error_response("GAME_LOGIC_INVALID_ARGS", "Invalid board/size passed to game logic", game);
        case GAME_LOGIC_ERR_INVALID_DIRECTION:
            return make_error_response("GAME_LOGIC_INVALID_DIRECTION", "Invalid direction passed to game logic", game);
        case GAME_LOGIC_ERR_INVALID_PLAYER:
            return make_error_response("GAME_LOGIC_INVALID_PLAYER", "Invalid player passed to game logic", game);
        case GAME_LOGIC_ERR_ALLOC:
            return make_error_response("SERVER_ERROR", "Game logic allocation failure", game);
        case GAME_LOGIC_ERR_NONE:
        default:
            return make_error_response("SERVER_ERROR", "Unknown game logic failure", game);
    }
}

static cJSON *get_game_by_id_from_request(cJSON *request, Game **out_game) {
    int game_id = 0;
    if (!get_required_int(request, "game_id", &game_id)) {
        return err_invalid_number_field("game_id");
    }

    Game *game = get_game_or_load(game_id);
    if (game == NULL) {
        return err_game_not_found();
    }

    *out_game = game;
    return NULL;
}

static Game *create_new_game(int size, int creator_fd) {
    Game *game = calloc(1, sizeof(Game));
    if (game == NULL) {
        return NULL;
    }

    int total = size * size;
    game->board = malloc(sizeof(int) * (size_t)total);
    if (game->board == NULL) {
        free(game);
        return NULL;
    }

    game->game_id = g_next_game_id++;
    game->size = size;
    game->active_player = ROLE_X;

    game_logic_seed_board(game->board, size);

    game->has_x = true;
    game->has_o = false;
    generate_token(game->x_token);
    game->o_token[0] = '\0';

    game->x_disconnected = false;
    game->o_disconnected = false;
    game->x_conn_fd = creator_fd;
    game->o_conn_fd = -1;

    game->status = STATUS_WAITING;
    game->winner = WINNER_NONE;
    snprintf(game->finish_reason, sizeof(game->finish_reason), "none");

    game->created_at = time(NULL);
    game->updated_at = game->created_at;

    add_loaded_game(game);
    return game;
}

static cJSON *handle_new_game(cJSON *request, int client_fd) {
    int size = 0;
    if (!get_required_int(request, "size", &size)) {
        return err_invalid_number_field("size");
    }

    if (size < 3 || size > 25) {
        return make_error_response("INVALID_SIZE", "size must be between 3 and 25", NULL);
    }

    Game *game = create_new_game(size, client_fd);
    if (game == NULL) {
        return make_error_response("SERVER_ERROR", "Failed to allocate game", NULL);
    }

    if (!save_game(game)) {
        return err_persist_failed(game);
    }

    cJSON *response = make_state_ok_response(game);
    cJSON_AddStringToObject(response, "role", "X");
    cJSON_AddStringToObject(response, "token", game->x_token);
    return response;
}

static cJSON *handle_join_game(cJSON *request, int client_fd) {
    Game *game = NULL;
    cJSON *lookup_err = get_game_by_id_from_request(request, &game);
    if (lookup_err != NULL) {
        return lookup_err;
    }

    if (game->status == STATUS_FINISHED) {
        return err_game_finished(game);
    }

    const char *token = get_optional_string(request, "token");
    int role = ROLE_SPECTATOR;
    bool changed = false;
    const char *return_token = NULL;

    if (token != NULL) {
        int existing_role = role_from_token(game, token);
        if (existing_role == ROLE_X) {
            role = ROLE_X;
            game->x_conn_fd = client_fd;
            game->x_disconnected = false;
            return_token = game->x_token;
            changed = true;
        } else if (existing_role == ROLE_O) {
            role = ROLE_O;
            game->o_conn_fd = client_fd;
            game->o_disconnected = false;
            return_token = game->o_token;
            changed = true;
        }
    }

    if (role == ROLE_SPECTATOR) {
        if (!game->has_x) {
            role = ROLE_X;
            game->has_x = true;
            game->x_disconnected = false;
            game->x_conn_fd = client_fd;
            generate_token(game->x_token);
            return_token = game->x_token;
            changed = true;
        } else if (!game->has_o) {
            role = ROLE_O;
            game->has_o = true;
            game->o_disconnected = false;
            game->o_conn_fd = client_fd;
            generate_token(game->o_token);
            return_token = game->o_token;
            changed = true;
        }
    }

    if (game->status == STATUS_WAITING && game->has_x && game->has_o) {
        game->status = STATUS_PLAYING;
        changed = true;
    }

    if (changed) {
        if (!persist_touched_game(game)) {
            return err_persist_failed(game);
        }
    }

    cJSON *response = make_state_ok_response(game);
    cJSON_AddStringToObject(response, "role", role_to_string(role));
    if (role == ROLE_X || role == ROLE_O) {
        cJSON_AddStringToObject(response, "token", return_token);
    }
    return response;
}

static cJSON *handle_get_state(cJSON *request, int client_fd) {
    (void)client_fd;
    Game *game = NULL;
    cJSON *lookup_err = get_game_by_id_from_request(request, &game);
    if (lookup_err != NULL) {
        return lookup_err;
    }

    return make_state_ok_response(game);
}

static cJSON *handle_move(cJSON *request, int client_fd) {
    (void)client_fd;
    int direction = -1;
    const char *token = NULL;

    Game *game = NULL;
    cJSON *lookup_err = get_game_by_id_from_request(request, &game);
    if (lookup_err != NULL) {
        return lookup_err;
    }
    if (!get_required_string(request, "token", &token)) {
        return err_invalid_string_field("token");
    }
    if (!get_required_int(request, "direction", &direction)) {
        return err_invalid_number_field("direction");
    }

    if (game->status == STATUS_FINISHED) {
        return err_game_finished(game);
    }

    if (game->status != STATUS_PLAYING) {
        return make_error_response("GAME_NOT_PLAYING", "Game is waiting for players", game);
    }

    if (!game_logic_is_valid_direction(direction)) {
        return make_error_response("INVALID_DIRECTION", "direction must be 0..3", game);
    }

    int role = role_from_token(game, token);
    if (role != ROLE_X && role != ROLE_O) {
        return make_error_response("UNAUTHORIZED", "Invalid player token", game);
    }

    if (game->active_player != role) {
        return make_error_response("NOT_YOUR_TURN", "It is not your turn", game);
    }

    int cell = role == ROLE_X ? CELL_X : CELL_O;
    GameLogicError logic_err = GAME_LOGIC_ERR_NONE;
    if (!game_logic_apply_move(game->board, game->size, cell, direction, &logic_err)) {
        return make_game_logic_error_response(logic_err, game);
    }

    game->active_player = (role == ROLE_X) ? ROLE_O : ROLE_X;
    evaluate_end_conditions(game);

    if (!persist_touched_game(game)) {
        return err_persist_failed(game);
    }

    return make_state_ok_response(game);
}

static cJSON *handle_quit_game(cJSON *request, int client_fd) {
    (void)client_fd;
    const char *token = NULL;

    Game *game = NULL;
    cJSON *lookup_err = get_game_by_id_from_request(request, &game);
    if (lookup_err != NULL) {
        return lookup_err;
    }
    if (!get_required_string(request, "token", &token)) {
        return err_invalid_string_field("token");
    }

    if (game->status == STATUS_FINISHED) {
        return err_game_finished(game);
    }

    int role = role_from_token(game, token);
    if (role != ROLE_X && role != ROLE_O) {
        return make_error_response("UNAUTHORIZED", "Invalid player token", game);
    }

    if (role == ROLE_X) {
        if (game->has_o) {
            set_game_finished(game, WINNER_O, "quit");
        } else {
            set_game_finished(game, WINNER_DRAW, "quit");
        }
    } else {
        if (game->has_x) {
            set_game_finished(game, WINNER_X, "quit");
        } else {
            set_game_finished(game, WINNER_DRAW, "quit");
        }
    }

    if (!persist_touched_game(game)) {
        return err_persist_failed(game);
    }

    return make_state_ok_response(game);
}

static cJSON *dispatch_request(const char *line, int client_fd) {
    cJSON *request = cJSON_Parse(line);
    if (request == NULL || !cJSON_IsObject(request)) {
        if (request != NULL) {
            cJSON_Delete(request);
        }
        return make_error_response("INVALID_JSON", "Request must be a valid JSON object", NULL);
    }

    const char *cmd = NULL;
    if (!get_required_string(request, "cmd", &cmd)) {
        cJSON_Delete(request);
        return err_invalid_string_field("cmd");
    }

    typedef cJSON *(*CommandHandler)(cJSON *, int);
    typedef struct {
        const char *name;
        CommandHandler handler;
    } CommandEntry;

    static const CommandEntry commands[] = {
        {"NEW_GAME", handle_new_game},
        {"JOIN_GAME", handle_join_game},
        {"MOVE", handle_move},
        {"GET_STATE", handle_get_state},
        {"QUIT_GAME", handle_quit_game},
    };

    cJSON *response = NULL;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (strcmp(cmd, commands[i].name) == 0) {
            response = commands[i].handler(request, client_fd);
            break;
        }
    }

    if (response == NULL) {
        response = make_error_response("UNKNOWN_COMMAND", "Unsupported cmd", NULL);
    }

    cJSON_Delete(request);
    return response;
}

static int create_server_socket(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_listen_port);
    if (inet_pton(AF_INET, g_listen_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind IP address: %s\n", g_listen_ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 32) != 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static void update_disconnect_flags_for_fd(int fd) {
    Game *game = g_games;
    while (game != NULL) {
        bool changed = false;
        if (game->x_conn_fd == fd) {
            game->x_conn_fd = -1;
            if (game->status != STATUS_FINISHED) {
                game->x_disconnected = true;
                changed = true;
            }
        }
        if (game->o_conn_fd == fd) {
            game->o_conn_fd = -1;
            if (game->status != STATUS_FINISHED) {
                game->o_disconnected = true;
                changed = true;
            }
        }

        if (changed) {
            (void)persist_touched_game(game);
        }

        game = game->next;
    }
}

static void close_client(ClientConn *client) {
    if (client->fd >= 0) {
        update_disconnect_flags_for_fd(client->fd);
        close(client->fd);
        client->fd = -1;
        client->len = 0;
    }
}

static bool find_line_end(const char *buffer, size_t len, size_t *line_end_index) {
    for (size_t i = 0; i < len; ++i) {
        if (buffer[i] == '\n') {
            *line_end_index = i;
            return true;
        }
    }
    return false;
}

static void handle_client_readable(ClientConn *client) {
    char incoming[4096];
    ssize_t n = recv(client->fd, incoming, sizeof(incoming), 0);

    if (n <= 0) {
        close_client(client);
        return;
    }

    if (client->len + (size_t)n >= CLIENT_BUFFER_SIZE) {
        cJSON *error = make_error_response("REQUEST_TOO_LARGE", "Request exceeds buffer limit", NULL);
        send_json_response(client->fd, error);
        cJSON_Delete(error);
        close_client(client);
        return;
    }

    memcpy(client->buffer + client->len, incoming, (size_t)n);
    client->len += (size_t)n;

    size_t line_end = 0;
    while (find_line_end(client->buffer, client->len, &line_end)) {
        size_t line_len = line_end;
        if (line_len >= LINE_BUFFER_SIZE) {
            cJSON *error = make_error_response("REQUEST_TOO_LARGE", "Request line exceeds limit", NULL);
            send_json_response(client->fd, error);
            cJSON_Delete(error);
            close_client(client);
            return;
        }

        char line[LINE_BUFFER_SIZE];
        memcpy(line, client->buffer, line_len);
        line[line_len] = '\0';

        if (line_len > 0 && line[line_len - 1] == '\r') {
            line[line_len - 1] = '\0';
        }

        size_t remaining = client->len - (line_end + 1);
        memmove(client->buffer, client->buffer + line_end + 1, remaining);
        client->len = remaining;

        if (line[0] == '\0') {
            continue;
        }

        cJSON *response = dispatch_request(line, client->fd);
        send_json_response(client->fd, response);
        cJSON_Delete(response);

        if (client->fd < 0) {
            return;
        }
    }
}

static int compute_next_game_id(void) {
    DIR *dir = opendir(DATA_DIR);
    if (dir == NULL) {
        return 1;
    }

    int max_id = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int id = 0;
        char tail[8] = {0};
        if (sscanf(entry->d_name, "game_%d.%7s", &id, tail) == 2) {
            if (strcmp(tail, "json") == 0 && id > max_id) {
                max_id = id;
            }
        }
    }

    closedir(dir);
    return max_id + 1;
}

int main(int argc, char **argv) {
    if (argc >= 2) {
        snprintf(g_listen_ip, sizeof(g_listen_ip), "%s", argv[1]);
    }
    if (argc >= 3) {
        char *end = NULL;
        long parsed = strtol(argv[2], &end, 10);
        if (end == NULL || *end != '\0' || parsed < 1 || parsed > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            fprintf(stderr, "Usage: %s [bind_ip] [port]\n", argv[0]);
            return 1;
        }
        g_listen_port = (int)parsed;
    }
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [bind_ip] [port]\n", argv[0]);
        return 1;
    }

    srand((unsigned)(time(NULL) ^ (unsigned)getpid()));

    ensure_data_dir();
    g_next_game_id = compute_next_game_id();

    int server_fd = create_server_socket();
    if (server_fd < 0) {
        return 1;
    }

    ClientConn *clients = calloc(MAX_CLIENTS, sizeof(ClientConn));
    if (clients == NULL) {
        fprintf(stderr, "Failed to allocate client table\n");
        close(server_fd);
        return 1;
    }
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        clients[i].fd = -1;
        clients[i].len = 0;
    }

    printf("XO-catch server listening on %s:%d\n", g_listen_ip, g_listen_port);

    while (true) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(server_fd, &read_set);

        int max_fd = server_fd;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].fd >= 0) {
                FD_SET(clients[i].fd, &read_set);
                if (clients[i].fd > max_fd) {
                    max_fd = clients[i].fd;
                }
            }
        }

        int ready = select(max_fd + 1, &read_set, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        if (FD_ISSET(server_fd, &read_set)) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd >= 0) {
                bool added = false;
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    if (clients[i].fd < 0) {
                        clients[i].fd = client_fd;
                        clients[i].len = 0;
                        added = true;
                        break;
                    }
                }
                if (!added) {
                    close(client_fd);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].fd >= 0 && FD_ISSET(clients[i].fd, &read_set)) {
                handle_client_readable(&clients[i]);
            }
        }
    }

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd >= 0) {
            close_client(&clients[i]);
        }
    }

    free(clients);
    close(server_fd);
    return 0;
}
