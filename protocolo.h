#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>

// Constantes del protocolo
#define MAX_DATA_SIZE 1024
#define MAX_FILENAME_SIZE 256
#define TIMEOUT_SEC 2
#define MAX_RETRIES 5
#define WINDOW_SIZE 10

// Tipos de paquetes
typedef enum
{
    PKT_START = 1, // Inicio de transmisión
    PKT_DATA = 2,  // Datos
    PKT_ACK = 3,   // Acknowledgment
    PKT_END = 4,   // Fin de transmisión
    PKT_NACK = 5   // Negative acknowledgment
} packet_type_t;

// Estructura del paquete
typedef struct
{
    uint32_t seq_num;                 // Número de secuencia
    uint32_t ack_num;                 // Número de acknowledgment
    packet_type_t type;               // Tipo de paquete
    uint16_t data_size;               // Tamaño de los datos
    uint32_t total_packets;           // Total de paquetes (solo en START)
    uint32_t checksum;                // Checksum simple
    uint32_t sender_id;               // ID del emisor para multi-emisor
    char filename[MAX_FILENAME_SIZE]; // Nombre del archivo (solo en START)
    char data[MAX_DATA_SIZE];         // Datos
} packet_t;

// Estructura para el control de ventana deslizante
typedef struct
{
    packet_t packet;
    int sent;
    struct timeval sent_time;
} window_slot_t;

// Estructura para el estado del emisor
typedef struct
{
    int sockfd;
    struct sockaddr_in dest_addr;
    FILE *file;
    char filename[MAX_FILENAME_SIZE]; // Nombre del archivo a enviar
    uint32_t next_seq;
    uint32_t base_seq;
    uint32_t total_packets;
    uint32_t sender_id;               // ID único del emisor
    window_slot_t window[WINDOW_SIZE];
    pthread_mutex_t mutex;
} sender_state_t;

// Estructura para el estado del receptor
typedef struct
{
    int sockfd;
    struct sockaddr_in client_addr;
    socklen_t client_len;
    FILE *file;
    uint32_t expected_seq;
    uint32_t total_packets;
    uint32_t sender_id;               // ID del emisor actual
    char filename[MAX_FILENAME_SIZE];
    int *received_packets;
    pthread_mutex_t mutex;
} receiver_state_t;

// Funciones del protocolo
uint32_t calculate_checksum(const packet_t *pkt);
int verify_checksum(const packet_t *pkt);
void create_packet(packet_t *pkt, packet_type_t type, uint32_t seq,
                   const char *data, size_t data_size);
int send_packet(int sockfd, const packet_t *pkt, struct sockaddr_in *addr);
int receive_packet(int sockfd, packet_t *pkt, struct sockaddr_in *addr, socklen_t *addr_len);
void print_packet_info(const packet_t *pkt, const char *direction);

// Funciones del emisor
int sender_init(sender_state_t *state, const char *host, int port, const char *filename);
int sender_start_transmission(sender_state_t *state);
int sender_send_window(sender_state_t *state);
int sender_handle_ack(sender_state_t *state, const packet_t *ack);
int sender_handle_nack(sender_state_t *state, const packet_t *nack);
int sender_wait_for_completion(sender_state_t *state);
void sender_cleanup(sender_state_t *state);
void *sender_timeout_handler(void *arg);

// Funciones del receptor
int receiver_init(receiver_state_t *state, int port);
int receiver_handle_start(receiver_state_t *state, const packet_t *pkt);
int receiver_handle_data(receiver_state_t *state, const packet_t *pkt);
int receiver_send_ack(receiver_state_t *state, uint32_t seq_num);
int receiver_send_nack(receiver_state_t *state, uint32_t seq_num);
void receiver_cleanup(receiver_state_t *state);

#endif // PROTOCOLO_H