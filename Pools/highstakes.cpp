/*
  highstakes.cpp (v0.5 OCR)
  - ScriptHookRDR2 ASI plugin sample
  - Detect poker using OCR keyword matching + hysteresis
  - Rockstar-style HUD toasts with legacy text fallback
  - INI config: highstakes.ini (next to game EXE)
  - Log: highstakes.log (next to game EXE)
  - Hot reload INI: PageUp
  - Toggle DrawMethod: PageDown
  - Money scanner/overlay: Delete toggles, End resets scan
*/

// Windows headers define min/max macros unless NOMINMAX is set, which breaks
// std::numeric_limits<T>::max() and other standard-library calls.
// These must be defined *before* any header that might include <windows.h>.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "script.h"
#include "global.h"
#include <windows.h>
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <limits>
#include <utility>

// ---------------- Logging ----------------
static FILE* gLog = nullptr;

static void Log(const char* fmt, ...)
{
    if (!gLog)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(gLog, fmt, args);
    va_end(args);
    fprintf(gLog, "\n");
    fflush(gLog);
}

// ---------------- INI helpers ----------------
static std::string IniGetString(const char* section, const char* key, const char* def, const char* path)
{
    char buf[512]{ 0 };
    GetPrivateProfileStringA(section, key, def, buf, sizeof(buf), path);
    return std::string(buf);
}

// robust-ish float parse (handles comma decimal by replacing with '.')
static float ParseFloatLoose(std::string s, float def)
{
    if (s.empty())
        return def;
    for (char& c : s)
        if (c == ',') c = '.';
    char* end = nullptr;
    float v = strtof(s.c_str(), &end);
    if (end == s.c_str())
        return def;
    return v;
}

static int IniGetInt(const char* section, const char* key, int def, const char* path)
{
    return GetPrivateProfileIntA(section, key, def, path);
}

static float IniGetFloat(const char* section, const char* key, float def, const char* path)
{
    std::string s = IniGetString(section, key, "", path);
    if (s.empty())
        return def;
    return ParseFloatLoose(s, def);
}

// ---------------- Settings ----------------
struct Settings
{
    float pokerRadius = 25.0f;          // meters (2D)
    int msgDurationMs = 1500;           // show "Mod Online" duration
    int enterCooldownMs = 3000;         // prevent spam
    int checkIntervalMs = 100;          // throttle detection
    int debugOverlay = 0;               // 1=show state text

    // -------- HUD style --------
    int hudUiMode = 2;                  // 0=legacy text, 1=hybrid panel+toasts, 2=rockstar toasts+panel
    int hudToastEnabled = 1;
    int hudToastFallbackText = 1;
    std::string hudToastIconDict = "ITEMTYPE_TEXTURES";
    std::string hudToastIcon = "ITEMTYPE_CASH";
    std::string hudToastColor = "COLOR_PURE_WHITE";
    int hudToastDurationMs = 450;
    std::string hudToastSoundSet = "";
    std::string hudToastSound = "";

    // -------- OCR detection --------
    int ocrEnabled = 1;
    int ocrIntervalMs = 1000;
    int ocrEnterHits = 1;
    int ocrExitHits = 1;
    int ocrEnterStableMs = 600;
    int ocrExitStableMs = 1800;
    int ocrProcessTimeoutMs = 2000;
    int ocrRegionXPct = 30;
    int ocrRegionYPct = 68;
    int ocrRegionWPct = 40;
    int ocrRegionHPct = 24;
    int ocrPsm = 6;
    int ocrDebugReasonOverlay = 0;
    std::string ocrTesseractPath = "tesseract";
    std::string ocrKeywords = "poker,ante,call,fold,raise,check,pot";

    // -------- Money sniffing (visual confirmation) --------
    int moneyOverlay = 1;               // 1=show money overlay while in poker
    int moneyScanEnable = 1;            // 1=scan globals for chip-like ints
    int moneyScanStart = 0;             // scan range start (global index)
    int moneyScanEnd = 100000;          // scan range end (exclusive)
    int moneyScanBatch = 512;           // indices per scan step
    int moneyScanIntervalMs = 20;       // ms between scan steps
    int moneyScanMaxReadsPerStep = 512; // hard cap of reads per frame step
    int moneyScanMaxStepMs = 4;         // soft time budget per frame step
    int moneyValueMin = 1;              // candidate int min (>=1 excludes zeros)
    int moneyValueMax = 500000;         // candidate int max
    int moneyTopN = 10;                 // show top N candidates
    int moneyPruneMs = 300000;          // drop candidates not seen recently
    int moneyLogEnable = 1;             // 1=write periodic money info to log
    int moneyLogIntervalMs = 3000;      // ms between money log snapshots
    int moneyLogTopN = 5;               // top changing candidates to log
    int moneyLogOnlyOnChange = 1;       // 1=skip repeated snapshots when nothing meaningful changed
    float moneyLikelyMaxChangesPerSec = 1.5f; // treat faster-changing candidates as noise
    int moneyExceptionLogCooldownMs = 30000; // SEH warning cooldown (0=log once per scan)
    int moneySkipFaultRuns = 1;         // 1=skip ahead after contiguous SEH faults

    // Optional watch list (once you identify the right globals)
    int potGlobal = -1;
    int stackGlobal0 = -1;
    int stackGlobal1 = -1;
    int stackGlobal2 = -1;
    int stackGlobal3 = -1;
    int stackGlobal4 = -1;
    int stackGlobal5 = -1;
};

static Settings gCfg;
static char gIniPath[MAX_PATH]{ 0 };
static char gLogPath[MAX_PATH]{ 0 };

// ---------------- ScriptHook export: getGlobalPtr ----------------
// Used for global scanning / watch-list reading.
using getGlobalPtr_t = uint64_t * (__cdecl*)(int globalIndex);
static getGlobalPtr_t gGetGlobalPtr = nullptr;
static bool gTriedResolveGetGlobalPtr = false;
static bool gGlobalReadSehFaultSeen = false;
static DWORD gNextGlobalReadFaultLogAt = 0;
static DWORD gLastResolveAttemptMs = 0;

static bool ClampIntSetting(const char* key, int& value, int minValue, int maxValue)
{
    int old = value;
    if (value < minValue)
        value = minValue;
    else if (value > maxValue)
        value = maxValue;

    if (old != value)
    {
        Log("[CFG] WARNING: Money.%s out of range (%d). Clamped to %d.", key, old, value);
        return true;
    }
    return false;
}

static bool ClampSectionIntSetting(const char* section, const char* key, int& value, int minValue, int maxValue)
{
    int old = value;
    if (value < minValue)
        value = minValue;
    else if (value > maxValue)
        value = maxValue;

    if (old != value)
    {
        Log("[CFG] WARNING: %s.%s out of range (%d). Clamped to %d.", section, key, old, value);
        return true;
    }
    return false;
}

static void ResolveGetGlobalPtrOnce()
{
    // v0.5 OCR: Retry every 5 seconds if first attempt failed (DLL load timing)
    DWORD now = GetTickCount();
    if (gGetGlobalPtr)
        return;
    if (gTriedResolveGetGlobalPtr && (now - gLastResolveAttemptMs) < 5000)
        return;

    gTriedResolveGetGlobalPtr = true;
    gLastResolveAttemptMs = now;

    // ScriptHook module name can vary across installs.
    const char* modules[] = {
        "ScriptHookRDR2.dll",
        "ScriptHookRDR2_V2.dll",
        "dinput8.dll"
    };

    const char* exportNames[] = {
        "getGlobalPtr",
        "?getGlobalPtr@@YAPEA_KH@Z"
    };

    for (const char* m : modules)
    {
        HMODULE hMod = GetModuleHandleA(m);
        if (!hMod)
            continue;
        for (const char* e : exportNames)
        {
            FARPROC proc = GetProcAddress(hMod, e);
            if (proc)
            {
                gGetGlobalPtr = reinterpret_cast<getGlobalPtr_t>(proc);
                Log("[MONEY] Resolved getGlobalPtr export '%s' from %s.", e, m);
                return;
            }
        }
    }

    if (!gGetGlobalPtr)
        Log("[MONEY] WARNING: getGlobalPtr export not found. Will retry in 5s.");
}

