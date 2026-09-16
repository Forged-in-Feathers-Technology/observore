#include "observore_improv.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "observore_netcfg.h"
#include "observore_wifi.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

/* A C5 board has two USB sockets and either one can be the one in somebody's
 * hand -- the browser flashes happily over both. The console can only read
 * from one of them, though: ESP-IDF's secondary console is output only, so
 * with UART as primary the native port can be talked *to* and never hears a
 * reply request. Listening on the USB peripheral directly is what makes the
 * choice of socket stop mattering. */
#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
#define IMPROV_SECOND_TRANSPORT 1
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#define IMPROV_SECOND_TRANSPORT 0
#endif

static const char *TAG = "observore.improv";

/* Wire format, from the specification:
 *   "IMPROV" | version | type | length | data... | checksum
 * The checksum is the low byte of the sum of every preceding byte, header
 * included.  The RPC payload nested inside a result carries no checksum of its
 * own -- the outer one covers it. */
static const char MAGIC[6] = {'I', 'M', 'P', 'R', 'O', 'V'};
#define IMPROV_VERSION 0x01

#define TYPE_CURRENT_STATE 0x01
#define TYPE_ERROR_STATE   0x02
#define TYPE_RPC           0x03
#define TYPE_RPC_RESULT    0x04

#define STATE_STOPPED      0x00
#define STATE_READY        0x02
#define STATE_PROVISIONING 0x03
#define STATE_PROVISIONED  0x04

#define ERR_NONE           0x00
#define ERR_INVALID_RPC    0x01
#define ERR_UNKNOWN_RPC    0x02
#define ERR_CANNOT_CONNECT 0x03
#define ERR_UNKNOWN        0xFF

#define CMD_WIFI_SETTINGS    0x01
#define CMD_GET_STATE        0x02
#define CMD_GET_DEVICE_INFO  0x03
#define CMD_GET_NETWORKS     0x04

#define MAX_PAYLOAD 256

/* How long to let beacons arrive before telling a client there are no networks.
 * A sniff sweep dwells a few hundred milliseconds per channel and an access
 * point beacons about ten times a second, so a handful of seconds is enough to
 * hear the ones nearby without leaving the dialog looking hung. */
#define NETWORK_WAIT_MS 8000
#define NETWORK_POLL_MS  250

static volatile bool s_provisioned;

/* ------------------------------------------------------------------ */
/* Transport                                                          */
/* ------------------------------------------------------------------ */

/* The console translates line endings in both directions by default, which
 * would corrupt any packet containing 0x0A or 0x0D -- and a checksum is a
 * uniformly distributed byte, so roughly one packet in thirty would break.
 *
 * Receive is switched to raw once and left there: nothing else on this device
 * reads stdin.  Transmit is switched only around a packet and put back, so the
 * log keeps its CRLF and terminals do not staircase. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static void tx_raw(bool raw)
{
    esp_line_endings_t mode = raw ? ESP_LINE_ENDINGS_LF : ESP_LINE_ENDINGS_CRLF;
    usb_serial_jtag_vfs_set_tx_line_endings(mode);
}
#endif

#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static bool s_uart_ready;
#endif

/* Set up the console-side transport.
 *
 * On a UART console this bypasses stdio and the VFS completely and uses the
 * driver, because going through them does not work -- and both halves failed
 * for different reasons, each measured on hardware rather than guessed:
 *
 *   read   Without the driver installed, the VFS reads the hardware FIFO at
 *          the instant it is asked and nothing buffers what arrives between
 *          polls.  An instrumented build logged "stdin read -> -1 (errno 11)"
 *          forever while packets were being sent at it.
 *
 *   write  With the driver installed, write(STDOUT_FILENO) returns -1 having
 *          written nothing: "console write: 0 of 11 (last -1)".  Log output
 *          still appeared, so the failure was invisible from the outside --
 *          the device looked alive and simply never answered.
 *
 * uart_read_bytes and uart_write_bytes have neither problem, and as a bonus
 * they carry no line-ending translation, so nothing has to be toggled around
 * a binary packet. */
