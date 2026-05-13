/*
 * WebSocket Control Server - Converted from C++ to C.
 * Provides a WebSocket endpoint at /ws that forwards messages to the MCP server.
 */

#include <cJSON.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "c_api/mcp_server_c_api.h"

#define TAG "WSControl"
#define MAX_WS_CLIENTS 8

typedef struct {
    httpd_handle_t server_handle;
    int client_fds[MAX_WS_CLIENTS];
    int client_count;
} ws_control_server_t;

static ws_control_server_t *g_ws_server = NULL;

static void ws_add_client(httpd_req_t *req) {
    if (!g_ws_server) return;
    int fd = httpd_req_to_sockfd(req);
    for (int i = 0; i < g_ws_server->client_count; i++) {
        if (g_ws_server->client_fds[i] == fd) return;
    }
    if (g_ws_server->client_count < MAX_WS_CLIENTS) {
        g_ws_server->client_fds[g_ws_server->client_count++] = fd;
        ESP_LOGI(TAG, "Client connected: %d (total: %d)", fd, g_ws_server->client_count);
    }
}

static void ws_remove_client(httpd_req_t *req) {
    if (!g_ws_server) return;
    int fd = httpd_req_to_sockfd(req);
    for (int i = 0; i < g_ws_server->client_count; i++) {
        if (g_ws_server->client_fds[i] == fd) {
            g_ws_server->client_fds[i] = g_ws_server->client_fds[--g_ws_server->client_count];
            ESP_LOGI(TAG, "Client disconnected: %d (total: %d)", fd, g_ws_server->client_count);
            return;
        }
    }
}

static void ws_handle_message(httpd_req_t *req, const char *data, size_t len) {
    if (!data || len == 0) {
        ESP_LOGE(TAG, "Invalid message: data is null or len is 0");
        return;
    }
    if (len > 4096) {
        ESP_LOGE(TAG, "Message too long: %zu bytes", len);
        return;
    }

    char *temp_buf = (char *)malloc(len + 1);
    if (!temp_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return;
    }
    memcpy(temp_buf, data, len);
    temp_buf[len] = '\0';

    cJSON *root = cJSON_Parse(temp_buf);
    free(temp_buf);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    cJSON *payload = NULL;
    cJSON *type = cJSON_GetObjectItem(root, "type");

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "mcp") == 0) {
        payload = cJSON_GetObjectItem(root, "payload");
        if (payload) {
            cJSON_DetachItemViaPointer(root, payload);
            mcp_server_parse_message_json(mcp_server_get_instance(), payload);
            cJSON_Delete(payload);
        }
    } else {
        payload = cJSON_Duplicate(root, 1);
        if (payload) {
            mcp_server_parse_message_json(mcp_server_get_instance(), payload);
            cJSON_Delete(payload);
        }
    }

    if (!payload) {
        ESP_LOGE(TAG, "Invalid message format or failed to parse");
    }

    cJSON_Delete(root);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (!g_ws_server) return ESP_FAIL;

    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        ws_add_client(req);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);

    if (ws_pkt.len) {
        buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
    }

    ESP_LOGI(TAG, "Packet type: %d", ws_pkt.type);

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "WebSocket close frame received");
        ws_remove_client(req);
        free(buf);
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        if (ws_pkt.len > 0 && buf) {
            buf[ws_pkt.len] = '\0';
            ws_handle_message(req, (const char *)buf, ws_pkt.len);
        }
    } else {
        ESP_LOGW(TAG, "Unsupported frame type: %d", ws_pkt.type);
    }

    free(buf);
    return ESP_OK;
}

bool ws_control_server_start(int port) {
    if (g_ws_server) return true;

    g_ws_server = (ws_control_server_t *)calloc(1, sizeof(ws_control_server_t));
    if (!g_ws_server) return false;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 7;
    config.ctrl_port = 32769;

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
    };

    if (httpd_start(&g_ws_server->server_handle, &config) == ESP_OK) {
        httpd_register_uri_handler(g_ws_server->server_handle, &ws_uri);
        ESP_LOGI(TAG, "WebSocket server started on port %d", port);
        return true;
    }

    ESP_LOGE(TAG, "Failed to start WebSocket server");
    free(g_ws_server);
    g_ws_server = NULL;
    return false;
}

void ws_control_server_stop(void) {
    if (!g_ws_server) return;
    if (g_ws_server->server_handle) {
        httpd_stop(g_ws_server->server_handle);
    }
    free(g_ws_server);
    g_ws_server = NULL;
    ESP_LOGI(TAG, "WebSocket server stopped");
}