static bool ReadGlobalInt(int idx, int& out, bool* outSehFault = nullptr)
{
    if (outSehFault)
        *outSehFault = false;

    if (!gGetGlobalPtr || idx < 0)
        return false;
    __try
    {
        uint64_t* p = gGetGlobalPtr(idx);
        if (!p)
            return false;
        out = *(int*)p;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (outSehFault)
            *outSehFault = true;

        DWORD cooldownMs = (gCfg.moneyExceptionLogCooldownMs > 0)
            ? (DWORD)gCfg.moneyExceptionLogCooldownMs
            : 0;
        DWORD now = GetTickCount();

        if (!gGlobalReadSehFaultSeen)
        {
            if (cooldownMs > 0)
                Log("[MONEY] WARNING: Exception while reading script global (idx=%d). Throttling repeats for %lu ms.", idx, (unsigned long)cooldownMs);
            else
                Log("[MONEY] WARNING: Exception while reading script global (idx=%d). Suppressing further.", idx);

            gGlobalReadSehFaultSeen = true;
            gNextGlobalReadFaultLogAt = (cooldownMs > 0) ? (now + cooldownMs) : 0;
        }
        else if (cooldownMs > 0 && now >= gNextGlobalReadFaultLogAt)
        {
            Log("[MONEY] WARNING: Exception while reading script global (idx=%d).", idx);
            gNextGlobalReadFaultLogAt = now + cooldownMs;
        }
        return false;
    }
}

// ---------------- Money scanning state ----------------
struct MoneyCandidate
{
    int idx = -1;
    int last = 0;
    int changes = 0;
    DWORD firstSeenMs = 0;
    DWORD lastSeenMs = 0;
    DWORD lastChangeMs = 0;
};

static bool  gMoneyOverlayRuntime = true;
static DWORD gNextMoneyScanAt = 0;
static DWORD gNextMoneyLogAt = 0;
static DWORD gNextMoneyRescanAt = 0;  // v0.5 OCR: re-read existing candidates
static DWORD gNextFaultRunSkipLogAt = 0;
static DWORD gLastMoneySnapshotLogAt = 0;
static int   gMoneyScanCursor = 0;
static bool  gMoneyScanWrapped = false;
static int   gMoneyScanWrapCount = 0;
static int   gLastLoggedTopIdx = -1;
static int   gLastLoggedTopVal = 0;
static int   gLastLoggedCandCount = -1;
static std::unordered_map<int, MoneyCandidate> gMoneyCands;

static void ResetMoneyScan(DWORD now)
{
    gMoneyCands.clear();
    gMoneyScanCursor = gCfg.moneyScanStart;
    gMoneyScanWrapped = false;
    gMoneyScanWrapCount = 0;
    gNextMoneyScanAt = now;
    gNextMoneyRescanAt = now;
    gNextMoneyLogAt = now;
    gNextFaultRunSkipLogAt = now;
    gLastMoneySnapshotLogAt = 0;
    gLastLoggedTopIdx = -1;
    gLastLoggedTopVal = 0;
    gLastLoggedCandCount = -1;
    gGlobalReadSehFaultSeen = false;
    gNextGlobalReadFaultLogAt = 0;
    Log("[MONEY] Reset scan. Range=[%d..%d) Batch=%d IntervalMs=%d ValueRange=[%d..%d]",
        gCfg.moneyScanStart, gCfg.moneyScanEnd, gCfg.moneyScanBatch, gCfg.moneyScanIntervalMs,
        gCfg.moneyValueMin, gCfg.moneyValueMax);
}

static float CandidateChangesPerSec(const MoneyCandidate& c, DWORD now)
{
    if (now <= c.firstSeenMs)
        return 0.0f;
    float ageSec = (float)(now - c.firstSeenMs) / 1000.0f;
    if (ageSec <= 0.0f)
        return 0.0f;
    return (float)c.changes / ageSec;
}

static bool IsLikelyMoneyCandidate(const MoneyCandidate& c, DWORD now)
{
    if (c.changes <= 0)
        return false;

    float maxCps = gCfg.moneyLikelyMaxChangesPerSec;
    if (maxCps > 0.0f && CandidateChangesPerSec(c, now) > maxCps)
        return false;

    return true;
}

static bool BuildSortedCandidates(DWORD now, std::vector<const MoneyCandidate*>& sorted)
{
    sorted.clear();
    sorted.reserve(gMoneyCands.size());

    for (auto& kv : gMoneyCands)
    {
        if (IsLikelyMoneyCandidate(kv.second, now))
            sorted.push_back(&kv.second);
    }

    bool usingLikely = !sorted.empty();
    if (!usingLikely)
    {
        for (auto& kv : gMoneyCands)
            sorted.push_back(&kv.second);
    }

    std::sort(sorted.begin(), sorted.end(), [](const MoneyCandidate* a, const MoneyCandidate* b) {
        if (a->changes != b->changes) return a->changes > b->changes;
        return a->idx < b->idx;
    });

    return usingLikely;
}

// ---------------- HUD text ----------------
enum HudUiMode
{
    HUD_UI_MODE_LEGACY_TEXT = 0,
    HUD_UI_MODE_HYBRID_PANEL_TOASTS = 1,
    HUD_UI_MODE_ROCKSTAR_TOASTS_HYBRID = 2
};

enum HudToastEventKind
{
    HUD_TOAST_EVENT_GENERIC = 0,
    HUD_TOAST_EVENT_ENTER_POKER = 1,
    HUD_TOAST_EVENT_EXIT_POKER = 2,
    HUD_TOAST_EVENT_OCR_UNAVAILABLE = 3,
    HUD_TOAST_EVENT_MONEY_OVERLAY_TOGGLE = 4,
    HUD_TOAST_EVENT_MONEY_SCAN_RESET = 5
};

// DrawMethod:
// 1 = _BG_DISPLAY_TEXT (often reliable, legacy fallback path)
// 2 = candidate _DISPLAY_TEXT hash (verify for your build)
static int gDrawMethod = 1;
static bool gHudToastNativeFailed = false;
static bool gHudToastNativeWarned = false;
static std::string gLegacyHudMessage = "~COLOR_GOLD~Mod Online";
static DWORD gLegacyHudMessageUntil = 0;
static int gOcrStartFailureStreak = 0;
static bool gOcrStartFailureWarned = false;
static Hash gHudToastIconHash = 0;
static Hash gHudToastColorHash = 0;

// SET_TEXT_SCALE:        0x4170B650590B3B00
// SET_TEXT_CENTRE:       0xBE5261939FBECB8C
// _SET_TEXT_COLOR:       0x50A41AD966910F03
// _BG_DISPLAY_TEXT:      0x16794E044C9EFB58
// _DISPLAY_TEXT:         0xD79334A4BB99BAD1 (candidate; may differ per native DB)
static void DrawTextBasic(const char* msg, float x, float y, bool center)
{
    const char* str = MISC::VAR_STRING(10, "LITERAL_STRING", msg);

    invoke<Void>(0x4170B650590B3B00, 0.6f, 0.6f);          // SET_TEXT_SCALE
    invoke<Void>(0xBE5261939FBECB8C, center);              // SET_TEXT_CENTRE
    invoke<Void>(0x50A41AD966910F03, 255, 255, 255, 255);  // _SET_TEXT_COLOR

    if (gDrawMethod == 2)
    {
        invoke<Void>(0xD79334A4BB99BAD1, str, x, y);       // _DISPLAY_TEXT
    }
    else
    {
        invoke<Void>(0x16794E044C9EFB58, str, x, y);       // _BG_DISPLAY_TEXT
    }
}

static void DrawCenteredText(const char* msg, float x, float y)
{
    DrawTextBasic(msg, x, y, true);
}

static void DrawLeftText(const char* msg, float x, float y)
{
    DrawTextBasic(msg, x, y, false);
}

