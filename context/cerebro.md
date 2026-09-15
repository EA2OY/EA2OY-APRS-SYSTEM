# ¿Buscabas `cerebro.md`? Se ha partido en tres capas (2026-09-14)

> **Este fichero ya no es la memoria del proyecto: es una señal de tráfico.**
> Se deja aquí porque su nombre estaba citado en muchos sitios y un agente nuevo
> lo buscaría. No contiene estado ni hay que leerlo entero: son 20 líneas.

## Dónde está ahora cada cosa

| Buscas | Está en | ¿Se lee siempre? |
|---|---|---|
| Reglas, verdades, lista negra, mapa del proyecto | **`REGLAS.md`** (Capa 1) | **Sí, siempre** |
| Qué está hecho, qué está abierto y qué está bloqueado | **`ESTADO.md`** (Capa 2) | **Sí, siempre** |
| Tareas apuntadas por el operador | `_memoria\PENDIENTE.md` | Al empezar una tanda |
| El histórico de sesiones (el antiguo `cerebro.md`, **íntegro y sin modificar**) | `_memoria\historia.md` | No: **se busca** |
| Conocimiento de hardware y patrones (las neuronas) | `_memoria\02_neuronas\` | No: **se busca** |
| Contexto inicial del proyecto (subnotas) | `_memoria\01_subnotas\` | No: **se busca** |
| Copias de rescate | `_archivo\backups\` | **No se leen** |

## Orden de lectura para un agente nuevo

1. `REGLAS.md`
2. `ESTADO.md`
3. Y ya está. **Lo demás se busca cuando el caso lo pide.**

## Por qué se hizo esto

La lectura obligatoria eran **446 KB (unos 112.000 tokens)**, y no cabe en el
contexto de un agente: cuando algo no cabe, se pica de aquí y de allá, y se
trabaja con datos viejos de la parte que uno se ha saltado. Ahora son
**17 KB (unos 4.200 tokens)**, y el histórico sigue entero para consultarlo.

La comprobación mecánica de que esto se mantiene vivo:
`Faketec_APRS_Igate_EA2OY\tools\verifica_memoria.ps1` (0 = todo bien).
