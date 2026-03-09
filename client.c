#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "raylib.h"

#if __has_include("cJSON.h")
#include "cJSON.h"
#elif __has_include(<cjson/cJSON.h>)
#include <cjson/cJSON.h>
#elif __has_include(<cJSON.h>)
#include <cJSON.h>
#else
#error "cJSON headers not found. Install libcjson development package."
#endif

#define SCREEN_W 980
#define SCREEN_H 820
#define STATUS_BAR_H 36
#define CLIENT_RECV_BUFFER 65536
#define RESPONSE_LINE_MAX 65536
#define REQUEST_TIMEOUT_MS 1200
#define CONNECT_TIMEOUT_MS 800
#define POLL_INTERVAL_SEC 0.25
#define RECONNECT_INTERVAL_SEC 1.0
#define MIN_BOARD_SIZE 3
#define MAX_BOARD_SIZE 25

typedef enum {
    APP_MENU = 0,
    APP_PLAYING
} AppScreen;

typedef enum {
    CLIENT_ROLE_NONE = 0,
    CLIENT_ROLE_X,
    CLIENT_ROLE_O,
    CLIENT_ROLE_SPECTATOR
} ClientRole;

typedef struct {
    int game_id;
    int size;
    int *board;
    bool x_disconnected;
    bool o_disconnected;
    char active_player[16];
    char status[16];
    char winner[16];
    char finish_reason[32];
    bool valid;
} RemoteState;

typedef struct {
    bool ok;
    bool has_state;
    bool has_role;
    bool has_token;
    char role[16];
    char token[32];
    char error_code[64];
    char error_message[256];
} ServerResponse;

typedef struct {
    int fd;
    char host[128];
    int port;
    char recv_buffer[CLIENT_RECV_BUFFER];
    size_t recv_len;
} NetClient;

typedef struct {
    AppScreen screen;
    int create_size;
    char join_input[16];
    bool join_input_active;

    ClientRole role;
    char token[32];
    int game_id;

    RemoteState state;

    char status_message[256];
    bool status_is_error;

    double last_poll_time;
    double last_reconnect_attempt;
} App;

static bool is_mouse_over(Rectangle rect) {
    Vector2 m = GetMousePosition();
    return m.x >= rect.x && m.x <= rect.x + rect.width &&
           m.y >= rect.y && m.y <= rect.y + rect.height;
}

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static void set_status(App *app, bool is_error, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(app->status_message, sizeof(app->status_message), fmt, args);
    va_end(args);
    app->status_is_error = is_error;
}

static const char *client_role_to_string(ClientRole role) {
    switch (role) {
        case CLIENT_ROLE_X:
            return "X";
        case CLIENT_ROLE_O:
            return "O";
        case CLIENT_ROLE_SPECTATOR:
            return "SPECTATOR";
        default:
            return "NONE";
    }
}

static ClientRole client_role_from_string(const char *value) {
    if (value == NULL) {
        return CLIENT_ROLE_NONE;
    }
    if (strcmp(value, "X") == 0) {
        return CLIENT_ROLE_X;
    }
    if (strcmp(value, "O") == 0) {
        return CLIENT_ROLE_O;
    }
    if (strcmp(value, "SPECTATOR") == 0) {
        return CLIENT_ROLE_SPECTATOR;
    }
    return CLIENT_ROLE_NONE;
}

static bool is_player_role(ClientRole role) {
    return role == CLIENT_ROLE_X || role == CLIENT_ROLE_O;
}

static void remote_state_clear(RemoteState *state) {
    free(state->board);
    state->board = NULL;
    state->size = 0;
    state->game_id = 0;
    state->x_disconnected = false;
    state->o_disconnected = false;
    state->active_player[0] = '\0';
    state->status[0] = '\0';
    state->winner[0] = '\0';
    state->finish_reason[0] = '\0';
    state->valid = false;
}

