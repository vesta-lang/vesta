/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file diagram_labels.h
 * @brief Como se LEE en un diagrama una expresion, un tipo, un valor o una
 *        instruccion del IR.
 *
 * Es la otra mitad del reparto que empezo @c vel_text_model.h: lo que se dice
 * de una cosa no depende del formato en el que se dibuje.  Un `add.i64 %3, %4`
 * se lee igual en DOT que en Mermaid; lo unico propio de cada uno es como se
 * escapa y con que sintaxis se emite el nodo.
 *
 * Estas cuatro estaban escritas dos veces, identicas byte a byte -- ciento
 * cuarenta y cuatro lineas --, y las dos grandes son justo las que mas van a
 * crecer: describir una instruccion del IR cambia cada vez que se anade una
 * operacion, y describir un tipo cada vez que el lenguaje gana una forma.
 * Escritas dos veces, la segunda copia se queda corta el dia que nadie mire.
 */

#ifndef VX_DIAGRAM_LABELS_H
#define VX_DIAGRAM_LABELS_H

#include "ir/ssa_ir.h"
#include "vx/ast.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace vx {

/**
 * @brief Texto de una expresion del AST.
 *
 * Sin truncar ni acotar la profundidad: en un diagrama se quiere ver la
 * expresion entera.
 *
 * @param e Expresion, o nullptr.
 * @return Su texto, o "?" si no hay.
 */
std::string fmt_expr(const ast::Expr *e);

/**
 * @brief Texto de una expresion, acotado en profundidad.
 *
 * Cada expresion se reduce a una linea con lo justo para entender el flujo sin
 * abrir el fuente: los operadores mantienen su simbolo, las llamadas muestran
 * nombre y numero de argumentos, y los literales su valor.
 *
 * @param e     Expresion, o nullptr.
 * @param depth Cuantos niveles quedan por bajar.
 * @return Su texto.
 */
std::string fmt_expr_brief(const ast::Expr *e, int depth);

/**
 * @brief Texto de un tipo declarado.
 *
 * @param tn Nodo de tipo, o nullptr.
 * @return Su texto, o "?" si no hay.
 */
std::string fmt_type_helper(const ast::TypeNode *tn);

/**
 * @brief Texto de un tipo declarado, envoltorio de @c fmt_type_helper.
 *
 * @param tn Nodo de tipo, o nullptr.
 * @return Su texto.
 */
std::string fmt_type(const ast::TypeNode *tn);

/**
 * @brief Nombre de un valor SSA: el que le puso quien lo creo, o su numero.
 *
 * @param fn Funcion a la que pertenece.
 * @param id Identificador del valor.
 * @return Su nombre.
 */
std::string fmt_value_id(const ir::IrFunction &fn, ir::IrValueId id);

/**
 * @brief Texto de UNA instruccion del IR, en una linea.
 *
 * Los saltos se escriben con el NOMBRE del bloque destino y no con su numero,
 * que es lo que hace legible un diagrama de flujo.
 *
 * @param fn     Funcion a la que pertenece.
 * @param ins    Instruccion.
 * @param blocks Bloques de la funcion, para poder nombrar los destinos.
 * @return Su texto.
 */
std::string fmt_instr(const ir::IrFunction &fn, const ir::IrInstr &ins,
                      const std::vector<ir::IrBlock> &blocks);

/**
 * @brief En que posicion declara el modulo cada clase, por su nombre.
 *
 * PARA PODER APUNTAR A LA CLASE DE VERDAD.  Al dibujar la herencia, el padre se
 * pintaba SIEMPRE como un nodo aparte y discontinuo -- el estilo de "esto viene
 * de fuera" --, tambien cuando estaba declarado tres lineas mas arriba.  Asi la
 * misma clase sale dos veces y la jerarquia no queda unida: dos clases en el
 * mismo dibujo, sin una arista entre ellas, que es justo lo que un diagrama de
 * herencia existe para ensenar.
 *
 * Con esto el `extends` apunta al nodo real cuando la clase esta aqui, y solo
 * cae al nodo de referencia cuando de verdad viene de otro modulo.
 *
 * Los dos formatos numeran las clases igual -- en orden de declaracion --, asi
 * que lo que se comparte es el NUMERO y cada uno le pone su prefijo.
 *
 * @param mod El modulo.
 * @return Nombre de la clase -> su posicion entre las clases del modulo.
 */
