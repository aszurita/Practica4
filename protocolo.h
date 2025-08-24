/**
 * @file protocolo.h
 * @brief Protocolo de comunicación UDP confiable con ventana deslizante
 * @author Angelo Zurita
 * @date 2024
 * @version 1.0
 *
 * @description
 * Este archivo define el protocolo de comunicación UDP confiable implementando:
 * - Ventana deslizante para control de flujo
 * - Control de errores con checksum
 * - Reconocimiento de paquetes (ACK/NACK)
 * - Soporte para múltiples emisores concurrentes
 * - Manejo de timeouts y reintentos
 *
 * El protocolo garantiza la entrega confiable de archivos a través de redes UDP
 * no confiables, similar a TCP pero optimizado para transferencias de archivos.
 */

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

// ============================================================================
// CONSTANTES DEL PROTOCOLO
// ============================================================================

/** @brief Tamaño máximo de datos por paquete (bytes) */
#define MAX_DATA_SIZE 1024

/** @brief Tamaño máximo del nombre de archivo */
#define MAX_FILENAME_SIZE 256

/** @brief Timeout en segundos para reintentos */
#define TIMEOUT_SEC 2

/** @brief Número máximo de reintentos para paquetes críticos */
#define MAX_RETRIES 5

/** @brief Tamaño de la ventana deslizante (paquetes) */
#define WINDOW_SIZE 10

// ============================================================================
// TIPOS DE PAQUETES
// ============================================================================

/**
 * @brief Tipos de paquetes del protocolo
 *
 * Define los diferentes tipos de mensajes que pueden intercambiarse
 * entre emisor y receptor para coordinar la transferencia de archivos.
 */
typedef enum
{
    PKT_START = 1, /**< Inicio de transmisión - establece sesión */
    PKT_DATA = 2,  /**< Datos del archivo - contenido real */
    PKT_ACK = 3,   /**< Acknowledgment - confirmación de recepción */
    PKT_END = 4,   /**< Fin de transmisión - cierra sesión */
    PKT_NACK = 5   /**< Negative acknowledgment - solicita retransmisión */
} packet_type_t;

// ============================================================================
// ESTRUCTURAS DE DATOS
// ============================================================================

/**
 * @brief Estructura principal del paquete de comunicación
 *
 * Contiene toda la información necesaria para la transmisión confiable:
 * - Metadatos de control (secuencia, tipo, checksum)
 * - Datos del archivo o información de control
 * - Identificación del emisor para soporte multi-emisor
 */
typedef struct
{
    uint32_t seq_num;                 /**< Número de secuencia del paquete */
    uint32_t ack_num;                 /**< Número de acknowledgment */
    packet_type_t type;               /**< Tipo de paquete */
    uint16_t data_size;               /**< Tamaño de los datos en bytes */
    uint32_t total_packets;           /**< Total de paquetes (solo en START) */
    uint32_t checksum;                /**< Checksum para detección de errores */
    uint32_t sender_id;               /**< ID único del emisor */
    char filename[MAX_FILENAME_SIZE]; /**< Nombre del archivo (solo en START) */
    char data[MAX_DATA_SIZE];         /**< Datos del archivo o payload */
} packet_t;

/**
 * @brief Slot de la ventana deslizante
 *
 * Cada slot mantiene el estado de un paquete en la ventana:
 * - El paquete en sí
 * - Estado de envío
 * - Timestamp del último envío para timeout
 */
typedef struct
{
    packet_t packet;          /**< Paquete almacenado en este slot */
    int sent;                 /**< Flag indicando si fue enviado */
    struct timeval sent_time; /**< Timestamp del envío */
} window_slot_t;

/**
 * @brief Estado completo del emisor
 *
 * Mantiene toda la información necesaria para la transmisión:
 * - Configuración de red y archivo
 * - Control de la ventana deslizante
 * - Estado de la secuencia y progreso
 * - Mutex para sincronización entre hilos
 */
