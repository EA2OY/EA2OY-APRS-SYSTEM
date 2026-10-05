#!/usr/bin/env python3
# nrf52_uf2.py — post-build UF2 packaging (recipe N-06).
# Converts the app HEX to Adafruit nRF52 UF2 (family 0xADA52840).
# Bootloader limit: app must stay below 0xEA000 (~794 KB / ~3103 UF2 blocks).
# It also guards the flash-log region (0xC8000..0xE7FFF): if the application
# ever grows into it the build fails instead of corrupting stored trips.

import sys
from os.path import basename

Import("env")

FLASH_LOG_BASE = 0xC8000
BOOTLOADER_BASE = 0xEA000

# ★★★ ZONA DE TRACKS (2026-09-22): 0x098000..0x0C4FFF ★★★
# El track en vivo (80 KB, 20 paginas) y las 5 ranuras de tracks cargados (20 KB cada una).
# Estas direcciones estan REPETIDAS en `src/tracks.h`: si se cambia una, hay que cambiar la otra.
#
# ★ POR QUE EMPIEZA EN 0x098000 Y NO PEGADO A LA APLICACION: el primer intento fue pegarlo
#   (0x082000) y esta misma guarda dijo que a la aplicacion le quedaban **1,4 KB** de margen:
#   el menu y la pantalla de navegacion habrian acabado pisando los tracks del usuario. Con
#   0x098000 quedan 89 KB de margen para la aplicacion, y encima NO se le quita sitio al
#   registro de viaje (que sigue en 0x0C8000 con sus 32 paginas).
#
# ★ OJO, ESTA ZONA NO ES DEL REGISTRO DE VIAJE: el registro (0xC8000..0xE7FFF) es un ANILLO y
#   se mueve por todo su rango, por eso los tracks van ANTES y no dentro.
FLASH_TRACKS_BASE = 0x098000
# ★ 0x0C6000 y no 0x0C5000 (2026-09-22): la zona crecio UNA pagina de 4 KB. La cabecera de fecha
#   del track en vivo tiene que ocupar SU PROPIA pagina ALINEADA, porque `flashBorraPagina()` usa
#   ERASEPAGE, que borra la pagina que CONTIENE la direccion: con la cabecera en los primeros 64
#   bytes y los puntos detras, cada cambio de pagina borraba 64 bytes de la anterior y las
#   ranuras se pisaban entre si. Lo cazo una revision independiente contando las paginas.
#   Reparto: cabecera 1 pagina + 20 paginas de puntos + 5 ranuras de 5 paginas = 0xC6000.
FLASH_TRACKS_END = 0x0C6000


def board_has_flash_log(env):
    """¿Esta placa usa la zona del registro de viaje (0xC8000..0xE7FFF)?

    El registro de viaje es nuestro y vive en esa zona en TODAS las placas, pero
    se deja desactivable por si alguna trae el mapa de memoria distinto: en ese
    caso el tope que aplica es solo el del bootloader.
    """
    try:
        flags = env.get("BUILD_FLAGS", [])
    except Exception:
        return True
    for f in flags:
        if "KACHO_NO_FLASH_LOG" in str(f):
            return False
    return True


def board_has_tracks(env):
    """¿Esta placa lleva la zona de TRACKS (0x98000..0xC4FFF)?

    ★ SOLO EL T-ECHO, y no por capricho: el mapa de esa zona se calculo sobre el reparto del
      T-Echo y se comprobo que esas direcciones estaban libres alli. En las Faketec no se ha
      comprobado, asi que **no existe**: `src/tracks.cpp` se compila a vacio en esas placas
      (ver el `#if` de tracks.h). Por eso aqui tampoco se les puede aplicar la guarda ni
      anunciarles una zona que no tienen.

    ★ SE MIRA EL NOMBRE DE LA PLACA, que es lo mismo que decide cual `boards/*.json` se usa, y
      es lo que de verdad separa a los dos T-Echo del resto. (El primer intento uso
      `board_has_flash_log`, que devuelve True en todas, y por eso el mensaje de los tracks
      salia tambien en las Faketec: un aviso de algo que alli no existe.)
    """
    try:
        board = str(env.get("BOARD", ""))
    except Exception:
        return False
    return board.startswith("techo-nrf52840")


def hex_highest_addr(path):
    """Highest byte address used by an Intel HEX file."""
    highest = 0
    base = 0
    with open(path, "r") as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith(":"):
                continue
            count = int(line[1:3], 16)
            addr = int(line[3:7], 16)
            rectype = int(line[7:9], 16)
            if rectype == 2:  # extended segment address (<< 4): what this toolchain emits
                base = int(line[9:13], 16) << 4
            elif rectype == 4:  # extended linear address (<< 16)
                base = int(line[9:13], 16) << 16
            elif rectype == 0 and count > 0:
                end = base + addr + count
                if end > highest:
                    highest = end
    return highest


def nrf52_hex_to_uf2(source, target, env):
    hex_path = target[0].get_abspath()
    uf2_path = hex_path.replace(".hex", ".uf2")

    top = hex_highest_addr(hex_path)
    con_registro = board_has_flash_log(env)
    con_tracks = board_has_tracks(env)

    # ★ PRIMERO la guarda de los TRACKS, y SOLO en las placas que los tienen: es la zona que
    #   esta mas cerca de la aplicacion, asi que es la que salta antes y la que evita que un dia
    #   la app pise los tracks del usuario.
    if con_registro and con_tracks and top > FLASH_TRACKS_BASE:
        sys.stderr.write(
            "\n*** BUILD STOPPED: the application reaches 0x%X, which runs into the TRACK "
            "region (starts at 0x%X). The live track and the 5 loaded-track slots live there, "
            "so flashing this would corrupt the user's tracks.\n"
            "    -> Move the track map (src/tracks.h AND this script) or trim the firmware.\n"
            % (top, FLASH_TRACKS_BASE)
        )
        env.Exit(1)

    if con_registro and top > FLASH_LOG_BASE:
        sys.stderr.write(
            "\n*** BUILD STOPPED: the application reaches 0x%X, which runs "
            "into the flash-log region (0x%X). Trim the firmware or move the "
            "log region before flashing.\n" % (top, FLASH_LOG_BASE)
        )
        env.Exit(1)
    if top > BOOTLOADER_BASE:
        sys.stderr.write("\n*** BUILD STOPPED: app over the bootloader at 0x%X\n"
                         % BOOTLOADER_BASE)
        env.Exit(1)
    if con_registro and con_tracks:
        print("Flash map: app ends at 0x%X, tracks 0x%X..0x%X, trip log 0x%X..0xE7FFF "
              "(headroom for the app: %.1f KB)"
              % (top, FLASH_TRACKS_BASE, FLASH_TRACKS_END - 1, FLASH_LOG_BASE,
                 (FLASH_TRACKS_BASE - top) / 1024.0))
    elif con_registro:
        print("Flash map: app ends at 0x%X, trip log 0x%X..0xE7FFF (%.1f KB free)"
              % (top, FLASH_LOG_BASE, (FLASH_LOG_BASE - top) / 1024.0))
    else:
        print("Flash map: app ends at 0x%X, bootloader at 0x%X (%.1f KB free)"
              % (top, BOOTLOADER_BASE, (BOOTLOADER_BASE - top) / 1024.0))

    env.Execute(
        env.VerboseAction(
            '"%s" "$PROJECT_DIR/bin/uf2conv.py" "%s" -c -f 0xADA52840 -o "%s"'
            % (sys.executable, hex_path, uf2_path),
            "Generating UF2 file from %s" % basename(hex_path),
        )
    )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", nrf52_hex_to_uf2)
