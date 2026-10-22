/**
 * @file net_mqtt.c
 * @brief MQTT 3.1.1 single-owner client over the professor's NO_SYS lwIP.
 * TCP/MQTT/Wi-Fi calls belong exclusively to cyw43_utk_task on core 0.
 * Control tasks communicate only through bounded kernel message buffers.
 * NOTE: BARR-C early returns keep failure paths shallow (6.2.c).
 */
#include "net_mqtt.h"
#include "net_mqtt_ipc.h"
#include "mqttcmd.h"
#include "telemetry.h"

#include <stdint.h>
#include <string.h>

/* The kernel-side IPC header cannot include the size_t-based contracts. */
_Static_assert(TELEMETRY_FRAME_MAX <= NET_MQTT_IPC_TX_MAX,
               "Every telemetry frame must fit one IPC message");
_Static_assert(MQTTCMD_LENGTH == NET_MQTT_IPC_RX_MAX,
               "A command is exactly one IPC byte");
_Static_assert(MQTTCMD_STOP_BYTE == NET_MQTT_IPC_STOP_BYTE,
               "STOP routing uses the command format's STOP byte");

#if ROBOT_MQTT
#include "cyw43.h"
#include "lwip/apps/mqtt.h"
#include "lwip/apps/mqtt_priv.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/sys.h"
#include "net_mqtt_config.h"
#include "wifi_credentials.h"

#define NET_MQTT_RETRY_MIN_MS 1000U
#define NET_MQTT_RETRY_MAX_MS 30000U
#define NET_MQTT_CONNECT_MS 30000U
#define NET_MQTT_PUBLISH_MS 100U
#define NET_MQTT_KEEPALIVE_S 15U
#define NET_MQTT_RETAIN_MASK 0x01U
#define NET_MQTT_TX_DRAIN_LIMIT 4U
/* Presence on the status topic: one byte, '1' online and '0' offline. */
#define NET_MQTT_ONLINE "1"
#define NET_MQTT_OFFLINE "0"
#define NET_MQTT_PRESENCE_BYTES 1U

static mqtt_client_t * gp_client = NULL;
static mqttcmd_t       g_command;
static ip_addr_t       g_broker_address;
static uint32_t        g_retry_started;
static uint32_t        g_retry_delay;
static uint32_t        g_connect_started;
static uint32_t        g_publish_started;
static uint32_t        g_wifi_started;
static uint32_t        g_wifi_delay;
static bool            gb_connecting;
static bool            gb_connected;
static bool            gb_subscribed;
static bool            gb_online_pending;
static bool            gb_wifi_joining;
static bool            gb_close_pending;
static bool            gb_command_topic;
static volatile uint8_t g_rejected_commands;
static uint8_t         g_pending_tx[NET_MQTT_IPC_TX_MAX];
static uint32_t        g_pending_length;

static const uint8_t g_wifi_ssid[] = UTK_WIFI_SSID;
static const uint8_t g_wifi_password[] = UTK_WIFI_PASSWORD;
static const struct mqtt_connect_client_info_t g_client_info =
{
    .client_id   = NET_MQTT_CLIENT_ID,
    .client_user = NET_MQTT_USERNAME,
    .client_pass = NET_MQTT_PASSWORD,
    .keep_alive  = NET_MQTT_KEEPALIVE_S,
    .will_topic  = NET_MQTT_STATUS_TOPIC,
    .will_msg    = NET_MQTT_OFFLINE,
    .will_qos    = 0U,
    .will_retain = 0U
};

_Static_assert(sizeof(g_wifi_ssid) > 1U, "Wi-Fi SSID must be configured");
_Static_assert(sizeof(g_wifi_ssid) <= 33U, "Wi-Fi SSID too long");
_Static_assert(sizeof(NET_MQTT_CLIENT_ID) > 1U, "Set unique MQTT ID");
_Static_assert(sizeof(NET_MQTT_CLIENT_ID) <= 65U, "MQTT ID too long");
_Static_assert(sizeof(NET_MQTT_COMMAND_TOPIC) <= 97U, "Topic too long");
_Static_assert(sizeof(NET_MQTT_TELEMETRY_TOPIC) <= 97U, "Topic too long");
_Static_assert(sizeof(NET_MQTT_STATUS_TOPIC) <= 97U, "Topic too long");
_Static_assert(sizeof(NET_MQTT_USERNAME) <= 65U, "Username too long");
_Static_assert(sizeof(NET_MQTT_PASSWORD) <= 65U, "Password too long");
_Static_assert(sizeof(NET_MQTT_COMMAND_TOPIC) > 1U, "Empty command topic");
_Static_assert(sizeof(NET_MQTT_TELEMETRY_TOPIC) > 1U, "Empty data topic");
_Static_assert(sizeof(NET_MQTT_STATUS_TOPIC) > 1U, "Empty status topic");
_Static_assert((NET_MQTT_PORT > 0U) && (NET_MQTT_PORT <= 65535U),
               "Broker port must be 1..65535");
