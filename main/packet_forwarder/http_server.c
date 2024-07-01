/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2023 Semtech

Description:
    Http service implementation for configure from browser

License: Revised BSD License, see LICENSE.TXT file include in the project
*/
#include <sys/stat.h>

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_tls_crypto.h"
#include <esp_http_server.h>

#include "http_server.h"
#include "web_config.h"
#include "loragw_aux.h"

static const char *TAG = "esp32 web server";
static bool black_theme_flag = true;

typedef struct {
    char    *username;
    char    *password;
} basic_auth_info_t;
basic_auth_info_t web_auth_info;

char web_username[32] = BASIC_AUTH_USERNAME;
char web_password[32] = BASIC_AUTH_PASSWORD;

char resp_buf[10240];
extern config_s config[CONFIG_NUM];

#ifdef ENABLE_HTML_AUTH
static char *http_auth_basic(const char *username, const char *password)
{
    int out;
    char *user_info = NULL;
    char *digest = NULL;
    size_t n = 0;

    asprintf(&user_info, "%s:%s", username, password);
    if (!user_info) {
        ESP_LOGE(TAG, "No enough memory for user information");
        return NULL;
    }
    esp_crypto_base64_encode(NULL, 0, &n, (const unsigned char *)user_info, strlen(user_info));

    /* 6: The length of the "Basic " string
     * n: Number of bytes for a base64 encode format
     * 1: Number of bytes for a reserved which be used to fill zero
    */
    digest = calloc(1, 6 + n + 1);
    if (digest) {
        strcpy(digest, "Basic ");
        esp_crypto_base64_encode((unsigned char *)digest + 6, n, (size_t *)&out, (const unsigned char *)user_info, strlen(user_info));
    }

    free(user_info);
    return digest;
}

static esp_err_t check_basic_auth(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buf_len = 0;
    basic_auth_info_t *basic_auth_info = &web_auth_info;

    buf_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (buf_len == 1) {
        ESP_LOGE(TAG, "No auth header received");
        return ESP_FAIL;
    }

    buf = calloc(1, buf_len);
    if (!buf) {
        ESP_LOGE(TAG, "No enough memory for basic authorization");
        return ESP_ERR_NO_MEM;
    }

    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, buf_len) == ESP_OK) {
        ESP_LOGI(TAG, "Found header => Authorization: %s", buf);
    } else {
        ESP_LOGE(TAG, "No auth value received");
        free(buf);
        return ESP_FAIL;
    }

    char *auth_credentials = http_auth_basic(basic_auth_info->username,
            basic_auth_info->password);
    if (!auth_credentials) {
        ESP_LOGE(TAG, "No enough memory for basic authorization credentials");
        free(buf);
        return ESP_ERR_NO_MEM;
    }

    if (strncmp(auth_credentials, buf, buf_len)) {
        ESP_LOGE(TAG, "Authenticated failed");
        free(auth_credentials);
        free(buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Authenticated!");
    free(auth_credentials);
    free(buf);
    return ESP_OK;
}


static esp_err_t handle_basic_auth(httpd_req_t *req)
{
    esp_err_t err = check_basic_auth(req);
    if(err == ESP_FAIL){
        ESP_LOGE(TAG, "No auth header received");
        httpd_resp_set_status(req, HTTPD_401);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "keep-alive");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"\"");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    return ESP_OK;
}
#endif