static bool parse_remote_state(RemoteState *state, cJSON *state_json, char *err, size_t err_size) {
    cJSON *game_id_item = cJSON_GetObjectItemCaseSensitive(state_json, "game_id");
    cJSON *size_item = cJSON_GetObjectItemCaseSensitive(state_json, "size");
    cJSON *board_item = cJSON_GetObjectItemCaseSensitive(state_json, "board");
    cJSON *active_player_item = cJSON_GetObjectItemCaseSensitive(state_json, "active_player");
    cJSON *status_item = cJSON_GetObjectItemCaseSensitive(state_json, "status");

    if (!cJSON_IsNumber(game_id_item) ||
        !cJSON_IsNumber(size_item) ||
        !cJSON_IsArray(board_item) ||
        !cJSON_IsString(active_player_item) ||
        !cJSON_IsString(status_item)) {
        snprintf(err, err_size, "Malformed state payload");
        return false;
    }

    int size = size_item->valueint;
    if (size < MIN_BOARD_SIZE || size > MAX_BOARD_SIZE) {
        snprintf(err, err_size, "Invalid board size in state");
        return false;
    }

    int total = size * size;
    if (cJSON_GetArraySize(board_item) != total) {
        snprintf(err, err_size, "Invalid board length in state");
        return false;
    }

    int *new_board = malloc(sizeof(int) * (size_t)total);
    if (new_board == NULL) {
        snprintf(err, err_size, "Out of memory for board");
        return false;
    }

    for (int i = 0; i < total; ++i) {
        cJSON *cell = cJSON_GetArrayItem(board_item, i);
        if (!cJSON_IsNumber(cell)) {
            free(new_board);
            snprintf(err, err_size, "Non-numeric board cell in state");
            return false;
        }
        int value = cell->valueint;
        if (value < 0 || value > 2) {
            free(new_board);
            snprintf(err, err_size, "Board cell out of range in state");
            return false;
        }
        new_board[i] = value;
    }

    cJSON *x_disc_item = cJSON_GetObjectItemCaseSensitive(state_json, "x_disconnected");
    cJSON *o_disc_item = cJSON_GetObjectItemCaseSensitive(state_json, "o_disconnected");
    cJSON *winner_item = cJSON_GetObjectItemCaseSensitive(state_json, "winner");
    cJSON *reason_item = cJSON_GetObjectItemCaseSensitive(state_json, "finish_reason");

    free(state->board);
    state->board = new_board;
    state->game_id = game_id_item->valueint;
    state->size = size;
    state->x_disconnected = cJSON_IsTrue(x_disc_item);
    state->o_disconnected = cJSON_IsTrue(o_disc_item);

    snprintf(state->active_player, sizeof(state->active_player), "%s", active_player_item->valuestring);
    snprintf(state->status, sizeof(state->status), "%s", status_item->valuestring);

    if (cJSON_IsString(winner_item) && winner_item->valuestring != NULL) {
        snprintf(state->winner, sizeof(state->winner), "%s", winner_item->valuestring);
    } else {
        state->winner[0] = '\0';
    }

    if (cJSON_IsString(reason_item) && reason_item->valuestring != NULL) {
        snprintf(state->finish_reason, sizeof(state->finish_reason), "%s", reason_item->valuestring);
    } else {
        state->finish_reason[0] = '\0';
    }

    state->valid = true;
    return true;
}

static void server_response_clear(ServerResponse *resp) {
    memset(resp, 0, sizeof(*resp));
}

static void net_client_init(NetClient *client, const char *host, int port) {
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    snprintf(client->host, sizeof(client->host), "%s", host);
    client->port = port;
}

static void net_close(NetClient *client) {
    if (client->fd >= 0) {
        close(client->fd);
    }
    client->fd = -1;
    client->recv_len = 0;
}

static bool net_set_blocking(int fd, bool blocking) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }

    if (blocking) {
        flags &= ~O_NONBLOCK;
    } else {
        flags |= O_NONBLOCK;
    }

    return fcntl(fd, F_SETFL, flags) == 0;
}

static bool net_connect(NetClient *client, int timeout_ms, char *err, size_t err_size) {
    if (client->fd >= 0) {
        return true;
    }

    struct addrinfo hints;
    struct addrinfo *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", client->port);

    int gai = getaddrinfo(client->host, port_str, &hints, &result);
    if (gai != 0) {
        snprintf(err, err_size, "DNS/addr error: %s", gai_strerror(gai));
        return false;
    }

    bool connected = false;
    for (struct addrinfo *it = result; it != NULL; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (!net_set_blocking(fd, false)) {
            close(fd);
            continue;
        }

        int rc = connect(fd, it->ai_addr, it->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            close(fd);
            continue;
        }

        if (rc < 0) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(fd, &write_set);
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;

            int ready = select(fd + 1, NULL, &write_set, NULL, &tv);
            if (ready <= 0) {
                close(fd);
                continue;
            }

            int so_error = 0;
            socklen_t so_len = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 || so_error != 0) {
                close(fd);
                continue;
            }
        }

        if (!net_set_blocking(fd, true)) {
            close(fd);
            continue;
        }

        client->fd = fd;
        client->recv_len = 0;
        connected = true;
        break;
    }

    freeaddrinfo(result);

    if (!connected) {
        snprintf(err, err_size, "Failed to connect to %s:%d", client->host, client->port);
    }

    return connected;
}

