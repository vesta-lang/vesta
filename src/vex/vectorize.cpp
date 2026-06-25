/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file vectorize.cpp
 * @brief Auto-vectorizacion del frontend Vex.
 *
 * Reconocimiento de idiomas de bucle (de momento la COPIA de elementos
 * `dst[i] = src[i]` en su forma @c while y @c for) y su bajada a operaciones
 * eficientes: un unico @c MEMCPY que el JIT/AOT materializan como @c rep
 * @c movsb / SIMD y el interprete como un bucle host->host.
 *
 * Estos son metodos de @c vex::Lowering (declarados en @c vex/lowering.h) pero
 * se definen en este TU separado para mantener la vectorizacion agrupada y
 * poder evolucionarla (unroll xW, fusion SIMD, memset, anchos AVX/AVX512) sin
 * inflar @c lowering.cpp ni dificultar un futuro split en mas archivos.
 *
 * Decision de diseno (host vs VM): el idioma solo se aplica sobre punteros
 * HOST (malloc/FFI).  El JIT baja @c MEMCPY a @c rep @c movsb (host->host) y el
 * interprete a un bucle host->host; sobre arrays VM-virtuales (@c T[N],
 * @c &local) eso seria incorrecto, asi que esos casos bailan y siguen como
 * bucle escalar normal.
 */

#include "vex/lowering.h"

#include "jit/vec_isa.h" // ancho del chunk vectorizado (SSE2/AVX2/AVX512)

#include <cstdio>
#include <cstdlib>

namespace vex {

// Helper file-local: true si @p e es un incremento de @p idx en 1, en
// cualquiera de las formas `idx++`, `++idx`, `idx = idx + 1`, `idx += 1`.
static bool mc_is_increment_expr(ast::Expr *e, const std::string &idx) {
    using namespace ast;
    if (!e) return false;
    if (e->kind == NodeKind::UnaryExpr) {
        auto *un = static_cast<UnaryExpr *>(e);
        return (un->op == UnOp::PostInc || un->op == UnOp::PreInc) &&
               un->operand && un->operand->kind == NodeKind::IdentExpr &&
               static_cast<IdentExpr *>(un->operand.get())->name == idx;
    }
    if (e->kind == NodeKind::AssignExpr) {
        auto *ia = static_cast<AssignExpr *>(e);
        if (!ia->target || ia->target->kind != NodeKind::IdentExpr) return false;
        if (static_cast<IdentExpr *>(ia->target.get())->name != idx)
            return false;
        if (ia->op == AssignOp::AddAssign) { // idx += 1
            return ia->value && ia->value->kind == NodeKind::IntLitExpr &&
                   static_cast<IntLitExpr *>(ia->value.get())->value == 1;
        }
        if (ia->op == AssignOp::Assign && ia->value && // idx = idx + 1
            ia->value->kind == NodeKind::BinaryExpr) {
            auto *add = static_cast<BinaryExpr *>(ia->value.get());
            return add->op == BinOp::Add && add->lhs &&
                   add->lhs->kind == NodeKind::IdentExpr &&
                   static_cast<IdentExpr *>(add->lhs.get())->name == idx &&
                   add->rhs && add->rhs->kind == NodeKind::IntLitExpr &&
                   static_cast<IntLitExpr *>(add->rhs.get())->value == 1;
        }
    }
    return false;
}

bool Lowering::mc_match_copy_assign(ast::AssignExpr *asg,
                                    const std::string &idx_name,
                                    ast::IdentExpr **out_dst,
                                    ast::IdentExpr **out_src, size_t *out_esz) {
    using namespace ast;
    if (!asg || asg->op != AssignOp::Assign) return false;
    if (!asg->target || asg->target->kind != NodeKind::IndexExpr) return false;
    if (!asg->value || asg->value->kind != NodeKind::IndexExpr) return false;
    auto *dst_ix = static_cast<IndexExpr *>(asg->target.get());
    auto *src_ix = static_cast<IndexExpr *>(asg->value.get());
    // Sin overloads de operador ni slices/ranges.
    if (!dst_ix->overload_method.empty() ||
        !dst_ix->index_set_method.empty() || dst_ix->is_range)
        return false;
    if (!src_ix->overload_method.empty() || src_ix->is_range) return false;
    // base = ident; index = el MISMO idx del loop en ambos.
    if (!dst_ix->base || dst_ix->base->kind != NodeKind::IdentExpr) return false;
    if (!src_ix->base || src_ix->base->kind != NodeKind::IdentExpr) return false;
    if (!dst_ix->index || dst_ix->index->kind != NodeKind::IdentExpr)
        return false;
    if (!src_ix->index || src_ix->index->kind != NodeKind::IdentExpr)
        return false;
    if (static_cast<IdentExpr *>(dst_ix->index.get())->name != idx_name)
        return false;
    if (static_cast<IdentExpr *>(src_ix->index.get())->name != idx_name)
        return false;
    auto *dst_base = static_cast<IdentExpr *>(dst_ix->base.get());
    auto *src_base = static_cast<IdentExpr *>(src_ix->base.get());
    // Tipos: ambos PTR/ARRAY HOST con pointee del MISMO tamano.
    const Type &dt = dst_base->result_type;
    const Type &stt = src_base->result_type;
    auto is_ptr_like = [](const Type &t) {
        return (t.kind == PrimitiveKind::PTR ||
                t.kind == PrimitiveKind::ARRAY) &&
               static_cast<bool>(t.pointee);
    };
    if (!is_ptr_like(dt) || !is_ptr_like(stt)) return false;
    if (dt.is_virtual || stt.is_virtual) return false; // solo punteros HOST
    const size_t esz = size_of_type(*dt.pointee);
    if (esz == 0 || size_of_type(*stt.pointee) != esz) return false;
    *out_dst = dst_base;
    *out_src = src_base;
    *out_esz = esz;
    return true;
}

bool Lowering::mc_emit_copy(ir::IrValueId v_idx, ast::Expr *limit,
                            ast::IdentExpr *dst_base, ast::IdentExpr *src_base,
                            size_t esz, uint32_t ln,
                            const std::string &idx_name_for_post) {
    if (v_idx == ir::IR_NO_VALUE) return false;
    const ir::IrType idx_ty = fn_->values[v_idx].type;
    // El indice debe ser un ENTERO valor.  Si es PTR (p.ej. la var del loop es
    // address-taken y su binding es la direccion del slot, no el valor) o un
    // host_ptr, bailar: usarlo como indice produciria basura.
    if (idx_ty == ir::IrType::PTR || idx_ty == ir::IrType::F32 ||
        idx_ty == ir::IrType::F64 || fn_->values[v_idx].is_host_ptr)
        return false;
    const ir::IrValueId base_dst = lower_expr(dst_base);
    const ir::IrValueId base_src = lower_expr(src_base);
    if (base_dst == ir::IR_NO_VALUE || base_src == ir::IR_NO_VALUE)
        return false;
    const ir::IrValueId v_lim = lower_expr(limit);
    if (v_lim == ir::IR_NO_VALUE) return false;

    // idx y limite a i64 para la aritmetica de tamano.
    const ir::IrValueId idx64 =
        cast_if_needed(v_idx, idx_ty, ir::IrType::I64, ln);
    const ir::IrValueId lim64 =
        cast_if_needed(v_lim, fn_->values[v_lim].type, ir::IrType::I64, ln);

    auto bin = [&](ir::IrOp op, ir::IrValueId a,
                   ir::IrValueId b) -> ir::IrValueId {
        const ir::IrValueId d = fn_->new_value(ir::IrType::I64);
        ir::IrInstr i{};
        i.op = op;
        i.type = ir::IrType::I64;
        i.dst = d;
        i.operands = {a, b};
        i.source_line = ln;
        fn_->append(current_block_, std::move(i));
        return d;
    };

    // diff = lim - idx;  count = diff & ~(diff >> 63)  (0 si diff<=0).
    const ir::IrValueId diff = bin(ir::IrOp::SUB, lim64, idx64);
    const ir::IrValueId sar =
        bin(ir::IrOp::SAR, diff, emit_const(ir::IrType::I64, 63, ln));
    const ir::IrValueId notmask = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr i{};
        i.op = ir::IrOp::NOT;
        i.type = ir::IrType::I64;
        i.dst = notmask;
        i.operands = {sar};
        i.source_line = ln;
        fn_->append(current_block_, std::move(i));
    }
    const ir::IrValueId count = bin(ir::IrOp::AND, diff, notmask);

