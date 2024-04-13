// General includes
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>

// Network related includes
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct client_info{
    int client_socket;
    struct sockaddr_in client_address;
    int uid;
};

int port = 8001;
int server_socket;
char* root;

#define NUM_OBS 1

char* data[NUM_OBS+5];
int data_len[NUM_OBS+5];

void* handle_connection(void* arg){

    struct client_info* client_info = (struct client_info*) arg;
    // need to decide how to set the http boundary between two messages in the 
    // stream of bytes (for persisstent connections)
    // receive the request
    int offset = 0;
    int size_of_array = 17000;
    data[client_info->uid] = (char*) malloc(sizeof(char)*17000);
    while(1){

        char buffer[16384];
        int bytesrecv = recv(client_info->client_socket, buffer, sizeof(buffer), 0);
        if(bytesrecv <= 0){
            printf("Closing connection\n");
            goto END;
        }

        if(bytesrecv+offset >= size_of_array){
            data[client_info->uid] = (char*) realloc(data[client_info->uid], 2*size_of_array);
            assert(data[client_info->uid] != NULL);
            size_of_array *= 2;
        }
        int i;
        for(i=0; i<bytesrecv; i++)
            data[client_info->uid][offset + i] = buffer[i];
        printf("Got packet %d\n", client_info->uid);
        offset += bytesrecv;
    }
    END:
    data_len[client_info->uid] = offset;
    close(client_info->client_socket);
    free(arg);
    return NULL;
}

int main(int argc, char* argv[]){

    int _ret; // to store return values of calls

    if(argc!=3){
        printf("Usage: %s <port> <root>\n", argv[0]);
        exit(1);
    }

    port = atoi(argv[1]);
    root = argv[2];

    // create the server socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if(server_socket < -1){
        perror("socket");
        exit(1);
    }

    // define the server address
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = INADDR_ANY;

    // bind the socket to our specified IP and port
    _ret = bind(server_socket, (struct sockaddr*) &server_address, sizeof(server_address));
    if(_ret < 0){
        perror("bind");
        exit(1);
    }

    // listen for connections
    _ret = listen(server_socket, 5);
    if(_ret < 0){
        perror("listen");
        exit(1);
    }

    int uid = 0;
    while(1){
        // accept a connection
        struct sockaddr_in client_address;
        int clientsock;
        socklen_t clientaddrsz = sizeof(client_address);
        clientsock = accept(server_socket, (struct sockaddr*) &client_address, &clientaddrsz);
        if(clientsock < 0){
            perror("accept");
            exit(1);
        }
        char clientip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_address.sin_addr, clientip, INET_ADDRSTRLEN);
        int clientport = ntohs(client_address.sin_port);
        printf("Connection from %s:%d\n", clientip, clientport);

        // create a new thread to handle the connection
        pthread_t thread_id;
        struct client_info* client_info = (struct client_info*) malloc(sizeof(struct client_info));
        client_info->client_socket = clientsock;
        client_info->client_address = client_address;   
        client_info->uid = uid++; 

        // set a 10s timeout for the client socket
        struct timeval timeout;      
        timeout.tv_sec = 100;
        timeout.tv_usec = 0;
        _ret = setsockopt (client_info->client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        if (_ret < 0)
            perror("setsockopt failed\n");
        _ret = pthread_create(&thread_id, NULL, handle_connection, (void*) client_info);
        if(_ret){
            perror("pthread_create");
            exit(1);
        }

        if(uid==NUM_OBS){
            pthread_join(thread_id, NULL);
            break;
        }

    }
     
    // save the data into a file
    FILE* datafile = fopen("data_slow_none.csv", "w");
    int i;
    for(i=0; i<NUM_OBS-1; i++){
        fprintf(datafile, "%d, ", i);
        int j;
        for(j=0; j<data_len[i]-1; j++){
            fprintf(datafile, "%d, ", data[i][j]);
        }
        fprintf(datafile, "%d\n", data[i][data_len[i]-1]);
    }
    fclose(datafile);
}
