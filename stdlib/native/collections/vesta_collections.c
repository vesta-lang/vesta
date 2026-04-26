/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file vesta_collections.c
 * @brief Implementacion de las colecciones nativas de VestaVM.
 *
 * Cuatro estructuras de datos de proposito general:
 *
 *   VestaList      -- array dinamico de uint32_t (GcHandle).
 *   VestaArrayList -- array dinamico de uint64_t (raw value o GcHandle).
 *   VestaHashMap   -- tabla hash uint64_t->uint64_t, open addressing lineal.
 *   VestaHashSet   -- conjunto uint64_t, envoltura sobre VestaHashMap.
 *
 * Todas las estructuras viven en memoria host (malloc/realloc/free).
 * Los handles devueltos al bytecode son punteros cast a uint64_t.
 *
 * Hash de clave: FNV-1a de 64 bits para distribucion uniforme sin division.
 * Factor de carga maximo de HashMap/Set: 75% (rehash al superar el umbral).
 * Factor de crecimiento de List/ArrayList: x2 desde la capacidad actual.
 *
 * @def VCOL_COLLECTIONS_DEBUG
 *   Define a 1 para activar mensajes de diagnostico en stderr.
 *   Valor 0 por defecto (produccion).
 */

#ifndef VCOL_COLLECTIONS_DEBUG
#  define VCOL_COLLECTIONS_DEBUG 0
#endif

#include "vesta_collections.h"
#include "../../../include/ffi/vesta_plugin.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if VCOL_COLLECTIONS_DEBUG
#  include <stdio.h>
#  define VCOL_DBG(fmt, ...) fprintf(stderr, "[vcol] " fmt "\n", ##__VA_ARGS__)
#else
#  define VCOL_DBG(fmt, ...)
#endif

/* =========================================================================
 * Estado global del plugin
 * ========================================================================= */

/** @brief Puntero a la API de VestaVM, recibido en vesta_init(). */
static const VestaPluginAPI *g_api = NULL;

/* =========================================================================
 * Utilidades internas
 * ========================================================================= */

/**
 * @brief Devuelve la potencia de 2 mayor o igual a n.
 * @param n Valor de entrada (>=1).
 * @return Potencia de 2 >= n.
 */
