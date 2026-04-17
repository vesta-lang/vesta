//
// Created by desmon0xff on 15/04/2026.
//

#ifndef JIT_CALL_H
#define JIT_CALL_H

typedef enum err_shellcode {
    NO_ERROR_SC,
    REALLOC_ERROR,
    EmitIndirect_ERROR,
    EmitIndirectByteDisplaced_ERROR,
    EmitIndirectDisplaced_ERROR,
    EmitIndirectIndexed_ERROR,
    EmitIndirectIndexedDisplaced_ERROR,
    EmitIndirectIndexedByteDisplaced_ERROR
} err_shellcode;

typedef struct shellcode_t {
    size_t capacity = 0;
    size_t size     = 0;

    uint8_t *     code = nullptr;
    err_shellcode err  = NO_ERROR_SC;

    void (*Emit8)(struct shellcode_t *code, uint8_t byte);

    void (*Emit32)(struct shellcode_t *code, uint32_t bytes);

    void (*Emit64)(struct shellcode_t *code, uint64_t bytes);

    void (*expand)(struct shellcode_t *code);

    void (*free)(struct shellcode_t *code);

    void (*dump)(const struct shellcode_t *shell);
} shellcode_t;

/**
 * Estructura para llamar a funciones nativas/externas a la VM.
 */
typedef struct PendingCall_t {
    void * (*    func)(void *arg) = nullptr; /** funcion nativa que llamar  */
    shellcode_t *arg              = nullptr; /** argumentos para la funcion, formando un shellcode */
    void *       result           = nullptr; /** valor de retorno */
    bool         finished         = false;   /** indica si la funcion fue ejecutada*/

    pthread_mutex_t lock; /** Proteccion de acceso para finished/result */
} PendingCall_t;


#endif //JIT_CALL_H
