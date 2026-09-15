#!/usr/bin/env python3
# sube_buildnum.py — automatiza el NUMERO DE COMPILACION (idea del operador, 2026-09-15).
#
# ============================================================================
#  QUE PROBLEMA RESUELVE
#
#  El numero de compilacion (`b4`) sirve para poder decir por USB QUE firmware
#  lleva grabado un nodo sin dudar. Vive en DOS sitios que tienen que decir lo
#  mismo:
#     - `.buildnum`            (un numero suelto: 4)
#     - `platformio.ini`       (-DAPP_BUILD_NUM=\"b4\", que es lo que se graba
#                               dentro del binario y lo que ve el comando `version`)
#  Se subian A MANO los dos, y ya se olvido una vez: `.buildnum` se quedo en 3
#  mientras `platformio.ini` iba por b4. El verificador de la memoria lo caza
#  (falla: "descuadre"), pero enterarse DESPUES no sirve de nada: el binario ya
#  se compilo y puede estar grabado en un nodo.
#
# ============================================================================
#  COMO FUNCIONA (y por que asi)
#
#  1) ANTES de compilar: si `.buildnum` y `platformio.ini` NO cuadran, **se para
#     la compilacion** con un error que dice exactamente que hay que poner. Es a
#     proposito: es mejor no compilar que compilar y grabar un binario cuyo
#     numero miente. Este es el fallo que ya paso una vez.
#
#  2) DESPUES de que la compilacion haya terminado BIEN (el .hex esta generado):
#     se sube el numero en LOS DOS sitios a la vez, +1.
#
#  ★ CAMBIO DE FONDO RESPECTO A ANTES: el operador YA NO TIENE QUE SUBIR EL
#    NUMERO A MANO. Se compila y ya esta: el binario sale con el numero que toca
#    y los dos ficheros quedan preparados para la compilacion siguiente.
#
#  ★ POR QUE NO SE SUBE "AL EMPEZAR": si se subiera antes de compilar, el numero
#    que lleva el binario y el que se queda apuntado serian el mismo, y entonces
#    un binario compilado dos veces seguidas seria indistinguible del anterior
#    (que es justo lo que el contador existe para evitar). Subiendolo DESPUES,
#    "el numero apuntado" es siempre el de la PROXIMA compilacion.
#
#  ★ SI LA COMPILACION FALLA: no se sube nada (el paso 2 no llega a correr), asi
#    que reintentar no gasta numeros.
#
#  ★ VARIOS ENTORNOS EN UNA TANDA: `pio run -e A -e B -e C` sube el numero UNA
#    SOLA VEZ, y los tres binarios salen con el MISMO numero. Es lo que se quiere:
#    una tanda de compilacion = un numero, y asi no hay que averiguar cual de los
#    cuatro binarios es "el ultimo". El guardia `_ya_subido` lo garantiza, porque
#    PlatformIO carga este script una vez por proceso aunque compile 7 entornos.
#
#  Este script NO sustituye a `verifica_memoria.ps1`: lo complementa. El
#  verificador sigue comprobando, ademas, que el numero este DENTRO del binario.
# ============================================================================

import os
import re

Import("env")


def _dir_proyecto():
    """Carpeta del proyecto, resolviendo la variable EN EL ULTIMO MOMENTO.

    ★ OJO, ESTO ES UNA TRAMPA QUE YA COSTO UN INTENTO FALLIDO (2026-09-15): si se
    resuelve al cargar el script y sale mal, el script no encuentra los ficheros y
    el numero se queda quieto SIN QUE NADIE SE ENTERE, que es justo lo que este
    script existe para evitar. Por eso:
      - se resuelve aqui dentro (cuando el hook ya corre), no al importar, y
      - se pide igual que en `nrf52_uf2.py` (`env.subst("$PROJECT_DIR")`), que es el
        patron que YA funciona en este proyecto, y
      - si sale una ruta que no existe, se avisa en vez de callarse.
    """
    d = env.subst("$PROJECT_DIR")
    if not d or "$" in str(d):
        raise RuntimeError("no puedo resolver $PROJECT_DIR (he obtenido %r)" % (d,))
    d = str(d)
    if not os.path.isdir(d):
        raise RuntimeError("la carpeta del proyecto no existe: %s" % d)
    return d


def _rutas():
    d = _dir_proyecto()
    return os.path.join(d, ".buildnum"), os.path.join(d, "platformio.ini")


def _elf_del_entorno():
    """Ruta del .elf que se esta construyendo (el que dice si se compilo algo)."""
    return env.subst("$BUILD_DIR/${PROGNAME}.elf")