char *assemble_webpage(const char *webpage_str)
{
    char *last = (char *)webpage_str, *fwd, *buf = resp_buf;
    char config_buf[80];
    int href_len = 16;  // 16 is the length of '<a href="cn470">'

    if(config[WIFI_MODE].val != NULL){
        if(strncmp(config[WIFI_MODE].val, "soft_ap", config[WIFI_MODE].len) == 0){
            fwd = strstr(last, "value='soft_ap'");
            if(fwd){
                strncpy(buf, last, fwd - last);
                buf += fwd - last;
                last = fwd;
                strncpy(buf, "checked ", 9);
                buf += 8;
            }
        }
        else if(strncmp(config[WIFI_MODE].val, "station", config[WIFI_MODE].len) == 0){
            fwd = strstr(last, "value='station'");
            if(fwd){
                strncpy(buf, last, fwd - last);
                buf += fwd - last;
                last = fwd;
                strncpy(buf, "checked ", 9);
                buf += 8;
            }
        }
        else if(strncmp(config[WIFI_MODE].val, "eth_net", config[WIFI_MODE].len) == 0){
            fwd = strstr(last, "value='eth_net'");
            if(fwd){
                strncpy(buf, last, fwd - last);
                buf += fwd - last;
                last = fwd;
                strncpy(buf, "checked ", 9);
                buf += 8;
            }
        }
    }

    if(config[FREQ_REGION].val != NULL){
        if(strncmp(config[FREQ_REGION].val, "cn470", config[FREQ_REGION].len) == 0){
            fwd = strstr(last, ">CN470");
            if(fwd){
                fwd -= href_len;
                strncpy(buf, last, fwd - last);
                buf += fwd - last;
                last = fwd;
                strncpy(buf, " checked", 9);
                buf += 8;
            }
        }
        else if(strncmp(config[FREQ_REGION].val, "eu868", config[FREQ_REGION].len) == 0){
            fwd = strstr(last, ">EU868");
            if(fwd){
                fwd -= href_len;
                strncpy(buf, last, fwd - last);
                buf += fwd - last;
                last = fwd;
                strncpy(buf, " checked", 9);
                buf += 8;
            }
        }
        else if(strncmp(config[FREQ_REGION].val, "us915", config[FREQ_REGION].len) == 0){
            fwd = strstr(last, ">US915");
            if(fwd){
                fwd -= href_len;
                strncpy(buf, last, fwd - last);
                buf += fwd - last;
                last = fwd;
                strncpy(buf, " checked", 9);
                buf += 8;
            }
        }
    }

    if(config[FREQ_RADIO0].val != NULL){
        fwd = strstr(last, "name='freq_radio0'");
        if(fwd){
            strncpy(buf, last, fwd - last);
            buf += fwd - last;
            last = fwd;
            sprintf(config_buf, "value='%s' ", config[FREQ_RADIO0].val);
            strncpy(buf, config_buf, strlen(config_buf));
            buf += strlen(config_buf);
        }
    }
    if(config[FREQ_RADIO1].val != NULL){
        fwd = strstr(last, "name='freq_radio1'");
        if(fwd){
            strncpy(buf, last, fwd - last);
            buf += fwd - last;
            last = fwd;
            sprintf(config_buf, "value='%s' ", config[FREQ_RADIO1].val);
            strncpy(buf, config_buf, strlen(config_buf));
            buf += strlen(config_buf);
        }
    }

    if(config[WIFI_SSID].val != NULL){
        fwd = strstr(last, "name='wifi_ssid'");
        if(fwd){
            strncpy(buf, last, fwd - last);
            buf += fwd - last;
            last = fwd;
            sprintf(config_buf, "value='%s' ", config[WIFI_SSID].val);
            strncpy(buf, config_buf, strlen(config_buf));
            buf += strlen(config_buf);
        }
    }

    if(config[WIFI_PASSWORD].val != NULL){
        fwd = strstr(last, "name='wifi_pswd'");
        if(fwd){
            strncpy(buf, last, fwd - last);
            buf += fwd - last;
            last = fwd;
            sprintf(config_buf, "placeholder='(password has been set)' ");  // don't show password
            strncpy(buf, config_buf, strlen(config_buf));
            buf += strlen(config_buf);
        }
    }

    if(config[NS_HOST].val != NULL){
        fwd = strstr(last, "name='ns_host'");
        if(fwd){
            strncpy(buf, last, fwd - last);
            buf += fwd - last;
            last = fwd;
            sprintf(config_buf, "value='%s' ", config[NS_HOST].val);
            strncpy(buf, config_buf, strlen(config_buf));
            buf += strlen(config_buf);
        }
    }

    if(config[NS_PORT].val != NULL){
        fwd = strstr(last, "name='ns_port'");
        if(fwd){
            strncpy(buf, last, fwd - last);
            buf += fwd - last;
            last = fwd;
            sprintf(config_buf, "value='%s' ", config[NS_PORT].val);
            strncpy(buf, config_buf, strlen(config_buf));
            buf += strlen(config_buf);
        }
    }

    if(config[GW_ID].val != NULL){
        fwd = strstr(last, "name='gw_id'");
        if(fwd){
            strncpy(buf, last, fwd - last);
            buf += fwd - last;
            last = fwd;
            sprintf(config_buf, "value='%s' ", config[GW_ID].val);
            strncpy(buf, config_buf, strlen(config_buf));
            buf += strlen(config_buf);
        }
    }

    // copy the tail content
    strcpy(buf, last);
    return (char *)resp_buf;
}

