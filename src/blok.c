/* ------------------------------------------------------------------
   BLOK  -  "input lock" prank for Windows
   Keyboard and mouse are blocked, the cursor shakes violently and
   random english letters rain down the screen.  Press R to release.
   If nobody presses R, everything unlocks by itself after 60 seconds.

   Pure Win32 + GDI, 32-bit, static, runs on Windows XP/7 and newer.
   Build: i686-w64-mingw32-gcc -O2 -s -municode -mwindows blok.c -lgdi32 -luser32
   ------------------------------------------------------------------ */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define RUN_MS   600000
#define COMBO_MS   5000
#define MAXG     96

#define RUNKEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUNVAL  L"Blok"

#define C_HOT     RGB(190, 255, 205)
#define C_MAIN    RGB( 60, 235, 115)
#define C_MID     RGB( 20, 155,  60)
#define C_DIM     RGB(  8,  75,  30)
#define C_HINT    RGB(  0, 135,  52)
#define C_HINTB   RGB(  0,  60,  24)
#define C_WORD    RGB(  0,  72,  28)
#define C_BORDER  RGB(  0,  55,  22)

typedef struct { int ch, x, y, vy, small; } Glyph;

static Glyph  gG[MAXG];
static int    gNG = 0;
static int    gSpawnWait = 200;

static const wchar_t *gWords[] = {
    L"НЕТ ДОСТУПА",
    L"INPUT LOCKED",
    L"ACCESS DENIED",
    L"СИСТЕМА",
    L"БЛОКИРОВКА",
    L"ВВОД ОТКЛЮЧЁН",
    L"ERROR 0x5",
    L"ДОСТУП ЗАПРЕЩЁН"
};
static const int gNW = (int)(sizeof(gWords) / sizeof(gWords[0]));
static int gWord = 0, gWordWait = 0;
static int gHideWait = 0, gHideLeft = 0;

static HWND  gWnd = 0;
static int   gW = 0, gH = 0;

static HDC     gBackDC = 0, gScrDC = 0;
static HBITMAP gBackBmp = 0;
static HGDIOBJ gOldBmp = 0;
static HBRUSH  gBrBlack = 0, gBrGreen = 0, gBrBorder = 0;

static HFONT gFG = 0, gFG2 = 0, gFWord = 0, gFHint = 0, gFSmall = 0;

static HHOOK gHkKb = 0, gHkMs = 0;

static DWORD gStart = 0, gNowMs = 0, gTick = 0;
static int   gStopReq = 0, gClosing = 0;

/* release combo: Esc + R + right Ctrl + Delete, held for COMBO_MS */
static int   gComboOn = 0, gComboMs = 0;
static DWORD gComboStart = 0;
static int   gKeyEsc = 0, gKeyR = 0, gKeyRCtrl = 0, gKeyDel = 0;

/* ---------------- random ---------------- */
static unsigned int gRng = 0x13579B1u;

static unsigned int rnd(void)
{
    gRng = gRng * 1103515245u + 12345u;
    return (gRng >> 16) & 0x7FFF;
}

static int rndR(int a, int b)
{
    if (b <= a) return a;
    return a + (int)(rnd() % (unsigned)(b - a + 1));
}

/* ---------------- text helpers ---------------- */
static int TextW(HDC dc, HFONT f, const wchar_t *s)
{
    SIZE sz;
    HGDIOBJ o = SelectObject(dc, f);
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, o);
    return sz.cx;
}

static void Txt(HDC dc, HFONT f, COLORREF c, int x, int y, const wchar_t *s)
{
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, x, y, s, (int)wcslen(s));
    SelectObject(dc, o);
}

static void TxtC(HDC dc, HFONT f, COLORREF c, int cx, int y, const wchar_t *s)
{
    Txt(dc, f, c, cx - TextW(dc, f, s) / 2, y, s);
}

