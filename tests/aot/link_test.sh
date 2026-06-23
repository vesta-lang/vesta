#!/bin/bash
# Phase AOT.5 -- validacion del linker propio (vm --link, sin ld/gcc para el
# enlace).  Tres escenarios:
#   A) un solo .o Vex -> ejecutable hosted (_start sintetico -> main).
#   B) .o Vex (extern) + .o de C (gcc) -> resuelve el simbolo cross-file.
#   C) --entry custom (kernel/bootloader: sin main, sin stub) -> entry propio.
# Mas valgrind sobre (A).
#
# Se corre desde WSL/Linux:  wsl bash ./tests/aot/link_test.sh <build_dir>
# (entorno uniforme: 'vm' nativo + gcc + el ELF resultante ejecutable).
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm"; [ -x "$VM" ] || VM="$BUILD/vm.exe"
WORK="$(mktemp -d)"
rc=0

# --- A) un solo .o Vex -> exec (hosted) ---
cat > "$WORK/a.vex" <<'EOF'
i64 triple(i64 x) { return x * 3; }
i64 main() { return triple(14); }
EOF
"$VM" --vex "$WORK/a.vex" -m aot --emit obj --format elf -o "$WORK/a.o" >/dev/null 2>&1
"$VM" --link "$WORK/a.o" -o "$WORK/a.elf" --format elf >/dev/null 2>&1
if [ -x "$WORK/a.elf" ]; then
  "$WORK/a.elf"; got=$?
  if [ "$got" = "42" ]; then echo "LINK A (un .o): exit=42 OK"
  else echo "LINK A: EXIT MISMATCH got=$got exp=42"; rc=1; fi
  valgrind --error-exitcode=88 -q "$WORK/a.elf"
  [ $? = 88 ] && { echo "  VALGRIND A FALLO"; rc=1; } || echo "  VALGRIND A OK"
else echo "LINK A: no se genero el ejecutable"; rc=1; fi

# --- B) .o Vex (extern) + .o de C (gcc) -> cross-file ---
cat > "$WORK/kern.vex" <<'EOF'
extern "rt" { fn rt_value() -> i64; }
i64 main() { return rt_value() + 2; }
EOF
cat > "$WORK/rt.c" <<'EOF'
long rt_value(void) { return 40; }
EOF
"$VM" --vex "$WORK/kern.vex" -m aot --emit obj --format elf -o "$WORK/kern.o" >/dev/null 2>&1
gcc -c -fno-pic "$WORK/rt.c" -o "$WORK/rt.o" 2>/dev/null
"$VM" --link "$WORK/kern.o" "$WORK/rt.o" -o "$WORK/prog.elf" --format elf >/dev/null 2>&1
if [ -x "$WORK/prog.elf" ]; then
  "$WORK/prog.elf"; got=$?
  if [ "$got" = "42" ]; then echo "LINK B (Vex .o + C .o cross-file): exit=42 OK"
  else echo "LINK B: EXIT MISMATCH got=$got exp=42"; rc=1; fi
else echo "LINK B: no se genero el ejecutable"; rc=1; fi

# --- C) --entry custom (dev-OS: sin main, sin stub) ---
cat > "$WORK/boot.c" <<'EOF'
void _kstart(void) {
    __asm__ volatile("mov $60, %%rax; mov $42, %%rdi; syscall" ::: "rax","rdi");
}
EOF
gcc -c -ffreestanding -fno-pic "$WORK/boot.c" -o "$WORK/boot.o" 2>/dev/null
"$VM" --link "$WORK/boot.o" -o "$WORK/boot.elf" --format elf --entry _kstart >/dev/null 2>&1
if [ -x "$WORK/boot.elf" ]; then
  "$WORK/boot.elf"; got=$?
  if [ "$got" = "42" ]; then echo "LINK C (--entry _kstart, sin main/stub): exit=42 OK"
  else echo "LINK C: EXIT MISMATCH got=$got exp=42"; rc=1; fi
else echo "LINK C: no se genero el ejecutable"; rc=1; fi

# --- D) multi-.o Vex: libreria sin main + app que la referencia (extern) ---
cat > "$WORK/lib.vex" <<'EOF'
i64 quad(i64 x) { return x * 4; }
i64 dec(i64 x) { return x - 1; }
EOF
cat > "$WORK/app.vex" <<'EOF'
extern "lib" { fn quad(i64 x) -> i64; fn dec(i64 x) -> i64; }
i64 main() { return dec(quad(11)) - 1; }
EOF
"$VM" --vex "$WORK/lib.vex" -m aot --emit obj --format elf -o "$WORK/lib.o" >/dev/null 2>&1
"$VM" --vex "$WORK/app.vex" -m aot --emit obj --format elf -o "$WORK/app.o" >/dev/null 2>&1
"$VM" --link "$WORK/app.o" "$WORK/lib.o" -o "$WORK/app.elf" --format elf >/dev/null 2>&1
if [ -x "$WORK/app.elf" ]; then
  "$WORK/app.elf"; got=$?
  if [ "$got" = "42" ]; then echo "LINK D (multi-.o Vex: lib sin main + app): exit=42 OK"
  else echo "LINK D: EXIT MISMATCH got=$got exp=42"; rc=1; fi
else echo "LINK D: no se genero el ejecutable"; rc=1; fi

# --- E) .bss: global sin inicializar (NOBITS) escrito/leido ---
cat > "$WORK/bss.c" <<'EOF'
static long g;   /* .bss */
void _kstart(void) {
    g = 42; long r = g;
    __asm__ volatile("mov %0,%%rdi; mov $60,%%rax; syscall" :: "r"(r) : "rax","rdi");
}
EOF
gcc -c -ffreestanding -fno-pic -fno-asynchronous-unwind-tables "$WORK/bss.c" -o "$WORK/bss.o" 2>/dev/null
"$VM" --link "$WORK/bss.o" -o "$WORK/bss.elf" --format elf --entry _kstart >/dev/null 2>&1
if [ -x "$WORK/bss.elf" ]; then
  "$WORK/bss.elf"; got=$?
  if [ "$got" = "42" ]; then echo "LINK E (.bss global escrito/leido): exit=42 OK"
  else echo "LINK E: EXIT MISMATCH got=$got exp=42"; rc=1; fi
else echo "LINK E: no se genero el ejecutable"; rc=1; fi

rm -rf "$WORK"
[ $rc = 0 ] && echo "AOT.5 linker: TODOS OK" || echo "AOT.5 linker: FALLOS"
exit $rc
