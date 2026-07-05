/**
 * @file test_incremental.cpp
 * @brief Tests del driver incremental granular: claves Merkle + CAS +
 *        plan_rebuild (ver vx/incremental.h).
 */
#include "vx/incremental.h"
#include "vx/semantic_index.h"

#include "ir/ssa_ir.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace vx;

static int g_checks = 0;
static int g_fail = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_fail;                                                           \
            std::printf("  FAIL: %s (linea %d)\n", (msg), __LINE__);            \
        }                                                                       \
    } while (0)

// Construye un SymbolEntry sintetico.  content_hash = hash del "cuerpo" para
// simular contenido; deps = nombres simples referidos.
static SymbolEntry sym(const std::string &qname, uint64_t content,
                       std::vector<std::string> deps) {
    SymbolEntry s;
    s.name = qname;
    s.kind = 0;
    s.content_hash = content;
    s.deps = std::move(deps);
    s.is_public = true;
    return s;
}

static SemanticIndex make_index(std::vector<SymbolEntry> syms) {
    SemanticIndex idx;
    idx.module_path = "test";
    idx.symbols = std::move(syms);
    return idx;
}

// A depende de B; B es hoja.  (grafo lineal para las propiedades basicas)
static SemanticIndex sample_AB(uint64_t a_content, uint64_t b_content) {
    return make_index({
        sym("m.A", a_content, {"B"}), // A usa B
        sym("m.B", b_content, {}),    // hoja
    });
}

