/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */
#include "runtime/exec_instruction.h"

namespace runtime {
    void exec_instr_hlt(VM *vm, const DecodedInstr &instr) {
        // indicamos que queremos matar la VM, esto hara que la fase
        // EXECUTE emita un evento de tipo EVT_ERROR
        vm->should_kill = true;
    }
}
