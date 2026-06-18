/**
 * @file bigo.cpp
 * @brief Implementacion del analisis de complejidad estatico (Big-O).
 *
 * Ver bigo.h para la descripcion del subsistema.  Algoritmo del nivel 1
 * (estatico estructural):
 *
 *   (a) Detectar loops naturales via back-edges del CFG.  Un back-edge es
 *       una arista @c (u -> h) donde @c h DOMINA a @c u (o, en la
 *       aproximacion conservadora que usamos: @c h aparece ANTES que @c u
 *       en el orden de bloques Y es alcanzable hacia atras desde @c u).
 *       El frontend Vex emite los loops con el header ANTES del body, asi
 *       que un sucesor con id <= id del bloque actual que ademas alcanza al
 *       bloque actual es un back-edge.
 *   (b) Profundidad de anidamiento: cuantos headers de loop CONTIENEN a
 *       otro.  La profundidad maxima -> grado polinomico:
 *           depth 0 -> O(1), 1 -> O(n), 2 -> O(n^2), 3 -> O(n^3), k -> O(n^k).
 *   (c) Recursion: la funcion contiene un CALL/TAILCALL a su propio nombre.
 *       Sin loops -> O(n) lineal (HEURISTIC).  Con el patron
 *       divide-y-venceras (>=2 self-calls + el argumento se reduce a la
 *       mitad via SHR/DIV-by-2) -> O(n log n) (HEURISTIC).
 *
 * Conservador: donde el patron no encaja, @c O_UNKNOWN con confianza
 * @c UNKNOWN (que NUNCA genera warning de contrato).
 *
 * --- Nivel 3 (EMPIRICO, --measure): DISENO documentado, no implementado ---
 * El harness compilaria el .vex a .velb, ejecutaria el interp con tamanos
 * crecientes n = 10, 100, 1000, ... contando instrucciones VM (el scheduler
 * ya lleva un instr-counter determinista).  Al doblar n se observa el ratio
 * de crecimiento del conteo:
 *       ratio ~ 1     -> O(1)
 *       ratio ~ 2     -> O(n)         (lineal: doblar n dobla el trabajo)
 *       ratio ~ 2+eps -> O(n log n)
 *       ratio ~ 4     -> O(n^2)
 *       ratio ~ 8     -> O(n^3)
 *       ratio crece sin tope -> O(2^n)
 * La variable de tamano sale de @c @complexity(O(n), n = <expr>).  Como la
 * cuenta de instrucciones del interp es DETERMINISTA, el ajuste es
 * reproducible (a diferencia de medir wall-time).  El hook para enchufar
 * esto es @c MeasureResult (reservado) + una rama --measure en main.cpp.
 */
#include "analyze/bigo.h"

#include "ir/ssa_ir.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace analyze {

/* ===================================================================== */
/*  Helpers de clases de coste                                            */
/* ===================================================================== */

const char *cost_class_str(CostClass c) {
    switch (c) {
    case CostClass::O_1:
        return "O(1)";
    case CostClass::O_LOGN:
        return "O(log n)";
    case CostClass::O_N:
        return "O(n)";
    case CostClass::O_NLOGN:
        return "O(n log n)";
    case CostClass::O_N2:
        return "O(n^2)";
    case CostClass::O_N3:
        return "O(n^3)";
    case CostClass::O_NK:
        return "O(n^k)";
    case CostClass::O_2N:
        return "O(2^n)";
    case CostClass::O_UNKNOWN:
    default:
        return "O(?)";
    }
}

const char *confidence_str(Confidence c) {
    switch (c) {
    case Confidence::EXACT:
        return "exacta";
    case Confidence::HEURISTIC:
        return "heuristica";
    case Confidence::UNKNOWN:
    default:
        return "desconocida";
    }
}

