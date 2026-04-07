#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 06: Screen Capture
 *
 * GDI-based screen capture. Captures active desktop to a memory bitmap,
 * compresses it to JPEG (in-memory, no disk drops), and pushes to IPC.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_06_SCREEN_H
#define SMIRROR_ATOM_06_SCREEN_H

#include <windows.h>

/* Setup and execute screen capture sequence */
DWORD WINAPI ScreenCaptureAtomMain(LPVOID lpParam);

#endif /* SMIRROR_ATOM_06_SCREEN_H */
