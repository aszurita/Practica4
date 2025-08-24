/**
 * @file protocolo.c
 * @brief Implementación del protocolo de comunicación UDP confiable
 * @author Angelo Zurita
 * @date 2024
 * @version 1.0
 *
 * @description
 * Este archivo implementa todas las funciones del protocolo de comunicación
 * UDP confiable definidas en protocolo.h. Incluye:
 *
 * - Funciones de manipulación de paquetes (creación, envío, recepción)
 * - Implementación del algoritmo de checksum para detección de errores
 * - Lógica del emisor con ventana deslizante
 * - Lógica del receptor para manejo de sesiones
 * - Funciones de utilidad para debugging y logging
 *
 * El protocolo implementa un mecanismo de ventana deslizante similar a TCP
 * pero optimizado para transferencias de archivos sobre UDP.
 */

#include "protocolo.h"

// ============================================================================
// FUNCIONES DEL PROTOCOLO BASE
// ============================================================================

/**
 * @brief Calcula el checksum de un paquete para verificación de integridad
 *
 * @param pkt Puntero al paquete cuyo checksum se calculará
 * @return uint32_t Valor del checksum calculado
 *
 * @details
 * Implementa un algoritmo de checksum simple pero efectivo que suma:
 * - Todos los campos numéricos del paquete
 * - Cada byte de los datos del paquete
 * - Cada byte del nombre del archivo
 *
 * Este checksum permite detectar corrupción de datos durante la transmisión
 * UDP, que es inherentemente no confiable.
 */
uint32_t calculate_checksum(const packet_t *pkt)
{
    uint32_t checksum = 0;

    // Sumar campos de control del paquete
    checksum += pkt->seq_num;
    checksum += pkt->ack_num;
    checksum += pkt->type;
    checksum += pkt->data_size;
    checksum += pkt->total_packets;
    checksum += pkt->sender_id;

    // Sumar cada byte de los datos del paquete
    for (int i = 0; i < pkt->data_size; i++)
    {
        checksum += (uint32_t)pkt->data[i];
    }

    // Sumar cada byte del nombre del archivo
    size_t filename_len = strlen(pkt->filename);
    for (size_t i = 0; i < filename_len; i++)
    {
        checksum += (uint32_t)pkt->filename[i];
    }

    return checksum;
}

/**
 * @brief Verifica la integridad de un paquete comparando checksums
 *
 * @param pkt Puntero al paquete a verificar
 * @return int 0 si el checksum es válido, -1 si no
 *
 * @details
 * Para verificar el checksum:
 * 1. Guarda el checksum recibido
 * 2. Crea una copia temporal del paquete con checksum = 0
 * 3. Calcula el checksum de la copia temporal
 * 4. Compara ambos valores
 *
 * Si no coinciden, el paquete se considera corrupto.
 */
int verify_checksum(const packet_t *pkt)
{
    uint32_t received_checksum = pkt->checksum;
    packet_t temp_pkt = *pkt;
    temp_pkt.checksum = 0; // Reset checksum para cálculo
    uint32_t calculated_checksum = calculate_checksum(&temp_pkt);

    return received_checksum == calculated_checksum;
}

/**
 * @brief Crea un nuevo paquete con los parámetros especificados
 *
 * @param pkt Puntero al paquete a crear (debe estar pre-allocado)
 * @param type Tipo de paquete (START, DATA, ACK, etc.)
 * @param seq Número de secuencia del paquete
 * @param data Datos a incluir en el paquete (puede ser NULL)
 * @param data_size Tamaño de los datos en bytes
 *
 * @details
 * La función:
 * 1. Inicializa todos los campos del paquete a 0
 * 2. Establece los campos de control (tipo, secuencia, tamaño)
 * 3. Copia los datos si se proporcionan
 * 4. Calcula y establece el checksum del paquete
 *
 * El paquete resultante está listo para ser enviado.
 */
void create_packet(packet_t *pkt, packet_type_t type, uint32_t seq,
                   const char *data, size_t data_size)
{
    // Inicializar todo el paquete a 0
    memset(pkt, 0, sizeof(packet_t));

    // Establecer campos de control
    pkt->seq_num = seq;
    pkt->type = type;
    pkt->data_size = (data_size > MAX_DATA_SIZE) ? MAX_DATA_SIZE : data_size;
    pkt->sender_id = 0; // Default sender ID, será establecido por el emisor

    // Copiar datos si se proporcionan
    if (data && data_size > 0)
    {
        memcpy(pkt->data, data, pkt->data_size);
    }

    // Calcular y establecer checksum
    pkt->checksum = calculate_checksum(pkt);
}