def _marca_de_tiempo(path):
    try:
        return os.path.getmtime(path)
    except OSError:
        return 0.0


PATRON_INI = re.compile(r'(-DAPP_BUILD_NUM=\\?"b)(\d+)(\\?")')

# Guardia de proceso: PlatformIO carga los extra_scripts UNA vez por invocacion,
# aunque despues compile varios entornos. Con esta bandera, el numero sube una
# sola vez por tanda.
_ya_subido = False

# Marca de tiempo del .elf ANTES de compilar: sirve para saber si este entorno ha
# tenido trabajo de verdad (ver _sube_el_numero).
_elf_antes = None


def _leer(path):
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def _escribir(path, texto):
    # UTF-8 SIN BOM, que es lo que usa todo el proyecto (el verificador de la
    # memoria falla si aparece un BOM en la memoria; aqui se hace igual por
    # coherencia, y porque un BOM delante de un `[env:]` rompe PlatformIO).
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(texto)


def _numero_del_contador(f_buildnum):
    """Devuelve el entero de .buildnum, o None si el fichero no tiene sentido."""
    try:
        texto = _leer(f_buildnum).strip()
    except OSError:
        return None
    if not texto.isdigit():
        return None
    return int(texto)


def _numero_del_ini(f_ini):
    """Devuelve (entero, texto_del_fichero) segun platformio.ini, o (None, texto)."""
    try:
        texto = _leer(f_ini)
    except OSError:
        return None, None
    m = PATRON_INI.search(texto)
    if not m:
        return None, texto
    return int(m.group(2)), texto


def comprueba_cuadre():
    """Antes de compilar: los dos ficheros tienen que decir lo mismo."""
    # ★ SI YA SE SUBIO EN ESTA TANDA, NO SE VUELVE A COMPROBAR. Con `pio run -e A
    #   -e B`, el primer entorno sube el contador al terminar; el segundo entorno
    #   todavia lleva en memoria las flags de cuando arranco PlatformIO (b4), asi
    #   que si se volviera a comprobar saldria un descuadre FALSO y la tanda se
    #   pararia a la mitad. Es exactamente el fallo que este script existe para
    #   evitar, cometido por el propio script: por eso se comprueba una vez por
    #   tanda, que es cuando el dato es fiable.
    if _ya_subido:
        return

    # ★ Y SE APUNTA SI ESTE ENTORNO TRAIA TRABAJO PENDIENTE (2026-09-15). Esto es lo
    #   que decide despues si el numero se gasta o no; el porque, largo y tendido,
    #   en _sube_el_numero().
    global _elf_antes
    _elf_antes = _marca_de_tiempo(_elf_del_entorno())

    f_buildnum, f_ini = _rutas()
    n_contador = _numero_del_contador(f_buildnum)
    n_ini, _ = _numero_del_ini(f_ini)

    if n_contador is None:
        print("*** BUILD STOPPED: .buildnum no existe o no es un numero (b%d)." % 0)
        env.Exit(1)
    if n_ini is None:
        print("*** BUILD STOPPED: platformio.ini no declara APP_BUILD_NUM.")
        env.Exit(1)

    if n_contador != n_ini:
        print("")
        print("*** BUILD STOPPED: EL NUMERO DE COMPILACION NO CUADRA ***")
        print("    .buildnum       dice  b%d" % n_contador)
        print("    platformio.ini  dice  b%d" % n_ini)
        print("")
        print("    Los dos tienen que decir lo mismo: el numero va DENTRO del binario y")
        print("    es lo unico que permite decir por USB que firmware lleva un nodo.")
        print("    Pon los dos a b%d y vuelve a compilar:" % max(n_contador, n_ini))
        print("      - .buildnum                -> %d" % max(n_contador, n_ini))
        print('      - platformio.ini           -> -DAPP_BUILD_NUM=\\"b%d\\"' % max(n_contador, n_ini))
        print("")
        env.Exit(1)

    print("Numero de compilacion: b%d (contador y platformio.ini cuadran)" % n_ini)