    // bytes = count * esz;  byteoff = idx * esz.
    const ir::IrValueId v_esz = emit_const(ir::IrType::I64, (uint64_t)esz, ln);
    const ir::IrValueId bytes =
        (esz == 1) ? count : bin(ir::IrOp::MUL, count, v_esz);
    const ir::IrValueId byteoff =
        (esz == 1) ? idx64 : bin(ir::IrOp::MUL, idx64, v_esz);

    // dst_ptr = base_dst + byteoff;  src_ptr = base_src + byteoff (HOST).
    auto ptr_add = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        const ir::IrValueId d = fn_->new_value(ir::IrType::PTR);
        fn_->values[d].is_host_ptr = true; // verificado HOST en mc_match_copy
        ir::IrInstr i{};
        i.op = ir::IrOp::ADD;
        i.type = ir::IrType::PTR;
        i.dst = d;
        i.operands = {base, off};
        i.source_line = ln;
        fn_->append(current_block_, std::move(i));
        return d;
    };
    const ir::IrValueId dst_ptr = ptr_add(base_dst, byteoff);
    const ir::IrValueId src_ptr = ptr_add(base_src, byteoff);

    // MEMCPY dst_ptr, src_ptr, bytes  (rep movsb / SIMD en JIT/AOT; bucle
    // host->host en el interprete).
    {
        ir::IrInstr mc{};
        mc.op = ir::IrOp::MEMCPY;
        mc.type = ir::IrType::I8;
        mc.dst = ir::IR_NO_VALUE;
        mc.operands = {dst_ptr, src_ptr, bytes};
        mc.source_line = ln;
        fn_->append(current_block_, std::move(mc));
    }

    // idx post-loop = idx_init + count  (== N si corrio, == idx_init si no).
    // Solo para el while con idx EXTERNA; el for que declara la var en init la
    // tiene loop-local y no se observa tras el loop (idx_name_for_post vacio).
    if (!idx_name_for_post.empty()) {
        const ir::IrValueId post64 = bin(ir::IrOp::ADD, idx64, count);
        const ir::IrValueId post =
            cast_if_needed(post64, ir::IrType::I64, idx_ty, ln);
        update_scope(idx_name_for_post, post);
    }
    return true;
}

bool Lowering::try_lower_memcpy_idiom(ast::WhileStmt *s) {
    using namespace ast;
    static const bool MC_DBG = std::getenv("VESTA_MC_IDIOM_DEBUG") != nullptr;
    if (!s->cond || !s->body) return false;

    // cond = (idx < limit): idx ident, limit libre de efectos (1 evaluacion).
    if (s->cond->kind != NodeKind::BinaryExpr) return false;
    auto *cond = static_cast<BinaryExpr *>(s->cond.get());
    if (cond->op != BinOp::Lt) return false;
    if (!cond->lhs || cond->lhs->kind != NodeKind::IdentExpr) return false;
    if (!cond->rhs) return false;
    {
        const NodeKind lk = cond->rhs->kind;
        if (lk != NodeKind::IdentExpr && lk != NodeKind::IntLitExpr)
            return false;
    }
    const std::string idx_name =
        static_cast<IdentExpr *>(cond->lhs.get())->name;
    if (lookup(idx_name) == ir::IR_NO_VALUE) return false;

    // body = bloque con EXACTAMENTE 2 statements (la copia + el incremento).
    if (s->body->kind != NodeKind::BlockStmt) return false;
    auto *blk = static_cast<BlockStmt *>(s->body.get());
    if (blk->body.size() != 2) return false;
    if (!blk->body[0] || blk->body[0]->kind != NodeKind::ExprStmt) return false;
    if (!blk->body[1] || blk->body[1]->kind != NodeKind::ExprStmt) return false;
    auto *st0 = static_cast<ExprStmt *>(blk->body[0].get());
    auto *st1 = static_cast<ExprStmt *>(blk->body[1].get());
    if (!st0->expr || !st1->expr) return false;

    // stmt0 = dst[idx] = src[idx]; stmt1 = incremento de idx.
    if (st0->expr->kind != NodeKind::AssignExpr) return false;
    ast::IdentExpr *dst_base = nullptr, *src_base = nullptr;
    size_t esz = 0;
    if (!mc_match_copy_assign(static_cast<AssignExpr *>(st0->expr.get()),
                              idx_name, &dst_base, &src_base, &esz))
        return false;
    if (!mc_is_increment_expr(st1->expr.get(), idx_name)) return false;

    if (MC_DBG)
        std::fprintf(stderr, "[mc-idiom] MATCH while idx=%s esz=%zu\n",
                     idx_name.c_str(), esz);
    // idx es una var EXTERNA: su valor actual viene del scope; tras el loop hay
    // que escribir el idx post-loop (idx_name_for_post = idx_name).
    return mc_emit_copy(lookup(idx_name), cond->rhs.get(), dst_base, src_base,
                        esz, s->loc.line, /*idx_name_for_post=*/idx_name);
}

bool Lowering::try_lower_memcpy_idiom_for(ast::ForStmt *s) {
    using namespace ast;
    static const bool MC_DBG = std::getenv("VESTA_MC_IDIOM_DEBUG") != nullptr;
    if (!s->cond || !s->body || !s->init || !s->step) return false;

    // cond = (idx < limit).
    if (s->cond->kind != NodeKind::BinaryExpr) return false;
    auto *cond = static_cast<BinaryExpr *>(s->cond.get());
    if (cond->op != BinOp::Lt) return false;
    if (!cond->lhs || cond->lhs->kind != NodeKind::IdentExpr) return false;
    if (!cond->rhs) return false;
    {
        const NodeKind lk = cond->rhs->kind;
        if (lk != NodeKind::IdentExpr && lk != NodeKind::IntLitExpr)
            return false;
    }
    const std::string idx_name =
        static_cast<IdentExpr *>(cond->lhs.get())->name;

    // init DECLARA la var del loop con inicializador -> loop-local (sin
    // writeback de scope tras el loop).  Otras formas de init bailan (el
    // lower_for normal las maneja).
    if (s->init->kind != NodeKind::VarDeclStmt) return false;
    auto *vd = static_cast<VarDeclStmt *>(s->init.get());
    if (vd->name != idx_name || !vd->init) return false;

    // step = incremento de idx.
    if (!mc_is_increment_expr(s->step.get(), idx_name)) return false;

    // body = la copia dst[idx]=src[idx] (bloque de 1 stmt o ExprStmt directo).
    ast::Expr *copy_expr = nullptr;
    if (s->body->kind == NodeKind::ExprStmt) {
        copy_expr = static_cast<ExprStmt *>(s->body.get())->expr.get();
    } else if (s->body->kind == NodeKind::BlockStmt) {
        auto *b = static_cast<BlockStmt *>(s->body.get());
        if (b->body.size() != 1 || !b->body[0] ||
            b->body[0]->kind != NodeKind::ExprStmt)
            return false;
        copy_expr = static_cast<ExprStmt *>(b->body[0].get())->expr.get();
    } else {
        return false;
    }
    if (!copy_expr || copy_expr->kind != NodeKind::AssignExpr) return false;
    ast::IdentExpr *dst_base = nullptr, *src_base = nullptr;
    size_t esz = 0;
    if (!mc_match_copy_assign(static_cast<AssignExpr *>(copy_expr), idx_name,
                              &dst_base, &src_base, &esz))
        return false;

    if (MC_DBG)
        std::fprintf(stderr, "[mc-idiom] MATCH for idx=%s esz=%zu\n",
                     idx_name.c_str(), esz);

    // MATCH: la var del loop es loop-local y solo necesitamos su VALOR inicial
    // para los limites del memcpy; NO la declaramos (evita el caso
    // address-taken donde su binding seria la direccion del slot, no el valor).
    // Bajamos directamente el init.  Sin push_scope ni post-update.
    const ir::IrValueId v_init = lower_expr(vd->init.get());
    if (v_init == ir::IR_NO_VALUE) return false;
    return mc_emit_copy(v_init, cond->rhs.get(), dst_base, src_base, esz,
                        s->loc.line, /*idx_name_for_post=*/std::string());
}