typedef struct
{
    int sockfd;                        /**< Descriptor del socket UDP */
    struct sockaddr_in dest_addr;      /**< Dirección del receptor */
    FILE *file;                        /**< Archivo a transmitir */
    char filename[MAX_FILENAME_SIZE];  /**< Nombre del archivo */
    uint32_t next_seq;                 /**< Siguiente número de secuencia a enviar */
    uint32_t base_seq;                 /**< Base de la ventana deslizante */
    uint32_t total_packets;            /**< Total de paquetes del archivo */
    uint32_t sender_id;                /**< ID único del emisor */
    window_slot_t window[WINDOW_SIZE]; /**< Ventana deslizante */
    pthread_mutex_t mutex;             /**< Mutex para sincronización */
} sender_state_t;

/**
 * @brief Estado del receptor para una sesión
 *
 * Mantiene el estado de recepción para un emisor específico:
 * - Archivo de destino y progreso
 * - Control de secuencia esperada
 * - Registro de paquetes recibidos
 * - Mutex para sincronización
 */
typedef struct
{
    int sockfd;                       /**< Descriptor del socket UDP */
    struct sockaddr_in client_addr;   /**< Dirección del emisor */
    socklen_t client_len;             /**< Longitud de la dirección */
    FILE *file;                       /**< Archivo de destino */
    uint32_t expected_seq;            /**< Siguiente número de secuencia esperado */
    uint32_t total_packets;           /**< Total de paquetes esperados */
    uint32_t sender_id;               /**< ID del emisor actual */
    char filename[MAX_FILENAME_SIZE]; /**< Nombre del archivo */
    int *received_packets;            /**< Array de paquetes recibidos */
    pthread_mutex_t mutex;            /**< Mutex para sincronización */
} receiver_state_t;

// ============================================================================
// FUNCIONES DEL PROTOCOLO BASE
// ============================================================================

/**
 * @brief Calcula el checksum de un paquete
 * @param pkt Puntero al paquete
 * @return Checksum calculado (uint32_t)
 *
 * Implementa un algoritmo de checksum simple sumando todos los campos
 * del paquete para detectar corrupción en la transmisión.
 */
uint32_t calculate_checksum(const packet_t *pkt);

/**
 * @brief Verifica la integridad de un paquete usando checksum
 * @param pkt Puntero al paquete a verificar
 * @return 0 si el checksum es válido, -1 si no
 *
 * Compara el checksum recibido con el calculado localmente
 * para detectar errores de transmisión.
 */
int verify_checksum(const packet_t *pkt);

/**
 * @brief Crea un nuevo paquete con los parámetros especificados
 * @param pkt Puntero al paquete a crear
 * @param type Tipo de paquete
 * @param seq Número de secuencia
 * @param data Datos a incluir (puede ser NULL)
 * @param data_size Tamaño de los datos
 *
 * Inicializa todos los campos del paquete y calcula el checksum.
 * Si no hay datos, el paquete se crea solo con metadatos de control.
 */
void create_packet(packet_t *pkt, packet_type_t type, uint32_t seq,
                   const char *data, size_t data_size);

/**
 * @brief Envía un paquete a través del socket UDP
 * @param sockfd Descriptor del socket
 * @param pkt Puntero al paquete a enviar
 * @param addr Dirección de destino
 * @return 0 en éxito, -1 en error
 *
 * Utiliza sendto() para enviar el paquete completo a la dirección especificada.
 */
int send_packet(int sockfd, const packet_t *pkt, struct sockaddr_in *addr);

/**
 * @brief Recibe un paquete del socket UDP
 * @param sockfd Descriptor del socket
 * @param pkt Puntero donde almacenar el paquete recibido
 * @param addr Dirección del emisor (se llena automáticamente)
 * @param addr_len Longitud de la dirección
 * @return 0 en éxito, -1 en error de red, -2 en checksum inválido
 *
 * Recibe un paquete y verifica su integridad usando checksum.
 * Maneja timeouts y errores de red apropiadamente.
 */
int receive_packet(int sockfd, packet_t *pkt, struct sockaddr_in *addr, socklen_t *addr_len);

/**
 * @brief Imprime información detallada de un paquete
 * @param pkt Puntero al paquete
 * @param direction Dirección del paquete ("ENVIANDO", "RECIBIDO", etc.)
 *
 * Función de debugging que muestra todos los campos relevantes
 * del paquete para facilitar el diagnóstico de problemas.
 */
