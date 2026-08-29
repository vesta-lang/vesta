/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/productores.cpp
 * @brief El motor de produccion y los dominios que hoy saben afirmar.
 *
 * El motor NO conoce ningun dominio: recorre los registrados.  Cada dominio es
 * una funcion corta que traduce SU analisis a hechos, y ahi -- no en quien
 * luego los mire -- vive el criterio de que merece afirmarse.
 */

#include "util/env_flags.h"
#include "analysis/asa/productores.h"

#include "ir/ssa_ir.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include "analysis/facts/alignment.h"
#include "vx/asm/asm_cfg.h"
#include "vx/asm/asm_effects.h" // isa_actual

namespace analysis {
namespace asa {

// ===========================================================================
// Contexto de produccion
// ===========================================================================

bool Produccion::interesa(const ir::IrFunction &fn) const {
    /* Un stub de funcion nativa no tiene cuerpo del que sacar nada.  No hay mas
     * criterio: el volcado es entero, sin variantes que combinar. */
    return !fn.is_native;
}

FactId Produccion::afirmar(Fact f) {
    ++resumen.miradas;
    ++resumen.hechos;
    return almacen.anadir(std::move(f));
}

void Produccion::callar(Sujeto de_quien, const char *motivo,
                        const char *dominio, const char *detalle) {
    ++resumen.miradas;
    ++resumen.callados;
    /* El motivo SIEMPRE, aunque no se pidan los hechos uno a uno: un dominio
     * que no supo algo tiene que decir por que, o su silencio no se puede
     * arreglar. Son pocos codigos por dominio -- un vector plano se recorre
     * antes de lo que un mapa calcula el hash. */
    bool contado = false;
    for (MotivoIgnorancia &m : resumen.motivos) {
        if (m.codigo == motivo || std::strcmp(m.codigo, motivo) == 0) {
            ++m.veces;
            contado = true;
            break;
        }
    }
    if (!contado) resumen.motivos.push_back({motivo, 1});
    /* "De esto no se sabe nada" ES un hecho -- con certeza Desconocida --, no
     * la ausencia de uno: distingue lo que se miro y no dio nada de lo que ni
     * siquiera se miro, y esa diferencia es la que dice donde ampliar. */
    Fact f;
    f.que.dominio = dominio;
    f.que.codigo = motivo;
    f.que.detalle = almacen.internar(detalle);
    f.de_quien = de_quien;
    f.sello.certeza = Certeza::Desconocida;
    f.sello.origen.productor = dominio;
    f.sello.origen.funcion = de_quien.funcion;
    almacen.anadir(std::move(f));
}

// ===========================================================================
// Registro
// ===========================================================================
namespace {

struct DominioRegistrado {
    const char *nombre;
    Productor productor;
};

/// Vector plano: son unos pocos y se recorren enteros; un mapa aqui seria
/// indireccion para nada.  Function-local para no depender del orden de
/// inicializacion estatica entre unidades de traduccion.
std::vector<DominioRegistrado> &registro() {
    static std::vector<DominioRegistrado> r;
    return r;
}

void registrar_los_incluidos();

/// Da de alta los dominios de casa una sola vez.  Explicito y no por
/// inicializacion estatica: asi el orden es el que se lee aqui, y no el que
/// decida el enlazador.  Importa: la estructura va primero para que los demas
/// puedan apoyar SUS hechos en el suyo.
void asegurar_registro() {
    static const bool hecho = [] {
        registrar_los_incluidos();
        return true;
    }();
    (void)hecho;
}

Sujeto sujeto_funcion(Produccion &p, const ir::IrFunction &fn) {
    Sujeto s;
    s.clase = Sujeto::Clase::Funcion;
    s.funcion = p.almacen.internar(fn.name);
    return s;
}

Sujeto sujeto_valor(Produccion &p, const ir::IrFunction &fn, ir::IrValueId v) {
    Sujeto s;
    s.clase = Sujeto::Clase::Valor;
    s.funcion = p.almacen.internar(fn.name);
    s.id = v;
    return s;
}

/// El hecho de estructura de @p fn si ya se produjo, para apoyarse en EL y no
/// solo en el nombre de su productor.
void apoyar_en_estructura(Produccion &p, const ir::IrFunction &fn, Fact &f,
                          const char *regla) {
    f.prueba.regla = regla;
    auto it = p.estructura_de.find(fn.name);
    if (it != p.estructura_de.end()) f.prueba.de.push_back(it->second);
    f.sello.apoyos.anadir(kProductorEstructura);
}

/// El intervalo, con sus numeros: un hecho que no ensena su valor obliga a
/// mirar el codigo para saber que dice.
std::string texto_rango(const ValueRange &r) {
    std::ostringstream o;
    int64_t lo = 0, hi = 0;
    if (r.vista_con_signo(lo, hi)) {
        if (lo == hi)
            o << "= " << lo;
        else
            o << "[" << lo << "," << hi << "]";
    } else {
        o << "[" << r.lo_c << "," << r.hi_c << "] sin signo";
    }
    o << " " << (r.t.sin_signo ? "u" : "i") << static_cast<int>(r.t.bits);
    return o.str();
}

// ===========================================================================
// DOMINIOS
// ===========================================================================

/// Estructura: la forma de la funcion.  Un recorrido, sin reticulo: lo que sale
/// de aqui es lo que el IR dice, no una aproximacion.
void producir_estructura(Produccion &p) {
    for (const ir::IrFunction &fn : p.mod.functions) {
        if (!p.interesa(fn)) continue;
        const IrFacts &h = p.base.estructura(fn);
        Fact f;
        f.que.dominio = kProductorEstructura;
        f.que.codigo = "estructura.forma";
        f.que.a = h.block_count;
        f.que.b = h.loop_count;
        std::ostringstream o;
        o << "bloques=" << h.block_count << " bucles=" << h.loop_count
          << " llamadas=" << h.static_callees.size()
          << (h.has_dynamic_call ? " +dinamicas" : "")
          << (h.recursive ? " recursiva" : "") << " params=" << fn.params.size()
          << " valores=" << fn.values.size();
        f.que.detalle = p.almacen.internar(o.str());
        f.de_quien = sujeto_funcion(p, fn);
        f.sello = p.base.sello(kProductorEstructura, fn);
        f.prueba.regla = "recorrido-del-cfg";
        p.estructura_de[fn.name] = p.afirmar(std::move(f));
    }
}

/// Rangos: entre que dos numeros esta cada valor.
///
/// CRITERIO DEL DOMINIO: se afirma lo que dice MAS que el tipo.  Repetir "un
/// u64 cabe en un u64" no es conocimiento, es la definicion del tipo.
void producir_rangos(Produccion &p) {
    for (const ir::IrFunction &fn : p.mod.functions) {
        if (!p.interesa(fn)) continue;
        const RangeFacts &rf = p.base.rangos(fn);
        const Sello s = p.base.sello(kProductorRangos, fn);
        for (ir::IrValueId v = 0; v < fn.values.size(); ++v) {
            const ValueRange &r = rf.at(v);
            if (r.es_bottom()) {
                Fact f;
                f.que.dominio = kProductorRangos;
                f.que.codigo = "rango.inalcanzable";
                f.que.detalle = "este punto no se ejecuta";
                f.de_quien = sujeto_valor(p, fn, v);
                f.sello = s;
                apoyar_en_estructura(p, fn, f, "flujo-de-datos");
                p.afirmar(std::move(f));
                continue;
            }
            if (!r.acotada() || r.es_todo()) {
                p.callar(sujeto_valor(p, fn, v), "rango.sin_cota",
                         kProductorRangos,
                         r.es_top() ? "sin dominio" : "vale todo su tipo");
                continue;
            }
            Fact f;
            f.que.dominio = kProductorRangos;
            f.que.codigo =
                r.es_constante() ? "rango.constante" : "rango.acotado";
            int64_t lo = 0, hi = 0;
            if (r.vista_con_signo(lo, hi)) {
                f.que.a = lo;
                f.que.b = hi;
            } else {
                f.que.a = static_cast<int64_t>(r.lo_c);
                f.que.b = static_cast<int64_t>(r.hi_c);
            }
            f.que.detalle = p.almacen.internar(texto_rango(r));
            f.de_quien = sujeto_valor(p, fn, v);
            f.sello = s;
            apoyar_en_estructura(p, fn, f, "flujo-de-datos");
            p.afirmar(std::move(f));
        }
    }
}

/// Frontera: lo que entra y sale de cada funcion.  Conocimiento del MODULO: lo
/// que le llega a un parametro solo se sabe mirando a todos los que llaman.
void producir_frontera(Produccion &p) {
    const RangeSummaries &rs = p.base.frontera(p.mod);
    const Sello s = p.base.sello_de_modulo(kProductorFrontera);
    for (const ir::IrFunction &fn : p.mod.functions) {
        if (!p.interesa(fn)) continue;
        const FnRangeSummary *r = rs.buscar(fn.name);
        if (r == nullptr) {
            p.callar(sujeto_funcion(p, fn), "frontera.sin_resumen",
                     kProductorFrontera, "no aparece en el grafo de llamadas");
            continue;
        }
        Fact f;
        f.que.dominio = kProductorFrontera;
        f.que.codigo = r->cerrada ? "frontera.cerrada" : "frontera.abierta";
        f.que.a = static_cast<int64_t>(r->params.size());
        std::ostringstream o;
        o << (r->cerrada ? "se ven todos los llamantes"
                         : "llamantes sin ver -- los parametros valen su tipo");
        for (size_t i = 0; i < r->params.size(); ++i) {
            if (!r->params[i].acotada() || r->params[i].es_todo()) continue;
            int64_t lo = 0, hi = 0;
            if (!r->params[i].vista_con_signo(lo, hi)) continue;
            o << " | param" << i << " en [" << lo << "," << hi << "]";
            ++f.que.b;
        }
        if (r->ret.acotada() && !r->ret.es_todo()) {
            int64_t lo = 0, hi = 0;
            if (r->ret.vista_con_signo(lo, hi))
                o << " | devuelve [" << lo << "," << hi << "]";
        }
        f.que.detalle = p.almacen.internar(o.str());
        f.de_quien = sujeto_funcion(p, fn);
        f.sello = s;
        apoyar_en_estructura(p, fn, f, "punto-fijo-del-grafo-de-llamadas");
        p.afirmar(std::move(f));
    }
}

/// Memoria: a que se puede referir cada puntero.
///
/// CRITERIO DEL DOMINIO: un puntero que puede apuntar a cualquier cosa no es un
/// hecho, es la ausencia de uno.
/**
 * @brief Con que alineacion se COLOCA cada seccion de datos.
 *
 * Es el hecho que un compilador con enlazador ajeno no puede producir: la
 * direccion final de un dato la decide otro, asi que lo unico afirmable es la
 * garantia generica del formato y cualquier exigencia mayor queda en "no puedo
 * probarlo".  Aqui la colocacion es nuestra.
 *
 * Y por eso mismo el hecho NACE CON AMBITO, en vez de ser un numero del
 * programa: la misma seccion no se coloca igual segun donde acabe corriendo.
 * En la maquina virtual el bloque lo reserva el cargador y el numero es firme.
 * En el nativo las secciones caen en pagina por defecto, PERO un guion de
 * enlazado puede ponerlas donde quiera (`place_section`), y esa direccion no
 * tiene por que cumplir nada.
 *
 * Afirmar el numero bueno sin decir para donde vale seria cometer justo el
 * fallo que este conocimiento existe para evitar: prometer una alineacion que
 * la memoria no da.  Y eso no falla ruidosamente -- lee mal.
 */
/**
 * @brief El FLUJO DE CONTROL de cada bloque `asm`, como hecho.
 *
 * Hoy esto se calcula dentro del elevado y muere ahi: si un bloque tiene un
 * salto, se marca opaco y nadie mas se entera de la FORMA que tiene -- cuantos
 * bloques basicos hay, que arista sale de cada uno, si algun destino no se
 * resuelve.  Y esa forma ya se sabe: `build_asm_cfg` la construye, y la
 * consumen el informe de efectos, `--analyze` y el calculo de coste, cada uno
 * volviendola a pedir por su cuenta.
 *
 * Puesta aqui, se calcula una vez y la lee quien quiera -- que es lo que hace
 * que el elevado del flujo (E1) no tenga que llevar su propio analisis.
 *
 * El terminador va en el hecho como NUMERO (`AsmTerm`) y no como frase: quien
 * decida algo compara el codigo, y el texto es solo para que lo lea una
 * persona.
 */
/// Nombre de la ISA con el vocabulario que ya usa @c Ambito::isa.  No existe un
/// helper para esto en la base, y escribirlo aqui es preferible a inventar otro
/// juego de nombres: el que compara es el filtro del alcance.
const char *nombre_isa_ambito(vx::instr_db::Isa isa) {
    switch (isa) {
    case vx::instr_db::Isa::X86: return "x86-64";
    case vx::instr_db::Isa::ARM64: return "aarch64";
    case vx::instr_db::Isa::ARM32: return "arm";
    case vx::instr_db::Isa::RISCV: return "riscv";
    }
    return "";
}

void producir_asm_flujo(Produccion &p) {
    const vx::instr_db::Isa isa = vx::isa_actual();
    for (const ir::IrFunction &fn : p.mod.functions) {
        if (!p.interesa(fn)) continue;
        uint32_t idx = 0; // posicion lineal de la instruccion dentro de la fn
        for (const ir::IrBlock &b : fn.blocks) {
            for (const ir::IrInstr &in : b.instrs) {
                ++idx;
                /* El cuerpo esta en `func_name` cuando el bloque quedo opaco, y
                 * en la ficha del micro cuando se elevo.  Los dos casos
                 * interesan: la forma del flujo no depende de si se elevo. */
                std::string cuerpo;
                if (in.op == ir::IrOp::INLINE_ASM) {
                    cuerpo = in.func_name;
                } else if (in.op == ir::IrOp::ASM_MICRO &&
                           in.imm < fn.asm_micros.size()) {
                    cuerpo = fn.asm_micros[in.imm].tmpl;
                } else {
                    continue;
                }
                if (cuerpo.empty()) continue;
                /* TAPoN PROVISIONAL, y conviene saber por que.
                 *
                 * Un `INLINE_ASM` no siempre lleva ensamblador: aqui llega
                 * `; __vxf_inject pendiente`, que es el MARCADOR de un bloque
                 * cuyo cuerpo lo genera `inject(...)` al compilar.  Saltarlos
                 * evita analizar un comentario, pero se lleva por delante justo
                 * los bloques CON FLUJO DE CONTROL: los `jb` del corpus salen
                 * todos de un `inject`, asi que lo que queda son los rectos.
                 *
                 * O sea que esto no esta bien: esta callado.  Lo correcto es
                 * que el flujo se analice DESPUES de la inyeccion, donde el
                 * cuerpo ya existe.  Hoy hay dos vistas y ninguna sirve sola --
                 * el lowering ve el asm expandido y avisa (VXA018), y aqui
                 * llega un marcador --, y por eso los dos recuentos nunca
                 * cuadraron. */
                if (cuerpo.find("__vxf_inject") != std::string::npos) continue;
                const vx::AsmCfg cfg = vx::build_asm_cfg(isa, cuerpo);
                if (util::flag_on(util::FlagId::AsmFlujoDebug))
                    std::fprintf(stderr,
                                 "[asm-flujo] %s#%u: %zu bloques, %zu instr, "
                                 "cuerpo=<%.60s>\n",
                                 fn.name.c_str(), idx, cfg.blocks.size(),
                                 cfg.insns.size(), cuerpo.c_str());
                if (cfg.blocks.size() <= 1 && !cfg.has_indirect &&
                    cfg.unknown_terminators.empty())
                    continue; // recto y sin sorpresas: no hay nada que decir

                Sujeto s;
                s.clase = Sujeto::Clase::Instruccion;
                s.funcion = p.almacen.internar(fn.name);
                s.id = idx;

                Fact f;
                f.que.dominio = kProductorAsmFlujo;
                f.que.codigo = "asm_flujo.forma";
                f.que.a = (int64_t)cfg.blocks.size();
                f.que.b = (int64_t)cfg.insns.size();
                std::ostringstream o;
                o << cfg.blocks.size() << " bloques basicos";
                if (cfg.has_indirect) o << ", con salto indirecto";
                if (cfg.has_unresolved_target)
                    o << ", con destino sin resolver";
                if (cfg.has_external_target)
                    o << ", con salida a un simbolo del modulo";
                if (!cfg.unknown_terminators.empty())
                    o << ", " << cfg.unknown_terminators.size()
                      << " terminador(es) sin clasificar";
                f.que.detalle = p.almacen.internar(o.str());
                f.de_quien = s;
                /* La forma del grafo no depende del backend, pero SI de la ISA:
                 * el mismo texto no se trocea igual en dos juegos de
                 * instrucciones. */
                f.donde.isa = p.almacen.internar(nombre_isa_ambito(isa));
                f.sello.certeza = Certeza::Demostrada;
                f.sello.origen.fuente = Fuente::Estatico;
                f.sello.origen.productor = kProductorAsmFlujo;
                f.sello.origen.funcion = s.funcion;
                f.sello.origen.sitio = idx;
                f.prueba.regla = "asm_flujo.grafo_del_bloque";
                p.afirmar(f);

                /* Y lo que NO se supo, con su motivo: un terminador sin
                 * clasificar es una laguna concreta -- el mnemonico -- y
                 * callarla la vuelve indistinguible de no haber mirado. */
                for (const std::string &mn : cfg.unknown_terminators)
                    p.callar(s, "asm_flujo.terminador_desconocido",
                             kProductorAsmFlujo, p.almacen.internar(mn));
            }
        }
    }
}

void producir_disposicion(Produccion &p) {
    /* Un hecho por SECCION, no por dato: la garantia es de la seccion, y el
     * desplazamiento de cada dato dentro de ella ya lo sabe quien pregunta. */
    bool hay_datos = false;
    for (size_t i = 0; i < p.mod.static_data.size(); ++i) {
        if (p.mod.static_data.meta_at(i).section_name == ".data") {
            hay_datos = true;
            break;
        }
    }
    if (!hay_datos) return; // sin datos estaticos no hay nada que colocar

    Sujeto sujeto;
    sujeto.clase = Sujeto::Clase::Modulo;

    /* Corriendo en la maquina -- interprete o JIT -- el bloque de globales lo
     * reserva el cargador, y lo hace con una alineacion que elegimos nosotros.
     * Ahi el numero no se estima: se sabe. */
    {
        Fact f;
        f.que.dominio = kProductorDisposicion;
        f.que.codigo = "disposicion.alineacion_seccion";
        f.que.a = static_cast<int64_t>(alineacion_seccion_datos("vm"));
        f.que.detalle = p.almacen.internar(".data");
        f.de_quien = sujeto;
        f.donde.backend = "vm";
        f.sello.certeza = Certeza::Demostrada;
        f.sello.origen.fuente = Fuente::Estatico;
        f.sello.origen.productor = kProductorDisposicion;
        f.prueba.regla = "disposicion.reserva_del_cargador";
        p.afirmar(f);
    }

    /* Y compilando a nativo NO se afirma, porque la colocacion la puede fijar
     * el usuario y aqui no se ve su guion de enlazado.  No decir nada seria
     * indistinguible de no haberlo mirado, asi que se deja constancia del
     * motivo: cuando el guion llegue hasta aqui, este silencio se convierte en
     * el numero que toque. */
    p.callar(sujeto, "disposicion.colocacion_configurable",
             kProductorDisposicion,
             "en el nativo la seccion la coloca el guion de enlazado; "
             "sin verlo no se puede afirmar su alineacion");
}

void producir_memoria(Produccion &p) {
    for (const ir::IrFunction &fn : p.mod.functions) {
        if (!p.interesa(fn)) continue;
        const PointsTo &pt = p.base.memoria(fn);
        const Sello s = p.base.sello(kProductorMemoria, fn);
        for (ir::IrValueId v = 0; v < fn.values.size(); ++v) {
            const effects::AbstractLoc l = loc_of(pt, v, 0);
            if (l.kind == effects::AbstractLoc::Kind::None ||
                l.kind == effects::AbstractLoc::Kind::Unknown) {
                p.callar(sujeto_valor(p, fn, v), "memoria.sin_localizar",
                         kProductorMemoria,
                         l.kind == effects::AbstractLoc::Kind::Unknown
                             ? "puede referirse a cualquier memoria"
                             : "no es un puntero localizable");
                continue;
            }
            Fact f;
            f.que.dominio = kProductorMemoria;
            f.que.codigo = "memoria.apunta_a";
            f.que.a = static_cast<int64_t>(l.id);
            f.que.b = l.off;
            const char *clase = "";
            switch (l.kind) {
            case effects::AbstractLoc::Kind::Stack: clase = "pila"; break;
            case effects::AbstractLoc::Kind::Heap: clase = "monton"; break;
            case effects::AbstractLoc::Kind::Global: clase = "global"; break;
            default: clase = "desde-parametro"; break;
            }
            std::ostringstream o;
            o << clase;
            if (l.id != effects::LOC_GENERIC) o << "#" << l.id;
            if (l.off != 0) o << (l.off > 0 ? "+" : "") << l.off;
            f.que.detalle = p.almacen.internar(o.str());
            f.de_quien = sujeto_valor(p, fn, v);
            f.sello = s;
            apoyar_en_estructura(p, fn, f, "propagacion-de-punteros");
            p.afirmar(std::move(f));
        }
    }
}

/// Bucles: donde estan y como de anidados.
void producir_bucles(Produccion &p) {
    for (const ir::IrFunction &fn : p.mod.functions) {
        if (!p.interesa(fn)) continue;
        const LoopFacts &lf = p.base.bucles(fn);
        const Sello s = p.base.sello(kProductorBucles, fn);
        uint32_t vistos = 0;
        for (ir::IrBlockId b = 0; b < fn.blocks.size(); ++b) {
            if (!lf.header_of(b)) continue;
            ++vistos;
            Fact f;
            f.que.dominio = kProductorBucles;
            f.que.codigo = "bucle.cabecera";
            f.que.a = lf.depth_of(b);
            std::ostringstream o;
            o << "cabecera de bucle, profundidad " << lf.depth_of(b);
            f.que.detalle = p.almacen.internar(o.str());
            f.de_quien.clase = Sujeto::Clase::Bloque;
            f.de_quien.funcion = p.almacen.internar(fn.name);
            f.de_quien.id = b;
            f.sello = s;
            apoyar_en_estructura(p, fn, f, "aristas-de-retroceso");
            p.afirmar(std::move(f));
        }
        if (vistos == 0)
            p.callar(sujeto_funcion(p, fn), "bucle.ninguno", kProductorBucles,
                     "no tiene bucles");
    }
}

void registrar_los_incluidos() {
    registrar_productor(kProductorEstructura, &producir_estructura);
    registrar_productor(kProductorRangos, &producir_rangos);
    registrar_productor(kProductorFrontera, &producir_frontera);
    registrar_productor(kProductorMemoria, &producir_memoria);
    registrar_productor(kProductorBucles, &producir_bucles);
    registrar_productor(kProductorDisposicion, &producir_disposicion);
    registrar_productor(kProductorAsmFlujo, &producir_asm_flujo);
}

} // namespace

// ===========================================================================
// Motor
// ===========================================================================

void registrar_productor(const char *dominio, Productor p) {
    for (const DominioRegistrado &d : registro())
        if (std::strcmp(d.nombre, dominio) == 0) return; // ya esta
    registro().push_back({dominio, p});
}

std::vector<const char *> productores_registrados() {
    asegurar_registro();
    std::vector<const char *> v;
    v.reserve(registro().size());
    for (const DominioRegistrado &d : registro())
        v.push_back(d.nombre);
    return v;
}

std::vector<ResumenProduccion> producir(const ir::IrModule &mod,
                                        FactStore &almacen) {
    /* Antes de producir nada: los nombres de los productores tienen que ser
     * canonicos para que un hecho leido de disco se reconozca como suyo. */
    register_asa_canonical_names();
    asegurar_registro();
    std::vector<ResumenProduccion> resumenes;
    /* UNA base para todos los dominios: si tres piden la estructura, se calcula
     * una vez.  Es la Regla 1 aplicada a la propia produccion. */
    BaseDeHechos base;
    std::unordered_map<std::string, FactId> estructura_de;

    /* Reservar de golpe: un modulo grande produce cientos de miles de hechos y
     * dejarlos crecer de uno en uno copia el vector entero una y otra vez.  La
     * cota se estima de lo unico que la determina -- valores por dominio -- y
     * no hace falta que sea exacta. */
    size_t valores = 0;
    for (const ir::IrFunction &fn : mod.functions)
        if (!fn.is_native) valores += fn.values.size();
    almacen.reservar(valores * 2u + 64u);

    /* Se reserva de golpe: los resumenes se referencian desde el contexto de
     * cada productor y un realloc a mitad dejaria la referencia colgando. */
    resumenes.reserve(registro().size());
    for (const DominioRegistrado &d : registro()) {
        resumenes.push_back(ResumenProduccion{});
        ResumenProduccion &r = resumenes.back();
        r.dominio = d.nombre;
        const auto t0 = std::chrono::steady_clock::now();
        Produccion p{mod, base, almacen, r, estructura_de};
        d.productor(p);
        r.micros = static_cast<long>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
    }
    return resumenes;
}

} // namespace asa
} // namespace analysis
