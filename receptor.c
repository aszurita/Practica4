/**
 * @file receptor.c
 * @brief Implementación del receptor multi-emisor del protocolo UDP confiable
 * @author Angelo Zurita
 * @date 2024
 * @version 1.0
 *
 * @description
 * Este archivo implementa el receptor del protocolo de comunicación UDP confiable
 * con soporte para múltiples emisores concurrentes. El receptor es responsable de:
 *
 * - Escuchar conexiones en un puerto UDP específico
 * - Manejar múltiples sesiones de transferencia simultáneamente
 * - Recibir archivos de diferentes emisores usando IDs únicos
 * - Implementar cola de paquetes thread-safe para procesamiento
 * - Coordinar múltiples hilos trabajadores para eficiencia
 * - Crear nombres de archivo únicos para evitar conflictos
 * - Limpiar sesiones completadas automáticamente
 *
 * El receptor utiliza una arquitectura multi-hilo con pool de trabajadores
 * y cola de paquetes para maximizar el throughput y manejar
 * múltiples transferencias concurrentes eficientemente.
 */

#include "protocolo.h"
#include <signal.h>
#include <semaphore.h>

// ============================================================================
// VARIABLES GLOBALES Y ESTADO
// ============================================================================

/** @brief Estado global del receptor multi-sesión */
static multi_receiver_state_t global_state;

/** @brief Cola thread-safe de paquetes para procesamiento */
static packet_queue_t packet_queue;

/** @brief Semáforo para controlar el acceso a la cola */
static sem_t queue_sem;

// ============================================================================
// FUNCIONES DE LA COLA DE PAQUETES
// ============================================================================

/**
 * @brief Inicializa la cola de paquetes thread-safe
 *
 * @param queue Puntero a la cola a inicializar
 *
 * @details
 * La función inicializa todos los componentes de la cola:
 * 1. Establece punteros head y tail a NULL
 * 2. Inicializa el mutex para sincronización
 * 3. Inicializa la variable de condición para señalización
 * 4. Inicializa el semáforo para control de acceso
 *
 * @note
 * Esta función debe ser llamada antes de usar la cola.
 */
void init_packet_queue(packet_queue_t *queue)
{
    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
    sem_init(&queue_sem, 0, 0); // Semáforo para contar paquetes
}

/**
 * @brief Agrega un paquete a la cola de manera thread-safe
 *
 * @param queue Puntero a la cola
 * @param pkt Puntero al paquete a agregar
 * @param addr Dirección del emisor del paquete
 *
 * @details
 * La función:
 * 1. Crea un nuevo nodo para el paquete
 * 2. Copia el paquete y la dirección del emisor
 * 3. Agrega el nodo al final de la cola
 * 4. Incrementa el contador de paquetes
 * 5. Señaliza a los hilos trabajadores que hay trabajo disponible
 * 6. Incrementa el semáforo para control de acceso
 *
 * @note
 * Esta función es thread-safe y puede ser llamada desde múltiples hilos.
 */
void enqueue_packet(packet_queue_t *queue, const packet_t *pkt, const struct sockaddr_in *addr)
{
    // Crear nuevo nodo para el paquete
    packet_queue_node_t *node = malloc(sizeof(packet_queue_node_t));
    node->packet = *pkt;
    node->from_addr = *addr;
    node->next = NULL;

    // Agregar nodo al final de la cola de manera thread-safe
    pthread_mutex_lock(&queue->mutex);

    if (queue->tail)
    {
        queue->tail->next = node;
    }
    else
    {
        queue->head = node;
    }
    queue->tail = node;
    queue->size++;

    pthread_mutex_unlock(&queue->mutex);

    // Despertar hilos trabajadores y actualizar semáforo
    pthread_cond_signal(&queue->cond);
    sem_post(&queue_sem);
}

/**
 * @brief Extrae un paquete de la cola de manera thread-safe
 *
 * @param queue Puntero a la cola
 * @param pkt Puntero donde almacenar el paquete extraído
 * @param addr Puntero donde almacenar la dirección del emisor
 * @return int 0 en éxito, -1 si la cola está vacía
 *
 * @details
 * La función:
 * 1. Espera hasta que haya paquetes disponibles (semáforo)
 * 2. Extrae el primer paquete de la cola (FIFO)
 * 3. Actualiza los punteros de la cola
 * 4. Libera la memoria del nodo extraído
 * 5. Decrementa el contador de paquetes
 *
 * @note
 * Esta función es bloqueante hasta que haya paquetes disponibles.
 */