static esp_err_t gw_config_handler(httpd_req_t *req)
{
#ifdef ENABLE_HTML_AUTH
    char*  buf;
    size_t buf_len;
    esp_err_t err;

    err = handle_basic_auth(req);
    if(err == ESP_FAIL)
        return err;

    /* Get header value string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        /* Copy null terminated value string into buffer */
        if (httpd_req_get_hdr_value_str(req, "Host", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Host: %s", buf);
        }
        free(buf);
    }
#endif

    FILE* f = NULL;
    char* web_path = NULL;
    char* web_data = NULL;

    if(black_theme_flag == true)
        web_path = "/spiffs/webpage.html";
    else
        web_path = "/spiffs/webpage_light_theme.html";

    // resp_str = assemble_webpage(webpage_str);
    struct stat st;
    if (stat(web_path, &st) == 0) {
        ESP_LOGI("web", "File size of '%s' is %lu bytes.", web_path, st.st_size);
    } else {
        ESP_LOGE("web", "Failed to get file info for '%s'", web_path);
    }

    f = fopen(web_path, "r");
    web_data = malloc(st.st_size);
    fread(web_data,1, st.st_size, f);
    fclose(f);

    /* Send response with custom headers and body */
    const char *resp_str = NULL;
    resp_str = assemble_webpage(web_data);

    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    free(web_data);
    return ESP_OK;
}

// for 'black' background theme
static esp_err_t b_gw_config_handler(httpd_req_t *req)
{
    black_theme_flag = true;
    return gw_config_handler(req);
}

// for 'white' background theme
static esp_err_t w_gw_config_handler(httpd_req_t *req)
{
    black_theme_flag = false;
    return gw_config_handler(req);
}