static bool HudUsesToastPath()
{
    return gCfg.hudUiMode != HUD_UI_MODE_LEGACY_TEXT && gCfg.hudToastEnabled;
}

static void ShowLegacyHudMessage(const char* msg, DWORD now, int durationMs)
{
    gLegacyHudMessage = msg ? msg : "";
    int safeDuration = (durationMs < 100) ? 100 : durationMs;
    gLegacyHudMessageUntil = now + (DWORD)safeDuration;
}

static void PostHudToast(const char* title, HudToastEventKind eventKind, DWORD now)
{
    (void)eventKind;

    bool attemptedToast = HudUsesToastPath();
    bool posted = false;
    if (attemptedToast && !gHudToastNativeFailed)
    {
        const char* iconDict = gCfg.hudToastIconDict.empty()
            ? "ITEMTYPE_TEXTURES"
            : gCfg.hudToastIconDict.c_str();
        const char* soundSet = gCfg.hudToastSoundSet.c_str();
        const char* soundToPlay = gCfg.hudToastSound.c_str();
        const char* toastTitle = MISC::VAR_STRING(10, "LITERAL_STRING", title ? title : "");

        __try
        {
            DisplayRightToast(
                toastTitle,
                iconDict,
                gHudToastIconHash,
                0,
                gHudToastColorHash,
                soundSet,
                soundToPlay,
                0,
                true,
                gCfg.hudToastDurationMs);
            posted = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            gHudToastNativeFailed = true;
            if (!gHudToastNativeWarned)
            {
                gHudToastNativeWarned = true;
                Log("[HUD] WARNING: Toast native path failed. Falling back to legacy text.");
            }
        }
    }

    if (!posted && attemptedToast && gCfg.hudToastFallbackText && gCfg.hudUiMode != HUD_UI_MODE_LEGACY_TEXT)
        ShowLegacyHudMessage(title ? title : "", now, gCfg.msgDurationMs);
}

// ---------------- Detection ----------------
struct DetectionInputs
{
    bool scanOk = false;
    bool seenKeyword = false;
    int keywordHits = 0;
    bool pending = false;
};

struct DetectionScore
{
    int total = 0;
    bool gateFail = false;
    const char* gateReason = "ok";
};

struct DetectionRuntime
{
    bool inPoker = false;
    DWORD enterCandidateSince = 0;
    DWORD exitCandidateSince = 0;
};

static DetectionInputs  gLastDetectInputs;
static DetectionScore   gLastDetectScore;
static DetectionRuntime gDetectRuntime;
static std::vector<std::string> gOcrKeywords;
static std::string gLastOcrText;
static char gOcrBmpPath[MAX_PATH]{ 0 };
static char gOcrOutBasePath[MAX_PATH]{ 0 };
static char gOcrTxtPath[MAX_PATH]{ 0 };
static HANDLE gOcrProcess = nullptr;
static DWORD gOcrProcessStartMs = 0;
static DWORD gNextOcrStartAt = 0;

static int ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static std::string TrimAscii(std::string s)
{
    size_t a = 0;
    while (a < s.size() && (unsigned char)s[a] <= ' ')
        a++;

    size_t b = s.size();
    while (b > a && (unsigned char)s[b - 1] <= ' ')
        b--;

    return s.substr(a, b - a);
}

static std::string ToLowerAscii(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return s;
}

static void BuildOcrKeywordList()
{
    gOcrKeywords.clear();

    std::string current;
    for (char c : gCfg.ocrKeywords)
    {
        if (c == ',' || c == ';' || c == '\n' || c == '\r' || c == '\t')
        {
            std::string t = TrimAscii(ToLowerAscii(current));
            if (!t.empty())
                gOcrKeywords.push_back(t);
            current.clear();
        }
        else
        {
            current.push_back(c);
        }
    }

    std::string tail = TrimAscii(ToLowerAscii(current));
    if (!tail.empty())
        gOcrKeywords.push_back(tail);
}

static bool SaveBitmap24(const char* path, HBITMAP bmp, HDC hdc, int width, int height)
{
    if (!bmp || width <= 0 || height <= 0)
        return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    int stride = ((width * 3 + 3) & ~3);
    int dataSize = stride * height;
    std::vector<unsigned char> pixels((size_t)dataSize);

    if (!GetDIBits(hdc, bmp, 0, (UINT)height, pixels.data(), &bi, DIB_RGB_COLORS))
        return false;

    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + dataSize;

    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f)
        return false;

    fwrite(&bfh, sizeof(bfh), 1, f);
    fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(pixels.data(), 1, (size_t)dataSize, f);
    fclose(f);
    return true;
}

static bool GetGameForegroundWindow(HWND& outHwnd)
{
    outHwnd = GetForegroundWindow();
    if (!outHwnd)
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(outHwnd, &pid);
    return pid == GetCurrentProcessId();
}

static bool CaptureOcrRegionToBmp(HWND hwnd)
{
    RECT rc{};
    if (!GetClientRect(hwnd, &rc))
        return false;

    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0)
        return false;

    int xPct = ClampInt(gCfg.ocrRegionXPct, 0, 100);
    int yPct = ClampInt(gCfg.ocrRegionYPct, 0, 100);
    int wPct = ClampInt(gCfg.ocrRegionWPct, 1, 100);
    int hPct = ClampInt(gCfg.ocrRegionHPct, 1, 100);

    int x = (cw * xPct) / 100;
    int y = (ch * yPct) / 100;
    int w = (cw * wPct) / 100;
    int h = (ch * hPct) / 100;

    if (x + w > cw) w = cw - x;
    if (y + h > ch) h = ch - y;
    if (w <= 0 || h <= 0)
        return false;

    POINT p{ 0, 0 };
    ClientToScreen(hwnd, &p);

    HDC screen = GetDC(nullptr);
    if (!screen)
        return false;

    HDC memdc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    if (!memdc || !bmp)
    {
        if (bmp) DeleteObject(bmp);
        if (memdc) DeleteDC(memdc);
        ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ old = SelectObject(memdc, bmp);
    BOOL bltOk = BitBlt(memdc, 0, 0, w, h, screen, p.x + x, p.y + y, SRCCOPY);
    if (old)
        SelectObject(memdc, old);

    bool writeOk = false;
    if (bltOk)
        writeOk = SaveBitmap24(gOcrBmpPath, bmp, memdc, w, h);

    DeleteObject(bmp);
    DeleteDC(memdc);
    ReleaseDC(nullptr, screen);

    return writeOk;
}

static bool ReadTextFileAll(const char* path, std::string& out)
{
    out.clear();
    FILE* f = nullptr;
    fopen_s(&f, path, "rb");
    if (!f)
        return false;

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0)
    {
        fclose(f);
        return true;
    }

    out.resize((size_t)n);
    fread(&out[0], 1, (size_t)n, f);
    fclose(f);
    return true;
}

static void StopOcrProcess(bool terminate)
{
    if (gOcrProcess)
    {
        if (terminate)
            TerminateProcess(gOcrProcess, 1);
        CloseHandle(gOcrProcess);
    }
    gOcrProcess = nullptr;
    gOcrProcessStartMs = 0;
}

static bool StartOcrProcess(DWORD now)
{
    if (gOcrProcess)
        return false;

    HWND hwnd = nullptr;
    if (!GetGameForegroundWindow(hwnd))
        return false;

    DeleteFileA(gOcrTxtPath);
    if (!CaptureOcrRegionToBmp(hwnd))
        return false;

    std::string cmd = "\"";
    cmd += gCfg.ocrTesseractPath;
    cmd += "\" \"";
    cmd += gOcrBmpPath;
    cmd += "\" \"";
    cmd += gOcrOutBasePath;
    cmd += "\" --psm ";
    cmd += std::to_string(gCfg.ocrPsm);
    cmd += " -l eng quiet";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<char> cmdLine(cmd.begin(), cmd.end());
    cmdLine.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr,
        cmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    if (!ok)
        return false;

    CloseHandle(pi.hThread);
    gOcrProcess = pi.hProcess;
    gOcrProcessStartMs = now;
    return true;
}

