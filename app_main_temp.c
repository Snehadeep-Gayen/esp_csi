/* Get recv router csi

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
/* Snehadeep */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "nvs_flash.h"

#include "esp_mac.h"
#include "rom/ets_sys.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_now.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#include <pthread.h>
#include <math.h>

// Added extra for TCP
// #include <sys/socket.h>
// #include <arpa/inet.h>
// #include <errno.h>
// #include <netdb.h>

#include "protocol_examples_common.h"

/* C++ includes and changes */
extern "C" void app_main(void);
// #include <eigen3/Eigen/Eigen>
// #include <math.h>
#include "esp_heap_caps.h"

#define TF_LITE_SHOW_MEMORY_USE (1)

/* TFLITE */
// #include "model5_tflite.cc"
// #include "model2_tflite.cc"
// #include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
// #include "tensorflow/lite/micro/micro_interpreter.h"
// #include "tensorflow/lite/micro/system_setup.h"
// #include "tensorflow/lite/schema/schema_generated.h"



/*****************************/
#define CONFIG_SEND_FREQUENCY      100

static const char *TAG = "csi_recv_router";


/*
############# THREE STAGE PIPELINE ##############
||||||||| ======> ||||||||||| ======> ||||||||||| 
 Array1              Array2             Array3

Callback           Compression          TCP sends
writes              happens             this
here                on this
#################################################
*/

#define NUM_VALUES_IN_RAW_CSI (128)
#define END_PACKET_COUNT (256000)
#define COMPRESSED_FLOATS (20)

/* VARIABLES RELATED TO CALLBACK */
int packet_count;
int8_t raw_data[NUM_VALUES_IN_RAW_CSI];

/* Preprocessed output */
float preprocessed_data[52];

/* compressed output */
pthread_mutex_t preprocess_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t preprocess_condv = PTHREAD_COND_INITIALIZER;
char preprocess_pred = 0;

pthread_mutex_t tcp_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t tcp_condv = PTHREAD_COND_INITIALIZER;
char tcp_pred = 0;

void* preprocess(void*); // called by callback function
// void* interpreting(void*); // called by preprocess
void* tcp_sender(void*); // called by interpreting

/* Variables used by tcp_sender */
char ip[] = "192.168.4.3";
int port = 8001;
int sock = -1;

void print_heap(){
    printf("Largest free block: %d\n", 
    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    fflush(stdout);
    // heap_caps_dump_all();
}

/* VARIABLES RELATED TO CALLBACK */
static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf) {
        ESP_LOGW(TAG, "<%s> wifi_csi_cb", esp_err_to_name(ESP_ERR_INVALID_ARG));
        return;
    }

    if (memcmp(info->mac, ctx, 6)) {
        return;
    }

    printf("Packet received\n");
    pthread_mutex_lock(&preprocess_lock);
    memcpy(raw_data, info->buf, NUM_VALUES_IN_RAW_CSI);
    pthread_mutex_unlock(&preprocess_lock);
    preprocess_pred = 1;
    pthread_cond_signal(&preprocess_condv);
    // printf("Packet done\n");
}

static void wifi_csi_init();
static esp_err_t wifi_ping_router_start();

// int8_t feature_buffer[kFeatureElementCount];
// int8_t* model_input_buffer = nullptr;