CostClass parse_cost_class(const std::string &expr) {
    // Normalizar: minusculas + sin espacios.
    std::string s;
    s.reserve(expr.size());
    for (char c : expr) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        s.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    // Quitar prefijo "o(" y sufijo ")" si estan presentes.
    if (s.size() >= 3 && s.compare(0, 2, "o(") == 0 && s.back() == ')')
        s = s.substr(2, s.size() - 3);
    // Tabla de equivalencias canonicas.  Comparamos contra la forma sin
    // espacios.  "nlogn" es la forma normalizada de "n log n".
    if (s == "1") return CostClass::O_1;
    if (s == "logn" || s == "log(n)") return CostClass::O_LOGN;
    if (s == "n") return CostClass::O_N;
    if (s == "nlogn" || s == "n*logn" || s == "n*log(n)")
        return CostClass::O_NLOGN;
    if (s == "n^2" || s == "n2" || s == "n*n" || s == "n*m" || s == "nm")
        return CostClass::O_N2;
    if (s == "n^3" || s == "n3" || s == "n*n*n") return CostClass::O_N3;
    if (s == "2^n" || s == "2n") return CostClass::O_2N;
    // n^k con k >= 4: reconocer "n^<digito>".
    if (s.size() >= 3 && s[0] == 'n' && s[1] == '^') {
        bool all_digit = true;
        for (size_t i = 2; i < s.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(s[i])))
                all_digit = false;
        if (all_digit) return CostClass::O_NK;
    }
    return CostClass::O_UNKNOWN;
}

/// @brief Mapea una profundidad de loop a la clase polinomica que aporta.
static CostClass class_from_depth(uint32_t depth) {
    switch (depth) {
    case 0:
        return CostClass::O_1;
    case 1:
        return CostClass::O_N;
    case 2:
        return CostClass::O_N2;
    case 3:
        return CostClass::O_N3;
    default:
        return CostClass::O_NK;
    }
}

/* ===================================================================== */
/*  Deteccion de loops via back-edges + profundidad de anidamiento        */
/* ===================================================================== */

/**
 * @brief Determina si @c from puede alcanzar @c to en el CFG (DFS).
 *
 * Usado para confirmar back-edges: un sucesor @c h del bloque @c u con
 * @c h.id <= u.id es back-edge si @c h alcanza a @c u (ciclo real), no una
 * simple arista hacia un bloque anterior de una rama if/else ya cerrada.
 */
static bool reaches(const ir::IrFunction &fn, ir::IrBlockId from,
                    ir::IrBlockId to,
                    std::vector<char> &visited) {
    if (from == to) return true;
    if (from >= fn.blocks.size()) return false;
    if (visited[from]) return false;
    visited[from] = 1;
    for (ir::IrBlockId s : fn.blocks[from].succs) {
        if (s < fn.blocks.size() && reaches(fn, s, to, visited))
            return true;
    }
    return false;
}

/**
 * @brief Recolecta los headers de loop (destinos de back-edge) de la fn.
 *
 * Un back-edge es @c (u -> h) con @c h.id <= u.id y @c h alcanzable desde
 * @c u (ciclo).  El header @c h se anota como cabecera de loop.
 */
static std::vector<ir::IrBlockId> collect_loop_headers(const ir::IrFunction &fn) {
    std::vector<ir::IrBlockId> headers;
    for (ir::IrBlockId u = 0; u < fn.blocks.size(); ++u) {
        for (ir::IrBlockId h : fn.blocks[u].succs) {
            if (h >= fn.blocks.size()) continue;
            if (h <= u) {
                // Candidato a back-edge: confirmar que h alcanza a u
                // (ciclo real) explorando desde los sucesores de h.
                std::vector<char> visited(fn.blocks.size(), 0);
                bool cyclic = false;
                for (ir::IrBlockId s : fn.blocks[h].succs) {
                    if (s < fn.blocks.size() && reaches(fn, s, u, visited)) {
                        cyclic = true;
                        break;
                    }
                    // resetear visited entre exploraciones de sucesores.
                    std::fill(visited.begin(), visited.end(), 0);
                }
                // El caso h==u (self-loop de un bloque) tambien es ciclo.
                if (h == u) cyclic = true;
                if (cyclic) {
                    if (std::find(headers.begin(), headers.end(), h) ==
                        headers.end())
                        headers.push_back(h);
                }
            }
        }
    }
    return headers;
}

/**
 * @brief Calcula la profundidad de anidamiento de cada header de loop.
 *
 * Aproximacion estructural por contencion de rango de bloques: el frontend
 * Vex emite el CFG de modo que un loop interno tiene su header CON id mayor
 * que el del externo y su back-edge tambien dentro del rango del externo.
 * Definimos el rango de un loop con header @c h como [h, max_u] donde
 * @c max_u es el mayor bloque con un back-edge a @c h.  El loop A anida
 * dentro de B si el rango de A esta contenido estrictamente en el de B.
 */