/**
 * @brief Envía un paquete completo a través del socket UDP
 *
 * @param sockfd Descriptor del socket UDP
 * @param pkt Puntero al paquete a enviar
 * @param addr Dirección de destino (estructura sockaddr_in)
 * @return int 0 en éxito, -1 en error
 *
 * @details
 * Utiliza la función sendto() del sistema para enviar el paquete completo.
 * La función maneja automáticamente la fragmentación UDP si es necesaria.
 *
 * @note
 * Esta función es bloqueante hasta que el paquete se envíe o ocurra un error.
 */
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

/**
 * @brief Recibe un paquete del socket UDP con verificación de integridad
 *
 * @param sockfd Descriptor del socket UDP
 * @param pkt Puntero donde almacenar el paquete recibido
 * @param addr Dirección del emisor (se llena automáticamente)
 * @param addr_len Longitud de la dirección del emisor
 * @return int 0 en éxito, -1 en error de red, -2 en checksum inválido
 *
 * @details
 * La función:
 * 1. Recibe el paquete usando recvfrom()
 * 2. Verifica la integridad usando checksum
 * 3. Llena la dirección del emisor para respuesta
 *
 * Maneja apropiadamente timeouts y errores de red.
 */
int receive_packet(int sockfd, packet_t *pkt, struct sockaddr_in *addr, socklen_t *addr_len)
{
    ssize_t received = recvfrom(sockfd, pkt, sizeof(packet_t), 0,
                                (struct sockaddr *)addr, addr_len);

    if (received < 0)
    {
        // Ignorar errores de timeout (EAGAIN/EWOULDBLOCK)
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            perror("Error recibiendo paquete");
        }
        return -1;
    }

    // Verificar integridad del paquete recibido
    if (!verify_checksum(pkt))
    {
        printf("Checksum inválido en paquete seq=%u\n", pkt->seq_num);
        return -2;
    }

    return 0;
}

/**
 * @brief Imprime información detallada de un paquete para debugging
 *
 * @param pkt Puntero al paquete a mostrar
 * @param direction Dirección del paquete ("ENVIANDO", "RECIBIDO", etc.)
 *
 * @details
 * Función de utilidad que muestra todos los campos relevantes del paquete:
 * - Tipo de paquete (convertido a string legible)
 * - Número de secuencia
 * - Número de acknowledgment
 * - Tamaño de datos
 * - ID del emisor
 *
 * Útil para debugging y monitoreo de la comunicación.
 */
void print_packet_info(const packet_t *pkt, const char *direction)
{
    // Convertir tipo de paquete a string legible
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

    // Imprimir información del paquete
    printf("%s paquete: tipo=%s, seq=%u, ack=%u, data_size=%u, sender_id=%u\n",
           direction, type_str, pkt->seq_num, pkt->ack_num, pkt->data_size, pkt->sender_id);
}

// ============================================================================
// FUNCIONES DEL EMISOR
// ============================================================================

/**
 * @brief Inicializa el estado completo del emisor
 *
 * @param state Puntero al estado a inicializar
 * @param host Dirección IP del receptor (string)
 * @param port Puerto del receptor
 * @param filename Nombre del archivo a transmitir
 * @return int 0 en éxito, -1 en error
 *
 * @details
 * La función realiza la inicialización completa del emisor:
 * 1. Crea y configura el socket UDP
 * 2. Establece timeout de recepción
 * 3. Configura la dirección de destino
 * 4. Abre el archivo a transmitir
 * 5. Calcula el número total de paquetes
 * 6. Genera un ID único para el emisor
 * 7. Inicializa la ventana deslizante y mutex
 *
 * @note
 * El archivo debe existir y ser legible. El socket se configura con
 * timeout para evitar bloqueos indefinidos.
 */