void print_packet_info(const packet_t *pkt, const char *direction);

// ============================================================================
// FUNCIONES DEL EMISOR
// ============================================================================

/**
 * @brief Inicializa el estado del emisor
 * @param state Puntero al estado a inicializar
 * @param host Dirección IP del receptor
 * @param port Puerto del receptor
 * @param filename Nombre del archivo a transmitir
 * @return 0 en éxito, -1 en error
 *
 * Configura el socket UDP, abre el archivo, calcula el número total
 * de paquetes y genera un ID único para el emisor.
 */
int sender_init(sender_state_t *state, const char *host, int port, const char *filename);

/**
 * @brief Inicia la transmisión enviando paquete START
 * @param state Puntero al estado del emisor
 * @return 0 en éxito, -1 en error
 *
 * Envía el paquete START y espera confirmación del receptor.
 * Reintenta hasta MAX_RETRIES veces si no hay respuesta.
 */
int sender_start_transmission(sender_state_t *state);

/**
 * @brief Envía paquetes hasta llenar la ventana deslizante
 * @param state Puntero al estado del emisor
 * @return 0 en éxito, -1 en error
 *
 * Lee datos del archivo y envía paquetes DATA hasta llenar la ventana
 * o hasta enviar todos los paquetes disponibles.
 */
int sender_send_window(sender_state_t *state);

/**
 * @brief Procesa un ACK recibido del receptor
 * @param state Puntero al estado del emisor
 * @param ack Puntero al paquete ACK recibido
 * @return 0 en éxito, -1 en error
 *
 * Actualiza la base de la ventana deslizante y marca los paquetes
 * confirmados como enviados exitosamente.
 */
int sender_handle_ack(sender_state_t *state, const packet_t *ack);

/**
 * @brief Procesa un NACK recibido del receptor
 * @param state Puntero al estado del emisor
 * @param nack Puntero al paquete NACK recibido
 * @return 0 en éxito, -1 en error
 *
 * Retransmite el paquete solicitado por el receptor
 * cuando se detecta un error o pérdida.
 */
int sender_handle_nack(sender_state_t *state, const packet_t *nack);

/**
 * @brief Espera a que todos los paquetes sean confirmados
 * @param state Puntero al estado del emisor
 * @return 0 en éxito, -1 en timeout
 *
 * Bucle que espera ACKs hasta que todos los paquetes
 * hayan sido confirmados por el receptor.
 */
int sender_wait_for_completion(sender_state_t *state);

/**
 * @brief Limpia los recursos del emisor
 * @param state Puntero al estado del emisor
 *
 * Cierra archivos, sockets y destruye mutexes
 * para liberar recursos del sistema.
 */
void sender_cleanup(sender_state_t *state);

/**
 * @brief Manejador de timeout para paquetes no confirmados
 * @param arg Puntero al estado del emisor (cast a void*)
 * @return NULL
 *
 * Hilo que monitorea timeouts y retransmite paquetes
 * que no han sido confirmados en tiempo razonable.
 */
void *sender_timeout_handler(void *arg);

// ============================================================================
// FUNCIONES DEL RECEPTOR
// ============================================================================

/**
 * @brief Inicializa el estado del receptor
 * @param state Puntero al estado a inicializar
 * @param port Puerto en el que escuchar
 * @return 0 en éxito, -1 en error
 *
 * Crea el socket UDP, lo vincula al puerto especificado
 * e inicializa las variables de estado.
 */
int receiver_init(receiver_state_t *state, int port);

/**
 * @brief Procesa un paquete START del emisor
 * @param state Puntero al estado del receptor
 * @param pkt Puntero al paquete START recibido
 * @return 0 en éxito, -1 en error
 *
 * Establece una nueva sesión de transferencia, crea el archivo
 * de destino y envía confirmación al emisor.
 */
int receiver_handle_start(receiver_state_t *state, const packet_t *pkt);

/**
 * @brief Procesa un paquete DATA del emisor
 * @param state Puntero al estado del receptor
 * @param pkt Puntero al paquete DATA recibido
 * @return 0 en éxito, -1 en error
 *
 * Escribe los datos recibidos en el archivo en la posición correcta
 * y envía un ACK al emisor para confirmar la recepción.
 */