bool Lowering::try_vectorize_elementwise_for(ast::ForStmt *s) {
    using namespace ast;
    static const bool MC_DBG = std::getenv("VESTA_MC_IDIOM_DEBUG") != nullptr;
    if (!s->cond || !s->body || !s->init || !s->step) return false;

    // --- estructura: for (T i = init; i < N; i++) c[i] = a[i] OP b[i]; ---
    if (s->cond->kind != NodeKind::BinaryExpr) return false;
    auto *cond = static_cast<BinaryExpr *>(s->cond.get());
    if (cond->op != BinOp::Lt) return false;
    if (!cond->lhs || cond->lhs->kind != NodeKind::IdentExpr) return false;
    if (!cond->rhs) return false;
    {
        const NodeKind lk = cond->rhs->kind;
        if (lk != NodeKind::IdentExpr && lk != NodeKind::IntLitExpr)
            return false;
    }
    const std::string idx_name =
        static_cast<IdentExpr *>(cond->lhs.get())->name;
    if (s->init->kind != NodeKind::VarDeclStmt) return false;
    auto *vd = static_cast<VarDeclStmt *>(s->init.get());
    if (vd->name != idx_name || !vd->init) return false;
    if (!mc_is_increment_expr(s->step.get(), idx_name)) return false;

    // body = un solo stmt: c[idx] = a[idx] OP b[idx].
    ast::Expr *bexpr = nullptr;
    if (s->body->kind == NodeKind::ExprStmt) {
        bexpr = static_cast<ExprStmt *>(s->body.get())->expr.get();
    } else if (s->body->kind == NodeKind::BlockStmt) {
        auto *b = static_cast<BlockStmt *>(s->body.get());
        if (b->body.size() != 1 || !b->body[0] ||
            b->body[0]->kind != NodeKind::ExprStmt)
            return false;
        bexpr = static_cast<ExprStmt *>(b->body[0].get())->expr.get();
    } else {
        return false;
    }
    if (!bexpr || bexpr->kind != NodeKind::AssignExpr) return false;
    auto *asg = static_cast<AssignExpr *>(bexpr);
    if (asg->op != AssignOp::Assign) return false;
    // target = c[idx]
    if (!asg->target || asg->target->kind != NodeKind::IndexExpr) return false;
    auto *c_ix = static_cast<IndexExpr *>(asg->target.get());
    // value = a[idx] OP b[idx]
    if (!asg->value || asg->value->kind != NodeKind::BinaryExpr) return false;
    auto *rhs = static_cast<BinaryExpr *>(asg->value.get());
    int subop = -1;
    switch (rhs->op) {
    case BinOp::Add: subop = 0; break;
    case BinOp::Sub: subop = 1; break;
    case BinOp::Mul: subop = 2; break;
    case BinOp::Div: subop = 3; break;
    default: return false;
    }
    if (!rhs->lhs || rhs->lhs->kind != NodeKind::IndexExpr) return false;
    if (!rhs->rhs || rhs->rhs->kind != NodeKind::IndexExpr) return false;
    auto *a_ix = static_cast<IndexExpr *>(rhs->lhs.get());
    auto *b_ix = static_cast<IndexExpr *>(rhs->rhs.get());

    // Tipos de elemento vectorizables (HOST): f64 (todas las ops) e enteros
    // i64/i32 (solo add/sub -- SSE2 no tiene mul/div packed de enteros).  El
    // valor de salida es el PrimitiveKind comun a las 3 bases.
    auto elem_info = [](PrimitiveKind k, ir::IrType *out_ty, uint64_t *out_esz,
                        bool *out_fp) -> bool {
        switch (k) {
        case PrimitiveKind::F64: *out_ty = ir::IrType::F64; *out_esz = 8; *out_fp = true;  return true;
        case PrimitiveKind::I64: *out_ty = ir::IrType::I64; *out_esz = 8; *out_fp = false; return true;
        case PrimitiveKind::U64: *out_ty = ir::IrType::U64; *out_esz = 8; *out_fp = false; return true;
        case PrimitiveKind::I32: *out_ty = ir::IrType::I32; *out_esz = 4; *out_fp = false; return true;
        case PrimitiveKind::U32: *out_ty = ir::IrType::U32; *out_esz = 4; *out_fp = false; return true;
        default: return false;
        }
    };
    // Helper: valida que @p ix es base_ident[idx] HOST de un tipo vectorizable;
    // devuelve la base + su PrimitiveKind de elemento.
    auto check_idx_host = [&](IndexExpr *ix, IdentExpr **out_base,
                              PrimitiveKind *out_kind) -> bool {
        if (!ix->overload_method.empty() || !ix->index_set_method.empty() ||
            ix->is_range)
            return false;
        if (!ix->base || ix->base->kind != NodeKind::IdentExpr) return false;
        if (!ix->index || ix->index->kind != NodeKind::IdentExpr) return false;
        if (static_cast<IdentExpr *>(ix->index.get())->name != idx_name)
            return false;
        auto *base = static_cast<IdentExpr *>(ix->base.get());
        const Type &t = base->result_type;
        const bool ptr_like = (t.kind == PrimitiveKind::PTR ||
                               t.kind == PrimitiveKind::ARRAY) &&
                              static_cast<bool>(t.pointee);
        if (!ptr_like || t.is_virtual) return false;        // solo HOST
        ir::IrType ety; uint64_t esz2; bool fp2;
        if (!elem_info(t.pointee->kind, &ety, &esz2, &fp2)) return false;
        *out_base = base;
        *out_kind = t.pointee->kind;
        return true;
    };
    ast::IdentExpr *c_base = nullptr, *a_base = nullptr, *b_base = nullptr;
    PrimitiveKind ck, ak, bk;
    if (!check_idx_host(c_ix, &c_base, &ck)) return false;
    if (!check_idx_host(a_ix, &a_base, &ak)) return false;
    if (!check_idx_host(b_ix, &b_base, &bk)) return false;
    if (ck != ak || ak != bk) return false;        // mismo tipo de elemento
    ir::IrType elem_ty; uint64_t esz; bool elem_fp;
    elem_info(ck, &elem_ty, &esz, &elem_fp);
    // Enteros: solo add/sub (sin mul/div packed en SSE2).
    if (!elem_fp && subop != 0 && subop != 1) return false;

    if (MC_DBG)
        std::fprintf(stderr, "[mc-idiom] MATCH vec_for idx=%s subop=%d\n",
                     idx_name.c_str(), subop);

    // ======== Emitir el loop vectorizado + cola escalar. ========
    // El chunk (16/32/64 bytes) lo elige la ISA: SSE2 W=2, AVX2 W=4, AVX512 W=8
    // (f64).  El IR es portable: el JIT descompone el chunk al ancho del host.
    const uint32_t ln = s->loc.line;
    const uint64_t width = jit::vec_isa_width(jit::vec_chunk_isa());
    const uint64_t W = width / esz; // lanes segun ancho/tipo

    // Bajar el VALOR inicial + las bases directamente (NO lower_stmt(init), que
    // declararia `i` address-taken -> ALLOCA).  NO re-bajamos el AST del cuerpo
    // para la cola (la var `i` es address-taken en el AST -> el load del index
    // leeria de un slot inexistente); emitimos la op escalar de la cola a mano
    // con phi_it + las bases.  Sin push_scope: solo leemos vars externas.
    const ir::IrValueId i_init = lower_expr(vd->init.get());
    const ir::IrValueId v_a = lower_expr(a_base);
    const ir::IrValueId v_b = lower_expr(b_base);
    const ir::IrValueId v_c = lower_expr(c_base);
    const ir::IrValueId v_N = lower_expr(cond->rhs.get());
    if (i_init == ir::IR_NO_VALUE || v_a == ir::IR_NO_VALUE ||
        v_b == ir::IR_NO_VALUE || v_c == ir::IR_NO_VALUE ||
        v_N == ir::IR_NO_VALUE)
        return false;
    const ir::IrType idx_ty = fn_->values[i_init].type;

    // bin: emite una op binaria en current_block_ (los helpers emit_const/
    // cast_if_needed tambien usan current_block_; por eso gestionamos
    // current_block_ por bloque en vez de emitir a un bloque arbitrario).
    auto bin = [&](ir::IrOp op, ir::IrType ty, ir::IrValueId a,
                   ir::IrValueId b) -> ir::IrValueId {
        const ir::IrValueId d = fn_->new_value(ty);
        ir::IrInstr in{};
        in.op = op; in.type = ty; in.dst = d;
        in.operands = {a, b}; in.source_line = ln;
        fn_->append(current_block_, std::move(in));
        return d;
    };

    const ir::IrBlockId entry = current_block_;
    const ir::IrBlockId mhdr = fn_->new_block("vec_main_hdr");
    const ir::IrBlockId mbody = fn_->new_block("vec_main_body");
    const ir::IrBlockId thdr = fn_->new_block("vec_tail_hdr");
    const ir::IrBlockId tbody = fn_->new_block("vec_tail_body");
    const ir::IrBlockId exit = fn_->new_block("vec_exit");

    // --- entry: consts loop-invariantes + BR mhdr ---
    current_block_ = entry;
    const ir::IrValueId v_W = emit_const(idx_ty, (uint64_t)W, ln);
    const ir::IrValueId v_esz = emit_const(ir::IrType::I64, esz, ln);
    { ir::IrInstr br{}; br.op = ir::IrOp::BR; br.target_block = mhdr;
      br.source_line = ln; fn_->append(entry, std::move(br)); }
    fn_->blocks[entry].succs.push_back(mhdr);
    fn_->blocks[mhdr].preds.push_back(entry);

    // --- mhdr: PHI i + cond (i+W) <= N -> mbody|thdr ---
    current_block_ = mhdr;
    const ir::IrValueId phi_im = fn_->new_value(idx_ty);
    {
        ir::IrInstr phi{}; phi.op = ir::IrOp::PHI; phi.type = idx_ty;
        phi.dst = phi_im; phi.source_line = ln;
        phi.phi_args.push_back({i_init, entry});
        fn_->append(mhdr, std::move(phi));
    }
    const ir::IrValueId v_ipW = bin(ir::IrOp::ADD, idx_ty, phi_im, v_W);
    const ir::IrValueId cond_m =
        bin(ir::IrOp::CMP_LE, ir::IrType::BOOL, v_ipW, v_N);
    {
        ir::IrInstr brc{}; brc.op = ir::IrOp::BR_COND; brc.operands = {cond_m};
        brc.target_block = mbody; brc.false_block = thdr; brc.source_line = ln;
        fn_->append(mhdr, std::move(brc));
    }
    fn_->blocks[mhdr].succs.push_back(mbody);
    fn_->blocks[mhdr].succs.push_back(thdr);
    fn_->blocks[mbody].preds.push_back(mhdr);
    fn_->blocks[thdr].preds.push_back(mhdr);

    // --- mbody: VEC_BINOP(c+i*esz, a+i*esz, b+i*esz); i += W; BR mhdr ---
    current_block_ = mbody;
    auto ptr_at = [&](ir::IrValueId base, ir::IrValueId i64off) -> ir::IrValueId {
        const ir::IrValueId d = fn_->new_value(ir::IrType::PTR);
        fn_->values[d].is_host_ptr = true;
        ir::IrInstr in{}; in.op = ir::IrOp::ADD; in.type = ir::IrType::PTR;
        in.dst = d; in.operands = {base, i64off}; in.source_line = ln;
        fn_->append(current_block_, std::move(in));
        return d;
    };
    {
        const ir::IrValueId i64 =
            cast_if_needed(phi_im, idx_ty, ir::IrType::I64, ln);
        const ir::IrValueId off = bin(ir::IrOp::MUL, ir::IrType::I64, i64, v_esz);
        const ir::IrValueId c_at = ptr_at(v_c, off);
        const ir::IrValueId a_at = ptr_at(v_a, off);
        const ir::IrValueId b_at = ptr_at(v_b, off);
        ir::IrInstr vb{}; vb.op = ir::IrOp::VEC_BINOP; vb.type = elem_ty;
        vb.dst = ir::IR_NO_VALUE;
        vb.operands = {c_at, a_at, b_at};
        vb.imm = ((uint64_t)subop << 8) | width;
        vb.source_line = ln;
        fn_->append(current_block_, std::move(vb));
    }
    const ir::IrValueId i_mnext = bin(ir::IrOp::ADD, idx_ty, phi_im, v_W);
    { ir::IrInstr br{}; br.op = ir::IrOp::BR; br.target_block = mhdr;
      br.source_line = ln; fn_->append(current_block_, std::move(br)); }
    fn_->blocks[mbody].succs.push_back(mhdr);
    fn_->blocks[mhdr].preds.push_back(mbody);
    fn_->blocks[mhdr].instrs[0].phi_args.push_back({i_mnext, mbody});

    // --- thdr: PHI i + cond i<N -> tbody|exit ---
    current_block_ = thdr;
    const ir::IrValueId phi_it = fn_->new_value(idx_ty);
    {
        ir::IrInstr phi{}; phi.op = ir::IrOp::PHI; phi.type = idx_ty;
        phi.dst = phi_it; phi.source_line = ln;
        phi.phi_args.push_back({phi_im, mhdr});
        fn_->append(thdr, std::move(phi));
    }
    const ir::IrValueId cond_t =
        bin(ir::IrOp::CMP_LT, ir::IrType::BOOL, phi_it, v_N);
    {
        ir::IrInstr brc{}; brc.op = ir::IrOp::BR_COND; brc.operands = {cond_t};
        brc.target_block = tbody; brc.false_block = exit; brc.source_line = ln;
        fn_->append(thdr, std::move(brc));
    }
    fn_->blocks[thdr].succs.push_back(tbody);
    fn_->blocks[thdr].succs.push_back(exit);
    fn_->blocks[tbody].preds.push_back(thdr);
    fn_->blocks[exit].preds.push_back(thdr);

    // --- tbody: op escalar c[i]=a[i] OP b[i] (emitida a mano, i=phi_it) ---
    current_block_ = tbody;
    block_terminated_ = false;
    {
        const ir::IrValueId i64 =
            cast_if_needed(phi_it, idx_ty, ir::IrType::I64, ln);
        const ir::IrValueId off = bin(ir::IrOp::MUL, ir::IrType::I64, i64,
                                      emit_const(ir::IrType::I64, esz, ln));
        auto load_el = [&](ir::IrValueId base) -> ir::IrValueId {
            const ir::IrValueId at = ptr_at(base, off);
            const ir::IrValueId v = fn_->new_value(elem_ty);
            ir::IrInstr ld{}; ld.op = ir::IrOp::LOAD; ld.type = elem_ty;
            ld.dst = v; ld.operands = {at}; ld.source_line = ln;
            fn_->append(current_block_, std::move(ld));
            return v;
        };
        const ir::IrValueId v_ai = load_el(v_a);
        const ir::IrValueId v_bi = load_el(v_b);
        ir::IrOp eop;
        if (elem_fp)
            eop = (subop == 0)   ? ir::IrOp::FADD
                  : (subop == 1) ? ir::IrOp::FSUB
                  : (subop == 2) ? ir::IrOp::FMUL
                                 : ir::IrOp::FDIV;
        else // enteros: solo add/sub (validado arriba)
            eop = (subop == 0) ? ir::IrOp::ADD : ir::IrOp::SUB;
        const ir::IrValueId v_res = bin(eop, elem_ty, v_ai, v_bi);
        const ir::IrValueId c_at = ptr_at(v_c, off);
        ir::IrInstr st{}; st.op = ir::IrOp::STORE; st.type = elem_ty;
        st.dst = ir::IR_NO_VALUE; st.operands = {v_res, c_at};
        st.source_line = ln;
        fn_->append(current_block_, std::move(st));
    }
    const ir::IrValueId i_tnext =
        bin(ir::IrOp::ADD, idx_ty, phi_it, emit_const(idx_ty, 1, ln));
    { ir::IrInstr br{}; br.op = ir::IrOp::BR; br.target_block = thdr;
      br.source_line = ln; fn_->append(current_block_, std::move(br)); }
    fn_->blocks[tbody].succs.push_back(thdr);
    fn_->blocks[thdr].preds.push_back(tbody);
    fn_->blocks[thdr].instrs[0].phi_args.push_back({i_tnext, tbody});

    // continuar en exit.
    current_block_ = exit;
    block_terminated_ = false;
    return true;
}