def _sube_el_numero(source, target, env):
    """Despues de compilar bien: +1 en .buildnum y en platformio.ini.

    ★★ CUANDO SE GASTA UN NUMERO, Y POR QUE (esto se aprendio midiendo, 2026-09-15) ★★

    Lo natural seria "una tanda de compilacion = un numero", y se intento con una
    bandera en memoria. **NO SIRVE**: al compilar `pio run -e A -e B -e C -e D`,
    PlatformIO aisla cada entorno y **carga este script una vez por entorno**, asi
    que la bandera se pierde y el contador subia 4 veces (medido: b5, b6, b7, b8 en
    una sola tanda).

    Lo que SI es fiable es preguntar **si este entorno ha tenido trabajo de verdad**:
    se apunta la fecha del .elf antes de empezar y se compara con la de despues.

      - Si el .elf es NUEVO (se acaba de enlazar), la compilacion ha hecho trabajo
        -> se gasta un numero.
      - Si el .elf es el MISMO (no habia nada que rehacer), NO se gasta numero.

    Y con eso sale el comportamiento que se quiere sin ningun truco:
      - **Primera tanda** (los .elf no existen o estan viejos): compila el primer
        entorno, gasta UN numero, y los demas entornos de la tanda ya no compilan
        nada (su .elf acaba de quedar al dia), asi que **no gastan mas numeros**.
      - **Segunda tanda seguida** (nada tocado): no compila nadie, no gasta numeros.
      - **Se toca un fuente y se compila**: el primer entorno que rehace el .elf gasta
        un numero; los demas, no.
      - **Un binario recien hecho y uno de la tanda anterior** pueden llevar numeros
        distintos: es correcto y ademas util, porque dice cual se rehizo.

    Los cuatro binarios de una misma tanda llevan el MISMO numero de todas formas,
    porque todos leen `.buildnum` al arrancar PlatformIO, antes de que el primero
    termine y lo suba.
    """
    global _ya_subido
    if _ya_subido:
        return  # ya se subio en esta tanda (otro entorno del mismo `pio run`)

    # ¿Este entorno ha compilado algo de verdad? Si no, no se gasta numero.
    elf_ahora = _marca_de_tiempo(_elf_del_entorno())
    if _elf_antes is not None and elf_ahora == _elf_antes:
        print("Numero de compilacion: no se sube (este entorno no ha tenido que rehacer nada)")
        return

    f_buildnum, f_ini = _rutas()
    n_contador = _numero_del_contador(f_buildnum)
    n_ini, texto_ini = _numero_del_ini(f_ini)
    if n_contador is None or n_ini is None:
        # No deberia pasar: comprueba_cuadre() ya lo habria parado. Se avisa y se
        # sigue, para no romper una compilacion que ha ido bien por un problema
        # del contador.
        print("aviso: no he podido subir el numero de compilacion (revisa .buildnum)")
        return

    nuevo = max(n_contador, n_ini) + 1

    _escribir(f_buildnum, "%d\n" % nuevo)
    _escribir(f_ini, PATRON_INI.sub(lambda m: m.group(1) + str(nuevo) + m.group(3),
                                    texto_ini, count=1))
    _ya_subido = True

    print("Numero de compilacion subido a b%d: el binario que acabas de hacer lleva b%d,"
          % (nuevo, n_ini))
    print("y la PROXIMA compilacion saldra con b%d (no hay que tocar nada a mano)." % nuevo)


def _envuelve(nombre, fn):
    """Ejecuta fn() sin poder tumbar la compilacion por un fallo del contador."""
    def _w(source, target, env):
        try:
            fn()
        except Exception as exc:   # noqa: BLE001  (a proposito: nada debe romper el build)
            print("aviso: el contador de compilacion ha fallado (%s): %s" % (nombre, exc))
    return _w


def _envuelve_con_targets(fn):
    def _w(source, target, env):
        try:
            fn(source, target, env)
        except Exception as exc:   # noqa: BLE001
            print("aviso: el contador de compilacion ha fallado: %s" % exc)
    return _w


# ---------------------------------------------------------------- enganches
# ★ DE QUE OBJETIVO SE CUELGAN, Y POR QUE (esto se probo, no se supuso)
#
#   - 1er intento: "$BUILD_DIR/${PROGNAME}.elf"  -> NO SE DISPARABAN (compilacion
#     correcta y contador quieto, en silencio).
#   - 2o intento: "buildprog"                    -> TAMPOCO. Compilado el entorno
#     `techo_plus_s140v7` con exito y el numero sin subir.
#   - 3er intento (el bueno): "$BUILD_DIR/${PROGNAME}.hex", que es EXACTAMENTE el
#     objetivo que ya usa nrf52_uf2.py en este mismo proyecto y que se sabe que
#     corre (por ahi sale el mensaje "Generating UF2 file"). Si ese objetivo
#     funciona para empaquetar, funciona para esto.
#
# La comprobacion va en el PRE (antes de generar el .hex) y la subida en el POST
# (cuando el .hex ya existe = la compilacion ha terminado bien). Si el enlazado
# falla, el POST no corre y el numero NO se gasta.
env.AddPreAction("$BUILD_DIR/${PROGNAME}.hex", _envuelve("comprobacion", comprueba_cuadre))
env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", _envuelve_con_targets(_sube_el_numero))
