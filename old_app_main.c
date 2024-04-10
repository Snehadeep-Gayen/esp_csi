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

#define QUANTISATION 1
#define NO_COMPRESSION 1


#define CONFIG_SEND_FREQUENCY      500

static const char *TAG = "csi_recv_router";

#define NUM_AMPLITUDES (128)
#define NUM_PACKETS (128)
#define NUM_DATA_POINTS (NUM_AMPLITUDES*NUM_PACKETS)
#define SEND_TO_PC 1

int data_index = 0;
#ifdef QUANTISATION
char incoming_data[NUM_DATA_POINTS]; // assuming info->len = 128
char data[NUM_DATA_POINTS]; // assuming info->len = 128
int8_t assignments[NUM_DATA_POINTS];
#define NO_COMP (4)
int cluster_centers[NO_COMP];
int freq[512] = {0};
#else
Eigen::MatrixXf incoming_data(NUM_PACKETS, NUM_AMPLITUDES);
Eigen::MatrixXf data(NUM_PACKETS, NUM_AMPLITUDES);
#define NO_COMP (4)
#endif
#ifdef SEND_TO_PC
char ip[] = "192.168.4.4";
int port = 8001;
pthread_mutex_t array_free = PTHREAD_MUTEX_INITIALIZER;
struct array{
    char* ptr;
    int size;
    int uid;
};
#endif

int collected_packets = 0;

int abs(int x){
    if(x>=0)
        return x;
    return -x;
}

pthread_mutex_t raw_data_ready = PTHREAD_MUTEX_INITIALIZER;

