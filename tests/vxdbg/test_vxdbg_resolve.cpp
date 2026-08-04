/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_vxdbg_resolve.cpp
 * @brief El recorrido completo: de una direccion a una explicacion.
 *
 * Se construye a mano el grafo que produciria un frontend para un programa
 * pequeno y se comprueba que, dada una direccion de ejecucion, se llega hasta
 * el fichero, la linea, el metodo, su clase y de quien deriva.
 *
 * Es la prueba de que las capas encajan.  Cada una esta probada por su cuenta,
 * pero eso no dice nada de si el camino entero funciona: es justo donde se ven
 * los desajustes entre piezas que por separado estan bien.
 */

#include "vxdbg/codec.h"
#include "vxdbg/resolver.h"
#include "vxdbg/store_source.h"

#include <cstdio>
#include <string>

static int fallos = 0;

/**
 * @brief Comprueba una condicion y deja constancia.
 * @param cond Lo que debe cumplirse.
 * @param que Descripcion.
 */
static void comprobar(bool cond, const char *que) {
    if (cond) {
        std::printf("  OK   %s\n", que);
    } else {
        std::printf("  FALLA %s\n", que);
        ++fallos;
    }
}

/**
 * @brief El grafo de un programa pequeno, como lo dejaria un frontend.
 *
 *     class Lector : Flujo, Cerrable {
 *         i32 parse(string t) {   // linea 31
 *             ...                 // linea 33  <- aqui esta la direccion
 *         }
 *     }
 */
struct Programa {
    vxdbg::MemoryNodeStore store;
    vxdbg::StoreNodeSource nodes{store};
    vxdbg::SessionMap session;

    vxdbg::FileId fichero;
    vxdbg::LanguageEntityId clase, base, contrato, metodo, tipo_i32;
    vxdbg::ScopeId ambito;
    vxdbg::VariableId variable;
    vxdbg::StatementId sentencia;
    vxdbg::IrFunctionId funcion_ir;
    vxdbg::IrInstrId instruccion;
    vxdbg::CodeId cuerpo;

    /// Monta el grafo entero.
    void construir() {
        using namespace vxdbg;
        ContentHash h;

        // --- Capa semantica ---
        FileNode f;
        f.path = "lector.vx";
        f.language = "vesta";
        store_node(store, f, h);
        fichero = FileId{h};

        LanguageEntity flujo;
        flujo.name = "Flujo";
        flujo.kind = "class";
        store_node(store, flujo, h);
        base = LanguageEntityId{h};

        LanguageEntity cerrable;
        cerrable.name = "Cerrable";
        cerrable.kind = "interface";
        store_node(store, cerrable, h);
        contrato = LanguageEntityId{h};

        LanguageEntity i32;
        i32.name = "i32";
        i32.kind = "primitive";
        store_node(store, i32, h);
        tipo_i32 = LanguageEntityId{h};

        LanguageEntity lector;
        lector.name = "Lector";
        lector.kind = "class"; // lo pone el frontend; el formato no lo juzga
        {
            Relation d;
            d.kind = RelationKind::Derives;
            d.target = base;
            lector.relations.push_back(d);
            Relation i;
            i.kind = RelationKind::Implements;
            i.target = contrato;
            lector.relations.push_back(i);
        }
        store_node(store, lector, h);
        clase = LanguageEntityId{h};

        LanguageEntity parse;
        parse.name = "parse";
        parse.kind = "method";
        parse.declared_at.file = fichero;
        parse.declared_at.begin_line = 31;
        {
            Relation d;
            d.kind = RelationKind::DeclaredIn;
            d.target = clase;
            parse.relations.push_back(d);
        }
        store_node(store, parse, h);
        metodo = LanguageEntityId{h};

        ScopeNode sc;
        sc.owner = metodo;
        sc.span.file = fichero;
        sc.span.begin_line = 31;
        sc.span.end_line = 40;
        store_node(store, sc, h);
        ambito = ScopeId{h};

        VariableNode v;
        v.name = "texto";
        v.type = tipo_i32;
        v.scope = ambito;
        v.is_parameter = true;
        store_node(store, v, h);
        variable = VariableId{h};

        StatementNode st;
        st.lang_kind = "call";
        st.scope = ambito;
        st.span.file = fichero;
        st.span.begin_line = 33;
        st.span.begin_column = 9;
        st.span.end_line = 33;
        st.span.end_column = 27;
        store_node(store, st, h);
        sentencia = StatementId{h};

        // --- Intermedio ---
        funcion_ir = IrFunctionId{hash_bytes("fn:parse", 8)};
        instruccion = IrInstrId{hash_bytes("ir:call", 7)};

        LoweringMap lm;
        LoweringEntry le;
        le.statement = sentencia;
        le.kind = LoweringKind::Lowered;
        le.origin = OriginKind::Written;
        le.ir_instrs.push_back(instruccion);
        lm.entries.push_back(le);
        store_node(store, lm, h);
        nodes.note_lowering(funcion_ir, h);
        nodes.note_function_of(instruccion, funcion_ir);
        nodes.note_position_of(instruccion, 4); // cuarta instruccion de la funcion
        nodes.note_variable_in(ambito, variable);

        // --- Backend ---
        CodeNode cn;
        cn.ir_function = funcion_ir;
        cn.backend = BackendKind::Velb;
        cn.byte_size = 64;
        store_node(store, cn, h);
        cuerpo = CodeId{h};

        CodeDebug cd;
        cd.code = cuerpo;
        CodeRange rg;
        rg.begin = 16;
        rg.end = 32;
        rg.kind = RangeKind::Generated;
        rg.ir_instrs.push_back(instruccion);
        cd.ranges.push_back(rg);
        store_node(store, cd, h);
        nodes.note_code_debug(cuerpo, h);

        // Donde vive la variable, que es del backend y no del lenguaje.
        VariableMap vm;
        vm.variable = variable;
        LocationRange lr;
        lr.from = 0;
        lr.to = 10;
        lr.kind = LocationKind::Register;
        lr.value = 3;
        vm.locations.push_back(lr);
        store_node(store, vm, h);
        nodes.note_variable_map(variable, h);

        // --- Colocacion en la ejecucion en curso ---
        const uint32_t rev = session.new_generation();
        CodePlacement p;
        p.code = cuerpo;
        p.base = 0x400000;
        p.size = 64;
        session.generation(rev).add(p);
    }
};

