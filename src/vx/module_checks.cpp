/**
 * @file module_checks.cpp
 * @brief Implementacion de las comprobaciones previas a optimizar.
 */
#include "vx/module_checks.h"

#include "analyze/int_wraparound.h"
#include "vx/parser.h" // get_aot_condcomp_target: el objetivo activo

namespace vx {

bool run_pre_opt_checks(const PreOptInput &in, Diagnostics &diags) {
    if (in.module == nullptr || in.file == nullptr) return true;
    const ir::IrModule &mod = *in.module;
    const std::string &file = *in.file;

    /* 1. Nativas que se contradicen.  `IrModule::register_native_import`
     * detecta el choque y se pone a salvo solo -- se queda con la union de lo
     * peor --, pero no puede DECIRLO porque no tiene canal de diagnostico. */
    analyze::report_native_effect_conflicts(mod, file, diags);

    // 2. Los contratos de huella, contra el intermedio PRE-opt, que es donde
    // TODAS las funciones existen todavia.
    if (in.contracts != nullptr && !in.contracts->empty() && !in.measure_only) {
        /* El objetivo ACTIVO, que decide con que tabla de efectos se analiza
         * cada bloque de asm; vacio = el anfitrion de la construccion. */
        std::string fp_os, fp_arch;
        vx::get_aot_condcomp_target(fp_os, fp_arch);
        if (fp_arch.empty()) fp_arch = "x86_64";
        auto fps = analyze::compute_module_fingerprints(mod, fp_arch);
        /* Con el modulo delante: lo que las importaciones DECLAREN de una
         * nativa cuenta, en vez de volver opaco el cierre entero. */
        analyze::compose_fingerprints(fps, in.contracts, &mod);
        /* Los tres veredictos por UNA puerta.  Aqui hubo copias del criterio
         * que se quedaban con el incumplimiento y descartaban el indecidible
         * sin decir nada. */
        const analyze::ContractReport rep = analyze::report_contract_checks(
            analyze::verify_contracts(fps, *in.contracts), file, diags);
        if (rep.violated != 0) return false;
    }

    /* 3. Los contratos de TIPO.  Son decidibles del layout -- nunca sale un
     * indecidible --, asi que esa rama de la puerta no se usa aqui; pero el
     * criterio de que hacer con cada veredicto tiene que estar escrito una sola
     * vez, o el dia que cambie se cambiara en uno de los dos sitios. */
    if (in.type_contracts != nullptr && !in.type_contracts->empty() &&
        in.type_fingerprints != nullptr) {
        const analyze::ContractReport trep = analyze::report_contract_checks(
            analyze::verify_type_contracts(*in.type_fingerprints,
                                           *in.type_contracts),
            file, diags);
        if (trep.violated != 0) return false;
    }

    /* 4. Una cuenta entera que se sale de su tipo, con los dos operandos
     * sabidos.  Es un ERROR y no un aviso porque envolver sin querer no da un
     * fallo, da otro numero, y eso no se ve hasta mucho despues.  Para que no
     * lo sea se escribe un cast al tipo, `(i8)(a + b)`, igual que se declara
     * cualquier otra conversion que pierde informacion. */
    bool wrapped = false;
    for (const ir::IrFunction &f : mod.functions) {
        for (const analyze::IntWrap &w : analyze::find_int_wraparounds(f)) {
            vx::SourceLoc loc;
            loc.set_file(file);
            loc.line = static_cast<int>(w.line);
            diags.diag(loc, vx::DiagLevel::ERR, "VX2050",
                       {std::to_string(w.exact), ir::ir_type_name(w.type),
                        std::to_string(w.lo), std::to_string(w.hi)});
            wrapped = true;
        }
    }
    if (wrapped) return false;

    return true;
}

} // namespace vx