static bool TryCollectOcrResult(DWORD now, DetectionInputs& out, bool& hasResult)
{
    hasResult = false;
    out = DetectionInputs{};

    if (!gOcrProcess)
        return false;

    DWORD wait = WaitForSingleObject(gOcrProcess, 0);
    if (wait == WAIT_TIMEOUT)
    {
        if (gOcrProcessStartMs > 0 &&
            (now - gOcrProcessStartMs) >= (DWORD)gCfg.ocrProcessTimeoutMs)
        {
            StopOcrProcess(true);
            hasResult = true;
            out.scanOk = false;
        }
        else
        {
            out.pending = true;
        }
        return true;
    }

    if (wait == WAIT_OBJECT_0)
    {
        StopOcrProcess(false);
        std::string text;
        if (!ReadTextFileAll(gOcrTxtPath, text))
        {
            hasResult = true;
            out.scanOk = false;
            gLastOcrText.clear();
            return true;
        }

        text = ToLowerAscii(text);
        gLastOcrText = text;
        out.scanOk = true;
        for (const auto& kw : gOcrKeywords)
        {
            if (!kw.empty() && text.find(kw) != std::string::npos)
                out.keywordHits++;
        }
        out.seenKeyword = (out.keywordHits > 0);
        hasResult = true;
        return true;
    }

    // WAIT_FAILED / WAIT_ABANDONED: treat as OCR failure.
    StopOcrProcess(true);
    hasResult = true;
    out.scanOk = false;
    return true;
}

static DetectionScore ComputeDetectionScore(const DetectionInputs& in)
{
    DetectionScore out;

    if (!in.scanOk)
    {
        out.gateFail = true;
        out.gateReason = "ocrFail";
        return out;
    }

    out.total = in.keywordHits;
    out.gateReason = in.seenKeyword ? "ocrHit" : "ocrMiss";
    return out;
}

static bool UpdatePokerStateMachine(const DetectionScore& score, DWORD now)
{
    if (!gDetectRuntime.inPoker)
    {
        if (!score.gateFail && score.total >= gCfg.ocrEnterHits)
        {
            if (gDetectRuntime.enterCandidateSince == 0)
                gDetectRuntime.enterCandidateSince = now;
            else if ((now - gDetectRuntime.enterCandidateSince) >= (DWORD)gCfg.ocrEnterStableMs)
            {
                gDetectRuntime.inPoker = true;
                gDetectRuntime.enterCandidateSince = 0;
                gDetectRuntime.exitCandidateSince = 0;
            }
        }
        else
        {
            gDetectRuntime.enterCandidateSince = 0;
        }
    }
    else
    {
        if (score.gateFail || score.total < gCfg.ocrExitHits)
        {
            if (gDetectRuntime.exitCandidateSince == 0)
                gDetectRuntime.exitCandidateSince = now;
            else if ((now - gDetectRuntime.exitCandidateSince) >= (DWORD)gCfg.ocrExitStableMs)
            {
                gDetectRuntime.inPoker = false;
                gDetectRuntime.enterCandidateSince = 0;
                gDetectRuntime.exitCandidateSince = 0;
            }
        }
        else
        {
            gDetectRuntime.exitCandidateSince = 0;
        }
    }

    return gDetectRuntime.inPoker;
}

static bool ComputeInPokerV2(DWORD now)
{
    if (!gCfg.ocrEnabled)
    {
        StopOcrProcess(true);
        gOcrStartFailureStreak = 0;
        gOcrStartFailureWarned = false;
        return false;
    }

    DetectionInputs in;
    bool hasResult = false;
    TryCollectOcrResult(now, in, hasResult);

    if (!hasResult && !gOcrProcess && now >= gNextOcrStartAt)
    {
        // Start OCR only when the game window is currently foreground.
        if (StartOcrProcess(now))
        {
            gNextOcrStartAt = now + gCfg.ocrIntervalMs;
            in.pending = true;
            gOcrStartFailureStreak = 0;
            gOcrStartFailureWarned = false;
        }
        else
        {
            HWND hwnd = nullptr;
            if (!GetGameForegroundWindow(hwnd))
            {
                // Ignore alt-tab / non-game foreground transitions.
                return gDetectRuntime.inPoker;
            }
            gNextOcrStartAt = now + gCfg.ocrIntervalMs;
            hasResult = true;
            in.scanOk = false;

            gOcrStartFailureStreak++;
            if (gOcrStartFailureStreak >= 3 && !gOcrStartFailureWarned)
            {
                gOcrStartFailureWarned = true;
                Log("[OCR] WARNING: Failed to start OCR process repeatedly. Check OCR.TesseractPath.");
                PostHudToast("OCR unavailable - check TesseractPath", HUD_TOAST_EVENT_OCR_UNAVAILABLE, now);
            }
        }
    }

    if (!hasResult)
    {
        gLastDetectInputs = in;
        gLastDetectScore.gateReason = in.pending ? "pending" : "idle";
        return gDetectRuntime.inPoker;
    }

    DetectionScore score = ComputeDetectionScore(in);
    gLastDetectInputs = in;
    gLastDetectScore = score;

    return UpdatePokerStateMachine(score, now);
}
// ---------------- Message + state ----------------
static bool  gWasInPoker = false;
static DWORD gNextAllowedEnterMsg = 0;

// Detection throttling
static DWORD gNextDetectAt = 0;
static bool  gCachedInPoker = false;