void* compressor(void* arg){
    /* SNEHADEEP */
    // acquire the mutex
    pthread_mutex_lock(&raw_data_ready);

#ifndef QUANTISATION
    // std::cout<<"Data is :\n"<<data<<std::endl;    
    // Calculate the mean of each feature (column)
    // heap_caps_dump_all();
    Eigen::VectorXf* mean = new Eigen::VectorXf(NUM_AMPLITUDES);
    for(int i=0; i<NUM_AMPLITUDES; i++){
        float sum = 0;
        for(int j=0; j<NUM_PACKETS; j++){
            sum += data(j, i);
        }
        (*mean)(i) = sum/NUM_PACKETS;
    }
    // Center the data by subtracting the mean from each feature
    for(int i=0; i<NUM_AMPLITUDES; i++){
        for(int j=0; j<NUM_PACKETS; j++){
            data(j, i) -= (*mean)(i);
        }
    }
    Eigen::MatrixXf* Cov = new Eigen::MatrixXf(NUM_AMPLITUDES, NUM_AMPLITUDES);
    // heap_caps_dump_all();
    for(int i=0; i<NUM_AMPLITUDES; i++){
        for(int j=i; j<NUM_AMPLITUDES; j++){
            float sum = 0;
            for(int k=0; k<NUM_PACKETS; k++){
                sum += data(k, i) * data(k, j);
            }
            (*Cov)(i, j) = sum/NUM_PACKETS;
            (*Cov)(j, i) = sum/NUM_PACKETS;
        }
    }
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXf>* eigensolver = 
        new Eigen::SelfAdjointEigenSolver<Eigen::MatrixXf>(*Cov);
    if(eigensolver->info() != Eigen::Success){
        printf("Eigen Computation failed\n");
        return NULL;
    }
    std::cout<<"EigenValues are "<<eigensolver->eigenvalues()<<std::endl;
    std::cout<<"EigenVectors are "<<eigensolver->eigenvectors()<<std::endl;
    // Calculate projections on number of components
    Eigen::MatrixXf* components = new Eigen::MatrixXf(NUM_PACKETS, NO_COMP);
    for(int i=0; i<NUM_PACKETS; i++){
        for(int j=0; j<NO_COMP; j++){
            float sum = 0;
            int eigindex = NUM_AMPLITUDES - 1 - j;
            for(int k=0; k<NUM_AMPLITUDES; k++){
                sum += data(i, k) * eigensolver->eigenvectors()(eigindex, k);
            }
            (*components)(i, j) = sum;
        }
    }
    float compression_ratio = NUM_AMPLITUDES * NUM_PACKETS / 
        (NO_COMP * (NUM_AMPLITUDES + NUM_PACKETS) + NUM_AMPLITUDES);
    printf("Compression Ratio: %f\n", compression_ratio);
    delete mean;
    delete Cov;
    delete eigensolver;
    delete components;
#else


    /* Add clustered values */
    for(int i=0; i<256; i++)
        freq[i] = 0;
    for(int i=0; i<NUM_DATA_POINTS; i++){
        int num = (int) data[i] + 128;
        if(num >= 0 && num < 512){
            freq[num]++;
        }
        else{
            printf("Data point %d is %d\n", i, data[i]);
            assert(0);
        }
    }
    // for(int i=0; i<512; i++){
    //     if(freq[i]==0)
    //         continue;
    //     printf("%d\t|\t %d\n", i, freq[i]);
    // }


    printf("Compression Ratio: %f\n", 256.0 * NUM_DATA_POINTS/(NO_COMP * 8.0 + NO_COMP * NUM_DATA_POINTS));

    /*  UNIFORM QUANTISATION */
    if(QUANTISATION == 0)
    {
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
        for(int i=0; i<NO_COMP; i++){
            cluster_centers[i] = min_val + i*step;
        }
        /* Print the cluster centers and MSE */
        long long int mse = 0;
        for(int i=0; i<NUM_DATA_POINTS; i++){
            int current_cluster = 0;
            for(int j=1; j<NO_COMP; j++){
                if(abs(data[i] - cluster_centers[current_cluster]) > 
                    abs(data[i] - cluster_centers[j]))
                    current_cluster = j;
            }
            int temp = data[i] - cluster_centers[current_cluster];
            mse += temp*temp;
            // data[i] = current_cluster; // THIS CHANGES THE DATA
        }
        printf("Number of data points: %d\n", NUM_DATA_POINTS);
        printf("[Uniform] Average MSE: %lld\n", mse/NUM_DATA_POINTS);

    }
    
    /* KMEANS */
    if(QUANTISATION == 1)
    {
        long long int mse = 0;

        /* Initialisation */
        assert(2 * NO_COMP < NUM_DATA_POINTS); // for faster initialisation this is necessary
        int cluster_indices[NO_COMP];
        srand(1978);
        int index = 0;
        while(index < NO_COMP){
            cluster_indices[index] = (rand() % NUM_DATA_POINTS);
            for(int i=0; i<index; i++)
                if(cluster_indices[i]==cluster_indices[index]){
                    index--;
                    break;
                }
            index++;
        }
        for(int i=0; i<NO_COMP; i++)
            cluster_centers[i] = data[cluster_indices[i]];

        int no_iterations = 0;
        int max_iterations = 100;
        while(1){

            bool nochange = true;

            // Assign each point to its closest cluster center
            for(int i=0; i<NUM_DATA_POINTS; i++){
                int closest = 0;
                int closest_dist = abs(data[i] - cluster_centers[0]);
                for(int j=1; j<NO_COMP; j++){
                    int dist = abs(data[i] - cluster_centers[j]);
                    if(dist < closest_dist){
                        closest = j;
                        closest_dist = dist;
                    }
                }
                if(no_iterations == 0 || assignments[i]!=closest){
                    nochange = false;
                    assignments[i] = closest;
                }
            }

            if(nochange)
                break;

            // recompute the cluster centers
            long long int cluster_sums[NO_COMP];
            int cluster_size[NO_COMP];
            for(int i=0; i<NO_COMP; i++)
                cluster_sums[i] = cluster_size[i] = 0;
            for(int i=0; i<NUM_DATA_POINTS; i++){
                cluster_sums[assignments[i]] += data[i];
                cluster_size[assignments[i]]++;
            }
            for(int i=0; i<NO_COMP; i++)
                if(cluster_size[i] != 0)
                    cluster_centers[i] = (int) (cluster_sums[i]/cluster_size[i]);
            
            // increment count
            no_iterations++;
            if(no_iterations==max_iterations)
                break;
        }
        // Calculate mse
        mse = 0;
        for(int i=0; i<NUM_DATA_POINTS; i++){
            int temp = abs(data[i] - cluster_centers[assignments[i]]);
            mse += temp * temp;
        }
        printf("[KMeans] Average MSE %lld in %d iterations\n", mse/NUM_DATA_POINTS, no_iterations);
    }
#endif

    // release the mutex
    pthread_mutex_unlock(&raw_data_ready);


    return NULL;
}

#ifdef SEND_TO_PC
void* tcp_sender(void* arg){
    struct array* data_struct = (struct array*) arg;
    char* data_pts = data_struct->ptr;
    int total_bytes = data_struct->size;
    int uid = data_struct->uid;
    // for(int i=0; i<total_bytes; i++)
    //     printf("%d ", data_pts[i]);
    // printf("\n\n");
    free(arg);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0){
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return NULL;
    }

    struct sockaddr_in dest_addr;
    inet_pton(AF_INET, ip, &dest_addr.sin_addr);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    int err = connect(sock, (struct sockaddr *) &dest_addr, sizeof(dest_addr));
    if(err < 0){
        ESP_LOGE(TAG, "Socket unable to connect: errorno %d", errno);
        return NULL;
    }
    printf("Connected to %s : %d succesfully\n", ip, port);

    pthread_mutex_lock(&array_free);
    printf("%dth uid tcp_sender acquired lock\n", uid);
    printf("%d bytes send\n", send(sock, data_pts, total_bytes, 0));
    printf("%dth uid tcp_sender released lock\n", uid);
    close(sock);
    pthread_mutex_unlock(&array_free);
    return NULL;
}
#endif

