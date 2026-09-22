/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_byte_buffer.cpp
 * @brief The common part every byte buffer in the project shares.
 *
 * Two of these checks are the design itself, not details:
 *
 * - a buffer always knows WHAT it is, so it can check its own header instead
 *   of every consumer remembering to;
 * - allocation belongs to the chain, not to the common part, which is what
 *   makes each chain show up under its own name in the allocation report.
 *   That is the whole reason this type exists: 60% of 116 million allocations
 *   could not be attributed because every buffer was the same `vector<uint8_t>`.
 */

#include "util/byte_buffer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace util;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);        \
        }                                                                      \
    } while (0)

/* --- two chains, each with its OWN allocation ----------------------------
 *
 * These are test doubles, not an example to copy: they use malloc so the test
 * can COUNT calls and stay free of the rest of the project.  A real chain
 * allocates through util::host_alloc, which is the one the allocation report
 * sees -- anything going out another door does not show up in it, which is
 * the very problem this type exists to fix. */

static size_t g_alpha_allocs = 0;
static size_t g_beta_allocs = 0;

static void *alpha_alloc(size_t n) {
    ++g_alpha_allocs;
    return std::malloc(n);
}
static void alpha_free(void *p) noexcept { std::free(p); }

static void *beta_alloc(size_t n) {
    ++g_beta_allocs;
    return std::malloc(n);
}
static void beta_free(void *p) noexcept { std::free(p); }

static const ByteKind kAlpha = {"alpha", 0x41414141u, 3, 0, alpha_alloc,
                                alpha_free};
static const ByteKind kBeta = {"beta", 0, 0, 0, beta_alloc, beta_free};

/* A chain whose allocation always fails, to check what a buffer does when it
 * runs out of memory. */
static void *never_alloc(size_t) { return nullptr; }
static void never_free(void *) noexcept {}
static const ByteKind kNoMemory = {"no-memory", 0, 0, 0, never_alloc,
                                   never_free};

/* A chain EXTENDS by putting the common part first. */
struct AlphaImage {
    ByteBuffer buf;
    uint32_t entry_point;
};

/* --- checks --------------------------------------------------------------- */

/* A buffer carries its own description, and that description is shared, not
 * copied per buffer. */
static void test_knows_what_it_is() {
    ByteBuffer a, b;
    byte_buffer_init(a, &kAlpha);
    byte_buffer_init(b, &kAlpha);
    CHECK(a.kind == &kAlpha, "the buffer keeps its kind");
    CHECK(a.kind == b.kind, "the descriptor is shared, not copied");
    CHECK(std::strcmp(a.kind->name, "alpha") == 0, "and it can name itself");
    byte_buffer_release(a);
    byte_buffer_release(b);
}

/* Releasing empties the storage but does NOT forget what the buffer is: an
 * emptied buffer can be refilled, and losing its kind along the way would turn
 * it into the anonymous buffer this type exists to prevent. */
static void test_release_keeps_the_kind() {
    ByteBuffer a;
    byte_buffer_init(a, &kAlpha);
    CHECK(byte_buffer_append_u32(a, 0x11223344u), "append ok");
    byte_buffer_release(a);
    CHECK(a.size == 0 && a.data == nullptr, "storage is gone");
    CHECK(a.kind == &kAlpha, "but it still knows what it is");
    CHECK(byte_buffer_append_u32(a, 1), "and can be refilled");
    byte_buffer_release(a);
}

/* Each chain allocates through its OWN function.  This is what the allocation
 * report walks; without it every buffer looks the same. */
static void test_each_chain_allocates_on_its_own() {
    const size_t alpha_before = g_alpha_allocs;
    const size_t beta_before = g_beta_allocs;

    ByteBuffer a, b;
    byte_buffer_init(a, &kAlpha);
    byte_buffer_init(b, &kBeta);
    CHECK(byte_buffer_reserve(a, 1024), "alpha reserve ok");
    CHECK(byte_buffer_reserve(b, 1024), "beta reserve ok");

    CHECK(g_alpha_allocs == alpha_before + 1, "alpha allocated through alpha");
    CHECK(g_beta_allocs == beta_before + 1, "beta allocated through beta");

    byte_buffer_release(a);
    byte_buffer_release(b);
}

/* Integers go out little-endian no matter what the host looks like: writing
 * the host representation produces an artifact that only works on machines
 * resembling the one that produced it, and that does not fail on write. */
static void test_little_endian_on_the_wire() {
    ByteBuffer a;
    byte_buffer_init(a, &kBeta);
    CHECK(byte_buffer_append_u32(a, 0x11223344u), "append u32");
    CHECK(a.size == 4, "four bytes");
    CHECK(a.data[0] == 0x44 && a.data[1] == 0x33 && a.data[2] == 0x22 &&
              a.data[3] == 0x11,
          "least significant byte first");
    byte_buffer_release(a);
}

