#include "audio_debugger.h"
#include "sdkconfig.h"

#include <stdlib.h>
#include <string.h>

#if CONFIG_USE_AUDIO_DEBUGGER
#include <esp_log.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#endif

#define TAG "AudioDebugger"

struct audio_debugger {
#if CONFIG_USE_AUDIO_DEBUGGER
    int udp_sockfd;
    struct sockaddr_in udp_server_addr;
#else
    char dummy;
#endif
};

audio_debugger_t *audio_debugger_create(void)
{
    audio_debugger_t *d = (audio_debugger_t *)calloc(1, sizeof(*d));
    if (!d) return NULL;

#if CONFIG_USE_AUDIO_DEBUGGER
    d->udp_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (d->udp_sockfd >= 0) {
        const char *server_addr = CONFIG_AUDIO_DEBUG_UDP_SERVER;
        const char *colon = strchr(server_addr, ':');
        if (colon) {
            char ip[64];
            size_t ip_len = (size_t)(colon - server_addr);
            if (ip_len >= sizeof(ip)) ip_len = sizeof(ip) - 1;
            memcpy(ip, server_addr, ip_len);
            ip[ip_len] = '\0';
            int port = atoi(colon + 1);

            memset(&d->udp_server_addr, 0, sizeof(d->udp_server_addr));
            d->udp_server_addr.sin_family = AF_INET;
            d->udp_server_addr.sin_port = htons((uint16_t)port);
            inet_pton(AF_INET, ip, &d->udp_server_addr.sin_addr);

            ESP_LOGI(TAG, "Initialized server address: %s", CONFIG_AUDIO_DEBUG_UDP_SERVER);
        } else {
            ESP_LOGW(TAG, "Invalid server address: %s, should be IP:PORT", CONFIG_AUDIO_DEBUG_UDP_SERVER);
            close(d->udp_sockfd);
            d->udp_sockfd = -1;
        }
    } else {
        ESP_LOGW(TAG, "Failed to create UDP socket: %d", errno);
    }
#endif
    return d;
}

void audio_debugger_destroy(audio_debugger_t *d)
{
    if (!d) return;
#if CONFIG_USE_AUDIO_DEBUGGER
    if (d->udp_sockfd >= 0) {
        close(d->udp_sockfd);
        ESP_LOGI(TAG, "Closed UDP socket");
    }
#endif
    free(d);
}

void audio_debugger_feed(audio_debugger_t *d, const int16_t *data, size_t samples)
{
    if (!d) return;
#if CONFIG_USE_AUDIO_DEBUGGER
    if (d->udp_sockfd >= 0) {
        ssize_t sent = sendto(d->udp_sockfd, data, samples * sizeof(int16_t), 0,
                             (struct sockaddr *)&d->udp_server_addr, sizeof(d->udp_server_addr));
        if (sent < 0) {
            ESP_LOGW(TAG, "Failed to send audio data to %s: %d", CONFIG_AUDIO_DEBUG_UDP_SERVER, errno);
        } else {
            ESP_LOGD(TAG, "Sent %d bytes audio data to %s", (int)sent, CONFIG_AUDIO_DEBUG_UDP_SERVER);
        }
    }
#endif
}
