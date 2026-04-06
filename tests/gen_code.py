import os

# Número de archivos a generar
N = 10000

# Carpeta donde se guardarán
OUTPUT_DIR = "generated_vel_files"

# Crear carpeta si no existe
os.makedirs(OUTPUT_DIR, exist_ok=True)

# Contenido del archivo .vel
VEL_TEMPLATE = """@Format("velb")

@SpaceAddress {
    @Name("anonymous"),
    @IniAddress(0x0000000000001000),
    @EndAddress(0xFFFFFFFFFFFFFFFF)
}

@Section {
    @Name("all"),
    @SpaceAddress("anonymous")
}
code:
    @InitPc(code)

divs r03b,  r01b
divs r03w,  r01w
divs r03d,  r01d
divs r03,   r01

divs r03b,  r01b
divs r03w,  r01w
divs r03d,  r01d
divs r03,   r01

divs r03b,  r01b
divs r03w,  r01w
divs r03d,  r01d
divs r03,   r01

divs r03b,  r01b
divs r03w,  r01w
divs r03d,  r01d
divs r03,   r01

divs r03b,  r01b
divs r03w,  r01w
divs r03d,  r01d
divs r03,   r01

divs r03b,  r01b
divs r03w,  r01w
divs r03d,  r01d
divs r03,   r01

divs r03b,  r01b
divs r03w,  r01w
divs r03d,  r01d
divs r03,   r01

divs r03b,  r01b
divs r03w,  r01w
divs r03d,  r01d
divs r03,   r01

divs r03b,  r01b
divs r03w,  r01w
divs r03d,  r01d
divs r03,   r01

divs r03b,  r01b
divs r03w,  r01w
divs r03d,  r01d
divs r03,   r01

divs r03b,  r01b
divs r03w,  r01w
divs r03d,  r01d
divs r03,   r01
"""

print(f"Generando {N} archivos .vel en '{OUTPUT_DIR}'...")

for i in range(1, N + 1):
    filename = f"file_{i:05d}.vel"
    path = os.path.join(OUTPUT_DIR, filename)

    with open(path, "w", encoding="utf-8") as f:
        f.write(VEL_TEMPLATE)

print("¡Listo! Archivos generados correctamente.")