static bool net_send_all(int fd, const char *data, size_t len) {
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

static bool net_extract_line(NetClient *client, char *out_line, size_t out_size) {
    for (size_t i = 0; i < client->recv_len; ++i) {
        if (client->recv_buffer[i] != '\n') {
            continue;
        }

        size_t line_len = i;
        if (line_len >= out_size) {
            return false;
        }

        memcpy(out_line, client->recv_buffer, line_len);
        out_line[line_len] = '\0';
        if (line_len > 0 && out_line[line_len - 1] == '\r') {
            out_line[line_len - 1] = '\0';
        }

        size_t remaining = client->recv_len - (i + 1);
        memmove(client->recv_buffer, client->recv_buffer + i + 1, remaining);
        client->recv_len = remaining;
        return true;
    }

    return false;
}

static bool net_recv_line(NetClient *client, int timeout_ms, char *out_line, size_t out_size, char *err, size_t err_size) {
    if (net_extract_line(client, out_line, out_size)) {
        return true;
    }

    long long deadline = monotonic_ms() + timeout_ms;

    while (true) {
        long long now = monotonic_ms();
        if (now >= deadline) {
            snprintf(err, err_size, "Timed out waiting for server response");
            return false;
        }

        int wait_ms = (int)(deadline - now);
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(client->fd, &read_set);

        struct timeval tv;
        tv.tv_sec = wait_ms / 1000;
        tv.tv_usec = (wait_ms % 1000) * 1000;

        int ready = select(client->fd + 1, &read_set, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            snprintf(err, err_size, "select() failed while reading response");
            return false;
        }

        if (ready == 0) {
            continue;
        }

        char chunk[4096];
        ssize_t n = recv(client->fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            snprintf(err, err_size, "Server closed connection");
            return false;
        }

        if (client->recv_len + (size_t)n >= CLIENT_RECV_BUFFER) {
            snprintf(err, err_size, "Response buffer overflow");
            return false;
        }

        memcpy(client->recv_buffer + client->recv_len, chunk, (size_t)n);
        client->recv_len += (size_t)n;

        if (net_extract_line(client, out_line, out_size)) {
            return true;
        }
    }
}

static bool parse_server_response(const char *line, ServerResponse *resp, RemoteState *state, char *err, size_t err_size) {
    server_response_clear(resp);

    cJSON *root = cJSON_Parse(line);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(err, err_size, "Invalid JSON response from server");
        return false;
    }

    cJSON *ok_item = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsBool(ok_item)) {
        cJSON_Delete(root);
        snprintf(err, err_size, "Response missing boolean 'ok'");
        return false;
    }

    resp->ok = cJSON_IsTrue(ok_item);

    cJSON *state_item = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsObject(state_item)) {
        if (!parse_remote_state(state, state_item, err, err_size)) {
            cJSON_Delete(root);
            return false;
        }
        resp->has_state = true;
    }

    if (resp->ok) {
        cJSON *role_item = cJSON_GetObjectItemCaseSensitive(root, "role");
        if (cJSON_IsString(role_item) && role_item->valuestring != NULL) {
            snprintf(resp->role, sizeof(resp->role), "%s", role_item->valuestring);
            resp->has_role = true;
        }

        cJSON *token_item = cJSON_GetObjectItemCaseSensitive(root, "token");
        if (cJSON_IsString(token_item) && token_item->valuestring != NULL) {
            snprintf(resp->token, sizeof(resp->token), "%s", token_item->valuestring);
            resp->has_token = true;
        }
    } else {
        cJSON *error_item = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (cJSON_IsObject(error_item)) {
            cJSON *code_item = cJSON_GetObjectItemCaseSensitive(error_item, "code");
            cJSON *message_item = cJSON_GetObjectItemCaseSensitive(error_item, "message");

            if (cJSON_IsString(code_item) && code_item->valuestring != NULL) {
                snprintf(resp->error_code, sizeof(resp->error_code), "%s", code_item->valuestring);
            }
            if (cJSON_IsString(message_item) && message_item->valuestring != NULL) {
                snprintf(resp->error_message, sizeof(resp->error_message), "%s", message_item->valuestring);
            }
        }

        if (resp->error_code[0] == '\0') {
            snprintf(resp->error_code, sizeof(resp->error_code), "SERVER_ERROR");
        }
        if (resp->error_message[0] == '\0') {
            snprintf(resp->error_message, sizeof(resp->error_message), "Server returned an error");
        }
    }

    cJSON_Delete(root);
    return true;
}

