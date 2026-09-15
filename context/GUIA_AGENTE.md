# GUÍA DEL AGENTE — Faketec_APRS_Igate_EA2OY

Punto de entrada para cualquier agente (persona o IA) que retome este proyecto.
**Léelo entero antes de tocar nada**: son dos minutos.

> Reorganizado el **2026-09-14**. La memoria dejó de ser un fichero que crecía sin
> límite (`cerebro.md`) y pasó a ser **tres capas**.

---

## 1. Qué leer, en este orden

### Obligatorio (siempre, y es corto: unos 4.200 tokens entre los dos)

| # | Fichero | Qué es |
|---|---|---|
| 1 | `REGLAS.md` | **Capa 1**: qué es el proyecto, las reglas de oro, la **lista negra** (lo que no se toca), las cifras verificadas, cómo se compila y se comprueba, y el mapa de dónde vive cada cosa |
| 2 | `ESTADO.md` | **Capa 2**: el **único** documento de estado. Qué hay grabado en el nodo, qué está hecho, qué está abierto, qué está bloqueado |

Con esos dos ya se puede trabajar. **No hace falta leer nada más para empezar.**

### Cuando el caso lo pida (Capa 3: se busca, NO se lee entera)

| Dónde | Qué hay |
|---|---|
| `_memoria\historia.md` | El histórico completo de sesiones (el antiguo `cerebro.md`, **íntegro y sin modificar**). Se busca por el número de entrada —van de la más nueva a la más antigua— o por la cadena que se recuerde |
| `_memoria\01_subnotas\` | Contexto inicial del proyecto. **Histórico**: describe la etapa en que no había código |
| `_memoria\02_neuronas\` | Conocimiento de hardware y patrones (pinout, radios, energía, sensores, OLED, compilación, apps) |
| `_memoria\PENDIENTE.md` | Tareas apuntadas por el operador |
| `_archivo\backups\` | Copias de rescate. **No se leen** |

### Documentación viva del proyecto

Viven en `..\Faketec_APRS_Igate_EA2OY\docs\` y **no entran en el orden obligatorio**:

`MANUAL_DE_USO.md` (manual para el operador) · `FUNCIONES.md` (qué sabe hacer el nodo) ·
`protocol_config_v1.md` (protocolo USB) · `USO_DEL_AIRE.md` (normas y ocupación del canal) ·
`APP_PROPIA_Y_COMPATIBILIDAD.md` (visión de la app futura: **no es normativo**).

---

## 2. Las reglas que se incumplen solas (resumen; el detalle está en `REGLAS.md`)

1. **Si un documento dice X y el código dice Y, GANA EL CÓDIGO** y el documento se corrige en ese momento.
2. **Referencias por nombre, nunca por número de línea** (`gpsManage()`, no «el fichero tal, línea cual»).
3. **Marca cada afirmación**: `[VERIFICADO]`, `[DEDUCIDO]`, `[OPERADOR]` (dato que solo sabe él) o `[NO CONSTA]`. Si no lo sabes, di que no lo sabes.
4. **Edita UTF-8 con la herramienta de edición, nunca con PowerShell.** Los reemplazos de una línea ya rompieron el código tres veces.
5. **No toques**: la configuración del nodo, su modo, el puerto serie de la iGate de casa, los repositorios del operador que son de solo lectura, ni los `.off` del Bluetooth.
6. **Dos fases**: primero el plan, luego la confirmación del operador, luego la ejecución.

---

## 3. La comprobación mecánica de que la memoria sigue sana

```
cd ..\Faketec_APRS_Igate_EA2OY
powershell -File tools\verifica_memoria.ps1
```

Devuelve **0** si todo está bien. Comprueba que existan las capas obligatorias, que haya un solo
documento de estado, la codificación, las referencias por número de línea, los datos obsoletos
conocidos y que el número de compilación cuadre entre `.buildnum`, `platformio.ini` y el binario.

**Regla**: lo que es informativo **avisa**; solo **falla** lo que está mal de verdad.

---

## 4. Flujo de trabajo

1. Lee las Capas 1 y 2.
2. **Fase 1 (PLAN)**: diagnostica y propón el plan con su método de comprobación. **No edites nada.**
3. Espera la confirmación explícita del operador.
4. **Fase 2 (EJECUCIÓN)**.
5. Al terminar: actualiza `ESTADO.md` si cambió el estado, apunta en `_memoria\PENDIENTE.md` lo que
   quede, y **pasa el verificador**.

---

## 5. Qué hay en el proyecto (mapa rápido)

Todo el firmware vive en `..\Faketec_APRS_Igate_EA2OY\`: `src\` (el código), `web\` (el configurador
de un solo fichero), `tools\` (herramientas y comprobadores), `docs\` (documentación),
`data\rescue\` (firmware de rescate, no borrar). El detalle, en `REGLAS.md` §7.

Fuera del proyecto pero del proyecto: `..\_BLE_DIAG\` (banco de pruebas del Bluetooth),
`..\_referencias\` (clones de referencia, solo lectura), `..\_pio_core\` (caché de PlatformIO).

**Repositorio git**: en `..\` (la raíz), con el commit de base del 2026-09-14. Deja fuera las
compilaciones, los registros y —muy importante— `data/igate_conf.json`, que lleva credenciales.
