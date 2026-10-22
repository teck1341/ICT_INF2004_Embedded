/**
 * @file net_mqtt_ipc.c
 * @brief Three bounded micro T-Kernel message buffers, all calls polling.
 * Kernel headers deliberately remain isolated from ISO stddef/lwIP headers.
 * NOTE: net_mqtt_ipc shares net_mqtt's first eight filename characters;
 * this explicit adapter boundary is a proposed BARR-C 4.1.b deviation.
 */
#include "net_mqtt_ipc.h"

#include <tk/tkernel.h>

#define NET_MQTT_IPC_DEPTH 4U
/* The kernel stores each message as a 4-byte header plus its length rounded
 * up to 4 bytes (messagebuf.h HEADERSZ and ROUNDSZ): words per message.
 */
#define NET_MQTT_IPC_WORDS(maximum) ((((maximum) + 3U) / 4U) + 1U)
/* HEADERSZ (4 B) + ROUNDSZ(20 B) = 24 B per frame and 4 + ROUNDSZ(1) = 8 B
 * per command: a change to either maximum must revisit the buffer sizes.
 */
#if (NET_MQTT_IPC_WORDS(NET_MQTT_IPC_TX_MAX) != 6U) || \
    (NET_MQTT_IPC_WORDS(NET_MQTT_IPC_RX_MAX) != 2U)
#error "IPC slots must hold the 4-byte header plus the rounded body"
#endif

static ID gh_tx_id   = 0;
static ID gh_rx_id   = 0;
static ID gh_stop_id = 0;

/* 4 x 24 B telemetry, 4 x 8 B commands, one 8 B pending STOP. */
static uint32_t g_tx_storage[NET_MQTT_IPC_DEPTH *
                             NET_MQTT_IPC_WORDS(NET_MQTT_IPC_TX_MAX)];
static uint32_t g_rx_storage[NET_MQTT_IPC_DEPTH *
                             NET_MQTT_IPC_WORDS(NET_MQTT_IPC_RX_MAX)];
static uint32_t g_stop_storage[NET_MQTT_IPC_WORDS(NET_MQTT_IPC_RX_MAX)];

static ID net_mqtt_ipc_create (void * p_buffer, uint32_t size,
                              uint32_t maximum);
static uint32_t net_mqtt_ipc_take (ID h_buffer, uint8_t * p_data);

/** @brief Called exactly once before task creation; idempotent on success. */
bool net_mqtt_ipc_start (void)
{
    ER result;

    if (gh_tx_id > 0)
    {
        return true;
    }
    gh_tx_id = net_mqtt_ipc_create(g_tx_storage, sizeof(g_tx_storage),
                                 NET_MQTT_IPC_TX_MAX);
    if (gh_tx_id <= 0)
    {
        return false;
    }
    gh_rx_id = net_mqtt_ipc_create(g_rx_storage, sizeof(g_rx_storage),
                                 NET_MQTT_IPC_RX_MAX);
    if (gh_rx_id > 0)
    {
        gh_stop_id = net_mqtt_ipc_create(g_stop_storage, sizeof(g_stop_storage),
                                       NET_MQTT_IPC_RX_MAX);
    }
    if ((gh_rx_id <= 0) || (gh_stop_id <= 0))
    {
        result = tk_del_mbf(gh_tx_id);
        /* Cleanup result cannot repair creation; report start failure. */
        (void)result;
        if (gh_rx_id > 0)
        {
            result = tk_del_mbf(gh_rx_id);
            /* Same fail-closed cleanup policy as above. */
            (void)result;
        }
        gh_tx_id = 0;
        gh_rx_id = 0;
        return false;
    }
    return true;
}

/** @brief Enqueue one telemetry frame without delaying control tasks. */
bool net_mqtt_ipc_send (const uint8_t * p_frame, uint32_t length)
{
    if ((NULL == p_frame) || (0U == length) ||
        (length > NET_MQTT_IPC_TX_MAX) || (gh_tx_id <= 0))
    {
        return false;
    }
    /* Validated length is at most 20, representable in kernel INT. */
    return E_OK == tk_snd_mbf(gh_tx_id, p_frame, (INT)length, TMO_POL);
}

/** @brief Return one command byte; a pending STOP always wins. */
bool net_mqtt_ipc_receive (uint8_t * p_command)
{
    if ((NULL == p_command) || (gh_rx_id <= 0) || (gh_stop_id <= 0))
    {
        return false;
    }
    if (0U == net_mqtt_ipc_take(gh_stop_id, p_command))
    {
        return 0U != net_mqtt_ipc_take(gh_rx_id, p_command);
    }
    return true;
}

/** @brief Only the radio task consumes the telemetry queue. */
uint32_t net_mqtt_ipc_take_tx (uint8_t * p_frame, uint32_t capacity)
{
    if ((NULL == p_frame) || (capacity < NET_MQTT_IPC_TX_MAX) ||
        (gh_tx_id <= 0))
    {
        return 0U;
    }
    return net_mqtt_ipc_take(gh_tx_id, p_frame);
}

/** @brief Route the STOP byte separately; duplicates remain safely pending.
 */
bool net_mqtt_ipc_put_rx (uint8_t command)
{
    ID h_target = gh_rx_id;
    ER result;

    if (gh_rx_id <= 0)
    {
        return false;
    }
    if (NET_MQTT_IPC_STOP_BYTE == command)
    {
        h_target = gh_stop_id;
    }
    result = tk_snd_mbf(h_target, &command, (INT)NET_MQTT_IPC_RX_MAX,
                        TMO_POL);
    return (E_OK == result) ||
           ((h_target == gh_stop_id) && (E_TMOUT == result));
}

/** @brief Create fixed user storage; kernel copies each complete message. */
static ID net_mqtt_ipc_create (void * p_buffer, uint32_t size,
                              uint32_t maximum)
{
    T_CMBF config = {0};

    config.mbfatr = TA_TFIFO | TA_USERBUF;
    /* All callers use static buffers <=96 bytes and messages <=20. */
    config.bufsz  = (SZ)size;
    config.maxmsz = (INT)maximum;
    config.bufptr = p_buffer;
    return tk_cre_mbf(&config);
}

/** @brief Internal callers guarantee capacity for the maximum message. */
static uint32_t net_mqtt_ipc_take (ID h_buffer, uint8_t * p_data)
{
    INT length = tk_rcv_mbf(h_buffer, p_data, TMO_POL);

    if (length <= 0)
    {
        return 0U;
    }
    /* Positive message length is bounded by the configured maximum, 20. */
    return (uint32_t)length;
}

/* End of file. */