static bool send_command(
    NetClient *net,
    cJSON *request,
    RemoteState *state,
    ServerResponse *resp,
    char *err,
    size_t err_size
) {
    char *encoded = cJSON_PrintUnformatted(request);
    if (encoded == NULL) {
        snprintf(err, err_size, "Failed to encode request JSON");
        return false;
    }

    if (!net_connect(net, CONNECT_TIMEOUT_MS, err, err_size)) {
        free(encoded);
        return false;
    }

    bool send_ok = net_send_all(net->fd, encoded, strlen(encoded)) && net_send_all(net->fd, "\n", 1);
    free(encoded);

    if (!send_ok) {
        snprintf(err, err_size, "Failed to send request to server");
        net_close(net);
        return false;
    }

    char line[RESPONSE_LINE_MAX];
    if (!net_recv_line(net, REQUEST_TIMEOUT_MS, line, sizeof(line), err, err_size)) {
        net_close(net);
        return false;
    }

    if (!parse_server_response(line, resp, state, err, err_size)) {
        return false;
    }

    return true;
}

static bool cmd_new_game(NetClient *net, int size, RemoteState *state, ServerResponse *resp, char *err, size_t err_size) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "cmd", "NEW_GAME");
    cJSON_AddNumberToObject(request, "size", size);

    bool ok = send_command(net, request, state, resp, err, err_size);
    cJSON_Delete(request);
    return ok;
}

static bool cmd_join_game(
    NetClient *net,
    int game_id,
    const char *token,
    RemoteState *state,
    ServerResponse *resp,
    char *err,
    size_t err_size
) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "cmd", "JOIN_GAME");
    cJSON_AddNumberToObject(request, "game_id", game_id);
    if (token != NULL && token[0] != '\0') {
        cJSON_AddStringToObject(request, "token", token);
    }

    bool ok = send_command(net, request, state, resp, err, err_size);
    cJSON_Delete(request);
    return ok;
}

static bool cmd_get_state(NetClient *net, int game_id, RemoteState *state, ServerResponse *resp, char *err, size_t err_size) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "cmd", "GET_STATE");
    cJSON_AddNumberToObject(request, "game_id", game_id);

    bool ok = send_command(net, request, state, resp, err, err_size);
    cJSON_Delete(request);
    return ok;
}

static bool cmd_move(
    NetClient *net,
    int game_id,
    const char *token,
    int direction,
    RemoteState *state,
    ServerResponse *resp,
    char *err,
    size_t err_size
) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "cmd", "MOVE");
    cJSON_AddNumberToObject(request, "game_id", game_id);
    cJSON_AddStringToObject(request, "token", token);
    cJSON_AddNumberToObject(request, "direction", direction);

    bool ok = send_command(net, request, state, resp, err, err_size);
    cJSON_Delete(request);
    return ok;
}

static bool cmd_quit_game(
    NetClient *net,
    int game_id,
    const char *token,
    RemoteState *state,
    ServerResponse *resp,
    char *err,
    size_t err_size
) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "cmd", "QUIT_GAME");
    cJSON_AddNumberToObject(request, "game_id", game_id);
    cJSON_AddStringToObject(request, "token", token);

    bool ok = send_command(net, request, state, resp, err, err_size);
    cJSON_Delete(request);
    return ok;
}

static void reset_to_menu(App *app) {
    app->screen = APP_MENU;
    app->role = CLIENT_ROLE_NONE;
    app->token[0] = '\0';
    app->game_id = 0;
    app->last_poll_time = 0.0;
    app->last_reconnect_attempt = 0.0;
    remote_state_clear(&app->state);
}

static void apply_join_or_create_result(App *app, const ServerResponse *resp) {
    app->game_id = app->state.game_id;
    app->screen = APP_PLAYING;
    app->last_poll_time = 0.0;
    app->last_reconnect_attempt = 0.0;

    if (resp->has_role) {
        app->role = client_role_from_string(resp->role);
    }
    if (resp->has_token) {
        snprintf(app->token, sizeof(app->token), "%s", resp->token);
    } else {
        app->token[0] = '\0';
    }
}

static void update_join_input(App *app) {
    if (!app->join_input_active) {
        return;
    }

    int key = GetCharPressed();
    while (key > 0) {
        if (key >= '0' && key <= '9') {
            size_t len = strlen(app->join_input);
            if (len + 1 < sizeof(app->join_input)) {
                app->join_input[len] = (char)key;
                app->join_input[len + 1] = '\0';
            }
        }
        key = GetCharPressed();
    }

    if (IsKeyPressed(KEY_BACKSPACE)) {
        size_t len = strlen(app->join_input);
        if (len > 0) {
            app->join_input[len - 1] = '\0';
        }
    }
}