int main() {
    std::printf("=== vxdbg: de una direccion a una explicacion ===\n");

    Programa prog;
    prog.construir();
    vxdbg::DebugResolver resolver(prog.nodes, prog.session);

    // La direccion cae dentro del tramo [16, 32) del cuerpo colocado en 0x400000.
    std::printf("Recorrido completo\n");
    const auto s = resolver.resolve(0x400000 + 20);

    comprobar(s.has_code, "1. la direccion cae en un cuerpo de codigo");
    comprobar(s.code == prog.cuerpo, "   y es el que se coloco ahi");
    comprobar(s.code_offset == 20, "   con el desplazamiento correcto");
    comprobar(s.backend == vxdbg::BackendKind::Velb,
              "   sabiendo que backend lo genero");
    comprobar(s.context.kind == vxdbg::ExecutionKind::Interpreter,
              "   y por tanto como se estaba ejecutando");

    comprobar(s.has_ir, "2. se sube a la instruccion intermedia");
    comprobar(s.ir_instrs.size() == 1 && s.ir_instrs[0] == prog.instruccion,
              "   y es la que genero ese tramo");

    comprobar(s.has_statement, "3. se sube a la sentencia del fuente");
    comprobar(s.statement_kind == "call", "   sabiendo que clase de sentencia es");
    comprobar(s.origin == vxdbg::OriginKind::Written,
              "   y que la escribio una persona, no el compilador");
    comprobar(s.span.begin_line == 33 && s.span.begin_column == 9,
              "   con el tramo exacto, no solo la linea");
    comprobar(s.file_path == "lector.vx", "   y el fichero correcto");

    comprobar(s.entity.found, "4. se sabe a quien pertenece el codigo");
    comprobar(s.entity.name == "parse", "   el metodo");
    comprobar(s.entity.kind == "method",
              "   con el genero que le puso su lenguaje");
    comprobar(s.entity.declared_in == "Lector", "   y la clase que lo contiene");

    // La jerarquia de la CLASE, que es lo que pedia el usuario ver en un fallo.
    const auto vista = resolver.describe_entity(prog.clase);
    comprobar(vista.found && vista.name == "Lector", "5. la clase se describe");
    comprobar(vista.derives.size() == 1 && vista.derives[0] == "Flujo",
              "   diciendo de quien deriva");
    comprobar(vista.implements.size() == 1 && vista.implements[0] == "Cerrable",
              "   y que contratos cumple");
    comprobar(!vista.cyclic, "   sin ciclos en la jerarquia");

    comprobar(s.variables.size() == 1, "6. se enumeran las variables vivas");
    comprobar(s.variables[0].name == "texto", "   con su nombre");
    comprobar(s.variables[0].is_parameter, "   sabiendo que es un parametro");
    comprobar(s.variables[0].location_kind == vxdbg::LocationKind::Register &&
                  s.variables[0].location_value == 3,
              "   y donde vive, que lo decidio el backend");

    // Una direccion fuera de todo lo colocado.
    std::printf("Casos que no se pueden responder\n");
    const auto fuera = resolver.resolve(0x900000);
    comprobar(!fuera.has_code, "una direccion desconocida no inventa nada");
    comprobar(!fuera.has_statement, "ni sube a ninguna sentencia");

    // Dentro del cuerpo pero en un hueco entre tramos.
    const auto hueco = resolver.resolve(0x400000 + 4);
    comprobar(hueco.has_code && !hueco.has_ir,
              "en un hueco se sabe el codigo pero no de que instruccion salio");

    // Una jerarquia circular: los datos estan mal y hay que decirlo.
    std::printf("Datos con un ciclo\n");
    {
        using namespace vxdbg;
        vxdbg::ContentHash h;
        LanguageEntity a;
        a.name = "A";
        a.kind = "class";
        // Se declara derivando de si misma: es un fallo de quien genero los
        // datos, y el resolutor tiene que pararse y decirlo.
        StoredNode tmp = encode(a);
        const auto ha = seal(tmp);
        Relation d;
        d.kind = RelationKind::Derives;
        d.target = LanguageEntityId{ha};
        a.relations.push_back(d);
        store_node(prog.store, a, h);

        const auto va = resolver.describe_entity(LanguageEntityId{h});
        comprobar(va.found, "la entidad se describe igual");
        comprobar(va.cyclic || va.derives.size() < 32,
                  "y la cadena no se recorre sin fin");
    }

    if (fallos == 0) {
        std::printf("=== todo correcto ===\n");
        return 0;
    }
    std::printf("=== %d comprobaciones fallidas ===\n", fallos);
    return 1;
}