int sender_init(sender_state_t *state, const char *host, int port, const char *filename)
{
    // Inicializar estructura de estado
    memset(state, 0, sizeof(sender_state_t));

    // Crear socket UDP
    state->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->sockfd < 0)
    {
        perror("Error creando socket");
        return -1;
    }

    // Configurar timeout de recepción para evitar bloqueos
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000; // 100ms
    setsockopt(state->sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Configurar dirección de destino
    memset(&state->dest_addr, 0, sizeof(state->dest_addr));
    state->dest_addr.sin_family = AF_INET;
    state->dest_addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &state->dest_addr.sin_addr);

    // Abrir archivo a transmitir
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

    // Calcular número total de paquetes
    fseek(state->file, 0, SEEK_END);
    long file_size = ftell(state->file);
    fseek(state->file, 0, SEEK_SET);

    // Asegurar al menos un paquete incluso para archivos vacíos
    if (file_size == 0)
    {
        state->total_packets = 1;
    }
    else
    {
        state->total_packets = (file_size + MAX_DATA_SIZE - 1) / MAX_DATA_SIZE;
    }

    // Generar ID único del emisor usando timestamp y PID
    state->sender_id = (uint32_t)(time(NULL) + getpid());

    // Inicializar secuencia y ventana
    state->next_seq = 1;
    state->base_seq = 1;

    // Inicializar mutex para sincronización entre hilos
    pthread_mutex_init(&state->mutex, NULL);

    // Mostrar información de inicialización
    printf("Archivo: %s, Tamaño: %ld bytes, Paquetes: %u\n",
           filename, file_size, state->total_packets);

    return 0;
}

/**
 * @brief Inicia la transmisión enviando paquete START y esperando confirmación
 *
 * @param state Puntero al estado del emisor
 * @return int 0 en éxito, -1 en error después de MAX_RETRIES
 *
 * @details
 * La función establece la sesión de transmisión:
 * 1. Crea y envía paquete START con metadatos del archivo
 * 2. Espera ACK de confirmación del receptor
 * 3. Reintenta hasta MAX_RETRIES veces si no hay respuesta
 * 4. Establece timeout entre reintentos
 *
 * @note
 * Esta función es crítica para el establecimiento de la sesión.
 * Si falla, la transmisión no puede continuar.
 */
int sender_start_transmission(sender_state_t *state)
{
    packet_t start_pkt;
    memset(&start_pkt, 0, sizeof(packet_t));

    // Configurar paquete START
    start_pkt.seq_num = 1; // Secuencia inicial
    start_pkt.type = PKT_START;
    start_pkt.total_packets = state->total_packets;
    start_pkt.sender_id = state->sender_id;
    strncpy(start_pkt.filename, state->filename, MAX_FILENAME_SIZE - 1);
    start_pkt.checksum = calculate_checksum(&start_pkt);

    // Enviar START y esperar ACK con reintentos
    for (int retry = 0; retry < MAX_RETRIES; retry++)
    {
        print_packet_info(&start_pkt, "ENVIANDO");
        send_packet(state->sockfd, &start_pkt, &state->dest_addr);

        // Esperar ACK de confirmación
        packet_t ack;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        if (receive_packet(state->sockfd, &ack, &from_addr, &from_len) == 0 &&
            ack.type == PKT_ACK && ack.ack_num == 1)
        {
            print_packet_info(&ack, "RECIBIDO");
            printf("Transmisión iniciada exitosamente\n");
            return 0;
        }

        printf("Reintentando START (%d/%d)\n", retry + 1, MAX_RETRIES);
        usleep(TIMEOUT_SEC * 1000000); // Esperar antes de reintentar
    }

    printf("Error: No se pudo iniciar transmisión\n");
    return -1;
}

/**
 * @brief Envía paquetes hasta llenar la ventana deslizante
 *
 * @param state Puntero al estado del emisor
 * @return int 0 en éxito, -1 en error
 *
 * @details
 * Implementa el algoritmo de ventana deslizante:
 * 1. Envía paquetes hasta llenar la ventana o enviar todos los disponibles
 * 2. Lee datos del archivo en bloques de MAX_DATA_SIZE
 * 3. Crea paquetes DATA con los datos leídos
 * 4. Almacena paquetes en la ventana para posible retransmisión
 * 5. Marca paquetes como enviados y registra timestamp
 *
 * @note
 * Esta función es thread-safe usando mutex para proteger el estado compartido.
 */
