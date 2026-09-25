/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_interned_name.cpp
 * @brief What an interned name promises, checked.
 *
 * The type exists because a bare `const std::string*` compiles when streamed
 * and prints an ADDRESS instead of the name, with no warning: nine traces did
 * exactly that when the module paths were interned.  So the promises below are
 * the point of the type, and the ones about conversions are checked at compile
 * time -- if someone adds an implicit conversion, this file stops compiling.
 */

#include "util/name_pool.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_set>

using util::InternedName;

/* No implicit way back to a raw pointer or to a truth value: those are the
 * conversions that let a name be printed as an address or tested as "set". */
static_assert(!std::is_convertible<InternedName, const std::string *>::value,
              "an interned name must not decay to a pointer");
static_assert(!std::is_convertible<InternedName, bool>::value,
              "an interned name must not convert to bool");
static_assert(!std::is_convertible<InternedName, std::string>::value,
              "reading the text must be explicit (str())");
static_assert(sizeof(InternedName) == sizeof(void *),
              "an interned name is one pointer, passed by value");

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);        \
        }                                                                      \
    } while (0)

int main() {
    const InternedName a = InternedName::intern("std.collections");
    const InternedName b = InternedName::intern(std::string("std.") +
                                                "collections");
    const InternedName c = InternedName::intern("std.io");

    CHECK(a == b, "the same text interned twice is the same name");
    CHECK(a != c, "different text is a different name");
    CHECK(a.ptr() == b.ptr(), "and equality is by identity");
    CHECK(a.str() == "std.collections", "str() gives the text");

    /* The reason for the type: streaming prints the TEXT. */
    std::ostringstream os;
    os << a << "|" << c;
    CHECK(os.str() == "std.collections|std.io",
          "streaming prints the name, not its address");

    /* One identity for the empty name, whichever way it is reached. */
    const InternedName empty_default;
    const InternedName empty_interned = InternedName::intern("");
    const InternedName empty_adopted = InternedName::from_interned(nullptr);
    CHECK(empty_default == empty_interned,
          "the empty name has one identity: default == interned");
    CHECK(empty_default == empty_adopted,
          "and adopting a null pointer gives that same empty name");
    CHECK(empty_default.empty() && empty_default.str().empty(),
          "the default name is empty, never null");

    /* Usable as a key by identity. */
    std::unordered_set<InternedName, util::InternedNameHash> set;
    set.insert(a);
    set.insert(b);
    set.insert(c);
    CHECK(set.size() == 2, "a hash set keys by identity");

    std::printf("=== interned name: %d checks, %d failures ===\n", g_checks,
                g_fail);
    return g_fail == 0 ? 0 : 1;
}