std::unordered_map<std::string, uint32_t>
class_index_by_name(const ast::ModuleNode &mod);

/**
 * @brief De quien hereda una clase, separando lo que esta AQUI de lo de fuera.
 *
 * La decision es la misma en los dos formatos -- a que nodo apunta cada base --
 * y lo unico propio de cada uno es con que sintaxis se dibuja la arista, asi
 * que se decide una vez y se dibuja dos.
 */
struct ClassBases {
    uint32_t super = 0;          ///< Posicion de la clase base en el modulo.
    bool super_is_local = false; ///< ...o si viene de otro modulo.
    std::vector<uint32_t> local_ifaces;       ///< Interfaces declaradas aqui.
    std::vector<const std::string *> foreign; ///< Y las que vienen de fuera.
};

/**
 * @brief Resuelve las bases de una clase contra las del modulo.
 *
 * @param cd      La clase.
 * @param classes Indice de @c class_index_by_name.
 * @return Que bases estan aqui y cuales no.
 */
ClassBases
class_bases(const ast::ClassDecl &cd,
            const std::unordered_map<std::string, uint32_t> &classes);

/* ------------------------------------------------------------------------ *
 *  QUE SE DICE DE UNA DECLARACION
 *
 *  Lo de arriba, aplicado a lo que mas texto ocupa de un diagrama del AST: el
 *  titulo de una clase, la firma de un metodo, la linea de un campo.  Estaba
 *  escrito DOS veces -- trescientas sesenta y cinco lineas identicas entre los
 *  dos generadores -- y es lo que cambia cada vez que el lenguaje gana un
 *  modificador: la segunda copia se queda corta el dia que nadie mire, y un
 *  diagrama que omite `final` no parece roto, parece que la clase no es final.
 * ------------------------------------------------------------------------ */

/**
 * @brief Cuantas sentencias tiene un cuerpo, contando las de dentro.
 *
 * @param s Sentencia raiz, o nullptr.
 * @return Cuantas hay.  Un bloque vacio cuenta como una.
 */
size_t count_stmts(const ast::Stmt *s);

/**
 * @brief La lista de parametros de una firma: `i32 a, string b`.
 *
 * @param params Los parametros.
 * @return Su texto, sin los parentesis.
 */
std::string
fmt_params(const std::vector<std::unique_ptr<ast::ParamDecl>> &params);

/**
 * @brief Firma de una funcion libre: `@Async fn nombre(i32 a) -> i32`.
 *
 * @param fd La funcion.
 * @return Su firma.
 */
std::string fmt_function_signature(const ast::FunctionDecl &fd);

/**
 * @brief Que hay en el cuerpo, para la segunda linea del nodo.
 *
 * El prefijo y el sufijo los pone quien llama porque cada vista dice lo suyo --
 * Mermaid anade "(ver subgraph)" y DOT no --; lo compartido es contar y el caso
 * de que no haya cuerpo, que es donde se equivocaria una segunda copia.
 *
 * @param body       El cuerpo, o nullptr.
 * @param prefix     Lo que va delante de la cuenta (p.ej. "body: ").
 * @param suffix     Y lo que va detras (p.ej. " stmts").
 * @param when_empty Que decir si no hay cuerpo.
 * @return Su texto.
 */
std::string fmt_body_info(const ast::BlockStmt *body, const char *prefix,
                          const char *suffix, const char *when_empty);

/**
 * @brief Titulo de una clase: `final class Foo<T>`, `interface Bar`.
 *
 * @param cd La clase.
 * @return Su titulo.
 */
