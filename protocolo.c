#include "protocolo.h"

// Función para calcular checksum simple
uint32_t calculate_checksum(const packet_t *pkt)
{
    uint32_t checksum = 0;
    checksum += pkt->seq_num;
    checksum += pkt->ack_num;
    checksum += pkt->type;
    checksum += pkt->data_size;
    checksum += pkt->total_packets;
    checksum += pkt->sender_id;

    for (int i = 0; i < pkt->data_size; i++)
    {
        checksum += (uint32_t)pkt->data[i];
    }

    // Cambiar esta parte:
    size_t filename_len = strlen(pkt->filename);
    for (size_t i = 0; i < filename_len; i++)
    {
        checksum += (uint32_t)pkt->filename[i];
    }

    return checksum;
}

// Función para verificar checksum
int verify_checksum(const packet_t *pkt)
{
    uint32_t received_checksum = pkt->checksum;
    packet_t temp_pkt = *pkt;
    temp_pkt.checksum = 0;
    uint32_t calculated_checksum = calculate_checksum(&temp_pkt);
    return received_checksum == calculated_checksum;
}

// Crear paquete
void create_packet(packet_t *pkt, packet_type_t type, uint32_t seq,
                   const char *data, size_t data_size)
{
    memset(pkt, 0, sizeof(packet_t));
    pkt->seq_num = seq;
    pkt->type = type;
    pkt->data_size = (data_size > MAX_DATA_SIZE) ? MAX_DATA_SIZE : data_size;
    pkt->sender_id = 0; // Default sender ID, will be set by sender

    if (data && data_size > 0)
    {
        memcpy(pkt->data, data, pkt->data_size);
    }

    pkt->checksum = calculate_checksum(pkt);
}

// Enviar paquete
int send_packet(int sockfd, const packet_t *pkt, struct sockaddr_in *addr)
{
    ssize_t sent = sendto(sockfd, pkt, sizeof(packet_t), 0,
                          (struct sockaddr *)addr, sizeof(struct sockaddr_in));
    if (sent < 0)
    {
        perror("Error enviando paquete");
        return -1;
    }
    return 0;
}

// Recibir paquete
int receive_packet(int sockfd, packet_t *pkt, struct sockaddr_in *addr, socklen_t *addr_len)
{
    ssize_t received = recvfrom(sockfd, pkt, sizeof(packet_t), 0,
                                (struct sockaddr *)addr, addr_len);
    if (received < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            perror("Error recibiendo paquete");
        }
        return -1;
    }

    if (!verify_checksum(pkt))
    {
        printf("Checksum inválido en paquete seq=%u\n", pkt->seq_num);
        return -2;
    }

    return 0;
}

// Imprimir información del paquete
void print_packet_info(const packet_t *pkt, const char *direction)
{
    const char *type_str;
    switch (pkt->type)
    {
    case PKT_START:
        type_str = "START";
        break;
    case PKT_DATA:
        type_str = "DATA";
        break;
    case PKT_ACK:
        type_str = "ACK";
        break;
    case PKT_END:
        type_str = "END";
        break;
    case PKT_NACK:
        type_str = "NACK";
        break;
    default:
        type_str = "UNKNOWN";
        break;
    }

    printf("%s paquete: tipo=%s, seq=%u, ack=%u, data_size=%u, sender_id=%u\n",
           direction, type_str, pkt->seq_num, pkt->ack_num, pkt->data_size, pkt->sender_id);
}