static bool reconnect_with_join(App *app, NetClient *net) {
    if (app->game_id <= 0) {
        return false;
    }

    ServerResponse resp;
    char err[256];

    const char *token = NULL;
    if (is_player_role(app->role) && app->token[0] != '\0') {
        token = app->token;
    }

    if (!cmd_join_game(net, app->game_id, token, &app->state, &resp, err, sizeof(err))) {
        set_status(app, true, "Reconnect failed: %s", err);
        return false;
    }

    if (!resp.ok) {
        set_status(app, true, "Reconnect rejected: %s (%s)", resp.error_message, resp.error_code);
        return false;
    }

    if (resp.has_role) {
        app->role = client_role_from_string(resp.role);
    }

    if (resp.has_token) {
        snprintf(app->token, sizeof(app->token), "%s", resp.token);
    } else if (app->role == CLIENT_ROLE_SPECTATOR || app->role == CLIENT_ROLE_NONE) {
        app->token[0] = '\0';
    }

    set_status(app, false, "Reconnected to game #%d as %s", app->game_id, client_role_to_string(app->role));
    return true;
}

static void poll_game_state(App *app, NetClient *net) {
    if (app->screen != APP_PLAYING || app->game_id <= 0) {
        return;
    }

    double now = GetTime();
    if (now - app->last_poll_time < POLL_INTERVAL_SEC) {
        return;
    }
    app->last_poll_time = now;

    if (net->fd < 0) {
        if (now - app->last_reconnect_attempt < RECONNECT_INTERVAL_SEC) {
            return;
        }
        app->last_reconnect_attempt = now;
        reconnect_with_join(app, net);
        return;
    }

    bool had_prev_state = app->state.valid;
    char prev_status[16] = {0};
    char prev_active_player[16] = {0};
    bool prev_x_disconnected = false;
    bool prev_o_disconnected = false;
    if (had_prev_state) {
        snprintf(prev_status, sizeof(prev_status), "%s", app->state.status);
        snprintf(prev_active_player, sizeof(prev_active_player), "%s", app->state.active_player);
        prev_x_disconnected = app->state.x_disconnected;
        prev_o_disconnected = app->state.o_disconnected;
    }

    ServerResponse resp;
    char err[256];
    if (!cmd_get_state(net, app->game_id, &app->state, &resp, err, sizeof(err))) {
        set_status(app, true, "Lost connection: %s", err);
        return;
    }

    if (!resp.ok) {
        set_status(app, true, "GET_STATE failed: %s (%s)", resp.error_message, resp.error_code);
        return;
    }

    if (!had_prev_state) {
        set_status(app, false, "Game state synchronized.");
        return;
    }

    if (strcmp(prev_status, app->state.status) != 0) {
        if (strcmp(prev_status, "waiting") == 0 &&
            strcmp(app->state.status, "playing") == 0) {
            set_status(app, false, "Second player joined. Game started.");
            return;
        }

        if (strcmp(app->state.status, "finished") == 0) {
            set_status(app, false, "Game finished: winner=%s reason=%s",
                       app->state.winner[0] ? app->state.winner : "none",
                       app->state.finish_reason[0] ? app->state.finish_reason : "none");
            return;
        }

        set_status(app, false, "Game status changed: %s -> %s", prev_status, app->state.status);
        return;
    }

    if (strcmp(prev_active_player, app->state.active_player) != 0 &&
        strcmp(app->state.status, "playing") == 0) {
        if (is_player_role(app->role) &&
            strcmp(app->state.active_player, client_role_to_string(app->role)) == 0) {
            set_status(app, false, "Your turn.");
        } else {
            set_status(app, false, "Turn changed: %s to move.", app->state.active_player);
        }
        return;
    }

    if (prev_x_disconnected != app->state.x_disconnected ||
        prev_o_disconnected != app->state.o_disconnected) {
        set_status(app, false, "Connection update: X %s, O %s.",
                   app->state.x_disconnected ? "offline" : "online",
                   app->state.o_disconnected ? "offline" : "online");
        return;
    }

}

static void try_submit_move(App *app, NetClient *net, int direction) {
    if (app->screen != APP_PLAYING || app->game_id <= 0) {
        return;
    }

    if (app->role == CLIENT_ROLE_SPECTATOR) {
        set_status(app, true, "Spectator mode is read-only");
        return;
    }

    if (!is_player_role(app->role) || app->token[0] == '\0') {
        set_status(app, true, "Missing player token");
        return;
    }

    if (!app->state.valid) {
        set_status(app, true, "No game state loaded");
        return;
    }

    if (strcmp(app->state.status, "playing") != 0) {
        if (strcmp(app->state.status, "waiting") == 0) {
            set_status(app, true, "Game is waiting for second player");
        }
        return;
    }

    const char *my_role = client_role_to_string(app->role);
    if (strcmp(app->state.active_player, my_role) != 0) {
        set_status(app, true, "Not your turn");
        return;
    }

    ServerResponse resp;
    char err[256];
    if (!cmd_move(net, app->game_id, app->token, direction, &app->state, &resp, err, sizeof(err))) {
        set_status(app, true, "Move failed: %s", err);
        return;
    }

    if (!resp.ok) {
        set_status(app, true, "Move rejected: %s (%s)", resp.error_message, resp.error_code);
        return;
    }

    set_status(app, false, "Move accepted");
}

