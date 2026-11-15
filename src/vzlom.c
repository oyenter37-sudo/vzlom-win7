/* ------------------------------------------------------------------
   VZLOM  -  fullscreen fake-hacker animation
   Pure Win32 + GDI, 32-bit, no runtime, runs on Windows XP/7 and newer.
   Build: i686-w64-mingw32-gcc -O2 -s -municode -mwindows vzlom.c -lgdi32 -luser32
   ------------------------------------------------------------------ */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>

#define NEVMAX      1024
#define EVTEXT       260
#define MAXLOG       512
#define HEXLINES      64
#define MAXDROPS     512

#define COL_BG      RGB(  0,   0,   0)
#define COL_TXT     RGB(  0, 255,  80)
#define COL_HI      RGB(170, 255, 190)
#define COL_DIM     RGB(  0, 150,  50)
#define COL_FAINT   RGB(  0,  80,  25)
#define COL_LINE    RGB(  0, 110,  35)
#define COL_PANEL   RGB(  0,  18,   6)
#define COL_RAIN_T  RGB(  0,  90,  30)
#define COL_RAIN_H  RGB(110, 255, 140)

enum { E_LOG = 1, E_BIG, E_BAR, E_FLASH, E_END };

typedef struct { int t, type, v; wchar_t text[EVTEXT]; } Ev;

static Ev   gEv[NEVMAX];
static int  gNEv = 0;

static HWND  gWnd = 0;
static int   gW = 0, gH = 0;
static int   gRunning = 0, gNow = 0, gEvIdx = 0, gFlash = 0, gEnded = 0;
static DWORD gStart = 0;

static HDC     gRainDC = 0, gBackDC = 0, gScrDC = 0;
static HBITMAP gRainBmp = 0, gBackBmp = 0;
static HGDIOBJ gOldRain = 0, gOldBack = 0;

static HFONT  gFLog = 0, gFBig = 0, gFTitle = 0;
static HBRUSH gBrBlack = 0, gBrPanel = 0, gBrGreen = 0,
              gBrBarBg = 0, gBrBarFg = 0, gBrLine = 0, gBrDim = 0;

static int gBaseH = 20, gLineH = 23, gCharW = 10, gCellH = 23, gCellW = 10;
static int gCols = 1, gRows = 1;

static wchar_t gLog[MAXLOG][EVTEXT];
static int     gLogTotal = 0;

static wchar_t gBig[EVTEXT];
static int     gBigTime = -100000;
static int     gBarVal = 0, gBarTime = 0;
static wchar_t gBarLabel[EVTEXT];

static int gHover = 0;                 /* 1 = start, 2 = cancel */
static int gHexTotal = 0, gLastHexTick = -1000;
static wchar_t gHex[HEXLINES][56];
static unsigned int gHexAddr = 0x00400000u;

/* ------------------------------------------------------------------ */
static unsigned int gRng = 0x2F6E2B1u;
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

static void FmtNum(wchar_t *out, long long v)
{
    wchar_t tmp[32];
    int n = 0, p = 0, i;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    do { tmp[n++] = (wchar_t)(L'0' + (int)(v % 10)); v /= 10; } while (v > 0);
    if (neg) out[p++] = L'-';
    for (i = n - 1; i >= 0; i--) {
        out[p++] = tmp[i];
        if (i > 0 && (i % 3) == 0) out[p++] = L' ';
    }
    out[p] = 0;
}

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

static void TxtR(HDC dc, HFONT f, COLORREF c, int rx, int y, const wchar_t *s)
{
    Txt(dc, f, c, rx - TextW(dc, f, s), y, s);
}