std::string fmt_class_title(const ast::ClassDecl &cd);

/**
 * @brief Cuantos campos y metodos tiene una clase.
 *
 * @param cd La clase.
 * @return `N fields, M methods`.
 */
std::string fmt_class_summary(const ast::ClassDecl &cd);

/**
 * @brief Linea de un campo de clase: `private static final i32 x`.
 *
 * @param f El campo.
 * @return Su linea.
 */
std::string fmt_field_line(const ast::ClassFieldDecl &f);

/**
 * @brief Firma de un metodo, con acceso, modificadores y aspectos.
 *
 * @param m El metodo.
 * @return Su firma.
 */
std::string fmt_method_signature(const ast::ClassMethodDecl &m);

/**
 * @brief Titulo de un struct: `struct Punto (2 fields)`.
 *
 * @param sd El struct.
 * @return Su titulo.
 */
std::string fmt_struct_title(const ast::StructDecl &sd);

/**
 * @brief Linea de un campo de struct, con su ancho en bits si lo tiene.
 *
 * @param f El campo.
 * @return Su linea.
 */
std::string fmt_struct_field_line(const ast::StructFieldDecl &f);

/**
 * @brief Titulo de un enum: `enum Color (3 variants)`.
 *
 * @param ed El enum.
 * @return Su titulo.
 */
std::string fmt_enum_title(const ast::EnumDecl &ed);

/**
 * @brief Linea de una variante, con su carga si la lleva: `Some(i32)`.
 *
 * @param v La variante.
 * @return Su linea.
 */
std::string fmt_enum_variant_line(const ast::EnumVariantDecl &v);

/**
 * @brief Linea de una global: `const i32 N = 10`.
 *
 * @param gv La global.
 * @return Su linea.
 */
std::string fmt_global_line(const ast::GlobalVarDecl &gv);

/**
 * @brief Linea de una funcion externa: `i32 MessageBoxA(u64 h, ...)`.
 *
 * @param ef La externa.
 * @return Su linea.
 */
std::string fmt_extern_line(const ast::ExternFnDecl &ef);

/**
 * @brief De que CLASE es una sentencia hoja, para pintarla.
 *
 * El nombre del estilo es lo unico propio de cada formato -- una clase CSS en
 * Mermaid, una @c NodeStyle en DOT --, asi que lo que viaja es la clase y cada
 * uno la traduce con una tabla plana indexada por este enum.
 */
enum class StmtStyle : uint8_t {
    None = 0, ///< No es fusionable.
    Var,      ///< Declaracion de variable.
    Aux,      ///< Sin efecto (un `;` suelto).
    Call,     ///< Llamada o creacion.
    Spawn,    ///< Lanzar un proceso.
    Assign,   ///< Asignacion.
    Expr,     ///< Cualquier otra expresion.
    Count     ///< Cuantas hay: el tamano de la tabla de cada formato.
};

/**
 * @brief Si una sentencia puede fundirse con las de al lado, y como se lee.
 *
 * Las hojas sin estructura propia -- declaraciones, expresiones -- que van
 * seguidas se dibujan en UN nodo de varias lineas, que es lo que hace legible
 * el cuerpo de una funcion larga.  Solo funden las del mismo @c group; una
 * sentencia de control no funde y deja el grupo vacio.
 */
struct StmtFusion {
    const char *group = nullptr;       ///< nullptr = no funde.
    std::string line;                  ///< Como se lee, en una linea.
    StmtStyle style = StmtStyle::None; ///< Con que estilo se pinta.
};

/**
 * @brief Clasifica una sentencia para la fusion.
 *
 * @param s La sentencia, o nullptr.
 * @return Su grupo, su texto y su estilo.
 */
StmtFusion classify_stmt_for_fusion(const ast::Stmt *s);

} // namespace vx

#endif // VX_DIAGRAM_LABELS_H