/* ---------------- fonts / buffers ---------------- */
static void MakeFonts(void)
{
    int bigH   = gH / 18;
    int wordH  = gW / 22;
    int hintH  = gH / 50;
    int statH  = gH / 64;

    if (bigH < 16) bigH = 16;
    if (wordH > gH / 9) wordH = gH / 9;
    if (wordH < 16) wordH = 16;
    if (hintH < 11) hintH = 11;
    if (hintH > 24) hintH = 24;
    if (statH < 10) statH = 10;
    if (statH > 18) statH = 18;

    if (gFG)     DeleteObject(gFG);
    if (gFG2)    DeleteObject(gFG2);
    if (gFWord)  DeleteObject(gFWord);
    if (gFHint)  DeleteObject(gFHint);
    if (gFSmall) DeleteObject(gFSmall);

    gFG = CreateFontW(bigH, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                      OUT_RASTER_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                      FIXED_PITCH | FF_MODERN, L"Lucida Console");
    gFG2 = CreateFontW(bigH * 2 / 3, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_RASTER_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       FIXED_PITCH | FF_MODERN, L"Lucida Console");
    gFWord = CreateFontW(wordH, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_RASTER_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                         FIXED_PITCH | FF_MODERN, L"Lucida Console");
    gFHint = CreateFontW(hintH, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_RASTER_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                         FIXED_PITCH | FF_MODERN, L"Lucida Console");
    gFSmall = CreateFontW(statH, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_RASTER_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          FIXED_PITCH | FF_MODERN, L"Lucida Console");
}

static void KillBuffers(void)
{
    if (gBackDC) { SelectObject(gBackDC, gOldBmp); DeleteDC(gBackDC); gBackDC = 0; }
    if (gBackBmp) { DeleteObject(gBackBmp); gBackBmp = 0; }
}

static void MakeBuffers(void)
{
    HDC sdc;
    KillBuffers();
    sdc = GetDC(gWnd);
    gBackDC  = CreateCompatibleDC(sdc);
    gBackBmp = CreateCompatibleBitmap(sdc, gW, gH);
    gOldBmp  = SelectObject(gBackDC, gBackBmp);
    ReleaseDC(gWnd, sdc);
    MakeFonts();
}

static void InitGDI(void)
{
    gBrBlack  = CreateSolidBrush(RGB(0, 0, 0));
    gBrGreen  = CreateSolidBrush(C_MAIN);
    gBrBorder = CreateSolidBrush(C_BORDER);
}

/* ---------------- cursor shake ---------------- */
static const int gCirX[24] = {
    1000, 966, 866, 707, 500, 259, 0, -259, -500, -707, -866, -966,
   -1000, -966, -866, -707, -500, -259, 0, 259, 500, 707, 866, 966
};
static const int gCirY[24] = {
    0, 259, 500, 707, 866, 966, 1000, 966, 866, 707, 500, 259,
    0, -259, -500, -707, -866, -966, -1000, -966, -866, -707, -500, -259
};

static int gDriftX = 0, gDriftY = 0;
static int gPhase = 0, gMode = 0, gModeLeft = 0;

/* 0..1000 depending on how much of the run has already passed */
static int Prog(void)
{
    int p = (int)((long long)gNowMs * 1000 / RUN_MS);
    return p > 1000 ? 1000 : p;
}

static void ShakeCursor(int dt)
{
    int amp, x, y, rad;

    amp = 4 + Prog() * 150 / 1000;
    if (amp > 160) amp = 160;

    gModeLeft -= dt;
    if (gModeLeft <= 0) {
        gMode = (rnd() % 100u) < 35u ? 1 : 0;
        gModeLeft = gMode ? rndR(600, 1300) : rndR(300, 900);
        gPhase = rndR(0, 23);
        gDriftX = gW / 2 + rndR(-gW / 5, gW / 5);
        gDriftY = gH / 2 + rndR(-gH / 5, gH / 5);
    }

    if (gMode) {                     /* fast circles around the anchor point */
        gPhase = (gPhase + 2) % 24;
        rad = amp + 40;
        x = gDriftX + gCirX[gPhase] * rad / 1000;
        y = gDriftY + gCirY[gPhase] * rad / 1000;
    } else {                         /* raw random jitter */
        x = gDriftX + rndR(-amp, amp);
        y = gDriftY + rndR(-amp, amp);
    }

    if (x < 2) x = 2;
    if (y < 2) y = 2;
    if (x > gW - 3) x = gW - 3;
    if (y > gH - 3) y = gH - 3;
    SetCursorPos(x, y);
}