static void LoadSettings()
{
    // Main
    gCfg.pokerRadius       = IniGetFloat("Main", "PokerRadius", 25.0f, gIniPath);
    gCfg.msgDurationMs     = IniGetInt("Main", "MessageDurationMs", 1500, gIniPath);
    gCfg.enterCooldownMs   = IniGetInt("Main", "EnterCooldownMs", 3000, gIniPath);
    gCfg.checkIntervalMs   = IniGetInt("Main", "CheckIntervalMs", 100, gIniPath);
    gCfg.debugOverlay      = IniGetInt("Main", "DebugOverlay", 0, gIniPath);

    // HUD
    gDrawMethod = IniGetInt("HUD", "DrawMethod", 1, gIniPath);
    gCfg.hudUiMode           = IniGetInt("HUD", "HUDUiMode", 2, gIniPath);
    gCfg.hudToastEnabled     = IniGetInt("HUD", "ToastEnabled", 1, gIniPath);
    gCfg.hudToastFallbackText = IniGetInt("HUD", "ToastFallbackText", 1, gIniPath);
    gCfg.hudToastIconDict    = IniGetString("HUD", "ToastIconDict", "ITEMTYPE_TEXTURES", gIniPath);
    gCfg.hudToastIcon        = IniGetString("HUD", "ToastIcon", "ITEMTYPE_CASH", gIniPath);
    gCfg.hudToastColor       = IniGetString("HUD", "ToastColor", "COLOR_PURE_WHITE", gIniPath);
    gCfg.hudToastDurationMs  = IniGetInt("HUD", "ToastDurationMs", 450, gIniPath);
    gCfg.hudToastSoundSet    = IniGetString("HUD", "ToastSoundSet", "", gIniPath);
    gCfg.hudToastSound       = IniGetString("HUD", "ToastSound", "", gIniPath);

    bool hudCfgClamped = false;
    hudCfgClamped |= ClampSectionIntSetting("HUD", "DrawMethod", gDrawMethod, 1, 2);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "HUDUiMode", gCfg.hudUiMode, HUD_UI_MODE_LEGACY_TEXT, HUD_UI_MODE_ROCKSTAR_TOASTS_HYBRID);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "ToastEnabled", gCfg.hudToastEnabled, 0, 1);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "ToastFallbackText", gCfg.hudToastFallbackText, 0, 1);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "ToastDurationMs", gCfg.hudToastDurationMs, 100, 10000);

    if (gCfg.hudToastIconDict.empty())
    {
        gCfg.hudToastIconDict = "ITEMTYPE_TEXTURES";
        hudCfgClamped = true;
        Log("[CFG] WARNING: HUD.ToastIconDict was empty. Defaulted to ITEMTYPE_TEXTURES.");
    }
    if (gCfg.hudToastIcon.empty())
    {
        gCfg.hudToastIcon = "ITEMTYPE_CASH";
        hudCfgClamped = true;
        Log("[CFG] WARNING: HUD.ToastIcon was empty. Defaulted to ITEMTYPE_CASH.");
    }
    if (gCfg.hudToastColor.empty())
    {
        gCfg.hudToastColor = "COLOR_PURE_WHITE";
        hudCfgClamped = true;
        Log("[CFG] WARNING: HUD.ToastColor was empty. Defaulted to COLOR_PURE_WHITE.");
    }

    gHudToastIconHash = MISC::GET_HASH_KEY(gCfg.hudToastIcon.c_str());
    gHudToastColorHash = MISC::GET_HASH_KEY(gCfg.hudToastColor.c_str());
    if (gHudToastIconHash == 0)
    {
        gHudToastIconHash = MISC::GET_HASH_KEY("ITEMTYPE_CASH");
        hudCfgClamped = true;
        Log("[CFG] WARNING: HUD.ToastIcon hash was 0. Defaulted to ITEMTYPE_CASH.");
    }
    if (gHudToastColorHash == 0)
    {
        gHudToastColorHash = MISC::GET_HASH_KEY("COLOR_PURE_WHITE");
        hudCfgClamped = true;
        Log("[CFG] WARNING: HUD.ToastColor hash was 0. Defaulted to COLOR_PURE_WHITE.");
    }

    if (hudCfgClamped)
        Log("[CFG] WARNING: Applied safety clamps to HUD settings.");

    // OCR
    gCfg.ocrEnabled            = IniGetInt("OCR", "Enabled", 1, gIniPath);
    gCfg.ocrIntervalMs         = IniGetInt("OCR", "IntervalMs", 1000, gIniPath);
    gCfg.ocrEnterHits          = IniGetInt("OCR", "EnterHits", 1, gIniPath);
    gCfg.ocrExitHits           = IniGetInt("OCR", "ExitHits", 1, gIniPath);
    gCfg.ocrEnterStableMs      = IniGetInt("OCR", "EnterStableMs", 600, gIniPath);
    gCfg.ocrExitStableMs       = IniGetInt("OCR", "ExitStableMs", 1800, gIniPath);
    gCfg.ocrProcessTimeoutMs   = IniGetInt("OCR", "ProcessTimeoutMs", 2000, gIniPath);
    gCfg.ocrRegionXPct         = IniGetInt("OCR", "RegionXPct", 30, gIniPath);
    gCfg.ocrRegionYPct         = IniGetInt("OCR", "RegionYPct", 68, gIniPath);
    gCfg.ocrRegionWPct         = IniGetInt("OCR", "RegionWPct", 40, gIniPath);
    gCfg.ocrRegionHPct         = IniGetInt("OCR", "RegionHPct", 24, gIniPath);
    gCfg.ocrPsm                = IniGetInt("OCR", "PSM", 6, gIniPath);
    gCfg.ocrDebugReasonOverlay = IniGetInt("OCR", "DebugReasonOverlay", 0, gIniPath);
    gCfg.ocrTesseractPath      = IniGetString("OCR", "TesseractPath", "tesseract", gIniPath);
    gCfg.ocrKeywords           = IniGetString("OCR", "Keywords", "poker,ante,call,fold,raise,check,pot", gIniPath);

    gCfg.ocrEnabled            = ClampInt(gCfg.ocrEnabled, 0, 1);
    gCfg.ocrIntervalMs         = ClampInt(gCfg.ocrIntervalMs, 200, 30000);
    gCfg.ocrEnterHits          = ClampInt(gCfg.ocrEnterHits, 1, 32);
    gCfg.ocrExitHits           = ClampInt(gCfg.ocrExitHits, 1, 32);
    gCfg.ocrEnterStableMs      = ClampInt(gCfg.ocrEnterStableMs, 0, 10000);
    gCfg.ocrExitStableMs       = ClampInt(gCfg.ocrExitStableMs, 0, 10000);
    gCfg.ocrProcessTimeoutMs   = ClampInt(gCfg.ocrProcessTimeoutMs, 250, 10000);
    gCfg.ocrRegionXPct         = ClampInt(gCfg.ocrRegionXPct, 0, 100);
    gCfg.ocrRegionYPct         = ClampInt(gCfg.ocrRegionYPct, 0, 100);
    gCfg.ocrRegionWPct         = ClampInt(gCfg.ocrRegionWPct, 1, 100);
    gCfg.ocrRegionHPct         = ClampInt(gCfg.ocrRegionHPct, 1, 100);
    gCfg.ocrPsm                = ClampInt(gCfg.ocrPsm, 3, 13);
    gCfg.ocrDebugReasonOverlay = ClampInt(gCfg.ocrDebugReasonOverlay, 0, 1);

    BuildOcrKeywordList();
    StopOcrProcess(true);
    gNextOcrStartAt = 0;
    gOcrStartFailureStreak = 0;
    gOcrStartFailureWarned = false;
    gHudToastNativeFailed = false;
    gHudToastNativeWarned = false;
    gLegacyHudMessage = "~COLOR_GOLD~Mod Online";
    gLegacyHudMessageUntil = 0;

    // Money
    gCfg.moneyOverlay           = IniGetInt("Money", "Overlay", 1, gIniPath);
    gCfg.moneyScanEnable        = IniGetInt("Money", "ScanEnable", 1, gIniPath);
    gCfg.moneyScanStart         = IniGetInt("Money", "ScanStart", 0, gIniPath);
    gCfg.moneyScanEnd           = IniGetInt("Money", "ScanEnd", 100000, gIniPath);
    gCfg.moneyScanBatch         = IniGetInt("Money", "ScanBatch", 512, gIniPath);
    gCfg.moneyScanIntervalMs    = IniGetInt("Money", "ScanIntervalMs", 20, gIniPath);
    gCfg.moneyScanMaxReadsPerStep = IniGetInt("Money", "ScanMaxReadsPerStep", 512, gIniPath);
    gCfg.moneyScanMaxStepMs     = IniGetInt("Money", "ScanMaxStepMs", 4, gIniPath);
    gCfg.moneyValueMin          = IniGetInt("Money", "ValueMin", 1, gIniPath);
    gCfg.moneyValueMax          = IniGetInt("Money", "ValueMax", 500000, gIniPath);
    gCfg.moneyTopN              = IniGetInt("Money", "TopN", 10, gIniPath);
    gCfg.moneyPruneMs           = IniGetInt("Money", "PruneMs", 300000, gIniPath);
    gCfg.moneyLogEnable         = IniGetInt("Money", "LogEnable", 1, gIniPath);
    gCfg.moneyLogIntervalMs     = IniGetInt("Money", "LogIntervalMs", 3000, gIniPath);
    gCfg.moneyLogTopN           = IniGetInt("Money", "LogTopN", 5, gIniPath);
    gCfg.moneyLogOnlyOnChange   = IniGetInt("Money", "LogOnlyOnChange", 1, gIniPath);
    gCfg.moneyLikelyMaxChangesPerSec = IniGetFloat("Money", "LikelyMaxChangesPerSec", 1.5f, gIniPath);
    gCfg.moneyExceptionLogCooldownMs = IniGetInt("Money", "ExceptionLogCooldownMs", 30000, gIniPath);
    gCfg.moneySkipFaultRuns     = IniGetInt("Money", "SkipFaultRuns", 1, gIniPath);

    gCfg.potGlobal    = IniGetInt("Money", "PotGlobal", -1, gIniPath);
    gCfg.stackGlobal0 = IniGetInt("Money", "StackGlobal0", -1, gIniPath);
    gCfg.stackGlobal1 = IniGetInt("Money", "StackGlobal1", -1, gIniPath);
    gCfg.stackGlobal2 = IniGetInt("Money", "StackGlobal2", -1, gIniPath);
    gCfg.stackGlobal3 = IniGetInt("Money", "StackGlobal3", -1, gIniPath);
    gCfg.stackGlobal4 = IniGetInt("Money", "StackGlobal4", -1, gIniPath);
    gCfg.stackGlobal5 = IniGetInt("Money", "StackGlobal5", -1, gIniPath);

    bool moneyCfgClamped = false;
    moneyCfgClamped |= ClampIntSetting("ScanStart", gCfg.moneyScanStart, 0, std::numeric_limits<int>::max() - 1);
    moneyCfgClamped |= ClampIntSetting("ScanEnd", gCfg.moneyScanEnd, 1, std::numeric_limits<int>::max());
    moneyCfgClamped |= ClampIntSetting("ScanBatch", gCfg.moneyScanBatch, 1, 1000000);
    moneyCfgClamped |= ClampIntSetting("ScanIntervalMs", gCfg.moneyScanIntervalMs, 1, 60000);
    moneyCfgClamped |= ClampIntSetting("ScanMaxReadsPerStep", gCfg.moneyScanMaxReadsPerStep, 1, 1000000);
    moneyCfgClamped |= ClampIntSetting("ScanMaxStepMs", gCfg.moneyScanMaxStepMs, 1, 1000);
    moneyCfgClamped |= ClampIntSetting("LogOnlyOnChange", gCfg.moneyLogOnlyOnChange, 0, 1);
    moneyCfgClamped |= ClampIntSetting("ExceptionLogCooldownMs", gCfg.moneyExceptionLogCooldownMs, 0, 600000);
    moneyCfgClamped |= ClampIntSetting("SkipFaultRuns", gCfg.moneySkipFaultRuns, 0, 1);
    moneyCfgClamped |= ClampIntSetting("LogTopN", gCfg.moneyLogTopN, 0, 64);

    if (gCfg.moneyLikelyMaxChangesPerSec < 0.0f)
    {
        gCfg.moneyLikelyMaxChangesPerSec = 0.0f;
        moneyCfgClamped = true;
        Log("[CFG] WARNING: Money.LikelyMaxChangesPerSec was negative. Clamped to 0.");
    }
    else if (gCfg.moneyLikelyMaxChangesPerSec > 1000.0f)
    {
        gCfg.moneyLikelyMaxChangesPerSec = 1000.0f;
        moneyCfgClamped = true;
        Log("[CFG] WARNING: Money.LikelyMaxChangesPerSec too large. Clamped to 1000.");
    }

    if (gCfg.moneyScanEnd <= gCfg.moneyScanStart)
    {
        int oldEnd = gCfg.moneyScanEnd;
        gCfg.moneyScanEnd = gCfg.moneyScanStart + 1;
        moneyCfgClamped = true;
        Log("[CFG] WARNING: Money.ScanEnd must be > ScanStart (%d <= %d). Clamped to %d.",
            oldEnd, gCfg.moneyScanStart, gCfg.moneyScanEnd);
    }

    if (gCfg.moneyValueMax < gCfg.moneyValueMin)
    {
        int oldMax = gCfg.moneyValueMax;
        gCfg.moneyValueMax = gCfg.moneyValueMin;
        moneyCfgClamped = true;
        Log("[CFG] WARNING: Money.ValueMax (%d) < ValueMin (%d). Clamped to %d.",
            oldMax, gCfg.moneyValueMin, gCfg.moneyValueMax);
    }

    if (moneyCfgClamped)
        Log("[CFG] WARNING: Applied safety clamps to Money settings.");

    // Log config
    Log("[CFG] PokerRadius=%.2f MsgMs=%d CooldownMs=%d CheckIntervalMs=%d DebugOverlay=%d",
        gCfg.pokerRadius, gCfg.msgDurationMs, gCfg.enterCooldownMs, gCfg.checkIntervalMs,
        gCfg.debugOverlay);
    Log("[CFG] OCR: Enabled=%d IntervalMs=%d EnterHits=%d ExitHits=%d EnterStableMs=%d ExitStableMs=%d ProcTimeoutMs=%d Region=(%d,%d,%d,%d) PSM=%d DebugReason=%d Tesseract='%s' Keywords=%d",
        gCfg.ocrEnabled, gCfg.ocrIntervalMs,
        gCfg.ocrEnterHits, gCfg.ocrExitHits,
        gCfg.ocrEnterStableMs, gCfg.ocrExitStableMs,
        gCfg.ocrProcessTimeoutMs,
        gCfg.ocrRegionXPct, gCfg.ocrRegionYPct, gCfg.ocrRegionWPct, gCfg.ocrRegionHPct,
        gCfg.ocrPsm, gCfg.ocrDebugReasonOverlay,
        gCfg.ocrTesseractPath.c_str(), (int)gOcrKeywords.size());
    Log("[CFG] HUD: DrawMethod=%d HUDUiMode=%d ToastEnabled=%d ToastFallbackText=%d ToastIconDict='%s' ToastIcon='%s' ToastColor='%s' ToastDurationMs=%d ToastSoundSet='%s' ToastSound='%s'",
        gDrawMethod, gCfg.hudUiMode, gCfg.hudToastEnabled, gCfg.hudToastFallbackText,
        gCfg.hudToastIconDict.c_str(), gCfg.hudToastIcon.c_str(), gCfg.hudToastColor.c_str(),
        gCfg.hudToastDurationMs, gCfg.hudToastSoundSet.c_str(), gCfg.hudToastSound.c_str());

    Log("[CFG] Money: Overlay=%d ScanEnable=%d Range=[%d..%d) Batch=%d IntervalMs=%d ValueRange=[%d..%d] TopN=%d PruneMs=%d",
        gCfg.moneyOverlay, gCfg.moneyScanEnable,
        gCfg.moneyScanStart, gCfg.moneyScanEnd,
        gCfg.moneyScanBatch, gCfg.moneyScanIntervalMs,
        gCfg.moneyValueMin, gCfg.moneyValueMax,
        gCfg.moneyTopN, gCfg.moneyPruneMs);
    Log("[CFG] Money perf: ScanMaxReadsPerStep=%d ScanMaxStepMs=%d ExceptionLogCooldownMs=%d SkipFaultRuns=%d LikelyMaxChangesPerSec=%.2f",
        gCfg.moneyScanMaxReadsPerStep, gCfg.moneyScanMaxStepMs,
        gCfg.moneyExceptionLogCooldownMs, gCfg.moneySkipFaultRuns, gCfg.moneyLikelyMaxChangesPerSec);
    if (gCfg.moneyLogEnable)
    {
        Log("[CFG] Money log: LogEnable=%d LogIntervalMs=%d LogTopN=%d LogOnlyOnChange=%d",
            gCfg.moneyLogEnable, gCfg.moneyLogIntervalMs, gCfg.moneyLogTopN, gCfg.moneyLogOnlyOnChange);
    }

    // Estimate wrap time
    if (gCfg.moneyScanEnable && gCfg.moneyScanBatch > 0 && gCfg.moneyScanIntervalMs > 0)
    {
        int range = gCfg.moneyScanEnd - gCfg.moneyScanStart;
        int effectiveBatch = (std::min)(gCfg.moneyScanBatch, gCfg.moneyScanMaxReadsPerStep);
        if (effectiveBatch > 0)
        {
            float steps = (float)range / (float)effectiveBatch;
            float wrapSec = steps * gCfg.moneyScanIntervalMs / 1000.0f;
            Log("[CFG] Money estWrap=%.1fs (approx, cap-based)", wrapSec);
        }
    }

    Log("[CFG] Money Watch: PotGlobal=%d StackGlobals=%d,%d,%d,%d,%d,%d",
        gCfg.potGlobal,
        gCfg.stackGlobal0, gCfg.stackGlobal1, gCfg.stackGlobal2,
        gCfg.stackGlobal3, gCfg.stackGlobal4, gCfg.stackGlobal5);
}

