#include "protocolo.h"
#include <signal.h>

static receiver_state_t global_state;
static volatile int running = 1;

void signal_handler(int sig)
{
    printf("\nInterrumpido por usuario, limpiando...\n");
    running = 0;
    receiver_cleanup(&global_state);
    exit(0);
}

void print_usage(const char *program_name)
{
    printf("Uso: %s <puerto>\n", program_name);
    printf("\nEjemplo:\n");
    printf("  %s 8080\n", program_name);
}

int check_transfer_complete(receiver_state_t *state)
{
    if (!state->received_packets || state->total_packets == 0)
    {
        return 0;
    }

    for (uint32_t i = 1; i <= state->total_packets; i++)
    {
        if (!state->received_packets[i])
        {
            return 0;
        }
    }
    return 1;
}

void print_transfer_stats(receiver_state_t *state)
{
    if (!state->received_packets || state->total_packets == 0)
    {
        return;
    }

    uint32_t received_count = 0;
    for (uint32_t i = 1; i <= state->total_packets; i++)
    {
        if (state->received_packets[i])
        {
            received_count++;
        }
    }

    float progress = ((float)received_count / state->total_packets) * 100;
    printf("Progreso: %.1f%% (%u/%u paquetes recibidos)\n",
           progress, received_count, state->total_packets);
}

int main(int argc, char *argv[])
{
    char output_dir[256] = "./";

    if (argc != 2)
    {
        fprintf(stderr, "Error: Se requiere exactamente 1 argumento (puerto)\n");
        print_usage(argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);

    if (port <= 0 || port > 65535)
    {
        fprintf(stderr, "Error: Puerto inválido %d\n", port);
        return 1;
    }

    // Verificar que el directorio existe
    if (access(output_dir, W_OK) != 0)
    {
        fprintf(stderr, "Error: No se puede escribir en el directorio %s\n", output_dir);
        return 1;
    }

    // Configurar manejador de señales
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=== RECEPTOR INICIADO ===\n");
    printf("Puerto: %d\n", port);
    printf("Directorio de salida: %s\n", output_dir);
    printf("========================\n\n");

    // Inicializar receptor
    if (receiver_init(&global_state, port) < 0)
    {
        fprintf(stderr, "Error inicializando receptor\n");
        return 1;
    }

    printf("Esperando conexiones...\n\n");

    int transfer_active = 0;
    int packets_since_last_update = 0;

    // Loop principal de recepción
    while (running)
    {
        packet_t pkt;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        int result = receive_packet(global_state.sockfd, &pkt, &from_addr, &from_len);

        if (result == 0)
        {
            print_packet_info(&pkt, "RECIBIDO");

            // Guardar dirección del cliente
            if (!transfer_active)
            {
                global_state.client_addr = from_addr;
                global_state.client_len = from_len;
            }

            switch (pkt.type)
            {
            case PKT_START:
                printf("\n=== INICIANDO NUEVA TRANSFERENCIA ===\n");

                // Crear nuevo nombre de archivo con "_recibido" preservando extensión
                char original_filename[256];
                char new_filename[256];
                strncpy(original_filename, pkt.filename, sizeof(original_filename) - 1);
                original_filename[sizeof(original_filename) - 1] = '\0';

                // Preservar extensión original
                char *dot = strrchr(original_filename, '.');
                if (dot != NULL)
                {
                    // Insertar "_recibido" antes de la extensión
                    char temp[256];
                    strncpy(temp, dot, sizeof(temp) - 1);
                    temp[sizeof(temp) - 1] = '\0';
                    *dot = '\0';
                    snprintf(new_filename, sizeof(new_filename), "%s_recibido%s", original_filename, temp);
                }
                else
                {
                    // No hay extensión, solo agregar "_recibido"
                    snprintf(new_filename, sizeof(new_filename), "%s_recibido", original_filename);
                }

                // Preparar nombre de archivo completo con directorio
                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s%s", output_dir, new_filename);
                strncpy(global_state.filename, full_path, MAX_FILENAME_SIZE - 1);

                if (receiver_handle_start(&global_state, &pkt) == 0)
                {
                    receiver_send_ack(&global_state, 1);  // ACK for START packet with seq=1
                    transfer_active = 1;
                    packets_since_last_update = 0;
                    printf("Archivo: %s\n", global_state.filename);
                    printf("Total paquetes esperados: %u\n", global_state.total_packets);
                    printf("====================================\n\n");
                }
                else
                {
                    receiver_send_nack(&global_state, 1);  // NACK for START packet with seq=1
                }
                break;

            case PKT_DATA:
                if (transfer_active)
                {
                    receiver_handle_data(&global_state, &pkt);
                    packets_since_last_update++;

                    // Mostrar progreso cada 50 paquetes
                    if (packets_since_last_update >= 50)
                    {
                        print_transfer_stats(&global_state);
                        packets_since_last_update = 0;
                    }

                    // Verificar si la transferencia está completa
                    if (check_transfer_complete(&global_state))
                    {
                        printf("\n=== TRANSFERENCIA COMPLETADA ===\n");
                        printf("Archivo recibido exitosamente: %s\n", global_state.filename);
                        print_transfer_stats(&global_state);
                        printf("===============================\n\n");

                        // Limpiar para próxima transferencia
                        if (global_state.file)
                        {
                            fclose(global_state.file);
                            global_state.file = NULL;
                        }
                        if (global_state.received_packets)
                        {
                            free(global_state.received_packets);
                            global_state.received_packets = NULL;
                        }
                        transfer_active = 0;
                        global_state.total_packets = 0;
                        global_state.expected_seq = 1;

                        printf("Esperando nueva transferencia...\n");
                    }
                }
                break;

            case PKT_END:
                printf("\nPaquete END recibido\n");
                if (transfer_active)
                {
                    if (check_transfer_complete(&global_state))
                    {
                        printf("Transferencia finalizada correctamente\n");
                        printf("Archivo recibido exitosamente: %s\n", global_state.filename);
                        print_transfer_stats(&global_state);
                        
                        // Limpiar para próxima transferencia
                        if (global_state.file)
                        {
                            fclose(global_state.file);
                            global_state.file = NULL;
                        }
                        if (global_state.received_packets)
                        {
                            free(global_state.received_packets);
                            global_state.received_packets = NULL;
                        }
                        transfer_active = 0;
                        global_state.total_packets = 0;
                        global_state.expected_seq = 1;

                        printf("Esperando nueva transferencia...\n");
                    }
                    else
                    {
                        printf("ADVERTENCIA: Transferencia incompleta\n");
                        print_transfer_stats(&global_state);

                        // Solicitar retransmisión de paquetes faltantes
                        printf("Solicitando retransmisión de paquetes faltantes...\n");
                        for (uint32_t i = 1; i <= global_state.total_packets; i++)
                        {
                            if (!global_state.received_packets[i])
                            {
                                printf("Solicitando paquete %u\n", i);
                                receiver_send_nack(&global_state, i);
                            }
                        }
                    }
                }
                break;

            default:
                printf("Tipo de paquete desconocido: %d\n", pkt.type);
                break;
            }
        }
        else if (result == -2)
        {
            printf("Paquete con checksum inválido ignorado\n");
        }

        usleep(1000); // Pequeña pausa para evitar saturar la CPU
    }

    printf("\nCerrando receptor...\n");
    receiver_cleanup(&global_state);

    return 0;
}