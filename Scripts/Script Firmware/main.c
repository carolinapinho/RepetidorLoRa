/*
 * Copyright (c) 2021, Pycom Limited.
 *
 * This software is licensed under the GNU GPL version 3 or any
 * later version, with permitted additional terms. For more information
 * see the Pycom Licence v1.0 document supplied with this file, or
 * available at https://www.pycom.io/opensource/licensing
 */
/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2013 Semtech-Cycleo
Description:
    Configure Lora concentrator and forward packets to a server
License: Revised BSD License, see LICENSE.TXT file include in the project
*/


/* -------------------------------------------------------------------------- */
/* --- DEPENDANCIES --------------------------------------------------------- */

/* fix an issue between POSIX and C99 */
#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif

#include <stdint.h>         /* C99 types */
#include <stdbool.h>        /* bool type */
#include <stdio.h>          /* printf, fprintf, snprintf, fopen, fputs */

#include <string.h>         /* memset */
#include <signal.h>         /* sigaction */
#include <time.h>           /* time, clock_gettime, strftime, gmtime */
#include <sys/time.h>       /* timeval */
#include <unistd.h>         /* getopt, access */
#include <stdlib.h>         /* atoi, exit */
#include <errno.h>          /* error messages */
#include <math.h>           /* modf */
#include <assert.h>

#include <sys/socket.h>     /* socket specific definitions */
#include <netinet/in.h>     /* INET constants and stuff */
#include <arpa/inet.h>      /* IP address conversion stuff */
#include <netdb.h>          /* gai_strerror */

#include <pthread.h>

#include "trace.h"
#include "jitqueue.h"
#include "timersync.h"
#include "parson.h"
#include "base64.h"
#include "loragw_hal.h"
#include "loragw_reg.h"
#include "loragw_aux.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"

#include "lwip/err.h"
#include "lwip/apps/sntp.h"
#include "utils/interrupt_char.h"
#include "py/obj.h"
#include "py/mpprint.h"
#include "modmachine.h"
#include "machpin.h"
#include "pins.h"
#include "sx1308-config.h"

/* -------------------------------------------------------------------------- */
/* --- PRIVATE MACROS ------------------------------------------------------- */

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define STRINGIFY(x)    #x
#define STR(x)          STRINGIFY(x)
#define exit(x)         loragw_exit(x)
/* -------------------------------------------------------------------------- */
/* --- PRIVATE CONSTANTS ---------------------------------------------------- */

#ifndef VERSION_STRING
#define VERSION_STRING "undefined"
#endif

#define DEFAULT_SERVER      127.0.0.1   /* hostname also supported */
#define DEFAULT_PORT_UP     1780
#define DEFAULT_PORT_DW     1782
#define DEFAULT_KEEPALIVE   5           /* default time interval for downstream keep-alive packet */
#define DEFAULT_STAT        30          /* default time interval for statistics */
#define PUSH_TIMEOUT_MS     100
#define PULL_TIMEOUT_MS     200
#define FETCH_SLEEP_MS      50          /* nb of ms waited when a fetch return no packets */

#define PROTOCOL_VERSION    2           /* v1.3 */

#define PKT_PUSH_DATA   0
#define PKT_PUSH_ACK    1
#define PKT_PULL_DATA   2
#define PKT_PULL_RESP   3
#define PKT_PULL_ACK    4
#define PKT_TX_ACK      5

#define NB_PKT_MAX      2 /* max number of packets per fetch/send cycle */

#define MIN_LORA_PREAMB 6 /* minimum Lora preamble length for this application */
#define STD_LORA_PREAMB 8
#define MIN_FSK_PREAMB  3 /* minimum FSK preamble length for this application */
#define STD_FSK_PREAMB  5

#define STATUS_SIZE     200
#define TX_BUFF_SIZE    ((540 * NB_PKT_MAX) + 30 + STATUS_SIZE)

#define NI_NUMERICHOST	1	/* return the host address, not the name */

#define COM_PATH_DEFAULT "/dev/ttyACM0"