_Static_assert((UTK_WIFI_AUTH == CYW43_AUTH_OPEN) ||
               ((sizeof(g_wifi_password) >= 9U) &&
                (sizeof(g_wifi_password) <= 64U)), "Invalid Wi-Fi password");

static void net_mqtt_connect (uint32_t now);
static void net_mqtt_retry (uint32_t now);
static void net_mqtt_transmit (uint32_t now);
static void net_mqtt_take_tx (void);
static void net_mqtt_wifi (uint32_t now, struct netif * p_interface);
static void net_mqtt_connection (mqtt_client_t * p_mqtt, void * p_context,
                                 mqtt_connection_status_t status);
static void net_mqtt_subscribed (void * p_context, err_t error);
static void net_mqtt_publication (void * p_context, const char * p_topic,
                                  u32_t total_length);
static void net_mqtt_fragment (void * p_context, const u8_t * p_data,
                               u16_t length, u8_t flags);
#endif

/** @brief Alias for ISO-C task callers; start is serialized before tasks. */
bool net_mqtt_start (void)
{
    return net_mqtt_ipc_start();
}

/** @brief Validate size_t before converting to the kernel-safe IPC width. */
bool net_mqtt_send (const uint8_t * p_frame, size_t length)
{
    if (length > NET_MQTT_IPC_TX_MAX)
    {
        return false;
    }
    /* At most 20 bytes, exactly representable in uint32_t. */
    return net_mqtt_ipc_send(p_frame, (uint32_t)length);
}

/** @brief One command byte crosses the boundary; STOP is taken first. */
bool net_mqtt_receive (uint8_t * p_command)
{
    return net_mqtt_ipc_receive(p_command);
}

/** @brief Single-writer session flag; the offline profile has none. */
bool net_mqtt_online (void)
{
#if ROBOT_MQTT
    return gb_subscribed;
#else
    return false;
#endif
}

/** @brief Single-writer wrapping count of rejected command payloads. */
uint8_t net_mqtt_rejected (void)
{
#if ROBOT_MQTT
    return g_rejected_commands;
#else
    return 0U;
#endif
}

/** @brief Pump bounded protocol work only inside the existing radio owner. */
void net_mqtt_poll (struct netif * p_interface)
{
#if ROBOT_MQTT
    uint32_t now = sys_now();
    bool b_publish_due =
        (now - g_publish_started) >= NET_MQTT_PUBLISH_MS;

    if (NULL == p_interface)
    {
        return;
    }
    if (gb_close_pending)
    {
        /* Close deferred from an lwIP callback, now outside its timer. */
        gb_close_pending = false;
        mqtt_disconnect(gp_client);
        net_mqtt_retry(now);
    }
    if (b_publish_due)
    {
        g_publish_started = now;
        net_mqtt_take_tx();
    }
    net_mqtt_wifi(now, p_interface);
    if ((!netif_is_up(p_interface)) ||
        (!netif_is_link_up(p_interface)) ||
        (ip4_addr_isany_val(*netif_ip4_addr(p_interface))))
    {
        if (gb_connected || gb_connecting)
        {
            mqtt_disconnect(gp_client);
            net_mqtt_retry(now);
        }
        return;
    }
    if (gb_connecting && ((now - g_connect_started) >= NET_MQTT_CONNECT_MS))
    {
        mqtt_disconnect(gp_client);
        net_mqtt_retry(now);
    }
    if ((!gb_connected) && (!gb_connecting) &&
        ((now - g_retry_started) >= g_retry_delay))
    {
        net_mqtt_connect(now);
    }
    if (gb_subscribed && b_publish_due)
    {
        net_mqtt_transmit(now);
    }
#else
    /* The disabled networking profile intentionally has no SDK dependency. */
    (void)p_interface;
#endif
}