static void FillC(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void FillB(HDC dc, int x, int y, int w, int h, HBRUSH b)
{
    RECT r;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    FillRect(dc, &r, b);
}

/* ------------------------------------------------------------------ */
/*  rain buffer                                                       */
/* ------------------------------------------------------------------ */
typedef struct { int col, row, len, step, phase; } Drop;
static Drop gDrops[MAXDROPS];
static int  gNDrops = 0;

static const wchar_t RAIN_CH[] =
    L"0123456789abcdefghijklmnopqrstuvwxyz!@#$%&*+=<>ABCEFHKMXZАБВГДЕЖЗКМНОПРСТУФХЦЧШЭЮЯ";

static void RainCell(int col, int row, int draw, COLORREF c)
{
    int x = col * gCellW, y = row * gCellH;
    if (x < 0 || y < 0 || x >= gW || y >= gH) return;
    FillB(gRainDC, x, y, gCellW, gCellH, gBrBlack);
    if (draw) {
        wchar_t ch[2];
        ch[0] = RAIN_CH[rnd() % (sizeof(RAIN_CH) / sizeof(wchar_t) - 1)];
        ch[1] = 0;
        Txt(gRainDC, gFLog, c, x, y, ch);
    }
}

static void RainInit(void)
{
    int i;
    FillC(gRainDC, 0, 0, gW, gH, COL_BG);
    gNDrops = gCols;
    if (gNDrops > MAXDROPS) gNDrops = MAXDROPS;
    for (i = 0; i < gNDrops; i++) {
        gDrops[i].col   = i;
        gDrops[i].row   = -rndR(0, gRows);
        gDrops[i].len   = rndR(5, 18);
        gDrops[i].step  = rndR(1, 3);
        gDrops[i].phase = rndR(0, 2);
    }
}

static void RainStep(void)
{
    int i;
    for (i = 0; i < gNDrops; i++) {
        Drop *d = &gDrops[i];
        int tail;
        if (++d->phase < d->step) continue;
        d->phase = 0;

        tail = d->row - d->len;
        if (tail >= 0 && tail < gRows) RainCell(d->col, tail, 0, 0);
        if (d->row >= 0 && d->row < gRows) RainCell(d->col, d->row, 1, COL_RAIN_T);

        d->row++;

        if (d->row >= 0 && d->row < gRows) RainCell(d->col, d->row, 1, COL_RAIN_H);

        if (d->row - d->len > gRows) {
            d->row   = -rndR(0, gRows / 2);
            d->len   = rndR(5, 18);
            d->step  = rndR(1, 3);
            d->col   = rndR(0, gCols - 1);
            d->phase = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
static void MakeFonts(void)
{
    if (gFLog)   DeleteObject(gFLog);
    if (gFBig)   DeleteObject(gFBig);
    if (gFTitle) DeleteObject(gFTitle);

    gBaseH = gH / 40;
    if (gBaseH < 13) gBaseH = 13;
    if (gBaseH > 34) gBaseH = 34;

    gFLog = CreateFontW(gBaseH, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_RASTER_PRECIS, CLIP_DEFAULT_PRECIS,
                        DRAFT_QUALITY, FIXED_PITCH | FF_MODERN, L"Lucida Console");
    gFBig = CreateFontW(gBaseH * 2, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_RASTER_PRECIS, CLIP_DEFAULT_PRECIS,
                        ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Lucida Console");
    gFTitle = CreateFontW(gH / 7, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_RASTER_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Lucida Console");

    gCharW  = TextW(gBackDC, gFLog, L"M");
    if (gCharW < 4) gCharW = 8;
    gCellW  = gCharW;
    gCellH  = gBaseH + 3;
    gLineH  = gBaseH + 3;
    gCols   = gW / gCellW;
    if (gCols < 1) gCols = 1;
    gRows   = gH / gCellH;
    if (gRows < 1) gRows = 1;
}

static void KillBuffers(void)
{
    if (gRainDC) { SelectObject(gRainDC, gOldRain); DeleteDC(gRainDC); gRainDC = 0; }
    if (gBackDC) { SelectObject(gBackDC, gOldBack); DeleteDC(gBackDC); gBackDC = 0; }
    if (gRainBmp) { DeleteObject(gRainBmp); gRainBmp = 0; }
    if (gBackBmp) { DeleteObject(gBackBmp); gBackBmp = 0; }
}

static void MakeBuffers(void)
{
    HDC sdc;
    KillBuffers();
    sdc = GetDC(gWnd);
    gRainDC  = CreateCompatibleDC(sdc);
    gRainBmp = CreateCompatibleBitmap(sdc, gW, gH);
    gOldRain = SelectObject(gRainDC, gRainBmp);
    gBackDC  = CreateCompatibleDC(sdc);
    gBackBmp = CreateCompatibleBitmap(sdc, gW, gH);
    gOldBack = SelectObject(gBackDC, gBackBmp);
    ReleaseDC(gWnd, sdc);

    MakeFonts();
    RainInit();
}

static void InitGDI(void)
{
    gBrBlack  = CreateSolidBrush(COL_BG);
    gBrPanel  = CreateSolidBrush(COL_PANEL);
    gBrGreen  = CreateSolidBrush(COL_TXT);
    gBrBarBg  = CreateSolidBrush(RGB(0, 40, 12));
    gBrBarFg  = CreateSolidBrush(RGB(0, 200, 70));
    gBrLine   = CreateSolidBrush(COL_LINE);
    gBrDim    = CreateSolidBrush(COL_FAINT);
}

/* ------------------------------------------------------------------ */
/*  script                                                            */
/* ------------------------------------------------------------------ */
static void EvAdd(int t, int type, int v, const wchar_t *s)
{
    Ev *e;
    if (gNEv >= NEVMAX) return;
    e = &gEv[gNEv++];
    e->t = t; e->type = type; e->v = v;
    if (s) { wcsncpy(e->text, s, EVTEXT - 1); e->text[EVTEXT - 1] = 0; }
    else e->text[0] = 0;
}

#define LOGT(tt, s) EvAdd((tt), E_LOG, 0, (s))

static const wchar_t *gCopyFiles[] = {
    L"C:\\Windows\\System32\\config\\SAM",
    L"C:\\Windows\\System32\\config\\SYSTEM",
    L"C:\\Windows\\System32\\config\\SECURITY",
    L"C:\\Windows\\System32\\lsass.exe",
    L"C:\\Windows\\System32\\winlogon.exe",
    L"C:\\Windows\\System32\\drivers\\etc\\hosts",
    L"C:\\Windows\\System32\\winevt\\Logs\\Security.evtx",
    L"C:\\Windows\\System32\\winevt\\Logs\\System.evtx",
    L"C:\\Users\\Анна\\Documents\\паспорт_скан.jpg",
    L"C:\\Users\\Анна\\Documents\\резюме_2026.docx",
    L"C:\\Users\\Анна\\Documents\\договор_аренды.pdf",
    L"C:\\Users\\Анна\\Desktop\\фото_отпуск\\IMG_0442.jpg",
    L"C:\\Users\\Анна\\Desktop\\фото_отпуск\\IMG_0443.jpg",
    L"C:\\Users\\Анна\\Desktop\\фото_отпуск\\IMG_0517.jpg",
    L"C:\\Users\\Анна\\AppData\\Roaming\\Mozilla\\logins.json",
    L"C:\\Users\\Анна\\AppData\\Local\\Google\\Chrome\\Login Data",
    L"C:\\Users\\Анна\\AppData\\Local\\Google\\Chrome\\Cookies",
    L"C:\\Users\\Анна\\AppData\\Local\\Microsoft\\Outlook\\outlook.pst",
    L"C:\\Users\\Анна\\My Documents\\1С\\база_склад.1CD",
    L"C:\\Users\\Анна\\My Documents\\налоги_2025.xlsx",
    L"C:\\Users\\Администратор\\Documents\\пароли.txt",
    L"C:\\Program Files\\1C\\Accounting\\conf.ini",
    L"C:\\Program Files\\BankClient\\keys\\private.key",
    L"C:\\Program Files\\BankClient\\cert\\cert.pfx",
    L"D:\\Backup\\2026-01-14\\full_image.vhd",
    L"D:\\Backup\\2026-02-02\\full_image.vhd",
    L"D:\\Projects\\crm\\database.sql",
    L"D:\\Projects\\crm\\backup_0902.zip",
    L"D:\\Photo\\Свадьба\\DSC_1024.NEF",
    L"D:\\Photo\\Свадьба\\DSC_1025.NEF",
    L"D:\\Видео\\отпуск_2026.mp4",
    L"D:\\Видео\\день_рождения.mp4",
    L"E:\\Архив\\бухгалтерия_2024.zip",
    L"E:\\Архив\\бухгалтерия_2025.zip",
    L"E:\\Архив\\договоры_все.zip",
    L"E:\\Games\\Saves\\profile01.sav"
};
#define NCOPYFILES ((int)(sizeof(gCopyFiles)/sizeof(gCopyFiles[0])))

static int CmpEv(const void *a, const void *b)
{
    const Ev *x = (const Ev *)a, *y = (const Ev *)b;
    if (x->t < y->t) return -1;
    if (x->t > y->t) return 1;
    return 0;
}

static void BuildScript(void)
{
    wchar_t b[EVTEXT];
    int i, t;

    gNEv = 0;

    /* ---------- 0. запуск ---------- */
    LOGT(   60, L"root@local:/# ./vzlom --target LOCALHOST --deep --silent");
    LOGT(  360, L"ядро: инициализация модулей перехвата...");
    LOGT(  640, L"  core.sys ......................... OK");
    LOGT(  880, L"  net.sys .......................... OK");
    LOGT( 1080, L"  inject.sys ....................... OK");
    LOGT( 1300, L"  crypt.sys ........................ OK");
    LOGT( 1520, L"обход брандмауэра: политика изменена");
    LOGT( 1740, L"скрытый канал: 127.0.0.1:31337 установлен");
    LOGT( 1960, L"запуск сканера портов 1-65535");

    /* ---------- 1. сканирование сети ---------- */
    EvAdd( 2050, E_BAR,   0, L"СКАНИРОВАНИЕ СЕТИ");
    EvAdd( 2100, E_BIG,   0, L"ПОИСК ЦЕЛЕЙ");
    LOGT( 2250, L"подсеть: 192.168.1.0/24  хостов: 254");
    LOGT( 2600, L"192.168.1.1   445/tcp   OPEN    SMB");
    LOGT( 2950, L"192.168.1.1   3389/tcp  OPEN    RDP");
    LOGT( 3300, L"192.168.1.4   21/tcp    OPEN    FTP");
    LOGT( 3650, L"192.168.1.7   80/tcp    OPEN    HTTP");
    LOGT( 4000, L"192.168.1.12  3306/tcp  OPEN    MySQL");
    LOGT( 4350, L"192.168.1.19  445/tcp   OPEN    SMB");
    LOGT( 4700, L"192.168.1.23  22/tcp    OPEN    SSH");
    LOGT( 5050, L"найдено узлов: 7   открытых портов: 14");
    EvAdd( 5100, E_BAR,  22, 0);
    LOGT( 5450, L"определение ОС: Windows 7 Professional SP1 x64");
    LOGT( 5800, L"проверка уязвимостей MS17-010 / CVE-2019-0708");
    EvAdd( 6000, E_BAR,  48, 0);
    LOGT( 6250, L"уязвимость MS17-010 .............. ПОДТВЕРЖДЕНА");
    LOGT( 6700, L"уязвимость CVE-2019-0708 ......... ПОДТВЕРЖДЕНА");
    LOGT( 7050, L"уязвимость CVE-2020-1472 ......... ПОДТВЕРЖДЕНА");
    EvAdd( 7200, E_BAR,  74, 0);
    LOGT( 7500, L"точка входа: SMB 445  (узел 192.168.1.1)");
    LOGT( 7900, L"отправка эксплойта: 4 пакета ... 12 пакетов");
    EvAdd( 8100, E_BAR,  96, 0);
    EvAdd( 8400, E_BIG,  0, L"СОЕДИНЕНИЕ УСТАНОВЛЕНО");
    LOGT( 8500, L"сеанс 0x7F3A2C .. права: GUEST -> USER");
    EvAdd( 9000, E_BAR, 100, L"СКАНИРОВАНИЕ СЕТИ");
    LOGT( 9050, L"канал шифрования: AES-256-CBC  активен");

    /* ---------- 2. подбор пароля ---------- */
    EvAdd( 9500, E_BAR,   0, L"ПОДБОР ПАРОЛЯ");
    EvAdd( 9550, E_BIG,   0, L"ПОДБОР ПАРОЛЯ");
    LOGT( 9700, L"цель: ADMIN$@192.168.1.1");
    LOGT(10050, L"загрузка словаря rockyou.txt (14 344 391 слов)");
    LOGT(10350, L"NTLM-хэш перехвачен: 8846f7eaee8fb117ad06bdd830b7586c");
    LOGT(10700, L"атака по словарю: потоков 64, скорость 8 400/с");
    EvAdd(10800, E_BAR,  14, 0);
    LOGT(11100, L"попытка 1024:  123456          -> отказ");
    LOGT(11450, L"попытка 4096:  qwerty          -> отказ");
    LOGT(11800, L"попытка 8192:  password        -> отказ");
    EvAdd(11900, E_BAR,  33, 0);
    LOGT(12150, L"попытка 16384: monkey          -> отказ");
    LOGT(12500, L"попытка 32768: letmein         -> отказ");
    LOGT(12850, L"попытка 65536: qwerty123       -> отказ");
    EvAdd(13000, E_BAR,  52, 0);
    LOGT(13200, L"перебор масок: ?u?l?l?l?d?d?d?d?s");
    LOGT(13550, L"попытка 131072: Anna1997       -> отказ");
    LOGT(13900, L"попытка 196608: Qwerty!2024    -> отказ");
    LOGT(14250, L"попытка 262144: Admin123       -> отказ");
    EvAdd(14400, E_BAR,  71, 0);
    LOGT(14700, L"подключение к домену WORKGROUP ...");
    LOGT(15050, L"попытка 393216: P@ssw0rd       -> отказ");
    LOGT(15400, L"попытка 524288: 1q2w3e4r5t     -> отказ");
    EvAdd(15600, E_BAR,  88, 0);
    LOGT(15900, L"попытка 688128: Dkflbvbh2010   -> отказ");
    LOGT(16250, L"попытка 892144: Sokolov#77     -> ПРИНЯТ");
    EvAdd(16400, E_BAR,  97, 0);
    EvAdd(16800, E_BIG,  0, L"ПАРОЛЬ ПОДОБРАН");
    LOGT(16900, L"пароль: **************   (время 12.4 с)");
    LOGT(17300, L"токен сеанса сохранён: \\\\temp\\~tk.tmp");
    EvAdd(17800, E_BAR, 100, L"ПОДБОР ПАРОЛЯ");
    LOGT(18000, L"права: USER -> LOCAL ADMIN");

    /* ---------- 3. повышение привилегий ---------- */
    EvAdd(18500, E_BAR,   0, L"ПОВЫШЕНИЕ ПРИВИЛЕГИЙ");
    EvAdd(18600, E_BIG,   0, L"ПОВЫШЕНИЕ ПРИВИЛЕГИЙ");
    LOGT(18800, L"внедрение в lsass.exe (PID 512) ...");
    EvAdd(19100, E_BAR,  12, 0);
    LOGT(19300, L"дамп памяти: SAM / SYSTEM / SECURITY");
    LOGT(19700, L"извлечено учётных записей: 14");
    LOGT(20050, L"привилегия SeDebugPrivilege ........ ВЫДАНА");
    EvAdd(20200, E_BAR,  28, 0);
    LOGT(20400, L"привилегия SeBackupPrivilege ....... ВЫДАНА");
    LOGT(20800, L"привилегия SeTakeOwnershipPrivilege  ВЫДАНА");
    LOGT(21150, L"обход UAC: согласие получено автоматически");
    EvAdd(21400, E_BAR,  44, 0);
    LOGT(21600, L"отключение Защитника Windows ....... OK");
    LOGT(22000, L"отключение центра обновления ....... OK");
    LOGT(22350, L"отключение журнала аудита ........... OK");
    EvAdd(22600, E_BAR,  60, 0);
    LOGT(22800, L"создание скрытого пользователя: support$");
    LOGT(23200, L"добавление в группу администраторов");
    LOGT(23550, L"установка службы: WinDefendUpdater");
    EvAdd(23800, E_BAR,  78, 0);
    LOGT(24000, L"автозапуск: HKLM\\...\\Run\\svc32");
    LOGT(24400, L"проверка антивируса: не обнаружено");
    LOGT(24800, L"контроль над узлом 192.168.1.1: ПОЛНЫЙ");
    EvAdd(25200, E_BAR,  92, 0);
    EvAdd(25500, E_BIG,  0, L"ПРАВА АДМИНИСТРАТОРА ПОЛУЧЕНЫ");
    EvAdd(25900, E_BAR, 100, L"ПОВЫШЕНИЕ ПРИВИЛЕГИЙ");
    LOGT(26000, L"подготовка к выгрузке данных ...");

    /* ---------- 4. копирование файлов (26500 - 41500) ---------- */
    EvAdd(26500, E_BAR,   0, L"КОПИРОВАНИЕ ФАЙЛОВ");
    EvAdd(26600, E_BIG,   0, L"КОПИРОВАНИЕ ФАЙЛОВ");
    LOGT(26800, L"выбранный объём: локальные диски C:, D:, E:");
    LOGT(27150, L"архивирование: 7z -mx=9 -p********");

    t = 27800;
    for (i = 0; i < NCOPYFILES; i++) {
        int mb = rndR(1, 640);
        int kb = rndR(10, 999);
        wsprintfW(b, L"  %s", gCopyFiles[i]);
        LOGT(t, b);
        wsprintfW(b, L"    -> %d.%d МБ    буфер 64 КБ    OK", mb, kb / 100);
        LOGT(t + 200, b);
        t += 380;
    }

    EvAdd(28200, E_BAR,  10, 0);
    EvAdd(31000, E_BAR,  26, 0);
    EvAdd(33000, E_BAR,  38, 0);
    LOGT(33200, L"скорость канала: 12.7 МБ/с   потерь: 0.02%");
    EvAdd(34800, E_BAR,  50, 0);
    LOGT(35600, L"сжатие архива: 2.4 ГБ -> 812 МБ");
    EvAdd(36600, E_BAR,  62, 0);
    LOGT(37400, L"докачка пакета 41/64 ... 78%");
    EvAdd(38400, E_BAR,  76, 0);
    LOGT(39000, L"проверка контрольных сумм: SHA-256 OK");
    EvAdd(39600, E_BAR,  88, 0);
    LOGT(40200, L"выгрузка на удалённый узел 45.77.12.9:8443");
    EvAdd(40600, E_BAR,  96, 0);
    EvAdd(41200, E_BIG,  0, L"ФАЙЛЫ СКОПИРОВАНЫ");
    EvAdd(41300, E_BAR, 100, L"КОПИРОВАНИЕ ФАЙЛОВ");
    LOGT(41400, L"архив закрыт: dump_2026-11-15.7z (812 МБ)");

    /* ---------- 5. заражение вирусами (41500 - 53000) ---------- */
    EvAdd(41500, E_BAR,   0, L"ЗАРАЖЕНИЕ ВИРУСАМИ");
    EvAdd(41600, E_BIG,   0, L"ЗАРАЖЕНИЕ ВИРУСАМИ");
    LOGT(41800, L"загрузка полезной нагрузки: payload_01.bin 8.2 МБ");
    EvAdd(42000, E_BAR,   6, 0);
    LOGT(42300, L"payload_01.bin .............. получено 100%");
    LOGT(42700, L"расшифровка модуля: Trojan.Win32.Agent.gen");
    LOGT(43100, L"внедрение в процесс: explorer.exe  (PID 1832)");
    EvAdd(43400, E_BAR,  15, 0);
    LOGT(43600, L"внедрение в процесс: svchost.exe   (PID 892)");
    LOGT(44000, L"внедрение в процесс: winlogon.exe  (PID 508)");
    LOGT(44350, L"внедрение в процесс: csrss.exe     (PID 436)");
    EvAdd(44600, E_BAR,  26, 0);
    LOGT(44800, L"загрузка: payload_02.bin 14.6 МБ ... 64%");
    LOGT(45200, L"загрузка: payload_02.bin 14.6 МБ ... 100%");
    LOGT(45600, L"червь: Worm.Win32.Conficker ....... 391 файл");
    LOGT(45950, L"червь: Worm.Win32.Autorun ......... 1 248 файлов");
    EvAdd(46200, E_BAR,  38, 0);
    LOGT(46400, L"поражение сектора MBR ................ OK");
    LOGT(46800, L"блокировка восстановления системы .... OK");
    LOGT(47150, L"шифрование личных файлов: ключ 2048 бит");
    EvAdd(47400, E_BAR,  50, 0);
    LOGT(47600, L"перехват клавиатуры .................. OK");
    LOGT(48000, L"перехват буфера обмена ............... OK");
    LOGT(48350, L"кража сохранённых паролей: 62 записи");
    EvAdd(48600, E_BAR,  62, 0);
    LOGT(48800, L"майнер: XMR  установлен в C:\\ProgramData\\svc");
    LOGT(49200, L"открытие тылового порта 4444 ......... OK");
    LOGT(49550, L"репликация на узлы: 7 из 7");
    EvAdd(49800, E_BAR,  74, 0);
    LOGT(50000, L"вирус: Backdoor.Win32.Remote ........ 86 файлов");
    LOGT(50400, L"вирус: Spyware.Win32.KeyLog ......... 212 файлов");
    LOGT(50750, L"уничтожение теневых копий ............ OK");
    EvAdd(51000, E_BAR,  88, 0);
    LOGT(51200, L"заражено процессов: 41 / 41");
    LOGT(51600, L"заражено файлов: 12 480");
    EvAdd(52000, E_BAR,  96, 0);
    EvAdd(52400, E_BIG,  0, L"СИСТЕМА ЗАРАЖЕНА");
    EvAdd(52600, E_BAR, 100, L"ЗАРАЖЕНИЕ ВИРУСАМИ");
    LOGT(52800, L"проверка: система не подлежит восстановлению");

    /* ---------- 6. заметание следов ---------- */
    EvAdd(53000, E_BAR,   0, L"УДАЛЕНИЕ СЛЕДОВ");
    EvAdd(53100, E_BIG,   0, L"УДАЛЕНИЕ СЛЕДОВ");
    LOGT(53300, L"очистка журналов: C:\\Windows\\System32\\winevt\\Logs");
    EvAdd(53600, E_BAR,  18, 0);
    LOGT(53900, L"  Security.evtx ..................... удалён");
    LOGT(54300, L"  System.evtx ....................... удалён");
    LOGT(54650, L"  Application.evtx .................. удалён");
    EvAdd(54900, E_BAR,  38, 0);
    LOGT(55200, L"перезапись свободного места: 3 прохода");
    LOGT(55600, L"  проход 1/3 ......................... 100%");
    LOGT(56000, L"  проход 2/3 ......................... 100%");
    EvAdd(56200, E_BAR,  62, 0);
    LOGT(56500, L"  проход 3/3 ......................... 100%");
    LOGT(56900, L"удаление временных файлов: 4 812 шт.");
    EvAdd(57100, E_BAR,  82, 0);
    LOGT(57400, L"сброс точке восстановления ........... OK");
    EvAdd(57600, E_BAR,  94, 0);
    EvAdd(57800, E_BAR, 100, L"УДАЛЕНИЕ СЛЕДОВ");

    /* ---------- 7. финал ---------- */
    EvAdd(57900, E_BIG,   0, L"СИСТЕМА ВЗЛОМАНА");
    LOGT(58050, L"соединение закрыто");
    LOGT(58300, L"трассировка отключена");
    LOGT(58600, L"управление передано удалённому узлу");
    EvAdd(59000, E_FLASH, 0, 0);
    EvAdd(60000, E_END,   0, 0);

    qsort(gEv, (size_t)gNEv, sizeof(Ev), CmpEv);
}

/* ------------------------------------------------------------------ */
static void AddHexLine(void)
{
    wchar_t *o = gHex[gHexTotal % HEXLINES];
    int i;
    wsprintfW(o, L"%06X  ", (unsigned int)gHexAddr);
    for (i = 0; i < 6; i++) {
        wchar_t h[8];
        wsprintfW(h, L"%02X ", (unsigned int)(rnd() & 0xFF));
        wcscat(o, h);
    }
    gHexAddr += 16;
    gHexTotal++;
}

static void ApplyEvents(void)
{
    while (gEvIdx < gNEv && gEv[gEvIdx].t <= gNow) {
        Ev *e = &gEv[gEvIdx++];
        if (e->type == E_LOG) {
            wchar_t *dst = gLog[gLogTotal % MAXLOG];
            wcsncpy(dst, e->text, EVTEXT - 1);
            dst[EVTEXT - 1] = 0;
            gLogTotal++;
        } else if (e->type == E_BIG) {
            wcsncpy(gBig, e->text, EVTEXT - 1);
            gBig[EVTEXT - 1] = 0;
            gBigTime = gNow;
        } else if (e->type == E_BAR) {
            gBarVal = e->v;
            gBarTime = gNow;
            if (e->text[0]) {
                wcsncpy(gBarLabel, e->text, EVTEXT - 1);
                gBarLabel[EVTEXT - 1] = 0;
            }
        } else if (e->type == E_FLASH) {
            gFlash = 1;
        } else if (e->type == E_END) {
            gEnded = 1;
        }
    }
}

static int BarValue(void)
{
    int i, nv = -1, nt = 0, v = gBarVal, vt = gBarTime;
    for (i = gEvIdx; i < gNEv; i++) {
        if (gEv[i].type == E_BAR) { nv = gEv[i].v; nt = gEv[i].t; break; }
    }
    if (nv >= 0 && nt > vt) {
        long long d = (long long)(nv - v) * (long long)(gNow - vt) / (long long)(nt - vt);
        return v + (int)d;
    }
    return v;
}

/* ------------------------------------------------------------------ */
static void GetButtons(RECT *start, RECT *cancel)
{
    SetRect(start, 0, 0, 0, 0);
    SetRect(cancel, 0, 0, 0, 0);
    if (!gRunning) {
        int bw = gBaseH * 17, bh = gBaseH * 3, gap = gBaseH * 2;
        int x = (gW - (bw * 2 + gap)) / 2;
        int y = gH * 62 / 100;
        if (bw < 200) bw = 200;
        if (bh < 50) bh = 50;
        x = (gW - (bw * 2 + gap)) / 2;
        SetRect(start,  x, y, x + bw, y + bh);
        SetRect(cancel, x + bw + gap, y, x + bw * 2 + gap, y + bh);
    } else {
        int bw = gBaseH * 9, bh = gBaseH * 3 / 2;
        int y = (gBaseH * 2 - bh) / 2;
        SetRect(cancel, gW - gBaseH - bw, y, gW - gBaseH, y + bh);
    }
}

static void DrawButton(HDC dc, RECT r, const wchar_t *label, int hover)
{
    int w = r.right - r.left, h = r.bottom - r.top;
    COLORREF fg = hover ? RGB(210, 255, 215) : RGB(0, 220, 80);
    FillC(dc, r.left, r.top, w, 2, fg);
    FillC(dc, r.left, r.bottom - 2, w, 2, fg);
    FillC(dc, r.left, r.top, 2, h, fg);
    FillC(dc, r.right - 2, r.top, 2, h, fg);
    FillB(dc, r.left + 2, r.top + 2, w - 4, h - 4, hover ? gBrPanel : gBrBlack);
    if (hover) {
        FillC(dc, r.left + 2, r.top + 2, w - 4, 1, RGB(0, 120, 40));
    }
    TxtC(dc, gFLog, fg, r.left + w / 2, r.top + (h - gBaseH) / 2, label);
}

static void DrawFrame(HDC dc, int c)
{
    int m = gBaseH / 2;
    FillC(dc, m, m, gW - 2 * m, 1, c);
    FillC(dc, m, gH - m, gW - 2 * m, 1, c);
    FillC(dc, m, m, 1, gH - 2 * m, c);
    FillC(dc, gW - m, m, 1, gH - 2 * m, c);
}

static void DrawIdle(HDC dc)
{
    wchar_t buf[EVTEXT];
    const wchar_t *title = L"Взлом!";
    RECT rs, rc;
    int ty = gH * 19 / 100;
    int sub;

    DrawFrame(dc, RGB(0, 60, 20));

    /* corner brackets */
    FillC(dc, gBaseH / 2, gBaseH / 2, gBaseH, 2, COL_DIM);
    FillC(dc, gBaseH / 2, gBaseH / 2, 2, gBaseH, COL_DIM);
    FillC(dc, gW - gBaseH / 2 - gBaseH, gBaseH / 2, gBaseH, 2, COL_DIM);
    FillC(dc, gW - gBaseH / 2 - 2, gBaseH / 2, 2, gBaseH, COL_DIM);
    FillC(dc, gBaseH / 2, gH - gBaseH / 2 - 2, gBaseH, 2, COL_DIM);
    FillC(dc, gBaseH / 2, gH - gBaseH / 2 - gBaseH, 2, gBaseH, COL_DIM);
    FillC(dc, gW - gBaseH / 2 - gBaseH, gH - gBaseH / 2 - 2, gBaseH, 2, COL_DIM);
    FillC(dc, gW - gBaseH / 2 - 2, gH - gBaseH / 2 - gBaseH, 2, gBaseH, COL_DIM);

    /* glow + title */
    TxtC(dc, gFTitle, RGB(0, 70, 18), gW / 2 + 4, ty + 4, title);
    TxtC(dc, gFTitle, RGB(0, 120, 35), gW / 2 - 3, ty - 3, title);
    TxtC(dc, gFTitle, COL_HI, gW / 2, ty, title);

    sub = ty + gH / 7 + gBaseH;
    TxtC(dc, gFLog, COL_DIM, gW / 2, sub, L"СИСТЕМА УДАЛЁННОГО ДОСТУПА  ·  МОДУЛЬ 7.1");
    FillC(dc, gW / 2 - gW / 5, sub + gBaseH * 2, gW * 2 / 5, 1, COL_FAINT);
    TxtC(dc, gFLog, COL_FAINT, gW / 2, sub + gBaseH * 2 + gBaseH,
         L"ЦЕЛЬ: LOCALHOST:31337   ·   СТАТУС: ОЖИДАНИЕ");

    GetButtons(&rs, &rc);
    DrawButton(dc, rs, L"НАЧАТЬ ВЗЛОМ", gHover == 1);
    DrawButton(dc, rc, L"ОТМЕНИТЬ", gHover == 2);

    wsprintfW(buf, L"СЕАНС НЕ АКТИВЕН   ·   ПАКЕТОВ: 0   ·   V7.1.4");
    Txt(dc, gFLog, COL_FAINT, gBaseH * 2, gH - gBaseH * 3, buf);
    TxtR(dc, gFLog, COL_FAINT, gW - gBaseH * 2, gH - gBaseH * 3,
         ((gNow / 600) % 2) ? L"● СИСТЕМА ГОТОВА" : L"  СИСТЕМА ГОТОВА");
}

static void DrawRun(HDC dc)
{
    wchar_t buf[EVTEXT], buf2[EVTEXT];
    RECT rs, rc;
    int m = gBaseH;
    int hdrH = gBaseH * 2;
    int by, bigZone, contentTop, contentBot;
    int bottomH, barLabelY, barY, barBH;
    int logX, logY, logW, logH, lines, i;
    int sepX, px, pw;
    int pct;

    /* header */
    FillB(dc, 0, 0, gW, hdrH, gBrBlack);
    FillC(dc, 0, hdrH, gW, 1, COL_LINE);
    Txt(dc, gFLog, COL_TXT, m, (hdrH - gBaseH) / 2,
        L"ВЗЛОМ  ·  ЦЕЛЬ: LOCALHOST  ·  СЕАНС 0x7F3A2C");
    wsprintfW(buf, L"T+%02d:%02d", gNow / 60000, (gNow / 1000) % 60);
    TxtR(dc, gFLog, COL_DIM, gW - m - gBaseH * 11, (hdrH - gBaseH) / 2, buf);

    GetButtons(&rs, &rc);
    DrawButton(dc, rc, L"ОТМЕНА", gHover == 2);

    /* big headline */
    by = hdrH + gBaseH;
    bigZone = gBaseH * 3;
    if (gBig[0]) {
        int el = gNow - gBigTime;
        int n = el / 20;
        int len = (int)wcslen(gBig);
        if (n > len) n = len;
        if (n > 0) {
            wmemcpy(buf, gBig, (size_t)n);
            buf[n] = 0;
            TxtC(dc, gFBig, RGB(160, 255, 180), gW / 2, by, buf);
        }
        if (el > 0) {
            int w = (int)((long long)(gW - 2 * m) * (el > 400 ? 400 : el) / 400);
            FillC(dc, m, by + bigZone - gBaseH / 2, w, 1, COL_LINE);
        }
    }
    contentTop = by + bigZone;

    /* bottom progress */
    bottomH = gBaseH * 4;
    barLabelY = gH - m - bottomH + gBaseH / 2;
    barY = barLabelY + gBaseH + gBaseH / 3;
    barBH = gBaseH;
    contentBot = barLabelY - gBaseH;

    /* ------- log column ------- */
    logX = m;
    logY = contentTop;
    logW = gW * 66 / 100 - m;
    logH = contentBot - contentTop;
    lines = logH / gLineH;
    if (lines < 4) lines = 4;

    SelectClipRgn(dc, NULL);
    {
        HRGN rgn = CreateRectRgn(logX, logY, logX + logW, logY + logH);
        SelectClipRgn(dc, rgn);
        DeleteObject(rgn);
    }

    if (gLogTotal > 0) {
        int first = gLogTotal - lines;
        int cnt, startLine;
        if (first < 0) first = 0;
        cnt = gLogTotal - first;
        startLine = lines - cnt;
        for (i = 0; i < cnt; i++) {
            int idx = (first + i) % MAXLOG;
            int y = logY + (startLine + i) * gLineH;
            COLORREF c = (i == cnt - 1) ? COL_TXT : COL_DIM;
            if (i == cnt - 2) c = RGB(0, 190, 60);
            Txt(dc, gFLog, c, logX, y, gLog[idx]);
        }
    }
    /* cursor */
    if ((gNow / 350) % 2) {
        int cy = logY + (lines - 1) * gLineH;
        FillC(dc, logX, cy + gBaseH, gCharW - 2, 3, COL_TXT);
    }
    SelectClipRgn(dc, NULL);

    /* ------- right panel ------- */
    sepX = gW * 68 / 100;
    FillC(dc, sepX - m / 2, contentTop, 1, contentBot - contentTop, COL_FAINT);
    px = sepX + m / 2;
    pw = gW - m - px;
    {
        int y = contentTop;
        long long packets = 18402 + (long long)gNow * 41;
        long long files = gNow < 26500 ? 0 : (long long)(gNow - 26500) * 12480 / 15000;
        long long kb = files * 380 + (gNow < 26500 ? 0 : 412000);
        long long threats = gNow < 41500 ? 0 : (long long)(gNow - 41500) * 1246 / 11500;
        long long streams = 3 + gNow / 9000;
        wchar_t num[40];

        Txt(dc, gFLog, COL_DIM, px, y, L"СТАТИСТИКА");
        FillC(dc, px, y + gBaseH + 2, pw, 1, COL_FAINT);
        y += gBaseH + gBaseH;

#define STATROW(lbl, val) do { \
            Txt(dc, gFLog, COL_FAINT, px, y, (lbl)); \
            TxtR(dc, gFLog, COL_TXT, px + pw, y, (val)); \
            y += gLineH; } while (0)

        FmtNum(num, packets);
        STATROW(L"пакетов", num);
        FmtNum(num, 7);
        STATROW(L"узлов", num);
        FmtNum(num, files);
        STATROW(L"файлов", num);
        {
            wchar_t gb[40];
            long long whole = kb / (1024 * 1024);
            long long frac = (kb % (1024 * 1024)) * 100 / (1024 * 1024);
            wsprintfW(gb, L"%d.%02d ГБ", (int)whole, (int)frac);
            STATROW(L"объём", gb);
        }
        FmtNum(num, streams);
        STATROW(L"потоков", num);
        FmtNum(num, threats);
        STATROW(L"угроз", num);
        FmtNum(num, (long long)(1240 + gNow / 60));
        STATROW(L"запросов/с", num);
#undef STATROW

        y += gBaseH / 2;
        FillC(dc, px, y, pw, 1, COL_FAINT);
        y += gBaseH / 2;
        Txt(dc, gFLog, COL_DIM, px, y, L"ПОТОК ДАННЫХ");
        y += gBaseH + 4;

        if (y + gLineH < contentBot) {
            int hexLines = (contentBot - y) / gLineH;
            int hx, first;
            if (hexLines > HEXLINES) hexLines = HEXLINES;
            first = gHexTotal - hexLines;
            if (first < 0) first = 0;
            hx = gHexTotal - first;
            for (i = 0; i < hx; i++) {
                int idx = (first + i) % HEXLINES;
                Txt(dc, gFLog, COL_FAINT, px, y + i * gLineH, gHex[idx]);
            }
        }
    }

    /* ------- progress bar ------- */
    pct = BarValue();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    Txt(dc, gFLog, COL_TXT, m, barLabelY, gBarLabel);
    wsprintfW(buf, L"%d%%", pct);
    TxtR(dc, gFLog, COL_TXT, gW - m, barLabelY, buf);

    FillC(dc, m, barY, gW - 2 * m, 1, COL_LINE);
    FillC(dc, m, barY + barBH - 1, gW - 2 * m, 1, COL_LINE);
    FillC(dc, m, barY, 1, barBH, COL_LINE);
    FillC(dc, gW - m - 1, barY, 1, barBH, COL_LINE);
    FillB(dc, m + 1, barY + 1, gW - 2 * m - 2, barBH - 2, gBrBarBg);
    {
        int tw = (gW - 2 * m - 4);
        int fw = tw * pct / 100;
        FillB(dc, m + 2, barY + 2, fw, barBH - 4, gBrBarFg);
        if (fw > 3) FillC(dc, m + 2 + fw - 3, barY + 2, 3, barBH - 4, RGB(220, 255, 225));
        /* ticking marks */
        for (i = 1; i < 20; i++) {
            int x = m + 2 + tw * i / 20;
            FillC(dc, x, barY + 2, 1, 3, RGB(0, 60, 18));
        }
    }

    wsprintfW(buf2, L"СКОРОСТЬ: %d.%d МБ/с    ПОТОКОВ: %d    ОТКЛЮЧЕНИЙ: 0",
              (int)(8 + (gNow / 300) % 9), (int)((gNow / 70) % 10), 64);
    Txt(dc, gFLog, COL_FAINT, m, barY + barBH + gBaseH / 2, buf2);
}

static void Render(void)
{
    if (!gBackDC || !gRainDC || !gScrDC) return;

    BitBlt(gBackDC, 0, 0, gW, gH, gRainDC, 0, 0, SRCCOPY);

    if (gRunning) {
        if (gFlash) {
            FillC(gBackDC, 0, 0, gW, gH, RGB(120, 255, 150));
        } else {
            DrawRun(gBackDC);
        }
    } else {
        DrawIdle(gBackDC);
    }

    BitBlt(gScrDC, 0, 0, gW, gH, gBackDC, 0, 0, SRCCOPY);
}

/* ------------------------------------------------------------------ */
static void StartHack(void)
{
    if (gRunning) return;
    BuildScript();
    gEvIdx = 0;
    gNow = 0;
    gLogTotal = 0;
    gBig[0] = 0;
    gBigTime = -100000;
    gBarVal = 0;
    gBarTime = 0;
    gBarLabel[0] = 0;
    gFlash = 0;
    gEnded = 0;
    gHexTotal = 0;
    gLastHexTick = -1000;
    wcscpy(gBarLabel, L"ПОДГОТОВКА");
    gStart = GetTickCount();
    gRunning = 1;
    gHover = 0;
    SetCursor(LoadCursor(NULL, IDC_ARROW));
}

static void StopHack(void)
{
    gRunning = 0;
    gFlash = 0;
    gEnded = 0;
    gNow = 0;
    gHover = 0;
}

/* ------------------------------------------------------------------ */
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
        SetTimer(h, 1, 33, NULL);
        return 0;

    case WM_TIMER:
        if (wp != 1) break;
        if (gRunning) {
            gNow = (int)(GetTickCount() - gStart);
            ApplyEvents();
            if (gEnded) {
                DestroyWindow(h);
                return 0;
            }
        } else {
            gNow = (int)(GetTickCount() - gStart);
        }
        RainStep();
        if (gRunning && gNow - gLastHexTick >= 90) {
            gLastHexTick = gNow;
            AddHexLine();
        }
        Render();
        return 0;

    case WM_MOUSEMOVE: {
        POINT p;
        RECT rs, rc;
        int hv = 0;
        p.x = (short)LOWORD(lp);
        p.y = (short)HIWORD(lp);
        GetButtons(&rs, &rc);
        if (PtInRect(&rs, p)) hv = 1;
        else if (PtInRect(&rc, p)) hv = 2;
        if (hv != gHover) {
            gHover = hv;
            SetCursor(LoadCursor(NULL, hv ? IDC_HAND : IDC_ARROW));
            Render();
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, gHover ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        POINT p;
        RECT rs, rc;
        p.x = (short)LOWORD(lp);
        p.y = (short)HIWORD(lp);
        GetButtons(&rs, &rc);
        if (PtInRect(&rs, p) && !gRunning) {
            StartHack();
        } else if (PtInRect(&rc, p)) {
            if (gRunning) StopHack();
            else DestroyWindow(h);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(h); return 0; }
        if ((wp == VK_RETURN || wp == VK_SPACE) && !gRunning) { StartHack(); return 0; }
        break;

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
        if (gScrDC) { ReleaseDC(h, gScrDC); gScrDC = 0; }
        KillBuffers();
        if (gFLog) { DeleteObject(gFLog); gFLog = 0; }
        if (gFBig) { DeleteObject(gFBig); gFBig = 0; }
        if (gFTitle) { DeleteObject(gFTitle); gFTitle = 0; }
        if (gBrBlack) { DeleteObject(gBrBlack); gBrBlack = 0; }
        if (gBrPanel) { DeleteObject(gBrPanel); gBrPanel = 0; }
        if (gBrGreen) { DeleteObject(gBrGreen); gBrGreen = 0; }
        if (gBrBarBg) { DeleteObject(gBrBarBg); gBrBarBg = 0; }
        if (gBrBarFg) { DeleteObject(gBrBarFg); gBrBarFg = 0; }
        if (gBrLine) { DeleteObject(gBrLine); gBrLine = 0; }
        if (gBrDim) { DeleteObject(gBrDim); gBrDim = 0; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    WNDCLASSW wc;
    MSG msg;
    int sw, sh;
    typedef BOOL (WINAPI *PFN_DPIAWARE)(void);
    PFN_DPIAWARE pDpi;

    (void)hPrev; (void)cmd; (void)show;

    pDpi = (PFN_DPIAWARE)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware");
    if (pDpi) pDpi();

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"VZLOM_WND";
    if (!RegisterClassW(&wc)) return 0;

    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);

    gWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, L"VZLOM_WND", L"",
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
    return 0;
}