static uint64_t next_pow2(uint64_t n) {
    if (n <= 1) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

/**
 * @brief Hash FNV-1a de 64 bits para una clave uint64_t.
 *
 * Mezcla los 8 bytes de la clave con la constante FNV para obtener una
 * distribucion de alta calidad sin usar division ni tablas.
 *
 * @param key Clave a hashear.
 * @return Hash de 64 bits.
 */
static uint64_t fnv1a_u64(uint64_t key) {
    static const uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    static const uint64_t FNV_PRIME  = 0x00000100000001b3ULL;
    uint64_t h = FNV_OFFSET;
    /* iterar sobre los 8 bytes de la clave */
    h ^= (key        ) & 0xFF; h *= FNV_PRIME;
    h ^= (key >>  8  ) & 0xFF; h *= FNV_PRIME;
    h ^= (key >> 16  ) & 0xFF; h *= FNV_PRIME;
    h ^= (key >> 24  ) & 0xFF; h *= FNV_PRIME;
    h ^= (key >> 32  ) & 0xFF; h *= FNV_PRIME;
    h ^= (key >> 40  ) & 0xFF; h *= FNV_PRIME;
    h ^= (key >> 48  ) & 0xFF; h *= FNV_PRIME;
    h ^= (key >> 56  ) & 0xFF; h *= FNV_PRIME;
    return h;
}

/* =========================================================================
 * VestaList -- array dinamico de uint32_t
 * ========================================================================= */

/** @brief Capacidad minima de una VestaList. */
#define VLIST_MIN_CAP 8u

/**
 * @brief Estructura interna de VestaList.
 *
 * Los datos se almacenan como uint32_t para compactar GcHandles (que son
 * indices de 32 bits).  El handle expuesto al bytecode es VestaList* cast.
 */
typedef struct VestaList {
    uint32_t *data;   /**< array de GcHandles (uint32_t cada uno) */
    uint64_t  size;   /**< numero de elementos actuales */
    uint64_t  cap;    /**< capacidad en slots uint32_t */
} VestaList;

/**
 * @brief Crece la lista al doble de capacidad si size == cap.
 * @return 1 si ok, 0 si fallo malloc.
 */
static int vlist_grow(VestaList *l) {
    uint64_t new_cap = l->cap * 2;
    uint32_t *new_data = (uint32_t *)realloc(l->data,
                                              new_cap * sizeof(uint32_t));
    if (!new_data) return 0;
    l->data = new_data;
    l->cap  = new_cap;
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_new(uint64_t initial_capacity) {
    uint64_t cap = initial_capacity < VLIST_MIN_CAP ? VLIST_MIN_CAP
                                                    : initial_capacity;
    VestaList *l = (VestaList *)malloc(sizeof(VestaList));
    if (!l) return VCOL_NULL;
    l->data = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!l->data) { free(l); return VCOL_NULL; }
    l->size = 0;
    l->cap  = cap;
    VCOL_DBG("list_new cap=%llu -> %p", (unsigned long long)cap, (void *)l);
    return (uint64_t)(uintptr_t)l;
}

VESTA_PLUGIN_EXPORT void vcol_list_free(uint64_t handle) {
    VestaList *l = (VestaList *)(uintptr_t)handle;
    if (!l) return;
    free(l->data);
    free(l);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_push(uint64_t handle, uint64_t element) {
    VestaList *l = (VestaList *)(uintptr_t)handle;
    if (!l) return VCOL_NOT_FOUND;
    if (l->size == l->cap && !vlist_grow(l)) return VCOL_NOT_FOUND;
    l->data[l->size++] = (uint32_t)(element & 0xFFFFFFFFu); /* almacenar 32 bits bajos */
    return l->size;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_pop(uint64_t handle) {
    VestaList *l = (VestaList *)(uintptr_t)handle;
    if (!l || l->size == 0) return VCOL_NOT_FOUND;
    return (uint64_t)l->data[--l->size];
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_get(uint64_t handle, uint64_t index) {
    VestaList *l = (VestaList *)(uintptr_t)handle;
    if (!l || index >= l->size) return VCOL_NOT_FOUND;
    return (uint64_t)l->data[index];
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_set(uint64_t handle, uint64_t index,
                                            uint64_t element) {
    VestaList *l = (VestaList *)(uintptr_t)handle;
    if (!l || index >= l->size) return VCOL_NOT_FOUND;
    uint64_t old = (uint64_t)l->data[index];
    l->data[index] = (uint32_t)(element & 0xFFFFFFFFu);
    return old;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_size(uint64_t handle) {
    VestaList *l = (VestaList *)(uintptr_t)handle;
    return l ? l->size : 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_cap(uint64_t handle) {
    VestaList *l = (VestaList *)(uintptr_t)handle;
    return l ? l->cap : 0;
}

VESTA_PLUGIN_EXPORT void vcol_list_clear(uint64_t handle) {
    VestaList *l = (VestaList *)(uintptr_t)handle;
    if (l) l->size = 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_remove_at(uint64_t handle, uint64_t index) {
    VestaList *l = (VestaList *)(uintptr_t)handle;
    if (!l || index >= l->size) return VCOL_NOT_FOUND;
    uint64_t old = (uint64_t)l->data[index];
    /* desplazar elementos a la izquierda */
    memmove(&l->data[index], &l->data[index + 1],
            (l->size - index - 1) * sizeof(uint32_t));
    l->size--;
    return old;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_insert(uint64_t handle, uint64_t index,
                                               uint64_t element) {
    VestaList *l = (VestaList *)(uintptr_t)handle;
    if (!l) return VCOL_NOT_FOUND;
    if (index > l->size) index = l->size; /* clamp al final */
    if (l->size == l->cap && !vlist_grow(l)) return VCOL_NOT_FOUND;
    /* desplazar elementos a la derecha */
    memmove(&l->data[index + 1], &l->data[index],
            (l->size - index) * sizeof(uint32_t));
    l->data[index] = (uint32_t)(element & 0xFFFFFFFFu);
    l->size++;
    return l->size;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_indexof(uint64_t handle, uint64_t element) {
    VestaList *l = (VestaList *)(uintptr_t)handle;
    if (!l) return VCOL_NOT_FOUND;
    uint32_t target = (uint32_t)(element & 0xFFFFFFFFu);
    for (uint64_t i = 0; i < l->size; i++) {
        if (l->data[i] == target) return i;
    }
    return VCOL_NOT_FOUND;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_list_clone(uint64_t handle) {
    VestaList *src = (VestaList *)(uintptr_t)handle;
    if (!src) return VCOL_NULL;
    uint64_t h = vcol_list_new(src->cap);
    if (!h) return VCOL_NULL;
    VestaList *dst = (VestaList *)(uintptr_t)h;
    memcpy(dst->data, src->data, src->size * sizeof(uint32_t));
    dst->size = src->size;
    return h;
}

/* =========================================================================
 * VestaArrayList -- array dinamico de uint64_t
 * ========================================================================= */

/** @brief Capacidad minima de un VestaArrayList. */
#define VALIST_MIN_CAP 8u

/**
 * @brief Estructura interna de VestaArrayList.
 *
 * Almacena uint64_t directamente: permite mezclar raw values y GcHandles
 * de 64 bits sin perdida de precision.
 */
typedef struct VestaArrayList {
    uint64_t *data;  /**< array de uint64_t */
    uint64_t  size;  /**< numero de elementos */
    uint64_t  cap;   /**< capacidad en slots */
} VestaArrayList;

static int valist_grow(VestaArrayList *al) {
    uint64_t new_cap = al->cap * 2;
    uint64_t *new_data = (uint64_t *)realloc(al->data,
                                              new_cap * sizeof(uint64_t));
    if (!new_data) return 0;
    al->data = new_data;
    al->cap  = new_cap;
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_new(uint64_t initial_capacity) {
    uint64_t cap = initial_capacity < VALIST_MIN_CAP ? VALIST_MIN_CAP
                                                     : initial_capacity;
    VestaArrayList *al = (VestaArrayList *)malloc(sizeof(VestaArrayList));
    if (!al) return VCOL_NULL;
    al->data = (uint64_t *)malloc(cap * sizeof(uint64_t));
    if (!al->data) { free(al); return VCOL_NULL; }
    al->size = 0;
    al->cap  = cap;
    return (uint64_t)(uintptr_t)al;
}

VESTA_PLUGIN_EXPORT void vcol_alist_free(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    if (!al) return;
    free(al->data);
    free(al);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_push(uint64_t handle, uint64_t value) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    if (!al) return VCOL_NOT_FOUND;
    if (al->size == al->cap && !valist_grow(al)) return VCOL_NOT_FOUND;
    al->data[al->size++] = value;
    return al->size;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_pop(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    if (!al || al->size == 0) return VCOL_NOT_FOUND;
    return al->data[--al->size];
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_get(uint64_t handle, uint64_t index) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    if (!al || index >= al->size) return VCOL_NOT_FOUND;
    return al->data[index];
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_set(uint64_t handle, uint64_t index,
                                             uint64_t value) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    if (!al || index >= al->size) return VCOL_NOT_FOUND;
    uint64_t old = al->data[index];
    al->data[index] = value;
    return old;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_size(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    return al ? al->size : 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_cap(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    return al ? al->cap : 0;
}

VESTA_PLUGIN_EXPORT void vcol_alist_clear(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    if (al) al->size = 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_remove_at(uint64_t handle, uint64_t index) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    if (!al || index >= al->size) return VCOL_NOT_FOUND;
    uint64_t old = al->data[index];
    memmove(&al->data[index], &al->data[index + 1],
            (al->size - index - 1) * sizeof(uint64_t));
    al->size--;
    return old;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_insert(uint64_t handle, uint64_t index,
                                                uint64_t value) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    if (!al) return VCOL_NOT_FOUND;
    if (index > al->size) index = al->size;
    if (al->size == al->cap && !valist_grow(al)) return VCOL_NOT_FOUND;
    memmove(&al->data[index + 1], &al->data[index],
            (al->size - index) * sizeof(uint64_t));
    al->data[index] = value;
    al->size++;
    return al->size;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_indexof(uint64_t handle, uint64_t value) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    if (!al) return VCOL_NOT_FOUND;
    for (uint64_t i = 0; i < al->size; i++) {
        if (al->data[i] == value) return i;
    }
    return VCOL_NOT_FOUND;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_alist_clone(uint64_t handle) {
    VestaArrayList *src = (VestaArrayList *)(uintptr_t)handle;
    if (!src) return VCOL_NULL;
    uint64_t h = vcol_alist_new(src->cap);
    if (!h) return VCOL_NULL;
    VestaArrayList *dst = (VestaArrayList *)(uintptr_t)h;
    memcpy(dst->data, src->data, src->size * sizeof(uint64_t));
    dst->size = src->size;
    return h;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t ua = *(const uint64_t *)a;
    uint64_t ub = *(const uint64_t *)b;
    return (ua > ub) - (ua < ub); /* sin overflow */
}

VESTA_PLUGIN_EXPORT void vcol_alist_sort(uint64_t handle) {
    VestaArrayList *al = (VestaArrayList *)(uintptr_t)handle;
    if (!al || al->size < 2) return;
    qsort(al->data, (size_t)al->size, sizeof(uint64_t), cmp_u64);
}

/* =========================================================================
 * VestaHashMap -- open addressing, sondeo lineal
 * ========================================================================= */

/** @brief Factor de carga maximo antes de rehash (75% = 3/4). */
#define VMAP_LOAD_NUM  3u
#define VMAP_LOAD_DEN  4u

/** @brief Capacidad minima de un VestaHashMap (potencia de 2). */
#define VMAP_MIN_CAP 16u

/** @brief Valor sentinel para slot vacio. */
#define VMAP_EMPTY_KEY   UINT64_MAX
/** @brief Valor sentinel para slot borrado (tombstone). */
#define VMAP_DELETED_KEY (UINT64_MAX - 1u)

/**
 * @brief Entrada de la tabla hash.
 *
 * Una entrada con key == VMAP_EMPTY_KEY esta libre.
 * Una entrada con key == VMAP_DELETED_KEY fue borrada (tombstone).
 */
typedef struct VMapEntry {
    uint64_t key;   /**< clave; EMPTY o DELETED son sentinels internos */
    uint64_t value; /**< valor asociado */
} VMapEntry;

/**
 * @brief Estructura interna de VestaHashMap.
 */
typedef struct VestaHashMap {
    VMapEntry *entries; /**< tabla de slots (potencia de 2) */
    uint64_t   cap;     /**< numero total de slots */
    uint64_t   size;    /**< numero de pares validos */
    uint64_t   tombst;  /**< numero de tombstones (para umbral de rehash) */
} VestaHashMap;

/**
 * @brief Inserta raw en la tabla sin rehash (usada durante el rehash).
 * @param entries Tabla destino.
 * @param cap     Capacidad de la tabla.
 * @param key     Clave.
 * @param value   Valor.
 */
static void vmap_insert_raw(VMapEntry *entries, uint64_t cap,
                             uint64_t key, uint64_t value) {
    uint64_t mask = cap - 1; /* cap es potencia de 2 */
    uint64_t idx  = fnv1a_u64(key) & mask;
    for (;;) {
        if (entries[idx].key == VMAP_EMPTY_KEY ||
            entries[idx].key == VMAP_DELETED_KEY) {
            entries[idx].key   = key;
            entries[idx].value = value;
            return;
        }
        idx = (idx + 1) & mask; /* sondeo lineal */
    }
}

/**
 * @brief Rehashea la tabla cuando la carga supera el umbral.
 * @return 1 si ok, 0 si fallo malloc.
 */
static int vmap_rehash(VestaHashMap *m) {
    uint64_t new_cap     = m->cap * 2;
    VMapEntry *new_ents  = (VMapEntry *)malloc(new_cap * sizeof(VMapEntry));
    if (!new_ents) return 0;
    /* inicializar slots vacios */
    for (uint64_t i = 0; i < new_cap; i++) {
        new_ents[i].key   = VMAP_EMPTY_KEY;
        new_ents[i].value = 0;
    }
    /* reinsertar solo entradas validas (sin tombstones) */
    for (uint64_t i = 0; i < m->cap; i++) {
        if (m->entries[i].key != VMAP_EMPTY_KEY &&
            m->entries[i].key != VMAP_DELETED_KEY) {
            vmap_insert_raw(new_ents, new_cap,
                            m->entries[i].key, m->entries[i].value);
        }
    }
    free(m->entries);
    m->entries = new_ents;
    m->cap     = new_cap;
    m->tombst  = 0; /* tombstones eliminados tras rehash */
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_new(uint64_t initial_capacity) {
    uint64_t cap = next_pow2(initial_capacity < VMAP_MIN_CAP ? VMAP_MIN_CAP
                                                              : initial_capacity);
    VestaHashMap *m = (VestaHashMap *)malloc(sizeof(VestaHashMap));
    if (!m) return VCOL_NULL;
    m->entries = (VMapEntry *)malloc(cap * sizeof(VMapEntry));
    if (!m->entries) { free(m); return VCOL_NULL; }
    for (uint64_t i = 0; i < cap; i++) {
        m->entries[i].key   = VMAP_EMPTY_KEY;
        m->entries[i].value = 0;
    }
    m->cap    = cap;
    m->size   = 0;
    m->tombst = 0;
    return (uint64_t)(uintptr_t)m;
}

VESTA_PLUGIN_EXPORT void vcol_map_free(uint64_t handle) {
    VestaHashMap *m = (VestaHashMap *)(uintptr_t)handle;
    if (!m) return;
    free(m->entries);
    free(m);
}

VESTA_PLUGIN_EXPORT void vcol_map_put(uint64_t handle, uint64_t key, uint64_t value) {
    VestaHashMap *m = (VestaHashMap *)(uintptr_t)handle;
    if (!m) return;
    /* rehash si (size + tombstones) supera el 75% de capacidad */
    if ((m->size + m->tombst + 1) * VMAP_LOAD_DEN > m->cap * VMAP_LOAD_NUM) {
        vmap_rehash(m); /* ignorar error: insercion fallara silenciosamente */
    }
    uint64_t mask    = m->cap - 1;
    uint64_t idx     = fnv1a_u64(key) & mask;
    uint64_t first_tomb = UINT64_MAX; /* primer tombstone encontrado */
    for (;;) {
        if (m->entries[idx].key == VMAP_EMPTY_KEY) {
            /* slot libre: insertar aqui o en el primer tombstone si lo vimos */
            uint64_t dest = (first_tomb != UINT64_MAX) ? first_tomb : idx;
            if (first_tomb != UINT64_MAX) m->tombst--; /* reutilizar tombstone */
            m->entries[dest].key   = key;
            m->entries[dest].value = value;
            m->size++;
            return;
        }
        if (m->entries[idx].key == VMAP_DELETED_KEY) {
            if (first_tomb == UINT64_MAX) first_tomb = idx; /* anotar primero */
        } else if (m->entries[idx].key == key) {
            m->entries[idx].value = value; /* actualizar valor existente */
            return;
        }
        idx = (idx + 1) & mask;
    }
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_get(uint64_t handle, uint64_t key) {
    VestaHashMap *m = (VestaHashMap *)(uintptr_t)handle;
    if (!m) return 0;
    uint64_t mask = m->cap - 1;
    uint64_t idx  = fnv1a_u64(key) & mask;
    for (;;) {
        if (m->entries[idx].key == VMAP_EMPTY_KEY) return 0; /* no existe */
        if (m->entries[idx].key == key) return m->entries[idx].value;
        idx = (idx + 1) & mask;
    }
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_get_or(uint64_t handle, uint64_t key,
                                              uint64_t default_val) {
    VestaHashMap *m = (VestaHashMap *)(uintptr_t)handle;
    if (!m) return default_val;
    uint64_t mask = m->cap - 1;
    uint64_t idx  = fnv1a_u64(key) & mask;
    for (;;) {
        if (m->entries[idx].key == VMAP_EMPTY_KEY) return default_val;
        if (m->entries[idx].key == key) return m->entries[idx].value;
        idx = (idx + 1) & mask;
    }
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_contains(uint64_t handle, uint64_t key) {
    VestaHashMap *m = (VestaHashMap *)(uintptr_t)handle;
    if (!m) return 0;
    uint64_t mask = m->cap - 1;
    uint64_t idx  = fnv1a_u64(key) & mask;
    for (;;) {
        if (m->entries[idx].key == VMAP_EMPTY_KEY) return 0;
        if (m->entries[idx].key == key) return 1;
        idx = (idx + 1) & mask;
    }
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_remove(uint64_t handle, uint64_t key) {
    VestaHashMap *m = (VestaHashMap *)(uintptr_t)handle;
    if (!m) return VCOL_NOT_FOUND;
    uint64_t mask = m->cap - 1;
    uint64_t idx  = fnv1a_u64(key) & mask;
    for (;;) {
        if (m->entries[idx].key == VMAP_EMPTY_KEY) return VCOL_NOT_FOUND;
        if (m->entries[idx].key == key) {
            uint64_t old = m->entries[idx].value;
            m->entries[idx].key = VMAP_DELETED_KEY; /* tombstone: no romper cadena */
            m->size--;
            m->tombst++;
            return old;
        }
        idx = (idx + 1) & mask;
    }
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_size(uint64_t handle) {
    VestaHashMap *m = (VestaHashMap *)(uintptr_t)handle;
    return m ? m->size : 0;
}

VESTA_PLUGIN_EXPORT void vcol_map_clear(uint64_t handle) {
    VestaHashMap *m = (VestaHashMap *)(uintptr_t)handle;
    if (!m) return;
    for (uint64_t i = 0; i < m->cap; i++) {
        m->entries[i].key   = VMAP_EMPTY_KEY;
        m->entries[i].value = 0;
    }
    m->size   = 0;
    m->tombst = 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_keys(uint64_t handle) {
    VestaHashMap *m = (VestaHashMap *)(uintptr_t)handle;
    if (!m) return VCOL_NULL;
    uint64_t al = vcol_alist_new(m->size > 0 ? m->size : VALIST_MIN_CAP);
    if (!al) return VCOL_NULL;
    for (uint64_t i = 0; i < m->cap; i++) {
        if (m->entries[i].key != VMAP_EMPTY_KEY &&
            m->entries[i].key != VMAP_DELETED_KEY) {
            vcol_alist_push(al, m->entries[i].key);
        }
    }
    return al;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_map_values(uint64_t handle) {
    VestaHashMap *m = (VestaHashMap *)(uintptr_t)handle;
    if (!m) return VCOL_NULL;
    uint64_t al = vcol_alist_new(m->size > 0 ? m->size : VALIST_MIN_CAP);
    if (!al) return VCOL_NULL;
    for (uint64_t i = 0; i < m->cap; i++) {
        if (m->entries[i].key != VMAP_EMPTY_KEY &&
            m->entries[i].key != VMAP_DELETED_KEY) {
            vcol_alist_push(al, m->entries[i].value);
        }
    }
    return al;
}

/* =========================================================================
 * VestaHashSet -- envoltura sobre VestaHashMap
 * ========================================================================= */

/** @brief Valor internal sentinel para indicar "elemento presente" en el set. */
#define VSET_PRESENT (UINT64_MAX - 2u)

VESTA_PLUGIN_EXPORT uint64_t vcol_set_new(uint64_t initial_capacity) {
    return vcol_map_new(initial_capacity); /* reutilizar HashMap internamente */
}

VESTA_PLUGIN_EXPORT void vcol_set_free(uint64_t handle) {
    vcol_map_free(handle);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_add(uint64_t handle, uint64_t element) {
    if (vcol_map_contains(handle, element)) return 0; /* ya existia */
    vcol_map_put(handle, element, VSET_PRESENT);
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_contains(uint64_t handle, uint64_t element) {
    return vcol_map_contains(handle, element);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_remove(uint64_t handle, uint64_t element) {
    return vcol_map_remove(handle, element) != VCOL_NOT_FOUND ? 1 : 0;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_size(uint64_t handle) {
    return vcol_map_size(handle);
}

VESTA_PLUGIN_EXPORT void vcol_set_clear(uint64_t handle) {
    vcol_map_clear(handle);
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_to_list(uint64_t handle) {
    return vcol_map_keys(handle); /* las claves son los elementos del set */
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_is_subset(uint64_t set_a, uint64_t set_b) {
    VestaHashMap *a = (VestaHashMap *)(uintptr_t)set_a;
    if (!a) return 1; /* conjunto vacio es subconjunto de cualquiera */
    for (uint64_t i = 0; i < a->cap; i++) {
        if (a->entries[i].key == VMAP_EMPTY_KEY ||
            a->entries[i].key == VMAP_DELETED_KEY) continue;
        if (!vcol_map_contains(set_b, a->entries[i].key)) return 0;
    }
    return 1;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_union(uint64_t set_a, uint64_t set_b) {
    VestaHashMap *a = (VestaHashMap *)(uintptr_t)set_a;
    VestaHashMap *b = (VestaHashMap *)(uintptr_t)set_b;
    uint64_t sz = (a ? a->size : 0) + (b ? b->size : 0);
    uint64_t result = vcol_set_new(sz > VMAP_MIN_CAP ? sz : VMAP_MIN_CAP);
    if (!result) return VCOL_NULL;
    /* insertar elementos de a */
    if (a) {
        for (uint64_t i = 0; i < a->cap; i++) {
            if (a->entries[i].key != VMAP_EMPTY_KEY &&
                a->entries[i].key != VMAP_DELETED_KEY) {
                vcol_map_put(result, a->entries[i].key, VSET_PRESENT);
            }
        }
    }
    /* insertar elementos de b (los duplicados se sobrescriben, sin efecto) */
    if (b) {
        for (uint64_t i = 0; i < b->cap; i++) {
            if (b->entries[i].key != VMAP_EMPTY_KEY &&
                b->entries[i].key != VMAP_DELETED_KEY) {
                vcol_map_put(result, b->entries[i].key, VSET_PRESENT);
            }
        }
    }
    return result;
}

VESTA_PLUGIN_EXPORT uint64_t vcol_set_intersect(uint64_t set_a, uint64_t set_b) {
    VestaHashMap *a = (VestaHashMap *)(uintptr_t)set_a;
    uint64_t sz = a ? a->size : 0;
    uint64_t result = vcol_set_new(sz > VMAP_MIN_CAP ? sz : VMAP_MIN_CAP);
    if (!result) return VCOL_NULL;
    if (!a) return result;
    /* anadir solo los elementos de a que tambien estan en b */
    for (uint64_t i = 0; i < a->cap; i++) {
        if (a->entries[i].key == VMAP_EMPTY_KEY ||
            a->entries[i].key == VMAP_DELETED_KEY) continue;
        if (vcol_map_contains(set_b, a->entries[i].key)) {
            vcol_map_put(result, a->entries[i].key, VSET_PRESENT);
        }
    }
    return result;
}

/* =========================================================================
 * Punto de entrada del plugin
 * ========================================================================= */

/**
 * @brief Funcion de inicializacion llamada por VestaVM tras cargar el plugin.
 *
 * @param api Puntero a la tabla de funciones de la API de VestaVM.
 */
VESTA_PLUGIN_EXPORT void vesta_init(const VestaPluginAPI *api) {
    g_api = api;
#if VCOL_COLLECTIONS_DEBUG
    if (api) api->log("[vesta_collections] plugin cargado (debug ON)");
#else
    if (api) api->log("[vesta_collections] plugin cargado");
#endif
}