int dequeue_packet(packet_queue_t *queue, packet_t *pkt, struct sockaddr_in *addr)
{
    // Esperar hasta que haya paquetes disponibles
    sem_wait(&queue_sem);

    pthread_mutex_lock(&queue->mutex);

    // Verificar que la cola no esté vacía
    if (!queue->head)
    {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    // Extraer el primer nodo de la cola
    packet_queue_node_t *node = queue->head;
    *pkt = node->packet;
    *addr = node->from_addr;

    // Actualizar punteros de la cola
    queue->head = node->next;
    if (!queue->head)
    {
        queue->tail = NULL;
    }
    queue->size--;

    pthread_mutex_unlock(&queue->mutex);

    // Liberar memoria del nodo extraído
    free(node);

    return 0;
}

// ============================================================================
// FUNCIONES DE GESTIÓN DE SESIONES
// ============================================================================

/**
 * @brief Busca una sesión existente por ID del emisor
 *
 * @param state Puntero al estado del receptor
 * @param sender_id ID único del emisor a buscar
 * @return transfer_session_t* Puntero a la sesión encontrada o NULL
 *
 * @details
 * La función busca en la lista enlazada de sesiones activas:
 * 1. Recorre la lista desde el inicio
 * 2. Compara el sender_id de cada sesión
 * 3. Retorna la primera sesión que coincida
 * 4. Retorna NULL si no se encuentra
 *
 * @note
 * Esta función es thread-safe usando mutex para proteger la lista.
 */
transfer_session_t *find_session(multi_receiver_state_t *state, uint32_t sender_id)
{
    pthread_mutex_lock(&state->sessions_mutex);

    transfer_session_t *current = state->sessions;
    while (current)
    {
        if (current->sender_id == sender_id)
        {
            pthread_mutex_unlock(&state->sessions_mutex);
            return current;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&state->sessions_mutex);
    return NULL;
}

/**
 * @brief Crea una nueva sesión de transferencia para un emisor
 *
 * @param state Puntero al estado del receptor
 * @param sender_id ID único del emisor
 * @param client_addr Dirección del emisor
 * @return transfer_session_t* Puntero a la nueva sesión creada
 *
 * @details
 * La función:
 * 1. Alloca memoria para la nueva sesión
 * 2. Inicializa todos los campos de la sesión
 * 3. Establece el timestamp de última actividad
 * 4. Inicializa el mutex de la sesión
 * 5. Agrega la sesión al inicio de la lista enlazada
 *
 * @note
 * La nueva sesión se agrega al inicio de la lista para acceso rápido.
 */
transfer_session_t *create_session(multi_receiver_state_t *state, uint32_t sender_id,
                                   const struct sockaddr_in *client_addr)
{
    // Crear nueva sesión
    transfer_session_t *session = malloc(sizeof(transfer_session_t));
    memset(session, 0, sizeof(transfer_session_t));

    // Inicializar campos de la sesión
    session->sender_id = sender_id;
    session->client_addr = *client_addr;
    session->client_len = sizeof(struct sockaddr_in);
    session->last_activity = time(NULL);
    session->expected_seq = 1;
    pthread_mutex_init(&session->session_mutex, NULL);

    // Agregar al inicio de la lista de manera thread-safe
    pthread_mutex_lock(&state->sessions_mutex);

    session->next = state->sessions;
    state->sessions = session;

    pthread_mutex_unlock(&state->sessions_mutex);

    printf("Nueva sesión creada para Sender ID: %u\n", sender_id);
    return session;
}

/**
 * @brief Limpia y elimina una sesión completada
 *
 * @param state Puntero al estado del receptor
 * @param sender_id ID del emisor cuya sesión se eliminará
 *
 * @details
 * La función:
 * 1. Busca la sesión en la lista enlazada
 * 2. Actualiza los punteros para mantener la integridad de la lista
 * 3. Cierra el archivo asociado a la sesión
 * 4. Libera la memoria del array de paquetes recibidos
 * 5. Destruye el mutex de la sesión
 * 6. Libera la memoria de la sesión
 *
 * @note
 * Esta función es thread-safe y debe ser llamada cuando una transferencia
 * se completa o cuando se detecta timeout de inactividad.
 */
void cleanup_session(multi_receiver_state_t *state, uint32_t sender_id)
{
    pthread_mutex_lock(&state->sessions_mutex);

    transfer_session_t *prev = NULL;
    transfer_session_t *current = state->sessions;

    // Buscar la sesión en la lista
    while (current)
    {
        if (current->sender_id == sender_id)
        {
            // Actualizar punteros de la lista enlazada
            if (prev)
            {
                prev->next = current->next;
            }
            else
            {
                state->sessions = current->next;
            }

            // Limpiar recursos de la sesión
            if (current->file)
            {
                fclose(current->file);
            }
            if (current->received_packets)
            {
                free(current->received_packets);
            }
            pthread_mutex_destroy(&current->session_mutex);

            printf("Sesión limpiada para Sender ID: %u\n", sender_id);
            free(current);
            break;
        }
        prev = current;
        current = current->next;
    }

    pthread_mutex_unlock(&state->sessions_mutex);
}

// ============================================================================
// FUNCIONES DE UTILIDAD
// ============================================================================

/**
 * @brief Crea un nombre de archivo único para evitar conflictos
 *
 * @param original_filename Nombre original del archivo
 * @param sender_id ID único del emisor (reservado para uso futuro)
 * @param new_filename Buffer donde almacenar el nuevo nombre
 * @param max_size Tamaño máximo del buffer
 *
 * @details
 * La función crea un nombre único agregando sufijos:
 * 1. Detecta si el archivo original tiene extensión
 * 2. Crea nombre único: base_recibido_.extension
 * 3. Si no hay extensión: nombre_recibido_
 *
 * @note
 * El parámetro sender_id está reservado para implementaciones futuras
 * que podrían incluir el ID en el nombre del archivo.
 */
void create_unique_filename(const char *original_filename, uint32_t sender_id,
                            char *new_filename, size_t max_size)
{
    (void)sender_id; // Suprimir warning - parámetro reservado para uso futuro

    // Buscar extensión del archivo
    const char *dot = strrchr(original_filename, '.');
    const char *slash = strrchr(original_filename, '/');

    if (dot && (!slash || dot > slash))
    {
        // El archivo tiene extensión
        size_t base_len = dot - original_filename;
        char base_name[256];

        if (base_len >= sizeof(base_name))
            base_len = sizeof(base_name) - 1;

        strncpy(base_name, original_filename, base_len);
        base_name[base_len] = '\0';

        // Crear nombre único: base_recibido_.extension
        snprintf(new_filename, max_size, "%s_recibido_%s", base_name, dot);
    }
    else
    {
        // El archivo no tiene extensión
        snprintf(new_filename, max_size, "%s_recibido_", original_filename);
    }
}

// ============================================================================
// MANEJADORES DE PAQUETES
// ============================================================================

/**
 * @brief Maneja un paquete START para establecer nueva sesión
 *
 * @param state Puntero al estado del receptor
 * @param pkt Puntero al paquete START recibido
 * @param from_addr Dirección del emisor
 * @return int 0 en éxito, -1 en error
 *
 * @details
 * Cuando se recibe un paquete START:
 * 1. Verifica si ya existe una sesión para el emisor
 * 2. Limpia sesión existente si es necesario
 * 3. Crea nueva sesión con ID único del emisor
 * 4. Genera nombre de archivo único para evitar conflictos
 * 5. Abre archivo de destino para escritura
 * 6. Envía ACK de confirmación al emisor
 *
 * @note
 * Esta función es crítica para establecer nuevas sesiones de transferencia.
 */
int handle_multi_start(multi_receiver_state_t *state, const packet_t *pkt,
                       const struct sockaddr_in *from_addr)
{
    // Buscar sesión existente para el emisor
    transfer_session_t *session = find_session(state, pkt->sender_id);

    if (session)
    {
        printf("Reiniciando transferencia existente para Sender ID: %u\n", pkt->sender_id);
        cleanup_session(state, pkt->sender_id);
    }

    // Crear nueva sesión
    session = create_session(state, pkt->sender_id, from_addr);
    if (!session)
    {
        printf("Error creando sesión para Sender ID: %u\n", pkt->sender_id);
        return -1;
    }

    pthread_mutex_lock(&session->session_mutex);

    // Crear nombre de archivo único
    char unique_filename[512];
    create_unique_filename(pkt->filename, pkt->sender_id, unique_filename, sizeof(unique_filename));
    strncpy(session->filename, unique_filename, MAX_FILENAME_SIZE - 1);

    // Configurar sesión
    session->total_packets = (pkt->total_packets > 0) ? pkt->total_packets : 1;
    session->received_packets = calloc(session->total_packets + 1, sizeof(int));

    if (!session->received_packets)
    {
        pthread_mutex_unlock(&session->session_mutex);
        cleanup_session(state, pkt->sender_id);
        return -1;
    }

    // Abrir archivo único para escritura
    session->file = fopen(session->filename, "wb");
    if (!session->file)
    {
        printf("Error creando archivo: %s\n", session->filename);
        pthread_mutex_unlock(&session->session_mutex);
        cleanup_session(state, pkt->sender_id);
        return -1;
    }

    session->last_activity = time(NULL);

    printf("Transferencia iniciada:\n");
    printf("   Archivo: %s\n", session->filename);
    printf("   Paquetes: %u\n", session->total_packets);
    printf("   Sender ID: %u\n", session->sender_id);

    pthread_mutex_unlock(&session->session_mutex);

    // Enviar ACK de confirmación
    packet_t ack;
    create_packet(&ack, PKT_ACK, 1, NULL, 0);
    ack.ack_num = 1;
    ack.sender_id = pkt->sender_id;
    ack.checksum = calculate_checksum(&ack);

    sendto(state->sockfd, &ack, sizeof(packet_t), 0,
           (struct sockaddr *)from_addr, sizeof(struct sockaddr_in));
    print_packet_info(&ack, "ENVIANDO ACK");

    return 0;
}

// ============================================================================
// HILOS TRABAJADORES
// ============================================================================

/**
 * @brief Hilo trabajador para procesar paquetes de la cola
 *
 * @param arg Puntero al estado del receptor (cast a void*)
 * @return void* Siempre NULL
 *
 * @details
 * Este hilo ejecuta el bucle principal de procesamiento:
 * 1. Extrae paquetes de la cola thread-safe
 * 2. Procesa cada tipo de paquete según su función
 * 3. Maneja sesiones de transferencia independientes
 * 4. Envía ACKs/NACKs apropiados
 * 5. Continúa hasta que se detenga el receptor
 *
 * El hilo implementa el protocolo completo para cada paquete recibido,
 * manteniendo sesiones separadas para cada emisor.
 *
 * @note
 * Múltiples hilos trabajadores pueden ejecutar esta función
 * simultáneamente para mejorar el throughput.
 */
void *packet_worker(void *arg)
{
    multi_receiver_state_t *state = (multi_receiver_state_t *)arg;

    while (state->running)
    {
        packet_t pkt;
        struct sockaddr_in from_addr;

        // Extraer paquete de la cola
        if (dequeue_packet(&packet_queue, &pkt, &from_addr) == 0)
        {
            printf("Procesando paquete tipo=%d, sender_id=%u, seq=%u\n",
                   pkt.type, pkt.sender_id, pkt.seq_num);

            // Procesar según el tipo de paquete
            switch (pkt.type)
            {
            case PKT_START:
                handle_multi_start(state, &pkt, &from_addr);
                break;

            case PKT_DATA:
            {
                // Buscar sesión para el emisor
                transfer_session_t *session = find_session(state, pkt.sender_id);
                if (session)
                {
                    pthread_mutex_lock(&session->session_mutex);

                    // Verificar si el paquete ya fue recibido (duplicado)
                    if (!session->received_packets[pkt.seq_num])
                    {
                        // Marcar como recibido
                        session->received_packets[pkt.seq_num] = 1;

                        // Escribir datos en la posición correcta del archivo
                        long file_pos = (pkt.seq_num - 1) * MAX_DATA_SIZE;
                        fseek(session->file, file_pos, SEEK_SET);
                        fwrite(pkt.data, 1, pkt.data_size, session->file);
                        fflush(session->file);

                        printf("Paquete %u/%u recibido para %s\n",
                               pkt.seq_num, session->total_packets, session->filename);
                    }

                    // Actualizar timestamp de última actividad
                    session->last_activity = time(NULL);

                    // Enviar ACK de confirmación
                    packet_t ack;
                    create_packet(&ack, PKT_ACK, pkt.seq_num, NULL, 0);
                    ack.ack_num = pkt.seq_num;
                    ack.sender_id = pkt.sender_id;
                    ack.checksum = calculate_checksum(&ack);

                    sendto(state->sockfd, &ack, sizeof(packet_t), 0,
                           (struct sockaddr *)&session->client_addr,
                           session->client_len);

                    pthread_mutex_unlock(&session->session_mutex);
                }
                break;
            }

            case PKT_END:
            {
                // Transferencia completada, limpiar sesión
                transfer_session_t *session = find_session(state, pkt.sender_id);
                if (session)
                {
                    printf("Transferencia completada para %s\n", session->filename);
                    cleanup_session(state, pkt.sender_id);
                }
                break;
            }

            case PKT_ACK:
                printf("ACK inesperado recibido (tipo no manejado por receptor)\n");
                break;

            case PKT_NACK:
                printf("NACK inesperado recibido (tipo no manejado por receptor)\n");
                break;
            }
        }
    }

    return NULL;
}

// ============================================================================
// FUNCIÓN PRINCIPAL
// ============================================================================

/**
 * @brief Función principal del receptor multi-emisor
 *
 * @param argc Número de argumentos de línea de comandos
 * @param argv Array de argumentos de línea de comandos
 * @return int 0 en éxito, 1 en error
 *
 * @details
 * La función principal realiza:
 * 1. Validación de argumentos de línea de comandos
 * 2. Inicialización del estado global del receptor
 * 3. Creación y configuración del socket UDP
 * 4. Inicialización de la cola de paquetes thread-safe
 * 5. Creación del pool de hilos trabajadores
 * 6. Ejecución del bucle principal de recepción
 * 7. Distribución de paquetes a la cola para procesamiento
 * 8. Limpieza de recursos al finalizar
 *
 * @note
 * El programa debe recibir exactamente 1 argumento:
 * - puerto: Puerto en el que escuchar conexiones
 */
int main(int argc, char *argv[])
{
    // Validar argumentos de línea de comandos
    if (argc != 2)
    {
        fprintf(stderr, "Uso: %s <puerto>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    // Mostrar información de inicio
    printf("RECEPTOR MULTI-EMISOR INICIADO\n");
    printf("Puerto: %d\n", port);
    printf("Soporte para emisores concurrentes: ACTIVADO\n");
    printf("====================================\n\n");

    // Inicializar estado global
    memset(&global_state, 0, sizeof(global_state));
    global_state.running = 1;
    global_state.num_workers = 4; // 4 hilos trabajadores
    pthread_mutex_init(&global_state.sessions_mutex, NULL);

    // Crear socket UDP
    global_state.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (global_state.sockfd < 0)
    {
        perror("Error creando socket");
        return 1;
    }

    // Configurar dirección de escucha
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // Escuchar en todas las interfaces
    addr.sin_port = htons(port);

    // Vincular socket al puerto
    if (bind(global_state.sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Error en bind");
        return 1;
    }

    // Inicializar cola de paquetes
    init_packet_queue(&packet_queue);

    // Crear pool de hilos trabajadores
    global_state.worker_threads = malloc(global_state.num_workers * sizeof(pthread_t));
    for (int i = 0; i < global_state.num_workers; i++)
    {
        pthread_create(&global_state.worker_threads[i], NULL, packet_worker, &global_state);
    }

    printf("Esperando conexiones...\n\n");

    // Bucle principal - solo recibe y encola paquetes
    while (global_state.running)
    {
        packet_t pkt;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        // Recibir paquete del socket
        ssize_t received = recvfrom(global_state.sockfd, &pkt, sizeof(packet_t), 0,
                                    (struct sockaddr *)&from_addr, &from_len);

        // Verificar integridad y encolar para procesamiento
        if (received > 0 && verify_checksum(&pkt))
        {
            enqueue_packet(&packet_queue, &pkt, &from_addr);
        }

        usleep(1000); // Pausa mínima para evitar saturar la CPU
    }

    return 0;
}