static void test_round_trip() {
    ByteBuffer a;
    byte_buffer_init(a, &kBeta);
    CHECK(byte_buffer_append_u8(a, 0x7F), "u8");
    CHECK(byte_buffer_append_u16(a, 0xBEEF), "u16");
    CHECK(byte_buffer_append_u32(a, 0xDEADBEEFu), "u32");
    CHECK(byte_buffer_append_u64(a, 0x0123456789ABCDEFull), "u64");

    ByteCursor c = byte_cursor_at_start(a);
    uint8_t v8 = 0;
    uint16_t v16 = 0;
    uint32_t v32 = 0;
    uint64_t v64 = 0;
    byte_cursor_read_u8(c, v8);
    byte_cursor_read_u16(c, v16);
    byte_cursor_read_u32(c, v32);
    byte_cursor_read_u64(c, v64);
    CHECK(c.fault == ByteFault::None, "the whole sequence read cleanly");
    CHECK(v8 == 0x7F && v16 == 0xBEEF && v32 == 0xDEADBEEFu &&
              v64 == 0x0123456789ABCDEFull,
          "every value comes back unchanged");
    CHECK(byte_cursor_left(c) == 0, "and nothing is left over");
    byte_buffer_release(a);
}

/* Once a read runs off the end the cursor stays broken, so a long sequence can
 * be checked once at the end instead of line by line -- which is what nobody
 * does. */
static void test_a_short_read_poisons_the_cursor() {
    ByteBuffer a;
    byte_buffer_init(a, &kBeta);
    CHECK(byte_buffer_append_u8(a, 1), "one byte in");

    ByteCursor c = byte_cursor_at_start(a);
    uint32_t v = 0;
    CHECK(!byte_cursor_read_u32(c, v), "reading four out of one fails");
    CHECK(c.fault == ByteFault::PastEnd, "and it says the reader overran");

    uint8_t v8 = 0;
    CHECK(!byte_cursor_read_u8(c, v8),
          "a later read fails too, even though one byte was there");
    CHECK(byte_cursor_left(c) == 0, "a broken cursor has nothing left");
    byte_buffer_release(a);
}

/* The header check comes out of the buffer, not out of every consumer
 * remembering the right constant. */
static void test_checks_its_own_magic() {
    ByteBuffer a;
    byte_buffer_init(a, &kAlpha);
    /* And the answer is not a yes/no: a buffer too short to hold a header is
     * a truncated file, while wrong bytes mean it is not of this kind at all.
     * Telling them apart is the difference between "resend it" and "this is
     * not what you think it is". */
    CHECK(byte_buffer_check_magic(a) == ByteFault::PastEnd,
          "an empty buffer is truncated, not of the wrong kind");
    CHECK(byte_buffer_append_u32(a, 0x41414141u), "write the magic");
    CHECK(byte_buffer_check_magic(a) == ByteFault::None, "now it matches");

    ByteBuffer b;
    byte_buffer_init(b, &kAlpha);
    CHECK(byte_buffer_append_u32(b, 0xFFFFFFFFu), "write a wrong one");
    CHECK(byte_buffer_check_magic(b) == ByteFault::BadMagic,
          "wrong bytes: not of this kind");

    ByteBuffer c;
    byte_buffer_init(c, &kBeta);
    CHECK(byte_buffer_check_magic(c) == ByteFault::None,
          "a kind with no magic has nothing to check");

    byte_buffer_release(a);
    byte_buffer_release(b);
    byte_buffer_release(c);
}

/* A chain extends by putting the common part first, which is what lets its
 * address be passed where the common part is expected. */
static void test_a_chain_extends_it() {
    AlphaImage img;
    byte_buffer_init(img.buf, &kAlpha);
    img.entry_point = 0x1000;

    CHECK(static_cast<void *>(&img) == static_cast<void *>(&img.buf),
          "the common part is first, so the addresses coincide");
    CHECK(byte_buffer_append_u32(img.buf, 0x41414141u), "fill it as usual");
    CHECK(byte_buffer_check_magic(img.buf) == ByteFault::None,
          "and the generic checks work on it");
    CHECK(img.entry_point == 0x1000, "its own fields are untouched");
    byte_buffer_release(img.buf);
}

/* Growth doubles rather than fitting exactly: these buffers are filled one
 * instruction, one field, one string at a time, and exact growth makes that
 * quadratic. */
static void test_growth_is_amortised() {
    const size_t before = g_beta_allocs;
    ByteBuffer a;
    byte_buffer_init(a, &kBeta);
    for (int i = 0; i < 4096; ++i) byte_buffer_append_u8(a, (uint8_t)i);
    CHECK(a.size == 4096, "everything went in");
    CHECK(g_beta_allocs - before < 12,
          "and it took a handful of allocations, not one per byte");
    byte_buffer_release(a);
}

/* Strings carry their length in front and are NOT nul-terminated, so they can
 * hold nul bytes -- and they do: Vesta string literals are arbitrary UTF-8.
 * Reading hands back a pointer INTO the buffer rather than a copy, because
 * whoever reads thousands of names should not allocate once per name. */