static void draw_status_bar(const App *app) {
    Color bg = app->status_is_error ? (Color){150, 30, 30, 255} : (Color){36, 90, 56, 255};
    DrawRectangle(0, 0, SCREEN_W, STATUS_BAR_H, bg);
    DrawText(app->status_message[0] ? app->status_message : "Ready",
             12, 10, 16, RAYWHITE);
}

static void draw_board(const RemoteState *state, int top_y) {
    if (!state->valid || state->board == NULL || state->size <= 0) {
        DrawText("No board loaded", 40, top_y + 40, 22, DARKGRAY);
        return;
    }

    int margin_sides = 40;
    int avail_w = SCREEN_W - margin_sides * 2;
    int avail_h = SCREEN_H - top_y - 40;
    int cell_size = avail_w / state->size;
    if (cell_size * state->size > avail_h) {
        cell_size = avail_h / state->size;
    }
    if (cell_size < 8) {
        cell_size = 8;
    }

    int board_w = cell_size * state->size;
    int board_x = (SCREEN_W - board_w) / 2;
    int board_y = top_y;

    for (int y = 0; y < state->size; ++y) {
        for (int x = 0; x < state->size; ++x) {
            Rectangle cell = {
                (float)(board_x + x * cell_size),
                (float)(board_y + y * cell_size),
                (float)cell_size,
                (float)cell_size
            };

            DrawRectangleRec(cell, BEIGE);
            DrawRectangleLinesEx(cell, 1, LIGHTGRAY);

            int value = state->board[y * state->size + x];
            if (value == 1) {
                int pad = cell_size / 6;
                float thickness = (float)(cell_size / 10);
                if (thickness < 2.0f) {
                    thickness = 2.0f;
                }

                DrawLineEx(
                    (Vector2){cell.x + (float)pad, cell.y + (float)pad},
                    (Vector2){cell.x + cell.width - (float)pad, cell.y + cell.height - (float)pad},
                    thickness,
                    MAROON
                );
                DrawLineEx(
                    (Vector2){cell.x + (float)pad, cell.y + cell.height - (float)pad},
                    (Vector2){cell.x + cell.width - (float)pad, cell.y + (float)pad},
                    thickness,
                    MAROON
                );
            } else if (value == 2) {
                int cx = (int)cell.x + (int)cell.width / 2;
                int cy = (int)cell.y + (int)cell.height / 2;
                float r = cell.width / 2.0f - cell.width / 6.0f;
                float ring_thickness = cell.width / 10.0f;
                if (ring_thickness < 2.0f) {
                    ring_thickness = 2.0f;
                }
                float inner_r = r - ring_thickness / 2.0f;
                float outer_r = r + ring_thickness / 2.0f;
                if (inner_r < 1.0f) {
                    inner_r = 1.0f;
                }

                DrawRing((Vector2){(float)cx, (float)cy}, inner_r, outer_r, 0.0f, 360.0f, 48, DARKBLUE);
            }
        }
    }
}

static void draw_menu(App *app, Rectangle minus_btn, Rectangle plus_btn, Rectangle create_btn,
                      Rectangle join_input_box, Rectangle join_btn, Rectangle exit_btn) {
    DrawText("XO-catch", 40, 54, 34, DARKGRAY);

    DrawText("Create Game", 120, 120, 24, DARKGRAY);
    DrawText("Board Size", 120, 158, 20, DARKGRAY);

    DrawRectangleRec(minus_btn, LIGHTGRAY);
    DrawText("-", (int)minus_btn.x + 20, (int)minus_btn.y + 8, 38, BLACK);

    DrawText(TextFormat("%dx%d", app->create_size, app->create_size),
             220, 212, 30, BLACK);

    DrawRectangleRec(plus_btn, LIGHTGRAY);
    DrawText("+", (int)plus_btn.x + 17, (int)plus_btn.y + 8, 38, BLACK);

    Color create_col = is_mouse_over(create_btn) ? DARKGREEN : GREEN;
    DrawRectangleRec(create_btn, create_col);
    DrawText("Create", (int)create_btn.x + 72, (int)create_btn.y + 16, 24, WHITE);

    DrawText("Join Game", 120, 314, 24, DARKGRAY);
    Color input_bg = app->join_input_active ? (Color){255, 255, 255, 255} : (Color){245, 245, 245, 255};
    DrawRectangleRec(join_input_box, input_bg);
    DrawRectangleLinesEx(join_input_box, 2, app->join_input_active ? BLUE : GRAY);

    const char *join_text = app->join_input[0] ? app->join_input : "Enter game ID";
    DrawText(join_text, (int)join_input_box.x + 12, (int)join_input_box.y + 14,
             24, app->join_input[0] ? BLACK : GRAY);

    Color join_col = is_mouse_over(join_btn) ? DARKBLUE : BLUE;
    DrawRectangleRec(join_btn, join_col);
    DrawText("Join", (int)join_btn.x + 44, (int)join_btn.y + 16, 24, WHITE);

    Color exit_col = is_mouse_over(exit_btn) ? MAROON : RED;
    DrawRectangleRec(exit_btn, exit_col);
    DrawText("Exit", (int)exit_btn.x + 94, (int)exit_btn.y + 16, 24, WHITE);
}