int receiver_handle_data(receiver_state_t *state, const packet_t *pkt);

/**
 * @brief Envía un ACK al emisor
 * @param state Puntero al estado del receptor
 * @param seq_num Número de secuencia confirmado
 * @return 0 en éxito, -1 en error
 *
 * Crea y envía un paquete ACK para confirmar la recepción
 * exitosa de un paquete específico.
 */
int receiver_send_ack(receiver_state_t *state, uint32_t seq_num);

/**
 * @brief Envía un NACK al emisor
 * @param state Puntero al estado del receptor
 * @param seq_num Número de secuencia con error
 * @return 0 en éxito, -1 en error
 *
 * Crea y envía un paquete NACK para solicitar la retransmisión
 * de un paquete que llegó corrupto o fuera de orden.
 */
int receiver_send_nack(receiver_state_t *state, uint32_t seq_num);

/**
 * @brief Limpia los recursos del receptor
 * @param state Puntero al estado del receptor
 *
 * Cierra archivos, sockets y libera memoria
 * para limpiar el estado de la sesión.
 */
void receiver_cleanup(receiver_state_t *state);

// ============================================================================
// ESTRUCTURAS PARA MÚLTIPLES EMISORES
// ============================================================================

/**
 * @brief Sesión de transferencia para un emisor específico
 *
 * Mantiene el estado independiente para cada emisor concurrente:
 * - Archivo y progreso de recepción
 * - Control de secuencia específico
 * - Timeout de actividad
 * - Mutex para sincronización
 */
typedef struct transfer_session
{
    uint32_t sender_id;               /**< ID único del emisor */
    char filename[MAX_FILENAME_SIZE]; /**< Nombre del archivo único */
    FILE *file;                       /**< Archivo de destino */
    uint32_t total_packets;           /**< Total de paquetes esperados */
    uint32_t expected_seq;            /**< Siguiente secuencia esperada */
    int *received_packets;            /**< Array de paquetes recibidos */
    struct sockaddr_in client_addr;   /**< Dirección del emisor */
    socklen_t client_len;             /**< Longitud de la dirección */
    time_t last_activity;             /**< Timestamp de última actividad */
    pthread_mutex_t session_mutex;    /**< Mutex de la sesión */
    struct transfer_session *next;    /**< Puntero a siguiente sesión */
} transfer_session_t;

/**
 * @brief Estado del receptor multi-sesión
 *
 * Coordina múltiples sesiones concurrentes:
 * - Lista enlazada de sesiones activas
 * - Pool de hilos trabajadores
 * - Cola de paquetes para procesamiento
 * - Control de ejecución global
 */
typedef struct
{
    int sockfd;                     /**< Socket principal del receptor */
    transfer_session_t *sessions;   /**< Lista de sesiones activas */
    pthread_mutex_t sessions_mutex; /**< Mutex para la lista de sesiones */
    pthread_t *worker_threads;      /**< Array de hilos trabajadores */
    int num_workers;                /**< Número de hilos trabajadores */
    volatile int running;           /**< Flag de ejecución */
} multi_receiver_state_t;

/**
 * @brief Nodo de la cola de paquetes
 *
 * Cada nodo contiene un paquete recibido y su dirección
 * de origen para procesamiento posterior por hilos trabajadores.
 */
typedef struct packet_queue_node
{
    packet_t packet;                /**< Paquete recibido */
    struct sockaddr_in from_addr;   /**< Dirección del emisor */
    struct packet_queue_node *next; /**< Puntero al siguiente nodo */
} packet_queue_node_t;

/**
 * @brief Cola thread-safe de paquetes
 *
 * Implementa una cola FIFO thread-safe para distribuir
 * paquetes entre múltiples hilos trabajadores.
 */
typedef struct
{
    packet_queue_node_t *head; /**< Cabeza de la cola */
    packet_queue_node_t *tail; /**< Cola de la lista */
    pthread_mutex_t mutex;     /**< Mutex para sincronización */
    pthread_cond_t cond;       /**< Variable de condición */
    int size;                  /**< Tamaño actual de la cola */
} packet_queue_t;

#endif // PROTOCOLO_H