/* ---------------- falling letters ---------------- */
static void SpawnGlyph(void)
{
    Glyph *g;
    if (gNG >= MAXG) return;
    g = &gG[gNG++];
    g->ch    = 'A' + (int)(rnd() % 26u);
    g->small = (rnd() % 100u) < 35u;
    g->x     = rndR(10, gW - 70);
    g->y     = -rndR(40, gH / 2 + 40);
    g->vy    = g->small ? rndR(5, 11) : rndR(4, 9);
}

static void StepGlyphs(int dt)
{
    int i = 0;
    while (i < gNG) {
        int d = dt * gG[i].vy / 16;
        if (d < 1) d = 1;
        gG[i].y += d;
        if (gG[i].y > gH + 90) gG[i] = gG[--gNG];
        else i++;
    }
}

static void TickSpawn(int dt)
{
    int base;
    gSpawnWait -= dt;
    if (gSpawnWait > 0) return;
    SpawnGlyph();
    if ((rnd() % 100u) < 30u) SpawnGlyph();
    base = 800 - Prog() * 620 / 1000;
    if (base < 170) base = 170;
    gSpawnWait = base + rndR(0, 400);
}

/* ---------------- word in the middle ---------------- */
static void TickWord(int dt)
{
    gWordWait -= dt;
    if (gWordWait <= 0) {
        gWord = rndR(0, gNW - 1);
        gWordWait = rndR(2600, 4600);
    }
    gHideWait -= dt;
    if (gHideWait <= 0) {
        gHideLeft = rndR(40, 140);
        gHideWait = rndR(700, 2600);
    }
    if (gHideLeft > 0) gHideLeft -= dt;
}

