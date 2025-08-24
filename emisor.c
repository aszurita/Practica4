/**
 * @file emisor.c
 * @brief Implementación del emisor del protocolo UDP confiable
 * @author Angelo Zurita
 * @date 2024
 * @version 1.0
 *
 * @description
 * Este archivo implementa el emisor del protocolo de comunicación UDP confiable.
 * El emisor es responsable de:
 *
 * - Establecer la sesión de transmisión con el receptor
 * - Leer y enviar archivos usando ventana deslizante
 * - Manejar acknowledgments (ACKs) y negative acknowledgments (NACKs)
 * - Implementar retransmisión automática en caso de timeouts
 * - Monitorear el progreso de la transmisión
 * - Limpiar recursos al finalizar
 *
 * El emisor utiliza múltiples hilos para manejo concurrente de envío
 * y recepción de confirmaciones, implementando un protocolo similar
 * a TCP pero optimizado para transferencias de archivos.
 */

#include "protocolo.h"
#include <signal.h>

// ============================================================================
// VARIABLES GLOBALES Y ESTADO
// ============================================================================

/** @brief Estado global del emisor compartido entre hilos */
static sender_state_t global_state;

/** @brief Flag de ejecución para controlar hilos */
static volatile int running = 1;

// ============================================================================
// MANEJADORES DE SEÑALES
// ============================================================================

/**
 * @brief Manejador de señales para terminación limpia del programa
 *
 * @param sig Número de señal recibida (no utilizado)
 *
 * @details
 * Esta función se ejecuta cuando el usuario presiona Ctrl+C (SIGINT)
 * o cuando se envía SIGTERM al proceso. Realiza:
 * 1. Cambia el flag de ejecución para detener hilos
 * 2. Limpia recursos del emisor
 * 3. Termina el programa de forma ordenada
 *
 * @note
 * Es importante manejar señales para evitar que el programa
 * termine abruptamente y deje recursos sin liberar.
 */
void signal_handler(int sig)
{
    (void)sig; // Suprimir warning de parámetro no utilizado
    printf("\nInterrumpido por usuario, limpiando...\n");
    running = 0;
    sender_cleanup(&global_state);
    exit(0);
}

// ============================================================================
// MANEJADOR DE TIMEOUTS
// ============================================================================

/**
 * @brief Hilo manejador de timeouts y retransmisiones
 *
 * @param arg Puntero al estado del emisor (cast a void*)
 * @return void* Siempre NULL
 *
 * @details
 * Este hilo ejecuta el bucle principal de envío y recepción:
 * 1. Envía paquetes usando la ventana deslizante
 * 2. Recibe y procesa ACKs/NACKs del receptor
 * 3. Maneja timeouts y retransmisiones automáticamente
 * 4. Muestra progreso de la transmisión
 * 5. Continúa hasta completar todos los paquetes
 *
 * El hilo implementa un timeout más corto para ACKs (50ms) para
 * mejorar la responsividad del protocolo.
 *
 * @note
 * Este hilo es crítico para el funcionamiento del emisor y debe
 * ejecutarse continuamente hasta completar la transmisión.
 */
void *timeout_handler(void *arg)
{
    (void)arg; // Suprimir warning de parámetro no utilizado

    // Bucle principal de envío y recepción
    while (running && global_state.base_seq <= global_state.total_packets)
    {
        // Enviar ventana de paquetes
        sender_send_window(&global_state);

        // Recibir y procesar ACKs con timeout más corto
        packet_t ack;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        // Configurar timeout más corto para ACKs (50ms)
        struct timeval old_timeout, new_timeout;
        socklen_t optlen = sizeof(old_timeout);
        getsockopt(global_state.sockfd, SOL_SOCKET, SO_RCVTIMEO, &old_timeout, &optlen);

        new_timeout.tv_sec = 0;
        new_timeout.tv_usec = 50000; // 50ms
        setsockopt(global_state.sockfd, SOL_SOCKET, SO_RCVTIMEO, &new_timeout, sizeof(new_timeout));

        // Intentar recibir paquete
        int result = receive_packet(global_state.sockfd, &ack, &from_addr, &from_len);

        // Restaurar timeout original
        setsockopt(global_state.sockfd, SOL_SOCKET, SO_RCVTIMEO, &old_timeout, sizeof(old_timeout));

        // Procesar paquete recibido
        if (result == 0)
        {
            print_packet_info(&ack, "RECIBIDO");

            // Manejar diferentes tipos de paquetes
            switch (ack.type)
            {
            case PKT_ACK:
                sender_handle_ack(&global_state, &ack);
                break;
            case PKT_NACK:
                sender_handle_nack(&global_state, &ack);
                break;
            default:
                printf("Paquete inesperado: tipo=%d\n", ack.type);
                break;
            }
        }

        // Mostrar progreso más frecuentemente
        static uint32_t last_progress = 0;
        if (global_state.base_seq != last_progress)
        {
            float progress = ((float)(global_state.base_seq - 1) / global_state.total_packets) * 100;
            printf("Progreso: %.1f%% (%u/%u paquetes confirmados)\n",
                   progress, global_state.base_seq - 1, global_state.total_packets);
            last_progress = global_state.base_seq;
        }

        // Pausa muy pequeña para evitar saturar la CPU
        usleep(1000); // 1ms
    }

    return NULL;
}

