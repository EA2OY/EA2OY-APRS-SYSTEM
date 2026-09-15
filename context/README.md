# MEMORIA — Faketec_APRS_Igate_EA2OY

Project memory (knowledge layer) of **Faketec_APRS_Igate_EA2OY**: custom APRS-LoRa 433 MHz
digipeater + tracker firmware for Faketec V1-V6 (nRF52840).

> Reorganised **2026-09-14** into **three layers**, ordered by *when they are read*, not by topic.
> Before that, the mandatory reading was 446 KB (~112 k tokens) and did not fit in an agent's
> context — which is exactly why it drifted. Now it is 17 KB (~4.2 k tokens), and everything else
> is searched on demand.

## What it is

Portable memory so that any agent resuming the project loads a short, dated, single-source state
instead of re-reading all the documentation.

## How to use it

1. Start each session with **`PROMPT_INICIALIZACION.md`**.
2. Any agent reads **`GUIA_AGENTE.md`** before touching a file.
3. Always read **Layer 1** (`REGLAS.md`) and **Layer 2** (`ESTADO.md`). Nothing else is mandatory.
4. At the end of a session: update `ESTADO.md` if the state changed, note what is left in
   `_memoria\PENDIENTE.md`, and run `..\Faketec_APRS_Igate_EA2OY\tools\verifica_memoria.ps1`
   (0 = the memory is consistent).

## Structure

```
Cerebro_Faketec_APRS_Igate_EA2OY\
  README.md                    <- this file
  GUIA_AGENTE.md               <- agent entry point (read first)
  PROMPT_INICIALIZACION.md     <- session opening prompt
  REGLAS.md                    <- LAYER 1 (always): rules, do-not-touch list, map, figures
  ESTADO.md                    <- LAYER 2 (always): THE single state document
  cerebro.md                   <- signpost: the old file was split; says where everything went
  _memoria\                    <- LAYER 3 (searched, never read whole)
    historia.md                <-   full session history (the old cerebro.md, untouched)
    PENDIENTE.md               <-   tasks written down by the operator
    01_subnotas\               <-   initial project context (historical)
    02_neuronas\               <-   hardware and patterns (N-00 .. N-12)
  _archivo\backups\            <- rescue copies. NOT to be read.
```

## How to transfer from one agent to another

1. In the current session: update `ESTADO.md` with the real state, and pass the memory checker.
2. Start a new session with `PROMPT_INICIALIZACION.md`.
3. The new agent reads Layers 1 and 2, checks them against the code and the folder tree, and
   continues from `ESTADO.md` §2 ("what to do first").
