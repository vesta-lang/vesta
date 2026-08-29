/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/builtins_optional.cpp
 * @brief Bajada de lo que puede NO estar: `Optional<T>` y `Result<T, E>`.
 *
 * Un `Optional` no es un puntero que a veces vale nulo y un `Result` no es un
 * codigo de error devuelto por convenio: los dos son un valor que lleva
 * consigo si esta o no esta, y el lenguaje no deja mirar dentro sin haber
 * preguntado antes.  Eso es todo lo que hacen estos diez builtins --
 * construirlos (`Some`, `None`, `Ok`, `Err`), preguntar (`isPresent`, `isOk`)
 * y sacar lo de dentro (`value`, `error`, `unwrap`) --.
 *
 * Lo importante de como bajan: NO tocan el monton.  Un Optional vive en la
 * pila como un par {hay, valor} y se devuelve por la direccion que da el
 * llamante, asi que envolver un valor no cuesta una reserva de memoria.  Por
 * eso construirlos y consultarlos esta aqui y no en una libreria: son forma
 * del codigo generado, no llamadas.
 *
 * Y hay dos maneras de sacar el valor, a proposito: `unwrap` comprueba y lanza
 * si no estaba, y `unwrap_unchecked` no comprueba nada -- baja a leer el
 * campo, sin mas --.  El segundo existe para quien YA comprobo y no quiere
 * pagarlo dos veces; si se equivoca, lee basura.  Ninguno de los dos es el
 * predeterminado del otro.
 *
 * Se separo de la funcion que despacha todos los builtins.  Entra por su
 * propio punto: si el nombre no es de esta familia, contesta que no y quien
 * pregunta sigue con las demas.
 */
#include "vx/lowering.h"
#include "ir/ir_type_info.h" // vocabulario UNICO de anchura/clase de un IrType
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include "lowering_internal.h" // la cocina compartida del lowering (no es su interfaz)

