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

// Added extra for TCP
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>

#include "protocol_examples_common.h"

/* C++ includes and changes */
extern "C" void app_main(void);
#include <iostream>
#include <eigen3/Eigen/Eigen>
#include <cmath>
#include "esp_heap_caps.h"

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

#define NUM_OBS (128) // HOW MANY DATA POINTS TO GROUP?
#define NUM_VALUES_IN_RAW_CSI (128) // FIXED
#define COMPRESSED_SIZE (64)
#define END_PACKET_COUNT (10000)

char arr1[NUM_OBS*NUM_VALUES_IN_RAW_CSI];
char arr2[NUM_OBS*NUM_VALUES_IN_RAW_CSI];
char arr3[NUM_OBS*COMPRESSED_SIZE];


pthread_mutex_t compressor_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t compressor_condv = PTHREAD_COND_INITIALIZER;
char compressor_pred = 0;

pthread_mutex_t tcp_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t tcp_condv = PTHREAD_COND_INITIALIZER;
char tcp_pred = 0;

void* compressor(void*); // called by callback function
/* Variables used by compressor */
int freq[256];
#define NO_COMP (16)
int cluster_centers[NO_COMP];
char mask[2];

void* tcp_sender(void*); // called by compressor
/* Variables used by tcp_sender */
int tcp_uid = 0;
int tcp_bytes = 0;
char* tcp_data = NULL;
char ip[] = "192.168.4.2";
int port = 8001;
int sock = -1;

/* VARIABLES RELATED TO CALLBACK */
int packet_count = 0;
static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf) {
        ESP_LOGW(TAG, "<%s> wifi_csi_cb", esp_err_to_name(ESP_ERR_INVALID_ARG));
        return;
    }

    if (memcmp(info->mac, ctx, 6)) {
        return;
    }

    int packet_index = packet_count % NUM_OBS;
    memcpy(arr1 + packet_index*NUM_VALUES_IN_RAW_CSI, info->buf, NUM_VALUES_IN_RAW_CSI);
    packet_count++;
    printf("Got packet %d\n", packet_count);
    if(packet_count == END_PACKET_COUNT){
        esp_wifi_stop();
    }
    if(packet_count % NUM_OBS == 0){
        // Copy arr1 to arr2
        assert(sizeof(arr1) == sizeof(arr2));
        pthread_mutex_lock(&compressor_lock);
        memcpy(arr2, arr1, sizeof(arr1));
        pthread_mutex_unlock(&compressor_lock);
        // release commpression thread
        compressor_pred = 1;
        pthread_cond_signal(&compressor_condv);
    }
}

static void wifi_csi_init();
static esp_err_t wifi_ping_router_start();

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

    /* Initialise important stuff first */
    pthread_t compressor_thread;
    pthread_create(&compressor_thread, NULL, compressor, NULL);
    pthread_t tcp_sender_thread;
    pthread_create(&tcp_sender_thread, NULL, tcp_sender, NULL);
    mask[0] = ~0;
    mask[0] = (mask[0] << 4);
    mask[1] = ~mask[0];
    printf("MASKS ARE %d %d\n", mask[0], mask[1]);

    wifi_csi_init();
    wifi_ping_router_start();
    

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0){
        printf("Socket creation failed\n");
        return;
    }

    struct sockaddr_in dest_addr;
    inet_pton(AF_INET, ip, &dest_addr.sin_addr);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    int err = connect(sock, (struct sockaddr *) &dest_addr, sizeof(dest_addr));
    if(err < 0){
        printf("Connection failed\n");
        return;
    }
    printf("Connected to %s : %d succesfully\n", ip, port);
    send(sock, "Hello", 5, 0);
    close(sock);
}

/* Trivial Compressor */
void* compressor(void*){

    char* temp_data = (char*) malloc(NUM_OBS*COMPRESSED_SIZE);
    while(1){
        pthread_mutex_lock(&compressor_lock);
        while(compressor_pred == 0)
            pthread_cond_wait(&compressor_condv, &compressor_lock);

        // DO QUANTISATION HERE
        for(int i=0; i<256; i++)
            freq[i] = 0;
        for(int i=0; i<NUM_OBS*NUM_VALUES_IN_RAW_CSI; i++){
            int num = (int) arr2[i] + 128;
            if(num >= 0 && num < 512){
                freq[num]++;
            }
            else{
                printf("Data point %d is %d\n", i, arr2[i]);
                assert(0);
            }
        }

        int min_val = 0;
        int max_val = 0;
        for(int i=0; i<512; i++){
            if(freq[i]){
                min_val = i;
                break;
            }
        }
        for(int i=511; i>=0; i--){
            if(freq[i]){
                max_val = i;
                break;
            }
        }
        int step = (max_val - min_val) / NO_COMP;
        for(int i=0; i<NO_COMP; i++)
            cluster_centers[i] = min_val + i*step;

        // long long int mse = 0;
        for(int i=0; i<NUM_OBS*NUM_VALUES_IN_RAW_CSI; i++){
            int current_cluster = 0;
            for(int j=1; j<NO_COMP; j++){
                if(abs(arr2[i] - cluster_centers[current_cluster]) > 
                    abs(arr2[i] - cluster_centers[j]))
                    current_cluster = j;
            }
            // int temp = arr2[i] - cluster_centers[current_cluster];
            // mse += temp*temp;
            temp_data[i/2] = mask[i%2] & current_cluster; // THIS CHANGES THE DATA
        }

        pthread_mutex_lock(&tcp_lock);
        memcpy(arr3, temp_data, NUM_OBS*COMPRESSED_SIZE);
        pthread_mutex_unlock(&tcp_lock);

        tcp_data = arr3;
        tcp_bytes = sizeof(arr3);
        tcp_pred = 1;
        pthread_cond_signal(&tcp_condv);
        compressor_pred = 0;
        pthread_mutex_unlock(&compressor_lock);
    }
    free(temp_data);
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

    while(1){
        pthread_mutex_lock(&tcp_lock);
        while(tcp_pred == 0)
            pthread_cond_wait(&tcp_condv, &tcp_lock);
        int bytes_sent = send(sock, tcp_data, tcp_bytes, 0);
        printf("%d bytes of %d sent\n", bytes_sent, tcp_bytes);
        tcp_pred = 0;
        pthread_mutex_unlock(&tcp_lock);
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