// Inicializar emisor
int sender_init(sender_state_t *state, const char *host, int port, const char *filename)
{
    memset(state, 0, sizeof(sender_state_t));

    // Crear socket
    state->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->sockfd < 0)
    {
        perror("Error creando socket");
        return -1;
    }

    // Configurar timeout
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000; // 100ms
    setsockopt(state->sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Configurar dirección destino
    memset(&state->dest_addr, 0, sizeof(state->dest_addr));
    state->dest_addr.sin_family = AF_INET;
    state->dest_addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &state->dest_addr.sin_addr);

    // Abrir archivo
    state->file = fopen(filename, "rb");
    if (!state->file)
    {
        perror("Error abriendo archivo");
        close(state->sockfd);
        return -1;
    }

    // Guardar nombre del archivo
    strncpy(state->filename, filename, MAX_FILENAME_SIZE - 1);
    state->filename[MAX_FILENAME_SIZE - 1] = '\0';

    // Calcular total de paquetes
    fseek(state->file, 0, SEEK_END);
    long file_size = ftell(state->file);
    fseek(state->file, 0, SEEK_SET);

    if (file_size == 0)
    {
        state->total_packets = 1; // At least one packet even for empty files
    }
    else
    {
        state->total_packets = (file_size + MAX_DATA_SIZE - 1) / MAX_DATA_SIZE;
    }

    // Generate unique sender ID
    state->sender_id = (uint32_t)(time(NULL) + getpid());

    state->next_seq = 1;
    state->base_seq = 1;

    pthread_mutex_init(&state->mutex, NULL);

    printf("Archivo: %s, Tamaño: %ld bytes, Paquetes: %u\n",
           filename, file_size, state->total_packets);

    return 0;
}

// Iniciar transmisión
int sender_start_transmission(sender_state_t *state)
{
    packet_t start_pkt;
    memset(&start_pkt, 0, sizeof(packet_t));

    start_pkt.seq_num = 1; // Changed from 0 to 1
    start_pkt.type = PKT_START;
    start_pkt.total_packets = state->total_packets;
    start_pkt.sender_id = state->sender_id; // Include sender ID
    strncpy(start_pkt.filename, state->filename, MAX_FILENAME_SIZE - 1);
    start_pkt.checksum = calculate_checksum(&start_pkt);

    // Enviar START y esperar ACK
    for (int retry = 0; retry < MAX_RETRIES; retry++)
    {
        print_packet_info(&start_pkt, "ENVIANDO");
        send_packet(state->sockfd, &start_pkt, &state->dest_addr);

        packet_t ack;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        if (receive_packet(state->sockfd, &ack, &from_addr, &from_len) == 0 &&
            ack.type == PKT_ACK && ack.ack_num == 1) // Changed from 0 to 1
        {
            print_packet_info(&ack, "RECIBIDO");
            printf("Transmisión iniciada exitosamente\n");
            return 0;
        }

        printf("Reintentando START (%d/%d)\n", retry + 1, MAX_RETRIES);
        usleep(TIMEOUT_SEC * 1000000);
    }

    printf("Error: No se pudo iniciar transmisión\n");
    return -1;
}

// Función corregida para enviar ventana
int sender_send_window(sender_state_t *state)
{
    pthread_mutex_lock(&state->mutex);

    // Enviar hasta llenar la ventana
    while (state->next_seq < state->base_seq + WINDOW_SIZE &&
           state->next_seq <= state->total_packets)
    {
        int window_index = (state->next_seq - 1) % WINDOW_SIZE;
        window_slot_t *slot = &state->window[window_index];

        // Solo enviar si no está ya enviado
        if (!slot->sent)
        {
            // Calcular posición en el archivo
            long file_pos = (state->next_seq - 1) * MAX_DATA_SIZE;
            fseek(state->file, file_pos, SEEK_SET);

            char buffer[MAX_DATA_SIZE];
            size_t bytes_read = fread(buffer, 1, MAX_DATA_SIZE, state->file);

            if (feof(state->file) || bytes_read > 0) // Permitir paquetes vacíos para archivos pequeños
            {
                create_packet(&slot->packet, PKT_DATA, state->next_seq, buffer, bytes_read);
                slot->packet.sender_id = state->sender_id;
                slot->packet.checksum = calculate_checksum(&slot->packet);

                if (send_packet(state->sockfd, &slot->packet, &state->dest_addr) == 0)
                {
                    slot->sent = 1;
                    gettimeofday(&slot->sent_time, NULL);
                    print_packet_info(&slot->packet, "ENVIANDO");
                    state->next_seq++;
                }
                else
                {
                    printf("Error enviando paquete %u\n", state->next_seq);
                    break;
                }
            }
            else
            {
                printf("Error leyendo archivo en posición %ld\n", file_pos);
                break;
            }
        }
        else
        {
            // El slot ya está ocupado, salir del bucle
            break;
        }
    }

    pthread_mutex_unlock(&state->mutex);
    return 0;
}