namespace vx {

/**
 * @brief Intenta bajar @p e como uno de los builtins de Optional / Result.
 *
 * @param e         La llamada.
 * @param b         Que builtin es, ya resuelto por quien despacha.
 * @param out_value Donde dejar el resultado; sin valor si el builtin no lo da.
 * @return @c true si @p b era de esta familia y quedo bajado.
 */
bool Lowering::try_lower_optional_builtins(ast::CallExpr *e, Builtin b,
                                           ir::IrValueId &out_value) {
    // Optional sobre referencias, que baja a las instrucciones isnull/unwrap
    // de la maquina en vez de al par {hay, valor}.
    const bool is_isPresent = (b == Builtin::IsPresent);
    const bool is_unwrap = (b == Builtin::Unwrap);
    // unwrap_unchecked: renunciar a la comprobacion en UN sitio.  Misma forma
    // que unwrap pero sin el chequeo (baja a leer el payload directo); si el
    // valor no estaba, lee basura.
    const bool is_unwrap_unchecked = (b == Builtin::UnwrapUnchecked);
    // Construir y consultar los que viven en la PILA.
    const bool is_Some = (b == Builtin::Some);
    const bool is_None = (b == Builtin::None);
    const bool is_Ok = (b == Builtin::Ok);
    const bool is_Err = (b == Builtin::Err);
    const bool is_isOk = (b == Builtin::IsOk);
    const bool is_value = (b == Builtin::Value);
    const bool is_error = (b == Builtin::Error);

    /* Salida rapida: si no es de esta familia no se mira nada de lo de abajo. */
    if (!(is_isPresent || is_unwrap || is_unwrap_unchecked || is_Some ||
          is_None || is_Ok || is_Err || is_isOk || is_value || is_error))
        return false;

    if (is_Some) {
        if (e->args.size() != 1) {
            return builtin_error(e->loc, "Some: requiere 1 argumento", out_value);
        }
        const ir::IrValueId v_payload = lower_expr(e->args[0].get());
        if (v_payload == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // Un payload STRUCT viaja por DIRECCION, no por valor: hay que copiar
        // sus bytes al buffer y dimensionarlo para que quepan.  Guardando el
        // puntero (lo que se hacia antes) el `Some` sobrevivia al ALLOCA que
        // apuntaba y `unwrap` leia memoria muerta -> devolvia 0 en silencio.
        const Type &arg_t = e->args[0]->result_type;
        /* Un `@overlay` NO es un struct por valor: no reserva nada, es una
         * VISTA TIPADA sobre un puntero, y su valor ES ese puntero (8 bytes).
         * Copiar "sus bytes" copiaba los primeros 8 bytes del OBJETO VISTO, con
         * lo que el `Some` guardaba `*base` en vez de `base` y el consumidor
         * leia campos en direcciones inventadas.  Se guarda el handle, como
         * cualquier otro valor de 8 bytes; el objeto sigue viviendo donde
         * vivia. */
        const bool payload_is_struct = (arg_t.kind == PrimitiveKind::STRUCT) &&
                                       !arg_t.struct_name.empty() &&
                                       !type_is_overlay(arg_t);
        const size_t payload_sz = payload_is_struct ? size_of_type(arg_t) : 8u;
        /* Cuanto ocupa y donde va cada cosa lo decide `optional_layout`, no
         * este sitio: aqui habia una sexta copia del calculo del tamano, y la
         * marca y el valor iban clavados en el cero y el ocho.  Con eso, hacer
         * que un `Optional` ocupe menos habria que escribirlo tambien aqui. */
        const OptionalLayout lay = optional_layout(e->result_type);
        const ir::IrValueId v_buf =
            stack_alloc_buf(lay.bytes, e->loc.line, /*host_memory=*/true);
        // La marca, si la disposicion dice que va aparte.
        if (lay.has_tag) {
            const ir::IrValueId v_one =
                emit_const(ir::IrType::I64, 1, e->loc.line);
            emit_store_typed(v_buf, v_one, ir::IrType::I64, e->loc.line);
        }
        // Y el valor, donde la disposicion diga.
        const ir::IrValueId v_off = emit_const(
            ir::IrType::I64, static_cast<uint64_t>(lay.value_offset),
            e->loc.line);
        const ir::IrValueId v_buf8 = emit_ptr_add(v_buf, v_off, e->loc.line);
        // BugFix sret-cross-mem: propagar is_host_ptr del buffer al puntero
        // buf+8.  Sin esto, el STORE del payload usa acceso VM (`mov`) sobre un
        // buffer HOST (stack_alloc_buf con host_alloca) -> el valor se escribe
        // a memoria VM en una direccion host (fuera de rango) y se pierde
        // (unwrap devuelve 0).  Funcionaba por SUERTE cuando la direccion del
        // alloc caia en rango VM.  Ok/Err ya tenian esta propagacion; Some no.
        fn_->values[v_buf8].is_host_ptr = fn_->values[v_buf].is_host_ptr;
        if (payload_is_struct) {
            // Copia de los bytes del struct al payload.  `v_payload` es la
            // direccion del agregado (asi viajan los structs por valor aqui).
            const ir::IrValueId v_len =
                emit_const(ir::IrType::I64, static_cast<uint64_t>(payload_sz),
                           e->loc.line);
            ir::IrInstr mc{};
            mc.op = ir::IrOp::MEMCPY;
            mc.type = ir::IrType::I8;
            mc.dst = ir::IR_NO_VALUE;
            mc.operands = {v_buf8, v_payload, v_len};
            mc.source_line = e->loc.line;
            emit(current_block_, std::move(mc));
        } else {
            const ir::IrType payload_t = fn_->values[v_payload].type;
            emit_store_typed(v_buf8, v_payload, payload_t, e->loc.line);
        }
        out_value = v_buf;
        return true;
    }
    // ----- None() -----  Optional vacio (flag=0) en stack.
    if (is_None) {
        /* El buffer se dimensiona por el Optional QUE SE ESPERA, no a 16 fijo.
         *
         * Un `Optional<u128>` mide 24 (tag + los 16 del struct), y quien
         * devuelve copia al retbuf tantos qwords como diga el TIPO DE RETORNO:
         * con 16 reservados, esa copia leia el tercer qword fuera del buffer --
         * memoria de pila sin inicializar.  No se notaba porque en un `None`
         * nadie mira el payload, pero el acceso era real: lo señalo el
         * comprobador de limites de `--analyze` (`region [0,16) ; acceso
         * [16,24)`) en `checked_div` y en 17 programas mas del corpus.
         *
         * Con el tipo a mano se usa el mismo calculo que el resto del camino
         * (`optional_buf_bytes`); si el contexto no lo da, se queda en 16 como
         * antes. */
        size_t none_sz = 16;
        if (e->result_type.kind == PrimitiveKind::OPTIONAL)
            none_sz = optional_buf_bytes(e->result_type);
        else if (sret_active_ && sret_buf_size_ >= 16)
            none_sz = static_cast<size_t>(sret_buf_size_);
        const ir::IrValueId v_buf =
            stack_alloc_buf(none_sz, e->loc.line, /*host_memory=*/true);
        const ir::IrValueId v_zero =
            emit_const(ir::IrType::I64, 0, e->loc.line);
        emit_store_typed(v_buf, v_zero, ir::IrType::I64, e->loc.line);
        out_value = v_buf;
        return true;
    }
    // ----- Ok(v) ----- / ----- Err(e) -----  Result<V,E> en heap.
    //   Layout: [+0 i64 tag (1=ok, 0=err)][+8 V][+16 E]. 24 bytes.
    if (is_Ok || is_Err) {
        if (e->args.size() != 1) {
            return builtin_error(e->loc, (is_Ok ? "Ok" : "Err") +
                                             std::string(": requiere 1 argumento"), out_value);
        }
        // Bug fix 2026-05-23: si el payload esperado del Result es STRING
        // y el arg es StringLitExpr no interpolado, promover a StringObject
        // ANTES del STORE.  Sin esto, el handle guardado seria el ptr
        // raw del literal -> garbage en isOk/value/error.  El expected_type
        // viene de e->result_type que ya fue calculado en check_call.
        ir::IrValueId v_payload;
        bool need_str_promo = false;
        if (e->args[0] && e->args[0]->kind == ast::NodeKind::StringLitExpr &&
            e->result_type.kind == PrimitiveKind::RESULT) {
            const Type *target = is_Ok ? e->result_type.pointee.get()
                                       : e->result_type.pointee2.get();
            if (target && target->kind == PrimitiveKind::STRING) {
                need_str_promo = true;
            }
        }
        if (need_str_promo) {
            auto *slit = static_cast<ast::StringLitExpr *>(e->args[0].get());
            v_payload = lower_string_literal_to_string_object(slit);
        } else {
            v_payload = lower_expr(e->args[0].get());
        }
        if (v_payload == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_buf =
            stack_alloc_buf(24, e->loc.line, /*host_memory=*/true);
        // Tag.
        const ir::IrValueId v_tag =
            emit_const(ir::IrType::I64, is_Ok ? 1 : 0, e->loc.line);
        emit_store_typed(v_buf, v_tag, ir::IrType::I64, e->loc.line);
        // Payload offset: V en +8 (Ok), E en +16 (Err).
        const uint64_t off = is_Ok ? 8 : 16;
        const ir::IrValueId v_off =
            emit_const(ir::IrType::I64, off, e->loc.line);
        const ir::IrValueId v_at = emit_ptr_add(v_buf, v_off, e->loc.line);
        // BugFix sret-cross-mem (2026-06-04): propagar is_host_ptr.
        fn_->values[v_at].is_host_ptr = fn_->values[v_buf].is_host_ptr;
        const ir::IrType payload_t = fn_->values[v_payload].type;
        emit_store_typed(v_at, v_payload, payload_t, e->loc.line);
        out_value = v_buf;
        return true;
    }
    // ----- isOk(r) -----  LOAD i64 at +0; returns 1/0 as i32.
    if (is_isOk) {
        if (e->args.size() != 1) {
            return builtin_error(e->loc, "isOk: requiere 1 argumento", out_value);
        }
        const ir::IrValueId v_buf = lower_expr(e->args[0].get());
        if (v_buf == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const ir::IrValueId v_dst =
            emit_load_typed(v_buf, ir::IrType::I32, e->loc.line);
        out_value = v_dst;
        return true;
    }
    // ----- value(r) -----  LOAD V from r+8 (sin tag check en MVP).
    // ----- error(r) -----  LOAD E from r+16 (sin tag check en MVP).
    if (is_value || is_error) {
        if (e->args.size() != 1) {
            return builtin_error(e->loc, "value/error: requiere 1 argumento", out_value);
        }
        const ir::IrValueId v_buf = lower_expr(e->args[0].get());
        if (v_buf == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        const Type at = e->args[0]->result_type;
        Type payload_st =
            (is_value
                 ? (at.pointee ? *at.pointee : Type{PrimitiveKind::I64})
                 : (at.pointee2 ? *at.pointee2 : Type{PrimitiveKind::I64}));
        const ir::IrType payload_t = ir_type_from_primitive(payload_st.kind);
        const uint64_t off = is_value ? 8 : 16;
        const ir::IrValueId v_off =
            emit_const(ir::IrType::I64, off, e->loc.line);
        const ir::IrValueId v_at = emit_ptr_add(v_buf, v_off, e->loc.line);
        // BugFix sret-cross-mem (2026-06-04): propagar is_host_ptr de
        // v_buf al v_at para que el LOAD downstream emita `movh`/`loadzh`.
        fn_->values[v_at].is_host_ptr = fn_->values[v_buf].is_host_ptr;
        const ir::IrValueId v_dst =
            emit_load_typed(v_at, payload_t, e->loc.line);
        out_value = v_dst;
        return true;
    }

    // ----- isPresent(x) -----
    // Para Optional<T> builtin: LOAD i64 al offset 0 del buffer.
    // Para referencias (CLASS/PTR) legacy: usa la instruccion VM
    // @c isnull (0x25) invertida con XOR.
    if (is_isPresent) {
        if (e->args.size() != 1) {
            return builtin_error(e->loc, "isPresent: requiere exactamente 1 argumento", out_value);
        }
        const ir::IrValueId v_arg = lower_expr(e->args[0].get());
        if (v_arg == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        /* Optional<T>: la respuesta depende de como este puesto en memoria.
         * Con marca aparte se lee la marca; sin ella, la lleva el propio valor
         * -- un puntero presente no puede ser nulo -- y la pregunta es la misma
         * que para un puntero crudo, que ya esta resuelta unas lineas mas
         * abajo.  Preguntando aqui, el dia que la disposicion cambie este sitio
         * no hay que tocarlo. */
        if (e->args[0]->result_type.kind == PrimitiveKind::OPTIONAL) {
            const OptionalLayout lay = optional_layout(e->args[0]->result_type);
            if (lay.has_tag) {
                out_value =
                    emit_load_typed(v_arg, ir::IrType::I32, e->loc.line);
                return true;
            }
            const ir::IrValueId v_val =
                emit_load_typed(v_arg, ir::IrType::I64, e->loc.line);
            const ir::IrValueId v_nulo = emit_ir_unop(
                ir::IrOp::ISNULL, v_val, ir::IrType::I32, e->loc.line);
            const ir::IrValueId v_uno =
                emit_const(ir::IrType::I32, 1, e->loc.line);
            out_value = emit_ir_binop(ir::IrOp::XOR, v_nulo, v_uno,
                                      ir::IrType::I32, e->loc.line);
            return true;
        }
        // raw_asm-elim 2026-05-28: isPresent(p) = (p != null) implementado
        // como secuencia de 2 IR ops:
        //   v_is_null = ISNULL(v_arg)   -> i32 (0 = not null, 1 = null)
        //   v_dst     = XOR(v_is_null, 1) -> i32 (invierte el bit 0)
        // Esto reemplaza el RAW_ASM original que hacia `isnull + mov r14,1 +
        // xor`. Beneficios: DCE puede eliminar la cadena si v_dst no se usa, el
        // Selector JIT trata cada paso natively, y CSE puede fundir
        // multiples isPresent del mismo arg.  Mismo bytecode emitido.
        const ir::IrValueId v_is_null =
            emit_ir_unop(ir::IrOp::ISNULL, v_arg, ir::IrType::I32, e->loc.line);
        const ir::IrValueId v_one = emit_const(ir::IrType::I32, 1, e->loc.line);
        const ir::IrValueId v_dst =
            emit_ir_binop(ir::IrOp::XOR, v_is_null, v_one,
                          ir::IrType::I32, e->loc.line);
        out_value = v_dst;
        return true;
    }

    // ----- unwrap(x) -----
    // Para Optional<T> builtin: LOAD flag at +0; pasa por VM
    // `unwrap` (genera NPE si flag==0); luego LOAD payload at +8.
    // Para referencias (CLASS/PTR) legacy: VM `unwrap` directo sobre
    // el puntero (0 = null).
    if (is_unwrap || is_unwrap_unchecked) {
        const char *bn = is_unwrap_unchecked ? "unwrap_unchecked" : "unwrap";
        if (e->args.size() != 1) {
            return builtin_error(e->loc,
                                 std::string(bn) + ": requiere exactamente 1 argumento", out_value);
        }
        const ir::IrValueId v_arg = lower_expr(e->args[0].get());
        if (v_arg == ir::IR_NO_VALUE) {
            out_value = ir::IR_NO_VALUE;
            return true;
        }
        // Optional<T>: LOAD flag, unwrap (lanza si 0), LOAD payload.
        // unwrap_unchecked: omite flag+UNWRAP, LOAD payload directo (UB
        // si vacio) -- cero coste, sin red.
        if (e->args[0]->result_type.kind == PrimitiveKind::OPTIONAL) {
            const Type at = e->args[0]->result_type;
            const Type payload_st =
                at.pointee ? *at.pointee : Type{PrimitiveKind::I64};
            const ir::IrType payload_t =
                ir_type_from_primitive(payload_st.kind);
            /* Donde esta cada cosa lo dice la disposicion.  Sin marca aparte,
             * lo que se comprueba es el propio valor -- que es lo que la lleva
             * -- y esta en el desplazamiento cero. */
            const OptionalLayout lay = optional_layout(at);
            if (is_unwrap) {
                /* Lo que hay en el desplazamiento cero es lo que decide si el
                 * valor esta: la marca cuando va aparte, y el valor mismo
                 * cuando no -- un puntero presente no es nulo --.  En los dos
                 * casos la comprobacion es la misma: lanzar si es cero. */
                const ir::IrValueId v_marca =
                    emit_load_typed(v_arg, ir::IrType::I64, e->loc.line);
                (void)emit_ir_unop(ir::IrOp::UNWRAP, v_marca, ir::IrType::I64,
                                   e->loc.line);
            }
            // Y el valor, donde la disposicion diga.
            const ir::IrValueId v_off = emit_const(
                ir::IrType::I64, static_cast<uint64_t>(lay.value_offset),
                e->loc.line);
            const ir::IrValueId v_at =
                emit_ptr_add(v_arg, v_off, e->loc.line);
            // BugFix sret-cross-mem: propagar is_host_ptr del buffer al puntero
            // buf+8 para que el LOAD del payload emita `loadzh`/`movh` (host).
            // Sin esto, un Optional en buffer host (retornado por una fn SRET)
            // se leia con `loadz` (VM) sobre direccion host -> unwrap daba 0.
            // value/error ya lo hacian; unwrap no.
            fn_->values[v_at].is_host_ptr = fn_->values[v_arg].is_host_ptr;
            // Un payload STRUCT no se carga en un registro: el valor de un
            // agregado ES su direccion.  Se devuelve `buf+8` y quien lo
            // consuma copiara los bytes que necesite.
            if (payload_st.kind == PrimitiveKind::STRUCT &&
                !payload_st.struct_name.empty()) {
                out_value = v_at;
                return true;
            }
            const ir::IrValueId v_dst =
                emit_load_typed(v_at, payload_t, e->loc.line);
            out_value = v_dst;
            return true;
        }
        // Referencias: unwrap_unchecked baja a IDENTIDAD (el "valor" de
        // un unwrap es el mismo puntero; lo unico que anade unwrap es el
        // chequeo).  Devolvemos v_arg directo -> cero IR, cero coste.
        if (is_unwrap_unchecked) {
            out_value = v_arg;
            return true;
        }
        // Referencias legacy: VM `unwrap` directo (con chequeo runtime).
        const ir::IrType t = fn_->values[v_arg].type;
        const ir::IrValueId v_dst = fn_->new_value(t);
        ir::IrInstr ra{};
        ra.op = ir::IrOp::UNWRAP;
        ra.type = t;
        ra.dst = v_dst;
        ra.operands = {v_arg};
        ra.source_line = e->loc.line;
        emit(current_block_, std::move(ra));
        if (fn_->values[v_arg].is_host_ptr) {
            fn_->values[v_dst].is_host_ptr = true;
        }
        out_value = v_dst;
        return true;
    }

    return false;
}

} // namespace vx
