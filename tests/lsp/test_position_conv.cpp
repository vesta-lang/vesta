/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file test_position_conv.cpp
 * @brief Tests de la conversion de columnas byte->UTF-16 del LSP.
 *
 * Verifica @c lsp::byte_column_to_utf16 y @c lsp::DocumentStore::line con
 * casos ASCII, UTF-8 multibyte (2 y 3 bytes), caracteres del plano astral
 * (4 bytes -> 2 unidades UTF-16) y limites (columna 0/1, fuera de rango).
 *
 * No usa framework externo: cuenta aciertos/fallos y devuelve 0 si todo
 * pasa (convencion del resto de tests del proyecto: un binario por test).
 */

#include <cstdint>
#include <cstdio>
#include <string>

#include "lsp/document_store.h"

namespace {

int g_pass = 0; ///< Contador de comprobaciones superadas.
int g_fail = 0; ///< Contador de comprobaciones fallidas.

/**
 * @brief Comprueba que @p got == @p want; registra el resultado.
 * @param what Descripcion del caso para el log.
 * @param got  Valor obtenido.
 * @param want Valor esperado.
 */
void check_eq(const char *what, uint32_t got, uint32_t want) {
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FALLO: %s -> obtenido %u, esperado %u\n", what, got,
                    want);
    }
}

} // namespace