bool Lowering::try_vectorize_unary_for(ast::ForStmt *s) {
    using namespace ast;
    static const bool MC_DBG = std::getenv("VESTA_MC_IDIOM_DEBUG") != nullptr;
    if (!s->cond || !s->body || !s->init || !s->step) return false;

    // --- estructura: for (T i = init; i < N; i++) b[i] = OP a[i]; ---
    if (s->cond->kind != NodeKind::BinaryExpr) return false;
    auto *cond = static_cast<BinaryExpr *>(s->cond.get());
    if (cond->op != BinOp::Lt) return false;
    if (!cond->lhs || cond->lhs->kind != NodeKind::IdentExpr) return false;
    if (!cond->rhs) return false;
    {
        const NodeKind lk = cond->rhs->kind;
        if (lk != NodeKind::IdentExpr && lk != NodeKind::IntLitExpr)
            return false;
    }
    const std::string idx_name =
        static_cast<IdentExpr *>(cond->lhs.get())->name;
    if (s->init->kind != NodeKind::VarDeclStmt) return false;
    auto *vd = static_cast<VarDeclStmt *>(s->init.get());
    if (vd->name != idx_name || !vd->init) return false;
    if (!mc_is_increment_expr(s->step.get(), idx_name)) return false;

    // body = un solo stmt: b[idx] = OP a[idx].
    ast::Expr *bexpr = nullptr;
    if (s->body->kind == NodeKind::ExprStmt) {
        bexpr = static_cast<ExprStmt *>(s->body.get())->expr.get();
    } else if (s->body->kind == NodeKind::BlockStmt) {
        auto *b = static_cast<BlockStmt *>(s->body.get());
        if (b->body.size() != 1 || !b->body[0] ||
            b->body[0]->kind != NodeKind::ExprStmt)
            return false;
        bexpr = static_cast<ExprStmt *>(b->body[0].get())->expr.get();
    } else {
        return false;
    }
    if (!bexpr || bexpr->kind != NodeKind::AssignExpr) return false;
    auto *asg = static_cast<AssignExpr *>(bexpr);
    if (asg->op != AssignOp::Assign) return false;
    // target = b[idx]
    if (!asg->target || asg->target->kind != NodeKind::IndexExpr) return false;
    auto *b_ix = static_cast<IndexExpr *>(asg->target.get());

    // value = OP a[idx].  OP in: -a[idx] (UnaryExpr Neg=fneg), sqrt(a[idx])
    // (CallExpr fsqrt), fabs(a[idx]) (CallExpr fabs).  La COPIA pura
    // (b[i]=a[i]) la cubre el memcpy-idiom (loop entero -> 1 rep movsb).
    int subop = -1;            // 1=fneg, 2=fabs, 3=fsqrt
    ast::IndexExpr *a_ix = nullptr;
    if (asg->value && asg->value->kind == NodeKind::UnaryExpr) {
        auto *u = static_cast<UnaryExpr *>(asg->value.get());
        if (u->op != UnOp::Neg) return false;
        if (!u->overload_method.empty()) return false; // operator overload
        if (!u->operand || u->operand->kind != NodeKind::IndexExpr)
            return false;
        subop = 1;
        a_ix = static_cast<IndexExpr *>(u->operand.get());
    } else if (asg->value && asg->value->kind == NodeKind::CallExpr) {
        auto *cl = static_cast<CallExpr *>(asg->value.get());
        if (!cl->callee || cl->callee->kind != NodeKind::IdentExpr) return false;
        if (!cl->type_args.empty() || cl->args.size() != 1) return false;
        const std::string &fname =
            static_cast<IdentExpr *>(cl->callee.get())->name;
        if (fname == "fabs") subop = 2;
        else if (fname == "sqrt") subop = 3;
        else return false;
        if (!cl->args[0] || cl->args[0]->kind != NodeKind::IndexExpr)
            return false;
        a_ix = static_cast<IndexExpr *>(cl->args[0].get());
    } else {
        return false;
    }

    // Helper: valida que @p ix es base_ident[idx] HOST f64, devuelve la base.
    // Solo f64: fneg/fabs/fsqrt son float (SQRTPD/XORPD/ANDPD operan f64).
    auto check_idx_f64_host = [&](IndexExpr *ix,
                                  IdentExpr **out_base) -> bool {
        if (!ix->overload_method.empty() || !ix->index_set_method.empty() ||
            ix->is_range)
            return false;
        if (!ix->base || ix->base->kind != NodeKind::IdentExpr) return false;
        if (!ix->index || ix->index->kind != NodeKind::IdentExpr) return false;
        if (static_cast<IdentExpr *>(ix->index.get())->name != idx_name)
            return false;
        auto *base = static_cast<IdentExpr *>(ix->base.get());
        const Type &t = base->result_type;
        const bool ptr_like = (t.kind == PrimitiveKind::PTR ||
                               t.kind == PrimitiveKind::ARRAY) &&
                              static_cast<bool>(t.pointee);
        if (!ptr_like || t.is_virtual) return false;        // solo HOST
        if (t.pointee->kind != PrimitiveKind::F64) return false; // solo f64
        *out_base = base;
        return true;
    };
    ast::IdentExpr *b_base = nullptr, *a_base = nullptr;
    if (!check_idx_f64_host(b_ix, &b_base)) return false;
    if (!check_idx_f64_host(a_ix, &a_base)) return false;

    if (MC_DBG)
        std::fprintf(stderr, "[mc-idiom] MATCH vec_unop idx=%s subop=%d\n",
                     idx_name.c_str(), subop);

    // ======== Emitir el loop vectorizado + cola escalar. ========
    // Chunk por ISA (SSE2/AVX2/AVX512); el JIT descompone al ancho del host.
    const uint32_t ln = s->loc.line;
    const uint64_t esz = 8;     // f64
    const uint64_t width = jit::vec_isa_width(jit::vec_chunk_isa());
    const uint64_t W = width / esz;

    const ir::IrValueId i_init = lower_expr(vd->init.get());
    const ir::IrValueId v_a = lower_expr(a_base);
    const ir::IrValueId v_b = lower_expr(b_base);
    const ir::IrValueId v_N = lower_expr(cond->rhs.get());
    if (i_init == ir::IR_NO_VALUE || v_a == ir::IR_NO_VALUE ||
        v_b == ir::IR_NO_VALUE || v_N == ir::IR_NO_VALUE)
        return false;
    const ir::IrType idx_ty = fn_->values[i_init].type;

    auto bin = [&](ir::IrOp op, ir::IrType ty, ir::IrValueId a,
                   ir::IrValueId b) -> ir::IrValueId {
        const ir::IrValueId d = fn_->new_value(ty);
        ir::IrInstr in{};
        in.op = op; in.type = ty; in.dst = d;
        in.operands = {a, b}; in.source_line = ln;
        fn_->append(current_block_, std::move(in));
        return d;
    };
    auto ptr_at = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        const ir::IrValueId d = fn_->new_value(ir::IrType::PTR);
        fn_->values[d].is_host_ptr = true;
        ir::IrInstr in{}; in.op = ir::IrOp::ADD; in.type = ir::IrType::PTR;
        in.dst = d; in.operands = {base, off}; in.source_line = ln;
        fn_->append(current_block_, std::move(in));
        return d;
    };

    const ir::IrBlockId entry = current_block_;
    const ir::IrBlockId mhdr = fn_->new_block("vun_main_hdr");
    const ir::IrBlockId mbody = fn_->new_block("vun_main_body");
    const ir::IrBlockId thdr = fn_->new_block("vun_tail_hdr");
    const ir::IrBlockId tbody = fn_->new_block("vun_tail_body");
    const ir::IrBlockId exit = fn_->new_block("vun_exit");

    // --- entry: consts + BR mhdr ---
    current_block_ = entry;
    const ir::IrValueId v_W = emit_const(idx_ty, (uint64_t)W, ln);
    const ir::IrValueId v_esz = emit_const(ir::IrType::I64, esz, ln);
    { ir::IrInstr br{}; br.op = ir::IrOp::BR; br.target_block = mhdr;
      br.source_line = ln; fn_->append(entry, std::move(br)); }
    fn_->blocks[entry].succs.push_back(mhdr);
    fn_->blocks[mhdr].preds.push_back(entry);

    // --- mhdr: PHI i + cond (i+W)<=N -> mbody|thdr ---
    current_block_ = mhdr;
    const ir::IrValueId phi_im = fn_->new_value(idx_ty);
    {
        ir::IrInstr phi{}; phi.op = ir::IrOp::PHI; phi.type = idx_ty;
        phi.dst = phi_im; phi.source_line = ln;
        phi.phi_args.push_back({i_init, entry});
        fn_->append(mhdr, std::move(phi));
    }
    const ir::IrValueId v_ipW = bin(ir::IrOp::ADD, idx_ty, phi_im, v_W);
    const ir::IrValueId cond_m =
        bin(ir::IrOp::CMP_LE, ir::IrType::BOOL, v_ipW, v_N);
    {
        ir::IrInstr brc{}; brc.op = ir::IrOp::BR_COND; brc.operands = {cond_m};
        brc.target_block = mbody; brc.false_block = thdr; brc.source_line = ln;
        fn_->append(mhdr, std::move(brc));
    }
    fn_->blocks[mhdr].succs.push_back(mbody);
    fn_->blocks[mhdr].succs.push_back(thdr);
    fn_->blocks[mbody].preds.push_back(mhdr);
    fn_->blocks[thdr].preds.push_back(mhdr);

    // --- mbody: VEC_UNOP(b+i*esz, a+i*esz); i += W; BR mhdr ---
    current_block_ = mbody;
    {
        const ir::IrValueId i64 =
            cast_if_needed(phi_im, idx_ty, ir::IrType::I64, ln);
        const ir::IrValueId off = bin(ir::IrOp::MUL, ir::IrType::I64, i64, v_esz);
        const ir::IrValueId b_at = ptr_at(v_b, off);
        const ir::IrValueId a_at = ptr_at(v_a, off);
        ir::IrInstr vu{}; vu.op = ir::IrOp::VEC_UNOP; vu.type = ir::IrType::F64;
        vu.dst = ir::IR_NO_VALUE;
        vu.operands = {b_at, a_at};
        vu.imm = ((uint64_t)subop << 8) | width;
        vu.source_line = ln;
        fn_->append(current_block_, std::move(vu));
    }
    const ir::IrValueId i_mnext = bin(ir::IrOp::ADD, idx_ty, phi_im, v_W);
    { ir::IrInstr br{}; br.op = ir::IrOp::BR; br.target_block = mhdr;
      br.source_line = ln; fn_->append(current_block_, std::move(br)); }
    fn_->blocks[mbody].succs.push_back(mhdr);
    fn_->blocks[mhdr].preds.push_back(mbody);
    fn_->blocks[mhdr].instrs[0].phi_args.push_back({i_mnext, mbody});

    // --- thdr: PHI i + cond i<N -> tbody|exit ---
    current_block_ = thdr;
    const ir::IrValueId phi_it = fn_->new_value(idx_ty);
    {
        ir::IrInstr phi{}; phi.op = ir::IrOp::PHI; phi.type = idx_ty;
        phi.dst = phi_it; phi.source_line = ln;
        phi.phi_args.push_back({phi_im, mhdr});
        fn_->append(thdr, std::move(phi));
    }
    const ir::IrValueId cond_t =
        bin(ir::IrOp::CMP_LT, ir::IrType::BOOL, phi_it, v_N);
    {
        ir::IrInstr brc{}; brc.op = ir::IrOp::BR_COND; brc.operands = {cond_t};
        brc.target_block = tbody; brc.false_block = exit; brc.source_line = ln;
        fn_->append(thdr, std::move(brc));
    }
    fn_->blocks[thdr].succs.push_back(tbody);
    fn_->blocks[thdr].succs.push_back(exit);
    fn_->blocks[tbody].preds.push_back(thdr);
    fn_->blocks[exit].preds.push_back(thdr);

    // --- tbody: op escalar b[i] = OP a[i] (emitida a mano, i=phi_it) ---
    current_block_ = tbody;
    block_terminated_ = false;
    {
        const ir::IrValueId i64 =
            cast_if_needed(phi_it, idx_ty, ir::IrType::I64, ln);
        const ir::IrValueId off = bin(ir::IrOp::MUL, ir::IrType::I64, i64,
                                      emit_const(ir::IrType::I64, esz, ln));
        const ir::IrValueId a_at = ptr_at(v_a, off);
        const ir::IrValueId v_ai = fn_->new_value(ir::IrType::F64);
        { ir::IrInstr ld{}; ld.op = ir::IrOp::LOAD; ld.type = ir::IrType::F64;
          ld.dst = v_ai; ld.operands = {a_at}; ld.source_line = ln;
          fn_->append(current_block_, std::move(ld)); }
        const ir::IrOp uop = (subop == 1)   ? ir::IrOp::FNEG
                             : (subop == 2) ? ir::IrOp::FABS
                                            : ir::IrOp::FSQRT;
        const ir::IrValueId v_res = fn_->new_value(ir::IrType::F64);
        { ir::IrInstr un{}; un.op = uop; un.type = ir::IrType::F64;
          un.dst = v_res; un.operands = {v_ai}; un.source_line = ln;
          fn_->append(current_block_, std::move(un)); }
        const ir::IrValueId b_at = ptr_at(v_b, off);
        ir::IrInstr st{}; st.op = ir::IrOp::STORE; st.type = ir::IrType::F64;
        st.dst = ir::IR_NO_VALUE; st.operands = {v_res, b_at};
        st.source_line = ln;
        fn_->append(current_block_, std::move(st));
    }
    const ir::IrValueId i_tnext =
        bin(ir::IrOp::ADD, idx_ty, phi_it, emit_const(idx_ty, 1, ln));
    { ir::IrInstr br{}; br.op = ir::IrOp::BR; br.target_block = thdr;
      br.source_line = ln; fn_->append(current_block_, std::move(br)); }
    fn_->blocks[tbody].succs.push_back(thdr);
    fn_->blocks[thdr].preds.push_back(tbody);
    fn_->blocks[thdr].instrs[0].phi_args.push_back({i_tnext, tbody});

    // continuar en exit.
    current_block_ = exit;
    block_terminated_ = false;
    return true;
}