static void draw_gameplay(const App *app, Rectangle return_btn) {
    DrawText(TextFormat("Game #%d", app->game_id), 20, 46, 24, DARKGRAY);
    DrawText(TextFormat("Role: %s", client_role_to_string(app->role)), 220, 50, 22, DARKGRAY);

    if (app->state.valid) {
        DrawText(TextFormat("Turn: %s", app->state.active_player), 420, 50, 22, BLACK);
        DrawText(TextFormat("Status: %s", app->state.status), 600, 50, 22, BLACK);
    }

    if (app->role == CLIENT_ROLE_SPECTATOR) {
        DrawRectangle(20, 84, SCREEN_W - 40, 32, (Color){30, 62, 102, 255});
        DrawText("Spectator mode (read-only)", 34, 92, 18, RAYWHITE);
    } else if (app->state.valid && strcmp(app->state.status, "waiting") == 0) {
        DrawRectangle(20, 84, SCREEN_W - 40, 32, (Color){122, 96, 24, 255});
        DrawText("Waiting for opponent to join...", 34, 92, 18, RAYWHITE);
    }

    draw_board(&app->state, 130);

    DrawText("Move: Arrow keys or WASD", 20, SCREEN_H - 42, 18, DARKGRAY);
    DrawText("ESC: quit game (player) / return to menu (spectator)", 20, SCREEN_H - 22, 16, GRAY);

    if (app->state.valid && strcmp(app->state.status, "finished") == 0) {
        DrawRectangle(0, 0, SCREEN_W, SCREEN_H, (Color){0, 0, 0, 120});

        Rectangle panel = {SCREEN_W / 2.0f - 230.0f, SCREEN_H / 2.0f - 130.0f, 460.0f, 260.0f};
        DrawRectangleRec(panel, RAYWHITE);
        DrawRectangleLinesEx(panel, 2, DARKGRAY);

        DrawText("Game Finished", (int)panel.x + 130, (int)panel.y + 22, 34, BLACK);

        const char *winner = app->state.winner[0] ? app->state.winner : "none";
        const char *reason = app->state.finish_reason[0] ? app->state.finish_reason : "none";

        DrawText(TextFormat("Winner: %s", winner), (int)panel.x + 50, (int)panel.y + 90, 24, DARKGRAY);
        DrawText(TextFormat("Reason: %s", reason), (int)panel.x + 50, (int)panel.y + 126, 24, DARKGRAY);

        Color btn_color = is_mouse_over(return_btn) ? DARKGREEN : GREEN;
        DrawRectangleRec(return_btn, btn_color);
        DrawText("Return to Menu", (int)return_btn.x + 24, (int)return_btn.y + 14, 26, WHITE);
    }
}