static void transport_init(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
#else
    esp_err_t err = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 256, 0, NULL, 0);
    s_uart_ready = (err == ESP_OK || err == ESP_ERR_INVALID_STATE);
    if (!s_uart_ready) {
        ESP_LOGW(TAG, "no UART transport: %s", esp_err_to_name(err));
        return;
    }
    /* Route the log through the driver as well, so it queues behind a packet
     * instead of racing it.  Without this the two reach the UART by different
     * paths -- the log straight at the registers through the VFS, packets
     * through the driver's ring -- and a log line emitted mid-packet lands in
     * the middle of one, which the client sees as a checksum failure on
     * roughly every other reply. */
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#endif
}

/* Which transport the request being answered arrived on.  A reply is written
 * back to that one specifically: the console mirrors log output to both ports,
 * but binary written through it would be mirrored to a port nobody is reading
 * and, worse, is not guaranteed to reach a port the console does not own. */
typedef enum { VIA_CONSOLE, VIA_USB_JTAG } improv_via_t;
static improv_via_t s_via = VIA_CONSOLE;

static void write_raw(const uint8_t *buf, size_t len)
{
#if IMPROV_SECOND_TRANSPORT
    if (s_via == VIA_USB_JTAG) {
        usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(100));
        return;
    }
#endif
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    tx_raw(true);
    fflush(stdout);                       /* don't interleave with buffered log */
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(STDOUT_FILENO, buf + sent, len - sent);
        if (n <= 0) {
            break;
        }
        sent += (size_t)n;
    }
    tx_raw(false);
#else
    if (s_uart_ready) {
        fflush(stdout);                   /* keep the log out of the packet */
        uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, buf, len);
    }
#endif
}

static void send_packet(uint8_t type, const uint8_t *data, size_t len)
{
    uint8_t pkt[9 + MAX_PAYLOAD + 1];
    if (len > MAX_PAYLOAD) {
        return;
    }
    memcpy(pkt, MAGIC, sizeof(MAGIC));
    pkt[6] = IMPROV_VERSION;
    pkt[7] = type;
    pkt[8] = (uint8_t)len;
    memcpy(pkt + 9, data, len);

    uint32_t sum = 0;
    for (size_t i = 0; i < 9 + len; i++) {
        sum += pkt[i];
    }
    pkt[9 + len] = (uint8_t)sum;

    write_raw(pkt, 9 + len + 1);
}

static void send_state(uint8_t state)
{
    send_packet(TYPE_CURRENT_STATE, &state, 1);
}

static void send_error(uint8_t err)
{
    send_packet(TYPE_ERROR_STATE, &err, 1);
}

/* An RPC result is a command byte, a payload length, then length-prefixed
 * strings. */
static void send_result(uint8_t command, const char *const *strings, size_t n)
{
    uint8_t body[MAX_PAYLOAD];
    size_t pos = 2;
    for (size_t i = 0; i < n; i++) {
        size_t sl = strlen(strings[i]);
        if (sl > 255 || pos + 1 + sl > sizeof(body)) {
            ESP_LOGW(TAG, "result for command 0x%02x does not fit", command);
            return;
        }
        body[pos++] = (uint8_t)sl;
        memcpy(body + pos, strings[i], sl);
        pos += sl;
    }
    body[0] = command;
    body[1] = (uint8_t)(pos - 2);
    send_packet(TYPE_RPC_RESULT, body, pos);
}

/* ------------------------------------------------------------------ */
/* Commands                                                           */
/* ------------------------------------------------------------------ */

/* Where the console can be reached, which the browser shows as the next step.
 *
 * The IP is only right while the uplink is actually up, and this device spends
 * most of its time deliberately disconnected -- patrol and uplink alternate,
 * because one radio cannot both hop channels and stay associated.  So the
 * mDNS name is the honest answer whenever it is not connected this instant. */
static void send_provisioned_result(uint8_t command)
{
    char url[80];
    if (observore_wifi_uplink_connected()) {
        snprintf(url, sizeof(url), "http://%s/", observore_wifi_uplink_ip());
    } else {
        /* observore_wifi_hostname() already carries the .local suffix; appending
         * another produced http://observore.local.local/, which resolves to
         * nothing. */
        snprintf(url, sizeof(url), "http://%s/", observore_wifi_hostname());
    }
    const char *strings[] = {url};
    send_result(command, strings, 1);
}