static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf) {
        ESP_LOGW(TAG, "<%s> wifi_csi_cb", esp_err_to_name(ESP_ERR_INVALID_ARG));
        return;
    }

    if (memcmp(info->mac, ctx, 6)) {
        return;
    }

    // static int s_count = 0;
    const wifi_pkt_rx_ctrl_t *rx_ctrl = &info->rx_ctrl;

    // if (!s_count) {
    //     ESP_LOGI(TAG, "================ CSI RECV ================");
    //     // ets_printf("type,seq,mac,rssi,rate,sig_mode,mcs,bandwidth,smoothing,not_sounding,aggregation,stbc,fec_coding,sgi,noise_floor,ampdu_cnt,channel,secondary_channel,local_timestamp,ant,sig_len,rx_state,len,first_word,data\n");
    // }

    /** Only LLTF sub-carriers are selected. */
    info->len = 128;
    collected_packets++;
    printf("Packet %d\n", collected_packets);
    // if(collected_packets==1000){
    //     printf("Exiting...");
    //     sleep(2);
    //     exit(0);
    // }


    // printf("CSI_DATA,%d," MACSTR ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
    //         s_count++, MAC2STR(info->mac), rx_ctrl->rssi, rx_ctrl->rate, rx_ctrl->sig_mode,
    //         rx_ctrl->mcs, rx_ctrl->cwb, rx_ctrl->smoothing, rx_ctrl->not_sounding,
    //         rx_ctrl->aggregation, rx_ctrl->stbc, rx_ctrl->fec_coding, rx_ctrl->sgi,
    //         rx_ctrl->noise_floor, rx_ctrl->ampdu_cnt, rx_ctrl->channel, rx_ctrl->secondary_channel,
    //         rx_ctrl->timestamp, rx_ctrl->ant, rx_ctrl->sig_len, rx_ctrl->rx_state);

    // printf(",%d,%d,\"[%d", info->len, info->first_word_invalid, info->buf[0]);

    // for (int i = 1; i < info->len; i++) {
    //     printf(",%d", info->buf[i]);
    // }

    // printf("]\"\n");
#ifndef NO_COMPRESSION
    /* Copy the data to global array */
    if(data_index < NUM_PACKETS){
#ifdef QUANTISATION
        /* For Quantisation */
        for(int i=0; i<info->len; i++)
            incoming_data[data_index*NUM_PACKETS + i] = info->buf[i];
#else
        /* For PCA */
        for(int i=0; i<info->len; i+=2){
            float f1 = info->buf[i];
            float f2 = info->buf[i+1];
            incoming_data(data_index, i/2) = sqrt(f1*f1 + f2*f2);
        }
#endif
        data_index++;
    }
    if(data_index < NUM_PACKETS)
        return;

    int result = pthread_mutex_trylock(&raw_data_ready);
    if(result != 0){
        printf("Mutex is locked\n");
        return;
    }
    // copy the incoming_data to data
#ifdef QUANTISATION
    for(int i=0; i<NUM_DATA_POINTS; i++)
        data[i] = incoming_data[i];
#else
    for(int i=0; i<NUM_PACKETS; i++)
        for(int j=0; j<NUM_AMPLITUDES; j++)
            data(i, j) = incoming_data(i, j);
#endif
    pthread_mutex_unlock(&raw_data_ready);
    // compressor();
    // create thread to run the compressor
    pthread_t compressor_thread;
    // set stack size to 10KB
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t stacksize;
    pthread_attr_getstacksize(&attr, &stacksize);
    pthread_create(&compressor_thread, &attr, compressor, NULL);
    data_index = 0;
#endif

#ifdef SEND_TO_PC
    if(collected_packets%NUM_PACKETS!=0){
        int index = collected_packets % NUM_PACKETS;
        for(int i=0; i<info->len; i++){
            if((index*NUM_AMPLITUDES + i)*sizeof(char) >= sizeof(incoming_data)){
                printf("i = %d\nindex: %d\nNUM_AMPLITUDES: %d\n sizeof(incoming_data): %d\n",
                    i, index, NUM_AMPLITUDES, sizeof(incoming_data));
                    assert(0);
            }
            incoming_data[index*NUM_AMPLITUDES + i] = info->buf[i];
        }
        return;
    }
    pthread_mutex_lock(&array_free);
    printf("%dth packet collector acquired lock\n", collected_packets);
    for(int i=0; i<NUM_DATA_POINTS; i++){
        assert(sizeof(char)*i < sizeof(data));
        assert(sizeof(char)*i < sizeof(incoming_data));
        data[i] = incoming_data[i];
    }
    printf("%dth packet collector left lock\n", collected_packets);
    pthread_mutex_unlock(&array_free);
    pthread_t tcp_thread;
    struct array* data_struct = (struct array*) malloc(sizeof(struct array));
    data_struct->ptr = data;
    data_struct->size = NUM_DATA_POINTS;
    data_struct->uid = collected_packets;
    pthread_create(&tcp_thread, NULL, tcp_sender, (void*) data_struct);
#endif

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

    sleep(5);
    wifi_csi_init();
    wifi_ping_router_start();
    // char* data = (char*) malloc(sizeof(char)*100);
    // sprintf(data, "Hello W00rld\n");
    // struct array* hi = (struct array*) malloc(sizeof(struct array));
    // hi->ptr = data;
    // hi->size = strlen(data)+1;
    // tcp_sender(hi);

}