static uint32_t compute_loop_depths(
    const ir::IrFunction &fn,
    const std::vector<ir::IrBlockId> &headers,
    std::vector<LoopCost> &out_loops) {

    // Para cada header, el id del back-edge mas lejano (fin del rango).
    struct Range {
        ir::IrBlockId h;
        ir::IrBlockId last;
    };
    std::vector<Range> ranges;
    ranges.reserve(headers.size());
    for (ir::IrBlockId h : headers) {
        ir::IrBlockId last = h;
        for (ir::IrBlockId u = 0; u < fn.blocks.size(); ++u) {
            for (ir::IrBlockId s : fn.blocks[u].succs) {
                if (s == h && u >= h) last = std::max(last, u);
            }
        }
        ranges.push_back({h, last});
    }

    uint32_t max_depth = 0;
    for (const Range &r : ranges) {
        // Profundidad = 1 + numero de loops que contienen estrictamente a r.
        uint32_t depth = 1;
        for (const Range &o : ranges) {
            if (o.h == r.h && o.last == r.last) continue;
            // o contiene estrictamente a r.
            if (o.h <= r.h && o.last >= r.last &&
                !(o.h == r.h && o.last == r.last))
                ++depth;
        }
        max_depth = std::max(max_depth, depth);

        // Linea fuente aproximada del header (primera instr con source_line).
        uint32_t line = 0;
        if (r.h < fn.blocks.size()) {
            for (const auto &ins : fn.blocks[r.h].instrs) {
                if (ins.source_line != 0) {
                    line = ins.source_line;
                    break;
                }
            }
        }
        out_loops.push_back({r.h, depth, line});
    }
    return max_depth;
}

/* ===================================================================== */
/*  Deteccion de recursion + divide-y-venceras                            */
/* ===================================================================== */

/**
 * @brief Cuenta las llamadas recursivas (a la propia fn) y detecta si el
 *        cuerpo reduce el argumento a la mitad (SHR por 1 / DIV por 2).
 */
static void detect_recursion(const ir::IrFunction &fn, uint32_t &self_calls,
                             bool &halves_arg) {
    self_calls = 0;
    halves_arg = false;
    for (const auto &b : fn.blocks) {
        for (const auto &ins : b.instrs) {
            if ((ins.op == ir::IrOp::CALL || ins.op == ir::IrOp::TAILCALL) &&
                ins.func_name == fn.name) {
                ++self_calls;
            }
            // Patron divide-y-venceras: shift a la derecha por 1 (a >> 1) o
            // division por la constante 2.  Buscamos un SHR cuyo operando de
            // desplazamiento sea el valor constante 1, o un DIV por 2.  Como
            // aproximacion barata, marcamos halves_arg si HAY un SHR o un DIV
            // en el cuerpo (el frontend genera /2 como SHR para potencias de
            // 2).  Conservador: solo se usa para subir O(n) -> O(n log n)
            // cuando ademas hay >=2 self-calls.
            if (ins.op == ir::IrOp::SHR || ins.op == ir::IrOp::SAR ||
                ins.op == ir::IrOp::DIV) {
                halves_arg = true;
            }
        }
    }
}

/* ===================================================================== */
/*  Analisis de funcion                                                   */
/* ===================================================================== */