/* ---------------- rendering ---------------- */
static void DrawWord(HDC dc)
{
    RECT rc;
    HGDIOBJ o;

    if (gHideLeft > 0) return;

    rc.left = 0; rc.right = gW;
    rc.top = gH / 2 - gH / 8;
    rc.bottom = gH / 2 + gH / 8;

    o = SelectObject(dc, gFWord);
    SetBkMode(dc, TRANSPARENT);

    if ((gNowMs % 3400u) < 80u) {      /* short glitch burst */
        RECT g2 = rc;
        OffsetRect(&g2, rndR(-14, 14), rndR(-4, 4));
        SetTextColor(dc, C_MID);
        DrawTextW(dc, gWords[gWord], -1, &g2,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        OffsetRect(&rc, rndR(-8, 8), 0);
    }

    SetTextColor(dc, C_WORD);
    DrawTextW(dc, gWords[gWord], -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, o);
}

static void DrawGlyphs(HDC dc)
{
    int i, k;
    for (i = 0; i < gNG; i++) {
        Glyph *g = &gG[i];
        wchar_t s[2];
        HGDIOBJ o;
        s[0] = (wchar_t)g->ch;
        s[1] = 0;
        o = SelectObject(dc, g->small ? gFG2 : gFG);
        SetBkMode(dc, TRANSPARENT);
        for (k = 4; k >= 0; k--) {
            int yy = g->y - g->vy * 3 * k;
            COLORREF c;
            if (yy < -70 || yy > gH + 40) continue;
            if (k == 0)      c = C_HOT;
            else if (k == 1) c = C_MAIN;
            else if (k == 2) c = C_MID;
            else             c = C_DIM;
            SetTextColor(dc, c);
            TextOutW(dc, g->x, yy, s, 1);
        }
        SelectObject(dc, o);
    }
}

static void DrawStatus(HDC dc)
{
    int pad = gW / 40;
    int rem = (int)((RUN_MS - gNowMs + 999) / 1000);
    wchar_t s[96];

    if (rem < 0) rem = 0;

    Txt(dc, gFSmall, C_MID, pad, gH / 40, L"SYS.INPUT: OFFLINE");
    if ((gNowMs / 500u) % 2u == 0u) {
        RECT r;
        int sz = gH / 90;
        if (sz < 6) sz = 6;
        if (sz > 12) sz = 12;
        r.left = pad;
        r.top = gH / 40 + gH / 30;
        r.right = r.left + sz;
        r.bottom = r.top + sz;
        FillRect(dc, &r, gBrGreen);
    }

    wsprintfW(s, L"LOCK %02d:%02d", rem / 60, rem % 60);
    Txt(dc, gFSmall, C_HINT, gW - pad - TextW(dc, gFSmall, s), gH / 40, s);
}

static void DrawHint(HDC dc)
{
    int y = gH - gH / 22;
    TxtC(dc, gFHint, C_HINT, gW / 2, y, L"ВВОД ЗАБЛОКИРОВАН");
}

static void DrawFrame(HDC dc)
{
    RECT r;
    r.left = 0; r.top = 0; r.right = gW; r.bottom = gH;

    if (gComboMs <= 0) {                 /* idle: dim constant border */
        FrameRect(dc, &r, gBrBorder);
        return;
    }

    {                                    /* combo is charging: border heats up */
        int p = gComboMs * 100 / COMBO_MS;
        HBRUSH b;
        if (p > 100) p = 100;
        b = CreateSolidBrush(RGB(30 + 190 * p / 100,
                                 55 - 25 * p / 100,
                                 22 - 10 * p / 100));
        FrameRect(dc, &r, b);
        if (p > 20) { RECT r2 = r; InflateRect(&r2, -2, -2); FrameRect(dc, &r2, b); }
        if (p > 50) { RECT r3 = r; InflateRect(&r3, -4, -4); FrameRect(dc, &r3, b); }
        DeleteObject(b);
    }
}

static void Render(void)
{
    RECT r;
    r.left = 0; r.top = 0; r.right = gW; r.bottom = gH;
    FillRect(gBackDC, &r, gBrBlack);
    DrawWord(gBackDC);
    DrawGlyphs(gBackDC);
    DrawStatus(gBackDC);
    DrawHint(gBackDC);
    DrawFrame(gBackDC);
    BitBlt(gScrDC, 0, 0, gW, gH, gBackDC, 0, 0, SRCCOPY);
}

/* ---------------- input hooks ---------------- */
static void TrackKey(int vk, int down)
{
    switch (vk) {
    case VK_ESCAPE:   gKeyEsc = down; break;
    case 'R':         gKeyR = down; break;
    case VK_RCONTROL: gKeyRCtrl = down; break;
    case VK_DELETE:   gKeyDel = down; break;
    default: return;
    }

    if (gKeyEsc && gKeyR && gKeyRCtrl && gKeyDel) {
        if (!gComboOn) {
            gComboOn = 1;
            gComboStart = GetTickCount();
        }
    } else {
        gComboOn = 0;
    }
}

static LRESULT CALLBACK KbProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        TrackKey((int)k->vkCode, !(k->flags & LLKHF_UP));
        return 1;                        /* swallow every key */
    }
    return CallNextHookEx(gHkKb, code, wp, lp);
}

static LRESULT CALLBACK MsProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) return 1;     /* buttons, wheel, moves */
    return CallNextHookEx(gHkMs, code, wp, lp);
}

static void InstallHooks(void)
{
    HINSTANCE hInst = GetModuleHandleW(NULL);
    gHkKb = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, hInst, 0);
    gHkMs = SetWindowsHookExW(WH_MOUSE_LL, MsProc, hInst, 0);
}

/* ---------------- autostart ---------------- */
static void EnableAutostart(void)
{
    HKEY k = 0;
    wchar_t path[MAX_PATH + 4];
    wchar_t val[MAX_PATH + 12];
    DWORD n;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;

    n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wsprintfW(val, L"\"%s\"", path);
        RegSetValueExW(k, RUNVAL, 0, REG_SZ, (const BYTE *)val,
                       (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);
}

static void DisableAutostart(void)
{
    HKEY k = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, RUNVAL);
    RegCloseKey(k);
}