/* "Provisioned" is about whether credentials are stored, not whether the radio
 * happens to be associated right now.  Keying it off the live connection would
 * make the reported state flap between ready and provisioned every time the
 * patrol/uplink cycle turns over, which would be both wrong and baffling. */
static bool provisioned(void)
{
    return s_provisioned || observore_netcfg_is_set();
}

static void handle_device_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const char *strings[] = {
        "Observore",
        app->version,
        CONFIG_IDF_TARGET,
        observore_wifi_hostname(),
    };
    send_result(CMD_GET_DEVICE_INFO, strings, 4);
}

/* The browser offers a list to pick from, and falls back to typing an SSID
 * when the list is empty.  These come from the patrol sweep that is already
 * running, so asking costs no radio time -- which matters, because a scan
 * started here would fight the sweep for the one radio this chip has. */
static void handle_networks(void)
{
    observore_scan_entry_t seen[OBSERVORE_SCAN_REPORT_MAX];
    size_t n = observore_wifi_last_scan(seen, OBSERVORE_SCAN_REPORT_MAX);

    /* Wait a little rather than answering "none" to a device that has only
     * just booted.
     *
     * This is the normal case, not an edge case: provisioning happens straight
     * after flashing, the browser resets the board as part of that, and the
     * dialog then asks for networks within seconds of boot. Reported from a
     * XIAO C5 as an empty Wi-Fi screen, which is a dead end for somebody
     * standing in front of the board with no other way in.
     *
     * The list fills from sniffed beacons as the patrol sweep runs, so waiting
     * costs nothing but time and needs no radio call from this task -- which
     * matters, because every radio operation in this firmware belongs to the
     * main loop and reaching in from here is how state machines get corrupted.
     */
    if (n == 0) {
        ESP_LOGI(TAG, "no networks known yet; listening for beacons");
        for (int waited = 0; waited < NETWORK_WAIT_MS && n == 0;
             waited += NETWORK_POLL_MS) {
            vTaskDelay(pdMS_TO_TICKS(NETWORK_POLL_MS));
            n = observore_wifi_last_scan(seen, OBSERVORE_SCAN_REPORT_MAX);
        }
        ESP_LOGI(TAG, "%u network%s to offer after waiting",
                 (unsigned)n, n == 1 ? "" : "s");
    }
    for (size_t i = 0; i < n; i++) {
        char rssi[8];
        snprintf(rssi, sizeof(rssi), "%d", seen[i].rssi);
        const char *strings[] = {seen[i].ssid, rssi, seen[i].secure ? "YES" : "NO"};
        send_result(CMD_GET_NETWORKS, strings, 3);
    }
    send_result(CMD_GET_NETWORKS, NULL, 0);   /* terminator */
}