void app_main()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /**
     * @brief This helper function configures Wi-Fi, as selected in menuconfig.
     *        Read "Establishing Wi-Fi Connection" section in esp-idf/examples/protocols/README.md
     *        for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    pthread_t preprocess_thread;
    pthread_create(&preprocess_thread, NULL, preprocess, NULL);

    pthread_t tcp_thread;
    pthread_create(&tcp_thread, NULL, tcp_sender, NULL);

    print_heap();

    wifi_csi_init();
    wifi_ping_router_start();

    print_heap();
}

void* preprocess(void*){
    while(1){
        pthread_mutex_lock(&preprocess_lock);
        while(preprocess_pred == 0)
            pthread_cond_wait(&preprocess_condv, &preprocess_lock);
        preprocess_pred = 0;
        // printf("Packet processing started\n");    
        pthread_mutex_lock(&tcp_lock);
        for(int i=6; i<59; i++){
            if(i==32)
                continue;
            int index = i-6;
            if(i>32)
                index = i-7;
            preprocessed_data[index] = sqrt(((float)raw_data[2*i])*raw_data[2*i] + 
                                        ((float)raw_data[2*i+1])*raw_data[2*i+1]);
            // printf("%d : %f\n", index, preprocessed_data[index]);
        }
        // printf("Packet processing ended\n");    
        pthread_mutex_unlock(&tcp_lock);
        pthread_mutex_unlock(&preprocess_lock);
        tcp_pred = 1;
        pthread_cond_signal(&tcp_condv);
    }
    return NULL;
}

/* TCP Sender */
void* tcp_sender(void*){

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0){
        printf("Socket creation failed\n");
        return NULL;
    }

    struct sockaddr_in* dest_addr = 
            (struct sockaddr_in*) malloc(sizeof(struct sockaddr_in));
    inet_pton(AF_INET, ip, &(dest_addr->sin_addr));
    dest_addr->sin_family = AF_INET;
    dest_addr->sin_port = htons(port);
    int err = connect(sock, (struct sockaddr *) dest_addr, sizeof(struct sockaddr_in));
    // // printf("Trying to connect\n");
    if(err < 0){
        printf("Connection failed\n");
        return NULL;
    }
    printf("Connected to %s : %d succesfully\n", ip, port);
    int time_bytes = sizeof(unsigned long);

    while(1){
        pthread_mutex_lock(&tcp_lock);
        while(tcp_pred == 0)
            pthread_cond_wait(&tcp_condv, &tcp_lock);
        unsigned long currentMillis = xTaskGetTickCount();
        assert(time_bytes == send(sock, &currentMillis, time_bytes, 0));
        int bytes_sent = send(sock, preprocessed_data, sizeof(preprocessed_data), 0);
        tcp_pred = 0;
        pthread_mutex_unlock(&tcp_lock);
        printf("%d bytes of %d sent at %ld\n", bytes_sent, sizeof(preprocessed_data), currentMillis);
    }
    close(sock);
    free(dest_addr);
    return NULL;
}

static void wifi_csi_init()
{
    /**
     * @brief In order to ensure the compatibility of routers, only LLTF sub-carriers are selected.
     */
    wifi_csi_config_t csi_config = {
        .lltf_en           = true,
        .htltf_en          = false,
        .stbc_htltf2_en    = false,
        .ltf_merge_en      = true,
        .channel_filter_en = true,
        .manu_scale        = true,
        .shift             = true,
    };

    static wifi_ap_record_t s_ap_info = {0};
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&s_ap_info));
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, s_ap_info.bssid));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
}

static esp_err_t wifi_ping_router_start()
{
    static esp_ping_handle_t ping_handle = NULL;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.count             = 0;
    ping_config.interval_ms       = 1000 / CONFIG_SEND_FREQUENCY;
    ping_config.task_stack_size   = 3072;
    ping_config.data_size         = 1;

    esp_netif_ip_info_t local_ip;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &local_ip);
    ESP_LOGI(TAG, "got ip:" IPSTR ", gw: " IPSTR, IP2STR(&local_ip.ip), IP2STR(&local_ip.gw));
    ping_config.target_addr.u_addr.ip4.addr = ip4_addr_get_u32(&local_ip.gw);
    ping_config.target_addr.type = ESP_IPADDR_TYPE_V4;

    esp_ping_callbacks_t cbs = { 0 };
    esp_ping_new_session(&ping_config, &cbs, &ping_handle);
    esp_ping_start(ping_handle);

    return ESP_OK;
}
