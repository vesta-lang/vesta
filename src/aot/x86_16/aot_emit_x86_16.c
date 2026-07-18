/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file aot/x86_16/aot_emit_x86_16.c
 * @brief Emisores de objetos AOT para x86-16 (real mode / 8086) -- reservado.
 *
 * Parte del desacople multi-arch (aot/common + aot/x86_64 + aot/x86_32 +
 * aot/x86_16 + aot/arm64).  Hoy el codigo de 16 bits (boot sectors BIOS, stubs
 * de real -> protegido -> largo) se emite como BINARIO PLANO via
 * @c aot_emit_flat_bin (en aot/common, arch-neutral): el ensamblado del cuerpo
 * lo hace Keystone en modo @c KS_MODE_16 desde los bloques @c @bits(16) asm,
 * y el layout/relleno/firma los da el toolchain de datos crudos (bytes/db/times
 * + @c $/@c $$ + @c @at/@c @order) del driver.
 *
 * Este fichero queda como punto de extension para un futuro emisor NATIVO de 16
 * bits (p.ej. un formato de imagen de firmware propio, o structs Elf16/segmentos
 * reales) si algun dia se necesita algo que el flat binary no cubra.  De momento
 * es una unidad de traduccion vacia (sin simbolos) que solo materializa el
 * subdirectorio del desacople.
 */

#include "aot/aot_emit_shim.h"

#include "../common/aot_emit_internal.h"

/* Sin emisores nativos de 16 bits todavia: ver el @brief.  TU intencionadamente
 * vacia (el enlazador no anade nada). */