static void handle_wifi_settings(const uint8_t *data, size_t len)
{
    char ssid[OBSERVORE_SSID_LEN] = {0};
    char pass[OBSERVORE_PASSWORD_LEN] = {0};

    if (len < 1) {
        send_error(ERR_INVALID_RPC);
        return;
    }
    size_t sl = data[0];
    if (1 + sl + 1 > len || sl >= sizeof(ssid)) {
        send_error(ERR_INVALID_RPC);
        return;
    }
    memcpy(ssid, data + 1, sl);

    size_t pl = data[1 + sl];
    if (1 + sl + 1 + pl > len || pl >= sizeof(pass)) {
        send_error(ERR_INVALID_RPC);
        return;
    }
    memcpy(pass, data + 1 + sl + 1, pl);

    const char *why = NULL;
    if (!observore_netcfg_valid(ssid, pass, &why)) {
        ESP_LOGW(TAG, "rejected credentials: %s", why ? why : "invalid");
        send_error(ERR_INVALID_RPC);
        return;
    }

    send_state(STATE_PROVISIONING);
    ESP_LOGI(TAG, "provisioning over serial: joining \"%s\"", ssid);

    /* Keep whatever was configured before.  Joining requires the credentials
     * to be stored first -- that is where the station reads them from -- so a
     * mistyped password would otherwise replace a working network with a
     * broken one and take the device off the air until somebody provisions it
     * again.  A failed attempt should cost nothing. */
    observore_netcfg_t previous;
    bool had_previous = observore_netcfg_get(&previous);

    if (observore_netcfg_set(ssid, pass) != ESP_OK) {
        memset(pass, 0, sizeof(pass));
        memset(&previous, 0, sizeof(previous));
        send_error(ERR_UNKNOWN);
        send_state(STATE_READY);
        return;
    }
    memset(pass, 0, sizeof(pass));

    /* Drop the existing association first.  Asking for uplink while already
     * on uplink is a no-op that returns success, so a device provisioned while
     * connected would report the *old* link as proof the *new* credentials
     * work -- accepting a wrong password without ever trying it.  Observed:
     * the same request failed correctly mid-patrol and passed while
     * associated, purely on which half of the cycle it landed in.
     *
     * This project has been caught by that guard once before, when a fallback
     * to patrol silently did nothing. */
    if (observore_wifi_mode() == OBSERVORE_MODE_UPLINK) {
        observore_wifi_set_mode(OBSERVORE_MODE_PATROL);
    }

    /* The uplink is what proves the credentials, so report the result of
     * actually joining rather than the fact that we stored something. */
    if (observore_wifi_set_mode(OBSERVORE_MODE_UPLINK) != ESP_OK ||
        !observore_wifi_uplink_connected()) {
        ESP_LOGW(TAG, "could not join \"%s\": %s", ssid,
                 observore_wifi_uplink_error());
        if (had_previous) {
            observore_netcfg_set(previous.ssid, previous.password);
            ESP_LOGI(TAG, "restored the previous network \"%s\"", previous.ssid);
        } else {
            observore_netcfg_clear();
        }
        memset(&previous, 0, sizeof(previous));
        send_error(ERR_CANNOT_CONNECT);
        send_state(provisioned() ? STATE_PROVISIONED : STATE_READY);
        return;
    }
    memset(&previous, 0, sizeof(previous));

    s_provisioned = true;
    ESP_LOGI(TAG, "provisioned: console at http://%s/", observore_wifi_uplink_ip());
    send_provisioned_result(CMD_WIFI_SETTINGS);
    send_state(STATE_PROVISIONED);
}