// ============================================================================
// FUNCIONES DE UTILIDAD
// ============================================================================

/**
 * @brief Muestra información de uso del programa
 *
 * @param program_name Nombre del programa ejecutable
 *
 * @details
 * Función de ayuda que muestra:
 * 1. Sintaxis correcta de la línea de comandos
 * 2. Descripción de cada parámetro
 * 3. Ejemplo de uso concreto
 *
 * Se llama cuando el usuario proporciona argumentos incorrectos.
 */
void print_usage(const char *program_name)
{
    printf("Uso: %s <host_destino> <puerto_destino> <nombre_archivo>\n", program_name);
    printf("\nEjemplo:\n");
    printf("  %s 127.0.0.1 8080 archivo_grande.txt\n", program_name);
}

// ============================================================================
// FUNCIÓN PRINCIPAL
// ============================================================================

/**
 * @brief Función principal del emisor
 *
 * @param argc Número de argumentos de línea de comandos
 * @param argv Array de argumentos de línea de comandos
 * @return int 0 en éxito, 1 en error
 *
 * @details
 * La función principal realiza:
 * 1. Validación de argumentos de línea de comandos
 * 2. Verificación de existencia y permisos del archivo
 * 3. Configuración de manejadores de señales
 * 4. Inicialización del estado del emisor
 * 5. Establecimiento de la sesión de transmisión
 * 6. Creación del hilo manejador de timeouts
 * 7. Ejecución del bucle principal de envío
 * 8. Envío del paquete de finalización
 * 9. Limpieza de recursos y terminación
 *
 * @note
 * El programa debe recibir exactamente 3 argumentos:
 * - host_destino: IP del receptor
 * - puerto_destino: Puerto del receptor
 * - nombre_archivo: Archivo a transmitir
 */
int main(int argc, char *argv[])
{
    // Validar número de argumentos
    if (argc != 4)
    {
        fprintf(stderr, "Error: Se requieren exactamente 3 argumentos\n");
        print_usage(argv[0]);
        return 1;
    }

    // Extraer argumentos de línea de comandos
    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *filename = argv[3];

    // Validar puerto
    if (port <= 0 || port > 65535)
    {
        fprintf(stderr, "Error: Puerto inválido %d\n", port);
        return 1;
    }

    // Verificar que el archivo existe y es legible
    if (access(filename, R_OK) != 0)
    {
        fprintf(stderr, "Error: No se puede leer el archivo %s\n", filename);
        return 1;
    }

    // Configurar manejadores de señales para terminación limpia
    signal(SIGINT, signal_handler);  // Ctrl+C
    signal(SIGTERM, signal_handler); // Señal de terminación

    // Mostrar información de inicio
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

    // Iniciar transmisión enviando paquete START
    if (sender_start_transmission(&global_state) < 0)
    {
        fprintf(stderr, "Error iniciando transmisión\n");
        sender_cleanup(&global_state);
        return 1;
    }

    // Crear hilo para manejo de timeouts y retransmisiones
    pthread_t timeout_thread;
    pthread_create(&timeout_thread, NULL, timeout_handler, &global_state);

    printf("\nIniciando envío de datos...\n");

    // Bucle principal de envío
    while (running && global_state.base_seq <= global_state.total_packets)
    {
        // Enviar ventana de paquetes
        sender_send_window(&global_state);

        // Recibir y procesar ACKs/NACKs
        packet_t ack;
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        int result = receive_packet(global_state.sockfd, &ack, &from_addr, &from_len);
        if (result == 0)
        {
            print_packet_info(&ack, "RECIBIDO");

            // Manejar diferentes tipos de paquetes
            switch (ack.type)
            {
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

        // Mostrar progreso cada 50 paquetes confirmados
        if ((global_state.base_seq - 1) % 50 == 0)
        {
            float progress = ((float)(global_state.base_seq - 1) / global_state.total_packets) * 100;
            printf("Progreso: %.1f%% (%u/%u paquetes confirmados)\n",
                   progress, global_state.base_seq - 1, global_state.total_packets);
        }

        // Pausa adaptativa según el estado de la transmisión
        if (global_state.next_seq > global_state.total_packets &&
            global_state.base_seq <= global_state.total_packets)
        {
            // Esperar ACKs finales
            usleep(10000); // 10ms
        }
        else
        {
            // Pausa mínima durante envío activo
            usleep(1000); // 1ms
        }
    }

    // Enviar paquete END para finalizar la transmisión
    if (running)
    {
        // Esperar a que todos los paquetes sean confirmados
        if (sender_wait_for_completion(&global_state) == 0)
        {
            // Crear y enviar paquete de finalización
            packet_t end_pkt;
            create_packet(&end_pkt, PKT_END, 0, NULL, 0);
            end_pkt.checksum = calculate_checksum(&end_pkt);

            printf("\nEnviando paquete de finalización...\n");

            // Enviar END múltiples veces para asegurar recepción
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

    // Mostrar resumen final
    printf("\n=== TRANSMISIÓN COMPLETADA ===\n");
    printf("Total de paquetes enviados: %u\n", global_state.total_packets);
    printf("Archivo enviado exitosamente\n");

    // Limpiar recursos y terminar
    sender_cleanup(&global_state);
    return 0;
}