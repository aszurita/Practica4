// receptor_multisender.c
#include "protocolo.h"
#include <signal.h>
#include <semaphore.h>

static multi_receiver_state_t global_state;
static packet_queue_t packet_queue;
static sem_t queue_sem;

// Inicializar cola de paquetes
void init_packet_queue(packet_queue_t *queue)
{
    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
    sem_init(&queue_sem, 0, 0); // Semáforo para contar paquetes
}

// Agregar paquete a la cola (thread-safe)
void enqueue_packet(packet_queue_t *queue, const packet_t *pkt, const struct sockaddr_in *addr)
{
    packet_queue_node_t *node = malloc(sizeof(packet_queue_node_t));
    node->packet = *pkt;
    node->from_addr = *addr;
    node->next = NULL;

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
    pthread_cond_signal(&queue->cond); // Despertar hilo trabajador
    sem_post(&queue_sem);              // Incrementar semáforo
}

// Obtener paquete de la cola (thread-safe)
int dequeue_packet(packet_queue_t *queue, packet_t *pkt, struct sockaddr_in *addr)
{
    sem_wait(&queue_sem); // Esperar hasta que haya paquetes

    pthread_mutex_lock(&queue->mutex);

    if (!queue->head)
    {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    packet_queue_node_t *node = queue->head;
    *pkt = node->packet;
    *addr = node->from_addr;

    queue->head = node->next;
    if (!queue->head)
    {
        queue->tail = NULL;
    }
    queue->size--;

    pthread_mutex_unlock(&queue->mutex);
    free(node);

    return 0;
}

// Buscar sesión por sender_id (thread-safe)
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

// Crear nueva sesión (thread-safe)
transfer_session_t *create_session(multi_receiver_state_t *state, uint32_t sender_id,
                                   const struct sockaddr_in *client_addr)
{
    transfer_session_t *session = malloc(sizeof(transfer_session_t));
    memset(session, 0, sizeof(transfer_session_t));

    session->sender_id = sender_id;
    session->client_addr = *client_addr;
    session->client_len = sizeof(struct sockaddr_in);
    session->last_activity = time(NULL);
    session->expected_seq = 1;
    pthread_mutex_init(&session->session_mutex, NULL);

    pthread_mutex_lock(&state->sessions_mutex);

    // Agregar al inicio de la lista
    session->next = state->sessions;
    state->sessions = session;

    pthread_mutex_unlock(&state->sessions_mutex);

    printf("🆕 Nueva sesión creada para Sender ID: %u\n", sender_id);
    return session;
}

// Limpiar sesión completada
void cleanup_session(multi_receiver_state_t *state, uint32_t sender_id)
{
    pthread_mutex_lock(&state->sessions_mutex);

    transfer_session_t *prev = NULL;
    transfer_session_t *current = state->sessions;

    while (current)
    {
        if (current->sender_id == sender_id)
        {
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

            printf("🗑️ Sesión limpiada para Sender ID: %u\n", sender_id);
            free(current);
            break;
        }
        prev = current;
        current = current->next;
    }

    pthread_mutex_unlock(&state->sessions_mutex);
}

// Función mejorada para crear nombre de archivo único por sender
void create_unique_filename(const char *original_filename, uint32_t sender_id,
                            char *new_filename, size_t max_size)
{
    const char *dot = strrchr(original_filename, '.');
    const char *slash = strrchr(original_filename, '/');

    if (dot && (!slash || dot > slash))
    {
        // Hay extensión
        size_t base_len = dot - original_filename;
        char base_name[256];

        if (base_len >= sizeof(base_name))
            base_len = sizeof(base_name) - 1;

        strncpy(base_name, original_filename, base_len);
        base_name[base_len] = '\0';

        // Crear nombre único: base_recibido_SENDERID.extension
        snprintf(new_filename, max_size, "%s_recibido_%u%s", base_name, sender_id, dot);
    }
    else
    {
        // No hay extensión
        snprintf(new_filename, max_size, "%s_recibido_%u", original_filename, sender_id);
    }
}

// Manejar paquete START para multi-emisor
int handle_multi_start(multi_receiver_state_t *state, const packet_t *pkt,
                       const struct sockaddr_in *from_addr)
{
    transfer_session_t *session = find_session(state, pkt->sender_id);

    if (session)
    {
        printf("⚠️ Reiniciando transferencia existente para Sender ID: %u\n", pkt->sender_id);
        cleanup_session(state, pkt->sender_id);
    }

    // Crear nueva sesión
    session = create_session(state, pkt->sender_id, from_addr);
    if (!session)
    {
        printf("❌ Error creando sesión para Sender ID: %u\n", pkt->sender_id);
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

    // Abrir archivo único
    session->file = fopen(session->filename, "wb");
    if (!session->file)
    {
        printf("❌ Error creando archivo: %s\n", session->filename);
        pthread_mutex_unlock(&session->session_mutex);
        cleanup_session(state, pkt->sender_id);
        return -1;
    }

    session->last_activity = time(NULL);

    printf("✅ Transferencia iniciada:\n");
    printf("   📁 Archivo: %s\n", session->filename);
    printf("   📦 Paquetes: %u\n", session->total_packets);
    printf("   🆔 Sender ID: %u\n", session->sender_id);

    pthread_mutex_unlock(&session->session_mutex);

    // Enviar ACK
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

// Hilo trabajador para procesar paquetes
void *packet_worker(void *arg)
{
    multi_receiver_state_t *state = (multi_receiver_state_t *)arg;

    while (state->running)
    {
        packet_t pkt;
        struct sockaddr_in from_addr;

        if (dequeue_packet(&packet_queue, &pkt, &from_addr) == 0)
        {
            printf("🔄 Procesando paquete tipo=%d, sender_id=%u, seq=%u\n",
                   pkt.type, pkt.sender_id, pkt.seq_num);

            switch (pkt.type)
            {
            case PKT_START:
                handle_multi_start(state, &pkt, &from_addr);
                break;

            case PKT_DATA:
            {
                transfer_session_t *session = find_session(state, pkt.sender_id);
                if (session)
                {
                    pthread_mutex_lock(&session->session_mutex);

                    // Verificar duplicado
                    if (!session->received_packets[pkt.seq_num])
                    {
                        session->received_packets[pkt.seq_num] = 1;

                        // Escribir datos
                        long file_pos = (pkt.seq_num - 1) * MAX_DATA_SIZE;
                        fseek(session->file, file_pos, SEEK_SET);
                        fwrite(pkt.data, 1, pkt.data_size, session->file);
                        fflush(session->file);

                        printf("📦 Paquete %u/%u recibido para %s\n",
                               pkt.seq_num, session->total_packets, session->filename);
                    }

                    session->last_activity = time(NULL);

                    // Enviar ACK
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
                transfer_session_t *session = find_session(state, pkt.sender_id);
                if (session)
                {
                    printf("🏁 Transferencia completada para %s\n", session->filename);
                    cleanup_session(state, pkt.sender_id);
                }
                break;
            }
            }
        }
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Uso: %s <puerto>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    printf("🚀 RECEPTOR MULTI-EMISOR INICIADO 🚀\n");
    printf("Puerto: %d\n", port);
    printf("Soporte para emisores concurrentes: ✅\n");
    printf("====================================\n\n");

    // Inicializar estado
    memset(&global_state, 0, sizeof(global_state));
    global_state.running = 1;
    global_state.num_workers = 4; // 4 hilos trabajadores
    pthread_mutex_init(&global_state.sessions_mutex, NULL);

    // Crear socket
    global_state.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (global_state.sockfd < 0)
    {
        perror("Error creando socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(global_state.sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Error en bind");
        return 1;
    }

    // Inicializar cola de paquetes
    init_packet_queue(&packet_queue);

    // Crear hilos trabajadores
    global_state.worker_threads = malloc(global_state.num_workers * sizeof(pthread_t));
    for (int i = 0; i < global_state.num_workers; i++)
    {
        pthread_create(&global_state.worker_threads[i], NULL, packet_worker, &global_state);
    }

    printf("Esperando conexiones...\n\n");

    // Loop principal - solo recibe y encola
    while (global_state.running)
    {
        packet_t pkt;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        ssize_t received = recvfrom(global_state.sockfd, &pkt, sizeof(packet_t), 0,
                                    (struct sockaddr *)&from_addr, &from_len);

        if (received > 0 && verify_checksum(&pkt))
        {
            enqueue_packet(&packet_queue, &pkt, &from_addr);
        }

        usleep(1000); // Pausa mínima
    }

    return 0;
}