static void handle_rpc(const uint8_t *body, size_t len)
{
    if (len < 2) {
        send_error(ERR_INVALID_RPC);
        return;
    }
    uint8_t command = body[0];
    uint8_t dlen = body[1];
    if ((size_t)dlen + 2 > len) {
        send_error(ERR_INVALID_RPC);
        return;
    }
    const uint8_t *data = body + 2;

    switch (command) {
        case CMD_WIFI_SETTINGS:
            handle_wifi_settings(data, dlen);
            break;
        case CMD_GET_STATE:
            send_state(provisioned() ? STATE_PROVISIONED : STATE_READY);
            if (provisioned()) {
                send_provisioned_result(CMD_GET_STATE);
            }
            break;
        case CMD_GET_DEVICE_INFO:
            handle_device_info();
            break;
        case CMD_GET_NETWORKS:
            handle_networks();
            break;
        default:
            send_error(ERR_UNKNOWN_RPC);
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Receive                                                            */
/* ------------------------------------------------------------------ */

/* Packets arrive interleaved with ordinary log output, so the parser hunts for
 * the magic rather than assuming a packet starts where the last one ended.
 *
 * One parser per transport: the two ports are independent byte streams, and
 * feeding both into a single state machine would let a log line on one port
 * corrupt a packet arriving on the other. */
typedef struct {
    enum { WANT_MAGIC, WANT_VERSION, WANT_TYPE, WANT_LEN, WANT_DATA, WANT_SUM } st;
    size_t   magic_seen, got, want;
    uint8_t  type;
    uint8_t  buf[MAX_PAYLOAD];
    uint32_t sum;
} improv_parser_t;

static void feed(improv_parser_t *p, uint8_t b, improv_via_t via)
{
    switch (p->st) {
        case WANT_MAGIC:
            if (b == (uint8_t)MAGIC[p->magic_seen]) {
                p->magic_seen++;
                if (p->magic_seen == sizeof(MAGIC)) {
                    p->magic_seen = 0;
                    p->sum = 0;
                    for (size_t i = 0; i < sizeof(MAGIC); i++) {
                        p->sum += (uint8_t)MAGIC[i];
                    }
                    p->st = WANT_VERSION;
                }
            } else {
                /* Re-test this byte as a fresh start, so "IIMPROV" still works. */
                p->magic_seen = (b == (uint8_t)MAGIC[0]) ? 1 : 0;
            }
            return;

        case WANT_VERSION:
            p->sum += b;
            p->st = (b == IMPROV_VERSION) ? WANT_TYPE : WANT_MAGIC;
            return;

        case WANT_TYPE:
            p->sum += b;
            p->type = b;
            p->st = WANT_LEN;
            return;

        case WANT_LEN:
            p->sum += b;
            p->want = b;
            p->got = 0;
            if (p->want > sizeof(p->buf)) {
                p->st = WANT_MAGIC;        /* not ours, or corrupt */
                return;
            }
            p->st = p->want ? WANT_DATA : WANT_SUM;
            return;

        case WANT_DATA:
            p->sum += b;
            p->buf[p->got++] = b;
            if (p->got == p->want) {
                p->st = WANT_SUM;
            }
            return;

        case WANT_SUM:
            p->st = WANT_MAGIC;
            if ((uint8_t)p->sum != b) {
                ESP_LOGD(TAG, "checksum mismatch, ignoring packet");
                return;
            }
            /* Only a client sends RPC. Ignoring everything else keeps two
             * devices sharing a bus from answering each other for ever. */
            if (p->type == TYPE_RPC) {
                s_via = via;
                handle_rpc(p->buf, p->want);
            }
            return;
    }
}

static void improv_task(void *arg)
{
    (void)arg;
    improv_parser_t console = {0};
    uint8_t in[64];

#if IMPROV_SECOND_TRANSPORT
    improv_parser_t usb = {0};
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = 256,
        .tx_buffer_size = 256,
    };
    bool have_usb = usb_serial_jtag_driver_install(&cfg) == ESP_OK;
    if (!have_usb) {
        ESP_LOGW(TAG, "no USB serial transport; provisioning works on the UART "
                      "port only");
    } else {
        /* Once the driver owns the peripheral, everything has to go through
         * it -- including the secondary console's log output, which until now
         * kept writing by polling the FIFO directly. Two writers with two ideas
         * of the FIFO's state coexist until the host drops and re-raises DTR,
         * which is exactly what a browser does when it opens the port; after
         * that the port went silent for the rest of the boot, and a device
         * that cannot answer the Improv handshake gets no Wi-Fi dialog and is
         * treated as a new install. Found by opening the port with pyserial
         * and watching the log stop. */
        usb_serial_jtag_vfs_use_driver();
    }
#endif

    transport_init();
    /* Announce unprompted at startup: the specification has the device do this
     * so a client that attaches mid-boot does not have to ask. */
    send_state(provisioned() ? STATE_PROVISIONED : STATE_READY);

    for (;;) {
        bool idle = true;

        /* read() on the descriptor, not fgetc(): the console VFS is
         * non-blocking, and stdio latches its EOF indicator the first time a
         * read comes back empty -- after which every later fgetc() returns EOF
         * without looking at the port again, and the device goes deaf. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        /* read() on the descriptor, not fgetc(): stdio latches its EOF
         * indicator the first time a non-blocking read comes back empty, and
         * every later call then returns EOF without looking at the port --
         * the device goes deaf while appearing perfectly healthy. */
        ssize_t n = read(STDIN_FILENO, in, sizeof(in));
#else
        int n = s_uart_ready
                    ? uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, in, sizeof(in), 0)
                    : 0;
#endif
        if (n > 0) {
            idle = false;
            for (int i = 0; i < (int)n; i++) {
                feed(&console, in[i], VIA_CONSOLE);
            }
        }

#if IMPROV_SECOND_TRANSPORT
        if (have_usb) {
            int m = usb_serial_jtag_read_bytes(in, sizeof(in), 0);
            if (m > 0) {
                idle = false;
                for (int i = 0; i < m; i++) {
                    feed(&usb, in[i], VIA_USB_JTAG);
                }
            }
        }
#endif

        if (idle) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

void observore_improv_init(void)
{
    xTaskCreate(improv_task, "observore_improv", 4096, NULL, 4, NULL);
}

bool observore_improv_provisioned(void)
{
    return s_provisioned;
}