#if ROBOT_MQTT
/** @brief One allocated client is reused; lwIP pool size bounds transport. */
static void net_mqtt_connect (uint32_t now)
{
    err_t result;

    if (NULL == gp_client)
    {
        gp_client = mqtt_client_new();
    }
    if ((NULL == gp_client) ||
        (0 == ipaddr_aton(NET_MQTT_BROKER_IPV4, &g_broker_address)))
    {
        net_mqtt_retry(now);
        return;
    }
    mqtt_set_inpub_callback(gp_client, net_mqtt_publication,
                             net_mqtt_fragment, NULL);
    g_connect_started = now;
    gb_connecting    = true;
    result = mqtt_client_connect(gp_client, &g_broker_address, NET_MQTT_PORT,
                                   net_mqtt_connection, NULL, &g_client_info);
    if (ERR_OK != result)
    {
        net_mqtt_retry(now);
    }
}

/** @brief Unsigned elapsed arithmetic is safe across the 32-bit ms wrap. */
static void net_mqtt_retry (uint32_t now)
{
    gb_connecting = false;
    gb_connected  = false;
    gb_subscribed = false;
    gb_online_pending = false;
    gb_close_pending = false;
    g_retry_started = now;
    g_pending_length = 0U;
    gb_command_topic = false;
    mqttcmd_begin(&g_command, 0U, false);
    if (g_retry_delay < NET_MQTT_RETRY_MIN_MS)
    {
        g_retry_delay = NET_MQTT_RETRY_MIN_MS;
    }
    else if (g_retry_delay < (NET_MQTT_RETRY_MAX_MS / 2U))
    {
        g_retry_delay *= 2U;
    }
    else
    {
        g_retry_delay = NET_MQTT_RETRY_MAX_MS;
    }
}

/** @brief QoS0 means accepted for transmission, never delivery guaranteed. */
static void net_mqtt_transmit (uint32_t now)
{
    err_t result;

    if (gb_online_pending)
    {
        result = mqtt_publish(gp_client, NET_MQTT_STATUS_TOPIC,
                               NET_MQTT_ONLINE, NET_MQTT_PRESENCE_BYTES, 0U,
                               0U, NULL, NULL);
        gb_online_pending = ERR_OK != result;
    }
    if (0U == g_pending_length)
    {
        return;
    }
    /* IPC lengths are <=20, exactly representable in lwIP's u16_t. */
    result = mqtt_publish(gp_client, NET_MQTT_TELEMETRY_TOPIC, g_pending_tx,
                           (u16_t)g_pending_length, 0U, 0U, NULL, NULL);
    if (ERR_OK == result)
    {
        g_pending_length = 0U;
    }
    else if (ERR_MEM == result)
    {
        /* Keep exactly one pending sample until the next 100 ms attempt. */
    }
    else
    {
        mqtt_disconnect(gp_client);
        net_mqtt_retry(now);
    }
}

/** @brief Preserve online frame order; keep only the newest while offline. */
static void net_mqtt_take_tx (void)
{
    uint32_t index;
    uint32_t length;
    uint8_t next_tx[NET_MQTT_IPC_TX_MAX];

    if (gb_subscribed)
    {
        if (0U == g_pending_length)
        {
            g_pending_length = net_mqtt_ipc_take_tx(g_pending_tx,
                                                  sizeof(g_pending_tx));
        }
        return;
    }
    /* Drain at most the four-message queue depth and retain the newest
     * sample. A full outage must not replay an unbounded telemetry history. */
    for (index = 0U; index < NET_MQTT_TX_DRAIN_LIMIT; index++)
    {
        length = net_mqtt_ipc_take_tx(next_tx, sizeof(next_tx));
        if (0U == length)
        {
            break;
        }
        memcpy(g_pending_tx, next_tx, length);
        g_pending_length = length;
    }
}

/** @brief Rejoin after link loss; each attempt has a 30 s bounded window. */
static void net_mqtt_wifi (uint32_t now, struct netif * p_interface)
{
    int result;

    if (cyw43_wifi_scan_active(&cyw43_state))
    {
        return;
    }
    if (netif_is_link_up(p_interface))
    {
        gb_wifi_joining = false;
        g_wifi_delay    = NET_MQTT_RETRY_MIN_MS;
        g_wifi_started  = now;
        return;
    }
    if (gb_wifi_joining)
    {
        if ((now - g_wifi_started) < NET_MQTT_CONNECT_MS)
        {
            return;
        }
        result = cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        /* Rejoin is still required after a failed leave; retain backoff. */
        (void)result;
        gb_wifi_joining = false;
        g_wifi_started  = now;
    }
    if ((now - g_wifi_started) < g_wifi_delay)
    {
        return;
    }
    result = cyw43_wifi_join(&cyw43_state, sizeof(g_wifi_ssid) - 1U,
                              g_wifi_ssid, sizeof(g_wifi_password) - 1U,
                              g_wifi_password, UTK_WIFI_AUTH, NULL,
                              CYW43_CHANNEL_NONE);
    g_wifi_started   = now;
    gb_wifi_joining = (0 == result);
    if (g_wifi_delay < NET_MQTT_RETRY_MIN_MS)
    {
        g_wifi_delay = NET_MQTT_RETRY_MIN_MS;
    }
    else if (g_wifi_delay < (NET_MQTT_RETRY_MAX_MS / 2U))
    {
        g_wifi_delay *= 2U;
    }
    else
    {
        g_wifi_delay = NET_MQTT_RETRY_MAX_MS;
    }
}

