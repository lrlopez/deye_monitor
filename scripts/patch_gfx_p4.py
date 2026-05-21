Import("env")
import os, shutil

# Aplica los parches de la GFX Library necesarios para ESP32-P4 (Guition JC1060P470):
#   - Arduino_ESP32DSIPanel.h: añade getPanelHandle() para flush con esp_lcd_panel_draw_bitmap
#   - Arduino_ESP32DSIPanel.cpp: num_fbs=2 + use_dma2d=true para doble buffer sin tearing
#
# Se ejecuta como pre-script antes de la compilación. La librería ya está instalada
# en $PROJECT_LIBDEPS_DIR gracias al mecanismo de dependencias de PlatformIO.

libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
pioenv      = env.subst("$PIOENV")
project_dir = env.subst("$PROJECT_DIR")

gfx_dir     = os.path.join(libdeps_dir, pioenv, "GFX Library for Arduino", "src", "databus")
patches_dir = os.path.join(project_dir, "patches")

if os.path.isdir(gfx_dir):
    for fname in ("Arduino_ESP32DSIPanel.h", "Arduino_ESP32DSIPanel.cpp"):
        src = os.path.join(patches_dir, fname)
        dst = os.path.join(gfx_dir, fname)
        if os.path.isfile(src):
            shutil.copy2(src, dst)
            print(f"[patch_gfx_p4] Aplicado: {fname}")
else:
    print(f"[patch_gfx_p4] AVISO: {gfx_dir} no existe — ¿librería no instalada aún?")
