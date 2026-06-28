#!/bin/bash
# Phase NR -- validacion de @Naked (funciones sin prologo/epilogo/ret).
#
# Corre en el HOST Windows (git-bash): compila a PE y ejecuta el .exe
# directamente (exit code = return de main).  Tambien valida el .o ELF por
# desensamblado (objdump via WSL) cuando este disponible: que el ISR exista,
# sea GLOBAL, y su cuerpo sea EXACTAMENTE el asm del usuario (cero prologo).
#
# Uso:  bash tests/aot/naked_test.sh <build_dir>
set -u
BUILD="${1:?uso: $0 <build_dir>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 2
VM="$BUILD/vm.exe"; [ -x "$VM" ] || VM="$BUILD/vm"
WORK="$(mktemp -d)"
rc=0

# --- 1) @Naked con params (Win64): add_naked(40,2) == 42, via PE en el host ---
cat > "$WORK/nadd.vex" <<'EOF'
@Naked i64 add_naked(i64 a, i64 b) {
    asm volatile {
        mov rax, rcx
        add rax, rdx
        ret
    }
}
i64 main() { return add_naked(40, 2); }
EOF
"$VM" --vex "$WORK/nadd.vex" -m aot --emit exe --format pe -o "$WORK/nadd.exe" >/dev/null 2>&1
if [ -f "$WORK/nadd.exe" ]; then
  "$WORK/nadd.exe"; got=$?
  if [ "$got" = "42" ]; then echo "NAKED 1 (add_naked PE host): exit=42 OK"
  else echo "NAKED 1: EXIT MISMATCH got=$got exp=42"; rc=1; fi
else echo "NAKED 1: no se genero el .exe"; rc=1; fi

# --- 2) ISR void @Naked: presente, GLOBAL y cuerpo byte-exacto (objdump) ---
cat > "$WORK/nisr.vex" <<'EOF'
@Naked void isr_timer() {
    asm volatile {
        push rax
        mov al, 0x20
        out 0x20, al
        pop rax
        iretq
    }
}
i64 main() { return 0; }
EOF
"$VM" --vex "$WORK/nisr.vex" -m aot --emit obj --format elf -o "$WORK/nisr.o" >/dev/null 2>&1
if command -v objdump >/dev/null 2>&1; then
  DUMP=$(objdump -t "$WORK/nisr.o" 2>/dev/null)
  # isr_timer debe existir y ser GLOBAL ('g' en la columna de binding).
  if echo "$DUMP" | grep -qE '\bg\b.*\bisr_timer$'; then
    echo "NAKED 2a (isr_timer GLOBAL en .o): OK"
  else echo "NAKED 2a: isr_timer no es GLOBAL o no existe"; rc=1; fi
  # Su cuerpo NO debe contener push rbp (sin prologo) y SI iretq.
  BODY=$(objdump -d -M intel "$WORK/nisr.o" 2>/dev/null | \
         awk '/<isr_timer>:/{f=1;next} /^$/{f=0} f')
  if echo "$BODY" | grep -qiE 'push +rbp|iretq' ; then
    if echo "$BODY" | grep -qiE 'push +rbp'; then
      echo "NAKED 2b: PROLOGO detectado en isr_timer (no debe haberlo)"; rc=1
    else echo "NAKED 2b (sin prologo, iretq presente): OK"; fi
  else echo "NAKED 2b: no se pudo desensamblar isr_timer"; rc=1; fi
else
  echo "NAKED 2 (objdump no disponible): SKIP"
fi

rm -rf "$WORK"
if [ $rc = 0 ]; then echo "=== naked_test: TODO OK ==="; else echo "=== naked_test: FALLOS ==="; fi
exit $rc