static void DrawWatchLine(const char* label, int idx, float x, float& y)
{
    if (idx < 0) return;
    int val = 0;
    bool ok = ReadGlobalInt(idx, val);
    char buf[128];
    if (ok)
        _snprintf_s(buf, sizeof(buf), "%s [%d] = %d", label, idx, val);
    else
        _snprintf_s(buf, sizeof(buf), "%s [%d] = ???", label, idx);
    DrawLeftText(buf, x, y);
    y += 0.025f;
}

// v0.5 OCR: Re-read all existing candidates and track value changes
static void RescanExistingCandidates(DWORD now)
{
    int reads = 0;
    int maxReads = gCfg.moneyScanMaxReadsPerStep;
    std::vector<int> toRemove;

    for (auto& kv : gMoneyCands)
    {
        if (reads >= maxReads)
            break;

        int val = 0;
        bool ok = ReadGlobalInt(kv.first, val);
        reads++;

        if (!ok)
        {
            toRemove.push_back(kv.first);
            continue;
        }

        // Check if value left valid range
        if (val < gCfg.moneyValueMin || val > gCfg.moneyValueMax)
        {
            toRemove.push_back(kv.first);
            continue;
        }

        // Detect change
        if (val != kv.second.last)
        {
            kv.second.changes++;
            kv.second.last = val;
            kv.second.lastChangeMs = now;
            kv.second.lastSeenMs = now;
        }
    }

    for (int idx : toRemove)
        gMoneyCands.erase(idx);
}

