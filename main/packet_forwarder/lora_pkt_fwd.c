/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2019 Semtech

Description:
    Configure Lora concentrator and forward packets to a server
    Use GPS for packet timestamping.
    Send a becon at a regular interval without server intervention

License: Revised BSD License, see LICENSE.TXT file include in the project
*/


/* fix an issue between POSIX and C99 */
#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif

#include <stdint.h>         /* C99 types */
#include <stdbool.h>        /* bool type */
#include <stdio.h>          /* printf, fprintf, snprintf, fopen, fputs */
#include <inttypes.h>       /* PRIx64, PRIu64... */

#include <string.h>         /* memset */
#include <signal.h>         /* sigaction */
#include <time.h>           /* time, clock_gettime, strftime, gmtime */
#include <sys/time.h>       /* timeval */
#include <unistd.h>         /* getopt, access */
#include <stdlib.h>         /* atoi, exit */
#include <errno.h>          /* error messages */
#include <math.h>           /* modf */

#include <sys/socket.h>     /* socket specific definitions */
#include <netinet/in.h>     /* INET constants and stuff */
#include <arpa/inet.h>      /* IP address conversion stuff */
#include <netdb.h>          /* gai_strerror */

#include <pthread.h>

#include "trace.h"
#include "jitqueue.h"
#include "parson.h"
#include "base64.h"
#include "loragw_hal.h"
#include "loragw_aux.h"
#include "loragw_reg.h"
#include "loragw_gps.h"
#include "loragw_gpio.h"

/// For ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include <lwip/netdb.h>
#include "esp_sntp.h"

#include "driver/gpio.h"

#include "http_server.h"
#include "led_indication.h"
// #include "ioe.h"
#include "web_config.h"
#include "loragw_version.h"

#include "esp_netif.h"
#include "esp_eth.h"
#include "ethernet_init.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define ENABLE_MODEM
#define ENABLE_ETH


#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define STRINGIFY(x)    #x
#define STR(x)          STRINGIFY(x)

#define RAND_RANGE(min, max) (rand() % (max + 1 - min) + min)

#ifndef VERSION_STRING
#define VERSION_STRING "undefined"
#endif

#define JSON_CONF_DEFAULT   "global_conf.json"

#define DEFAULT_SERVER      127.0.0.1   /* hostname also supported */
#define DEFAULT_PORT_UP     1780
#define DEFAULT_PORT_DW     1782
#define DEFAULT_KEEPALIVE   5           /* default time interval for downstream keep-alive packet */
#define DEFAULT_STAT        30          /* default time interval for statistics */
#define PUSH_TIMEOUT_MS     100
#define PULL_TIMEOUT_MS     200
#define GPS_REF_MAX_AGE     30          /* maximum admitted delay in seconds of GPS loss before considering latest GPS sync unusable */
#define FETCH_SLEEP_MS      10          /* nb of ms waited when a fetch return no packets */
#define BEACON_POLL_MS      50          /* time in ms between polling of beacon TX status */

#define PROTOCOL_VERSION    2           /* v1.6 */
#define PROTOCOL_JSON_RXPK_FRAME_FORMAT 1

#define XERR_INIT_AVG       16          /* nb of measurements the XTAL correction is averaged on as initial value */
#define XERR_FILT_COEF      256         /* coefficient for low-pass XTAL error tracking */

#define PKT_PUSH_DATA   0
#define PKT_PUSH_ACK    1
#define PKT_PULL_DATA   2
#define PKT_PULL_RESP   3
#define PKT_PULL_ACK    4
#define PKT_TX_ACK      5

#define NB_PKT_MAX      24 /* max number of packets per fetch/send cycle */

#define MIN_LORA_PREAMB 6 /* minimum Lora preamble length for this application */
#define STD_LORA_PREAMB 8
#define MIN_FSK_PREAMB  3 /* minimum FSK preamble length for this application */
#define STD_FSK_PREAMB  5

#define STATUS_SIZE     200
#define TX_BUFF_SIZE    ((540 * NB_PKT_MAX) + 30 + STATUS_SIZE)
#define ACK_BUFF_SIZE   64

#define UNIX_GPS_EPOCH_OFFSET 315964800 /* Number of seconds ellapsed between 01.Jan.1970 00:00:00
                                                                          and 06.Jan.1980 00:00:00 */

#define DEFAULT_BEACON_FREQ_HZ      869525000
#define DEFAULT_BEACON_FREQ_NB      1
#define DEFAULT_BEACON_FREQ_STEP    0
#define DEFAULT_BEACON_DATARATE     9
#define DEFAULT_BEACON_BW_HZ        125000
#define DEFAULT_BEACON_POWER        14
#define DEFAULT_BEACON_INFODESC     0


/* for buttons on the bottom board */

/* for wifi configures */
#define ESP_WIFI_SSID      "LilyGo-Gateway"
#define ESP_WIFI_PASS      "12345678"
#define ESP_WIFI_CHANNEL   1
#define MAX_STA_CONN       4

#define WIFI_MAXIMUM_RETRY  5

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1





/* for display info on screen */
// #ifndef BLINK_GPIO
// #define BLINK_GPIO      38
// #endif

#define TIME_REFRESH    5  // display the time on screen every 5s
#define N_CHAR_A_ROW    21  // max chars in a row is 21 in oled display mode=1

#define IP_LEN  32  // a right ip address should be no more than 16 bytes. some extra space for failed solving


/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

extern config_s config[CONFIG_NUM];
bool wifi_ready = false;
char wifi_ssid[32];
char wifi_pswd[64];
char udp_host[64];
char udp_port_str[16];
char self_ip[16] = "(unknown)";
char gw_id[32];
char freq1[64];
char freq2[64];
char region[16];

uint32_t udp_port;
uint32_t time_count = 0;  // used for display time on screen
static uint8_t wifi_mode = false;    //Configuration mode flags

static const char *BOOT_TAG = "gateway boot";
static const char *PKT_TAG = "packet-forward";
static const char *WIFI_TAG = "wifi station";
static const char *ETH_TAG = "eth";
static const char *SD_TAG = "sd";

/* spectral scan */
typedef struct spectral_scan_s {
    bool enable;            /* enable spectral scan thread */
    uint32_t freq_hz_start; /* first channel frequency, in Hz */
    uint8_t nb_chan;        /* number of channels to scan (200kHz between each channel) */
    uint16_t nb_scan;       /* number of scan points for each frequency scan */
    uint32_t pace_s;        /* number of seconds between 2 scans in the thread */
} spectral_scan_t;


/* signal handling variables */
volatile bool exit_sig = false; /* 1 -> application terminates cleanly (shut down hardware, close open files, etc) */
volatile bool quit_sig = false; /* 1 -> application terminates without shutting down the hardware */

/* packets filtering configuration variables */
static bool fwd_valid_pkt = true; /* packets with PAYLOAD CRC OK are forwarded */
static bool fwd_error_pkt = false; /* packets with PAYLOAD CRC ERROR are NOT forwarded */
static bool fwd_nocrc_pkt = false; /* packets with NO PAYLOAD CRC are NOT forwarded */

/* network configuration variables */
static uint64_t lgwm = 0; /* Lora gateway MAC address */
static char serv_addr[64] = STR(DEFAULT_SERVER); /* address of the server (host name or IPv4/IPv6) */
static char serv_port_up[8] = STR(DEFAULT_PORT_UP); /* server port for upstream traffic */
static char serv_port_down[8] = STR(DEFAULT_PORT_DW); /* server port for downstream traffic */
static int keepalive_time = DEFAULT_KEEPALIVE; /* send a PULL_DATA request every X seconds, negative = disabled */

/* statistics collection configuration variables */
static unsigned stat_interval = DEFAULT_STAT; /* time interval (in sec) at which statistics are collected and displayed */

/* gateway <-> MAC protocol variables */
static uint32_t net_mac_h; /* Most Significant Nibble, network order */
static uint32_t net_mac_l; /* Least Significant Nibble, network order */

/* network sockets */
static int sock_up; /* socket for upstream traffic */
static int sock_down; /* socket for downstream traffic */
struct sockaddr_in dest_addr;
struct sockaddr_in source_addr;

/* network protocol variables */
static struct timeval push_timeout_half = {0, (PUSH_TIMEOUT_MS * 500)}; /* cut in half, critical for throughput */
static struct timeval pull_timeout = {0, (PULL_TIMEOUT_MS * 1000)}; /* non critical for throughput */

/* hardware access control and correction */
SemaphoreHandle_t mx_concent; /* control access to the concentrator */
static SemaphoreHandle_t mx_xcorr; /* control access to the XTAL correction */
static bool xtal_correct_ok = false; /* set true when XTAL correction is stable enough */
static double xtal_correct = 1.0;

/* GPS configuration and synchronization */
static char gps_tty_path[64] = "\0"; /* path of the TTY port GPS is connected on */
static int gps_tty_fd = -1; /* file descriptor of the GPS TTY port */
static bool gps_enabled = false; /* is GPS enabled on that gateway ? */
/* GPS time reference */
static SemaphoreHandle_t mx_timeref; /* control access to GPS time reference */
static bool gps_ref_valid; /* is GPS reference acceptable (ie. not too old) */
static struct tref time_reference_gps; /* time reference used for GPS <-> timestamp conversion */

/* Reference coordinates, for broadcasting (beacon) */
static struct coord_s reference_coord;

/* Enable faking the GPS coordinates of the gateway */
static bool gps_fake_enable; /* enable the feature */

/* measurements to establish statistics */
static SemaphoreHandle_t mx_meas_up; /* control access to the upstream measurements */
static uint32_t meas_nb_rx_rcv = 0; /* count packets received */
static uint32_t meas_nb_rx_ok = 0; /* count packets received with PAYLOAD CRC OK */
static uint32_t meas_nb_rx_bad = 0; /* count packets received with PAYLOAD CRC ERROR */
static uint32_t meas_nb_rx_nocrc = 0; /* count packets received with NO PAYLOAD CRC */
static uint32_t meas_up_pkt_fwd = 0; /* number of radio packet forwarded to the server */
static uint32_t meas_up_network_byte = 0; /* sum of UDP bytes sent for upstream traffic */
static uint32_t meas_up_payload_byte = 0; /* sum of radio payload bytes sent for upstream traffic */
static uint32_t meas_up_dgram_sent = 0; /* number of datagrams sent for upstream traffic */
static uint32_t meas_up_ack_rcv = 0; /* number of datagrams acknowledged for upstream traffic */

static SemaphoreHandle_t mx_meas_dw; /* control access to the downstream measurements */
static uint32_t meas_dw_pull_sent = 0; /* number of PULL requests sent for downstream traffic */
static uint32_t meas_dw_ack_rcv = 0; /* number of PULL requests acknowledged for downstream traffic */
static uint32_t meas_dw_dgram_rcv = 0; /* count PULL response packets received for downstream traffic */
static uint32_t meas_dw_network_byte = 0; /* sum of UDP bytes sent for upstream traffic */
static uint32_t meas_dw_payload_byte = 0; /* sum of radio payload bytes sent for upstream traffic */
static uint32_t meas_nb_tx_ok = 0; /* count packets emitted successfully */
static uint32_t meas_nb_tx_fail = 0; /* count packets were TX failed for other reasons */
static uint32_t meas_nb_tx_requested = 0; /* count TX request from server (downlinks) */
static uint32_t meas_nb_tx_rejected_collision_packet = 0; /* count packets were TX request were rejected due to collision with another packet already programmed */
static uint32_t meas_nb_tx_rejected_collision_beacon = 0; /* count packets were TX request were rejected due to collision with a beacon already programmed */
static uint32_t meas_nb_tx_rejected_too_late = 0; /* count packets were TX request were rejected because it is too late to program it */
static uint32_t meas_nb_tx_rejected_too_early = 0; /* count packets were TX request were rejected because timestamp is too much in advance */
static uint32_t meas_nb_beacon_queued = 0; /* count beacon inserted in jit queue */
static uint32_t meas_nb_beacon_sent = 0; /* count beacon actually sent to concentrator */
static uint32_t meas_nb_beacon_rejected = 0; /* count beacon rejected for queuing */

static SemaphoreHandle_t mx_meas_gps; /* control access to the GPS statistics */
static bool gps_coord_valid; /* could we get valid GPS coordinates ? */
static struct coord_s meas_gps_coord; /* GPS position of the gateway */
static struct coord_s meas_gps_err; /* GPS position of the gateway */

static SemaphoreHandle_t mx_stat_rep; /* control access to the status report */
static bool report_ready = false; /* true when there is a new report to send to the server */
static char status_report[STATUS_SIZE]; /* status report as a JSON object */

/* beacon parameters */
static uint32_t beacon_period = 0; /* set beaconing period, must be a sub-multiple of 86400, the nb of sec in a day */
static uint32_t beacon_freq_hz = DEFAULT_BEACON_FREQ_HZ; /* set beacon TX frequency, in Hz */
static uint8_t beacon_freq_nb = DEFAULT_BEACON_FREQ_NB; /* set number of beaconing channels beacon */
static uint32_t beacon_freq_step = DEFAULT_BEACON_FREQ_STEP; /* set frequency step between beacon channels, in Hz */
static uint8_t beacon_datarate = DEFAULT_BEACON_DATARATE; /* set beacon datarate (SF) */
static uint32_t beacon_bw_hz = DEFAULT_BEACON_BW_HZ; /* set beacon bandwidth, in Hz */
static int8_t beacon_power = DEFAULT_BEACON_POWER; /* set beacon TX power, in dBm */
static uint8_t beacon_infodesc = DEFAULT_BEACON_INFODESC; /* set beacon information descriptor */

/* auto-quit function */
static uint32_t autoquit_threshold = 0; /* enable auto-quit after a number of non-acknowledged PULL_DATA (0 = disabled)*/

/* Just In Time TX scheduling */
static struct jit_queue_s jit_queue[LGW_RF_CHAIN_NB];

/* Gateway specificities */
static int8_t antenna_gain = 0;

/* TX capabilities */
static struct lgw_tx_gain_lut_s txlut[LGW_RF_CHAIN_NB]; /* TX gain table */
static uint32_t tx_freq_min[LGW_RF_CHAIN_NB]; /* lowest frequency supported by TX chain */
static uint32_t tx_freq_max[LGW_RF_CHAIN_NB]; /* highest frequency supported by TX chain */
static bool tx_enable[LGW_RF_CHAIN_NB] = {false}; /* Is TX enabled for a given RF chain ? */

static uint32_t nb_pkt_log[LGW_IF_CHAIN_NB][8]; /* [CH][SF] */
static uint32_t nb_pkt_received_lora = 0;
static uint32_t nb_pkt_received_fsk = 0;

static struct lgw_conf_debug_s debugconf;
static uint32_t nb_pkt_received_ref[16];

/* Interface type */
static lgw_com_type_t com_type = LGW_COM_SPI;

/* Spectral Scan */
static spectral_scan_t spectral_scan_params = {
    .enable = false,
    .freq_hz_start = 0,
    .nb_chan = 0,
    .nb_scan = 0,
    .pace_s = 10
};

TaskHandle_t pJit;
TaskHandle_t pThreadUp;
TaskHandle_t pThreadDn;
TaskHandle_t pHttpServer;
TaskHandle_t pLed;
TaskHandle_t pkt_fwd_handle;
TaskHandle_t pGps;


//static void sig_handler(int sigio);

static int parse_SX130x_configuration(const char *conf_array);

static int parse_gateway_configuration(const char *conf_array);

static int parse_debug_configuration(const char *conf_array);

static uint16_t crc16(const uint8_t *data, unsigned size);

static double difftimespec(struct timespec end, struct timespec beginning);

static void gps_process_sync(void);

static void gps_process_coords(void);

static int get_tx_gain_lut_index(uint8_t rf_chain, int8_t rf_power, uint8_t *lut_index);

/* threads */
void thread_up(void);
void thread_down(void);
void thread_jit(void);
void thread_gps(void);
void thread_valid(void);
void thread_spectral_scan(void);


/*
static void sig_handler(int sigio) {
    if (sigio == SIGQUIT) {
        quit_sig = true;
    } else if ((sigio == SIGINT) || (sigio == SIGTERM)) {
        exit_sig = true;
    }
    return;
}
*/

// void Init_Led( void )
// {
//     esp_rom_gpio_pad_select_gpio( LED_BLUE_GPIO );
//     esp_rom_gpio_pad_select_gpio( LED_GREEN_GPIO );
//     esp_rom_gpio_pad_select_gpio( LED_RED_GPIO );
//     gpio_set_direction( LED_BLUE_GPIO, GPIO_MODE_OUTPUT );
//     gpio_set_direction( LED_GREEN_GPIO, GPIO_MODE_OUTPUT );
//     gpio_set_direction( LED_RED_GPIO, GPIO_MODE_OUTPUT );
//     gpio_set_level( LED_BLUE_GPIO, 1 );   // Default state
//     gpio_set_level( LED_GREEN_GPIO, 1 );   // Default state
//     gpio_set_level( LED_RED_GPIO, 1 );   // Default state
// }

