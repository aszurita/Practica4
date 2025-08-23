#include "protocolo.h"
#include <signal.h>

static sender_state_t global_state;
static volatile int running = 1;

void signal_handler(int sig)
{
    printf("\nInterrumpido por usuario, limpiando...\n");
    running = 0;
    sender_cleanup(&global_state);
    exit(0);
}

void *timeout_handler(void *arg)
{
    sender_state_t *state = (sender_state_t *)arg;

    while (running && state->base_seq <= state->total_packets)
    {
        usleep(TIMEOUT_SEC * 1000000); // Esperar timeout

        pthread_mutex_lock(&state->mutex);
        struct timeval now;
        gettimeofday(&now, NULL);

        // Verificar timeouts y retransmitir
        for (int i = 0; i < WINDOW_SIZE; i++)
        {
            window_slot_t *slot = &state->window[i];
            if (slot->sent)
            {
                double elapsed = (now.tv_sec - slot->sent_time.tv_sec) +
                                 (now.tv_usec - slot->sent_time.tv_usec) / 1000000.0;

                if (elapsed > TIMEOUT_SEC)
                {
                    printf("Timeout para paquete %u, retransmitiendo\n",
                           slot->packet.seq_num);

                    send_packet(state->sockfd, &slot->packet, &state->dest_addr);
                    gettimeofday(&slot->sent_time, NULL);
                    print_packet_info(&slot->packet, "RETRANSMITIENDO");
                }
            }
        }
        pthread_mutex_unlock(&state->mutex);
    }

    return NULL;
}

void print_usage(const char *program_name)
{
    printf("Uso: %s <host_destino> <puerto_destino> <nombre_archivo>\n", program_name);
    printf("\nEjemplo:\n");
    printf("  %s 127.0.0.1 8080 archivo_grande.txt\n", program_name);
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "Error: Se requieren exactamente 3 argumentos\n");
        print_usage(argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *filename = argv[3];

    if (port <= 0 || port > 65535)
    {
        fprintf(stderr, "Error: Puerto inválido %d\n", port);
        return 1;
    }

    // Verificar que el archivo existe
    if (access(filename, R_OK) != 0)
    {
        fprintf(stderr, "Error: No se puede leer el archivo %s\n", filename);
        return 1;
    }

    // Configurar manejador de señales
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=== EMISOR INICIADO ===\n");
    printf("Host destino: %s\n", host);
    printf("Puerto: %d\n", port);
    printf("Archivo: %s\n", filename);
    printf("Tamaño de ventana: %d paquetes\n", WINDOW_SIZE);
    printf("Timeout: %d segundos\n", TIMEOUT_SEC);
    printf("=====================\n\n");

    // Inicializar estado del emisor
    if (sender_init(&global_state, host, port, filename) < 0)
    {
        fprintf(stderr, "Error inicializando emisor\n");
        return 1;
    }

    // Iniciar transmisión
    if (sender_start_transmission(&global_state) < 0)
    {
        fprintf(stderr, "Error iniciando transmisión\n");
        sender_cleanup(&global_state);
        return 1;
    }

    // Crear hilo para manejo de timeouts
    pthread_t timeout_thread;
    pthread_create(&timeout_thread, NULL, timeout_handler, &global_state);

    printf("\nIniciando envío de datos...\n");

    // Loop principal de envío
    while (running && global_state.base_seq <= global_state.total_packets)
    {
        // Enviar ventana de paquetes
        sender_send_window(&global_state);

        // Recibir y procesar ACKs
        packet_t ack;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        int result = receive_packet(global_state.sockfd, &ack, &from_addr, &from_len);
        if (result == 0)
        {
            print_packet_info(&ack, "RECIBIDO");
            
            // Handle different packet types
            switch (ack.type) {
                case PKT_ACK:
                    sender_handle_ack(&global_state, &ack);
                    break;
                case PKT_NACK:
                    sender_handle_nack(&global_state, &ack);
                    break;
                default:
                    printf("Paquete inesperado recibido: tipo=%d\n", ack.type);
                    break;
            }
        }

        // Mostrar progreso
        if ((global_state.base_seq - 1) % 50 == 0)
        {
            float progress = ((float)(global_state.base_seq - 1) / global_state.total_packets) * 100;
            printf("Progreso: %.1f%% (%u/%u paquetes confirmados)\n",
                   progress, global_state.base_seq - 1, global_state.total_packets);
        }

        // Check if we've sent all packets and are waiting for final ACKs
        if (global_state.next_seq > global_state.total_packets && 
            global_state.base_seq <= global_state.total_packets)
        {
            usleep(10000); // Wait a bit for ACKs to arrive
        }
        else
        {
            usleep(1000); // Pequeña pausa para evitar saturar la CPU
        }
    }

    // Enviar paquete END
    if (running)
    {
        // Wait for all packets to be acknowledged
        if (sender_wait_for_completion(&global_state) == 0)
        {
            packet_t end_pkt;
            create_packet(&end_pkt, PKT_END, 0, NULL, 0);
            end_pkt.checksum = calculate_checksum(&end_pkt);

            printf("\nEnviando paquete de finalización...\n");
            for (int i = 0; i < 3; i++)
            {
                send_packet(global_state.sockfd, &end_pkt, &global_state.dest_addr);
                print_packet_info(&end_pkt, "ENVIANDO");
                usleep(500000); // 0.5 segundos entre intentos
            }
        }
        else
        {
            printf("Error: No se pudieron confirmar todos los paquetes\n");
        }
    }

    // Esperar a que termine el hilo de timeout
    running = 0;
    pthread_join(timeout_thread, NULL);

    printf("\n=== TRANSMISIÓN COMPLETADA ===\n");
    printf("Total de paquetes enviados: %u\n", global_state.total_packets);
    printf("Archivo enviado exitosamente\n");

    sender_cleanup(&global_state);
    return 0;
}