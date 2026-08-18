/**
 * @file native_callback.cpp
 * @brief Resolucion de callbacks Vesta -> C nativos.
 *
 * Sprint B.1 (2026-06-01): version inicial via thunk hand-emitted que
 * adaptaba la convencion C nativa -> VM_ABI -> codigo JIT de la fn Vesta,
 * salvando/restaurando los 16 registros VM en cada invocacion.
 *
 * callback-ABI (2026-06-06): el thunk se ELIMINA.  En su lugar la fn Vesta
 * se compila DIRECTAMENTE con un entry de ABI C nativo (modo callback del
 * selector, ver @c jit::compile_native_callback).  El prologo nativo lee
 * @c ProcessVM* via TLS/call y mueve los args nativos a los slots de los
 * params; solo salva/restaura el banco de registros VM cuando el cuerpo
 * puede ensuciarlo (funciones que llaman / usan raw_asm).  Una funcion
 * hoja pura (e.g. un comparator de qsort) NO salva nada -> ~2x mas rapida
 * que el thunk, que pagaba 16 save + 16 restore por llamada.
 *
 * Patron de uso (sin cambios para el usuario):
 *
 *   extern "msvcrt.dll" { fn qsort(u8* base, u64 n, u64 sz, u64 cmp) -> void; }
 *   i32 mi_cmp(u8* a, u8* b) { return (i32)*(u8*)a - (i32)*(u8*)b; }
 *   i32 main() {
 *       u8[5] arr = {3, 1, 4, 1, 5};
 *       u64 cb = as_native_callback(mi_cmp);   // direccion del codigo callback
 *       qsort(&arr[0], 5, 1, cb);
 *       return 42;
 *   }
 */

#include "runtime/native_callback.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "jit/auto_jit.h"
#include "runtime/exception_runtime.h"
#include "ffi/virtual_lib_registry.h"

#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h> // __get_cpuid / __get_cpuid_count (CPU dispatch)
#endif

/* Wrapper extern "C" para que el builtin Vesta `as_native_callback(fn)` lo
 * invoque via CALLN.  Args en proc->registers.regs[R01]=fn_pc, R02=argc.
 * Retorno en proc->registers.regs[R00] = direccion host del codigo callback.
 *
 * argc se ignora: el numero real de params lo deriva el selector del IR de
 * la funcion.  Se mantiene en la firma por compatibilidad con el call site
 * que el lowering ya emite (r1=fn_pc, r2=argc).
 *
 * Registrado en el virtual_lib_registry como
 * @c "vesta_runtime:vx_get_native_thunk". */
extern "C" uint64_t vx_get_native_thunk(uint64_t fn_pc, uint64_t /*argc*/) {
    runtime::ProcessVM *proc = runtime::get_current_executing_process();
    if (proc == nullptr) return 0;
    /* Compila (o reusa de cache) la fn como callback de ABI nativo y
     * devuelve su direccion.  compile_native_callback fuerza el compile
     * aunque el JIT este desactivado por flag (el callback REQUIERE codigo
     * nativo: su direccion se entrega a la API C). */
    return jit::compile_native_callback(proc, fn_pc);
}

/* Auto-registro en el virtual_lib_registry al cargar el binario.  Asi
 * tanto el path --vx (compile) como --run (ejecutar .velb) tienen la
 * fn disponible sin necesidad de llamar al TypeChecker constructor.
 *
 * `__attribute__((used))` evita que `-Wl,--gc-sections` elimine el
 * objeto (no es referenciado desde main).  `__attribute__((constructor))`
 * fuerza ejecucion antes de main, garantizado por GCC/Clang. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((used,
               constructor)) static void vx_callback_register_virtual_fn() {
    ffi::register_virtual_fn("vesta_runtime", "vx_get_native_thunk",
                             reinterpret_cast<void *>(&::vx_get_native_thunk));
}
#else
namespace {
struct VxCallbackAutoRegister {
    VxCallbackAutoRegister() {
        ffi::register_virtual_fn(
            "vesta_runtime", "vx_get_native_thunk",
            reinterpret_cast<void *>(&::vx_get_native_thunk));
    }
};
static VxCallbackAutoRegister _vx_callback_auto_register;
} // namespace
#endif

/* Helper que el main.cpp puede llamar explicitamente para forzar el
 * registro (defense-in-depth si el constructor no se ejecuta por algun
 * motivo de linking). */
extern "C" void runtime_ensure_vx_callback_registered(void) {
    ffi::register_virtual_fn("vesta_runtime", "vx_get_native_thunk",
                             reinterpret_cast<void *>(&::vx_get_native_thunk));
}

/* CPU dispatch (cimiento): deteccion de features del HOST para interp/JIT.
 * En AOT el binario corre su propio __vx_cpu_init (cpuid dentro del .exe);
 * en interp/JIT la VM corre sobre la CPU real -> exponemos las features del
 * host via esta fn nativa, resuelta por el virtual_lib_registry (sin DLL).
 * MISMO bit layout que el __vx_cpu_init de AOT (ver lowering.cpp):
 *   bit0=SSE2 1=SSE4.2 2=POPCNT 3=AVX 4=AVX2 5=BMI1 6=BMI2 7=AVX512F 8=ERMS.
 * Cacheado: cpuid corre una sola vez por proceso. */
extern "C" uint64_t vesta_runtime_cpu_features(void) {
    static uint64_t cached = 0;
    static bool done = false;
    if (done) return cached;
    uint64_t f = 0;
#if defined(__GNUC__) || defined(__clang__)
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1u, &a, &b, &c, &d)) {
        if (d & (1u << 26)) f |= 1ull << 0; // EDX.26 SSE2
        if (c & (1u << 20)) f |= 1ull << 1; // ECX.20 SSE4.2
        if (c & (1u << 23)) f |= 1ull << 2; // ECX.23 POPCNT
        if (c & (1u << 28)) f |= 1ull << 3; // ECX.28 AVX
    }
    if (__get_cpuid_count(7u, 0u, &a, &b, &c, &d)) {
        if (b & (1u << 5)) f |= 1ull << 4;  // EBX.5  AVX2
        if (b & (1u << 3)) f |= 1ull << 5;  // EBX.3  BMI1
        if (b & (1u << 8)) f |= 1ull << 6;  // EBX.8  BMI2
        if (b & (1u << 16)) f |= 1ull << 7; // EBX.16 AVX512F
        if (b & (1u << 9)) f |= 1ull << 8;  // EBX.9  ERMS
    }
#else
    // Sin cpuid.h (no GNU/Clang): x86-64 garantiza SSE2 por ABI base.
    f = 1ull << 0;
#endif
    cached = f;
    done = true;
    return f;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((used,
               constructor)) static void vx_cpu_features_register_virtual_fn() {
    ffi::register_virtual_fn(
        "vesta_runtime", "cpu_features",
        reinterpret_cast<void *>(&::vesta_runtime_cpu_features));
}
#else
namespace {
struct VxCpuFeaturesAutoRegister {
    VxCpuFeaturesAutoRegister() {
        ffi::register_virtual_fn(
            "vesta_runtime", "cpu_features",
            reinterpret_cast<void *>(&::vesta_runtime_cpu_features));
    }
};
static VxCpuFeaturesAutoRegister _vx_cpu_features_auto_register;
} // namespace
#endif