#define LORA_GW_STACK_SIZE                                             (15000)
#define LORA_GW_PRIORITY                                               (10)

TaskHandle_t xLoraGwTaskHndl;
typedef void (*_sig_func_cb_ptr)(int);

void TASK_lora_gw(void *pvParameters);
void mp_hal_set_signal_exit_cb (_sig_func_cb_ptr fun);
bool mach_is_rtc_synced (void);
/* -------------------------------------------------------------------------- */
/* --- PRIVATE TYPES -------------------------------------------------------- */

/**
@struct coord_s
@brief Geodesic coordinates
*/
struct coord_s {
    double  lat;    /*!> latitude [-90,90] (North +, South -) */
    double  lon;    /*!> longitude [-180,180] (East +, West -)*/
    short   alt;    /*!> altitude in meters (WGS 84 geoid ref.) */
};

/* -------------------------------------------------------------------------- */
/* --- PRIVATE VARIABLES (GLOBAL) ------------------------------------------- */

/* signal handling variables */
volatile DRAM_ATTR bool exit_sig = false; /* 1 -> application terminates cleanly (shut down hardware, close open files, etc) */
volatile DRAM_ATTR bool quit_sig = false; /* 1 -> application terminates without shutting down the hardware */

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

/* network protocol variables */
static struct timeval push_timeout_half = {0, (PUSH_TIMEOUT_MS * 500)}; /* cut in half, critical for throughput */
static struct timeval pull_timeout = {0, (PULL_TIMEOUT_MS * 1000)}; /* non critical for throughput */

/* hardware access control and correction */
pthread_mutex_t mx_concent = PTHREAD_MUTEX_INITIALIZER; /* control access to the concentrator */

/* Reference coordinates, for broadcasting (beacon) */
static struct coord_s reference_coord;

/* Enable faking the GPS coordinates of the gateway */
static bool gps_fake_enable; /* enable the feature */

/* measurements to establish statistics */
static pthread_mutex_t mx_meas_up = PTHREAD_MUTEX_INITIALIZER; /* control access to the upstream measurements */
static uint32_t meas_nb_rx_rcv = 0; /* count packets received */
static uint32_t meas_nb_rx_ok = 0; /* count packets received with PAYLOAD CRC OK */
static uint32_t meas_nb_rx_bad = 0; /* count packets received with PAYLOAD CRC ERROR */
static uint32_t meas_nb_rx_nocrc = 0; /* count packets received with NO PAYLOAD CRC */
static uint32_t meas_up_pkt_fwd = 0; /* number of radio packet forwarded to the server */
static uint32_t meas_up_network_byte = 0; /* sum of UDP bytes sent for upstream traffic */
static uint32_t meas_up_payload_byte = 0; /* sum of radio payload bytes sent for upstream traffic */
static uint32_t meas_up_dgram_sent = 0; /* number of datagrams sent for upstream traffic */
static uint32_t meas_up_ack_rcv = 0; /* number of datagrams acknowledged for upstream traffic */

static pthread_mutex_t mx_meas_dw = PTHREAD_MUTEX_INITIALIZER; /* control access to the downstream measurements */
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

static pthread_mutex_t mx_stat_rep = PTHREAD_MUTEX_INITIALIZER; /* control access to the status report */
static bool report_ready = false; /* true when there is a new report to send to the server */
static char status_report[STATUS_SIZE]; /* status report as a JSON object */

/* auto-quit function */
static uint32_t autoquit_threshold = 0; /* enable auto-quit after a number of non-acknowledged PULL_DATA (0 = disabled)*/

/* Just In Time TX scheduling */
static struct jit_queue_s jit_queue;

/* Gateway specificities */
static int8_t antenna_gain = 0;

/* TX capabilities */
static struct lgw_tx_gain_lut_s txlut; /* TX gain table */
static uint32_t tx_freq_min[LGW_RF_CHAIN_NB]; /* lowest frequency supported by TX chain */
static uint32_t tx_freq_max[LGW_RF_CHAIN_NB]; /* highest frequency supported by TX chain */

int debug_level = LORAPF_INFO_;