static void test_strings_carry_their_length() {
    ByteBuffer a;
    byte_buffer_init(a, &kBeta);

    const char with_nul[] = {'a', '\0', 'b'};
    CHECK(byte_buffer_append_bytes_with_len(a, "hola", 4), "plain string");
    CHECK(byte_buffer_append_bytes_with_len(a, with_nul, 3), "with a nul");
    CHECK(byte_buffer_append_bytes_with_len(a, "", 0), "empty string");

    ByteCursor c = byte_cursor_at_start(a);
    const char *p = nullptr;
    size_t n = 0;

    CHECK(byte_cursor_read_bytes_with_len(c, &p, &n), "read the first");
    CHECK(n == 4 && std::memcmp(p, "hola", 4) == 0, "and it is unchanged");
    CHECK(p >= (const char *)a.data && p < (const char *)a.data + a.size,
          "it points INTO the buffer, it is not a copy");

    CHECK(byte_cursor_read_bytes_with_len(c, &p, &n), "read the second");
    CHECK(n == 3 && std::memcmp(p, with_nul, 3) == 0,
          "the nul byte inside survives");

    CHECK(byte_cursor_read_bytes_with_len(c, &p, &n), "read the third");
    CHECK(n == 0 && p == nullptr, "an empty string reads back empty");
    CHECK(c.fault == ByteFault::None && byte_cursor_left(c) == 0,
          "and nothing is left over");
    byte_buffer_release(a);
}

/* A length that claims more than is there must fail, not read past the end. */
static void test_a_lying_length_is_caught() {
    ByteBuffer a;
    byte_buffer_init(a, &kBeta);
    CHECK(byte_buffer_append_u32(a, 9999), "a length with no bytes behind it");

    ByteCursor c = byte_cursor_at_start(a);
    const char *p = nullptr;
    size_t n = 0;
    CHECK(!byte_cursor_read_bytes_with_len(c, &p, &n), "the read fails");
    /* And it blames the DATA, not the reader: the length came out of the
     * buffer itself, so a length that lies means a bad artifact -- which is
     * rejected -- and not an overrun, which would mean suspecting the code. */
    CHECK(c.fault == ByteFault::BadLength, "the artifact lied about a length");
    byte_buffer_release(a);
}

/* A buffer that ran out of memory remembers it, exactly like a cursor that
 * read past the end.  That is what lets a long write sequence be checked once
 * at the end instead of per call -- and 118 call sites in the IR serializer
 * alone would otherwise be ignoring a return value. */
static void test_a_failed_write_poisons_the_buffer() {
    ByteBuffer a;
    byte_buffer_init(a, &kNoMemory);
    CHECK(a.fault == ByteFault::None, "starts out fine");
    CHECK(!byte_buffer_append_u32(a, 1), "the write fails");
    CHECK(a.fault == ByteFault::OutOfMemory,
          "and it says the environment ran out, not that the data was bad");
    CHECK(!byte_buffer_append_u8(a, 2), "a later write fails too");
    CHECK(a.size == 0, "and nothing was written");
    CHECK(std::strcmp(byte_fault_name(a.fault), "out-of-memory") == 0,
          "the reason can be told");

    byte_buffer_release(a);
    CHECK(a.fault == ByteFault::None,
          "releasing clears the failure, so it can be reused");
}

/* A borrowed buffer looks at someone else's bytes: a range inside a larger
 * artifact, a packet off the wire, a mapped region.  It is read-only by
 * construction -- its kind cannot allocate -- rather than by convention. */
static void test_borrowed_bytes() {
    const uint8_t raw[] = {0x44, 0x33, 0x22, 0x11, 0x99};

    ByteBuffer b = byte_buffer_borrow(raw, sizeof(raw));
    CHECK(b.size == sizeof(raw), "it sees all of them");
    CHECK(b.data == raw, "without copying anything");

    ByteCursor c = byte_cursor_at_start(b);
    uint32_t v = 0;
    CHECK(byte_cursor_read_u32(c, v), "and reads like any other buffer");
    CHECK(v == 0x11223344u, "with the same result");

    /* Writing must fail rather than touch memory that is not ours. */
    CHECK(!byte_buffer_append_u8(b, 0), "a write is refused");
    CHECK(b.fault == ByteFault::OutOfMemory, "and says so");

    /* Releasing it must not free what belongs to someone else: if this were
     * wrong the test would not fail here, it would corrupt the caller's heap
     * somewhere else entirely. */
    byte_buffer_release(b);
    CHECK(raw[4] == 0x99, "the lender's bytes are untouched");
}

int main() {
    test_knows_what_it_is();
    test_borrowed_bytes();
    test_strings_carry_their_length();
    test_a_lying_length_is_caught();
    test_a_failed_write_poisons_the_buffer();
    test_release_keeps_the_kind();
    test_each_chain_allocates_on_its_own();
    test_little_endian_on_the_wire();
    test_round_trip();
    test_a_short_read_poisons_the_cursor();
    test_checks_its_own_magic();
    test_a_chain_extends_it();
    test_growth_is_amortised();

    std::printf("%s: %d checks, %d failed\n", g_fail == 0 ? "OK" : "FAIL",
                g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