static void MoneyTick(bool inPoker, DWORD now)
{
    // Hotkeys (always active)
    if (GetAsyncKeyState(VK_DELETE) & 1)
    {
        gMoneyOverlayRuntime = !gMoneyOverlayRuntime;
        Log("[MONEY] DEL: Money overlay %s", gMoneyOverlayRuntime ? "ON" : "OFF");
        if (gCfg.hudUiMode != HUD_UI_MODE_LEGACY_TEXT)
            PostHudToast(gMoneyOverlayRuntime ? "Money overlay enabled" : "Money overlay disabled", HUD_TOAST_EVENT_MONEY_OVERLAY_TOGGLE, now);
    }
    if (GetAsyncKeyState(VK_END) & 1)
    {
        ResetMoneyScan(now);
        if (gCfg.hudUiMode != HUD_UI_MODE_LEGACY_TEXT)
            PostHudToast("Money scan reset", HUD_TOAST_EVENT_MONEY_SCAN_RESET, now);
    }

    // Resolve getGlobalPtr (retries automatically)
    ResolveGetGlobalPtrOnce();

    if (!inPoker || !gCfg.moneyOverlay || !gMoneyOverlayRuntime)
        return;

    // ---- Scan: discover new candidates ----
    if (gCfg.moneyScanEnable && gGetGlobalPtr && now >= gNextMoneyScanAt)
    {
        gNextMoneyScanAt = now + gCfg.moneyScanIntervalMs;
        DWORD stepStart = GetTickCount();

        int reads = 0;
        int maxReads = gCfg.moneyScanMaxReadsPerStep;
        int consecutiveSehFaults = 0;
        constexpr int kFaultRunThreshold = 16;
        constexpr int kFaultRunSkipSpan = 256;
        int batchEnd = (std::min)(gMoneyScanCursor + gCfg.moneyScanBatch, gCfg.moneyScanEnd);

        for (int i = gMoneyScanCursor; i < batchEnd; i++)
        {
            if (reads >= maxReads)
                break;
            if ((GetTickCount() - stepStart) >= (DWORD)gCfg.moneyScanMaxStepMs)
                break;

            // Skip if already a candidate
            if (gMoneyCands.count(i))
            {
                gMoneyScanCursor = i + 1;
                continue;
            }

            int val = 0;
            bool sehFault = false;
            reads++;
            if (!ReadGlobalInt(i, val, &sehFault))
            {
                if (sehFault)
                    consecutiveSehFaults++;
                else
                    consecutiveSehFaults = 0;

                gMoneyScanCursor = i + 1;

                if (gCfg.moneySkipFaultRuns && sehFault && consecutiveSehFaults >= kFaultRunThreshold)
                {
                    int oldCursor = gMoneyScanCursor;
                    int skipCursor = (std::min)(i + kFaultRunSkipSpan + 1, gCfg.moneyScanEnd);
                    if (skipCursor > oldCursor)
                    {
                        gMoneyScanCursor = skipCursor;
                        if (now >= gNextFaultRunSkipLogAt)
                        {
                            Log("[MONEY] SkipFaultRuns: %d consecutive SEH faults near idx=%d. cursor %d -> %d.",
                                consecutiveSehFaults, i, oldCursor, gMoneyScanCursor);
                            gNextFaultRunSkipLogAt = now + 2000;
                        }
                    }
                    break;
                }
                continue;
            }
            consecutiveSehFaults = 0;

            if (val >= gCfg.moneyValueMin && val <= gCfg.moneyValueMax)
            {
                MoneyCandidate mc;
                mc.idx = i;
                mc.last = val;
                mc.changes = 0;
                mc.firstSeenMs = now;
                mc.lastSeenMs = now;
                mc.lastChangeMs = 0;
                gMoneyCands[i] = mc;
            }

            gMoneyScanCursor = i + 1;
        }

        // Wrap
        if (gMoneyScanCursor >= gCfg.moneyScanEnd)
        {
            gMoneyScanCursor = gCfg.moneyScanStart;
            gMoneyScanWrapCount++;
            if (!gMoneyScanWrapped)
            {
                gMoneyScanWrapped = true;
                Log("[MONEY] First full scan wrap complete. candidates=%d wraps=%d",
                    (int)gMoneyCands.size(), gMoneyScanWrapCount);
            }
        }
    }

    // ---- Re-read existing candidates to detect value changes ----
    if (gGetGlobalPtr && now >= gNextMoneyRescanAt)
    {
        gNextMoneyRescanAt = now + (gCfg.moneyScanIntervalMs / 2);  // rescan faster than discovery
        RescanExistingCandidates(now);
    }

    // ---- Prune stale candidates ----
    if (gCfg.moneyPruneMs > 0)
    {
        std::vector<int> pruneList;
        for (auto& kv : gMoneyCands)
        {
            // Only prune candidates with 0 changes that are old
            if (kv.second.changes == 0 && (now - kv.second.firstSeenMs) > (DWORD)gCfg.moneyPruneMs)
                pruneList.push_back(kv.first);
        }
        for (int idx : pruneList)
            gMoneyCands.erase(idx);
    }

    // ---- Log snapshot ----
    if (gCfg.moneyLogEnable && now >= gNextMoneyLogAt)
    {
        gNextMoneyLogAt = now + gCfg.moneyLogIntervalMs;

        std::vector<const MoneyCandidate*> sorted;
        bool usingLikely = BuildSortedCandidates(now, sorted);

        bool shouldLog = true;
        if (gCfg.moneyLogOnlyOnChange)
        {
            int topIdx = sorted.empty() ? -1 : sorted[0]->idx;
            int topVal = sorted.empty() ? 0 : sorted[0]->last;
            int candCount = (int)gMoneyCands.size();
            int candDiff = (gLastLoggedCandCount < 0) ? candCount : (candCount - gLastLoggedCandCount);
            if (candDiff < 0) candDiff = -candDiff;
            DWORD heartbeatMs = (DWORD)(std::max)(15000, gCfg.moneyLogIntervalMs * 10);
            bool heartbeatDue = (gLastMoneySnapshotLogAt == 0) || ((now - gLastMoneySnapshotLogAt) >= heartbeatMs);
            bool changed = (topIdx != gLastLoggedTopIdx) || (topVal != gLastLoggedTopVal) || (candDiff >= 256);
            shouldLog = heartbeatDue || changed;
        }

        if (shouldLog)
        {
            gLastMoneySnapshotLogAt = now;
            gLastLoggedTopIdx = sorted.empty() ? -1 : sorted[0]->idx;
            gLastLoggedTopVal = sorted.empty() ? 0 : sorted[0]->last;
            gLastLoggedCandCount = (int)gMoneyCands.size();

            Log("[MONEY] Snapshot: inPoker=%d scan=%d cands=%d cursor=%d/%d wraps=%d mode=%s",
                inPoker ? 1 : 0, gCfg.moneyScanEnable,
                (int)gMoneyCands.size(), gMoneyScanCursor, gCfg.moneyScanEnd,
                gMoneyScanWrapCount, usingLikely ? "likely" : "all");

            int logN = (std::min)((int)sorted.size(), gCfg.moneyLogTopN);
            for (int i = 0; i < logN; i++)
            {
                float cps = CandidateChangesPerSec(*sorted[i], now);
                Log("[MONEY] Cand idx=%d val=%d (~%.2f if cents) changes=%d rate=%.2f/s",
                    sorted[i]->idx, sorted[i]->last, (double)sorted[i]->last / 100.0,
                    sorted[i]->changes, cps);
            }
        }
    }

    // ---- Draw overlay ----
    float ox = 0.01f, oy = 0.40f;
    char buf[256];

    if (gCfg.hudUiMode >= HUD_UI_MODE_HYBRID_PANEL_TOASTS)
    {
        DrawLeftText("Poker Scanner", ox, oy);
        oy += 0.025f;
    }

    // Watch list (known globals)
    DrawWatchLine("Pot", gCfg.potGlobal, ox, oy);
    DrawWatchLine("Stk0", gCfg.stackGlobal0, ox, oy);
    DrawWatchLine("Stk1", gCfg.stackGlobal1, ox, oy);
    DrawWatchLine("Stk2", gCfg.stackGlobal2, ox, oy);
    DrawWatchLine("Stk3", gCfg.stackGlobal3, ox, oy);
    DrawWatchLine("Stk4", gCfg.stackGlobal4, ox, oy);
    DrawWatchLine("Stk5", gCfg.stackGlobal5, ox, oy);

    // Scanner status
    _snprintf_s(buf, sizeof(buf), "Scanner idx=%d/%d cands=%d wraps=%d",
        gMoneyScanCursor, gCfg.moneyScanEnd,
        (int)gMoneyCands.size(), gMoneyScanWrapCount);
    DrawLeftText(buf, ox, oy);
    oy += 0.025f;

    // Top N candidates sorted by likely money behavior first.
    std::vector<const MoneyCandidate*> sorted;
    BuildSortedCandidates(now, sorted);

    int showN = (std::min)((int)sorted.size(), gCfg.moneyTopN);
    for (int i = 0; i < showN; i++)
    {
        const MoneyCandidate* c = sorted[i];
        _snprintf_s(buf, sizeof(buf), "C%02d idx=%d val=%d ($%.2f) chg=%d",
            i + 1, c->idx, c->last, (double)c->last / 100.0, c->changes);
        DrawLeftText(buf, ox, oy);
        oy += 0.022f;
    }
}