/* -------------------------------------------------------------------------- */
/* --- PRIVATE FUNCTIONS DECLARATION ---------------------------------------- */

static IRAM_ATTR void sig_handler(int sigio);

static int parse_SX1301_configuration(const char * conf_file);

static int parse_gateway_configuration(const char * conf_file);

//static double difftimespec(struct timespec end, struct timespec beginning);

static double time_diff(struct timeval x , struct timeval y);

static void obtain_time(void);

static void loragw_exit(int status);

/* threads */
void thread_up(void);
void thread_down(void);
void thread_jit(void);
void thread_timersync(void);

/* -------------------------------------------------------------------------- */
/* --- PRIVATE FUNCTIONS DEFINITION ----------------------------------------- */

static void exit_cleanup(void) {
    MSG_INFO("[main] Stopping concentrator\n");
    lgw_stop();
}

static IRAM_ATTR void sig_handler(int sigio) {
    if (sigio == SIGQUIT) {
        quit_sig = true;
    } else if ((sigio == SIGINT) || (sigio == SIGTERM)) {
        exit_sig = true;
    }
    return;
}
static void loragw_exit(int status)
{
    exit_sig = false;
    if(status == EXIT_FAILURE)
    {
        exit_cleanup();
        machine_pygate_set_status(PYGATE_ERROR);
        vTaskDelete(NULL);
        for(;;);
    }
    else
    {
        if(quit_sig)
        {
            quit_sig = false;
            machine_pygate_set_status(PYGATE_STOPPED);
            vTaskDelete(NULL);
            for(;;);
        }
        esp_restart();
    }
}