// Manejar ACK recibido
int sender_handle_ack(sender_state_t *state, const packet_t *ack)
{
    pthread_mutex_lock(&state->mutex);

    if (ack->type == PKT_ACK && ack->ack_num >= state->base_seq && ack->ack_num <= state->total_packets)
    {
        printf("ACK recibido para paquete: %u (base actual: %u)\n", ack->ack_num, state->base_seq);

        // Marcar todos los paquetes hasta ack_num como confirmados
        for (uint32_t seq = state->base_seq; seq <= ack->ack_num; seq++)
        {
            int window_index = (seq - 1) % WINDOW_SIZE;
            state->window[window_index].sent = 0;
        }

        // Actualizar base de la ventana
        if (ack->ack_num >= state->base_seq)
        {
            state->base_seq = ack->ack_num + 1;
            printf("Nueva base de ventana: %u\n", state->base_seq);
        }
    }
    else
    {
        printf("ACK fuera de rango o inválido: seq=%u, ack=%u, base=%u\n",
               ack->seq_num, ack->ack_num, state->base_seq);
    }

    pthread_mutex_unlock(&state->mutex);
    return 0;
}
// Manejar NACK recibido
int sender_handle_nack(sender_state_t *state, const packet_t *nack)
{
    pthread_mutex_lock(&state->mutex);

    if (nack->type == PKT_NACK && nack->ack_num > 0 && nack->ack_num <= state->total_packets)
    {
        printf("NACK recibido para paquete %u, retransmitiendo\n", nack->ack_num);

        // Retransmitir el paquete solicitado
        uint32_t seq_num = nack->ack_num;
        int window_index = (seq_num - 1) % WINDOW_SIZE;
        window_slot_t *slot = &state->window[window_index];

        if (slot->sent)
        {
            // Re-send the packet
            send_packet(state->sockfd, &slot->packet, &state->dest_addr);
            gettimeofday(&slot->sent_time, NULL);
            print_packet_info(&slot->packet, "RETRANSMITIENDO");
        }
    }

    pthread_mutex_unlock(&state->mutex);
    return 0;
}

// Esperar a que todos los paquetes sean acknowledgados
int sender_wait_for_completion(sender_state_t *state)
{
    printf("Esperando confirmación de todos los paquetes...\n");

    struct timeval start_time, current_time;
    gettimeofday(&start_time, NULL);

    while (state->base_seq <= state->total_packets)
    {
        // Recibir y procesar ACKs
        packet_t ack;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        int result = receive_packet(state->sockfd, &ack, &from_addr, &from_len);
        if (result == 0)
        {
            print_packet_info(&ack, "RECIBIDO");
            sender_handle_ack(state, &ack);
        }

        // Check timeout
        gettimeofday(&current_time, NULL);
        double elapsed = (current_time.tv_sec - start_time.tv_sec) +
                         (current_time.tv_usec - start_time.tv_usec) / 1000000.0;

        if (elapsed > TIMEOUT_SEC * 2)
        {
            printf("Timeout esperando confirmación final\n");
            return -1;
        }

        usleep(10000); // 10ms
    }

    printf("Todos los paquetes confirmados\n");
    return 0;
}

// Limpiar emisor
void sender_cleanup(sender_state_t *state)
{
    if (state->file)
    {
        fclose(state->file);
    }
    if (state->sockfd >= 0)
    {
        close(state->sockfd);
    }
    pthread_mutex_destroy(&state->mutex);
}

// Inicializar receptor
int receiver_init(receiver_state_t *state, int port)
{
    memset(state, 0, sizeof(receiver_state_t));

    // Crear socket
    state->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->sockfd < 0)
    {
        perror("Error creando socket");
        return -1;
    }

    // Configurar dirección
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(state->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Error en bind");
        close(state->sockfd);
        return -1;
    }

    state->expected_seq = 1;
    state->client_len = sizeof(state->client_addr);
    pthread_mutex_init(&state->mutex, NULL);

    printf("Receptor iniciado en puerto %d\n", port);
    return 0;
}