static void InitPaths()
{
    // Get game EXE directory for INI and log paths
    GetModuleFileNameA(nullptr, gIniPath, MAX_PATH);
    char* lastSlash = strrchr(gIniPath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    strcpy_s(gLogPath, MAX_PATH, gIniPath);
    strcat_s(gLogPath, MAX_PATH, "highstakes.log");
    strcat_s(gIniPath, MAX_PATH, "highstakes.ini");

    char tempPath[MAX_PATH]{ 0 };
    DWORD tn = GetTempPathA(MAX_PATH, tempPath);
    if (tn > 0 && tn < MAX_PATH)
    {
        strcpy_s(gOcrBmpPath, MAX_PATH, tempPath);
        strcat_s(gOcrBmpPath, MAX_PATH, "highstakes_ocr.bmp");

        strcpy_s(gOcrOutBasePath, MAX_PATH, tempPath);
        strcat_s(gOcrOutBasePath, MAX_PATH, "highstakes_ocr");

        strcpy_s(gOcrTxtPath, MAX_PATH, tempPath);
        strcat_s(gOcrTxtPath, MAX_PATH, "highstakes_ocr.txt");
    }
    else
    {
        strcpy_s(gOcrBmpPath, MAX_PATH, gIniPath);
        strcat_s(gOcrBmpPath, MAX_PATH, "highstakes_ocr.bmp");

        strcpy_s(gOcrOutBasePath, MAX_PATH, gIniPath);
        strcat_s(gOcrOutBasePath, MAX_PATH, "highstakes_ocr");

        strcpy_s(gOcrTxtPath, MAX_PATH, gIniPath);
        strcat_s(gOcrTxtPath, MAX_PATH, "highstakes_ocr.txt");
    }
}

static void Tick()
{
    DWORD now = GetTickCount();
    Player plr = PLAYER::PLAYER_ID();

    // Frontend/loading guard: avoid running game-state logic before story is fully active.
    if (!PLAYER::IS_PLAYER_PLAYING(plr))
        return;

    // Hotkey: PageUp = reload INI
    if (GetAsyncKeyState(VK_PRIOR) & 1)
    {
        LoadSettings();
        Log("[CFG] Reloaded INI via PGUP.");
    }

    // Hotkey: PageDown = toggle draw method
    if (GetAsyncKeyState(VK_NEXT) & 1)
    {
        gDrawMethod = (gDrawMethod == 1) ? 2 : 1;
        Log("[HUD] DrawMethod toggled to %d.", gDrawMethod);
    }

    // Throttled detection
    bool inPoker = gCachedInPoker;
    if (now >= gNextDetectAt)
    {
        gNextDetectAt = now + gCfg.checkIntervalMs;
        inPoker = ComputeInPokerV2(now);
        gCachedInPoker = inPoker;
    }

    // State transition: enter poker
    if (inPoker && !gWasInPoker)
    {
        if (now >= gNextAllowedEnterMsg)
        {
            gNextAllowedEnterMsg = now + gCfg.enterCooldownMs;
            Log("[STATE] EnterPoker detected. Showing notification.");
            if (gCfg.hudUiMode == HUD_UI_MODE_LEGACY_TEXT)
                ShowLegacyHudMessage("~COLOR_GOLD~Mod Online", now, gCfg.msgDurationMs);
            else
                PostHudToast("Poker table joined", HUD_TOAST_EVENT_ENTER_POKER, now);
            ResetMoneyScan(now);
        }
    }

    // State transition: exit poker
    if (!inPoker && gWasInPoker)
    {
        Log("[STATE] ExitPoker detected.");
        if (gCfg.hudUiMode != HUD_UI_MODE_LEGACY_TEXT)
            PostHudToast("Poker table left", HUD_TOAST_EVENT_EXIT_POKER, now);
    }

    gWasInPoker = inPoker;

    // Draw legacy/fallback message
    if (now < gLegacyHudMessageUntil && !gLegacyHudMessage.empty())
    {
        DrawCenteredText(gLegacyHudMessage.c_str(), 0.5f, 0.02f);
    }

    // Debug overlay
    if (gCfg.debugOverlay)
    {
        char dbg[256];
        _snprintf_s(dbg, sizeof(dbg), "inPoker=%d score=%d gate=%s",
            inPoker ? 1 : 0, gLastDetectScore.total, gLastDetectScore.gateReason);
        DrawLeftText(dbg, 0.01f, 0.08f);
    }

    // OCR debug reason overlay
    if (gCfg.ocrDebugReasonOverlay)
    {
        char dbg[256];
        _snprintf_s(dbg, sizeof(dbg),
            "scan=%d pending=%d seen=%d hits=%d | score=%d gate=%s",
            gLastDetectInputs.scanOk ? 1 : 0,
            gLastDetectInputs.pending ? 1 : 0,
            gLastDetectInputs.seenKeyword ? 1 : 0,
            gLastDetectInputs.keywordHits,
            gLastDetectScore.total,
            gLastDetectScore.gateReason);
        DrawLeftText(dbg, 0.01f, 0.11f);
    }

    // Money tick
    MoneyTick(inPoker, now);
}

void HighStakesTick()
{
    // One-time init
    static bool inited = false;
    if (!inited)
    {
        inited = true;
        InitPaths();

        gLog = nullptr;
        fopen_s(&gLog, gLogPath, "a");
        Log("========== highstakes start (v0.5 OCR) ==========");

        LoadSettings();
        ResolveGetGlobalPtrOnce();
        gNextDetectAt = 0;
    }

    while (true)
    {
        WAIT(0);
        Tick();
    }
}