static int parse_SX1301_configuration(const char * conf_file) {
    int i;
    char param_name[32]; /* used to generate variable parameter names */
    const char *str; /* used to store string value from JSON object */
    const char conf_obj_name[] = "SX1301_conf";
    JSON_Value *root_val = NULL;
    JSON_Object *conf_obj = NULL;
    JSON_Value *val = NULL;
    struct lgw_conf_board_s boardconf;
    struct lgw_conf_rxrf_s rfconf;
    struct lgw_conf_rxif_s ifconf;
    uint32_t sf, bw, fdev;

    /* try to parse JSON */
    root_val = json_parse_file_with_comments(conf_file);
    if (root_val == NULL) {
        MSG_ERROR("[main] %s is not a valid JSON file\n", conf_file);
        exit(EXIT_FAILURE);
    }

    /* point to the gateway configuration object */
    conf_obj = json_object_get_object(json_value_get_object(root_val), conf_obj_name);
    if (conf_obj == NULL) {
        MSG_INFO("[main] %s does not contain a JSON object named %s\n", conf_file, conf_obj_name);
        return -1;
    } else {
        //MSG_INFO("[main] %s does contain a JSON object named %s, parsing SX1301 parameters\n", conf_file, conf_obj_name);
    }

    /* set board configuration */
    memset(&boardconf, 0, sizeof boardconf); /* initialize configuration structure */
    val = json_object_get_value(conf_obj, "lorawan_public"); /* fetch value (if possible) */
    if (json_value_get_type(val) == JSONBoolean) {
        boardconf.lorawan_public = (bool)json_value_get_boolean(val);
    } else {
        MSG_WARN("[main] Data type for lorawan_public seems wrong, please check\n");
        boardconf.lorawan_public = false;
    }
    val = json_object_get_value(conf_obj, "clksrc"); /* fetch value (if possible) */
    if (json_value_get_type(val) == JSONNumber) {
        boardconf.clksrc = (uint8_t)json_value_get_number(val);
    } else {
        MSG_WARN("[main] Data type for clksrc seems wrong, please check\n");
        boardconf.clksrc = 0;
    }
    MSG_INFO("[main] lorawan_public %d, clksrc %d\n", boardconf.lorawan_public, boardconf.clksrc);
    /* all parameters parsed, submitting configuration to the HAL */
    if (lgw_board_setconf(&boardconf) != LGW_HAL_SUCCESS) {
        MSG_ERROR("[main] Failed to configure board\n");
        return -1;
    }

    /* set antenna gain configuration */
    val = json_object_get_value(conf_obj, "antenna_gain"); /* fetch value (if possible) */
    if (val != NULL) {
        if (json_value_get_type(val) == JSONNumber) {
            antenna_gain = (int8_t)json_value_get_number(val);
        } else {
            MSG_WARN("[main] Data type for antenna_gain seems wrong, please check\n");
            antenna_gain = 0;
        }
    }
    MSG_INFO("[main] antenna_gain %d dBi\n", antenna_gain);

    /* set configuration for tx gains */
    memset(&txlut, 0, sizeof txlut); /* initialize configuration structure */
    for (i = 0; i < TX_GAIN_LUT_SIZE_MAX; i++) {
        snprintf(param_name, sizeof param_name, "tx_lut_%i", i); /* compose parameter path inside JSON structure */
        val = json_object_get_value(conf_obj, param_name); /* fetch value (if possible) */
        if (json_value_get_type(val) != JSONObject) {
            MSG_INFO("[main] no configuration for tx gain lut %i\n", i);
            continue;
        }
        txlut.size++; /* update TX LUT size based on JSON object found in configuration file */
        /* there is an object to configure that TX gain index, let's parse it */
        snprintf(param_name, sizeof param_name, "tx_lut_%i.pa_gain", i);
        val = json_object_dotget_value(conf_obj, param_name);
        if (json_value_get_type(val) == JSONNumber) {
            txlut.lut[i].pa_gain = (uint8_t)json_value_get_number(val);
        } else {
            MSG_WARN("[main] Data type for %s[%d] seems wrong, please check\n", param_name, i);
            txlut.lut[i].pa_gain = 0;
        }
        snprintf(param_name, sizeof param_name, "tx_lut_%i.dac_gain", i);
        val = json_object_dotget_value(conf_obj, param_name);
        if (json_value_get_type(val) == JSONNumber) {
            txlut.lut[i].dac_gain = (uint8_t)json_value_get_number(val);
            
        } else {
            txlut.lut[i].dac_gain = 3; /* This is the only dac_gain supported for now */
        }
        snprintf(param_name, sizeof param_name, "tx_lut_%i.dig_gain", i);
        val = json_object_dotget_value(conf_obj, param_name);
        if (json_value_get_type(val) == JSONNumber) {
            txlut.lut[i].dig_gain = (uint8_t)json_value_get_number(val);
        } else {
            MSG_WARN("[main] Data type for %s[%d] seems wrong, please check\n", param_name, i);
            txlut.lut[i].dig_gain = 0;
        }
        snprintf(param_name, sizeof param_name, "tx_lut_%i.mix_gain", i);
        val = json_object_dotget_value(conf_obj, param_name);
        if (json_value_get_type(val) == JSONNumber) {
            txlut.lut[i].mix_gain = (uint8_t)json_value_get_number(val);
        } else {
            MSG_WARN("[main] Data type for %s[%d] seems wrong, please check\n", param_name, i);
            txlut.lut[i].mix_gain = 0;
        }
        snprintf(param_name, sizeof param_name, "tx_lut_%i.rf_power", i);
        val = json_object_dotget_value(conf_obj, param_name);
        if (json_value_get_type(val) == JSONNumber) {
            txlut.lut[i].rf_power = (int8_t)json_value_get_number(val);
        } else {
            MSG_WARN("[main] Data type for %s[%d] seems wrong, please check\n", param_name, i);
            txlut.lut[i].rf_power = 0;
        }
    }
    /* all parameters parsed, submitting configuration to the HAL */
    if (txlut.size > 0) {
        MSG_INFO("[main] Configuring TX LUT with %u indexes\n", txlut.size);
        if (lgw_txgain_setconf(&txlut) != LGW_HAL_SUCCESS) {
            MSG_ERROR("[main] Failed to configure concentrator TX Gain LUT\n");
            return -1;
        }
    } else {
        MSG_WARN("[main] No TX gain LUT defined\n");
    }

    /* set configuration for RF chains */
    for (i = 0; i < LGW_RF_CHAIN_NB; ++i) {
        memset(&rfconf, 0, sizeof rfconf); /* initialize configuration structure */
        snprintf(param_name, sizeof param_name, "radio_%i", i); /* compose parameter path inside JSON structure */
        val = json_object_get_value(conf_obj, param_name); /* fetch value (if possible) */
        if (json_value_get_type(val) != JSONObject) {
            MSG_INFO("[main] no configuration for radio %i\n", i);
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
            MSG_INFO("[main] radio %i disabled\n", i);
        } else  { /* radio enabled, will parse the other parameters */
            snprintf(param_name, sizeof param_name, "radio_%i.freq", i);
            rfconf.freq_hz = (uint32_t)json_object_dotget_number(conf_obj, param_name);
            snprintf(param_name, sizeof param_name, "radio_%i.rssi_offset", i);
            rfconf.rssi_offset = (float)json_object_dotget_number(conf_obj, param_name);
            snprintf(param_name, sizeof param_name, "radio_%i.type", i);
            str = json_object_dotget_string(conf_obj, param_name);
            if (!strncmp(str, "SX1255", 6)) {
                rfconf.type = LGW_RADIO_TYPE_SX1255;
            } else if (!strncmp(str, "SX1257", 6)) {
                rfconf.type = LGW_RADIO_TYPE_SX1257;
            } else {
                MSG_WARN("[main] invalid radio type: %s (should be SX1255 or SX1257)\n", str);
            }
            snprintf(param_name, sizeof param_name, "radio_%i.tx_enable", i);
            val = json_object_dotget_value(conf_obj, param_name);
            if (json_value_get_type(val) == JSONBoolean) {
                rfconf.tx_enable = (bool)json_value_get_boolean(val);
                if (rfconf.tx_enable == true) {
                    /* tx is enabled on this rf chain, we need its frequency range */
                    snprintf(param_name, sizeof param_name, "radio_%i.tx_freq_min", i);
                    tx_freq_min[i] = (uint32_t)json_object_dotget_number(conf_obj, param_name);
                    snprintf(param_name, sizeof param_name, "radio_%i.tx_freq_max", i);
                    tx_freq_max[i] = (uint32_t)json_object_dotget_number(conf_obj, param_name);
                    if ((tx_freq_min[i] == 0) || (tx_freq_max[i] == 0)) {
                        MSG_WARN("[main] no frequency range specified for TX rf chain %d\n", i);
                    }
                }
            } else {
                rfconf.tx_enable = false;
            }
            MSG_INFO("[main] radio %i enabled (type %s), center frequency %u, RSSI offset %f, tx enabled %d\n", i, str, rfconf.freq_hz, rfconf.rssi_offset, rfconf.tx_enable);
        }
        /* all parameters parsed, submitting configuration to the HAL */
        if (lgw_rxrf_setconf(i, &rfconf) != LGW_HAL_SUCCESS) {
            MSG_ERROR("[main] invalid configuration for radio %i\n", i);
            return -1;
        }
    }
    /* set configuration for Lora standard channel */
    memset(&ifconf, 0, sizeof ifconf); /* initialize configuration structure */
    val = json_object_get_value(conf_obj, "chan_Lora_std"); /* fetch value (if possible) */
    if (json_value_get_type(val) != JSONObject) {
        MSG_INFO("[main] no configuration for Lora standard channel\n");
    } else {
        val = json_object_dotget_value(conf_obj, "chan_Lora_std.enable");
        if (json_value_get_type(val) == JSONBoolean) {
            ifconf.enable = (bool)json_value_get_boolean(val);
        } else {
            ifconf.enable = false;
        }
        if (ifconf.enable == false) {
            MSG_INFO("[main] Lora standard channel %i disabled\n", i);
        } else  {
            ifconf.rf_chain = (uint32_t)json_object_dotget_number(conf_obj, "chan_Lora_std.radio");
            ifconf.freq_hz = (int32_t)json_object_dotget_number(conf_obj, "chan_Lora_std.if");
            bw = (uint32_t)json_object_dotget_number(conf_obj, "chan_Lora_std.bandwidth");
            switch(bw) {
                case 500000:
                    ifconf.bandwidth = BW_500KHZ;
                    break;
                case 250000:
                    ifconf.bandwidth = BW_250KHZ;
                    break;
                case 125000:
                    ifconf.bandwidth = BW_125KHZ;
                    break;
                default:
                    ifconf.bandwidth = BW_UNDEFINED;
            }
            sf = (uint32_t)json_object_dotget_number(conf_obj, "chan_Lora_std.spread_factor");
            switch(sf) {
                case  7:
                    ifconf.datarate = DR_LORA_SF7;
                    break;
                case  8:
                    ifconf.datarate = DR_LORA_SF8;
                    break;
                case  9:
                    ifconf.datarate = DR_LORA_SF9;
                    break;
                case 10:
                    ifconf.datarate = DR_LORA_SF10;
                    break;
                case 11:
                    ifconf.datarate = DR_LORA_SF11;
                    break;
                case 12:
                    ifconf.datarate = DR_LORA_SF12;
                    break;
                default:
                    ifconf.datarate = DR_UNDEFINED;
            }
            MSG_INFO("[main] Lora std channel> radio %i, IF %i Hz, %u Hz bw, SF %u\n", ifconf.rf_chain, ifconf.freq_hz, bw, sf);
        }
        if (lgw_rxif_setconf(8, &ifconf) != LGW_HAL_SUCCESS) {
            MSG_ERROR("[main] invalid configuration for Lora standard channel\n");
            return -1;
        }
    }

    /* set configuration for FSK channel */
    memset(&ifconf, 0, sizeof ifconf); /* initialize configuration structure */
    val = json_object_get_value(conf_obj, "chan_FSK"); /* fetch value (if possible) */
    if (json_value_get_type(val) != JSONObject) {
        MSG_INFO("[main] no configuration for FSK channel\n");
    } else {
        val = json_object_dotget_value(conf_obj, "chan_FSK.enable");
        if (json_value_get_type(val) == JSONBoolean) {
            ifconf.enable = (bool)json_value_get_boolean(val);
        } else {
            ifconf.enable = false;
        }
        if (ifconf.enable == false) {
            MSG_INFO("[main] FSK channel %i disabled\n", i);
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
            if      (bw == 0) {
                ifconf.bandwidth = BW_UNDEFINED;
            } else if (bw <= 7800) {
                ifconf.bandwidth = BW_7K8HZ;
            } else if (bw <= 15600) {
                ifconf.bandwidth = BW_15K6HZ;
            } else if (bw <= 31200) {
                ifconf.bandwidth = BW_31K2HZ;
            } else if (bw <= 62500) {
                ifconf.bandwidth = BW_62K5HZ;
            } else if (bw <= 125000) {
                ifconf.bandwidth = BW_125KHZ;
            } else if (bw <= 250000) {
                ifconf.bandwidth = BW_250KHZ;
            } else if (bw <= 500000) {
                ifconf.bandwidth = BW_500KHZ;
            } else {
                ifconf.bandwidth = BW_UNDEFINED;
            }

            MSG_INFO("[main] FSK channel> radio %i, IF %i Hz, %u Hz bw, %u bps datarate\n", ifconf.rf_chain, ifconf.freq_hz, bw, ifconf.datarate);
        }
        if (lgw_rxif_setconf(9, &ifconf) != LGW_HAL_SUCCESS) {
            MSG_ERROR("[main] invalid configuration for FSK channel\n");
            return -1;
        }
    }
    json_value_free(root_val);

    return 0;
}