/** @brief Subscribe anew on every clean-session connection. */
static void net_mqtt_connection (mqtt_client_t * p_mqtt, void * p_context,
                                 mqtt_connection_status_t status)
{
    err_t result;

    /* Callback ABI context is unused because the service has one owner. */
    (void)p_context;
    gb_connecting = false;
    if (MQTT_CONNECT_ACCEPTED != status)
    {
        net_mqtt_retry(sys_now());
        return;
    }
    gb_connected = true;
    result = mqtt_subscribe(p_mqtt, NET_MQTT_COMMAND_TOPIC, 0U,
                             net_mqtt_subscribed, NULL);
    if (ERR_OK != result)
    {
        mqtt_disconnect(p_mqtt);
        net_mqtt_retry(sys_now());
    }
}

/** @brief Only a successful SUBACK enables telemetry and command handling.
 * A SUBACK timeout arrives from inside lwIP's cyclic MQTT timer, which
 * re-arms itself after this callback; closing here would leave a duplicate
 * timer after reconnection. The close is deferred to net_mqtt_poll instead.
 */
static void net_mqtt_subscribed (void * p_context, err_t error)
{
    /* Callback ABI context is unused in this single-client service. */
    (void)p_context;
    if (ERR_OK == error)
    {
        gb_subscribed = true;
        gb_online_pending = true;
        g_retry_delay  = 0U;
    }
    else
    {
        gb_close_pending = true;
    }
}

/** @brief Reject retained commands using the pinned lwIP fixed header. */
static void net_mqtt_publication (void * p_context, const char * p_topic,
                                  u32_t total_length)
{
    bool b_topic_matches;
    bool b_retained;

    /* Callback ABI context is unused in this single-client service. */
    (void)p_context;
    /* NOTE: Pinned mqtt.c calls this before changing rx_buffer[0]. This
     * narrow mqtt_priv.h dependency must be re-reviewed on SDK upgrades. */
    b_retained = 0U != (gp_client->rx_buffer[0] & NET_MQTT_RETAIN_MASK);
    b_topic_matches = gb_subscribed && (!b_retained) &&
                      (NULL != p_topic) &&
                      (0 == strcmp(p_topic, NET_MQTT_COMMAND_TOPIC));
    /* A rejected payload on the live command topic is counted, so a
     * malformed or legacy STOP is visible to the operator in "ign".
     */
    gb_command_topic = b_topic_matches;
    mqttcmd_begin(&g_command, total_length, b_topic_matches);
}

/** @brief Fragment assembly validates declared size before each copy. */
static void net_mqtt_fragment (void * p_context, const u8_t * p_data,
                               u16_t length, u8_t flags)
{
    bool b_queued;
    bool b_last = 0U != (flags & MQTT_DATA_FLAG_LAST);

    /* Callback ABI context is unused in this single-client service. */
    (void)p_context;
    if (mqttcmd_feed(&g_command, p_data, length, b_last))
    {
        b_queued = net_mqtt_ipc_put_rx(g_command.command);
        /* STOP has its own queue (a duplicate pending STOP reports success),
         * so only a valid non-STOP command can find its queue full: it is
         * dropped and counted with the rejected payloads.
         */
        if (!b_queued)
        {
            /* Unsigned increment wraps modulo 256; readers difference it. */
            g_rejected_commands = (uint8_t)(g_rejected_commands + 1U);
        }
    }
    else if (b_last && gb_command_topic)
    {
        /* Unsigned increment wraps modulo 256; readers difference it. */
        g_rejected_commands = (uint8_t)(g_rejected_commands + 1U);
    }
    else
    {
        /* Non-final fragment, or not a live command-topic publication. */
    }
    if (b_last)
    {
        gb_command_topic = false;
    }
}
#endif

/* End of file. */

