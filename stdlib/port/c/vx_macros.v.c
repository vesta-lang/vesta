// @vx-snippet: vx_macros
// @vx-requires:
// @vx-includes:
// @vx-freestanding-skip: no

#if defined(__GNUC__) || defined(__clang__)
#define VX_RESTRICT __restrict__
#define VX_HOT __attribute__((hot))
#define VX_COLD __attribute__((cold))
#define VX_UNUSED __attribute__((unused))
#define VX_NORETURN __attribute__((noreturn))
#define VX_UNREACHABLE __builtin_unreachable()
#else
#define VX_RESTRICT
#define VX_HOT
#define VX_COLD
#define VX_UNUSED
#define VX_NORETURN
#define VX_UNREACHABLE ((void)0)
#endif

#ifndef VX_TLS
#if defined(__GNUC__) || defined(__clang__)
#define VX_TLS __thread
#elif defined(_MSC_VER)
#define VX_TLS __declspec(thread)
#else
#define VX_TLS
#endif
#endif