static double time_diff(struct timeval x , struct timeval y)
{
    double x_ms , y_ms , diff;

    x_ms = (double)x.tv_sec*1000000 + (double)x.tv_usec;
    y_ms = (double)y.tv_sec*1000000 + (double)y.tv_usec;

    diff = (double)y_ms - (double)x_ms;

    return diff / 1000000.0;
}

static void obtain_time(void)
{
    // wait for time to be set
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    while(!mach_is_rtc_synced() && ++retry < retry_count) {
        MSG_INFO("[main] Waiting for system time to be set... (%d/%d)\n", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    if(retry == retry_count)
    {
        MSG_ERROR("[main] Failed to set system time.. please Sync time via an NTP server using RTC module.!\n");
        exit(EXIT_FAILURE);
    }

}

//tx_ack not there

void lora_gw_init(const char* global_conf) {
    MSG_INFO("lora_gw_init() start fh=%u high=%u LORA_GW_STACK_SIZE=%u\n", xPortGetFreeHeapSize(), uxTaskGetStackHighWaterMark(NULL), LORA_GW_STACK_SIZE);

    quit_sig = false;
    exit_sig = false;

    xTaskCreatePinnedToCore(TASK_lora_gw, "LoraGW",
        LORA_GW_STACK_SIZE / sizeof(StackType_t),
        (void *) global_conf,
        LORA_GW_PRIORITY, &xLoraGwTaskHndl, 1);
    MSG_INFO("lora_gw_init() done fh=%u high=%u\n", xPortGetFreeHeapSize(), uxTaskGetStackHighWaterMark(NULL));
}

void pygate_reset() {
    MSG_INFO("pygate_reset\n");

    // pull sx1257 and sx1308 reset high, the PIC FW should power cycle the ESP32 as a result
    pin_obj_t* sx1308_rst = SX1308_RST_PIN;
    pin_config(sx1308_rst, -1, -1, GPIO_MODE_OUTPUT, MACHPIN_PULL_NONE, 0);
    pin_obj_t* sx1257_rst = (&PIN_MODULE_P8);
    pin_config(sx1257_rst, -1, -1, GPIO_MODE_OUTPUT, MACHPIN_PULL_NONE, 0);

    sx1308_rst->value = 1;
    sx1257_rst->value = 1;

    pin_set_value(sx1308_rst);
    pin_set_value(sx1257_rst);

    vTaskDelay(5000 / portTICK_PERIOD_MS);

    // if this is still being executed, then it seems the ESP32 reset did not take place
    // set the two reset lines low again and stop the lora gw task, to make sure we return to a defined state
    MSG_ERROR("pygate_reset failed to reset\n");
    sx1308_rst->value = 0;
    sx1257_rst->value = 0;
    pin_set_value(sx1308_rst);
    pin_set_value(sx1257_rst);

    if (xLoraGwTaskHndl){
        vTaskDelete(xLoraGwTaskHndl);
        xLoraGwTaskHndl = NULL;
    }

}

int lora_gw_get_debug_level(){
    return debug_level;
}

void lora_gw_set_debug_level(int level){
    debug_level = level;
}

void TASK_lora_gw(void *pvParameters) {
	int i;
	int x;
	uint32_t cp_nb_rx_rcv;
	mp_hal_set_signal_exit_cb(sig_handler);
    	machine_register_pygate_sig_handler(sig_handler);
    	mp_hal_set_interrupt_char(3);
	pthread_t thrid_up;
	const char com_path_default[] = COM_PATH_DEFAULT;
    	const char *com_path = com_path_default;
	
	x = lgw_connect(NULL);
  	if (x == LGW_REG_ERROR) {
       	MSG_ERROR("[main] FAIL TO CONNECT BOARD ON %s\n", com_path);
       	exit(EXIT_FAILURE);
  	}
  	 
  	
  	#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    	MSG_INFO("[main] Little endian host\n");
	#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    	MSG_INFO("[main] Big endian host\n");
	#else
    	MSG_INFO("[main] Host endianness unknown\n");
	#endif
  	x = parse_SX1301_configuration((char *)pvParameters);
  	if (x != 0) {
       	exit(EXIT_FAILURE);
  	}
  
  	MSG_INFO("[main] found global configuration file and parsed correctly\n");
    	wait_ms (2000);

	i = lgw_start();
    	if (i == LGW_HAL_SUCCESS) {
        	MSG_INFO("[main] concentrator started, packet can now be received\n");
    	} else {
        	MSG_ERROR("[main] failed to start the concentrator\n");
        	//exit(EXIT_FAILURE);
    	}
	esp_pthread_cfg_t cfg = {
            (10 * 1024),
            10,
            true
    	};
    	esp_pthread_set_cfg(&cfg);
    	
	i = pthread_create( &thrid_up, NULL, (void * (*)(void *))thread_up, NULL);
    	if (i != 0) {
        	MSG_ERROR("[main] impossible to create upstream thread\n");
        	exit(EXIT_FAILURE);
    	}
    	machine_pygate_set_status(PYGATE_STARTED);
    	mp_printf(&mp_plat_print, "LoRa GW started\n");
    	
    	while (!exit_sig && !quit_sig) {
    		wait_ms ((1000 * stat_interval) / portTICK_PERIOD_MS);
    		pthread_mutex_lock(&mx_meas_up);
        	cp_nb_rx_rcv       = meas_nb_rx_rcv;
        	meas_nb_rx_rcv = 0;
        	pthread_mutex_unlock(&mx_meas_up);
    	#if LORAPF_DEBUG_LEVEL >= LORAPF_INFO_
        	if ( debug_level >= LORAPF_INFO_){
        	mp_printf(&mp_plat_print, "### [UPSTREAM] ###\n");
        	mp_printf(&mp_plat_print, "# RF packets received by concentrator: %u\n", cp_nb_rx_rcv);
    		mp_printf(&mp_plat_print, "##### END #####\n");
    		}
    	#endif	
    		wait_ms(50);
    		
    		report_ready = true;
    	}
	
	pthread_join(thrid_up, NULL);
}

void thread_up(void) {

  MSG_INFO("[up  ] start\n");
  int i;
  unsigned pkt_in_dgram;

  struct lgw_pkt_rx_s rxpkt[NB_PKT_MAX]; /* array containing inbound packets + metadata */
  struct lgw_pkt_rx_s *p; 
  int nb_pkt;

  bool send_report = false;

  /* mote info variables */
  uint32_t mote_addr = 0;
  uint16_t mote_fcnt = 0;
  
   while (!exit_sig && !quit_sig) {
        
        pthread_mutex_lock(&mx_concent);
        nb_pkt = lgw_receive(NB_PKT_MAX, rxpkt);  // Crashing here
        pthread_mutex_unlock(&mx_concent);
	if (nb_pkt == LGW_HAL_ERROR) {
            MSG_ERROR("[up  ] failed packet fetch, exiting\n");
            //exit(EXIT_FAILURE);
        }
        
        if(nb_pkt!=0){
        printf("inicio +%d + fim\n",nb_pkt);
        } 
	
	send_report = report_ready;
	
	if ((nb_pkt == 0) && (send_report == false)) {
            wait_ms ((FETCH_SLEEP_MS));
            continue;
        }
        //printf("pos nb_packet \n");
        

 /* serialize Lora packets metadata and payload */
 	pkt_in_dgram = 0;
        for (i = 0; i < nb_pkt; ++i) {
            p = &rxpkt[i];
	    //printf(p);
            /* Get mote information from current packet (addr, fcnt) */
            /* FHDR - DevAddr */
            mote_addr  = p->payload[1];
            mote_addr |= p->payload[2] << 8;
            mote_addr |= p->payload[3] << 16;
            mote_addr |= p->payload[4] << 24;
            /* FHDR - FCnt */
            mote_fcnt  = p->payload[6];
            mote_fcnt |= p->payload[7] << 8;
            
            //printf("inicio +%d + fim\n",mote_addr);
	    //printf("inicio +%d + fim\n",mote_fcnt);
 	    //wait_ms(5);
		    
	    pthread_mutex_lock(&mx_meas_up);
            meas_nb_rx_rcv += 1;   
            pthread_mutex_unlock(&mx_meas_up);
	}
         if (send_report == true) {
            pthread_mutex_lock(&mx_stat_rep);
            report_ready = false;
            pthread_mutex_unlock(&mx_stat_rep);
        }
     wait_ms (5);
    }
}