static int parse_join_game_id(const char *text) {
    if (text == NULL || text[0] == '\0') {
        return -1;
    }

    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == NULL || *end != '\0') {
        return -1;
    }
    if (value <= 0 || value > 2147483647L) {
        return -1;
    }

    return (int)value;
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 8080;

    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = atoi(argv[2]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            return 1;
        }
    }
    if (argc > 3) {
        fprintf(stderr, "Usage: %s [host] [port]\n", argv[0]);
        return 1;
    }

    App app;
    memset(&app, 0, sizeof(app));
    app.screen = APP_MENU;
    app.create_size = 9;
    app.role = CLIENT_ROLE_NONE;
    app.state.board = NULL;
    set_status(&app, false, "Ready. Server target: %s:%d", host, port);

    NetClient net;
    net_client_init(&net, host, port);

    InitWindow(SCREEN_W, SCREEN_H, "XO-catch Client");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    Rectangle minus_btn = {120, 188, 56, 56};
    Rectangle plus_btn = {344, 188, 56, 56};
    Rectangle create_btn = {120, 258, 280, 58};

    Rectangle join_input_box = {120, 352, 280, 56};
    Rectangle join_btn = {420, 352, 140, 56};
    Rectangle exit_btn = {120, 438, 280, 58};

    Rectangle finished_return_btn = {SCREEN_W / 2.0f - 130.0f, SCREEN_H / 2.0f + 56.0f, 260.0f, 58.0f};

    while (!WindowShouldClose()) {
        if (app.screen == APP_MENU) {
            update_join_input(&app);

            bool clicked = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
            if (clicked) {
                if (is_mouse_over(join_input_box)) {
                    app.join_input_active = true;
                } else {
                    app.join_input_active = false;
                }
            }

            if (clicked && is_mouse_over(minus_btn) && app.create_size > MIN_BOARD_SIZE) {
                app.create_size--;
            }
            if (clicked && is_mouse_over(plus_btn) && app.create_size < MAX_BOARD_SIZE) {
                app.create_size++;
            }

            if (clicked && is_mouse_over(create_btn)) {
                ServerResponse resp;
                char err[256];
                if (!cmd_new_game(&net, app.create_size, &app.state, &resp, err, sizeof(err))) {
                    set_status(&app, true, "NEW_GAME failed: %s", err);
                } else if (!resp.ok) {
                    set_status(&app, true, "NEW_GAME rejected: %s (%s)", resp.error_message, resp.error_code);
                } else {
                    apply_join_or_create_result(&app, &resp);
                    set_status(&app, false, "Created game #%d as X", app.game_id);
                }
            }

            if ((clicked && is_mouse_over(join_btn)) || (app.join_input_active && IsKeyPressed(KEY_ENTER))) {
                int game_id = parse_join_game_id(app.join_input);
                if (game_id <= 0) {
                    set_status(&app, true, "Enter a valid numeric game ID");
                } else {
                    ServerResponse resp;
                    char err[256];
                    if (!cmd_join_game(&net, game_id, NULL, &app.state, &resp, err, sizeof(err))) {
                        set_status(&app, true, "JOIN_GAME failed: %s", err);
                    } else if (!resp.ok) {
                        set_status(&app, true, "JOIN_GAME rejected: %s (%s)", resp.error_message, resp.error_code);
                    } else {
                        apply_join_or_create_result(&app, &resp);
                        set_status(&app, false, "Joined game #%d as %s", app.game_id, client_role_to_string(app.role));
                    }
                }
            }

            if (clicked && is_mouse_over(exit_btn)) {
                break;
            }
        } else if (app.screen == APP_PLAYING) {
            poll_game_state(&app, &net);

            if (IsKeyPressed(KEY_ESCAPE)) {
                if (app.role == CLIENT_ROLE_SPECTATOR) {
                    set_status(&app, false, "Returned to menu from spectator view");
                    reset_to_menu(&app);
                } else if (is_player_role(app.role)) {
                    if (net.fd < 0) {
                        set_status(&app, true, "Disconnected. Cannot QUIT_GAME until reconnect succeeds.");
                    } else {
                        ServerResponse resp;
                        char err[256];
                        if (!cmd_quit_game(&net, app.game_id, app.token, &app.state, &resp, err, sizeof(err))) {
                            set_status(&app, true, "QUIT_GAME failed: %s", err);
                        } else if (!resp.ok) {
                            set_status(&app, true, "QUIT_GAME rejected: %s (%s)", resp.error_message, resp.error_code);
                        } else {
                            set_status(&app, false, "Quit game #%d", app.game_id);
                            reset_to_menu(&app);
                        }
                    }
                }
            }

            bool finished = app.state.valid && strcmp(app.state.status, "finished") == 0;
            if (!finished) {
                int direction = -1;
                if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) {
                    direction = 0;
                } else if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) {
                    direction = 1;
                } else if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) {
                    direction = 2;
                } else if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) {
                    direction = 3;
                }

                if (direction >= 0) {
                    try_submit_move(&app, &net, direction);
                }
            } else if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && is_mouse_over(finished_return_btn)) {
                set_status(&app, false, "Returned to menu from finished game");
                reset_to_menu(&app);
            }
        }

        BeginDrawing();
        ClearBackground(RAYWHITE);

        draw_status_bar(&app);

        if (app.screen == APP_MENU) {
            draw_menu(&app, minus_btn, plus_btn, create_btn, join_input_box, join_btn, exit_btn);
        } else {
            draw_gameplay(&app, finished_return_btn);
        }

        EndDrawing();
    }

    net_close(&net);
    remote_state_clear(&app.state);
    CloseWindow();
    return 0;
}