static void RemoveHooks(void)
{
    if (gHkKb) { UnhookWindowsHookEx(gHkKb); gHkKb = 0; }
    if (gHkMs) { UnhookWindowsHookEx(gHkMs); gHkMs = 0; }
}

static void DoStop(int byCombo)
{
    if (gClosing) return;
    gClosing = 1;
    if (byCombo) DisableAutostart();
    RemoveHooks();
    if (gWnd) DestroyWindow(gWnd);
}

/* ---------------- window ---------------- */
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        gWnd = h;
        gW = GetSystemMetrics(SM_CXSCREEN);
        gH = GetSystemMetrics(SM_CYSCREEN);
        InitGDI();
        MakeBuffers();
        gScrDC = GetDC(h);
        gStart = GetTickCount();
        gTick = gStart;
        InstallHooks();
        EnableAutostart();
        SetTimer(h, 1, 16, NULL);
        return 0;

    case WM_TIMER: {
        DWORD now;
        int dt;
        if (wp != 1) break;
        now = GetTickCount();
        dt = (int)(now - gTick);
        gTick = now;
        if (dt < 1) dt = 1;
        if (dt > 120) dt = 120;
        gNowMs = now - gStart;

        if (gStopReq || gNowMs >= (DWORD)RUN_MS) {
            DoStop(0);                    /* time is up: autostart stays armed */
            return 0;
        }

        if (gComboOn) {
            gComboMs = (int)(now - gComboStart);
            if (gComboMs >= COMBO_MS) {
                DoStop(1);                /* proper exit: disarm autostart */
                return 0;
            }
        } else {
            gComboMs = 0;
        }

        TickSpawn(dt);
        StepGlyphs(dt);
        TickWord(dt);
        ShakeCursor(dt);
        Render();
        return 0;
    }

    case WM_KEYDOWN:                 /* fallback if the hook failed */
        TrackKey((int)wp, 1);
        return 0;

    case WM_KEYUP:
        TrackKey((int)wp, 0);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        Render();
        EndPaint(h, &ps);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        KillTimer(h, 1);
        RemoveHooks();
        if (gScrDC) { ReleaseDC(h, gScrDC); gScrDC = 0; }
        KillBuffers();
        if (gFG) { DeleteObject(gFG); gFG = 0; }
        if (gFG2) { DeleteObject(gFG2); gFG2 = 0; }
        if (gFWord) { DeleteObject(gFWord); gFWord = 0; }
        if (gFHint) { DeleteObject(gFHint); gFHint = 0; }
        if (gFSmall) { DeleteObject(gFSmall); gFSmall = 0; }
        if (gBrBlack) { DeleteObject(gBrBlack); gBrBlack = 0; }
        if (gBrGreen) { DeleteObject(gBrGreen); gBrGreen = 0; }
        if (gBrBorder) { DeleteObject(gBrBorder); gBrBorder = 0; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    WNDCLASSW wc;
    MSG msg;
    HANDLE one;
    int sw, sh;
    typedef BOOL (WINAPI *PFN_DPIAWARE)(void);
    PFN_DPIAWARE pDpi;

    (void)hPrev; (void)cmd; (void)show;

    one = CreateMutexW(NULL, FALSE, L"BlokPrankSingleInstance");
    if (one && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(one);
        return 0;                        /* already running: do not stack */
    }

    pDpi = (PFN_DPIAWARE)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware");
    if (pDpi) pDpi();

    SetCursor(LoadCursor(NULL, IDC_ARROW));

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"BLOK_WND";
    if (!RegisterClassW(&wc)) return 0;

    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);

    gWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"BLOK_WND", L"",
                           WS_POPUP | WS_VISIBLE, 0, 0, sw, sh,
                           NULL, NULL, hInst, NULL);
    if (!gWnd) return 0;

    ShowWindow(gWnd, SW_SHOW);
    SetForegroundWindow(gWnd);
    SetFocus(gWnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (one) CloseHandle(one);
    return 0;
}
