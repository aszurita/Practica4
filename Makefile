# ============================================================================
# Makefile para el Protocolo UDP Confiable
# ============================================================================
# @author Angelo Zurita
# @date 2024
# @version 1.0
# 
# @description
# Este Makefile compila el sistema completo de comunicación UDP confiable
# que incluye:
# 
# - emisor: Programa cliente que envía archivos usando ventana deslizante
# - receptor: Programa servidor que recibe archivos de múltiples emisores
# - protocolo: Biblioteca compartida con implementación del protocolo
# 
# El sistema implementa transferencia confiable de archivos sobre UDP
# con soporte para múltiples emisores concurrentes.
# ============================================================================

# ============================================================================
# VARIABLES DE COMPILACIÓN
# ============================================================================

# Compilador C
CC = gcc

# Flags de compilación
# -c: Solo compilar, no enlazar
# -I.: Incluir directorio actual para headers
# -Wall: Habilitar todos los warnings
# -Wextra: Habilitar warnings adicionales
CFLAGS = -c -I. -Wall -Wextra

# ============================================================================
# REGLAS PRINCIPALES
# ============================================================================

# Meta principal: compilar emisor y receptor
all: emisor receptor

# ============================================================================
# REGLAS DE COMPILACIÓN
# ============================================================================

# Compilar el emisor (cliente)
# Depende de: emisor.o y protocolo.o
emisor: emisor.o protocolo.o
	$(CC) -o emisor emisor.o protocolo.o 

# Compilar el receptor (servidor)
# Depende de: receptor.o y protocolo.o
receptor: receptor.o protocolo.o
	$(CC) -o receptor receptor.o protocolo.o

# ============================================================================
# REGLAS DE OBJETOS
# ============================================================================

# Compilar emisor.c en objeto
# Depende de: emisor.c y protocolo.h
emisor.o: emisor.c protocolo.h
	$(CC) $(CFLAGS) emisor.c

# Compilar receptor.c en objeto
# Depende de: receptor.c y protocolo.h
receptor.o: receptor.c protocolo.h
	$(CC) $(CFLAGS) receptor.c

# Compilar protocolo.c en objeto
# Depende de: protocolo.c y protocolo.h
protocolo.o: protocolo.c protocolo.h
	$(CC) $(CFLAGS) protocolo.c

# ============================================================================
# REGLAS DE LIMPIEZA
# ============================================================================

# Meta para limpiar archivos generados
# Elimina: ejecutables, objetos y archivos de texto
.PHONY: clean
clean:
	rm -f emisor receptor *.o *.txt