// Manejar paquete START
int receiver_handle_start(receiver_state_t *state, const packet_t *pkt)
{
    state->total_packets = pkt->total_packets;
    state->sender_id = pkt->sender_id; // Store sender ID
    strncpy(state->filename, pkt->filename, MAX_FILENAME_SIZE - 1);

    // Ensure we have at least 1 packet
    if (state->total_packets == 0)
    {
        state->total_packets = 1;
    }

    // Inicializar array de paquetes recibidos
    state->received_packets = calloc(state->total_packets + 1, sizeof(int));
    if (!state->received_packets)
    {
        perror("Error allocando memoria para paquetes recibidos");
        return -1;
    }

    // Abrir archivo para escritura
    state->file = fopen(state->filename, "wb");
    if (!state->file)
    {
        perror("Error creando archivo");
        free(state->received_packets);
        state->received_packets = NULL;
        return -1;
    }

    printf("Iniciando recepción: %s, %u paquetes, Sender ID: %u\n",
           state->filename, state->total_packets, state->sender_id);

    return 0;
}

// En receptor.c, modificar receiver_handle_data para enviar ACK individual
int receiver_handle_data(receiver_state_t *state, const packet_t *pkt)
{
    pthread_mutex_lock(&state->mutex);

    // Verificar sender ID
    if (pkt->sender_id != state->sender_id)
    {
        printf("Sender ID no coincide (%u != %u)\n", pkt->sender_id, state->sender_id);
        pthread_mutex_unlock(&state->mutex);
        return -1;
    }

    // Verificar rango válido
    if (pkt->seq_num == 0 || pkt->seq_num > state->total_packets)
    {
        printf("Número de secuencia inválido: %u\n", pkt->seq_num);
        pthread_mutex_unlock(&state->mutex);
        return -1;
    }

    // Verificar duplicado
    if (state->received_packets[pkt->seq_num])
    {
        printf("Paquete duplicado: %u\n", pkt->seq_num);
        receiver_send_ack(state, pkt->seq_num);
        pthread_mutex_unlock(&state->mutex);
        return 0;
    }

    // Marcar como recibido
    state->received_packets[pkt->seq_num] = 1;

    // Escribir datos
    long file_pos = (pkt->seq_num - 1) * MAX_DATA_SIZE;
    fseek(state->file, file_pos, SEEK_SET);
    fwrite(pkt->data, 1, pkt->data_size, state->file);
    fflush(state->file);

    printf("Paquete %u/%u recibido (%u bytes)\n",
        pkt->seq_num, state->total_packets, (unsigned int)pkt->data_size);

    // Enviar ACK individual (no acumulativo)
    receiver_send_ack(state, pkt->seq_num);

    pthread_mutex_unlock(&state->mutex);
    return 0;
}

// Enviar ACK
int receiver_send_ack(receiver_state_t *state, uint32_t seq_num)
{
    packet_t ack;
    create_packet(&ack, PKT_ACK, seq_num, NULL, 0); // Use seq_num instead of 0
    ack.ack_num = seq_num;
    ack.sender_id = state->sender_id; // Include sender ID
    ack.checksum = calculate_checksum(&ack);

    send_packet(state->sockfd, &ack, &state->client_addr);
    print_packet_info(&ack, "ENVIANDO");

    return 0;
}

// Enviar NACK
int receiver_send_nack(receiver_state_t *state, uint32_t seq_num)
{
    packet_t nack;
    create_packet(&nack, PKT_NACK, 0, NULL, 0);
    nack.ack_num = seq_num;
    nack.sender_id = state->sender_id; // Include sender ID
    nack.checksum = calculate_checksum(&nack);

    send_packet(state->sockfd, &nack, &state->client_addr);
    print_packet_info(&nack, "ENVIANDO");

    return 0;
}

// Limpiar receptor
void receiver_cleanup(receiver_state_t *state)
{
    if (state->file)
    {
        fclose(state->file);
    }
    if (state->sockfd >= 0)
    {
        close(state->sockfd);
    }
    if (state->received_packets)
    {
        free(state->received_packets);
    }
    pthread_mutex_destroy(&state->mutex);
}