int main() {
    using lsp::byte_column_to_utf16;

    // --- Caso 1: ASCII puro ---
    {
        // "hola": columnas 1..4 -> caracteres UTF-16 0..3.
        const std::string s = "hola";
        check_eq("ascii col 1", byte_column_to_utf16(s, 1), 0);
        check_eq("ascii col 2", byte_column_to_utf16(s, 2), 1);
        check_eq("ascii col 4", byte_column_to_utf16(s, 4), 3);
        // Columna 0 se trata como inicio de linea.
        check_eq("ascii col 0", byte_column_to_utf16(s, 0), 0);
        // Columna tras el final (longitud+1) cae al final.
        check_eq("ascii col 5 (fin)", byte_column_to_utf16(s, 5), 4);
        // Columna desbordada se clampa al final.
        check_eq("ascii col 99 (overflow)", byte_column_to_utf16(s, 99), 4);
    }

    // --- Caso 2: UTF-8 de 2 bytes (e con acento, U+00E9) ---
    {
        // "a" + e-acute (2 bytes) + "b".  Bytes: 'a'(1) C3 A9 (2) 'b'(1).
        // Offsets de byte 1-based: 'a'@1, e-acute@2..3, 'b'@4.
        std::string s;
        s += 'a';
        s += static_cast<char>(0xC3);
        s += static_cast<char>(0xA9);
        s += 'b';
        // Columna 1 = antes de 'a' -> caracter 0.
        check_eq("2b col 1", byte_column_to_utf16(s, 1), 0);
        // Columna 2 = antes de e-acute -> caracter 1.
        check_eq("2b col 2", byte_column_to_utf16(s, 2), 1);
        // Columna 4 = antes de 'b' (e-acute = 1 unidad UTF-16) -> caracter 2.
        check_eq("2b col 4", byte_column_to_utf16(s, 4), 2);
        // Columna 5 = fin -> caracter 3.
        check_eq("2b col 5 (fin)", byte_column_to_utf16(s, 5), 3);
    }

    // --- Caso 3: UTF-8 de 3 bytes (CJK, U+4E2D) ---
    {
        // 'x' + CJK (3 bytes E4 B8 AD) + 'y'.
        std::string s;
        s += 'x';
        s += static_cast<char>(0xE4);
        s += static_cast<char>(0xB8);
        s += static_cast<char>(0xAD);
        s += 'y';
        // Columna 2 = antes del CJK -> caracter 1.
        check_eq("3b col 2", byte_column_to_utf16(s, 2), 1);
        // Columna 5 = antes de 'y' (CJK = 1 unidad UTF-16) -> caracter 2.
        check_eq("3b col 5", byte_column_to_utf16(s, 5), 2);
    }

    // --- Caso 4: UTF-8 de 4 bytes (emoji, U+1F600 -> par surrogate) ---
    {
        // 'A' + emoji (4 bytes F0 9F 98 80) + 'B'.
        std::string s;
        s += 'A';
        s += static_cast<char>(0xF0);
        s += static_cast<char>(0x9F);
        s += static_cast<char>(0x98);
        s += static_cast<char>(0x80);
        s += 'B';
        // Columna 2 = antes del emoji -> caracter 1.
        check_eq("4b col 2", byte_column_to_utf16(s, 2), 1);
        // Columna 6 = antes de 'B'.  El emoji ocupa 2 unidades UTF-16, mas
        // la 'A' previa = 3.
        check_eq("4b col 6 (emoji=2 unidades)", byte_column_to_utf16(s, 6), 3);
    }

    // --- Caso 5: DocumentStore::line con CRLF y rangos ---
    {
        lsp::DocumentStore store;
        // Documento con tres lineas usando CRLF.
        store.open("file:///t.vx", "uno\r\ndos\r\ntres");
        // line() debe devolver la linea SIN el CR final.
        check_eq("line 0 len", static_cast<uint32_t>(
                                   store.line("file:///t.vx", 0).size()),
                 3); // "uno"
        check_eq("line 1 len", static_cast<uint32_t>(
                                   store.line("file:///t.vx", 1).size()),
                 3); // "dos"
        check_eq("line 2 len", static_cast<uint32_t>(
                                   store.line("file:///t.vx", 2).size()),
                 4); // "tres"
        // Linea fuera de rango -> vacia.
        check_eq("line 99 (fuera) len",
                 static_cast<uint32_t>(
                     store.line("file:///t.vx", 99).size()),
                 0);
        // Documento inexistente -> vacia.
        check_eq("line de uri inexistente",
                 static_cast<uint32_t>(store.line("file:///nope", 0).size()),
                 0);
    }

    // --- Caso 6: conversion INVERSA posicion LSP -> offset de byte ---
    {
        using lsp::lsp_position_to_byte_offset;
        // Documento de dos lineas: "hola\nmundo".
        const std::string s = "hola\nmundo";
        // Linea 0, caracter 0 -> offset 0 ('h').
        check_eq("inv l0 c0", lsp_position_to_byte_offset(s, 0, 0), 0);
        // Linea 0, caracter 4 -> offset 4 (el '\n').
        check_eq("inv l0 c4", lsp_position_to_byte_offset(s, 0, 4), 4);
        // Linea 1, caracter 0 -> offset 5 ('m').
        check_eq("inv l1 c0", lsp_position_to_byte_offset(s, 1, 0), 5);
        // Linea 1, caracter 3 -> offset 8 ('d').
        check_eq("inv l1 c3", lsp_position_to_byte_offset(s, 1, 3), 8);
        // Linea fuera de rango -> fin del texto.
        check_eq("inv l9 (fuera)",
                 lsp_position_to_byte_offset(s, 9, 0),
                 static_cast<uint32_t>(s.size()));
    }

    // --- Caso 7: inversa con UTF-8 multibyte (e-acute = 2 bytes, 1 unidad) ---
    {
        using lsp::lsp_position_to_byte_offset;
        // "a" + e-acute (C3 A9) + "b" en una sola linea.
        std::string s;
        s += 'a';
        s += static_cast<char>(0xC3);
        s += static_cast<char>(0xA9);
        s += 'b';
        // caracter 0 -> offset 0 ('a').
        check_eq("inv 2b c0", lsp_position_to_byte_offset(s, 0, 0), 0);
        // caracter 1 -> offset 1 (inicio de e-acute).
        check_eq("inv 2b c1", lsp_position_to_byte_offset(s, 0, 1), 1);
        // caracter 2 -> offset 3 ('b'): e-acute ocupa 2 bytes pero 1 unidad.
        check_eq("inv 2b c2", lsp_position_to_byte_offset(s, 0, 2), 3);
    }

    // --- Caso 8: inversa con emoji (4 bytes, 2 unidades UTF-16) ---
    {
        using lsp::lsp_position_to_byte_offset;
        // 'A' + emoji (F0 9F 98 80) + 'B'.
        std::string s;
        s += 'A';
        s += static_cast<char>(0xF0);
        s += static_cast<char>(0x9F);
        s += static_cast<char>(0x98);
        s += static_cast<char>(0x80);
        s += 'B';
        // caracter 1 -> offset 1 (inicio del emoji).
        check_eq("inv emoji c1", lsp_position_to_byte_offset(s, 0, 1), 1);
        // caracter 3 -> offset 5 ('B'): emoji = 2 unidades (1 + 2 = 3).
        check_eq("inv emoji c3", lsp_position_to_byte_offset(s, 0, 3), 5);
    }

    // --- Caso 9: round-trip offset -> posicion -> offset ---
    {
        using lsp::byte_offset_to_lsp_position;
        using lsp::lsp_position_to_byte_offset;
        // Mezcla de lineas y multibyte.
        std::string s = "uno\ndos";
        s += static_cast<char>(0xC3); // empezamos otra linea con e-acute.
        // Recorrer cada offset de inicio de code point y verificar el ciclo.
        // Para los offsets ASCII el round-trip debe ser identidad.
        for (uint32_t off : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
            uint32_t l = 0, c = 0;
            byte_offset_to_lsp_position(s, off, l, c);
            uint32_t back = lsp_position_to_byte_offset(s, l, c);
            check_eq("round-trip ascii", back, off);
        }
    }

    // --- Resumen ---
    std::printf("test_position_conv: %d OK, %d FALLOS\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