CostResult analyze_function(const ir::IrFunction &fn) {
    CostResult r;
    r.function = fn.name;

    // 1. Loops + profundidad de anidamiento.
    std::vector<ir::IrBlockId> headers = collect_loop_headers(fn);
    uint32_t max_depth = compute_loop_depths(fn, headers, r.loops);
    r.max_loop_depth = max_depth;

    // 2. Recursion + divide-y-venceras.
    uint32_t self_calls = 0;
    bool halves = false;
    detect_recursion(fn, self_calls, halves);
    r.is_recursive = (self_calls > 0);
    r.is_divide_conquer = (self_calls >= 2 && halves);

    // 3. Combinar: el coste es el MAYOR entre el aporte de los loops y el
    //    de la recursion.
    CostClass loop_class = class_from_depth(max_depth);
    Confidence loop_conf = Confidence::EXACT;

    CostClass rec_class = CostClass::O_1;
    Confidence rec_conf = Confidence::EXACT;
    if (r.is_divide_conquer) {
        rec_class = CostClass::O_NLOGN;
        rec_conf = Confidence::HEURISTIC;
    } else if (r.is_recursive) {
        rec_class = CostClass::O_N;
        rec_conf = Confidence::HEURISTIC;
    }

    // Elegir el mayor coste (mayor valor del enum, salvo O_UNKNOWN que es el
    // ultimo y representa "no se").
    if (static_cast<int>(loop_class) >= static_cast<int>(rec_class)) {
        r.big_o = loop_class;
        r.confidence = loop_conf;
    } else {
        r.big_o = rec_class;
        r.confidence = rec_conf;
    }
    // Si hay loops Y recursion, baja la confianza (la composicion exacta no
    // esta modelada) pero mantenemos la cota dominante.
    if (max_depth > 0 && r.is_recursive)
        r.confidence = Confidence::HEURISTIC;

    // 4. Construir la explicacion legible.
    std::ostringstream det;
    if (max_depth == 0 && !r.is_recursive) {
        det << "sin loops ni recursion";
    } else {
        bool first = true;
        if (max_depth > 0) {
            det << headers.size() << " loop(s), anidamiento max " << max_depth;
            first = false;
        }
        if (r.is_recursive) {
            if (!first) det << "; ";
            det << "recursiva (" << self_calls << " self-call"
                << (self_calls == 1 ? "" : "s") << ")";
            if (r.is_divide_conquer) det << " divide-y-venceras";
        }
    }
    r.detail = det.str();

    // 5. Contrato @complexity: validacion CONSERVADORA.
    if (!fn.complexity_expr.empty()) {
        r.declared_expr = fn.complexity_expr;
        r.declared_class = parse_cost_class(fn.complexity_expr);
        // Solo avisar si: confianza EXACT, ambas clases conocidas, y
        // difieren.  Nunca avisar cuando inferimos O(?) o la declarada es
        // O(?) (la conservadora exige certeza para senalar el error).
        if (r.confidence == Confidence::EXACT &&
            r.big_o != CostClass::O_UNKNOWN &&
            r.declared_class != CostClass::O_UNKNOWN &&
            r.declared_class != r.big_o) {
            r.contract_mismatch = true;
        }
    }
    return r;
}

ModuleCost analyze_module(const ir::IrModule &mod) {
    ModuleCost mc;
    for (const auto &fn : mod.functions) {
        // Saltar stubs nativos (sin cuerpo IR) y macros compiladas (solo
        // existen en compile-time, no representan trabajo runtime).
        if (fn.is_native) continue;
        if (fn.is_macro_compiled) continue;
        mc.functions.push_back(analyze_function(fn));
    }
    return mc;
}

/* ===================================================================== */
/*  Serializacion JSON (hook para diagramas)                              */
/* ===================================================================== */

/// @brief Escapa una cadena para insertarla en un literal JSON.
static std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

std::string cost_result_to_json(const CostResult &r) {
    std::ostringstream o;
    o << "{";
    o << "\"function\":\"" << json_escape(r.function) << "\",";
    o << "\"big_o\":\"" << cost_class_str(r.big_o) << "\",";
    o << "\"confidence\":\"" << confidence_str(r.confidence) << "\",";
    o << "\"max_loop_depth\":" << r.max_loop_depth << ",";
    o << "\"is_recursive\":" << (r.is_recursive ? "true" : "false") << ",";
    o << "\"is_divide_conquer\":"
      << (r.is_divide_conquer ? "true" : "false") << ",";
    o << "\"declared\":\""
      << json_escape(r.declared_expr.empty() ? std::string()
                                             : r.declared_expr)
      << "\",";
    o << "\"contract_mismatch\":"
      << (r.contract_mismatch ? "true" : "false") << ",";
    o << "\"detail\":\"" << json_escape(r.detail) << "\",";
    o << "\"loops\":[";
    for (size_t i = 0; i < r.loops.size(); ++i) {
        const LoopCost &l = r.loops[i];
        if (i) o << ",";
        o << "{\"header_block\":" << l.header_block
          << ",\"depth\":" << l.depth << ",\"source_line\":" << l.source_line
          << "}";
    }
    o << "]";
    o << "}";
    return o.str();
}

std::string module_cost_to_json(const ModuleCost &m) {
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < m.functions.size(); ++i) {
        if (i) o << ",";
        o << cost_result_to_json(m.functions[i]);
    }
    o << "]";
    return o.str();
}

} // namespace analyze
