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
    printf("  %s 8081\n", program_name);
}

// Función mejorada para crear nombre de archivo con sufijo "_recibido"
void create_received_filename(const char *original_filename, char *new_filename, size_t max_size)
{
    // Buscar la última aparición del punto para encontrar la extensión
    const char *dot = strrchr(original_filename, '.');
    const char *slash = strrchr(original_filename, '/');
    
    // Asegurar que el punto encontrado es parte del nombre del archivo, no del directorio
    if (dot && (!slash || dot > slash))
    {
        // Hay extensión
        size_t base_len = dot - original_filename;
        char base_name[256];
        
        // Copiar la parte base del nombre
        if (base_len >= sizeof(base_name))
            base_len = sizeof(base_name) - 1;
        
        strncpy(base_name, original_filename, base_len);
        base_name[base_len] = '\0';
        
        // Crear nuevo nombre: base_recibido.extension
        snprintf(new_filename, max_size, "%s_recibido%s", base_name, dot);
    }
    else
    {
        // No hay extensión
        snprintf(new_filename, max_size, "%s_recibido", original_filename);
    }
    
    printf("Archivo original: %s\n", original_filename);
    printf("Archivo a crear: %s\n", new_filename);
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

void cleanup_transfer(receiver_state_t *state)
{
    if (state->file)
    {
        fclose(state->file);
        state->file = NULL;
    }
    if (state->received_packets)
    {
        free(state->received_packets);
        state->received_packets = NULL;
    }
    state->total_packets = 0;
    state->expected_seq = 1;
    state->sender_id = 0;
    memset(state->filename, 0, sizeof(state->filename));
}

int main(int argc, char *argv[])
{
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

    // Configurar manejador de señales
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=== RECEPTOR INICIADO ===\n");
    printf("Puerto: %d\n", port);
    printf("Directorio de salida: ./\n");
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

            // Guardar dirección del cliente en la primera recepción
            if (!transfer_active || memcmp(&global_state.client_addr, &from_addr, sizeof(from_addr)) != 0)
            {
                global_state.client_addr = from_addr;
                global_state.client_len = from_len;
            }

            switch (pkt.type)
            {
            case PKT_START:
                printf("\n=== INICIANDO NUEVA TRANSFERENCIA ===\n");

                // Si hay una transferencia activa, limpiar primero
                if (transfer_active)
                {
                    printf("Limpiando transferencia anterior...\n");
                    cleanup_transfer(&global_state);
                }

                // Crear nombre de archivo con sufijo "_recibido"
                char received_filename[512];
                create_received_filename(pkt.filename, received_filename, sizeof(received_filename));
                
                // Guardar el nombre de archivo en el estado
                strncpy(global_state.filename, received_filename, MAX_FILENAME_SIZE - 1);
                global_state.filename[MAX_FILENAME_SIZE - 1] = '\0';

                // Actualizar el paquete para usar el nuevo nombre
                packet_t modified_start_pkt = pkt;
                strncpy(modified_start_pkt.filename, received_filename, MAX_FILENAME_SIZE - 1);
                modified_start_pkt.filename[MAX_FILENAME_SIZE - 1] = '\0';

                if (receiver_handle_start(&global_state, &modified_start_pkt) == 0)
                {
                    receiver_send_ack(&global_state, 1);
                    transfer_active = 1;
                    packets_since_last_update = 0;
                    printf("Archivo de destino: %s\n", global_state.filename);
                    printf("Total paquetes esperados: %u\n", global_state.total_packets);
                    printf("Sender ID: %u\n", global_state.sender_id);
                    printf("====================================\n\n");
                }
                else
                {
                    printf("Error iniciando transferencia\n");
                    // Enviar NACK para el paquete START
                    packet_t nack;
                    create_packet(&nack, PKT_NACK, 0, NULL, 0);
                    nack.ack_num = 1;
                    nack.sender_id = pkt.sender_id;
                    nack.checksum = calculate_checksum(&nack);
                    send_packet(global_state.sockfd, &nack, &global_state.client_addr);
                    print_packet_info(&nack, "ENVIANDO");
                }
                break;

            case PKT_DATA:
                if (transfer_active)
                {
                    if (receiver_handle_data(&global_state, &pkt) == 0)
                    {
                        packets_since_last_update++;

                        // Mostrar progreso cada 10 paquetes para archivos pequeños, 50 para grandes
                        int progress_interval = (global_state.total_packets <= 100) ? 10 : 50;
                        if (packets_since_last_update >= progress_interval)
                        {
                            print_transfer_stats(&global_state);
                            packets_since_last_update = 0;
                        }

                        // Verificar si la transferencia está completa
                        if (check_transfer_complete(&global_state))
                        {
                            printf("\n=== TRANSFERENCIA COMPLETADA ===\n");
                            printf("✓ Archivo recibido exitosamente: %s\n", global_state.filename);
                            print_transfer_stats(&global_state);
                            
                            // Verificar tamaño del archivo
                            if (global_state.file)
                            {
                                fflush(global_state.file);
                                long file_size = ftell(global_state.file);
                                printf("✓ Tamaño final del archivo: %ld bytes\n", file_size);
                            }
                            
                            printf("===============================\n\n");

                            // Limpiar para próxima transferencia
                            cleanup_transfer(&global_state);
                            transfer_active = 0;

                            printf("Esperando nueva transferencia...\n");
                        }
                    }
                }
                else
                {
                    printf("Paquete DATA recibido sin transferencia activa, ignorando\n");
                }
                break;

            case PKT_END:
                printf("\n=== PAQUETE END RECIBIDO ===\n");
                if (transfer_active)
                {
                    if (check_transfer_complete(&global_state))
                    {
                        printf("✓ Transferencia finalizada correctamente\n");
                        printf("✓ Archivo completo: %s\n", global_state.filename);
                        print_transfer_stats(&global_state);
                        
                        if (global_state.file)
                        {
                            fflush(global_state.file);
                            long file_size = ftell(global_state.file);
                            printf("✓ Tamaño final: %ld bytes\n", file_size);
                        }
                    }
                    else
                    {
                        printf("⚠ ADVERTENCIA: Transferencia incompleta\n");
                        print_transfer_stats(&global_state);

                        // Solicitar retransmisión de paquetes faltantes
                        printf("Solicitando retransmisión de paquetes faltantes:\n");
                        int missing_count = 0;
                        for (uint32_t i = 1; i <= global_state.total_packets; i++)
                        {
                            if (!global_state.received_packets[i])
                            {
                                if (missing_count < 10) // Limitar salida para archivos grandes
                                {
                                    printf("- Falta paquete %u\n", i);
                                }
                                receiver_send_nack(&global_state, i);
                                missing_count++;
                            }
                        }
                        if (missing_count > 10)
                        {
                            printf("- ... y %d paquetes más\n", missing_count - 10);
                        }
                    }

                    // Limpiar transferencia independientemente del resultado
                    cleanup_transfer(&global_state);
                    transfer_active = 0;
                    printf("Esperando nueva transferencia...\n");
                }
                else
                {
                    printf("Paquete END recibido sin transferencia activa\n");
                }
                break;

            default:
                printf("⚠ Tipo de paquete desconocido: %d\n", pkt.type);
                break;
            }
        }
        else if (result == -2)
        {
            printf("⚠ Paquete con checksum inválido ignorado\n");
        }
        else if (result == -1)
        {
            // Error de recepción normal (timeout), no imprimir mensaje
        }

        // Pequeña pausa para evitar saturar la CPU
        usleep(1000);
    }

    printf("\nCerrando receptor...\n");
    receiver_cleanup(&global_state);

    return 0;
}