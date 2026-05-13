#ifndef MAIN_EVENT_BITS_H
#define MAIN_EVENT_BITS_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

/*
 * Shared event bit masks for Application main-loop (C/C++).
 * Keep identical to legacy MAIN_EVENT_* in application.h.
 */
#define MAIN_EVENT_SCHEDULE             ((EventBits_t)1u << 0)
#define MAIN_EVENT_SEND_AUDIO           ((EventBits_t)1u << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   ((EventBits_t)1u << 2)
#define MAIN_EVENT_VAD_CHANGE           ((EventBits_t)1u << 3)
#define MAIN_EVENT_ERROR                ((EventBits_t)1u << 4)
#define MAIN_EVENT_ACTIVATION_DONE      ((EventBits_t)1u << 5)
#define MAIN_EVENT_CLOCK_TICK           ((EventBits_t)1u << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    ((EventBits_t)1u << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED ((EventBits_t)1u << 8)
#define MAIN_EVENT_TOGGLE_CHAT          ((EventBits_t)1u << 9)
#define MAIN_EVENT_START_LISTENING      ((EventBits_t)1u << 10)
#define MAIN_EVENT_STOP_LISTENING       ((EventBits_t)1u << 11)
#define MAIN_EVENT_STATE_CHANGED        ((EventBits_t)1u << 12)

#define MAIN_EVENT_MASK_ALL                                                \
    (MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO |                          \
     MAIN_EVENT_WAKE_WORD_DETECTED | MAIN_EVENT_VAD_CHANGE |                \
     MAIN_EVENT_ERROR | MAIN_EVENT_ACTIVATION_DONE | MAIN_EVENT_CLOCK_TICK | \
     MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED |     \
     MAIN_EVENT_TOGGLE_CHAT | MAIN_EVENT_START_LISTENING |                  \
     MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_STATE_CHANGED)

#endif /* MAIN_EVENT_BITS_H */