static int parse_SX130x_configuration(const char *conf_array)
{
    int i, j, number;
    char param_name[40]; /* used to generate variable parameter names */
    const char *str; /* used to store string value from JSON object */
    const char conf_obj_name[] = "SX130x_conf";
    JSON_Value *root_val = NULL;
    JSON_Value *val = NULL;
    JSON_Object *conf_obj = NULL;
    JSON_Object *conf_txgain_obj;
    JSON_Object *conf_ts_obj;
    JSON_Object *conf_sx1261_obj = NULL;
    JSON_Object *conf_scan_obj = NULL;
    JSON_Object *conf_lbt_obj = NULL;
    JSON_Object *conf_lbtchan_obj = NULL;
    JSON_Array *conf_txlut_array = NULL;
    JSON_Array *conf_lbtchan_array = NULL;
    JSON_Array *conf_demod_array = NULL;

    struct lgw_conf_board_s boardconf;
    struct lgw_conf_rxrf_s rfconf;
    struct lgw_conf_rxif_s ifconf;
    struct lgw_conf_demod_s demodconf;
    struct lgw_conf_ftime_s tsconf;
    struct lgw_conf_sx1261_s sx1261conf;
    uint32_t sf, bw, fdev;
    bool sx1250_tx_lut;
    size_t size;

    /* try to parse JSON */
    root_val = json_parse_array_with_comments(conf_array);
    if (root_val == NULL) {
        MSG("ERROR: conf array is not a valid JSON string\n");
        exit(EXIT_FAILURE);
    }

    /* point to the gateway configuration object */
    conf_obj = json_object_get_object(json_value_get_object(root_val), conf_obj_name);
    if (conf_obj == NULL) {
        MSG("INFO: conf array does not contain a JSON object named %s\n", conf_obj_name);
        return -1;
    } else {
        MSG("INFO: conf array does contain a JSON object named %s, parsing SX1302 parameters\n", conf_obj_name);
    }

    /* set board configuration */
    memset(&boardconf, 0, sizeof boardconf); /* initialize configuration structure */
    str = json_object_get_string(conf_obj, "com_type");
    if (str == NULL) {
        MSG("ERROR: com_type must be configured in conf array\n");
        return -1;
    } else if (!strncmp(str, "SPI", 3) || !strncmp(str, "spi", 3)) {
        boardconf.com_type = LGW_COM_SPI;
    } else if (!strncmp(str, "USB", 3) || !strncmp(str, "usb", 3)) {
        boardconf.com_type = LGW_COM_USB;
    } else {
        MSG("ERROR: invalid com type: %s (should be SPI or USB)\n", str);
        return -1;
    }
    com_type = boardconf.com_type;
    str = json_object_get_string(conf_obj, "com_path");
    if (str != NULL) {
        strncpy(boardconf.com_path, str, sizeof boardconf.com_path);
        boardconf.com_path[sizeof boardconf.com_path - 1] = '\0'; /* ensure string termination */
    } else {
        MSG("ERROR: com_path must be configured in conf array\n");
        return -1;
    }
    val = json_object_get_value(conf_obj, "lorawan_public"); /* fetch value (if possible) */
    if (json_value_get_type(val) == JSONBoolean) {
        boardconf.lorawan_public = (bool)json_value_get_boolean(val);
    } else {
        MSG("WARNING: Data type for lorawan_public seems wrong, please check\n");
        boardconf.lorawan_public = false;
    }
    val = json_object_get_value(conf_obj, "clksrc"); /* fetch value (if possible) */
    if (json_value_get_type(val) == JSONNumber) {
        boardconf.clksrc = (uint8_t)json_value_get_number(val);
    } else {
        MSG("WARNING: Data type for clksrc seems wrong, please check\n");
        boardconf.clksrc = 0;
    }
    val = json_object_get_value(conf_obj, "full_duplex"); /* fetch value (if possible) */
    if (json_value_get_type(val) == JSONBoolean) {
        boardconf.full_duplex = (bool)json_value_get_boolean(val);
    } else {
        MSG("WARNING: Data type for full_duplex seems wrong, please check\n");
        boardconf.full_duplex = false;
    }
    MSG("INFO: com_type %s, com_path %s, lorawan_public %d, clksrc %d, full_duplex %d\n",
        (boardconf.com_type == LGW_COM_SPI) ? "SPI" : "USB",
        boardconf.com_path, boardconf.lorawan_public, boardconf.clksrc,
        boardconf.full_duplex);
    /* all parameters parsed, submitting configuration to the HAL */
    if (lgw_board_setconf(&boardconf) != LGW_HAL_SUCCESS) {
        MSG("ERROR: Failed to configure board\n");
        return -1;
    }

    /* set antenna gain configuration */
    val = json_object_get_value(conf_obj, "antenna_gain"); /* fetch value (if possible) */
    if (val != NULL) {
        if (json_value_get_type(val) == JSONNumber) {
            antenna_gain = (int8_t)json_value_get_number(val);
        } else {
            MSG("WARNING: Data type for antenna_gain seems wrong, please check\n");
            antenna_gain = 0;
        }
    }
    MSG("INFO: antenna_gain %d dBi\n", antenna_gain);

    /* set timestamp configuration */
    conf_ts_obj = json_object_get_object(conf_obj, "fine_timestamp");
    if (conf_ts_obj == NULL) {
        MSG("INFO: conf array does not contain a JSON object for fine timestamp\n");
    } else {
        val = json_object_get_value(conf_ts_obj, "enable"); /* fetch value (if possible) */
        if (json_value_get_type(val) == JSONBoolean) {
            tsconf.enable = (bool)json_value_get_boolean(val);
        } else {
            MSG("WARNING: Data type for fine_timestamp.enable seems wrong, please check\n");
            tsconf.enable = false;
        }
        if (tsconf.enable == true) {
            str = json_object_get_string(conf_ts_obj, "mode");
            if (str == NULL) {
                MSG("ERROR: fine_timestamp.mode must be configured in conf array\n");
                return -1;
            } else if (!strncmp(str, "high_capacity", 13) || !strncmp(str, "HIGH_CAPACITY", 13)) {
                tsconf.mode = LGW_FTIME_MODE_HIGH_CAPACITY;
            } else if (!strncmp(str, "all_sf", 6) || !strncmp(str, "ALL_SF", 6)) {
                tsconf.mode = LGW_FTIME_MODE_ALL_SF;
            } else {
                MSG("ERROR: invalid fine timestamp mode: %s (should be high_capacity or all_sf)\n", str);
                return -1;
            }
            MSG("INFO: Configuring fine timestamp with %s mode\n", str);

            /* all parameters parsed, submitting configuration to the HAL */
            if (lgw_ftime_setconf(&tsconf) != LGW_HAL_SUCCESS) {
                MSG("ERROR: Failed to configure fine timestamp\n");
                return -1;
            }
        } else {
            MSG("INFO: Configuring legacy timestamp\n");
        }
    }

    /* set SX1261 configuration */
    memset(&sx1261conf, 0, sizeof sx1261conf); /* initialize configuration structure */
    conf_sx1261_obj = json_object_get_object(conf_obj, "sx1261_conf"); /* fetch value (if possible) */
    if (conf_sx1261_obj == NULL) {
        MSG("INFO: no configuration for SX1261\n");
    } else {
        /* Global SX1261 configuration */
        str = json_object_get_string(conf_sx1261_obj, "spi_path");
        if (str != NULL) {
            strncpy(sx1261conf.spi_path, str, sizeof sx1261conf.spi_path);
            sx1261conf.spi_path[sizeof sx1261conf.spi_path - 1] = '\0'; /* ensure string termination */
        } else {
            MSG("INFO: SX1261 spi_path is not configured in conf array\n");
        }
        val = json_object_get_value(conf_sx1261_obj, "rssi_offset"); /* fetch value (if possible) */
        if (json_value_get_type(val) == JSONNumber) {
            sx1261conf.rssi_offset = (int8_t)json_value_get_number(val);
        } else {
            MSG("WARNING: Data type for sx1261_conf.rssi_offset seems wrong, please check\n");
            sx1261conf.rssi_offset = 0;
        }

        /* Spectral Scan configuration */
        conf_scan_obj = json_object_get_object(conf_sx1261_obj, "spectral_scan"); /* fetch value (if possible) */
        if (conf_scan_obj == NULL) {
            MSG("INFO: no configuration for Spectral Scan\n");
        } else {
            val = json_object_get_value(conf_scan_obj, "enable"); /* fetch value (if possible) */
            if (json_value_get_type(val) == JSONBoolean) {
                /* Enable background spectral scan thread in packet forwarder */
                spectral_scan_params.enable = (bool)json_value_get_boolean(val);
            } else {
                MSG("WARNING: Data type for spectral_scan.enable seems wrong, please check\n");
            }
            if (spectral_scan_params.enable == true) {
                /* Enable the sx1261 radio hardware configuration to allow spectral scan */
                sx1261conf.enable = true;
                MSG("INFO: Spectral Scan with SX1261 is enabled\n");

                /* Get Spectral Scan Parameters */
                val = json_object_get_value(conf_scan_obj, "freq_start"); /* fetch value (if possible) */
                if (json_value_get_type(val) == JSONNumber) {
                    spectral_scan_params.freq_hz_start = (uint32_t)json_value_get_number(val);
                } else {
                    MSG("WARNING: Data type for spectral_scan.freq_start seems wrong, please check\n");
                }
                val = json_object_get_value(conf_scan_obj, "nb_chan"); /* fetch value (if possible) */
                if (json_value_get_type(val) == JSONNumber) {
                    spectral_scan_params.nb_chan = (uint8_t)json_value_get_number(val);
                } else {
                    MSG("WARNING: Data type for spectral_scan.nb_chan seems wrong, please check\n");
                }
                val = json_object_get_value(conf_scan_obj, "nb_scan"); /* fetch value (if possible) */
                if (json_value_get_type(val) == JSONNumber) {
                    spectral_scan_params.nb_scan = (uint16_t)json_value_get_number(val);
                } else {
                    MSG("WARNING: Data type for spectral_scan.nb_scan seems wrong, please check\n");
                }
                val = json_object_get_value(conf_scan_obj, "pace_s"); /* fetch value (if possible) */
                if (json_value_get_type(val) == JSONNumber) {
                    spectral_scan_params.pace_s = (uint32_t)json_value_get_number(val);
                } else {
                    MSG("WARNING: Data type for spectral_scan.pace_s seems wrong, please check\n");
                }
            }
        }

        /* LBT configuration */
        conf_lbt_obj = json_object_get_object(conf_sx1261_obj, "lbt"); /* fetch value (if possible) */
        if (conf_lbt_obj == NULL) {
            MSG("INFO: no configuration for LBT\n");
        } else {
            val = json_object_get_value(conf_lbt_obj, "enable"); /* fetch value (if possible) */
            if (json_value_get_type(val) == JSONBoolean) {
                sx1261conf.lbt_conf.enable = (bool)json_value_get_boolean(val);
            } else {
                MSG("WARNING: Data type for lbt.enable seems wrong, please check\n");
            }
            if (sx1261conf.lbt_conf.enable == true) {
                /* Enable the sx1261 radio hardware configuration to allow spectral scan */
                sx1261conf.enable = true;
                MSG("INFO: Listen-Before-Talk with SX1261 is enabled\n");

                val = json_object_get_value(conf_lbt_obj, "rssi_target"); /* fetch value (if possible) */
                if (json_value_get_type(val) == JSONNumber) {
                    sx1261conf.lbt_conf.rssi_target = (int8_t)json_value_get_number(val);
                } else {
                    MSG("WARNING: Data type for lbt.rssi_target seems wrong, please check\n");
                    sx1261conf.lbt_conf.rssi_target = 0;
                }
                /* set LBT channels configuration */
                conf_lbtchan_array = json_object_get_array(conf_lbt_obj, "channels");
                if (conf_lbtchan_array != NULL) {
                    sx1261conf.lbt_conf.nb_channel = json_array_get_count(conf_lbtchan_array);
                    MSG("INFO: %u LBT channels configured\n", sx1261conf.lbt_conf.nb_channel);
                }
                for (i = 0; i < (int)sx1261conf.lbt_conf.nb_channel; i++) {
                    /* Sanity check */
                    if (i >= LGW_LBT_CHANNEL_NB_MAX) {
                        MSG("ERROR: LBT channel %d not supported, skip it\n", i);
                        break;
                    }
                    /* Get LBT channel configuration object from array */
                    conf_lbtchan_obj = json_array_get_object(conf_lbtchan_array, i);

                    /* Channel frequency */
                    val = json_object_dotget_value(conf_lbtchan_obj, "freq_hz"); /* fetch value (if possible) */
                    if (val != NULL) {
                        if (json_value_get_type(val) == JSONNumber) {
                            sx1261conf.lbt_conf.channels[i].freq_hz = (uint32_t)json_value_get_number(val);
                        } else {
                            MSG("WARNING: Data type for lbt.channels[%d].freq_hz seems wrong, please check\n", i);
                            sx1261conf.lbt_conf.channels[i].freq_hz = 0;
                        }
                    } else {
                        MSG("ERROR: no frequency defined for LBT channel %d\n", i);
                        return -1;
                    }

                    /* Channel bandiwdth */
                    val = json_object_dotget_value(conf_lbtchan_obj, "bandwidth"); /* fetch value (if possible) */
                    if (val != NULL) {
                        if (json_value_get_type(val) == JSONNumber) {
                            bw = (uint32_t)json_value_get_number(val);
                            switch (bw) {
                            case 500000: sx1261conf.lbt_conf.channels[i].bandwidth = BW_500KHZ; break;
                            case 250000: sx1261conf.lbt_conf.channels[i].bandwidth = BW_250KHZ; break;
                            case 125000: sx1261conf.lbt_conf.channels[i].bandwidth = BW_125KHZ; break;
                            default: sx1261conf.lbt_conf.channels[i].bandwidth = BW_UNDEFINED;
                            }
                        } else {
                            MSG("WARNING: Data type for lbt.channels[%d].freq_hz seems wrong, please check\n", i);
                            sx1261conf.lbt_conf.channels[i].bandwidth = BW_UNDEFINED;
                        }
                    } else {
                        MSG("ERROR: no bandiwdth defined for LBT channel %d\n", i);
                        return -1;
                    }

                    /* Channel scan time */
                    val = json_object_dotget_value(conf_lbtchan_obj, "scan_time_us"); /* fetch value (if possible) */
                    if (val != NULL) {
                        if (json_value_get_type(val) == JSONNumber) {
                            if ((uint16_t)json_value_get_number(val) == 128) {
                                sx1261conf.lbt_conf.channels[i].scan_time_us = LGW_LBT_SCAN_TIME_128_US;
                            } else if ((uint16_t)json_value_get_number(val) == 5000) {
                                sx1261conf.lbt_conf.channels[i].scan_time_us = LGW_LBT_SCAN_TIME_5000_US;
                            } else {
                                MSG("ERROR: scan time not supported for LBT channel %d, must be 128 or 5000\n", i);
                                return -1;
                            }
                        } else {
                            MSG("WARNING: Data type for lbt.channels[%d].scan_time_us seems wrong, please check\n", i);
                            sx1261conf.lbt_conf.channels[i].scan_time_us = 0;
                        }
                    } else {
                        MSG("ERROR: no scan_time_us defined for LBT channel %d\n", i);
                        return -1;
                    }

                    /* Channel transmit time */
                    val = json_object_dotget_value(conf_lbtchan_obj, "transmit_time_ms"); /* fetch value (if possible) */
                    if (val != NULL) {
                        if (json_value_get_type(val) == JSONNumber) {
                            sx1261conf.lbt_conf.channels[i].transmit_time_ms = (uint16_t)json_value_get_number(val);
                        } else {
                            MSG("WARNING: Data type for lbt.channels[%d].transmit_time_ms seems wrong, please check\n", i);
                            sx1261conf.lbt_conf.channels[i].transmit_time_ms = 0;
                        }
                    } else {
                        MSG("ERROR: no transmit_time_ms defined for LBT channel %d\n", i);
                        return -1;
                    }
                }
            }
        }

        /* all parameters parsed, submitting configuration to the HAL */
        if (lgw_sx1261_setconf(&sx1261conf) != LGW_HAL_SUCCESS) {
            MSG("ERROR: Failed to configure the SX1261 radio\n");
            return -1;
        }
    }

    /* set configuration for RF chains */
    for (i = 0; i < LGW_RF_CHAIN_NB; ++i) {
        memset(&rfconf, 0, sizeof rfconf); /* initialize configuration structure */
        snprintf(param_name, sizeof param_name, "radio_%i", i); /* compose parameter path inside JSON structure */
        val = json_object_get_value(conf_obj, param_name); /* fetch value (if possible) */
        if (json_value_get_type(val) != JSONObject) {
            MSG("INFO: no configuration for radio %i\n", i);
            continue;
        }
        /* there is an object to configure that radio, let's parse it */
        snprintf(param_name, sizeof param_name, "radio_%i.enable", i);
        val = json_object_dotget_value(conf_obj, param_name);
        if (json_value_get_type(val) == JSONBoolean) {
            rfconf.enable = (bool)json_value_get_boolean(val);
        } else {
            rfconf.enable = false;
        }
        if (rfconf.enable == false) { /* radio disabled, nothing else to parse */
            MSG("INFO: radio %i disabled\n", i);
        } else  { /* radio enabled, will parse the other parameters */
            snprintf(param_name, sizeof param_name, "radio_%i.freq", i);
            rfconf.freq_hz = (uint32_t)json_object_dotget_number(conf_obj, param_name);
            snprintf(param_name, sizeof param_name, "radio_%i.rssi_offset", i);
            rfconf.rssi_offset = (float)json_object_dotget_number(conf_obj, param_name);
            snprintf(param_name, sizeof param_name, "radio_%i.rssi_tcomp.coeff_a", i);
            rfconf.rssi_tcomp.coeff_a = (float)json_object_dotget_number(conf_obj, param_name);
            snprintf(param_name, sizeof param_name, "radio_%i.rssi_tcomp.coeff_b", i);
            rfconf.rssi_tcomp.coeff_b = (float)json_object_dotget_number(conf_obj, param_name);
            snprintf(param_name, sizeof param_name, "radio_%i.rssi_tcomp.coeff_c", i);
            rfconf.rssi_tcomp.coeff_c = (float)json_object_dotget_number(conf_obj, param_name);
            snprintf(param_name, sizeof param_name, "radio_%i.rssi_tcomp.coeff_d", i);
            rfconf.rssi_tcomp.coeff_d = (float)json_object_dotget_number(conf_obj, param_name);
            snprintf(param_name, sizeof param_name, "radio_%i.rssi_tcomp.coeff_e", i);
            rfconf.rssi_tcomp.coeff_e = (float)json_object_dotget_number(conf_obj, param_name);
            snprintf(param_name, sizeof param_name, "radio_%i.type", i);
            str = json_object_dotget_string(conf_obj, param_name);
            if (!strncmp(str, "SX1255", 6)) {
                rfconf.type = LGW_RADIO_TYPE_SX1255;
            } else if (!strncmp(str, "SX1257", 6)) {
                rfconf.type = LGW_RADIO_TYPE_SX1257;
            } else if (!strncmp(str, "SX1250", 6)) {
                rfconf.type = LGW_RADIO_TYPE_SX1250;
            } else {
                MSG("WARNING: invalid radio type: %s (should be SX1255 or SX1257 or SX1250)\n", str);
            }
            snprintf(param_name, sizeof param_name, "radio_%i.single_input_mode", i);
            val = json_object_dotget_value(conf_obj, param_name);
            if (json_value_get_type(val) == JSONBoolean) {
                rfconf.single_input_mode = (bool)json_value_get_boolean(val);
            } else {
                rfconf.single_input_mode = false;
            }

            snprintf(param_name, sizeof param_name, "radio_%i.tx_enable", i);
            val = json_object_dotget_value(conf_obj, param_name);
            if (json_value_get_type(val) == JSONBoolean) {
                rfconf.tx_enable = (bool)json_value_get_boolean(val);
                tx_enable[i] = rfconf.tx_enable; /* update global context for later check */
                if (rfconf.tx_enable == true) {
                    /* tx is enabled on this rf chain, we need its frequency range */
                    snprintf(param_name, sizeof param_name, "radio_%i.tx_freq_min", i);
                    tx_freq_min[i] = (uint32_t)json_object_dotget_number(conf_obj, param_name);
                    snprintf(param_name, sizeof param_name, "radio_%i.tx_freq_max", i);
                    tx_freq_max[i] = (uint32_t)json_object_dotget_number(conf_obj, param_name);
                    if ((tx_freq_min[i] == 0) || (tx_freq_max[i] == 0)) {
                        MSG("WARNING: no frequency range specified for TX rf chain %d\n", i);
                    }

                    /* set configuration for tx gains */
                    memset(&txlut[i], 0, sizeof txlut[i]); /* initialize configuration structure */
                    snprintf(param_name, sizeof param_name, "radio_%i.tx_gain_lut", i);
                    conf_txlut_array = json_object_dotget_array(conf_obj, param_name);
                    if (conf_txlut_array != NULL) {
                        txlut[i].size = json_array_get_count(conf_txlut_array);
                        /* Detect if we have a sx125x or sx1250 configuration */
                        conf_txgain_obj = json_array_get_object(conf_txlut_array, 0);
                        val = json_object_dotget_value(conf_txgain_obj, "pwr_idx");
                        if (val != NULL) {
                            printf("INFO: Configuring Tx Gain LUT for rf_chain %d with %u indexes for sx1250\n", i, txlut[i].size);
                            sx1250_tx_lut = true;
                        } else {
                            printf("INFO: Configuring Tx Gain LUT for rf_chain %d with %u indexes for sx125x\n", i, txlut[i].size);
                            sx1250_tx_lut = false;
                        }
                        /* Parse the table */
                        for (j = 0; j < (int)txlut[i].size; j++) {
                            /* Sanity check */
                            if (j >= TX_GAIN_LUT_SIZE_MAX) {
                                printf("ERROR: TX Gain LUT [%d] index %d not supported, skip it\n", i, j);
                                break;
                            }
                            /* Get TX gain object from LUT */
                            conf_txgain_obj = json_array_get_object(conf_txlut_array, j);
                            /* rf power */
                            val = json_object_dotget_value(conf_txgain_obj, "rf_power");
                            if (json_value_get_type(val) == JSONNumber) {
                                txlut[i].lut[j].rf_power = (int8_t)json_value_get_number(val);
                            } else {
                                printf("WARNING: Data type for %s[%d] seems wrong, please check\n", "rf_power", j);
                                txlut[i].lut[j].rf_power = 0;
                            }
                            /* PA gain */
                            val = json_object_dotget_value(conf_txgain_obj, "pa_gain");
                            if (json_value_get_type(val) == JSONNumber) {
                                txlut[i].lut[j].pa_gain = (uint8_t)json_value_get_number(val);
                            } else {
                                printf("WARNING: Data type for %s[%d] seems wrong, please check\n", "pa_gain", j);
                                txlut[i].lut[j].pa_gain = 0;
                            }
                            if (sx1250_tx_lut == false) {
                                /* DIG gain */
                                val = json_object_dotget_value(conf_txgain_obj, "dig_gain");
                                if (json_value_get_type(val) == JSONNumber) {
                                    txlut[i].lut[j].dig_gain = (uint8_t)json_value_get_number(val);
                                } else {
                                    printf("WARNING: Data type for %s[%d] seems wrong, please check\n", "dig_gain", j);
                                    txlut[i].lut[j].dig_gain = 0;
                                }
                                /* DAC gain */
                                val = json_object_dotget_value(conf_txgain_obj, "dac_gain");
                                if (json_value_get_type(val) == JSONNumber) {
                                    txlut[i].lut[j].dac_gain = (uint8_t)json_value_get_number(val);
                                } else {
                                    printf("WARNING: Data type for %s[%d] seems wrong, please check\n", "dac_gain", j);
                                    txlut[i].lut[j].dac_gain = 3; /* This is the only dac_gain supported for now */
                                }
                                /* MIX gain */
                                val = json_object_dotget_value(conf_txgain_obj, "mix_gain");
                                if (json_value_get_type(val) == JSONNumber) {
                                    txlut[i].lut[j].mix_gain = (uint8_t)json_value_get_number(val);
                                } else {
                                    printf("WARNING: Data type for %s[%d] seems wrong, please check\n", "mix_gain", j);
                                    txlut[i].lut[j].mix_gain = 0;
                                }
                            } else {
                                /* TODO: rework this, should not be needed for sx1250 */
                                txlut[i].lut[j].mix_gain = 5;

                                /* power index */
                                val = json_object_dotget_value(conf_txgain_obj, "pwr_idx");
                                if (json_value_get_type(val) == JSONNumber) {
                                    txlut[i].lut[j].pwr_idx = (uint8_t)json_value_get_number(val);
                                } else {
                                    printf("WARNING: Data type for %s[%d] seems wrong, please check\n", "pwr_idx", j);
                                    txlut[i].lut[j].pwr_idx = 0;
                                }
                            }
                        }
                        /* all parameters parsed, submitting configuration to the HAL */
                        if (txlut[i].size > 0) {
                            if (lgw_txgain_setconf(i, &txlut[i]) != LGW_HAL_SUCCESS) {
                                MSG("ERROR: Failed to configure concentrator TX Gain LUT for rf_chain %d\n", i);
                                return -1;
                            }
                        } else {
                            MSG("WARNING: No TX gain LUT defined for rf_chain %d\n", i);
                        }
                    } else {
                        MSG("WARNING: No TX gain LUT defined for rf_chain %d\n", i);
                    }
                }
            } else {
                rfconf.tx_enable = false;
            }
            MSG("INFO: radio %i enabled (type %s), center frequency %lu, RSSI offset %f, tx enabled %d, single input mode %d\n",
                i, str, rfconf.freq_hz, rfconf.rssi_offset, rfconf.tx_enable, rfconf.single_input_mode);
        }
        /* all parameters parsed, submitting configuration to the HAL */
        if (lgw_rxrf_setconf(i, &rfconf) != LGW_HAL_SUCCESS) {
            MSG("ERROR: invalid configuration for radio %i\n", i);
            return -1;
        }
    }

    /* set configuration for demodulators */
    memset(&demodconf, 0, sizeof demodconf); /* initialize configuration structure */
    val = json_object_get_value(conf_obj, "chan_multiSF_All"); /* fetch value (if possible) */
    if (json_value_get_type(val) != JSONObject) {
        MSG("INFO: no configuration for LoRa multi-SF spreading factors enabling\n");
    } else {
        conf_demod_array = json_object_dotget_array(conf_obj, "chan_multiSF_All.spreading_factor_enable");
        if ((conf_demod_array != NULL) && ((size = json_array_get_count(conf_demod_array)) <= LGW_MULTI_NB)) {
            for (i = 0; i < (int)size; i++) {
                number = json_array_get_number(conf_demod_array, i);
                if (number < 5 || number > 12) {
                    MSG("WARNING: failed to parse chan_multiSF_All.spreading_factor_enable (wrong value at idx %d)\n", i);
                    demodconf.multisf_datarate = 0xFF; /* enable all SFs */
                    break;
                } else {
                    /* set corresponding bit in the bitmask SF5 is LSB -> SF12 is MSB */
                    demodconf.multisf_datarate |= (1 << (number - 5));
                }
            }
        } else {
            MSG("WARNING: failed to parse chan_multiSF_All.spreading_factor_enable\n");
            demodconf.multisf_datarate = 0xFF; /* enable all SFs */
        }
        /* all parameters parsed, submitting configuration to the HAL */
        if (lgw_demod_setconf(&demodconf) != LGW_HAL_SUCCESS) {
            MSG("ERROR: invalid configuration for demodulation parameters\n");
            return -1;
        }
    }

    /* set configuration for Lora multi-SF channels (bandwidth cannot be set) */
    for (i = 0; i < LGW_MULTI_NB; ++i) {
        memset(&ifconf, 0, sizeof ifconf); /* initialize configuration structure */
        snprintf(param_name, sizeof param_name, "chan_multiSF_%i", i); /* compose parameter path inside JSON structure */
        val = json_object_get_value(conf_obj, param_name); /* fetch value (if possible) */
        if (json_value_get_type(val) != JSONObject) {
            MSG("INFO: no configuration for Lora multi-SF channel %i\n", i);
            continue;
        }
        /* there is an object to configure that Lora multi-SF channel, let's parse it */
        snprintf(param_name, sizeof param_name, "chan_multiSF_%i.enable", i);
        val = json_object_dotget_value(conf_obj, param_name);
        if (json_value_get_type(val) == JSONBoolean) {
            ifconf.enable = (bool)json_value_get_boolean(val);
        } else {
            ifconf.enable = false;
        }
        if (ifconf.enable == false) { /* Lora multi-SF channel disabled, nothing else to parse */
            MSG("INFO: Lora multi-SF channel %i disabled\n", i);
        } else  { /* Lora multi-SF channel enabled, will parse the other parameters */
            snprintf(param_name, sizeof param_name, "chan_multiSF_%i.radio", i);
            ifconf.rf_chain = (uint32_t)json_object_dotget_number(conf_obj, param_name);
            snprintf(param_name, sizeof param_name, "chan_multiSF_%i.if", i);
            ifconf.freq_hz = (int32_t)json_object_dotget_number(conf_obj, param_name);
            // TODO: handle individual SF enabling and disabling (spread_factor)
            MSG("INFO: Lora multi-SF channel %i>  radio %i, IF %li Hz, 125 kHz bw, SF 5 to 12\n", i, ifconf.rf_chain, ifconf.freq_hz);
        }
        /* all parameters parsed, submitting configuration to the HAL */
        if (lgw_rxif_setconf(i, &ifconf) != LGW_HAL_SUCCESS) {
            MSG("ERROR: invalid configuration for Lora multi-SF channel %i\n", i);
            return -1;
        }
    }

    /* set configuration for Lora standard channel */
    memset(&ifconf, 0, sizeof ifconf); /* initialize configuration structure */
    val = json_object_get_value(conf_obj, "chan_Lora_std"); /* fetch value (if possible) */
    if (json_value_get_type(val) != JSONObject) {
        MSG("INFO: no configuration for Lora standard channel\n");
    } else {
        val = json_object_dotget_value(conf_obj, "chan_Lora_std.enable");
        if (json_value_get_type(val) == JSONBoolean) {
            ifconf.enable = (bool)json_value_get_boolean(val);
        } else {
            ifconf.enable = false;
        }
        if (ifconf.enable == false) {
            MSG("INFO: Lora standard channel %i disabled\n", i);
        } else  {
            ifconf.rf_chain = (uint32_t)json_object_dotget_number(conf_obj, "chan_Lora_std.radio");
            ifconf.freq_hz = (int32_t)json_object_dotget_number(conf_obj, "chan_Lora_std.if");
            bw = (uint32_t)json_object_dotget_number(conf_obj, "chan_Lora_std.bandwidth");
            switch (bw) {
            case 500000: ifconf.bandwidth = BW_500KHZ; break;
            case 250000: ifconf.bandwidth = BW_250KHZ; break;
            case 125000: ifconf.bandwidth = BW_125KHZ; break;
            default: ifconf.bandwidth = BW_UNDEFINED;
            }
            sf = (uint32_t)json_object_dotget_number(conf_obj, "chan_Lora_std.spread_factor");
            switch (sf) {
            case  5: ifconf.datarate = DR_LORA_SF5;  break;
            case  6: ifconf.datarate = DR_LORA_SF6;  break;
            case  7: ifconf.datarate = DR_LORA_SF7;  break;
            case  8: ifconf.datarate = DR_LORA_SF8;  break;
            case  9: ifconf.datarate = DR_LORA_SF9;  break;
            case 10: ifconf.datarate = DR_LORA_SF10; break;
            case 11: ifconf.datarate = DR_LORA_SF11; break;
            case 12: ifconf.datarate = DR_LORA_SF12; break;
            default: ifconf.datarate = DR_UNDEFINED;
            }
            val = json_object_dotget_value(conf_obj, "chan_Lora_std.implicit_hdr");
            if (json_value_get_type(val) == JSONBoolean) {
                ifconf.implicit_hdr = (bool)json_value_get_boolean(val);
            } else {
                ifconf.implicit_hdr = false;
            }
            if (ifconf.implicit_hdr == true) {
                val = json_object_dotget_value(conf_obj, "chan_Lora_std.implicit_payload_length");
                if (json_value_get_type(val) == JSONNumber) {
                    ifconf.implicit_payload_length = (uint8_t)json_value_get_number(val);
                } else {
                    MSG("ERROR: payload length setting is mandatory for implicit header mode\n");
                    return -1;
                }
                val = json_object_dotget_value(conf_obj, "chan_Lora_std.implicit_crc_en");
                if (json_value_get_type(val) == JSONBoolean) {
                    ifconf.implicit_crc_en = (bool)json_value_get_boolean(val);
                } else {
                    MSG("ERROR: CRC enable setting is mandatory for implicit header mode\n");
                    return -1;
                }
                val = json_object_dotget_value(conf_obj, "chan_Lora_std.implicit_coderate");
                if (json_value_get_type(val) == JSONNumber) {
                    ifconf.implicit_coderate = (uint8_t)json_value_get_number(val);
                } else {
                    MSG("ERROR: coding rate setting is mandatory for implicit header mode\n");
                    return -1;
                }
            }

            MSG("INFO: Lora std channel> radio %i, IF %li Hz, %lu Hz bw, SF %lu, %s\n",
                ifconf.rf_chain, ifconf.freq_hz, bw, sf,
                (ifconf.implicit_hdr == true) ? "Implicit header" : "Explicit header");
        }
        if (lgw_rxif_setconf(8, &ifconf) != LGW_HAL_SUCCESS) {
            MSG("ERROR: invalid configuration for Lora standard channel\n");
            return -1;
        }
    }

    /* set configuration for FSK channel */
    memset(&ifconf, 0, sizeof ifconf); /* initialize configuration structure */
    val = json_object_get_value(conf_obj, "chan_FSK"); /* fetch value (if possible) */
    if (json_value_get_type(val) != JSONObject) {
        MSG("INFO: no configuration for FSK channel\n");
    } else {
        val = json_object_dotget_value(conf_obj, "chan_FSK.enable");
        if (json_value_get_type(val) == JSONBoolean) {
            ifconf.enable = (bool)json_value_get_boolean(val);
        } else {
            ifconf.enable = false;
        }
        if (ifconf.enable == false) {
            MSG("INFO: FSK channel %i disabled\n", i);
        } else  {
            ifconf.rf_chain = (uint32_t)json_object_dotget_number(conf_obj, "chan_FSK.radio");
            ifconf.freq_hz = (int32_t)json_object_dotget_number(conf_obj, "chan_FSK.if");
            bw = (uint32_t)json_object_dotget_number(conf_obj, "chan_FSK.bandwidth");
            fdev = (uint32_t)json_object_dotget_number(conf_obj, "chan_FSK.freq_deviation");
            ifconf.datarate = (uint32_t)json_object_dotget_number(conf_obj, "chan_FSK.datarate");

            /* if chan_FSK.bandwidth is set, it has priority over chan_FSK.freq_deviation */
            if ((bw == 0) && (fdev != 0)) {
                bw = 2 * fdev + ifconf.datarate;
            }
            if      (bw == 0)      ifconf.bandwidth = BW_UNDEFINED;
#if 0 /* TODO */
            else if (bw <= 7800)   ifconf.bandwidth = BW_7K8HZ;
            else if (bw <= 15600)  ifconf.bandwidth = BW_15K6HZ;
            else if (bw <= 31200)  ifconf.bandwidth = BW_31K2HZ;
            else if (bw <= 62500)  ifconf.bandwidth = BW_62K5HZ;
#endif
            else if (bw <= 125000) ifconf.bandwidth = BW_125KHZ;
            else if (bw <= 250000) ifconf.bandwidth = BW_250KHZ;
            else if (bw <= 500000) ifconf.bandwidth = BW_500KHZ;
            else ifconf.bandwidth = BW_UNDEFINED;

            MSG("INFO: FSK channel> radio %i, IF %li Hz, %lu Hz bw, %lu bps datarate\n",
                ifconf.rf_chain, ifconf.freq_hz, bw, ifconf.datarate);
        }
        if (lgw_rxif_setconf(9, &ifconf) != LGW_HAL_SUCCESS) {
            MSG("ERROR: invalid configuration for FSK channel\n");
            return -1;
        }
    }
    json_value_free(root_val);

    return 0;
}