int main() {
    std::printf("=== test_incremental ===\n");

    // -- 1. Determinismo: mismo indice -> mismas claves, sin importar orden ---
    {
        SemanticIndex i1 = make_index({
            sym("m.A", 111, {"B", "C"}),
            sym("m.B", 222, {"C"}),
            sym("m.C", 333, {}),
        });
        SemanticIndex i2 = make_index({ // mismo grafo, orden distinto.
            sym("m.C", 333, {}),
            sym("m.A", 111, {"B", "C"}),
            sym("m.B", 222, {"C"}),
        });
        MerkleKeys k1 = compute_merkle_keys(i1);
        MerkleKeys k2 = compute_merkle_keys(i2);
        CHECK(k1.of("m.A") == k2.of("m.A"), "det A");
        CHECK(k1.of("m.B") == k2.of("m.B"), "det B");
        CHECK(k1.of("m.C") == k2.of("m.C"), "det C");
        CHECK(k1.of("m.A") != 0 && k1.of("m.B") != 0 && k1.of("m.C") != 0,
              "claves no nulas");
        // Claves distintas entre simbolos distintos.
        CHECK(k1.of("m.A") != k1.of("m.B"), "A != B");
        CHECK(k1.of("m.B") != k1.of("m.C"), "B != C");
    }

    // -- 2. Propagacion Merkle: cambiar B cambia B y su dependiente A ---------
    {
        MerkleKeys k0 = compute_merkle_keys(sample_AB(111, 222));
        MerkleKeys k1 = compute_merkle_keys(sample_AB(111, 999)); // B cambia
        CHECK(k1.of("m.B") != k0.of("m.B"), "B cambia de clave");
        CHECK(k1.of("m.A") != k0.of("m.A"),
              "A (dependiente) cambia por B");
    }

    // -- 3. Independencia: cambiar A no cambia B (que no depende de A) --------
    {
        MerkleKeys k0 = compute_merkle_keys(sample_AB(111, 222));
        MerkleKeys k1 = compute_merkle_keys(sample_AB(555, 222)); // A cambia
        CHECK(k1.of("m.A") != k0.of("m.A"), "A cambia de clave");
        CHECK(k1.of("m.B") == k0.of("m.B"),
              "B (independiente) NO cambia por A");
    }

    // -- 4. Reuso cross-proyecto: dos indices con la MISMA stdlib -> misma ----
    //       clave (misma identidad de contenido) aunque el resto difiera.
    {
        // "stdlib": util depende de core; ambos identicos en los 2 proyectos.
        auto stdlib = [] {
            return std::vector<SymbolEntry>{
                sym("std.util", 0xABCD, {"core"}),
                sym("std.core", 0x1234, {}),
            };
        };
        SemanticIndex projA = make_index([&] {
            auto v = stdlib();
            v.push_back(sym("app.main", 0xAAAA, {"util"}));
            return v;
        }());
        SemanticIndex projB = make_index([&] {
            auto v = stdlib();
            v.push_back(sym("app.otra", 0xBBBB, {"core"}));
            v.push_back(sym("app.mas", 0xCCCC, {"util", "otra"}));
            return v;
        }());
        MerkleKeys ka = compute_merkle_keys(projA);
        MerkleKeys kb = compute_merkle_keys(projB);
        CHECK(ka.of("std.core") == kb.of("std.core"),
              "std.core misma clave cross-proyecto");
        CHECK(ka.of("std.util") == kb.of("std.util"),
              "std.util misma clave cross-proyecto");
        CHECK(ka.of("app.main") != kb.of("app.otra"),
              "codigo de app difiere");
    }

    // -- 5. Ciclos (SCC): A<->B mutuamente recursivos; cambiar A cambia ambos -
    {
        auto cyc = [](uint64_t a, uint64_t b) {
            return make_index({
                sym("m.A", a, {"B"}),
                sym("m.B", b, {"A"}),
                sym("m.C", 700, {"A"}), // depende del ciclo.
            });
        };
        MerkleKeys k0 = compute_merkle_keys(cyc(1, 2));
        MerkleKeys k1 = compute_merkle_keys(cyc(9, 2)); // solo A cambia
        CHECK(k0.of("m.A") != 0 && k0.of("m.B") != 0, "ciclo tiene claves");
        CHECK(k1.of("m.A") != k0.of("m.A"), "A cambia");
        CHECK(k1.of("m.B") != k0.of("m.B"),
              "B (mismo ciclo) cambia con A");
        CHECK(k1.of("m.C") != k0.of("m.C"),
              "C (dependiente del ciclo) cambia");
        CHECK(k0.of("m.A") != k0.of("m.B"),
              "miembros del ciclo tienen claves distintas");
    }

    // -- 6. CAS: put/get/has roundtrip + atomicidad idempotente --------------
    {
        const std::string root =
            (fs::temp_directory_path() / "vx_cas_test").string();
        std::error_code ec;
        fs::remove_all(root, ec);
        CasStore cas(root);
        const MerkleKey k = 0x0123456789ABCDEFull;
        CHECK(!cas.has(k), "clave ausente al inicio");
        std::vector<uint8_t> data = {1, 2, 3, 4, 5, 0, 7};
        CHECK(cas.put(k, data), "put ok");
        CHECK(cas.has(k), "has tras put");
        std::vector<uint8_t> got;
        CHECK(cas.get(k, got), "get ok");
        CHECK(got == data, "roundtrip byte-exacto (incl. nul)");
        CHECK(cas.put(k, data), "put idempotente ok");
        // Artefacto vacio.
        const MerkleKey k2 = 0xFFFFFFFFFFFFFFFFull;
        CHECK(cas.put(k2, nullptr, 0), "put vacio ok");
        std::vector<uint8_t> empty;
        CHECK(cas.get(k2, empty) && empty.empty(), "get vacio ok");
        fs::remove_all(root, ec);
    }

    // -- 7. plan_rebuild: particiona {reuse, recompile} segun el CAS ---------
    {
        const std::string root =
            (fs::temp_directory_path() / "vx_cas_plan").string();
        std::error_code ec;
        fs::remove_all(root, ec);
        CasStore cas(root);
        SemanticIndex idx = make_index({
            sym("m.A", 111, {"B"}),
            sym("m.B", 222, {}),
            sym("m.C", 333, {}),
        });
        // Store vacio: todo recompila.
        RebuildPlan p0 = plan_rebuild(idx, cas);
        CHECK(p0.reuse.empty(), "plan inicial: nada que reusar");
        CHECK(p0.recompile.size() == 3, "plan inicial: 3 a recompilar");
        // Poblar el artefacto de B y volver a planear.
        std::vector<uint8_t> art = {0xB};
        CHECK(cas.put(p0.keys.of("m.B"), art), "poblar B");
        RebuildPlan p1 = plan_rebuild(idx, cas);
        CHECK(p1.reuse.size() == 1 && p1.reuse[0] == "m.B", "B reusado");
        CHECK(p1.recompile.size() == 2, "A y C a recompilar");
        // Editar B (nuevo content_hash) -> su clave cambia -> ya no reusa, y A
        // (dependiente) tampoco.
        SemanticIndex idx2 = make_index({
            sym("m.A", 111, {"B"}),
            sym("m.B", 999, {}), // editado
            sym("m.C", 333, {}),
        });
        RebuildPlan p2 = plan_rebuild(idx2, cas);
        CHECK(p2.reuse.empty(), "tras editar B: nada reusable (clave nueva)");
        fs::remove_all(root, ec);
    }

    // -- 8. Fragmentos de IR: extraer -> serializar -> CAS -> parse -> merge -
    //       reproduce el contenido de los literales por STR_LIT_ADDR + RAW_ASM.
    {
        auto B = [](const char *s) {
            return std::vector<uint8_t>(s, s + std::strlen(s));
        };
        auto blob_str = [](const ir::IrModule &m, uint64_t idx) {
            auto b = m.static_data.bytes_at(static_cast<size_t>(idx));
            return std::string(reinterpret_cast<const char *>(b.first),
                               b.second);
        };
        // Modulo origen: 3 literales, 2 funciones.
        ir::IrModule mod;
        const uint64_t iHello = mod.intern_static_data(B("hello"));
        const uint64_t iWorld = mod.intern_static_data(B("world"));
        const uint64_t iXyz = mod.intern_static_data(B("xyz"));

        // fn A: STR_LIT_ADDR hello, STR_LIT_ADDR world.
        ir::IrFunction fA;
        fA.name = "m.A";
        fA.ret_type = ir::IrType::I64;
        {
            ir::IrBlock bb;
            bb.name = "entry";
            ir::IrInstr s1{};
            s1.op = ir::IrOp::STR_LIT_ADDR;
            s1.imm = iHello;
            bb.instrs.push_back(s1);
            ir::IrInstr s2{};
            s2.op = ir::IrOp::STR_LIT_ADDR;
            s2.imm = iWorld;
            bb.instrs.push_back(s2);
            fA.blocks.push_back(std::move(bb));
        }
        // fn B: STR_LIT_ADDR world + RAW_ASM que referencia code.s_<xyz>.
        ir::IrFunction fB;
        fB.name = "m.B";
        fB.ret_type = ir::IrType::I64;
        {
            ir::IrBlock bb;
            bb.name = "entry";
            ir::IrInstr s1{};
            s1.op = ir::IrOp::STR_LIT_ADDR;
            s1.imm = iWorld;
            bb.instrs.push_back(s1);
            ir::IrInstr asm1{};
            asm1.op = ir::IrOp::RAW_ASM;
            asm1.func_name = "mov r0, code.s_" + std::to_string(iXyz) + "\n";
            bb.instrs.push_back(asm1);
            fB.blocks.push_back(std::move(bb));
        }
        mod.functions.push_back(fA);
        mod.functions.push_back(fB);

        // Extraer fragmentos, pasar por el CAS (serializar/parsear) y re-montar.
        const std::string root =
            (fs::temp_directory_path() / "vx_cas_frag").string();
        std::error_code ec;
        fs::remove_all(root, ec);
        CasStore cas(root);

        ir::IrModule reasm; // modulo reensamblado desde fragmentos.
        for (const auto &fn : mod.functions) {
            IrFragment frag = extract_ir_fragment(mod, fn);
            // Los indices de la fn del fragmento son LOCALES (0..k-1).
            CHECK(frag.blobs.size() >= 1, "fragmento con blobs");
            std::vector<uint8_t> bytes = serialize_ir_fragment(frag);
            // Guardar/leer del CAS con una clave arbitraria (aqui hash del nombre).
            const MerkleKey k = static_cast<MerkleKey>(
                std::hash<std::string>{}(fn.name) | 1ull);
            CHECK(cas.put(k, bytes), "put fragmento");
            std::vector<uint8_t> got;
            CHECK(cas.get(k, got) && got == bytes, "get fragmento byte-exacto");
            IrFragment parsed;
            CHECK(parse_ir_fragment(got, parsed), "parse fragmento");
            merge_ir_fragment(reasm, parsed);
        }
        fs::remove_all(root, ec);

        // Verificar: el modulo reensamblado tiene A y B, y sus literales
        // resuelven al mismo CONTENIDO que el original.
        CHECK(reasm.functions.size() == 2, "reensamblado: 2 funciones");
        auto find_fn = [&](const std::string &nm) -> const ir::IrFunction * {
            for (const auto &f : reasm.functions)
                if (f.name == nm) return &f;
            return nullptr;
        };
        const ir::IrFunction *rA = find_fn("m.A");
        const ir::IrFunction *rB = find_fn("m.B");
        CHECK(rA && rB, "A y B presentes tras merge");
        if (rA) {
            std::vector<std::string> lits;
            for (const auto &bb : rA->blocks)
                for (const auto &ins : bb.instrs)
                    if (ins.op == ir::IrOp::STR_LIT_ADDR)
                        lits.push_back(blob_str(reasm, ins.imm));
            CHECK(lits.size() == 2, "A: 2 STR_LIT_ADDR");
            CHECK(lits.size() == 2 && lits[0] == "hello" && lits[1] == "world",
                  "A: literales hello/world correctos");
        }
        if (rB) {
            std::string lit, asm_txt;
            for (const auto &bb : rB->blocks)
                for (const auto &ins : bb.instrs) {
                    if (ins.op == ir::IrOp::STR_LIT_ADDR)
                        lit = blob_str(reasm, ins.imm);
                    if (ins.op == ir::IrOp::RAW_ASM) asm_txt = ins.func_name;
                }
            CHECK(lit == "world", "B: STR_LIT_ADDR world correcto");
            // El RAW_ASM debe referir code.s_<idx> cuyo contenido es "xyz".
            const size_t p = asm_txt.find("code.s_");
            CHECK(p != std::string::npos, "B: RAW_ASM tiene code.s_");
            if (p != std::string::npos) {
                uint64_t idx = std::strtoull(asm_txt.c_str() + p + 7, nullptr, 10);
                CHECK(blob_str(reasm, idx) == "xyz",
                      "B: code.s_ resuelve a xyz");
            }
        }
    }

    // -- 9. BuildConfig: keying por configuracion (layered) ------------------
    {
        BuildConfig base; // defaults.
        const uint64_t ir0 = base.ir_fingerprint();
        const uint64_t full0 = base.full_fingerprint();
        CHECK(ir0 != 0 && full0 != 0, "fingerprints no nulos");
        CHECK(ir0 != full0, "ir_fp != full_fp");

        // Config identica -> mismos fingerprints (determinismo).
        BuildConfig same;
        CHECK(same.ir_fingerprint() == ir0 && same.full_fingerprint() == full0,
              "config identica -> mismos fp");

        // Dimensiones que CAMBIAN el IR pre-optimize -> cambian ir_fp Y full_fp.
        {
            BuildConfig c = base;
            c.asm_target_bits = 32;
            CHECK(c.ir_fingerprint() != ir0, "asm_bits cambia ir_fp");
            CHECK(c.full_fingerprint() != full0, "asm_bits cambia full_fp");
        }
        {
            BuildConfig c = base;
            c.native_poo = true;
            CHECK(c.ir_fingerprint() != ir0, "native_poo cambia ir_fp");
        }
        {
            BuildConfig c = base;
            c.exceptions_enabled = false;
            CHECK(c.ir_fingerprint() != ir0, "exceptions cambia ir_fp");
        }
        {
            BuildConfig c = base;
            c.instrument_mode = "trace";
            CHECK(c.ir_fingerprint() != ir0, "instrument cambia ir_fp");
        }

        // Dimensiones POST-merge -> NO cambian ir_fp, PERO SI full_fp.
        {
            BuildConfig c = base;
            c.opt_level = 0;
            CHECK(c.ir_fingerprint() == ir0,
                  "opt_level NO cambia ir_fp (post-merge)");
            CHECK(c.full_fingerprint() != full0, "opt_level cambia full_fp");
        }
        {
            BuildConfig c = base;
            c.aot_vec_width = 32;
            CHECK(c.ir_fingerprint() == ir0,
                  "aot_vec_width NO cambia ir_fp (post-opt)");
            CHECK(c.full_fingerprint() != full0, "aot_vec_width cambia full_fp");
        }
        {
            BuildConfig c = base;
            c.emit_debug = true;
            CHECK(c.ir_fingerprint() == ir0,
                  "emit_debug NO cambia ir_fp (solo emit final)");
            CHECK(c.full_fingerprint() != full0, "emit_debug cambia full_fp");
        }
        {
            BuildConfig c = base;
            c.profile_id = "pgo-v1";
            CHECK(c.ir_fingerprint() == ir0, "profile NO cambia ir_fp");
            CHECK(c.full_fingerprint() != full0, "profile cambia full_fp");
        }
    }

    if (g_fail == 0)
        std::printf("=== test_incremental: %d checks OK, 0 fallidos ===\n",
                    g_checks);
    else
        std::printf("=== test_incremental: %d checks, %d FALLIDOS ===\n",
                    g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