bool Lowering::try_vectorize_reduction_for(ast::ForStmt *s) {
    using namespace ast;
    static const bool MC_DBG = std::getenv("VESTA_MC_IDIOM_DEBUG") != nullptr;
    if (!s->cond || !s->body || !s->init || !s->step) return false;

    // --- estructura: for (T i = init; i < N; i++) acc = acc + a[i]; ---
    if (s->cond->kind != NodeKind::BinaryExpr) return false;
    auto *cond = static_cast<BinaryExpr *>(s->cond.get());
    if (cond->op != BinOp::Lt) return false;
    if (!cond->lhs || cond->lhs->kind != NodeKind::IdentExpr) return false;
    if (!cond->rhs) return false;
    {
        const NodeKind lk = cond->rhs->kind;
        if (lk != NodeKind::IdentExpr && lk != NodeKind::IntLitExpr)
            return false;
    }
    const std::string idx_name =
        static_cast<IdentExpr *>(cond->lhs.get())->name;
    if (s->init->kind != NodeKind::VarDeclStmt) return false;
    auto *vd = static_cast<VarDeclStmt *>(s->init.get());
    if (vd->name != idx_name || !vd->init) return false;
    if (!mc_is_increment_expr(s->step.get(), idx_name)) return false;

    // body = un solo stmt: acc = acc + a[idx].
    ast::Expr *bexpr = nullptr;
    if (s->body->kind == NodeKind::ExprStmt) {
        bexpr = static_cast<ExprStmt *>(s->body.get())->expr.get();
    } else if (s->body->kind == NodeKind::BlockStmt) {
        auto *b = static_cast<BlockStmt *>(s->body.get());
        if (b->body.size() != 1 || !b->body[0] ||
            b->body[0]->kind != NodeKind::ExprStmt)
            return false;
        bexpr = static_cast<ExprStmt *>(b->body[0].get())->expr.get();
    } else {
        return false;
    }
    if (!bexpr || bexpr->kind != NodeKind::AssignExpr) return false;
    auto *asg = static_cast<AssignExpr *>(bexpr);
    if (asg->op != AssignOp::Assign) return false;
    // target = acc (ident f64)
    if (!asg->target || asg->target->kind != NodeKind::IdentExpr) return false;
    auto *acc_id = static_cast<IdentExpr *>(asg->target.get());
    // Tipos de acumulador vectorizables: f64 e enteros i64/i32 (suma).
    auto red_elem_info = [](PrimitiveKind k, ir::IrType *out_ty,
                            uint64_t *out_esz, bool *out_fp) -> bool {
        switch (k) {
        case PrimitiveKind::F64: *out_ty = ir::IrType::F64; *out_esz = 8; *out_fp = true;  return true;
        case PrimitiveKind::I64: *out_ty = ir::IrType::I64; *out_esz = 8; *out_fp = false; return true;
        case PrimitiveKind::U64: *out_ty = ir::IrType::U64; *out_esz = 8; *out_fp = false; return true;
        case PrimitiveKind::I32: *out_ty = ir::IrType::I32; *out_esz = 4; *out_fp = false; return true;
        case PrimitiveKind::U32: *out_ty = ir::IrType::U32; *out_esz = 4; *out_fp = false; return true;
        default: return false;
        }
    };
    ir::IrType elem_ty; uint64_t esz; bool elem_fp;
    if (!red_elem_info(acc_id->result_type.kind, &elem_ty, &esz, &elem_fp))
        return false;
    const std::string acc_name = acc_id->name;
    // value = acc + a[idx]  (solo suma v1)
    if (!asg->value || asg->value->kind != NodeKind::BinaryExpr) return false;
    auto *rhs = static_cast<BinaryExpr *>(asg->value.get());
    if (rhs->op != BinOp::Add) return false;
    if (!rhs->lhs || rhs->lhs->kind != NodeKind::IdentExpr) return false;
    if (static_cast<IdentExpr *>(rhs->lhs.get())->name != acc_name)
        return false; // lhs debe ser el MISMO acc
    if (!rhs->rhs || rhs->rhs->kind != NodeKind::IndexExpr) return false;
    auto *a_ix = static_cast<IndexExpr *>(rhs->rhs.get());
    if (!a_ix->overload_method.empty() || a_ix->is_range) return false;
    if (!a_ix->base || a_ix->base->kind != NodeKind::IdentExpr) return false;
    if (!a_ix->index || a_ix->index->kind != NodeKind::IdentExpr) return false;
    if (static_cast<IdentExpr *>(a_ix->index.get())->name != idx_name)
        return false;
    auto *a_base = static_cast<IdentExpr *>(a_ix->base.get());
    {
        const Type &t = a_base->result_type;
        const bool ptr_like = (t.kind == PrimitiveKind::PTR ||
                               t.kind == PrimitiveKind::ARRAY) &&
                              static_cast<bool>(t.pointee);
        if (!ptr_like || t.is_virtual) return false; // solo HOST
        // el array debe tener EL MISMO tipo de elemento que el acumulador.
        if (t.pointee->kind != acc_id->result_type.kind) return false;
    }

    if (MC_DBG)
        std::fprintf(stderr, "[mc-idiom] MATCH vec_reduce idx=%s acc=%s\n",
                     idx_name.c_str(), acc_name.c_str());

    // ======== Emitir: acumulador vectorial + reduccion horizontal + cola.
    // El ancho del acumulador vectorial lo elige la ISA (16/32/64 = W lanes).
    const uint32_t ln = s->loc.line;
    const uint64_t width = jit::vec_isa_width(jit::vec_chunk_isa());
    const uint64_t W = width / esz;     // lanes segun ancho/tipo

    // acc_init = valor f64 de acc ANTES del loop (binding SSA plano).
    const ir::IrValueId acc_init = lookup(acc_name);
    const ir::IrValueId i_init = lower_expr(vd->init.get());
    const ir::IrValueId v_a = lower_expr(a_base);
    const ir::IrValueId v_N = lower_expr(cond->rhs.get());
    if (acc_init == ir::IR_NO_VALUE || i_init == ir::IR_NO_VALUE ||
        v_a == ir::IR_NO_VALUE || v_N == ir::IR_NO_VALUE)
        return false;
    if (fn_->values[acc_init].type != elem_ty) return false;
    const ir::IrType idx_ty = fn_->values[i_init].type;

    auto bin = [&](ir::IrOp op, ir::IrType ty, ir::IrValueId a,
                   ir::IrValueId b) -> ir::IrValueId {
        const ir::IrValueId d = fn_->new_value(ty);
        ir::IrInstr in{};
        in.op = op; in.type = ty; in.dst = d;
        in.operands = {a, b}; in.source_line = ln;
        fn_->append(current_block_, std::move(in));
        return d;
    };
    auto ptr_at = [&](ir::IrValueId base, ir::IrValueId off) -> ir::IrValueId {
        const ir::IrValueId d = fn_->new_value(ir::IrType::PTR);
        fn_->values[d].is_host_ptr = true;
        ir::IrInstr in{}; in.op = ir::IrOp::ADD; in.type = ir::IrType::PTR;
        in.dst = d; in.operands = {base, off}; in.source_line = ln;
        fn_->append(current_block_, std::move(in));
        return d;
    };
    // load/store del tipo de elemento (acumulador y array).
    auto load_el = [&](ir::IrValueId at) -> ir::IrValueId {
        const ir::IrValueId v = fn_->new_value(elem_ty);
        ir::IrInstr ld{}; ld.op = ir::IrOp::LOAD; ld.type = elem_ty;
        ld.dst = v; ld.operands = {at}; ld.source_line = ln;
        fn_->append(current_block_, std::move(ld));
        return v;
    };
    // op de acumulacion: FADD para float, ADD para entero.
    const ir::IrOp acc_op = elem_fp ? ir::IrOp::FADD : ir::IrOp::ADD;
    // store crudo de 8 bytes a 0 (zero-init del slot de 16B, cualquier tipo).
    auto store_zero8 = [&](ir::IrValueId at, ir::IrValueId vz) {
        ir::IrInstr st{}; st.op = ir::IrOp::STORE; st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE; st.operands = {vz, at}; st.source_line = ln;
        fn_->append(current_block_, std::move(st));
    };

    const ir::IrBlockId entry = current_block_;
    const ir::IrBlockId mhdr = fn_->new_block("vred_main_hdr");
    const ir::IrBlockId mbody = fn_->new_block("vred_main_body");
    const ir::IrBlockId redb = fn_->new_block("vred_reduce");
    const ir::IrBlockId thdr = fn_->new_block("vred_tail_hdr");
    const ir::IrBlockId tbody = fn_->new_block("vred_tail_body");
    const ir::IrBlockId exit = fn_->new_block("vred_exit");

    // --- entry: acc_slot host `width` bytes = {0..}; consts; BR mhdr ---
    current_block_ = entry;
    const ir::IrValueId acc_slot = fn_->new_value(ir::IrType::PTR);
    fn_->values[acc_slot].is_host_ptr = true;
    {
        ir::IrInstr al{}; al.op = ir::IrOp::ALLOCA; al.type = ir::IrType::I8;
        al.dst = acc_slot; al.imm = static_cast<int64_t>(width);
        al.host_alloca = true;
        al.source_line = ln; fn_->append(entry, std::move(al));
    }
    // zero-init de los `width` bytes del slot (width/8 stores de 8 bytes a 0),
    // valido para cualquier tipo de elemento (W lanes == todo a 0).
    const ir::IrValueId v_zero8 = emit_const(ir::IrType::I64, 0, ln);
    for (uint64_t q = 0; q < width; q += 8) {
        const ir::IrValueId at =
            (q == 0) ? acc_slot
                     : ptr_at(acc_slot, emit_const(ir::IrType::I64, q, ln));
        store_zero8(at, v_zero8);
    }
    const ir::IrValueId v_W = emit_const(idx_ty, (uint64_t)W, ln);
    const ir::IrValueId v_esz = emit_const(ir::IrType::I64, esz, ln);
    { ir::IrInstr br{}; br.op = ir::IrOp::BR; br.target_block = mhdr;
      br.source_line = ln; fn_->append(entry, std::move(br)); }
    fn_->blocks[entry].succs.push_back(mhdr);
    fn_->blocks[mhdr].preds.push_back(entry);

    // --- mhdr: PHI i; cond (i+W)<=N -> mbody|redb ---
    current_block_ = mhdr;
    const ir::IrValueId phi_im = fn_->new_value(idx_ty);
    {
        ir::IrInstr phi{}; phi.op = ir::IrOp::PHI; phi.type = idx_ty;
        phi.dst = phi_im; phi.source_line = ln;
        phi.phi_args.push_back({i_init, entry});
        fn_->append(mhdr, std::move(phi));
    }
    const ir::IrValueId v_ipW = bin(ir::IrOp::ADD, idx_ty, phi_im, v_W);
    const ir::IrValueId cond_m =
        bin(ir::IrOp::CMP_LE, ir::IrType::BOOL, v_ipW, v_N);
    {
        ir::IrInstr brc{}; brc.op = ir::IrOp::BR_COND; brc.operands = {cond_m};
        brc.target_block = mbody; brc.false_block = redb; brc.source_line = ln;
        fn_->append(mhdr, std::move(brc));
    }
    fn_->blocks[mhdr].succs.push_back(mbody);
    fn_->blocks[mhdr].succs.push_back(redb);
    fn_->blocks[mbody].preds.push_back(mhdr);
    fn_->blocks[redb].preds.push_back(mhdr);

    // --- mbody: acc_slot += a[i..i+1] (VEC_BINOP add); i += W; BR mhdr ---
    current_block_ = mbody;
    {
        const ir::IrValueId i64 =
            cast_if_needed(phi_im, idx_ty, ir::IrType::I64, ln);
        const ir::IrValueId off = bin(ir::IrOp::MUL, ir::IrType::I64, i64, v_esz);
        const ir::IrValueId a_at = ptr_at(v_a, off);
        ir::IrInstr vb{}; vb.op = ir::IrOp::VEC_BINOP; vb.type = elem_ty;
        vb.dst = ir::IR_NO_VALUE;
        vb.operands = {acc_slot, acc_slot, a_at}; // acc_slot = acc_slot + a
        vb.imm = ((uint64_t)0 << 8) | width;      // subop 0 = add
        vb.source_line = ln;
        fn_->append(current_block_, std::move(vb));
    }
    const ir::IrValueId i_mnext = bin(ir::IrOp::ADD, idx_ty, phi_im, v_W);
    { ir::IrInstr br{}; br.op = ir::IrOp::BR; br.target_block = mhdr;
      br.source_line = ln; fn_->append(current_block_, std::move(br)); }
    fn_->blocks[mbody].succs.push_back(mhdr);
    fn_->blocks[mhdr].preds.push_back(mbody);
    fn_->blocks[mhdr].instrs[0].phi_args.push_back({i_mnext, mbody});

    // --- redb: horizontal sum_{k<W} acc_slot[k] + acc_init; BR thdr ---
    current_block_ = redb;
    ir::IrValueId result0 = acc_init;
    for (uint64_t k = 0; k < W; ++k) {
        const ir::IrValueId at =
            (k == 0) ? acc_slot
                     : ptr_at(acc_slot, emit_const(ir::IrType::I64, k * esz, ln));
        const ir::IrValueId lane = load_el(at);
        result0 = bin(acc_op, elem_ty, result0, lane);
    }
    { ir::IrInstr br{}; br.op = ir::IrOp::BR; br.target_block = thdr;
      br.source_line = ln; fn_->append(redb, std::move(br)); }
    fn_->blocks[redb].succs.push_back(thdr);
    fn_->blocks[thdr].preds.push_back(redb);

    // --- thdr: PHI i + PHI result; cond i<N -> tbody|exit ---
    current_block_ = thdr;
    const ir::IrValueId phi_it = fn_->new_value(idx_ty);
    {
        ir::IrInstr phi{}; phi.op = ir::IrOp::PHI; phi.type = idx_ty;
        phi.dst = phi_it; phi.source_line = ln;
        phi.phi_args.push_back({phi_im, redb});
        fn_->append(thdr, std::move(phi));
    }
    const ir::IrValueId phi_res = fn_->new_value(elem_ty);
    {
        ir::IrInstr phi{}; phi.op = ir::IrOp::PHI; phi.type = elem_ty;
        phi.dst = phi_res; phi.source_line = ln;
        phi.phi_args.push_back({result0, redb});
        fn_->append(thdr, std::move(phi));
    }
    const ir::IrValueId cond_t =
        bin(ir::IrOp::CMP_LT, ir::IrType::BOOL, phi_it, v_N);
    {
        ir::IrInstr brc{}; brc.op = ir::IrOp::BR_COND; brc.operands = {cond_t};
        brc.target_block = tbody; brc.false_block = exit; brc.source_line = ln;
        fn_->append(thdr, std::move(brc));
    }
    fn_->blocks[thdr].succs.push_back(tbody);
    fn_->blocks[thdr].succs.push_back(exit);
    fn_->blocks[tbody].preds.push_back(thdr);
    fn_->blocks[exit].preds.push_back(thdr);

    // --- tbody: result += a[i]; i++; BR thdr ---
    current_block_ = tbody;
    {
        const ir::IrValueId i64 =
            cast_if_needed(phi_it, idx_ty, ir::IrType::I64, ln);
        const ir::IrValueId off = bin(ir::IrOp::MUL, ir::IrType::I64, i64, v_esz);
        const ir::IrValueId ai = load_el(ptr_at(v_a, off));
        const ir::IrValueId rnext = bin(acc_op, elem_ty, phi_res, ai);
        const ir::IrValueId i_tnext =
            bin(ir::IrOp::ADD, idx_ty, phi_it, emit_const(idx_ty, 1, ln));
        { ir::IrInstr br{}; br.op = ir::IrOp::BR; br.target_block = thdr;
          br.source_line = ln; fn_->append(current_block_, std::move(br)); }
        fn_->blocks[tbody].succs.push_back(thdr);
        fn_->blocks[thdr].preds.push_back(tbody);
        fn_->blocks[thdr].instrs[0].phi_args.push_back({i_tnext, tbody});
        fn_->blocks[thdr].instrs[1].phi_args.push_back({rnext, tbody});
    }

    // --- exit: acc = phi_res (resultado final) ---
    current_block_ = exit;
    block_terminated_ = false;
    update_scope(acc_name, phi_res);
    return true;
}

} // namespace vex