static int parse_gateway_configuration(const char *conf_array)
{
    const char conf_obj_name[] = "gateway_conf";
    JSON_Value *root_val;
    JSON_Object *conf_obj = NULL;
    JSON_Value *val = NULL; /* needed to detect the absence of some fields */
    const char *str; /* pointer to sub-strings in the JSON data */
    unsigned long long ull = 0;

    /* try to parse JSON */
    root_val = json_parse_array_with_comments(conf_array);
    if (root_val == NULL) {
        MSG("ERROR: conf array is not a valid JSON string\n");
        exit(EXIT_FAILURE);
    }

    /* point to the gateway configuration object */
    conf_obj = json_object_get_object(json_value_get_object(root_val), conf_obj_name);
    if (conf_obj == NULL) {
        MSG("INFO: conf array does not contain a JSON object named %s\n", conf_obj_name);
        return -1;
    } else {
        MSG("INFO: conf array does contain a JSON object named %s, parsing gateway parameters\n", conf_obj_name);
    }

    /* gateway unique identifier (aka MAC address) (optional) */
    str = json_object_get_string(conf_obj, "gateway_ID");
    if (str != NULL) {
        sscanf(str, "%llx", &ull);
        lgwm = ull;
        MSG("INFO: gateway MAC address is configured to %016llX\n", ull);
    }

    /* server hostname or IP address (optional) */
    str = json_object_get_string(conf_obj, "server_address");
    if (str != NULL) {
        strncpy(serv_addr, str, sizeof serv_addr);
        serv_addr[sizeof serv_addr - 1] = '\0'; /* ensure string termination */
        MSG("INFO: server hostname or IP address is configured to \"%s\"\n", serv_addr);
    }

    /* get up and down ports (optional) */
    val = json_object_get_value(conf_obj, "serv_port_up");
    if (val != NULL) {
        snprintf(serv_port_up, sizeof serv_port_up, "%u", (uint16_t)json_value_get_number(val));
        MSG("INFO: upstream port is configured to \"%s\"\n", serv_port_up);
    }
    val = json_object_get_value(conf_obj, "serv_port_down");
    if (val != NULL) {
        snprintf(serv_port_down, sizeof serv_port_down, "%u", (uint16_t)json_value_get_number(val));
        MSG("INFO: downstream port is configured to \"%s\"\n", serv_port_down);
    }

    /* get keep-alive interval (in seconds) for downstream (optional) */
    val = json_object_get_value(conf_obj, "keepalive_interval");
    if (val != NULL) {
        keepalive_time = (int)json_value_get_number(val);
        MSG("INFO: downstream keep-alive interval is configured to %u seconds\n", keepalive_time);
    }

    /* get interval (in seconds) for statistics display (optional) */
    val = json_object_get_value(conf_obj, "stat_interval");
    if (val != NULL) {
        stat_interval = (unsigned)json_value_get_number(val);
        MSG("INFO: statistics display interval is configured to %u seconds\n", stat_interval);
    }

    /* get time-out value (in ms) for upstream datagrams (optional) */
    val = json_object_get_value(conf_obj, "push_timeout_ms");
    if (val != NULL) {
        push_timeout_half.tv_usec = 500 * (long int)json_value_get_number(val);
        MSG("INFO: upstream PUSH_DATA time-out is configured to %u ms\n", (unsigned)(push_timeout_half.tv_usec / 500));
    }

    /* packet filtering parameters */
    val = json_object_get_value(conf_obj, "forward_crc_valid");
    if (json_value_get_type(val) == JSONBoolean) {
        fwd_valid_pkt = (bool)json_value_get_boolean(val);
    }
    MSG("INFO: packets received with a valid CRC will%s be forwarded\n", (fwd_valid_pkt ? "" : " NOT"));
    val = json_object_get_value(conf_obj, "forward_crc_error");
    if (json_value_get_type(val) == JSONBoolean) {
        fwd_error_pkt = (bool)json_value_get_boolean(val);
    }
    MSG("INFO: packets received with a CRC error will%s be forwarded\n", (fwd_error_pkt ? "" : " NOT"));
    val = json_object_get_value(conf_obj, "forward_crc_disabled");
    if (json_value_get_type(val) == JSONBoolean) {
        fwd_nocrc_pkt = (bool)json_value_get_boolean(val);
    }
    MSG("INFO: packets received with no CRC will%s be forwarded\n", (fwd_nocrc_pkt ? "" : " NOT"));

    /* GPS module TTY path (optional) */
    str = json_object_get_string(conf_obj, "gps_tty_path");
    if (str != NULL) {
        strncpy(gps_tty_path, str, sizeof gps_tty_path);
        gps_tty_path[sizeof gps_tty_path - 1] = '\0'; /* ensure string termination */
        MSG("INFO: GPS serial port path is configured to \"%s\"\n", gps_tty_path);
    }

    /* get reference coordinates */
    val = json_object_get_value(conf_obj, "ref_latitude");
    if (val != NULL) {
        reference_coord.lat = (double)json_value_get_number(val);
        MSG("INFO: Reference latitude is configured to %f deg\n", reference_coord.lat);
    }
    val = json_object_get_value(conf_obj, "ref_longitude");
    if (val != NULL) {
        reference_coord.lon = (double)json_value_get_number(val);
        MSG("INFO: Reference longitude is configured to %f deg\n", reference_coord.lon);
    }
    val = json_object_get_value(conf_obj, "ref_altitude");
    if (val != NULL) {
        reference_coord.alt = (short)json_value_get_number(val);
        MSG("INFO: Reference altitude is configured to %i meters\n", reference_coord.alt);
    }

    /* Gateway GPS coordinates hardcoding (aka. faking) option */
    val = json_object_get_value(conf_obj, "fake_gps");
    if (json_value_get_type(val) == JSONBoolean) {
        gps_fake_enable = (bool)json_value_get_boolean(val);
        if (gps_fake_enable == true) {
            MSG("INFO: fake GPS is enabled\n");
        } else {
            MSG("INFO: fake GPS is disabled\n");
        }
    }

    /* Beacon signal period (optional) */
    val = json_object_get_value(conf_obj, "beacon_period");
    if (val != NULL) {
        beacon_period = (uint32_t)json_value_get_number(val);
        if ((beacon_period > 0) && (beacon_period < 6)) {
            MSG("ERROR: invalid configuration for Beacon period, must be >= 6s\n");
            return -1;
        } else {
            MSG("INFO: Beaconing period is configured to %lu seconds\n", beacon_period);
        }
    }

    /* Beacon TX frequency (optional) */
    val = json_object_get_value(conf_obj, "beacon_freq_hz");
    if (val != NULL) {
        beacon_freq_hz = (uint32_t)json_value_get_number(val);
        MSG("INFO: Beaconing signal will be emitted at %lu Hz\n", beacon_freq_hz);
    }

    /* Number of beacon channels (optional) */
    val = json_object_get_value(conf_obj, "beacon_freq_nb");
    if (val != NULL) {
        beacon_freq_nb = (uint8_t)json_value_get_number(val);
        MSG("INFO: Beaconing channel number is set to %u\n", beacon_freq_nb);
    }

    /* Frequency step between beacon channels (optional) */
    val = json_object_get_value(conf_obj, "beacon_freq_step");
    if (val != NULL) {
        beacon_freq_step = (uint32_t)json_value_get_number(val);
        MSG("INFO: Beaconing channel frequency step is set to %luHz\n", beacon_freq_step);
    }

    /* Beacon datarate (optional) */
    val = json_object_get_value(conf_obj, "beacon_datarate");
    if (val != NULL) {
        beacon_datarate = (uint8_t)json_value_get_number(val);
        MSG("INFO: Beaconing datarate is set to SF%d\n", beacon_datarate);
    }

    /* Beacon modulation bandwidth (optional) */
    val = json_object_get_value(conf_obj, "beacon_bw_hz");
    if (val != NULL) {
        beacon_bw_hz = (uint32_t)json_value_get_number(val);
        MSG("INFO: Beaconing modulation bandwidth is set to %luHz\n", beacon_bw_hz);
    }

    /* Beacon TX power (optional) */
    val = json_object_get_value(conf_obj, "beacon_power");
    if (val != NULL) {
        beacon_power = (int8_t)json_value_get_number(val);
        MSG("INFO: Beaconing TX power is set to %ddBm\n", beacon_power);
    }

    /* Beacon information descriptor (optional) */
    val = json_object_get_value(conf_obj, "beacon_infodesc");
    if (val != NULL) {
        beacon_infodesc = (uint8_t)json_value_get_number(val);
        MSG("INFO: Beaconing information descriptor is set to %d\n", beacon_infodesc);
    }

    /* Auto-quit threshold (optional) */
    val = json_object_get_value(conf_obj, "autoquit_threshold");
    if (val != NULL) {
        autoquit_threshold = (uint32_t)json_value_get_number(val);
        MSG("INFO: Auto-quit after %lu non-acknowledged PULL_DATA\n", autoquit_threshold);
    }

    /* free JSON parsing data structure */
    json_value_free(root_val);
    return 0;
}

static int parse_debug_configuration(const char *config_array)
{
    int i;
    const char conf_obj_name[] = "debug_conf";
    JSON_Value *root_val;
    JSON_Object *conf_obj = NULL;
    JSON_Array *conf_array = NULL;
    JSON_Object *conf_obj_array = NULL;
    const char *str; /* pointer to sub-strings in the JSON data */

    /* Initialize structure */
    memset(&debugconf, 0, sizeof debugconf);

    /* try to parse JSON */
    root_val = json_parse_array_with_comments(config_array);
    if (root_val == NULL) {
        MSG("ERROR: conf array is not a valid JSON string\n");
        exit(EXIT_FAILURE);
    }

    /* point to the gateway configuration object */
    conf_obj = json_object_get_object(json_value_get_object(root_val), conf_obj_name);
    if (conf_obj == NULL) {
        MSG("INFO: conf array does not contain a JSON object named %s\n", conf_obj_name);
        json_value_free(root_val);
        return -1;
    } else {
        MSG("INFO: conf array does contain a JSON object named %s, parsing debug parameters\n", conf_obj_name);
    }

    /* Get reference payload configuration */
    conf_array = json_object_get_array (conf_obj, "ref_payload");
    if (conf_array != NULL) {
        debugconf.nb_ref_payload = json_array_get_count(conf_array);
        MSG("INFO: got %u debug reference payload\n", debugconf.nb_ref_payload);

        for (i = 0; i < (int)debugconf.nb_ref_payload; i++) {
            conf_obj_array = json_array_get_object(conf_array, i);
            /* id */
            str = json_object_get_string(conf_obj_array, "id");
            if (str != NULL) {
                sscanf(str, "0x%08lX", &(debugconf.ref_payload[i].id));
                MSG("INFO: reference payload ID %d is 0x%08lX\n", i, debugconf.ref_payload[i].id);
            }

            /* global count */
            nb_pkt_received_ref[i] = 0;
        }
    }

    /* Get log file configuration */
    str = json_object_get_string(conf_obj, "log_file");
    if (str != NULL) {
        strncpy(debugconf.log_file_name, str, sizeof debugconf.log_file_name);
        debugconf.log_file_name[sizeof debugconf.log_file_name - 1] = '\0'; /* ensure string termination */
        MSG("INFO: setting debug log file name to %s\n", debugconf.log_file_name);
    }

    /* Commit configuration */
    if (lgw_debug_setconf(&debugconf) != LGW_HAL_SUCCESS) {
        MSG("ERROR: Failed to configure debug\n");
        json_value_free(root_val);
        return -1;
    }

    /* free JSON parsing data structure */
    json_value_free(root_val);
    return 0;
}

static uint16_t crc16(const uint8_t *data, unsigned size)
{
    const uint16_t crc_poly = 0x1021;
    const uint16_t init_val = 0x0000;
    uint16_t x = init_val;
    unsigned i, j;

    if (data == NULL)  {
        return 0;
    }

    for (i = 0; i < size; ++i) {
        x ^= (uint16_t)data[i] << 8;
        for (j = 0; j < 8; ++j) {
            x = (x & 0x8000) ? (x << 1) ^ crc_poly : (x << 1);
        }
    }

    return x;
}

static double difftimespec(struct timespec end, struct timespec beginning)
{
    double x;

    x = 1E-9 * (double)(end.tv_nsec - beginning.tv_nsec);
    x += (double)(end.tv_sec - beginning.tv_sec);

    return x;
}