int sender_send_window(sender_state_t *state)
{
    pthread_mutex_lock(&state->mutex);

    // Enviar paquetes hasta llenar la ventana o enviar todos los disponibles
    while (state->next_seq < state->base_seq + WINDOW_SIZE &&
           state->next_seq <= state->total_packets)
    {
        // Calcular índice en la ventana (circular)
        int window_index = (state->next_seq - 1) % WINDOW_SIZE;
        window_slot_t *slot = &state->window[window_index];

        // Solo enviar si el slot no está ocupado
        if (!slot->sent)
        {
            // Calcular posición en el archivo para este paquete
            long file_pos = (state->next_seq - 1) * MAX_DATA_SIZE;
            fseek(state->file, file_pos, SEEK_SET);

            // Leer datos del archivo
            char buffer[MAX_DATA_SIZE];
            size_t bytes_read = fread(buffer, 1, MAX_DATA_SIZE, state->file);

            // Permitir paquetes vacíos para archivos pequeños
            if (feof(state->file) || bytes_read > 0)
            {
                // Crear paquete DATA con los datos leídos
                create_packet(&slot->packet, PKT_DATA, state->next_seq, buffer, bytes_read);
                slot->packet.sender_id = state->sender_id;
                slot->packet.checksum = calculate_checksum(&slot->packet);

                // Enviar paquete
                if (send_packet(state->sockfd, &slot->packet, &state->dest_addr) == 0)
                {
                    // Marcar como enviado y registrar timestamp
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

/**
 * @brief Procesa un ACK recibido del receptor
 *
 * @param state Puntero al estado del emisor
 * @param ack Puntero al paquete ACK recibido
 * @return int 0 en éxito, -1 en error
 *
 * @details
 * Cuando se recibe un ACK:
 * 1. Verifica que el ACK sea válido y esté en rango
 * 2. Marca todos los paquetes hasta ack_num como confirmados
 * 3. Actualiza la base de la ventana deslizante
 * 4. Libera slots en la ventana para nuevos paquetes
 *
 * @note
 * Esta función es thread-safe y actualiza el estado de la ventana.
 */
int sender_handle_ack(sender_state_t *state, const packet_t *ack)
{
    pthread_mutex_lock(&state->mutex);

    // Verificar que el ACK sea válido y esté en rango
    if (ack->type == PKT_ACK && ack->ack_num >= state->base_seq && ack->ack_num <= state->total_packets)
    {
        printf("ACK recibido para paquete: %u (base actual: %u)\n", ack->ack_num, state->base_seq);

        // Marcar todos los paquetes hasta ack_num como confirmados
        for (uint32_t seq = state->base_seq; seq <= ack->ack_num; seq++)
        {
            int window_index = (seq - 1) % WINDOW_SIZE;
            state->window[window_index].sent = 0; // Liberar slot
        }

        // Actualizar base de la ventana deslizante
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

/**
 * @brief Procesa un NACK recibido del receptor
 *
 * @param state Puntero al estado del emisor
 * @param nack Puntero al paquete NACK recibido
 * @return int 0 en éxito, -1 en error
 *
 * @details
 * Cuando se recibe un NACK:
 * 1. Verifica que el NACK sea válido
 * 2. Retransmite el paquete solicitado
 * 3. Actualiza el timestamp de envío
 *
 * @note
 * Esta función implementa retransmisión selectiva para eficiencia.
 */
int sender_handle_nack(sender_state_t *state, const packet_t *nack)
{
    pthread_mutex_lock(&state->mutex);

    // Verificar que el NACK sea válido
    if (nack->type == PKT_NACK && nack->ack_num > 0 && nack->ack_num <= state->total_packets)
    {
        printf("NACK recibido para paquete %u, retransmitiendo\n", nack->ack_num);

        // Retransmitir el paquete solicitado
        uint32_t seq_num = nack->ack_num;
        int window_index = (seq_num - 1) % WINDOW_SIZE;
        window_slot_t *slot = &state->window[window_index];

        if (slot->sent)
        {
            // Re-enviar el paquete
            send_packet(state->sockfd, &slot->packet, &state->dest_addr);
            gettimeofday(&slot->sent_time, NULL); // Actualizar timestamp
            print_packet_info(&slot->packet, "RETRANSMITIENDO");
        }
    }

    pthread_mutex_unlock(&state->mutex);
    return 0;
}

/**
 * @brief Espera a que todos los paquetes sean confirmados
 *
 * @param state Puntero al estado del emisor
 * @return int 0 en éxito, -1 en timeout
 *
 * @details
 * Bucle de espera que:
 * 1. Recibe y procesa ACKs del receptor
 * 2. Actualiza el estado de la ventana
 * 3. Continúa hasta que todos los paquetes estén confirmados
 * 4. Implementa timeout para evitar bloqueos indefinidos
 *
 * @note
 * Esta función es crítica para asegurar la entrega completa del archivo.
 */
int sender_wait_for_completion(sender_state_t *state)
{
    printf("Esperando confirmación de todos los paquetes...\n");

    struct timeval start_time, current_time;
    gettimeofday(&start_time, NULL);

    // Esperar hasta que todos los paquetes estén confirmados
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

        // Verificar timeout
        gettimeofday(&current_time, NULL);
        double elapsed = (current_time.tv_sec - start_time.tv_sec) +
                         (current_time.tv_usec - start_time.tv_usec) / 1000000.0;

        if (elapsed > TIMEOUT_SEC * 2)
        {
            printf("Timeout esperando confirmación final\n");
            return -1;
        }

        usleep(10000); // 10ms de pausa
    }

    printf("Todos los paquetes confirmados\n");
    return 0;
}

/**
 * @brief Limpia todos los recursos del emisor
 *
 * @param state Puntero al estado del emisor
 *
 * @details
 * Función de limpieza que:
 * 1. Cierra el archivo abierto
 * 2. Cierra el socket UDP
 * 3. Destruye el mutex de sincronización
 *
 * @note
 * Esta función debe ser llamada antes de terminar el programa
 * para evitar fugas de recursos del sistema.
 */
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

// ============================================================================
// FUNCIONES DEL RECEPTOR
// ============================================================================

/**
 * @brief Inicializa el estado del receptor
 *
 * @param state Puntero al estado a inicializar
 * @param port Puerto en el que escuchar conexiones
 * @return int 0 en éxito, -1 en error
 *
 * @details
 * La función:
 * 1. Crea el socket UDP
 * 2. Lo vincula al puerto especificado
 * 3. Inicializa las variables de estado
 * 4. Prepara el receptor para recibir conexiones
 *
 * @note
 * El receptor escucha en todas las interfaces (INADDR_ANY).
 */
int receiver_init(receiver_state_t *state, int port)
{
    // Inicializar estructura de estado
    memset(state, 0, sizeof(receiver_state_t));

    // Crear socket UDP
    state->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->sockfd < 0)
    {
        perror("Error creando socket");
        return -1;
    }

    // Configurar dirección de escucha
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // Escuchar en todas las interfaces
    addr.sin_port = htons(port);

    // Vincular socket al puerto
    if (bind(state->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Error en bind");
        close(state->sockfd);
        return -1;
    }

    // Inicializar variables de estado
    state->expected_seq = 1;
    state->client_len = sizeof(state->client_addr);
    pthread_mutex_init(&state->mutex, NULL);

    printf("Receptor iniciado en puerto %d\n", port);
    return 0;
}

/**
 * @brief Procesa un paquete START del emisor
 *
 * @param state Puntero al estado del receptor
 * @param pkt Puntero al paquete START recibido
 * @return int 0 en éxito, -1 en error
 *
 * @details
 * Cuando se recibe START:
 * 1. Establece una nueva sesión de transferencia
 * 2. Crea el archivo de destino
 * 3. Inicializa el array de paquetes recibidos
 * 4. Configura el estado para la recepción
 *
 * @note
 * Esta función es crítica para establecer la sesión de transferencia.
 */
int receiver_handle_start(receiver_state_t *state, const packet_t *pkt)
{
    // Configurar sesión de transferencia
    state->total_packets = pkt->total_packets;
    state->sender_id = pkt->sender_id;
    strncpy(state->filename, pkt->filename, MAX_FILENAME_SIZE - 1);

    // Asegurar al menos 1 paquete
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

/**
 * @brief Procesa un paquete DATA del emisor
 *
 * @param state Puntero al estado del receptor
 * @param pkt Puntero al paquete DATA recibido
 * @return int 0 en éxito, -1 en error
 *
 * @details
 * Para cada paquete DATA:
 * 1. Verifica el ID del emisor
 * 2. Valida el número de secuencia
 * 3. Detecta duplicados
 * 4. Escribe datos en la posición correcta del archivo
 * 5. Envía ACK de confirmación
 *
 * @note
 * Esta función implementa recepción ordenada y manejo de duplicados.
 */
int receiver_handle_data(receiver_state_t *state, const packet_t *pkt)
{
    pthread_mutex_lock(&state->mutex);

    // Verificar ID del emisor
    if (pkt->sender_id != state->sender_id)
    {
        printf("Sender ID no coincide (%u != %u)\n", pkt->sender_id, state->sender_id);
        pthread_mutex_unlock(&state->mutex);
        return -1;
    }

    // Verificar rango válido de secuencia
    if (pkt->seq_num == 0 || pkt->seq_num > state->total_packets)
    {
        printf("Número de secuencia inválido: %u\n", pkt->seq_num);
        pthread_mutex_unlock(&state->mutex);
        return -1;
    }

    // Verificar duplicados
    if (state->received_packets[pkt->seq_num])
    {
        printf("Paquete duplicado: %u\n", pkt->seq_num);
        receiver_send_ack(state, pkt->seq_num); // Re-enviar ACK
        pthread_mutex_unlock(&state->mutex);
        return 0;
    }

    // Marcar como recibido
    state->received_packets[pkt->seq_num] = 1;

    // Escribir datos en la posición correcta del archivo
    long file_pos = (pkt->seq_num - 1) * MAX_DATA_SIZE;
    fseek(state->file, file_pos, SEEK_SET);
    fwrite(pkt->data, 1, pkt->data_size, state->file);
    fflush(state->file); // Asegurar escritura inmediata

    printf("Paquete %u/%u recibido (%u bytes)\n",
           pkt->seq_num, state->total_packets, (unsigned int)pkt->data_size);

    // Enviar ACK individual (no acumulativo)
    receiver_send_ack(state, pkt->seq_num);

    pthread_mutex_unlock(&state->mutex);
    return 0;
}

/**
 * @brief Envía un ACK al emisor
 *
 * @param state Puntero al estado del receptor
 * @param seq_num Número de secuencia confirmado
 * @return int 0 en éxito, -1 en error
 *
 * @details
 * Crea y envía un paquete ACK que:
 * 1. Confirma la recepción exitosa de un paquete específico
 * 2. Incluye el ID del emisor para identificación
 * 3. Calcula checksum para integridad
 *
 * @note
 * Los ACKs son individuales para mayor control y eficiencia.
 */
int receiver_send_ack(receiver_state_t *state, uint32_t seq_num)
{
    packet_t ack;
    create_packet(&ack, PKT_ACK, seq_num, NULL, 0);
    ack.ack_num = seq_num;
    ack.sender_id = state->sender_id;
    ack.checksum = calculate_checksum(&ack);

    send_packet(state->sockfd, &ack, &state->client_addr);
    print_packet_info(&ack, "ENVIANDO");

    return 0;
}

/**
 * @brief Envía un NACK al emisor
 *
 * @param state Puntero al estado del receptor
 * @param seq_num Número de secuencia con error
 * @return int 0 en éxito, -1 en error
 *
 * @details
 * Crea y envía un paquete NACK que:
 * 1. Solicita la retransmisión de un paquete específico
 * 2. Se usa cuando se detecta corrupción o pérdida
 * 3. Incluye el ID del emisor para identificación
 *
 * @note
 * Los NACKs implementan retransmisión selectiva para eficiencia.
 */
int receiver_send_nack(receiver_state_t *state, uint32_t seq_num)
{
    packet_t nack;
    create_packet(&nack, PKT_NACK, 0, NULL, 0);
    nack.ack_num = seq_num;
    nack.sender_id = state->sender_id;
    nack.checksum = calculate_checksum(&nack);

    send_packet(state->sockfd, &nack, &state->client_addr);
    print_packet_info(&nack, "ENVIANDO");

    return 0;
}

/**
 * @brief Limpia los recursos del receptor
 *
 * @param state Puntero al estado del receptor
 *
 * @details
 * Función de limpieza que:
 * 1. Cierra el archivo de destino
 * 2. Cierra el socket UDP
 * 3. Libera memoria del array de paquetes
 * 4. Destruye el mutex de sincronización
 *
 * @note
 * Esta función debe ser llamada al finalizar la sesión.
 */
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