static esp_err_t gw_response_handler(httpd_req_t *req)
{
    char buf[256];

#ifdef ENABLE_HTML_AUTH
    esp_err_t err = handle_basic_auth(req);
    if(err == ESP_FAIL)
        return err;
#endif

    size_t recv_size = MIN(req->content_len, sizeof(buf)-1);
    int ret = httpd_req_recv(req, buf, recv_size);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    ESP_LOGI(TAG, "Found Data: %s", buf);

    extract_data_items(buf);
    save_config();

    const char *resp_str = NULL;
    if(black_theme_flag == true){
        resp_str = (const char *) "<html><head><meta http-equiv='refresh' content=\"4; URL=/\" /></head>"
                                "<body><center>Config applied.</center><br><br><center>Back in seconds...</center><br><br>"
                                "<form action='/' method='get' style='text-align: center'>"
                                    "<button type='submit' name='back'>Back</button>"
                                "</form></body></html>";
    } else {
        resp_str = (const char *) "<html><head><meta http-equiv='refresh' content=\"4; URL=/w\" /></head>"
                                "<body><center>Config applied.</center><br><br><center>Back in seconds...</center><br><br>"
                                "<form action='/w' method='get' style='text-align: center'>"
                                    "<button type='submit' name='back'>Back</button>"
                                "</form></body></html>";
    }

    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t gw_reboot_handler(httpd_req_t *req)
{
#ifdef ENABLE_HTML_AUTH
    esp_err_t err = handle_basic_auth(req);
    if(err == ESP_FAIL)
        return err;
#endif

    ESP_LOGW(TAG, "Reboot required");

    const char *resp_str = NULL;
    if(black_theme_flag == true){
        resp_str = (const char *) "<html><head><meta http-equiv='refresh' content=\"6; URL=/\" /></head>"
                                "<body><center>Gateway is reboot...</center><br><br><center>Waiting for 6 seconds...</center></body></html>";
    } else {
        resp_str = (const char *) "<html><head><meta http-equiv='refresh' content=\"6; URL=/w\" /></head>"
                                "<body><center>Gateway is reboot...</center><br><br><center>Waiting for 6 seconds...</center></body></html>";
    }

    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    wait_ms(500);
    esp_restart();

    return ESP_OK;
}

char json_conf_buf[1024*5];  // 5K should be enough
static void assemble_json_str(char *str)
{
    int len = strlen(str);

    strcpy(json_conf_buf, "<pre>");
    strcpy(json_conf_buf+5, str);
    strcpy(json_conf_buf+5+len, "</pre>");
    json_conf_buf[5+len+6] = '\0';
}

static esp_err_t gw_json_conf_handler(httpd_req_t *req)
{
#ifdef ENABLE_HTML_AUTH
    esp_err_t err = handle_basic_auth(req);
    if(err == ESP_FAIL)
        return err;
#endif

    char *conf_array = NULL;
    FILE* f = NULL;
    char* global_conf_path = NULL;

    if(strncmp((const char *) req->user_ctx, "cn470", 5) == 0)
        global_conf_path = "/spiffs/global_conf/cn490.json";
    else if(strncmp((const char *) req->user_ctx, "eu868", 5) == 0)
        global_conf_path = "/spiffs/global_conf/eu868.json";
    else if(strncmp((const char *) req->user_ctx, "us915", 5) == 0)
        global_conf_path = "/spiffs/global_conf/us915.json";

    struct stat st;
    // 获取文件信息（包含大小）
    if (stat(global_conf_path, &st) == 0) {
        ESP_LOGI(TAG, "File size of '%s' is %lu bytes.", global_conf_path, st.st_size);
    } else {
        ESP_LOGE(TAG, "Failed to get file info for '%s'", global_conf_path);
        return ESP_FAIL;
    }
   
    conf_array = malloc(st.st_size);
    f = fopen(global_conf_path, "r");
    fread(conf_array,1,st.st_size,f);
    fclose(f);
    assemble_json_str((char *)conf_array);  // ignore the first 2 bytes which is the length
    httpd_resp_send(req, (const char *)json_conf_buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Default: black theme. 'b' means 'black' background.
static const httpd_uri_t gw_config = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = b_gw_config_handler,
    .user_ctx  = "Hello ESXP1302 Gateway!"
};

// light theme. 'w' means 'white' background
static const httpd_uri_t w_gw_config = {
    .uri       = "/w",
    .method    = HTTP_GET,
    .handler   = w_gw_config_handler,
    .user_ctx  = "Hello ESXP1302 Gateway!"
};

// response
static const httpd_uri_t resp_config = {
    .uri       = "/resp",
    .method    = HTTP_POST,
    .handler   = gw_response_handler,
    .user_ctx  = "Response"
};

// reboot
static const httpd_uri_t reboot_config = {
    .uri       = "/reboot",
    .method    = HTTP_POST,
    .handler   = gw_reboot_handler,
    .user_ctx  = "ESP32 is rebooting"
};

// return cn470 json config
static const httpd_uri_t cn470_json_conf = {
    .uri       = "/cn470",
    .method    = HTTP_GET,
    .handler   = gw_json_conf_handler,
    .user_ctx  = "cn470"
};

// return eu868 json config
static const httpd_uri_t eu868_json_conf = {
    .uri       = "/eu868",
    .method    = HTTP_GET,
    .handler   = gw_json_conf_handler,
    .user_ctx  = "eu868"
};

// return us915 json config
static const httpd_uri_t us915_json_conf = {
    .uri       = "/us915",
    .method    = HTTP_GET,
    .handler   = gw_json_conf_handler,
    .user_ctx  = "us915"
};

static httpd_handle_t start_web_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");

        web_auth_info.username = web_username;
        web_auth_info.password = web_password;

        httpd_register_uri_handler(server, &gw_config);
        httpd_register_uri_handler(server, &w_gw_config);
        httpd_register_uri_handler(server, &resp_config);
        httpd_register_uri_handler(server, &reboot_config);
        httpd_register_uri_handler(server, &cn470_json_conf);
        httpd_register_uri_handler(server, &eu868_json_conf);
        httpd_register_uri_handler(server, &us915_json_conf);

        return server;
    }

    ESP_LOGI(TAG, "Error starting web server!");
    return NULL;
}

static void stop_web_server(httpd_handle_t server)
{
    httpd_stop(server);
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;

    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting esp32 internal web server");
        *server = start_web_server();
    }
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;

    if (*server) {
        ESP_LOGI(TAG, "Stopping esp32 internal web server");
        stop_web_server(*server);
        *server = NULL;
    }
}

void http_server_task(void *pvParameters)
{
    static httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
    //ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnect_handler, &server));

    /* Start the server for the first time */
    server = start_web_server();

    vTaskDelete(NULL);

    while(true){
        printf("http server ended\n");
        vTaskDelay(8000 / portTICK_PERIOD_MS);
    }
}