static int send_tx_ack(uint8_t token_h, uint8_t token_l, enum jit_error_e error, int32_t error_value)
{
    uint8_t buff_ack[ACK_BUFF_SIZE]; /* buffer to give feedback to server */
    int buff_index;
    int j;

    /* reset buffer */
    memset(&buff_ack, 0, sizeof buff_ack);

    /* Prepare downlink feedback to be sent to server */
    buff_ack[0] = PROTOCOL_VERSION;
    buff_ack[1] = token_h;
    buff_ack[2] = token_l;
    buff_ack[3] = PKT_TX_ACK;
    *(uint32_t *)(buff_ack + 4) = net_mac_h;
    *(uint32_t *)(buff_ack + 8) = net_mac_l;
    buff_index = 12; /* 12-byte header */

    /* Put no JSON string if there is nothing to report */
    if (error != JIT_ERROR_OK) {
        /* start of JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"{\"txpk_ack\":{", 13);
        buff_index += 13;
        /* set downlink error/warning status in JSON structure */
        switch ( error ) {
        case JIT_ERROR_TX_POWER:
            memcpy((void *)(buff_ack + buff_index), (void *)"\"warn\":", 7);
            buff_index += 7;
            break;
        default:
            memcpy((void *)(buff_ack + buff_index), (void *)"\"error\":", 8);
            buff_index += 8;
            break;
        }
        /* set error/warning type in JSON structure */
        switch (error) {
        case JIT_ERROR_FULL:
        case JIT_ERROR_COLLISION_PACKET:
            memcpy((void *)(buff_ack + buff_index), (void *)"\"COLLISION_PACKET\"", 18);
            buff_index += 18;
            /* update stats */
            xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
            meas_nb_tx_rejected_collision_packet += 1;
            xSemaphoreGive(mx_meas_dw);
            break;
        case JIT_ERROR_TOO_LATE:
            memcpy((void *)(buff_ack + buff_index), (void *)"\"TOO_LATE\"", 10);
            buff_index += 10;
            /* update stats */
            xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
            meas_nb_tx_rejected_too_late += 1;
            xSemaphoreGive(mx_meas_dw);
            break;
        case JIT_ERROR_TOO_EARLY:
            memcpy((void *)(buff_ack + buff_index), (void *)"\"TOO_EARLY\"", 11);
            buff_index += 11;
            /* update stats */
            xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
            meas_nb_tx_rejected_too_early += 1;
            xSemaphoreGive(mx_meas_dw);
            break;
        case JIT_ERROR_COLLISION_BEACON:
            memcpy((void *)(buff_ack + buff_index), (void *)"\"COLLISION_BEACON\"", 18);
            buff_index += 18;
            /* update stats */
            xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
            meas_nb_tx_rejected_collision_beacon += 1;
            xSemaphoreGive(mx_meas_dw);
            break;
        case JIT_ERROR_TX_FREQ:
            memcpy((void *)(buff_ack + buff_index), (void *)"\"TX_FREQ\"", 9);
            buff_index += 9;
            break;
        case JIT_ERROR_TX_POWER:
            memcpy((void *)(buff_ack + buff_index), (void *)"\"TX_POWER\"", 10);
            buff_index += 10;
            break;
        case JIT_ERROR_GPS_UNLOCKED:
            memcpy((void *)(buff_ack + buff_index), (void *)"\"GPS_UNLOCKED\"", 14);
            buff_index += 14;
            break;
        default:
            memcpy((void *)(buff_ack + buff_index), (void *)"\"UNKNOWN\"", 9);
            buff_index += 9;
            break;
        }
        /* set error/warning details in JSON structure */
        switch (error) {
        case JIT_ERROR_TX_POWER:
            j = snprintf((char *)(buff_ack + buff_index), ACK_BUFF_SIZE - buff_index, ",\"value\":%ld", error_value);
            if (j > 0) {
                buff_index += j;
            } else {
                MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                exit(EXIT_FAILURE);
            }
            break;
        default:
            /* Do nothing */
            break;
        }
        /* end of JSON structure */
        memcpy((void *)(buff_ack + buff_index), (void *)"}}", 2);
        buff_index += 2;
    }

    buff_ack[buff_index] = 0; /* add string terminator, for safety */

    /* send datagram to server */
    //return send(sock_down, (void *)buff_ack, buff_index, 0);
    return sendto(sock_down, (void *)buff_ack, buff_index, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
}

static int dns_loopup(char *hostname, char *ip)
{
    struct hostent *he;
    struct in_addr **addrlist;

    if ((he = gethostbyname(hostname)) == NULL) {
        return -1;
    }

    addrlist = (struct in_addr **) he->h_addr_list;
    for (int i = 0; addrlist[i] != NULL; i++) {
        strncpy(ip, inet_ntoa(*addrlist[i]), IP_LEN);
        ip[IP_LEN - 1] = '\0'; // just to be safe
        return 0;
    }

    return -1;
}

int pkt_fwd_main(void)
{
    int i; /* loop variable and temporary variable for return value */
    int x;
    int l, m;

    /* variables to get local copies of measurements */
    uint32_t cp_nb_rx_rcv;
    uint32_t cp_nb_rx_ok;
    uint32_t cp_nb_rx_bad;
    uint32_t cp_nb_rx_nocrc;
    uint32_t cp_up_pkt_fwd;
    uint32_t cp_up_network_byte;
    uint32_t cp_up_payload_byte;
    uint32_t cp_up_dgram_sent;
    uint32_t cp_up_ack_rcv;
    uint32_t cp_dw_pull_sent;
    uint32_t cp_dw_ack_rcv;
    uint32_t cp_dw_dgram_rcv;
    uint32_t cp_dw_network_byte;
    uint32_t cp_dw_payload_byte;
    uint32_t cp_nb_tx_ok;
    uint32_t cp_nb_tx_fail;
    uint32_t cp_nb_tx_requested = 0;
    uint32_t cp_nb_tx_rejected_collision_packet = 0;
    uint32_t cp_nb_tx_rejected_collision_beacon = 0;
    uint32_t cp_nb_tx_rejected_too_late = 0;
    uint32_t cp_nb_tx_rejected_too_early = 0;
    uint32_t cp_nb_beacon_queued = 0;
    uint32_t cp_nb_beacon_sent = 0;
    uint32_t cp_nb_beacon_rejected = 0;

    /* GPS coordinates variables */
    bool coord_ok = false;
    struct coord_s cp_gps_coord = {0.0, 0.0, 0};

    /* SX1302 data variables */
    uint32_t trig_tstamp;
    uint32_t inst_tstamp;

    /* statistics variable */
    time_t t;
    char stat_timestamp[24];
    float rx_ok_ratio;
    float rx_bad_ratio;
    float rx_nocrc_ratio;
    float up_ack_ratio;
    float dw_ack_ratio;

    // init all mutexes
    mx_concent = xSemaphoreCreateMutex();
    assert(mx_concent);
    mx_xcorr = xSemaphoreCreateMutex();
    assert(mx_xcorr);
    mx_timeref = xSemaphoreCreateMutex();
    assert(mx_timeref);
    mx_meas_up = xSemaphoreCreateMutex();
    assert(mx_meas_up);
    mx_meas_dw = xSemaphoreCreateMutex();
    assert(mx_meas_dw);
    mx_meas_gps = xSemaphoreCreateMutex();
    assert(mx_meas_gps);
    mx_stat_rep = xSemaphoreCreateMutex();
    assert(mx_stat_rep);


    /* display version informations */
    MSG("*** Packet Forwarder ***\nVersion: " VERSION_STRING "\n");
    MSG("*** SX1302 HAL library version info ***\n%s\n***\n", lgw_version_info());

    /* display host endianness */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    MSG("INFO: Little endian host\n");
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    MSG("INFO: Big endian host\n");
#else
    MSG("INFO: Host endianness unknown\n");
#endif

    char *conf_array = NULL;
    int conf_size = 0;
    FILE *f = NULL;
    char *global_conf_path = NULL;
    struct stat *st = malloc(sizeof(struct stat));

    // change conf_array if set to a different region
    if (config[FREQ_REGION].val != NULL) {
        if (strncmp(config[FREQ_REGION].val, "eu868", 5) == 0) {
            global_conf_path = "/spiffs/global_conf/eu868.json";
        } else if (strncmp(config[FREQ_REGION].val, "us915", 5) == 0) {
            global_conf_path = "/spiffs/global_conf/us915.json";
        } else {
            global_conf_path = "/spiffs/global_conf/cn490.json";
        }
    } else {
        global_conf_path = "/spiffs/global_conf/cn490.json";
    }

    f = fopen(global_conf_path, "r");
    if (f == NULL)
        ESP_LOGE(PKT_TAG, "global_confg can't found,%s\n", global_conf_path);


    int fd = fileno(f);
    fstat(fd, st);
    conf_size = st->st_size + 3;
    conf_array = malloc(conf_size);
    conf_array[0] = st->st_size >> 8;
    conf_array[1] = st->st_size & 0xff;
    fread(conf_array + 2, 1, st->st_size, f);
    fclose(f);
    conf_array[conf_size - 1] = '\0';
    MSG("Config loaded into memory\n");

    free(st);

    // update radio_0 and radio_1 frequencies
    int fwd_offset = 8;  // forward '"freq": ' which is 8 characters.
    int new_freq_len = 9;  // sub-G with unit 'Hz' should have 9 characters
    char *radio0_index = strstr(conf_array, "\"freq\"");
    char *radio1_index = strstr(radio0_index + fwd_offset, "\"freq\"");

    if (config[FREQ_RADIO0].val != NULL && radio0_index != NULL) {
        if (config[FREQ_RADIO0].len == new_freq_len) // need to match the exact length
            strncpy(radio0_index + fwd_offset, config[FREQ_RADIO0].val, new_freq_len);
    }
    if (config[FREQ_RADIO1].val != NULL && radio1_index != NULL) {
        if (config[FREQ_RADIO1].len == new_freq_len) // need to match the exact length
            strncpy(radio1_index + fwd_offset, config[FREQ_RADIO1].val, new_freq_len);
    }

    /* load configuration files */
    x = parse_SX130x_configuration(conf_array);
    if (x != 0) {
        MSG("INFO: no SX130x configuration\n");
        exit(EXIT_FAILURE);
    }
    x = parse_gateway_configuration(conf_array);
    if (x != 0) {
        MSG("INFO: no gateway configuration\n");
        exit(EXIT_FAILURE);
    }
    x = parse_debug_configuration(conf_array);
    if (x != 0) {
        MSG("INFO: no debug configuration\n");
    }

    free(conf_array);

    // TODO
    /* Start GPS a.s.a.p., to allow it to lock */
    gps_enabled = true;
    gps_ref_valid = false;
#ifndef GPS_DISABLE
    i = lgw_gps_enable("ATGM336H", 0, (uart_port_t *)&gps_tty_fd); /* HAL only supports atgm336h or u-blox 7 for now */
    if (i != LGW_GPS_SUCCESS) {
        printf("WARNING: [main] impossible to open %s for GPS sync (check permissions)\n", gps_tty_path);
        gps_enabled = false;
        gps_ref_valid = false;
    } else {
        printf("INFO: [main] TTY port %s open for GPS synchronization\n", gps_tty_path);
        //gps_enabled = true;
        //gps_ref_valid = false;
    }
#endif

    /* get timezone info */
    tzset();

    /* sanity check on configuration variables */
    // TODO

    // some debug to see if everything goes all right
    ESP_LOGI(PKT_TAG, "serv_addr: %s", serv_addr);
    ESP_LOGI(PKT_TAG, "serv_port_up: %s", serv_port_up);
    ESP_LOGI(PKT_TAG, "serv_port_down: %s", serv_port_down);
    ESP_LOGI(PKT_TAG, "udp_host: %s", udp_host);
    ESP_LOGI(PKT_TAG, "udp_port: %lu", udp_port);
    ESP_LOGI(PKT_TAG, "gw_id: %s", gw_id);

    // use the values from json file if not set in config[]
    if (udp_host[0] == '\0') {
        strncpy(udp_host, serv_addr, sizeof udp_host);
        udp_host[sizeof udp_host - 1] = '\0'; /* ensure string termination */
    }
    if (udp_port == 0)
        udp_port = atoi(serv_port_up);

    unsigned long long ull = 0;
    if (gw_id[0] != '\0') {
        sscanf(gw_id, "%llx", &ull);
        lgwm = ull;
        MSG("INFO: gateway MAC address is configured to %016llX\n", ull);
    }

    /* process some of the configuration variables */
    net_mac_h = htonl((uint32_t)(0xFFFFFFFF & (lgwm >> 32)));
    net_mac_l = htonl((uint32_t)(0xFFFFFFFF &  lgwm  ));

    // -------------------------------
    int addr_family = 0;
    int ip_protocol = 0;
    char ip[IP_LEN] = { 0 };

    addr_family = AF_INET;
    ip_protocol = IPPROTO_IP;
    sock_up = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock_up < 0) {
        ESP_LOGE(PKT_TAG, "Unable to create up socket: errno %d", errno);
        return -1;
    }
    sock_down = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock_down < 0) {
        ESP_LOGE(PKT_TAG, "Unable to create down socket: errno %d", errno);
        return -1;
    }
    ESP_LOGI(PKT_TAG, "Socket created, sending to %s:%lu", udp_host, udp_port);

    dns_loopup(udp_host, ip);
    dest_addr.sin_addr.s_addr = inet_addr(ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(udp_port);

    // Init_Led(); // Initialize LED

#if 0
    /* network socket creation */
    struct addrinfo *result; /* store result of getaddrinfo */
    struct addrinfo *q; /* pointer to move into *result data */

    /* connect so we can send/receive packet with the server only */
    i = connect(sock_up, q->ai_addr, q->ai_addrlen);
    if (i != 0) {
        MSG("ERROR: [up] connect returned %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    freeaddrinfo(result);

    /* connect so we can send/receive packet with the server only */
    i = connect(sock_down, q->ai_addr, q->ai_addrlen);
    if (i != 0) {
        MSG("ERROR: [down] connect returned %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    freeaddrinfo(result);
#endif

    /* set upstream socket RX timeout */
    i = setsockopt(sock_up, SOL_SOCKET, SO_RCVTIMEO, (void *)&push_timeout_half, sizeof push_timeout_half);
    if (i != 0) {
        MSG("ERROR: [up] setsockopt returned %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (com_type == LGW_COM_SPI) {
        /* Board reset */
        lgw_reset();
    }

    for (l = 0; l < LGW_IF_CHAIN_NB; l++) {
        for (m = 0; m < 8; m++) {
            nb_pkt_log[l][m] = 0;
        }
    }

    /* starting the concentrator */
    i = lgw_start();
    if (i == LGW_HAL_SUCCESS) {
        MSG("INFO: [main] concentrator started, packet can now be received\n");
    } else {
        MSG("ERROR: [main] failed to start the concentrator\n");
        exit(EXIT_FAILURE);
    }

#if 0  // TODO
    /* get the concentrator EUI */
    uint64_t eui;
    i = lgw_get_eui(&eui);
    if (i != LGW_HAL_SUCCESS) {
        printf("ERROR: failed to get concentrator EUI\n");
    } else {
        printf("INFO: concentrator EUI: 0x%016" PRIx64 "\n", eui);
    }
#endif

    // if( xTaskCreatePinnedToCore(((TaskFunction_t) vDaemonLedIndication), "led_flash", 4096, (void *)pLed, 1, NULL, tskNO_AFFINITY) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY) {
    //     printf( "Failed to spawn led_flash\n");
    // } else {
    //     printf( "led_flash spawned\n" );
    // }

    /* JIT queue initialization */
    jit_queue_init(&jit_queue[0]);
    jit_queue_init(&jit_queue[1]);

    if ( xTaskCreatePinnedToCore(((TaskFunction_t) thread_up), "thread_up", (4096 * 4), NULL, 6, &pThreadUp, tskNO_AFFINITY) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY) {
        printf( "Failed to spawn thread_up\n");
        printf( "largest_free_block: %d\n", heap_caps_get_largest_free_block( MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT ));
    } else {
        printf( "Thread_up spawned\n" );
    }

    if ( xTaskCreatePinnedToCore(((TaskFunction_t) thread_down), "thread_down", 4096 * 2, NULL, 6, &pThreadDn, tskNO_AFFINITY) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY) {
        printf( "Failed to spawn thread_down\n");
    } else {
        printf( "Thread_down spawned\n" );
    }

    if ( xTaskCreatePinnedToCore(((TaskFunction_t) thread_jit), "thread_jit", 4096 * 2, NULL, 6, &pJit, tskNO_AFFINITY) == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY) {
        printf( "Failed to spawn thread_jit\n");
    } else {
        printf( "Thread_jit spawned\n" );
    }

#if 1
    /* threads */
    // pthread_t thrid_gps;
    // pthread_t thrid_valid;

    /* spawn thread to manage GPS */
    if (gps_enabled == true) {
        // xTaskCreate();
        xTaskCreate(((TaskFunction_t) thread_gps), "gps", 2 * 4096, NULL, 6, &pGps);
        xTaskCreate(((TaskFunction_t) thread_valid), "gps_valid", 1 * 4096, NULL, 6, NULL);

        // i = pthread_create(&thrid_gps, NULL, (void * (*)(void *))thread_gps, NULL);
        // if (i != 0) {
        //     MSG("ERROR: [main] impossible to create GPS thread\n");
        //     exit(EXIT_FAILURE);
        // }
        // i = pthread_create(&thrid_valid, NULL, (void * (*)(void *))thread_valid, NULL);
        // if (i != 0) {
        //     MSG("ERROR: [main] impossible to create validation thread\n");
        //     exit(EXIT_FAILURE);
        // }
    }
#endif

    /* main loop task: statistics collection */
    while (!exit_sig && !quit_sig) {
        time_count = 0;
        while (time_count < stat_interval) {
            /* wait for next reporting interval */
            //wait_ms(1000 * stat_interval);
            vTaskDelay(1000 * TIME_REFRESH / portTICK_PERIOD_MS);
            time_count += TIME_REFRESH;

            /* get timestamp for statistics */
            t = time(NULL);
            // we use "Z" to replace the "GMT" to shorten the display
            strftime(stat_timestamp, sizeof stat_timestamp, "%F %T Z", gmtime(&t));
            if (gps_enabled) {
                // // Read data from GPS UART.
                // uint8_t data[1024];
                // int length, min;
                // ESP_ERROR_CHECK(uart_get_buffered_data_len((uart_port_t)gps_tty_fd, (size_t *)&length));
                // min = (length < 1024) ? length : 1024;
                // length = uart_read_bytes((uart_port_t)gps_tty_fd, data, min, 100);
                // data[min] = '\0';
                // printf("GPS Raw Data -------> length = %d, min = %d:\n%s\n", length, min, data);
            }
        }
        strftime(stat_timestamp, sizeof stat_timestamp, "%F %T %Z", gmtime(&t));

        /* access upstream statistics, copy and reset them */
        xSemaphoreTake(mx_meas_up, portMAX_DELAY);
        cp_nb_rx_rcv       = meas_nb_rx_rcv;
        cp_nb_rx_ok        = meas_nb_rx_ok;
        cp_nb_rx_bad       = meas_nb_rx_bad;
        cp_nb_rx_nocrc     = meas_nb_rx_nocrc;
        cp_up_pkt_fwd      = meas_up_pkt_fwd;
        cp_up_network_byte = meas_up_network_byte;
        cp_up_payload_byte = meas_up_payload_byte;
        cp_up_dgram_sent   = meas_up_dgram_sent;
        cp_up_ack_rcv      = meas_up_ack_rcv;
        meas_nb_rx_rcv = 0;
        meas_nb_rx_ok = 0;
        meas_nb_rx_bad = 0;
        meas_nb_rx_nocrc = 0;
        meas_up_pkt_fwd = 0;
        meas_up_network_byte = 0;
        meas_up_payload_byte = 0;
        meas_up_dgram_sent = 0;
        meas_up_ack_rcv = 0;
        xSemaphoreGive(mx_meas_up);
        if (cp_nb_rx_rcv > 0) {
            rx_ok_ratio = (float)cp_nb_rx_ok / (float)cp_nb_rx_rcv;
            rx_bad_ratio = (float)cp_nb_rx_bad / (float)cp_nb_rx_rcv;
            rx_nocrc_ratio = (float)cp_nb_rx_nocrc / (float)cp_nb_rx_rcv;
        } else {
            rx_ok_ratio = 0.0;
            rx_bad_ratio = 0.0;
            rx_nocrc_ratio = 0.0;
        }
        if (cp_up_dgram_sent > 0) {
            up_ack_ratio = (float)cp_up_ack_rcv / (float)cp_up_dgram_sent;
        } else {
            up_ack_ratio = 0.0;
        }

        /* access downstream statistics, copy and reset them */
        xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
        cp_dw_pull_sent    =  meas_dw_pull_sent;
        cp_dw_ack_rcv      =  meas_dw_ack_rcv;
        cp_dw_dgram_rcv    =  meas_dw_dgram_rcv;
        cp_dw_network_byte =  meas_dw_network_byte;
        cp_dw_payload_byte =  meas_dw_payload_byte;
        cp_nb_tx_ok        =  meas_nb_tx_ok;
        cp_nb_tx_fail      =  meas_nb_tx_fail;
        cp_nb_tx_requested                 +=  meas_nb_tx_requested;
        cp_nb_tx_rejected_collision_packet +=  meas_nb_tx_rejected_collision_packet;
        cp_nb_tx_rejected_collision_beacon +=  meas_nb_tx_rejected_collision_beacon;
        cp_nb_tx_rejected_too_late         +=  meas_nb_tx_rejected_too_late;
        cp_nb_tx_rejected_too_early        +=  meas_nb_tx_rejected_too_early;
        cp_nb_beacon_queued   +=  meas_nb_beacon_queued;
        cp_nb_beacon_sent     +=  meas_nb_beacon_sent;
        cp_nb_beacon_rejected +=  meas_nb_beacon_rejected;
        meas_dw_pull_sent = 0;
        meas_dw_ack_rcv = 0;
        meas_dw_dgram_rcv = 0;
        meas_dw_network_byte = 0;
        meas_dw_payload_byte = 0;
        meas_nb_tx_ok = 0;
        meas_nb_tx_fail = 0;
        meas_nb_tx_requested = 0;
        meas_nb_tx_rejected_collision_packet = 0;
        meas_nb_tx_rejected_collision_beacon = 0;
        meas_nb_tx_rejected_too_late = 0;
        meas_nb_tx_rejected_too_early = 0;
        meas_nb_beacon_queued = 0;
        meas_nb_beacon_sent = 0;
        meas_nb_beacon_rejected = 0;
        xSemaphoreGive(mx_meas_dw);
        if (cp_dw_pull_sent > 0) {
            dw_ack_ratio = (float)cp_dw_ack_rcv / (float)cp_dw_pull_sent;
        } else {
            dw_ack_ratio = 0.0;
        }

        /* access GPS statistics, copy them */
        if (gps_enabled == true) {
            xSemaphoreTake(mx_meas_gps, portMAX_DELAY);
            coord_ok = gps_coord_valid;
            cp_gps_coord = meas_gps_coord;
            xSemaphoreGive(mx_meas_gps);
        }

        /* overwrite with reference coordinates if function is enabled */
        if (gps_fake_enable == true) {
            cp_gps_coord = reference_coord;
        }

        /* display a report */
        printf("esp running time : %llu ms\n", (esp_timer_get_time() / 1000ULL));
        printf("\n##### %s #####\n", stat_timestamp);
        printf("### [UPSTREAM] ###\n");
        printf("# RF packets received by concentrator: %lu\n", cp_nb_rx_rcv);
        printf("# CRC_OK: %.2f%%, CRC_FAIL: %.2f%%, NO_CRC: %.2f%%\n", 100.0 * rx_ok_ratio, 100.0 * rx_bad_ratio, 100.0 * rx_nocrc_ratio);
        printf("# RF packets forwarded: %lu (%lu bytes)\n", cp_up_pkt_fwd, cp_up_payload_byte);
        printf("# PUSH_DATA datagrams sent: %lu (%lu bytes)\n", cp_up_dgram_sent, cp_up_network_byte);
        printf("# PUSH_DATA acknowledged: %.2f%%\n", 100.0 * up_ack_ratio);
        printf("### [DOWNSTREAM] ###\n");
        printf("# PULL_DATA sent: %lu (%.2f%% acknowledged)\n", cp_dw_pull_sent, 100.0 * dw_ack_ratio);
        printf("# PULL_RESP(onse) datagrams received: %lu (%lu bytes)\n", cp_dw_dgram_rcv, cp_dw_network_byte);
        printf("# RF packets sent to concentrator: %lu (%lu bytes)\n", (cp_nb_tx_ok + cp_nb_tx_fail), cp_dw_payload_byte);
        printf("# TX errors: %lu\n", cp_nb_tx_fail);
        if (cp_nb_tx_requested != 0 ) {
            printf("# TX rejected (collision packet): %.2f%% (req:%lu, rej:%lu)\n", 100.0 * cp_nb_tx_rejected_collision_packet / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_collision_packet);
            printf("# TX rejected (collision beacon): %.2f%% (req:%lu, rej:%lu)\n", 100.0 * cp_nb_tx_rejected_collision_beacon / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_collision_beacon);
            printf("# TX rejected (too late): %.2f%% (req:%lu, rej:%lu)\n", 100.0 * cp_nb_tx_rejected_too_late / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_too_late);
            printf("# TX rejected (too early): %.2f%% (req:%lu, rej:%lu)\n", 100.0 * cp_nb_tx_rejected_too_early / cp_nb_tx_requested, cp_nb_tx_requested, cp_nb_tx_rejected_too_early);
        }
        printf("### SX1302 Status ###\n");
        xSemaphoreTake(mx_concent, portMAX_DELAY);
        i  = lgw_get_instcnt(&inst_tstamp);
        i |= lgw_get_trigcnt(&trig_tstamp);
        xSemaphoreGive(mx_concent);
        if (i != LGW_HAL_SUCCESS) {
            printf("# SX1302 counter unknown\n");
        } else {
            printf("# SX1302 counter (INST): %lu\n", inst_tstamp);
            printf("# SX1302 counter (PPS):  %lu\n", trig_tstamp);
        }
        printf("# BEACON queued: %lu\n", cp_nb_beacon_queued);
        printf("# BEACON sent so far: %lu\n", cp_nb_beacon_sent);
        printf("# BEACON rejected: %lu\n", cp_nb_beacon_rejected);
        printf("### [JIT] ###\n");
        /* get timestamp captured on PPM pulse  */
        jit_print_queue (&jit_queue[0], false, DEBUG_LOG);
        printf("#--------\n");
        jit_print_queue (&jit_queue[1], false, DEBUG_LOG);
        printf("### [GPS] ###\n");
        if (gps_enabled == true) {
            /* no need for mutex, display is not critical */
            if (gps_ref_valid == true) {
                printf("# Valid time reference (age: %li sec)\n", (long)difftime(time(NULL), time_reference_gps.systime));
            } else {
                printf("# Invalid time reference (age: %li sec)\n", (long)difftime(time(NULL), time_reference_gps.systime));
            }
            if (coord_ok == true) {
                printf("# GPS coordinates: latitude %.5f, longitude %.5f, altitude %i m\n", cp_gps_coord.lat, cp_gps_coord.lon, cp_gps_coord.alt);
            } else {
                printf("# no valid GPS coordinates available yet\n");
            }
        } else if (gps_fake_enable == true) {
            printf("# GPS *FAKE* coordinates: latitude %.5f, longitude %.5f, altitude %i m\n", cp_gps_coord.lat, cp_gps_coord.lon, cp_gps_coord.alt);
        } else {
            printf("# GPS sync is disabled\n");
        }
        printf("##### END #####\n");

        /* generate a JSON report (will be sent to server by upstream thread) */
        xSemaphoreTake(mx_stat_rep, portMAX_DELAY);
        if (((gps_enabled == true) && (coord_ok == true)) || (gps_fake_enable == true)) {
            snprintf(status_report, STATUS_SIZE, "\"stat\":{\"time\":\"%s\",\"lati\":%.5f,\"long\":%.5f,\"alti\":%i,\"rxnb\":%lu,\"rxok\":%lu,\"rxfw\":%lu,\"ackr\":%.1f,\"dwnb\":%lu,\"txnb\":%lu}", stat_timestamp, cp_gps_coord.lat, cp_gps_coord.lon, cp_gps_coord.alt, cp_nb_rx_rcv, cp_nb_rx_ok, cp_up_pkt_fwd, 100.0 * up_ack_ratio, cp_dw_dgram_rcv, cp_nb_tx_ok);
        } else {
            snprintf(status_report, STATUS_SIZE, "\"stat\":{\"time\":\"%s\",\"rxnb\":%lu,\"rxok\":%lu,\"rxfw\":%lu,\"ackr\":%.1f,\"dwnb\":%lu,\"txnb\":%lu}", stat_timestamp, cp_nb_rx_rcv, cp_nb_rx_ok, cp_up_pkt_fwd, 100.0 * up_ack_ratio, cp_dw_dgram_rcv, cp_nb_tx_ok);
        }
        report_ready = true;
        xSemaphoreGive(mx_stat_rep);
    }

    // TODO
    /* wait for all threads with a COM with the concentrator board to finish (1 fetch cycle max) */
#if 0
    i = pthread_join(thrid_up, NULL);
    if (i != 0) {
        printf("ERROR: failed to join upstream thread with %d - %s\n", i, strerror(errno));
    }
    i = pthread_join(thrid_down, NULL);
    if (i != 0) {
        printf("ERROR: failed to join downstream thread with %d - %s\n", i, strerror(errno));
    }
    i = pthread_join(thrid_jit, NULL);
    if (i != 0) {
        printf("ERROR: failed to join JIT thread with %d - %s\n", i, strerror(errno));
    }
    if (spectral_scan_params.enable == true) {
        i = pthread_join(thrid_ss, NULL);
        if (i != 0) {
            printf("ERROR: failed to join Spectral Scan thread with %d - %s\n", i, strerror(errno));
        }
    }
    if (gps_enabled == true) {
        pthread_cancel(thrid_gps); /* don't wait for GPS thread, no access to concentrator board */
        pthread_cancel(thrid_valid); /* don't wait for validation thread, no access to concentrator board */

        i = lgw_gps_disable(gps_tty_fd);
        if (i == LGW_HAL_SUCCESS) {
            MSG("INFO: GPS closed successfully\n");
        } else {
            MSG("WARNING: failed to close GPS successfully\n");
        }
    }

    /* if an exit signal was received, try to quit properly */
    if (exit_sig) {
        /* shut down network sockets */
        shutdown(sock_up, SHUT_RDWR);
        shutdown(sock_down, SHUT_RDWR);
        /* stop the hardware */
        i = lgw_stop();
        if (i == LGW_HAL_SUCCESS) {
            MSG("INFO: concentrator stopped successfully\n");
        } else {
            MSG("WARNING: failed to stop concentrator successfully\n");
        }
    }
#endif

    if (com_type == LGW_COM_SPI) {
        /* Board reset */
        lgw_reset();
    }

    MSG("INFO: Exiting packet forwarder program\n");
    return 0;
}


/* --- THREAD 1: RECEIVING PACKETS AND FORWARDING THEM ---------------------- */
uint8_t buff_up[TX_BUFF_SIZE]; /* buffer to compose the upstream packet */
struct lgw_pkt_rx_s rxpkt[NB_PKT_MAX]; /* array containing inbound packets + metadata */
void thread_up(void)
{
    int i, j, k; /* loop variables */
    unsigned pkt_in_dgram; /* nb on Lora packet in the current datagram */
    char stat_timestamp[24];
    time_t t;

    /* allocate memory for packet fetching and processing */
    struct lgw_pkt_rx_s *p; /* pointer on a RX packet */
    int nb_pkt;

    /* local copy of GPS time reference */
    bool ref_ok = false; /* determine if GPS time reference must be used or not */
    struct tref local_ref; /* time reference used for UTC <-> timestamp conversion */

    /* data buffers */
    int buff_index;
    uint8_t buff_ack[32]; /* buffer to receive acknowledges */

    /* protocol variables */
    uint8_t token_h; /* random token for acknowledgement matching */
    uint8_t token_l; /* random token for acknowledgement matching */

    /* ping measurement variables */
    struct timespec send_time;
    struct timespec recv_time;

    /* GPS synchronization variables */
    struct timespec pkt_utc_time;
    struct tm *x;  /* broken-up UTC time */
    struct timespec pkt_gps_time;
    uint64_t pkt_gps_time_ms;

    /* report management variable */
    bool send_report = false;

    /* mote info variables */
    uint32_t mote_addr = 0;
    uint16_t mote_fcnt = 0;

    /* set upstream socket RX timeout */
    i = setsockopt(sock_up, SOL_SOCKET, SO_RCVTIMEO, (void *)&push_timeout_half, sizeof push_timeout_half);
    if (i != 0) {
        MSG("ERROR: [up] setsockopt returned %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* pre-fill the data buffer with fixed fields */
    buff_up[0] = PROTOCOL_VERSION;
    buff_up[3] = PKT_PUSH_DATA;
    *(uint32_t *)(buff_up + 4) = net_mac_h;
    *(uint32_t *)(buff_up + 8) = net_mac_l;

    while (!exit_sig && !quit_sig) {

        /* fetch packets */
        xSemaphoreTake(mx_concent, portMAX_DELAY);
        nb_pkt = lgw_receive(NB_PKT_MAX, rxpkt);
        xSemaphoreGive(mx_concent);
        if (nb_pkt == LGW_HAL_ERROR) {
            MSG("ERROR: [up] failed packet fetch, exiting\n");
            exit(EXIT_FAILURE);
        }

        /* check if there are status report to send */
        send_report = report_ready; /* copy the variable so it doesn't change mid-function */
        /* no mutex, we're only reading */

        /* wait a short time if no packets, nor status report */
        if ((nb_pkt == 0) && (send_report == false)) {
            //wait_ms(FETCH_SLEEP_MS);
            vTaskDelay(FETCH_SLEEP_MS / portTICK_PERIOD_MS);
            continue;
        }
        if (nb_pkt > 0)
            vUplinkFlash(10);

        /* get a copy of GPS time reference (avoid 1 mutex per packet) */
        if ((nb_pkt > 0) && (gps_enabled == true)) {
            xSemaphoreTake(mx_timeref, portMAX_DELAY);
            ref_ok = gps_ref_valid;
            local_ref = time_reference_gps;
            xSemaphoreGive(mx_timeref);
        } else {
            ref_ok = false;
        }

        /* get timestamp for statistics */
        t = time(NULL);
        strftime(stat_timestamp, sizeof stat_timestamp, "%F %T %Z", gmtime(&t));
        MSG_DEBUG(DEBUG_PKT_FWD, "\nCurrent time: %s \n", stat_timestamp);

        /* start composing datagram with the header */
        token_h = (uint8_t)rand(); /* random token */
        token_l = (uint8_t)rand(); /* random token */
        buff_up[1] = token_h;
        buff_up[2] = token_l;
        buff_index = 12; /* 12-byte header */

        /* start of JSON structure */
        memcpy((void *)(buff_up + buff_index), (void *)"{\"rxpk\":[", 9);
        buff_index += 9;

        /* serialize Lora packets metadata and payload */
        pkt_in_dgram = 0;
        for (i = 0; i < nb_pkt; ++i) {
            p = &rxpkt[i];

            /* Get mote information from current packet (addr, fcnt) */
            /* FHDR - DevAddr */
            if (p->size >= 8) {
                mote_addr  = p->payload[1];
                mote_addr |= p->payload[2] << 8;
                mote_addr |= p->payload[3] << 16;
                mote_addr |= p->payload[4] << 24;
                /* FHDR - FCnt */
                mote_fcnt  = p->payload[6];
                mote_fcnt |= p->payload[7] << 8;
            } else {
                mote_addr = 0;
                mote_fcnt = 0;
            }

            /* basic packet filtering */
            xSemaphoreTake(mx_meas_up, portMAX_DELAY);
            meas_nb_rx_rcv += 1;
            switch (p->status) {
            case STAT_CRC_OK:
                meas_nb_rx_ok += 1;
                if (!fwd_valid_pkt) {
                    xSemaphoreGive(mx_meas_up);
                    continue; /* skip that packet */
                }
                break;
            case STAT_CRC_BAD:
                meas_nb_rx_bad += 1;
                if (!fwd_error_pkt) {
                    xSemaphoreGive(mx_meas_up);
                    continue; /* skip that packet */
                }
                break;
            case STAT_NO_CRC:
                meas_nb_rx_nocrc += 1;
                if (!fwd_nocrc_pkt) {
                    xSemaphoreGive(mx_meas_up);
                    continue; /* skip that packet */
                }
                break;
            default:
                MSG("WARNING: [up] received packet with unknown status %u (size %u, modulation %u, BW %u, DR %lu, RSSI %.1f)\n", p->status, p->size, p->modulation, p->bandwidth, p->datarate, p->rssic);
                xSemaphoreGive(mx_meas_up);
                continue; /* skip that packet */
                // exit(EXIT_FAILURE);
            }
            meas_up_pkt_fwd += 1;
            meas_up_payload_byte += p->size;
            xSemaphoreGive(mx_meas_up);
            printf( "\nINFO: Received pkt from mote: %08lX (fcnt=%u)\n", mote_addr, mote_fcnt );

            /* Start of packet, add inter-packet separator if necessary */
            if (pkt_in_dgram == 0) {
                buff_up[buff_index] = '{';
                ++buff_index;
            } else {
                buff_up[buff_index] = ',';
                buff_up[buff_index + 1] = '{';
                buff_index += 2;
            }

            /* JSON rxpk frame format version, 8 useful chars */
            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, "\"jver\":%d", PROTOCOL_JSON_RXPK_FRAME_FORMAT );
            if (j > 0) {
                buff_index += j;
            } else {
                MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                exit(EXIT_FAILURE);
            }

            /* RAW timestamp, 8-17 useful chars */
            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, ",\"tmst\":%lu", p->count_us);
            if (j > 0) {
                buff_index += j;
            } else {
                MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                exit(EXIT_FAILURE);
            }

            /* Packet RX time (GPS based), 37 useful chars */
            if (ref_ok == true) {
                /* convert packet timestamp to UTC absolute time */
                j = lgw_cnt2utc(local_ref, p->count_us, &pkt_utc_time);
                if (j == LGW_GPS_SUCCESS) {
                    /* split the UNIX timestamp to its calendar components */
                    x = gmtime(&(pkt_utc_time.tv_sec));
                    j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, ",\"time\":\"%04i-%02i-%02iT%02i:%02i:%02i.%06liZ\"", (x->tm_year) + 1900, (x->tm_mon) + 1, x->tm_mday, x->tm_hour, x->tm_min, x->tm_sec, (pkt_utc_time.tv_nsec) / 1000); /* ISO 8601 format */
                    if (j > 0) {
                        buff_index += j;
                    } else {
                        MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                        exit(EXIT_FAILURE);
                    }
                }
                /* convert packet timestamp to GPS absolute time */
                j = lgw_cnt2gps(local_ref, p->count_us, &pkt_gps_time);
                if (j == LGW_GPS_SUCCESS) {
                    pkt_gps_time_ms = pkt_gps_time.tv_sec * 1E3 + pkt_gps_time.tv_nsec / 1E6;
                    j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, ",\"tmms\":%" PRIu64 "", pkt_gps_time_ms); /* GPS time in milliseconds since 06.Jan.1980 */
                    if (j > 0) {
                        buff_index += j;
                    } else {
                        MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                        exit(EXIT_FAILURE);
                    }
                }
            }

            /* Fine timestamp */
            if (p->ftime_received == true) {
                j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, ",\"ftime\":%lu", p->ftime);
                if (j > 0) {
                    buff_index += j;
                } else {
                    MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                    exit(EXIT_FAILURE);
                }
            }

            /* Packet concentrator channel, RF chain & RX frequency, 34-36 useful chars */
            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, ",\"chan\":%1u,\"rfch\":%1u,\"freq\":%.6lf,\"mid\":%2u", p->if_chain, p->rf_chain, ((double)p->freq_hz / 1e6), p->modem_id);
            if (j > 0) {
                buff_index += j;
            } else {
                MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                exit(EXIT_FAILURE);
            }

            /* Packet status, 9-10 useful chars */
            switch (p->status) {
            case STAT_CRC_OK:
                memcpy((void *)(buff_up + buff_index), (void *)",\"stat\":1", 9);
                buff_index += 9;
                break;
            case STAT_CRC_BAD:
                memcpy((void *)(buff_up + buff_index), (void *)",\"stat\":-1", 10);
                buff_index += 10;
                break;
            case STAT_NO_CRC:
                memcpy((void *)(buff_up + buff_index), (void *)",\"stat\":0", 9);
                buff_index += 9;
                break;
            default:
                MSG("ERROR: [up] received packet with unknown status 0x%02X\n", p->status);
                memcpy((void *)(buff_up + buff_index), (void *)",\"stat\":?", 9);
                buff_index += 9;
                exit(EXIT_FAILURE);
            }

            /* Packet modulation, 13-14 useful chars */
            if (p->modulation == MOD_LORA) {
                memcpy((void *)(buff_up + buff_index), (void *)",\"modu\":\"LORA\"", 14);
                buff_index += 14;

                /* Lora datarate & bandwidth, 16-19 useful chars */
                switch (p->datarate) {
                case DR_LORA_SF5:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF5", 12);
                    buff_index += 12;
                    break;
                case DR_LORA_SF6:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF6", 12);
                    buff_index += 12;
                    break;
                case DR_LORA_SF7:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF7", 12);
                    buff_index += 12;
                    break;
                case DR_LORA_SF8:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF8", 12);
                    buff_index += 12;
                    break;
                case DR_LORA_SF9:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF9", 12);
                    buff_index += 12;
                    break;
                case DR_LORA_SF10:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF10", 13);
                    buff_index += 13;
                    break;
                case DR_LORA_SF11:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF11", 13);
                    buff_index += 13;
                    break;
                case DR_LORA_SF12:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF12", 13);
                    buff_index += 13;
                    break;
                default:
                    MSG("ERROR: [up] lora packet with unknown datarate 0x%02X\n", (uint16_t)p->datarate);
                    memcpy((void *)(buff_up + buff_index), (void *)",\"datr\":\"SF?", 12);
                    buff_index += 12;
                    exit(EXIT_FAILURE);
                }
                switch (p->bandwidth) {
                case BW_125KHZ:
                    memcpy((void *)(buff_up + buff_index), (void *)"BW125\"", 6);
                    buff_index += 6;
                    break;
                case BW_250KHZ:
                    memcpy((void *)(buff_up + buff_index), (void *)"BW250\"", 6);
                    buff_index += 6;
                    break;
                case BW_500KHZ:
                    memcpy((void *)(buff_up + buff_index), (void *)"BW500\"", 6);
                    buff_index += 6;
                    break;
                default:
                    MSG("ERROR: [up] lora packet with unknown bandwidth 0x%02X\n", p->bandwidth);
                    memcpy((void *)(buff_up + buff_index), (void *)"BW?\"", 4);
                    buff_index += 4;
                    exit(EXIT_FAILURE);
                }

                /* Packet ECC coding rate, 11-13 useful chars */
                switch (p->coderate) {
                case CR_LORA_4_5:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"4/5\"", 13);
                    buff_index += 13;
                    break;
                case CR_LORA_4_6:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"4/6\"", 13);
                    buff_index += 13;
                    break;
                case CR_LORA_4_7:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"4/7\"", 13);
                    buff_index += 13;
                    break;
                case CR_LORA_4_8:
                    memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"4/8\"", 13);
                    buff_index += 13;
                    break;
                case 0: /* treat the CR0 case (mostly false sync) */
                    memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"OFF\"", 13);
                    buff_index += 13;
                    break;
                default:
                    MSG("ERROR: [up] lora packet with unknown coderate 0x%02X\n", p->coderate);
                    memcpy((void *)(buff_up + buff_index), (void *)",\"codr\":\"?\"", 11);
                    buff_index += 11;
                    exit(EXIT_FAILURE);
                }

                /* Signal RSSI, payload size */
                j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, ",\"rssis\":%.0f", roundf(p->rssis));
                if (j > 0) {
                    buff_index += j;
                } else {
                    MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                    exit(EXIT_FAILURE);
                }

                /* Lora SNR */
                j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, ",\"lsnr\":%.1f", p->snr);
                if (j > 0) {
                    buff_index += j;
                } else {
                    MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                    exit(EXIT_FAILURE);
                }

                /* Lora frequency offset */
                j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, ",\"foff\":%ld", p->freq_offset);
                if (j > 0) {
                    buff_index += j;
                } else {
                    MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                    exit(EXIT_FAILURE);
                }
            } else if (p->modulation == MOD_FSK) {
                memcpy((void *)(buff_up + buff_index), (void *)",\"modu\":\"FSK\"", 13);
                buff_index += 13;

                /* FSK datarate, 11-14 useful chars */
                j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, ",\"datr\":%lu", p->datarate);
                if (j > 0) {
                    buff_index += j;
                } else {
                    MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                    exit(EXIT_FAILURE);
                }
            } else {
                MSG("ERROR: [up] received packet with unknown modulation 0x%02X\n", p->modulation);
                exit(EXIT_FAILURE);
            }

            /* Channel RSSI, payload size, 18-23 useful chars */
            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, ",\"rssi\":%.0f,\"size\":%u", roundf(p->rssic), p->size);
            if (j > 0) {
                buff_index += j;
            } else {
                MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 4));
                exit(EXIT_FAILURE);
            }

            /* Packet base64-encoded payload, 14-350 useful chars */
            memcpy((void *)(buff_up + buff_index), (void *)",\"data\":\"", 9);
            buff_index += 9;
            j = bin_to_b64(p->payload, p->size, (char *)(buff_up + buff_index), 341); /* 255 bytes = 340 chars in b64 + null char */
            if (j >= 0) {
                buff_index += j;
            } else {
                MSG("ERROR: [up] bin_to_b64 failed line %d\n", (__LINE__ - 5));
                exit(EXIT_FAILURE);
            }
            buff_up[buff_index] = '"';
            ++buff_index;

            /* End of packet serialization */
            buff_up[buff_index] = '}';
            ++buff_index;
            ++pkt_in_dgram;

            if (p->modulation == MOD_LORA) {
                /* Log nb of packets per channel, per SF */
                nb_pkt_log[p->if_chain][p->datarate - 5] += 1;
                nb_pkt_received_lora += 1;

                /* Log nb of packets for ref_payload (DEBUG) */
                for (k = 0; k < debugconf.nb_ref_payload; k++) {
                    if ((p->payload[0] == (uint8_t)(debugconf.ref_payload[k].id >> 24)) &&
                            (p->payload[1] == (uint8_t)(debugconf.ref_payload[k].id >> 16)) &&
                            (p->payload[2] == (uint8_t)(debugconf.ref_payload[k].id >> 8))  &&
                            (p->payload[3] == (uint8_t)(debugconf.ref_payload[k].id >> 0))) {
                        nb_pkt_received_ref[k] += 1;
                    }
                }
            } else if (p->modulation == MOD_FSK) {
                nb_pkt_log[p->if_chain][0] += 1;
                nb_pkt_received_fsk += 1;
            }
        }


        /* DEBUG: print the number of packets received per channel and per SF */
        {
            int l, m;
            MSG_PRINTF(DEBUG_PKT_FWD, "\n");
            for (l = 0; l < (LGW_IF_CHAIN_NB - 1); l++) {
                MSG_PRINTF(DEBUG_PKT_FWD, "CH%d: ", l);
                for (m = 0; m < 8; m++) {
                    MSG_PRINTF(DEBUG_PKT_FWD, "\t%lu", nb_pkt_log[l][m]);
                }
                MSG_PRINTF(DEBUG_PKT_FWD, "\n");
            }
            MSG_PRINTF(DEBUG_PKT_FWD, "FSK: \t%lu", nb_pkt_log[9][0]);
            MSG_PRINTF(DEBUG_PKT_FWD, "\n");
            MSG_PRINTF(DEBUG_PKT_FWD, "Total number of LoRa packet received: %lu\n", nb_pkt_received_lora);
            MSG_PRINTF(DEBUG_PKT_FWD, "Total number of FSK packet received: %lu\n", nb_pkt_received_fsk);
            for (l = 0; l < debugconf.nb_ref_payload; l++) {
                MSG_PRINTF(DEBUG_PKT_FWD, "Total number of LoRa packet received from 0x%08lX: %lu\n", debugconf.ref_payload[l].id, nb_pkt_received_ref[l]);
            }
        }

        /* restart fetch sequence without sending empty JSON if all packets have been filtered out */
        if (pkt_in_dgram == 0) {
            if (send_report == true) {
                /* need to clean up the beginning of the payload */
                buff_index -= 8; /* removes "rxpk":[ */
            } else {
                /* all packet have been filtered out and no report, restart loop */
                continue;
            }
        } else {
            /* end of packet array */
            buff_up[buff_index] = ']';
            ++buff_index;
            /* add separator if needed */
            if (send_report == true) {
                buff_up[buff_index] = ',';
                ++buff_index;
            }
        }

        /* add status report if a new one is available */
        if (send_report == true) {
            xSemaphoreTake(mx_stat_rep, portMAX_DELAY);
            report_ready = false;
            j = snprintf((char *)(buff_up + buff_index), TX_BUFF_SIZE - buff_index, "%s", status_report);
            xSemaphoreGive(mx_stat_rep);
            if (j > 0) {
                buff_index += j;
            } else {
                MSG("ERROR: [up] snprintf failed line %d\n", (__LINE__ - 5));
                exit(EXIT_FAILURE);
            }
        }

        /* end of JSON datagram payload */
        buff_up[buff_index] = '}';
        ++buff_index;
        buff_up[buff_index] = 0; /* add string terminator, for safety */

        printf("\nJSON up: %s\n", (char *)(buff_up + 12)); /* DEBUG: display JSON payload */

        /* send datagram to server */
        //send(sock_up, (void *)buff_up, buff_index, 0);
        sendto(sock_up, (void *)buff_up, buff_index, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        clock_gettime(CLOCK_MONOTONIC, &send_time);
        xSemaphoreTake(mx_meas_up, portMAX_DELAY);
        meas_up_dgram_sent += 1;
        meas_up_network_byte += buff_index;

        /* wait for acknowledge (in 2 times, to catch extra packets) */
        socklen_t socklen = sizeof(source_addr);
        for (i = 0; i < 2; ++i) {
            //j = recv(sock_up, (void *)buff_ack, sizeof buff_ack, 0);
            j = recvfrom(sock_up, (void *)buff_ack, sizeof buff_ack, 0, (struct sockaddr *)&dest_addr, &socklen);
            clock_gettime(CLOCK_MONOTONIC, &recv_time);
            if (j == -1) {
                if (errno == EAGAIN) { /* timeout */
                    continue;
                } else { /* server connection error */
                    break;
                }
            } else if ((j < 4) || (buff_ack[0] != PROTOCOL_VERSION) || (buff_ack[3] != PKT_PUSH_ACK)) {
                MSG("WARNING: [up] ignored invalid non-ACL packet\n");
                continue;
            } else if ((buff_ack[1] != token_h) || (buff_ack[2] != token_l)) {
                MSG("WARNING: [up] ignored out-of sync ACK packet,token[0x%02x:0x%02x],recv[0x%02x:0x%02x]\n",
                    token_h, token_l, buff_ack[1], buff_ack[2]);
                continue;
            } else {
                MSG("INFO: [up] PUSH_ACK received in %i ms\n", (int)(1000 * difftimespec(recv_time, send_time)));
                meas_up_ack_rcv += 1;
                vBackhaulFlash( 10 );
                break;
            }
        }
        xSemaphoreGive(mx_meas_up);
    }
    MSG("\nINFO: End of upstream thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 2: POLLING SERVER AND ENQUEUING PACKETS IN JIT QUEUE ---------- */

static int get_tx_gain_lut_index(uint8_t rf_chain, int8_t rf_power, uint8_t *lut_index)
{
    uint8_t pow_index;
    int current_best_index = -1;
    uint8_t current_best_match = 0xFF;
    int diff;

    /* Check input parameters */
    if (lut_index == NULL) {
        MSG("ERROR: %s - wrong parameter\n", __FUNCTION__);
        return -1;
    }

    /* Search requested power in TX gain LUT */
    for (pow_index = 0; pow_index < txlut[rf_chain].size; pow_index++) {
        diff = rf_power - txlut[rf_chain].lut[pow_index].rf_power;
        if (diff < 0) {
            /* The selected power must be lower or equal to requested one */
            continue;
        } else {
            /* Record the index corresponding to the closest rf_power available in LUT */
            if ((current_best_index == -1) || (diff < current_best_match)) {
                current_best_match = diff;
                current_best_index = pow_index;
            }
        }
    }

    /* Return corresponding index */
    if (current_best_index > -1) {
        *lut_index = (uint8_t)current_best_index;
    } else {
        *lut_index = 0;
        MSG("ERROR: %s - failed to find tx gain lut index\n", __FUNCTION__);
        return -1;
    }

    return 0;
}

uint8_t buff_down[1000]; /* buffer to receive downstream packets */
void thread_down(void)
{
    int i; /* loop variables */

    /* configuration and metadata for an outbound packet */
    struct lgw_pkt_tx_s txpkt;
    bool sent_immediate = false; /* option to sent the packet immediately */

    /* local timekeeping variables */
    struct timespec send_time; /* time of the pull request */
    struct timespec recv_time; /* time of return from recv socket call */

    /* data buffers */
    uint8_t buff_req[12]; /* buffer to compose pull requests */
    int msg_len;

    /* protocol variables */
    uint8_t token_h; /* random token for acknowledgement matching */
    uint8_t token_l; /* random token for acknowledgement matching */
    bool req_ack = false; /* keep track of whether PULL_DATA was acknowledged or not */

    /* JSON parsing variables */
    JSON_Value *root_val = NULL;
    JSON_Object *txpk_obj = NULL;
    JSON_Value *val = NULL; /* needed to detect the absence of some fields */
    const char *str; /* pointer to sub-strings in the JSON data */
    short x0, x1;
    uint64_t x2;
    double x3, x4;

    /* variables to send on GPS timestamp */
    struct tref local_ref; /* time reference used for GPS <-> timestamp conversion */
    struct timespec gps_tx; /* GPS time that needs to be converted to timestamp */

    /* beacon variables */
    struct lgw_pkt_tx_s beacon_pkt;
    uint8_t beacon_chan;
    uint8_t beacon_loop;
    size_t beacon_RFU1_size = 0;
    size_t beacon_RFU2_size = 0;
    uint8_t beacon_pyld_idx = 0;
    time_t diff_beacon_time;
    struct timespec next_beacon_gps_time; /* gps time of next beacon packet */
    struct timespec last_beacon_gps_time; /* gps time of last enqueued beacon packet */
    int retry;

    /* beacon data fields, byte 0 is Least Significant Byte */
    int32_t field_latitude; /* 3 bytes, derived from reference latitude */
    int32_t field_longitude; /* 3 bytes, derived from reference longitude */
    uint16_t field_crc1, field_crc2;

    /* auto-quit variable */
    uint32_t autoquit_cnt = 0; /* count the number of PULL_DATA sent since the latest PULL_ACK */

    /* Just In Time downlink */
    uint32_t current_concentrator_time;
    enum jit_error_e jit_result = JIT_ERROR_OK;
    enum jit_pkt_type_e downlink_type;
    enum jit_error_e warning_result = JIT_ERROR_OK;
    int32_t warning_value = 0;
    uint8_t tx_lut_idx = 0;

    /* set downstream socket RX timeout */
    i = setsockopt(sock_down, SOL_SOCKET, SO_RCVTIMEO, (void *)&pull_timeout, sizeof pull_timeout);
    if (i != 0) {
        MSG("ERROR: [down] setsockopt returned %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* pre-fill the pull request buffer with fixed fields */
    buff_req[0] = PROTOCOL_VERSION;
    buff_req[3] = PKT_PULL_DATA;
    *(uint32_t *)(buff_req + 4) = net_mac_h;
    *(uint32_t *)(buff_req + 8) = net_mac_l;

    /* beacon variables initialization */
    last_beacon_gps_time.tv_sec = 0;
    last_beacon_gps_time.tv_nsec = 0;

    /* beacon packet parameters */
    beacon_pkt.tx_mode = ON_GPS; /* send on PPS pulse */
    beacon_pkt.rf_chain = 0; /* antenna A */
    beacon_pkt.rf_power = beacon_power;
    beacon_pkt.modulation = MOD_LORA;
    switch (beacon_bw_hz) {
    case 125000:
        beacon_pkt.bandwidth = BW_125KHZ;
        break;
    case 500000:
        beacon_pkt.bandwidth = BW_500KHZ;
        break;
    default:
        /* should not happen */
        MSG("ERROR: unsupported bandwidth for beacon\n");
        exit(EXIT_FAILURE);
    }
    switch (beacon_datarate) {
    case 8:
        beacon_pkt.datarate = DR_LORA_SF8;
        beacon_RFU1_size = 1;
        beacon_RFU2_size = 3;
        break;
    case 9:
        beacon_pkt.datarate = DR_LORA_SF9;
        beacon_RFU1_size = 2;
        beacon_RFU2_size = 0;
        break;
    case 10:
        beacon_pkt.datarate = DR_LORA_SF10;
        beacon_RFU1_size = 3;
        beacon_RFU2_size = 1;
        break;
    case 12:
        beacon_pkt.datarate = DR_LORA_SF12;
        beacon_RFU1_size = 5;
        beacon_RFU2_size = 3;
        break;
    default:
        /* should not happen */
        MSG("ERROR: unsupported datarate for beacon\n");
        exit(EXIT_FAILURE);
    }
    beacon_pkt.size = beacon_RFU1_size + 4 + 2 + 7 + beacon_RFU2_size + 2;
    beacon_pkt.coderate = CR_LORA_4_5;
    beacon_pkt.invert_pol = false;
    beacon_pkt.preamble = 10;
    beacon_pkt.no_crc = true;
    beacon_pkt.no_header = true;

    /* network common part beacon fields (little endian) */
    for (i = 0; i < (int)beacon_RFU1_size; i++) {
        beacon_pkt.payload[beacon_pyld_idx++] = 0x0;
    }

    /* network common part beacon fields (little endian) */
    beacon_pyld_idx += 4; /* time (variable), filled later */
    beacon_pyld_idx += 2; /* crc1 (variable), filled later */

    /* calculate the latitude and longitude that must be publicly reported */
    field_latitude = (int32_t)((reference_coord.lat / 90.0) * (double)(1 << 23));
    if (field_latitude > (int32_t)0x007FFFFF) {
        field_latitude = (int32_t)0x007FFFFF; /* +90 N is represented as 89.99999 N */
    } else if (field_latitude < (int32_t)0xFF800000) {
        field_latitude = (int32_t)0xFF800000;
    }
    field_longitude = (int32_t)((reference_coord.lon / 180.0) * (double)(1 << 23));
    if (field_longitude > (int32_t)0x007FFFFF) {
        field_longitude = (int32_t)0x007FFFFF; /* +180 E is represented as 179.99999 E */
    } else if (field_longitude < (int32_t)0xFF800000) {
        field_longitude = (int32_t)0xFF800000;
    }

    /* gateway specific beacon fields */
    beacon_pkt.payload[beacon_pyld_idx++] = beacon_infodesc;
    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  field_latitude;
    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_latitude >>  8);
    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_latitude >> 16);
    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  field_longitude;
    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_longitude >>  8);
    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_longitude >> 16);

    /* RFU */
    for (i = 0; i < (int)beacon_RFU2_size; i++) {
        beacon_pkt.payload[beacon_pyld_idx++] = 0x0;
    }

    /* CRC of the beacon gateway specific part fields */
    field_crc2 = crc16((beacon_pkt.payload + 6 + beacon_RFU1_size), 7 + beacon_RFU2_size);
    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  field_crc2;
    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_crc2 >> 8);

    /* JIT queue initialization */
    jit_queue_init(&jit_queue[0]);
    jit_queue_init(&jit_queue[1]);

    while (!exit_sig && !quit_sig) {

        /* auto-quit if the threshold is crossed */
        if ((autoquit_threshold > 0) && (autoquit_cnt >= autoquit_threshold)) {
            exit_sig = true;
            MSG("INFO: [down] the last %lu PULL_DATA were not ACKed, exiting application\n", autoquit_threshold);
            break;
        }

        /* generate random token for request */
        token_h = (uint8_t)rand(); /* random token */
        token_l = (uint8_t)rand(); /* random token */
        buff_req[1] = token_h;
        buff_req[2] = token_l;

        /* send PULL request and record time */
        //send(sock_down, (void *)buff_req, sizeof buff_req, 0);
        sendto(sock_down, (void *)buff_req, sizeof buff_req, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        clock_gettime(CLOCK_MONOTONIC, &send_time);
        xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
        meas_dw_pull_sent += 1;
        xSemaphoreGive(mx_meas_dw);
        req_ack = false;
        autoquit_cnt++;

        /* listen to packets and process them until a new PULL request must be sent */
        recv_time = send_time;
        socklen_t socklen = sizeof(source_addr);
        while (((int)difftimespec(recv_time, send_time) < keepalive_time) && !exit_sig && !quit_sig) {

            /* try to receive a datagram */
            //msg_len = recv(sock_down, (void *)buff_down, (sizeof buff_down)-1, 0);
            msg_len = recvfrom(sock_down, (void *)buff_down, (sizeof buff_down) - 1, 0, (struct sockaddr *)&dest_addr, &socklen);
            clock_gettime(CLOCK_MONOTONIC, &recv_time);

            /* Pre-allocate beacon slots in JiT queue, to check downlink collisions */
            beacon_loop = JIT_NUM_BEACON_IN_QUEUE - jit_queue[0].num_beacon;
            retry = 0;
            while (beacon_loop && (beacon_period != 0)) {
                xSemaphoreTake(mx_timeref, portMAX_DELAY);
                /* Wait for GPS to be ready before inserting beacons in JiT queue */
                if ((gps_ref_valid == true) && (xtal_correct_ok == true)) {

                    /* compute GPS time for next beacon to come      */
                    /*   LoRaWAN: T = k*beacon_period + TBeaconDelay */
                    /*            with TBeaconDelay = [1.5ms +/- 1µs]*/
                    if (last_beacon_gps_time.tv_sec == 0) {
                        /* if no beacon has been queued, get next slot from current GPS time */
                        diff_beacon_time = time_reference_gps.gps.tv_sec % ((time_t)beacon_period);
                        next_beacon_gps_time.tv_sec = time_reference_gps.gps.tv_sec +
                                                      ((time_t)beacon_period - diff_beacon_time);
                    } else {
                        /* if there is already a beacon, take it as reference */
                        next_beacon_gps_time.tv_sec = last_beacon_gps_time.tv_sec + beacon_period;
                    }
                    /* now we can add a beacon_period to the reference to get next beacon GPS time */
                    next_beacon_gps_time.tv_sec += (retry * beacon_period);
                    next_beacon_gps_time.tv_nsec = 0;

#if DEBUG_BEACON
                    {
                        time_t time_unix;

                        time_unix = time_reference_gps.gps.tv_sec + UNIX_GPS_EPOCH_OFFSET;
                        MSG_DEBUG(DEBUG_BEACON, "GPS-now : %s", ctime(&time_unix));
                        time_unix = last_beacon_gps_time.tv_sec + UNIX_GPS_EPOCH_OFFSET;
                        MSG_DEBUG(DEBUG_BEACON, "GPS-last: %s", ctime(&time_unix));
                        time_unix = next_beacon_gps_time.tv_sec + UNIX_GPS_EPOCH_OFFSET;
                        MSG_DEBUG(DEBUG_BEACON, "GPS-next: %s", ctime(&time_unix));
                    }
#endif

                    /* convert GPS time to concentrator time, and set packet counter for JiT trigger */
                    lgw_gps2cnt(time_reference_gps, next_beacon_gps_time, &(beacon_pkt.count_us));
                    xSemaphoreGive(mx_timeref);

                    /* apply frequency correction to beacon TX frequency */
                    if (beacon_freq_nb > 1) {
                        beacon_chan = (next_beacon_gps_time.tv_sec / beacon_period) % beacon_freq_nb; /* floor rounding */
                    } else {
                        beacon_chan = 0;
                    }
                    /* Compute beacon frequency */
                    beacon_pkt.freq_hz = beacon_freq_hz + (beacon_chan * beacon_freq_step);

                    /* load time in beacon payload */
                    beacon_pyld_idx = beacon_RFU1_size;
                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF &  next_beacon_gps_time.tv_sec;
                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (next_beacon_gps_time.tv_sec >>  8);
                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (next_beacon_gps_time.tv_sec >> 16);
                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (next_beacon_gps_time.tv_sec >> 24);

                    /* calculate CRC */
                    field_crc1 = crc16(beacon_pkt.payload, 4 + beacon_RFU1_size); /* CRC for the network common part */
                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & field_crc1;
                    beacon_pkt.payload[beacon_pyld_idx++] = 0xFF & (field_crc1 >> 8);

                    /* Insert beacon packet in JiT queue */
                    xSemaphoreTake(mx_concent, portMAX_DELAY);
                    lgw_get_instcnt(&current_concentrator_time);
                    xSemaphoreGive(mx_concent);
                    jit_result = jit_enqueue(&jit_queue[0], current_concentrator_time, &beacon_pkt, JIT_PKT_TYPE_BEACON);
                    if (jit_result == JIT_ERROR_OK) {
                        /* update stats */
                        xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
                        meas_nb_beacon_queued += 1;
                        xSemaphoreGive(mx_meas_dw);

                        /* One more beacon in the queue */
                        beacon_loop--;
                        retry = 0;
                        last_beacon_gps_time.tv_sec = next_beacon_gps_time.tv_sec; /* keep this beacon time as reference for next one to be programmed */

                        /* display beacon payload */
                        MSG("INFO: Beacon queued (count_us=%lu, freq_hz=%lu, size=%u):\n", beacon_pkt.count_us, beacon_pkt.freq_hz, beacon_pkt.size);
                        printf( "   => " );
                        for (i = 0; i < beacon_pkt.size; ++i) {
                            MSG("%02X ", beacon_pkt.payload[i]);
                        }
                        MSG("\n");
                    } else {
                        MSG_DEBUG(DEBUG_BEACON, "--> beacon queuing failed with %d\n", jit_result);
                        /* update stats */
                        xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
                        if (jit_result != JIT_ERROR_COLLISION_BEACON) {
                            meas_nb_beacon_rejected += 1;
                        }
                        xSemaphoreGive(mx_meas_dw);
                        /* In case previous enqueue failed, we retry one period later until it succeeds */
                        /* Note: In case the GPS has been unlocked for a while, there can be lots of retries */
                        /*       to be done from last beacon time to a new valid one */
                        retry++;
                        MSG_DEBUG(DEBUG_BEACON, "--> beacon queuing retry=%d\n", retry);
                    }
                } else {
                    xSemaphoreGive(mx_timeref);
                    break;
                }
            }

            /* if no network message was received, got back to listening sock_down socket */
            if (msg_len == -1) {
                //MSG("WARNING: [down] recv returned %s\n", strerror(errno)); /* too verbose */
                continue;
            }

            /* program coming here means a datagram received */
            vBackhaulFlash( 10 );

            /* if the datagram does not respect protocol, just ignore it */
            if ((msg_len < 4) || (buff_down[0] != PROTOCOL_VERSION) || ((buff_down[3] != PKT_PULL_RESP) && (buff_down[3] != PKT_PULL_ACK))) {
                MSG("WARNING: [down] ignoring invalid packet len=%d, protocol_version=%d, id=%d\n",
                    msg_len, buff_down[0], buff_down[3]);
                continue;
            }

            /* if the datagram is an ACK, check token */
            if (buff_down[3] == PKT_PULL_ACK) {
                if ((buff_down[1] == token_h) && (buff_down[2] == token_l)) {
                    if (req_ack) {
                        MSG("INFO: [down] duplicate ACK received :)\n");
                    } else { /* if that packet was not already acknowledged */
                        req_ack = true;
                        autoquit_cnt = 0;
                        xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
                        meas_dw_ack_rcv += 1;
                        xSemaphoreGive(mx_meas_dw);
                        MSG("INFO: [down] PULL_ACK received in %i ms\n", (int)(1000 * difftimespec(recv_time, send_time)));
                    }
                } else { /* out-of-sync token */
                    MSG("INFO: [down] received out-of-sync ACK\n");
                }
                continue;
            }

            /* the datagram is a PULL_RESP */
            buff_down[msg_len] = 0; /* add string terminator, just to be safe */
            MSG("INFO: [down] PULL_RESP received  - token[%d:%d] :)\n", buff_down[1], buff_down[2]); /* very verbose */
            printf("\nJSON down: %s\n", (char *)(buff_down + 4)); /* DEBUG: display JSON payload */

            /* initialize TX struct and try to parse JSON */
            memset(&txpkt, 0, sizeof txpkt);
            root_val = json_parse_string_with_comments((const char *)(buff_down + 4)); /* JSON offset */
            if (root_val == NULL) {
                MSG("WARNING: [down] invalid JSON, TX aborted\n");
                continue;
            }

            /* look for JSON sub-object 'txpk' */
            txpk_obj = json_object_get_object(json_value_get_object(root_val), "txpk");
            if (txpk_obj == NULL) {
                MSG("WARNING: [down] no \"txpk\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }

            /* Parse "immediate" tag, or target timestamp, or UTC time to be converted by GPS (mandatory) */
            i = json_object_get_boolean(txpk_obj, "imme"); /* can be 1 if true, 0 if false, or -1 if not a JSON boolean */
            if (i == 1) {
                /* TX procedure: send immediately */
                sent_immediate = true;
                downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_C;
                MSG("INFO: [down] a packet will be sent in \"immediate\" mode\n");
            } else {
                sent_immediate = false;
                val = json_object_get_value(txpk_obj, "tmst");
                if (val != NULL) {
                    /* TX procedure: send on timestamp value */
                    txpkt.count_us = (uint32_t)json_value_get_number(val);

                    /* Concentrator timestamp is given, we consider it is a Class A downlink */
                    downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_A;
                } else {
                    /* TX procedure: send on GPS time (converted to timestamp value) */
                    val = json_object_get_value(txpk_obj, "tmms");
                    if (val == NULL) {
                        MSG("WARNING: [down] no mandatory \"txpk.tmst\" or \"txpk.tmms\" objects in JSON, TX aborted\n");
                        json_value_free(root_val);
                        continue;
                    }
                    if (gps_enabled == true) {
                        xSemaphoreTake(mx_timeref, portMAX_DELAY);
                        if (gps_ref_valid == true) {
                            local_ref = time_reference_gps;
                            xSemaphoreGive(mx_timeref);
                        } else {
                            xSemaphoreGive(mx_timeref);
                            MSG("WARNING: [down] no valid GPS time reference yet, impossible to send packet on specific GPS time, TX aborted\n");
                            json_value_free(root_val);

                            /* send acknoledge datagram to server */
                            send_tx_ack(buff_down[1], buff_down[2], JIT_ERROR_GPS_UNLOCKED, 0);
                            continue;
                        }
                    } else {
                        MSG("WARNING: [down] GPS disabled, impossible to send packet on specific GPS time, TX aborted\n");
                        json_value_free(root_val);

                        /* send acknoledge datagram to server */
                        send_tx_ack(buff_down[1], buff_down[2], JIT_ERROR_GPS_UNLOCKED, 0);
                        continue;
                    }

                    /* Get GPS time from JSON */
                    x2 = (uint64_t)json_value_get_number(val);

                    /* Convert GPS time from milliseconds to timespec */
                    x3 = modf((double)x2 / 1E3, &x4);
                    gps_tx.tv_sec = (time_t)x4; /* get seconds from integer part */
                    gps_tx.tv_nsec = (long)(x3 * 1E9); /* get nanoseconds from fractional part */

                    /* transform GPS time to timestamp */
                    i = lgw_gps2cnt(local_ref, gps_tx, &(txpkt.count_us));
                    if (i != LGW_GPS_SUCCESS) {
                        MSG("WARNING: [down] could not convert GPS time to timestamp, TX aborted\n");
                        json_value_free(root_val);
                        continue;
                    } else {
                        MSG("INFO: [down] a packet will be sent on timestamp value %lu (calculated from GPS time)\n", txpkt.count_us);
                    }

                    /* GPS timestamp is given, we consider it is a Class B downlink */
                    downlink_type = JIT_PKT_TYPE_DOWNLINK_CLASS_B;
                }
            }

            /* Parse "No CRC" flag (optional field) */
            val = json_object_get_value(txpk_obj, "ncrc");
            if (val != NULL) {
                txpkt.no_crc = (bool)json_value_get_boolean(val);
            }

            /* Parse "No header" flag (optional field) */
            val = json_object_get_value(txpk_obj, "nhdr");
            if (val != NULL) {
                txpkt.no_header = (bool)json_value_get_boolean(val);
            }

            /* parse target frequency (mandatory) */
            val = json_object_get_value(txpk_obj, "freq");
            if (val == NULL) {
                MSG("WARNING: [down] no mandatory \"txpk.freq\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            txpkt.freq_hz = (uint32_t)((double)(1.0e6) * json_value_get_number(val));

            /* parse RF chain used for TX (mandatory) */
            val = json_object_get_value(txpk_obj, "rfch");
            if (val == NULL) {
                MSG("WARNING: [down] no mandatory \"txpk.rfch\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            txpkt.rf_chain = (uint8_t)json_value_get_number(val);
            if (tx_enable[txpkt.rf_chain] == false) {
                MSG("WARNING: [down] TX is not enabled on RF chain %u, TX aborted\n", txpkt.rf_chain);
                json_value_free(root_val);
                continue;
            }

            /* parse TX power (optional field) */
            val = json_object_get_value(txpk_obj, "powe");
            if (val != NULL) {
                txpkt.rf_power = (int8_t)json_value_get_number(val) - antenna_gain;
            }

            /* Parse modulation (mandatory) */
            str = json_object_get_string(txpk_obj, "modu");
            if (str == NULL) {
                MSG("WARNING: [down] no mandatory \"txpk.modu\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            if (strcmp(str, "LORA") == 0) {
                /* Lora modulation */
                txpkt.modulation = MOD_LORA;

                /* Parse Lora spreading-factor and modulation bandwidth (mandatory) */
                str = json_object_get_string(txpk_obj, "datr");
                if (str == NULL) {
                    MSG("WARNING: [down] no mandatory \"txpk.datr\" object in JSON, TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }
                i = sscanf(str, "SF%2hdBW%3hd", &x0, &x1);
                if (i != 2) {
                    MSG("WARNING: [down] format error in \"txpk.datr\", TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }
                switch (x0) {
                case  5: txpkt.datarate = DR_LORA_SF5;  break;
                case  6: txpkt.datarate = DR_LORA_SF6;  break;
                case  7: txpkt.datarate = DR_LORA_SF7;  break;
                case  8: txpkt.datarate = DR_LORA_SF8;  break;
                case  9: txpkt.datarate = DR_LORA_SF9;  break;
                case 10: txpkt.datarate = DR_LORA_SF10; break;
                case 11: txpkt.datarate = DR_LORA_SF11; break;
                case 12: txpkt.datarate = DR_LORA_SF12; break;
                default:
                    MSG("WARNING: [down] format error in \"txpk.datr\", invalid SF, TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }
                switch (x1) {
                case 125: txpkt.bandwidth = BW_125KHZ; break;
                case 250: txpkt.bandwidth = BW_250KHZ; break;
                case 500: txpkt.bandwidth = BW_500KHZ; break;
                default:
                    MSG("WARNING: [down] format error in \"txpk.datr\", invalid BW, TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }

                /* Parse ECC coding rate (optional field) */
                str = json_object_get_string(txpk_obj, "codr");
                if (str == NULL) {
                    MSG("WARNING: [down] no mandatory \"txpk.codr\" object in json, TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }
                if      (strcmp(str, "4/5") == 0) txpkt.coderate = CR_LORA_4_5;
                else if (strcmp(str, "4/6") == 0) txpkt.coderate = CR_LORA_4_6;
                else if (strcmp(str, "2/3") == 0) txpkt.coderate = CR_LORA_4_6;
                else if (strcmp(str, "4/7") == 0) txpkt.coderate = CR_LORA_4_7;
                else if (strcmp(str, "4/8") == 0) txpkt.coderate = CR_LORA_4_8;
                else if (strcmp(str, "1/2") == 0) txpkt.coderate = CR_LORA_4_8;
                else {
                    MSG("WARNING: [down] format error in \"txpk.codr\", TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }

                /* Parse signal polarity switch (optional field) */
                val = json_object_get_value(txpk_obj, "ipol");
                if (val != NULL) {
                    txpkt.invert_pol = (bool)json_value_get_boolean(val);
                }

                /* parse Lora preamble length (optional field, optimum min value enforced) */
                val = json_object_get_value(txpk_obj, "prea");
                if (val != NULL) {
                    i = (int)json_value_get_number(val);
                    if (i >= MIN_LORA_PREAMB) {
                        txpkt.preamble = (uint16_t)i;
                    } else {
                        txpkt.preamble = (uint16_t)MIN_LORA_PREAMB;
                    }
                } else {
                    txpkt.preamble = (uint16_t)STD_LORA_PREAMB;
                }

            } else if (strcmp(str, "FSK") == 0) {
                /* FSK modulation */
                txpkt.modulation = MOD_FSK;

                /* parse FSK bitrate (mandatory) */
                val = json_object_get_value(txpk_obj, "datr");
                if (val == NULL) {
                    MSG("WARNING: [down] no mandatory \"txpk.datr\" object in JSON, TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }
                txpkt.datarate = (uint32_t)(json_value_get_number(val));

                /* parse frequency deviation (mandatory) */
                val = json_object_get_value(txpk_obj, "fdev");
                if (val == NULL) {
                    MSG("WARNING: [down] no mandatory \"txpk.fdev\" object in JSON, TX aborted\n");
                    json_value_free(root_val);
                    continue;
                }
                txpkt.f_dev = (uint8_t)(json_value_get_number(val) / 1000.0); /* JSON value in Hz, txpkt.f_dev in kHz */

                /* parse FSK preamble length (optional field, optimum min value enforced) */
                val = json_object_get_value(txpk_obj, "prea");
                if (val != NULL) {
                    i = (int)json_value_get_number(val);
                    if (i >= MIN_FSK_PREAMB) {
                        txpkt.preamble = (uint16_t)i;
                    } else {
                        txpkt.preamble = (uint16_t)MIN_FSK_PREAMB;
                    }
                } else {
                    txpkt.preamble = (uint16_t)STD_FSK_PREAMB;
                }

            } else {
                MSG("WARNING: [down] invalid modulation in \"txpk.modu\", TX aborted\n");
                json_value_free(root_val);
                continue;
            }

            /* Parse payload length (mandatory) */
            val = json_object_get_value(txpk_obj, "size");
            if (val == NULL) {
                MSG("WARNING: [down] no mandatory \"txpk.size\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            txpkt.size = (uint16_t)json_value_get_number(val);

            /* Parse payload data (mandatory) */
            str = json_object_get_string(txpk_obj, "data");
            if (str == NULL) {
                MSG("WARNING: [down] no mandatory \"txpk.data\" object in JSON, TX aborted\n");
                json_value_free(root_val);
                continue;
            }
            i = b64_to_bin(str, strlen(str), txpkt.payload, sizeof txpkt.payload);
            if (i != txpkt.size) {
                MSG("WARNING: [down] mismatch between .size and .data size once converter to binary\n");
            }

            /* free the JSON parse tree from memory */
            json_value_free(root_val);

            /* select TX mode */
            if (sent_immediate) {
                txpkt.tx_mode = IMMEDIATE;
            } else {
                txpkt.tx_mode = TIMESTAMPED;
            }

            /* record measurement data */
            xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
            meas_dw_dgram_rcv += 1; /* count only datagrams with no JSON errors */
            meas_dw_network_byte += msg_len; /* meas_dw_network_byte */
            meas_dw_payload_byte += txpkt.size;
            xSemaphoreGive(mx_meas_dw);

            /* reset error/warning results */
            jit_result = warning_result = JIT_ERROR_OK;
            warning_value = 0;

            /* check TX frequency before trying to queue packet */
            if ((txpkt.freq_hz < tx_freq_min[txpkt.rf_chain]) || (txpkt.freq_hz > tx_freq_max[txpkt.rf_chain])) {
                jit_result = JIT_ERROR_TX_FREQ;
                MSG("ERROR: Packet REJECTED, unsupported frequency - %lu (min:%lu,max:%lu)\n", txpkt.freq_hz, tx_freq_min[txpkt.rf_chain], tx_freq_max[txpkt.rf_chain]);
            }

            /* check TX power before trying to queue packet, send a warning if not supported */
            if (jit_result == JIT_ERROR_OK) {
                i = get_tx_gain_lut_index(txpkt.rf_chain, txpkt.rf_power, &tx_lut_idx);
                if ((i < 0) || (txlut[txpkt.rf_chain].lut[tx_lut_idx].rf_power != txpkt.rf_power)) {
                    /* this RF power is not supported, throw a warning, and use the closest lower power supported */
                    warning_result = JIT_ERROR_TX_POWER;
                    warning_value = (int32_t)txlut[txpkt.rf_chain].lut[tx_lut_idx].rf_power;
                    printf("WARNING: Requested TX power is not supported (%ddBm), actual power used: %lddBm\n", txpkt.rf_power, warning_value);
                    txpkt.rf_power = txlut[txpkt.rf_chain].lut[tx_lut_idx].rf_power;
                }
            }

            /* insert packet to be sent into JIT queue */
            if (jit_result == JIT_ERROR_OK) {
                xSemaphoreTake(mx_concent, portMAX_DELAY);
                lgw_get_instcnt(&current_concentrator_time);
                xSemaphoreGive(mx_concent);
                jit_result = jit_enqueue(&jit_queue[txpkt.rf_chain], current_concentrator_time, &txpkt, downlink_type);
                if (jit_result != JIT_ERROR_OK) {
                    printf("ERROR: Packet REJECTED (jit error=%d)\n", jit_result);
                } else {
                    /* In case of a warning having been raised before, we notify it */
                    jit_result = warning_result;
                }
                xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
                meas_nb_tx_requested += 1;
                xSemaphoreGive(mx_meas_dw);
            }

            /* Send acknoledge datagram to server */
            send_tx_ack(buff_down[1], buff_down[2], jit_result, warning_value);
        }
    }
    MSG("\nINFO: End of downstream thread\n");
}

void print_tx_status(uint8_t tx_status)
{
    switch (tx_status) {
    case TX_OFF:
        MSG("INFO: [jit] lgw_status returned TX_OFF\n");
        break;
    case TX_FREE:
        MSG("INFO: [jit] lgw_status returned TX_FREE\n");
        break;
    case TX_EMITTING:
        MSG("INFO: [jit] lgw_status returned TX_EMITTING\n");
        break;
    case TX_SCHEDULED:
        MSG("INFO: [jit] lgw_status returned TX_SCHEDULED\n");
        break;
    default:
        MSG("INFO: [jit] lgw_status returned UNKNOWN (%d)\n", tx_status);
        break;
    }
}


/* -------------------------------------------------------------------------- */
/* --- THREAD 3: CHECKING PACKETS TO BE SENT FROM JIT QUEUE AND SEND THEM --- */

void thread_jit(void)
{
    int result = LGW_HAL_SUCCESS;
    struct lgw_pkt_tx_s pkt;
    int pkt_index = -1;
    uint32_t current_concentrator_time;
    enum jit_error_e jit_result;
    enum jit_pkt_type_e pkt_type;
    uint8_t tx_status;
    int i;

    while (!exit_sig && !quit_sig) {
        //wait_ms(10);
        vTaskDelay(10 / portTICK_PERIOD_MS);

        for (i = 0; i < LGW_RF_CHAIN_NB; i++) {
            /* transfer data and metadata to the concentrator, and schedule TX */
            xSemaphoreTake(mx_concent, portMAX_DELAY);
            lgw_get_instcnt(&current_concentrator_time);
            xSemaphoreGive(mx_concent);
            jit_result = jit_peek(&jit_queue[i], current_concentrator_time, &pkt_index);
            if (jit_result == JIT_ERROR_OK) {
                if (pkt_index > -1) {
                    jit_result = jit_dequeue(&jit_queue[i], pkt_index, &pkt, &pkt_type);
                    if (jit_result == JIT_ERROR_OK) {
                        /* update beacon stats */
                        if (pkt_type == JIT_PKT_TYPE_BEACON) {
                            /* Compensate breacon frequency with xtal error */
                            xSemaphoreTake(mx_xcorr, portMAX_DELAY);
                            pkt.freq_hz = (uint32_t)(xtal_correct * (double)pkt.freq_hz);
                            MSG_DEBUG(DEBUG_BEACON, "beacon_pkt.freq_hz=%lu (xtal_correct=%.15lf)\n", pkt.freq_hz, xtal_correct);
                            xSemaphoreGive(mx_xcorr);

                            /* Update statistics */
                            xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
                            meas_nb_beacon_sent += 1;
                            xSemaphoreGive(mx_meas_dw);
                            MSG("INFO: Beacon dequeued (count_us=%lu)\n", pkt.count_us);
                        }

                        /* check if concentrator is free for sending new packet */
                        xSemaphoreTake(mx_concent, portMAX_DELAY); /* may have to wait for a fetch to finish */
                        result = lgw_status(pkt.rf_chain, TX_STATUS, &tx_status);
                        xSemaphoreGive(mx_concent); /* free concentrator ASAP */
                        if (result == LGW_HAL_ERROR) {
                            MSG("WARNING: [jit%d] lgw_status failed\n", i);
                        } else {
                            if (tx_status == TX_EMITTING) {
                                MSG("ERROR: concentrator is currently emitting on rf_chain %d\n", i);
                                print_tx_status(tx_status);
                                continue;
                            } else if (tx_status == TX_SCHEDULED) {
                                MSG("WARNING: a downlink was already scheduled on rf_chain %d, overwritting it...\n", i);
                                print_tx_status(tx_status);
                            } else {
                                /* Nothing to do */
                            }
                        }

                        /* send packet to concentrator */
                        xSemaphoreTake(mx_concent, portMAX_DELAY); /* may have to wait for a fetch to finish */
                        if (spectral_scan_params.enable == true) {
                            result = lgw_spectral_scan_abort();
                            if (result != LGW_HAL_SUCCESS) {
                                MSG("WARNING: [jit%d] lgw_spectral_scan_abort failed\n", i);
                            }
                        }
                        result = lgw_send(&pkt);
                        xSemaphoreGive(mx_concent); /* free concentrator ASAP */
                        if (result != LGW_HAL_SUCCESS) {
                            xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
                            meas_nb_tx_fail += 1;
                            xSemaphoreGive(mx_meas_dw);
                            MSG("WARNING: [jit] lgw_send failed on rf_chain %d\n", i);
                            continue;
                        } else {
                            xSemaphoreTake(mx_meas_dw, portMAX_DELAY);
                            meas_nb_tx_ok += 1;
                            xSemaphoreGive(mx_meas_dw);
                            MSG_DEBUG(DEBUG_PKT_FWD, "lgw_send done on rf_chain %d: count_us=%lu\n", i, pkt.count_us);
                            vDownlinkFlash( 10 );
                        }
                    } else {
                        MSG("ERROR: jit_dequeue failed on rf_chain %d with %d\n", i, jit_result);
                    }
                }
            } else if (jit_result == JIT_ERROR_EMPTY) {
                /* Do nothing, it can happen */
            } else {
                MSG("ERROR: jit_peek failed on rf_chain %d with %d\n", i, jit_result);
            }
        }
    }
    vTaskDelete( pJit );
    MSG("\nINFO: End of JIT thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 4: PARSE GPS MESSAGE AND KEEP GATEWAY IN SYNC ----------------- */

static void gps_process_sync(void)
{
    struct timespec gps_time;
    struct timespec utc;
    uint32_t trig_tstamp; /* concentrator timestamp associated with PPM pulse */
    int i = lgw_gps_get(&utc, &gps_time, NULL, NULL);

    /* get GPS time for synchronization */
    if (i != LGW_GPS_SUCCESS) {
        MSG("WARNING: [gps] could not get GPS time from GPS\n");
        return;
    }

    /* get timestamp captured on PPM pulse  */
    xSemaphoreTake(mx_concent, portMAX_DELAY);
    i = lgw_get_trigcnt(&trig_tstamp);
    xSemaphoreGive(mx_concent);
    if (i != LGW_HAL_SUCCESS) {
        MSG("WARNING: [gps] failed to read concentrator timestamp\n");
        return;
    }

    /* try to update time reference with the new GPS time & timestamp */
    xSemaphoreTake(mx_timeref, portMAX_DELAY);
    i = lgw_gps_sync(&time_reference_gps, trig_tstamp, utc, gps_time);
    xSemaphoreGive(mx_timeref);
    if (i != LGW_GPS_SUCCESS) {
        MSG("WARNING: [gps] GPS out of sync, keeping previous time reference\n");
    }
}

static void gps_process_coords(void)
{
    /* position variable */
    struct coord_s coord;
    struct coord_s gpserr;
    int    i = lgw_gps_get(NULL, NULL, &coord, &gpserr);
    /* update gateway coordinates */
    xSemaphoreTake(mx_meas_gps, portMAX_DELAY);
    if (i == LGW_GPS_SUCCESS) {
        gps_coord_valid = true;
        meas_gps_coord = coord;
        meas_gps_err = gpserr;
        // TODO: report other GPS statistics (typ. signal quality & integrity)
    } else {
        gps_coord_valid = false;
    }
    xSemaphoreGive(mx_meas_gps);
}

void thread_gps(void)
{
    /* serial variables */
    char serial_buff[128]; /* buffer to receive GPS data */
    size_t wr_idx = 0;     /* pointer to end of chars in buffer */

    /* variables for PPM pulse GPS synchronization */
    enum gps_msg latest_msg; /* keep track of latest NMEA message parsed */

    /* initialize some variables before loop */
    memset(serial_buff, 0, sizeof serial_buff);

    while (!exit_sig && !quit_sig) {
        size_t rd_idx = 0;
        size_t frame_end_idx = 0;

        /* blocking non-canonical read on serial port */


        // Read data from GPS UART.

        // uint8_t data[1024];
        // int length, min;
        // ESP_ERROR_CHECK(uart_get_buffered_data_len((uart_port_t)gps_tty_fd, (size_t *)&length));
        // min = (length < 1024) ? length : 1024;
        // length = uart_read_bytes((uart_port_t)gps_tty_fd, data, min, 100);
        // data[min] = '\0';
        // printf("GPS Raw Data -------> length = %d, min = %d:\n%s\n", length, min, data);

        // ssize_t nb_char = read(gps_tty_fd, serial_buff + wr_idx, LGW_GPS_MIN_MSG_SIZE);
        ssize_t  nb_char = uart_read_bytes((uart_port_t)gps_tty_fd, serial_buff + wr_idx, LGW_GPS_MIN_MSG_SIZE, 100);
        // if (nb_char <= 0) {
        //     MSG("WARNING: [gps] read() returned value %zd\n", nb_char);
        //     continue;
        // }
        wr_idx += (size_t)nb_char;

        /*******************************************
         * Scan buffer for UBX/NMEA sync chars and *
         * attempt to decode frame if one is found *
         *******************************************/
        while (rd_idx < wr_idx) {
            size_t frame_size = 0;

            /* Scan buffer for UBX sync char */
            if (serial_buff[rd_idx] == (char)LGW_GPS_UBX_SYNC_CHAR) {

                /***********************
                 * Found UBX sync char *
                 ***********************/
                latest_msg = lgw_parse_ubx(&serial_buff[rd_idx], (wr_idx - rd_idx), &frame_size);

                if (frame_size > 0) {
                    if (latest_msg == INCOMPLETE) {
                        /* UBX header found but frame appears to be missing bytes */
                        frame_size = 0;
                    } else if (latest_msg == INVALID) {
                        /* message header received but message appears to be corrupted */
                        MSG("WARNING: [gps] could not get a valid message from GPS (no time)\n");
                        frame_size = 0;
                    } else if (latest_msg == UBX_NAV_TIMEGPS) {
                        gps_process_sync();
                    }
                }
            } else if (serial_buff[rd_idx] == (char)LGW_GPS_NMEA_SYNC_CHAR) {
                /************************
                 * Found NMEA sync char *
                 ************************/
                /* scan for NMEA end marker (LF = 0x0a) */
                char *nmea_end_ptr = memchr(&serial_buff[rd_idx], (int)0x0a, (wr_idx - rd_idx));

                if (nmea_end_ptr) {
                    /* found end marker */
                    frame_size = nmea_end_ptr - &serial_buff[rd_idx] + 1;
                    latest_msg = lgw_parse_nmea(&serial_buff[rd_idx], frame_size);

                    if (latest_msg == INVALID || latest_msg == UNKNOWN) {
                        /* checksum failed */
                        frame_size = 0;
                    } else if (latest_msg == NMEA_RMC) { /* Get location from RMC frames */
                        gps_process_coords();
                    }
                }
            }

            if (frame_size > 0) {
                /* At this point message is a checksum verified frame
                   we're processed or ignored. Remove frame from buffer */
                rd_idx += frame_size;
                frame_end_idx = rd_idx;
            } else {
                rd_idx++;
            }
        } /* ...for(rd_idx = 0... */

        if (frame_end_idx) {
            /* Frames have been processed. Remove bytes to end of last processed frame */
            memcpy(serial_buff, &serial_buff[frame_end_idx], wr_idx - frame_end_idx);
            wr_idx -= frame_end_idx;
        } /* ...for(rd_idx = 0... */

        /* Prevent buffer overflow */
        if ((sizeof(serial_buff) - wr_idx) < LGW_GPS_MIN_MSG_SIZE) {
            memcpy(serial_buff, &serial_buff[LGW_GPS_MIN_MSG_SIZE], wr_idx - LGW_GPS_MIN_MSG_SIZE);
            wr_idx -= LGW_GPS_MIN_MSG_SIZE;
        }
    }
    MSG("\nINFO: End of GPS thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 5: CHECK TIME REFERENCE AND CALCULATE XTAL CORRECTION --------- */

void thread_valid(void)
{
    /* GPS reference validation variables */
    long gps_ref_age = 0;
    bool ref_valid_local = false;
    double xtal_err_cpy;

    /* variables for XTAL correction averaging */
    unsigned init_cpt = 0;
    double init_acc = 0.0;
    double x;

    /* correction debug */
    // FILE * log_file = NULL;
    // time_t now_time;
    // char log_name[64];

    /* initialization */
    // time(&now_time);
    // strftime(log_name,sizeof log_name,"xtal_err_%Y%m%dT%H%M%SZ.csv",localtime(&now_time));
    // log_file = fopen(log_name, "w");
    // setbuf(log_file, NULL);
    // fprintf(log_file,"\"xtal_correct\",\"XERR_INIT_AVG %lu XERR_FILT_COEF %lu\"\n", XERR_INIT_AVG, XERR_FILT_COEF); // DEBUG

    /* main loop task */
    while (!exit_sig && !quit_sig) {
        //wait_ms(1000);
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        /* calculate when the time reference was last updated */
        xSemaphoreTake(mx_timeref, portMAX_DELAY);
        gps_ref_age = (long)difftime(time(NULL), time_reference_gps.systime);
        if ((gps_ref_age >= 0) && (gps_ref_age <= GPS_REF_MAX_AGE)) {
            /* time ref is ok, validate and  */
            gps_ref_valid = true;
            ref_valid_local = true;
            xtal_err_cpy = time_reference_gps.xtal_err;
            //printf("XTAL err: %.15lf (1/XTAL_err:%.15lf)\n", xtal_err_cpy, 1/xtal_err_cpy); // DEBUG
        } else {
            /* time ref is too old, invalidate */
            gps_ref_valid = false;
            ref_valid_local = false;
        }
        xSemaphoreGive(mx_timeref);

        /* manage XTAL correction */
        if (ref_valid_local == false) {
            /* couldn't sync, or sync too old -> invalidate XTAL correction */
            xSemaphoreTake(mx_xcorr, portMAX_DELAY);
            xtal_correct_ok = false;
            xtal_correct = 1.0;
            xSemaphoreGive(mx_xcorr);
            init_cpt = 0;
            init_acc = 0.0;
        } else {
            if (init_cpt < XERR_INIT_AVG) {
                /* initial accumulation */
                init_acc += xtal_err_cpy;
                ++init_cpt;
            } else if (init_cpt == XERR_INIT_AVG) {
                /* initial average calculation */
                xSemaphoreTake(mx_xcorr, portMAX_DELAY);
                xtal_correct = (double)(XERR_INIT_AVG) / init_acc;
                //printf("XERR_INIT_AVG=%d, init_acc=%.15lf\n", XERR_INIT_AVG, init_acc);
                xtal_correct_ok = true;
                xSemaphoreGive(mx_xcorr);
                ++init_cpt;
                // fprintf(log_file,"%.18lf,\"average\"\n", xtal_correct); // DEBUG
            } else {
                /* tracking with low-pass filter */
                x = 1 / xtal_err_cpy;
                xSemaphoreTake(mx_xcorr, portMAX_DELAY);
                xtal_correct = xtal_correct - xtal_correct / XERR_FILT_COEF + x / XERR_FILT_COEF;
                xSemaphoreGive(mx_xcorr);
                // fprintf(log_file,"%.18lf,\"track\"\n", xtal_correct); // DEBUG
            }
        }
        // printf("Time ref: %s, XTAL correct: %s (%.15lf)\n", ref_valid_local?"valid":"invalid", xtal_correct_ok?"valid":"invalid", xtal_correct); // DEBUG
    }
    MSG("\nINFO: End of validation thread\n");
}

/* -------------------------------------------------------------------------- */
/* --- THREAD 6: BACKGROUND SPECTRAL SCAN                           --------- */

void thread_spectral_scan(void)
{
    int i, x;
    uint32_t freq_hz = spectral_scan_params.freq_hz_start;
    uint32_t freq_hz_stop = spectral_scan_params.freq_hz_start + spectral_scan_params.nb_chan * 200E3;
    int16_t levels[LGW_SPECTRAL_SCAN_RESULT_SIZE];
    uint16_t results[LGW_SPECTRAL_SCAN_RESULT_SIZE];
    struct timeval tm_start;
    lgw_spectral_scan_status_t status;
    uint8_t tx_status = TX_FREE;
    bool spectral_scan_started;
    bool exit_thread = false;

    /* main loop task */
    while (!exit_sig && !quit_sig) {
        /* Pace the scan thread (1 sec min), and avoid waiting several seconds when exit */
        for (i = 0; i < (int)(spectral_scan_params.pace_s ? spectral_scan_params.pace_s : 1); i++) {
            if (exit_sig || quit_sig) {
                exit_thread = true;
                break;
            }
            wait_ms(1000);
        }
        if (exit_thread == true) {
            break;
        }

        spectral_scan_started = false;

        /* Start spectral scan (if no downlink programmed) */
        xSemaphoreTake(mx_concent, portMAX_DELAY);
        /* -- Check if there is a downlink programmed */
        for (i = 0; i < LGW_RF_CHAIN_NB; i++) {
            if (tx_enable[i] == true) {
                x = lgw_status((uint8_t)i, TX_STATUS, &tx_status);
                if (x != LGW_HAL_SUCCESS) {
                    printf("ERROR: failed to get TX status on chain %d\n", i);
                } else {
                    if (tx_status == TX_SCHEDULED || tx_status == TX_EMITTING) {
                        printf("INFO: skip spectral scan (downlink programmed on RF chain %d)\n", i);
                        break; /* exit for loop */
                    }
                }
            }
        }
        if (tx_status != TX_SCHEDULED && tx_status != TX_EMITTING) {
            x = lgw_spectral_scan_start(freq_hz, spectral_scan_params.nb_scan);
            if (x != 0) {
                printf("ERROR: spectral scan start failed\n");
                xSemaphoreGive(mx_concent);
                continue; /* main while loop */
            }
            spectral_scan_started = true;
        }
        xSemaphoreGive(mx_concent);

        if (spectral_scan_started == true) {
            /* Wait for scan to be completed */
            status = LGW_SPECTRAL_SCAN_STATUS_UNKNOWN;
            timeout_start(&tm_start);
            do {
                /* handle timeout */
                if (timeout_check(tm_start, 2000) != 0) {
                    printf("ERROR: %s: TIMEOUT on Spectral Scan\n", __FUNCTION__);
                    break;  /* do while */
                }

                /* get spectral scan status */
                xSemaphoreTake(mx_concent, portMAX_DELAY);
                x = lgw_spectral_scan_get_status(&status);
                xSemaphoreGive(mx_concent);
                if (x != 0) {
                    printf("ERROR: spectral scan status failed\n");
                    break; /* do while */
                }

                /* wait a bit before checking status again */
                wait_ms(10);
            } while (status != LGW_SPECTRAL_SCAN_STATUS_COMPLETED && status != LGW_SPECTRAL_SCAN_STATUS_ABORTED);

            if (status == LGW_SPECTRAL_SCAN_STATUS_COMPLETED) {
                /* Get spectral scan results */
                memset(levels, 0, sizeof levels);
                memset(results, 0, sizeof results);
                xSemaphoreTake(mx_concent, portMAX_DELAY);
                x = lgw_spectral_scan_get_results(levels, results);
                xSemaphoreGive(mx_concent);
                if (x != 0) {
                    printf("ERROR: spectral scan get results failed\n");
                    continue; /* main while loop */
                }

                /* print results */
                printf("SPECTRAL SCAN - %lu Hz: ", freq_hz);
                for (i = 0; i < LGW_SPECTRAL_SCAN_RESULT_SIZE; i++) {
                    printf("%u ", results[i]);
                }
                printf("\n");

                /* Next frequency to scan */
                freq_hz += 200000; /* 200kHz channels */
                if (freq_hz >= freq_hz_stop) {
                    freq_hz = spectral_scan_params.freq_hz_start;
                }
            } else if (status == LGW_SPECTRAL_SCAN_STATUS_ABORTED) {
                printf("INFO: %s: spectral scan has been aborted\n", __FUNCTION__);
            } else {
                printf("ERROR: %s: spectral scan status us unexpected 0x%02X\n", __FUNCTION__, status);
            }
        }
    }
    printf("\nINFO: End of Spectral Scan thread\n");
}

static void pkt_fwd_task(void *pvParameters)
{
    heap_caps_check_integrity_all( true );
    pkt_fwd_main();

    vTaskDelete(NULL);

    while (true) {
        printf("end\n");
        vTaskDelay(8000 / portTICK_PERIOD_MS);
    }
}


static bool reboot_flag = false;
static esp_timer_handle_t reboot_timer = NULL;
static bool pkt_fwd_task_started = false;


void start_pkt_fwd_task()
{
    if (pkt_fwd_task_started == false) {
        pkt_fwd_task_started = true;  // only run once
        printf("Wi-Fi ready. Start tasks...\n");

        // update the time by NTP
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "cn.pool.ntp.org");
        esp_sntp_init();

        // config_wifi_mode(WIFI_MODE_STATION);

        xTaskCreate(((TaskFunction_t) http_server_task), "http_server", 1 * 4096, NULL, 6, &pHttpServer);
        xTaskCreatePinnedToCore(((TaskFunction_t) pkt_fwd_task), "pkt_fwd", 2 * 4096, NULL, 6, &pkt_fwd_handle, 0);
    }
}

static void reboot_timer_callback(void *args)
{
    if (reboot_flag) {
        printf("\n!!! reboot timer called\n");
        esp_restart();
    }
}

void start_reboot_timer_ms(int reboot_delay)
{
    const esp_timer_create_args_t reboot_timer_args = {
        .callback = &reboot_timer_callback,
        .name = "one-shot"
    };
    ESP_ERROR_CHECK(esp_timer_create(&reboot_timer_args, &reboot_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(reboot_timer, reboot_delay * 1000));
}


static void wifi_ap_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        // wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        // ESP_LOGI(WIFI_TAG, "station "MACSTR" join, AID=%d",
        //         MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        // wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        // ESP_LOGI(WIFI_TAG, "station "MACSTR" leave, AID=%d",
        //         MAC2STR(event->mac), event->aid);
    }
}


void wifi_init_soft_ap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID,
                    &wifi_ap_event_handler,
                    NULL,
                    NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .channel = ESP_WIFI_CHANNEL,
            .password = ESP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(WIFI_TAG, "wifi_init_soft_ap finished. SSID:%s password:%s channel:%d",
             ESP_WIFI_SSID, ESP_WIFI_PASS, ESP_WIFI_CHANNEL);
}

static int s_retry_num = 0;

static void wifi_sta_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    int reboot_delay_s = 60 * 3;  // reboot in 3 minute if wifi disconnected
    static bool reboot_timer_started = false;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {

        if (reboot_timer_started == false) {
            reboot_timer_started = true;
            start_reboot_timer_ms(reboot_delay_s * 1000); // change to ms
        }

        wifi_ready = false;
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            ESP_LOGI(WIFI_TAG, "retry to connect to the AP");
            esp_wifi_connect();
            s_retry_num++;
        } else {
            ESP_LOGI(WIFI_TAG, "Failed to connect to the AP; retry again...");
            esp_wifi_connect();
            s_retry_num = 0;
            reboot_flag = true;
        }
        ESP_LOGI(WIFI_TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_ready = true;
        ESP_LOGI(WIFI_TAG, "connected to ap (SSID:%s) succeeded", wifi_ssid);
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(WIFI_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        sprintf(self_ip, "%d.%d.%d.%d", IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        reboot_flag = false;


        if (wifi_mode == WIFI_MODE_SOFT_AP) {
            ESP_LOGE(WIFI_TAG, "WiFi Sta : Config mode exit!");
            return ;
        }

        start_pkt_fwd_task();

    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID,
                    &wifi_sta_event_handler,
                    NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                    IP_EVENT_STA_GOT_IP,
                    &wifi_sta_event_handler,
                    NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {},
    };


    if (wifi_ssid[0] == '\0') {
        ESP_LOGE(WIFI_TAG, "No Wi-Fi ssid provided. Skip Wi-Fi connection");
        return;
    }

    strncpy((char *)wifi_config.sta.ssid, (char *)wifi_ssid, 32);
    strncpy((char *)wifi_config.sta.password, (char *)wifi_pswd, 64);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");
}


void read_config_from_nvs(void)
{
    // init nvs storage
    init_config_storage();

    // check configs from nvs
    read_config();
    dump_config();

    if (config[WIFI_SSID].len > 0 && config[WIFI_SSID].len < 32)
        strncpy(wifi_ssid, config[WIFI_SSID].val, config[WIFI_SSID].len);

    if (config[WIFI_PASSWORD].len > 0 && config[WIFI_PASSWORD].len < 64)
        strncpy(wifi_pswd, config[WIFI_PASSWORD].val, config[WIFI_PASSWORD].len);

    if (config[NS_HOST].len > 0 && config[NS_HOST].len < 64)
        strncpy(udp_host, config[NS_HOST].val, config[NS_HOST].len);

    if (config[NS_PORT].len > 0 && config[NS_PORT].len < 32) {
        udp_port = atoi(config[NS_PORT].val);
        if (udp_port == 0)
            ESP_LOGI(BOOT_TAG, "Convert port(%s) failed", config[NS_PORT].val);
    }

    if (config[GW_ID].len > 0 && config[GW_ID].len == 16)
        strncpy(gw_id, config[GW_ID].val, config[GW_ID].len);
}


static struct {
    struct arg_lit *restore;
    struct arg_lit *help;
    struct arg_str *wifi_ssid;
    struct arg_str *wifi_pswd;
    struct arg_str *udp_host;
    struct arg_int *udp_port;
    struct arg_str *gw_id;
    struct arg_str *freq1;
    struct arg_str *freq2;
    struct arg_str *region;
    struct arg_end *end;
} net_conf_args;

void usage(void)
{
    printf("\n\n ---- pkt_fwd ----\n");
    printf("\nAvailable options:\n");
    printf(" -h                   print this help\n");
    printf(" -u <wifi ssid>       Wifi SSID\n");
    printf(" -p <wifi password>   Wifi Password\n");
    printf(" --host <NS Host>     NS Host\n");
    printf(" --port <NS Port>     NS Port\n");
    printf(" --gwid <Gateway id>  Gateway ID\n");
    printf(" --freq1 <Radio0 center frequency>  Radio0 center frequency\n");
    printf(" --freq2 <Radio2 center frequency>  Radio1 center frequency\n");
    printf(" --region <Region frequency plan>  Frequency Plan\n");

}

int restore_to_default()
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("nvs", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW("restore", "opening NVS handle failed: %s!", esp_err_to_name(err));
        ESP_LOGW("restore", "Config not updated");
        return ESP_FAIL;
    }

    nvs_erase_all(my_handle);

    // Commit config
    printf("Restore config to NVS... ");
    err = nvs_commit(my_handle);
    printf((err != ESP_OK) ? "Failed!\n" : "Done\n");
    nvs_close(my_handle);
    return ESP_OK;
}

// Note: No Error Checking!
// TODO: Should release the resources with arg_freetable().
// Please provide right arguments, or take whatever consequences.
// We don't bother to check and free config[xx].val because we'll reboot esp32 if >= 1 config is updated
static int do_net_config_cmd(int argc, char **argv)
{
    uint32_t val;
    const char *sval;
    int nerrors;
    bool config_updated_config = false;

    // cancel the reboot now that user inputs something on the command line
    reboot_flag = false;

    nerrors = arg_parse(argc, argv, (void **)&net_conf_args);

    // process '-h' or '--help' first, before the error reporting
    if (net_conf_args.help->count) {
        usage();
        return 0;
    }

    // process '-h' or '--help' first, before the error reporting
    if (net_conf_args.restore->count) {
        restore_to_default();
        return 0;
    }

    if (nerrors != 0) {
        arg_print_errors(stderr, net_conf_args.end, argv[0]);
        return 0;
    }

    // process '-u' for modulation type
    if (net_conf_args.wifi_ssid->count > 0) {
        sval = net_conf_args.wifi_ssid->sval[0];
        sprintf((char *)wifi_ssid, "%s", (char *)sval);
        config[WIFI_SSID].val = wifi_ssid;
        config[WIFI_SSID].len = strlen(wifi_ssid);
        config_updated_config = true;
    }

    // process '-p' for modulation type
    if (net_conf_args.wifi_pswd->count > 0) {
        sval = net_conf_args.wifi_pswd->sval[0];
        sprintf((char *)wifi_pswd, "%s", (char *)sval);
        config[WIFI_PASSWORD].val = wifi_pswd;
        config[WIFI_PASSWORD].len = strlen(wifi_pswd);
        config_updated_config = true;
    }

    // process '--host' for modulation type
    if (net_conf_args.udp_host->count > 0) {
        sval = net_conf_args.udp_host->sval[0];
        sprintf((char *)udp_host, "%s", (char *)sval);
        config[NS_HOST].val = udp_host;
        config[NS_HOST].len = strlen(udp_host);
        config_updated_config = true;
    }

    // process '--port' for number of packets to be received before exiting
    if (net_conf_args.udp_port->count > 0) {
        val = (uint32_t)net_conf_args.udp_port->ival[0];
        udp_port = val;

        sprintf((char *)udp_port_str, "%lu", val);
        config[NS_PORT].val = udp_port_str;
        config[NS_PORT].len = strlen(udp_port_str);
        config_updated_config = true;
    }

    // process '--gwid' for modulation type
    if (net_conf_args.gw_id->count > 0) {
        sval = net_conf_args.gw_id->sval[0];
        sprintf(gw_id, "%s", (char *)sval);
        config[GW_ID].val = gw_id;
        config[GW_ID].len = strlen(gw_id);
        config_updated_config = true;
    }

    // process '--gwid' for modulation type
    if (net_conf_args.freq1->count > 0) {
        sval = net_conf_args.freq1->sval[0];
        sprintf(freq1, "%s", (char *)sval);
        config[FREQ_RADIO0].val = freq1;
        config[FREQ_RADIO0].len = strlen(freq1);
        config_updated_config = true;
    }

    // process '--gwid' for modulation type
    if (net_conf_args.freq2->count > 0) {
        sval = net_conf_args.freq2->sval[0];
        sprintf(freq2, "%s", (char *)sval);
        config[FREQ_RADIO1].val = freq2;
        config[FREQ_RADIO1].len = strlen(freq2);
        config_updated_config = true;
    }

    // process '--gwid' for modulation type
    if (net_conf_args.region->count > 0) {
        sval = net_conf_args.region->sval[0];
        sprintf(region, "%s", (char *)sval);
        config[FREQ_REGION].val = region;
        config[FREQ_REGION].len = strlen(region);
        config_updated_config = true;
    }

    dump_config();

    if (config_updated_config == true) {
        // config_wifi_mode(WIFI_MODE_STATION);
        ESP_LOGI("CONFIG", "Restart Gateway....");
        save_config();
        esp_restart();
    }

    return 0;
}


static void register_config(void)
{
    net_conf_args.help       = arg_lit0("h", "help",          "print help");
    net_conf_args.wifi_ssid  = arg_str0("u", NULL, "<wifi ssid>", "Wifi SSID");
    net_conf_args.wifi_pswd  = arg_str0("p", NULL, "<wifi password>", "Wifi Password");
    net_conf_args.restore    = arg_lit0(NULL, "restore",          "clean NVS config");
    net_conf_args.udp_host   = arg_str0(NULL, "host", "<UDP Host>", "UDP Host");
    net_conf_args.udp_port   = arg_int0(NULL, "port", "<UDP Port>",  "UDP Port");
    net_conf_args.gw_id      = arg_str0(NULL, "gwid", "<gateway id>", "Gateway Id");

    net_conf_args.freq1      = arg_str0(NULL, "freq1", "<Radio0 center frequency>", "Radio0 center frequency");
    net_conf_args.freq2      = arg_str0(NULL, "freq2", "<Radio1 center frequency>", "Radio1 center frequency");
    net_conf_args.region      = arg_str0(NULL, "region", "<Region frequency plan>", "Region Frequency Plan");

    net_conf_args.end = arg_end(2);

    const esp_console_cmd_t hal_conf_cmd = {
        .command = "pkt_fwd",
        .help = "ESP32 packet forwarder based on sx1302_hal",
        .hint = NULL,
        .func = &do_net_config_cmd,
        .argtable = &net_conf_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&hal_conf_cmd));
}


#ifdef ENABLE_ETH
/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(ETH_TAG, "Ethernet Link Up");
        ESP_LOGI(ETH_TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(ETH_TAG, "Ethernet Link Down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(ETH_TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(ETH_TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(ETH_TAG, "Ethernet Got IP Address");
    ESP_LOGI(ETH_TAG, "~~~~~~~~~~~");
    ESP_LOGI(ETH_TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(ETH_TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(ETH_TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(ETH_TAG, "~~~~~~~~~~~");

    if (reboot_timer) {
        esp_timer_delete(reboot_timer);
        reboot_timer = NULL;
    }

    if (wifi_mode == WIFI_MODE_SOFT_AP) {
        ESP_LOGE(ETH_TAG, "ETH : Config mode exit!");
        return ;
    }

    start_pkt_fwd_task();
}
#endif

void ethernet_init()
{
#ifdef ENABLE_ETH

    // Initialize Ethernet driver
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(example_eth_init(&eth_handles, &eth_port_cnt));

    // Create instance(s) of esp-netif for Ethernet(s)
    if (eth_port_cnt == 1) {
        // Use ESP_NETIF_DEFAULT_ETH when just one Ethernet interface is used and you don't need to modify
        // default esp-netif configuration parameters.
        esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
        esp_netif_t *eth_netif = esp_netif_new(&cfg);
        // Attach Ethernet driver to TCP/IP stack
        ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));
    } else {
        // Use ESP_NETIF_INHERENT_DEFAULT_ETH when multiple Ethernet interfaces are used and so you need to modify
        // esp-netif configuration parameters for each interface (name, priority, etc.).
        esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
        esp_netif_config_t cfg_spi = {
            .base = &esp_netif_config,
            .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH
        };
        char if_key_str[10];
        char if_desc_str[10];
        char num_str[3];
        for (int i = 0; i < eth_port_cnt; i++) {
            itoa(i, num_str, 10);
            strcat(strcpy(if_key_str, "ETH_"), num_str);
            strcat(strcpy(if_desc_str, "eth"), num_str);
            esp_netif_config.if_key = if_key_str;
            esp_netif_config.if_desc = if_desc_str;
            esp_netif_config.route_prio -= i * 5;
            esp_netif_t *eth_netif = esp_netif_new(&cfg_spi);

            // Attach Ethernet driver to TCP/IP stack
            ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[i])));
        }
    }

    // Register user defined event handers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // Start Ethernet driver state machine
    for (int i = 0; i < eth_port_cnt; i++) {
        ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
    }
#endif
}


void init_spi()
{
    esp_err_t ret;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ret = spi_bus_initialize(SX1302_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    ESP_ERROR_CHECK(ret != ESP_OK);
}

void init_sd()
{
    esp_err_t ret;
    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";
    ESP_LOGI(SD_TAG, "Initializing SD card");

    // Use settings defined above to initialize SD card and mount FAT filesystem.
    // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
    // Please check its source code and implement error recovery when developing
    // production applications.
    ESP_LOGI(SD_TAG, "Using SPI peripheral");

    // By default, SD card frequency is initialized to SDMMC_FREQ_DEFAULT (20MHz)
    // For setting a specific frequency, use host.max_freq_khz (range 400kHz - 20MHz for SDSPI)
    // Example: for fixed frequency of 10MHz, use host.max_freq_khz = 10000;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SX1302_SPI_HOST;

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = BOARD_SD_CS_PIN;
    slot_config.host_id = host.slot;

    ESP_LOGI(SD_TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(SD_TAG, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(SD_TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
#ifdef CONFIG_EXAMPLE_DEBUG_PIN_CONNECTIONS
            check_sd_card_pins(&config, pin_count);
#endif
        }
        return;
    }
    ESP_LOGI(SD_TAG, "Filesystem mounted");
}



bool check_reconfig()
{
    gpio_set_direction(SX1302_CONFIG_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SX1302_CONFIG_PIN, GPIO_PULLUP_ONLY);
    int i = 3;
    while (i--) {
        ESP_LOGI("CONFIG", "Waiting to enter configuration mode, %d second remaining", i);
        if (gpio_get_level(SX1302_CONFIG_PIN) == 0) {
            gpio_reset_pin(SX1302_CONFIG_PIN);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    gpio_reset_pin(SX1302_CONFIG_PIN);
    return false;
}


void app_main(void)
{
    int reboot_delay_s;

    // Initialize TCP/IP network interface aka the esp-netif (should be called only once in application)
    ESP_ERROR_CHECK(esp_netif_init());
    // Create default event loop that running in background
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#ifdef BLINK_GPIO
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BLINK_GPIO, 0);
#endif

    // SPI Bus
    init_spi();
    // SD Card
    init_sd();

    printf("\n\n*** ESXP1302 Gateway. Version: %s ***\n\n\n", ESP32_1302_VERSION);

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = false
    };

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE("SPIFFS", "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE("SPIFFS", "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE("SPIFFS", "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    read_config_from_nvs();

    // Wait to enter the configuration mode, press the BOOT button to enter the reconfiguration mode
    if (check_reconfig()) {
        ESP_LOGI("CONFIG", "The user presses the BOOT button to clear the configuration and enter the configuration mode");
        wifi_mode = WIFI_MODE_SOFT_AP;
    }


    ethernet_init();

    // modem_init();
    if (config[WIFI_MODE].val != NULL) {
        ESP_LOGW("MAIN", "NET WORK MODE :   %s", config[WIFI_MODE].val);
        ESP_LOGW("MAIN", "NET WORK MODE :   %s", config[WIFI_MODE].val);
        ESP_LOGW("MAIN", "NET WORK MODE :   %s", config[WIFI_MODE].val);
    }

    if (config[WIFI_MODE].val == NULL) {
        // if not set yet, go to soft_ap_mode mode as the default
        wifi_mode = WIFI_MODE_SOFT_AP;

    } else if (strncmp(config[WIFI_MODE].val, "eth_net", 7) == 0) {
        wifi_mode = WIFI_MODE_ETH;
        config_wifi_mode(WIFI_MODE_ETH);

    } else if (strncmp(config[WIFI_MODE].val, "soft_ap", 7) == 0) {
        wifi_mode = WIFI_MODE_SOFT_AP;
        
    } else if (strncmp(config[WIFI_MODE].val, "station", 7) == 0) {
        if (config[WIFI_SSID].val == NULL || config[WIFI_PASSWORD].val == NULL) {
            ESP_LOGE("WIFI", "Failed to init wifi , using soft ap mode!");
            wifi_mode = WIFI_MODE_SOFT_AP;
        } else if (strlen(config[WIFI_SSID].val) == 0 || strlen(config[WIFI_PASSWORD].val) < 8 ) {
            ESP_LOGE("WIFI", "Failed to init wifi , using soft ap mode!");
            wifi_mode = WIFI_MODE_SOFT_AP;
        } else {
            wifi_mode = WIFI_MODE_STATION;
        }
    }

    if (wifi_mode == WIFI_MODE_SOFT_AP) {

#ifdef BLINK_GPIO
        gpio_set_level(BLINK_GPIO, 1);
#endif

        ESP_LOGE(WIFI_TAG, "ESP_WIFI_MODE_SOFT_AP");
        config_wifi_mode(WIFI_MODE_STATION);

        // set up timer for reboot
        reboot_delay_s = 60 * 10; // 10 minutes
        reboot_flag = true;
        start_reboot_timer_ms(reboot_delay_s * 1000); // change to ms

        ESP_LOGI(WIFI_TAG, "ESP_WIFI_MODE_SOFT_AP");
        wifi_init_soft_ap();

        // start http service
        xTaskCreate(((TaskFunction_t) http_server_task), "http_server", 1 * 4096, NULL, 6, &pHttpServer);

    } else if (wifi_mode == WIFI_MODE_STATION)  {

        ESP_LOGE(WIFI_TAG, "ESP_WIFI_MODE_STA");
        // assume wifi can't connect; set up timer preparing for reboot soon
        config_wifi_mode(WIFI_MODE_SOFT_AP);
        reboot_delay_s = 60 * 5; // 5 minutes
        reboot_flag = true;
        start_reboot_timer_ms(reboot_delay_s * 1000); // change to ms

        ESP_LOGI(WIFI_TAG, "ESP_WIFI_MODE_STA");
        wifi_init_sta();

    }

    // start command line
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.task_stack_size = 4096 * 2;
    repl_config.prompt = "ESXP1302_GW>";

    usage();
    register_config();

    // initialize console REPL environment
    esp_console_repl_t *repl = NULL;


#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));

#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));

#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));

#else
#error Unsupported console type
#endif



    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    // char* pcBuf = malloc(4096);
    while (1) {
        // heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);
        // vTaskList(pcBuf);
        // printf("%s\n",pcBuf);

        // printf("pJit:%p\n",pJit);
        // printf("pThreadUp:%p\n",pThreadUp);
        // printf("pThreadDn:%p\n",pThreadDn);
        // printf("pHttpServer:%p\n",pHttpServer);
        // printf("pkt_fwd_handle:%p\n",pkt_fwd_handle);

        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
