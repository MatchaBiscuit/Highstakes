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
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <limits>
#include <utility>
#include <array>
#include <deque>
#include <unordered_set>
#include <cmath>

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
    int hudToastRetryMs = 4000;          // retry native toast path after failure
    std::string hudToastSoundSet = "";
    std::string hudToastSound = "";
    int hudPanelX = 80;                 // percent
    int hudPanelY = 94;                 // percent
    float hudPanelLineStep = 2.2f;      // percent
    int hudPanelMaxLines = 24;
    int hudPanelAnchorBottom = 1;       // 1=stack upward from bottom anchor

    // -------- OCR detection --------
    int ocrEnabled = 1;
    int ocrIntervalMs = 1000;
    int ocrProcessTimeoutMs = 2000;
    int ocrBottomLeftXPct = 0;
    int ocrBottomLeftYPct = 34;
    int ocrBottomLeftWPct = 34;
    int ocrBottomLeftHPct = 66;
    int ocrTopRightXPct = 72;
    int ocrTopRightYPct = 0;
    int ocrTopRightWPct = 28;
    int ocrTopRightHPct = 30;
    int ocrPsm = 11;
    int ocrDebugReasonOverlay = 0;
    int ocrLogEveryMs = 0;             // 0=disabled; otherwise logs OCR scan summaries
    int ocrDumpArtifacts = 0;
    int ocrPhaseStableMs = 1800;
    int ocrOutStableMs = 4200;
    float ocrPhaseConfThreshold = 0.62f;
    int ocrOpacityHintEnable = 1;
    int ocrOpacityRoiXPct = 72;
    int ocrOpacityRoiYPct = 66;
    int ocrOpacityRoiWPct = 27;
    int ocrOpacityRoiHPct = 30;
    float ocrOpacityLow = 8.0f;
    float ocrOpacityHigh = 28.0f;
    int ocrBlackoutGuardEnable = 1;    // 1=hold phase during short low-opacity fades
    float ocrBlackoutOpacityThreshold = 0.18f; // normalized opacity below this is treated as blackout/fade
    int ocrBlackoutAnchorGraceMs = 6000; // keep poker state if anchors were seen recently
    int ocrBlackoutOutExtraMs = 2500;  // extra OUT_OF_POKER stable time during fade
    int ocrBlackoutMaxHoldMs = 2500;   // max extra hold time before allowing OUT transition
    int ocrPayoutGuardEnable = 1;      // 1=hold in-poker during winner payout collection pauses
    int ocrPayoutMarkerGraceMs = 9000; // recent payout marker window to apply hold logic
    int ocrPayoutOutExtraMs = 5000;    // extra OUT stable time while payout marker grace is active
    std::string ocrPlayerNameHint = "arthur"; // lowercase token used to pick player row amount from OCR
    std::string ocrTesseractPath = "tesseract";
    std::string ocrKeywords = "poker,ante,call,fold,raise,check,bet,pot,blind,cards,community,turn";

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
    int moneyBetStepFilterEnable = 1;   // 1=prefer candidates whose deltas follow table bet increments
    int moneyBetStepDollars = 5;        // legal bet increment (Saint Denis high-stakes: 5)
    int moneyBetMinDollars = 10;        // smallest legal bet change (Saint Denis high-stakes: 10)
    int moneyExceptionLogCooldownMs = 30000; // SEH warning cooldown (0=log once per scan)
    int moneySkipFaultRuns = 1;         // 1=skip ahead after contiguous SEH faults
    int moneyOcrMatchToleranceCents = 6; // max abs delta to treat candidate as matching OCR amount
    int moneyNpcTrackMax = 5;           // max OCR-derived NPC amounts tracked per sample
    int moneyAutoLockPot = 1;           // 1=auto-lock pot global from OCR-correlated candidates
    int moneyAutoLockPotMinMatches = 10; // minimum OCR pot matches before auto-locking
    int moneyAutoLockPlayer = 1;        // 1=auto-lock player stack global from OCR-correlated candidates
    int moneyAutoLockPlayerMinMatches = 8; // minimum OCR player matches before auto-locking
    float moneyOverlayMultiplier = 2.0f; // multiplier shown in overlay
    int moneyPayoutEnable = 0;          // 1=auto payout bonus during payout phase
    float moneyPayoutMultiplier = 2.0f; // payout multiplier; bonus = src*(multiplier-1)
    int moneyPayoutUseWinsAmount = 1;   // prefer OCR "wins $X" as payout source
    int moneyPayoutFallbackToPot = 1;   // fallback to pot source if wins amount unavailable
    int moneyPayoutCooldownMs = 6000;   // minimum delay between payouts
    float moneyPayoutMinPhaseConf = 0.55f; // phase confidence threshold for payout

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
static char gGameDirPath[MAX_PATH]{ 0 };
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
    int lastDelta = 0;
    int changes = 0;
    int betStepMatches = 0;
    int betStepMismatches = 0;
    int ocrAnyMatches = 0;
    int ocrPotMatches = 0;
    int ocrPlayerMatches = 0;
    int ocrNpcMatches = 0;
    int lastOcrAnySampleId = -1;
    int lastOcrPotSampleId = -1;
    int lastOcrPlayerSampleId = -1;
    int lastOcrNpcSampleId = -1;
    DWORD firstSeenMs = 0;
    DWORD lastSeenMs = 0;
    DWORD lastChangeMs = 0;
    DWORD lastOcrMatchMs = 0;
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
static int   gAutoPotGlobal = -1;
static int   gAutoPlayerGlobal = -1;

struct OcrMoneySnapshot
{
    int sampleId = 0;
    DWORD sampleMs = 0;
    int potCents = -1;
    int mainPotCents = -1;
    int sidePotCents = -1;
    int genericPotCents = -1;
    int winsCents = -1;
    int playerCents = -1;
    std::vector<int> npcAmountsCents;
    int potSource = 0; // 0=none,1=main+side,2=main,3=side,4=genericPot,5=maxFallback
    std::vector<int> amountsCents;
};

static OcrMoneySnapshot gOcrMoney;

static bool IsOcrMoneyFresh(DWORD now, DWORD maxAgeMs = 10000)
{
    if (gOcrMoney.sampleMs == 0)
        return false;
    if (now < gOcrMoney.sampleMs)
        return false;
    return (now - gOcrMoney.sampleMs) <= maxAgeMs;
}

static void SortUniqueIntVector(std::vector<int>& vals)
{
    std::sort(vals.begin(), vals.end());
    vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
}

static bool AmountMatchesRefWithTol(int amountCents, int refCents, int tolCents)
{
    if (amountCents <= 0 || refCents <= 0)
        return false;
    int diff = amountCents - refCents;
    if (diff < 0)
        diff = -diff;
    return diff <= tolCents;
}

static bool ParseMoneyTokenCents(const std::string& token, int& outCents)
{
    if (token.empty())
        return false;

    bool hasSep = false;
    int sepPos = -1;
    for (size_t i = 0; i < token.size(); i++)
    {
        char c = token[i];
        if (c == '.' || c == ',')
        {
            hasSep = true;
            sepPos = (int)i;
            break;
        }
        if (c < '0' || c > '9')
            return false;
    }

    long long cents = 0;
    if (hasSep)
    {
        std::string left = token.substr(0, (size_t)sepPos);
        std::string right = token.substr((size_t)sepPos + 1);
        if (left.empty() || right.empty())
            return false;

        for (char c : left)
            if (c < '0' || c > '9')
                return false;
        for (char c : right)
            if (c < '0' || c > '9')
                return false;

        long long dollars = _atoi64(left.c_str());
        int frac = 0;
        if (right.size() == 1)
            frac = (right[0] - '0') * 10;
        else
            frac = (right[0] - '0') * 10 + (right[1] - '0');
        cents = dollars * 100 + frac;
    }
    else
    {
        for (char c : token)
            if (c < '0' || c > '9')
                return false;

        long long raw = _atoi64(token.c_str());
        long long asCents = raw;
        long long asDollars = raw * 100ll;
        long long hint = -1;
        if (gOcrMoney.winsCents > 0)
            hint = gOcrMoney.winsCents;
        else if (gOcrMoney.potCents > 0)
            hint = gOcrMoney.potCents;

        // OCR often drops decimal separators. Be conservative for 3+ digit tokens
        // to avoid catastrophic "$8.55" -> "$855.00" promotions.
        if (token.size() <= 2)
        {
            cents = asDollars;
        }
        else if (token.size() == 3)
        {
            // Default to cent-form (e.g. 855 -> $8.55). Only allow dollar-form
            // if a recent hint strongly supports it and remains close in magnitude.
            cents = asCents;
            if (hint > 0)
            {
                long long diffC = (asCents > hint) ? (asCents - hint) : (hint - asCents);
                long long diffD = (asDollars > hint) ? (asDollars - hint) : (hint - asDollars);
                if (diffD + 200 < diffC && asDollars <= (hint * 3ll + 10000ll))
                    cents = asDollars;
            }
        }
        else
        {
            // 4+ digits without separators are usually cent-formatted in OCR rows.
            cents = asCents;
            if (hint > 0 && token.size() == 4)
            {
                long long diffC = (asCents > hint) ? (asCents - hint) : (hint - asCents);
                long long diffD = (asDollars > hint) ? (asDollars - hint) : (hint - asDollars);
                if (diffD + 300 < diffC && asDollars <= (hint * 4ll + 20000ll))
                    cents = asDollars;
            }
        }
    }

    if (cents <= 0 || cents > 50000000ll)
        return false;

    outCents = (int)cents;
    return true;
}

static bool ParseAmountAfterDollar(const std::string& text, size_t dollarPos, int& outCents)
{
    if (dollarPos >= text.size() || text[dollarPos] != '$')
        return false;

    size_t i = dollarPos + 1;
    while (i < text.size() && text[i] == ' ')
        i++;

    std::string token;
    token.reserve(24);
    bool seenSep = false;
    bool seenDigit = false;
    int spaceRun = 0;

    auto mapOcrDigit = [](char c, char& mapped) -> bool
    {
        switch (c)
        {
        case 'o': case 'O': case 'q': case 'Q': case 'd': case 'D':
            mapped = '0'; return true;
        case 'i': case 'I': case 'l': case 'L': case '|': case '!':
            mapped = '1'; return true;
        case 'z': case 'Z':
            mapped = '2'; return true;
        case 's': case 'S':
            mapped = '5'; return true;
        case 'b': case 'B':
            mapped = '8'; return true;
        default:
            return false;
        }
    };

    while (i < text.size())
    {
        char c = text[i];
        if (c >= '0' && c <= '9')
        {
            token.push_back(c);
            seenDigit = true;
            spaceRun = 0;
            i++;
            if (token.size() >= 16)
                break;
            continue;
        }
        char mapped = 0;
        if (mapOcrDigit(c, mapped))
        {
            // Don't pull OCR-lookalike letters from the next word after a spacing break.
            if (seenDigit && spaceRun > 0)
                break;
            token.push_back(mapped);
            seenDigit = true;
            spaceRun = 0;
            i++;
            if (token.size() >= 16)
                break;
            continue;
        }
        if ((c == '.' || c == ',') && !seenSep)
        {
            if (spaceRun > 0)
                break;
            seenSep = true;
            token.push_back('.');
            spaceRun = 0;
            i++;
            continue;
        }
        if (c == ' ')
        {
            if (!seenDigit)
            {
                i++;
                continue;
            }
            // OCR may split one gap inside a token; more than one space ends token.
            spaceRun++;
            if (spaceRun <= 1)
            {
                i++;
                continue;
            }
            break;
        }
        break;
    }

    if (!seenDigit || token.empty())
        return false;
    while (!token.empty() && token.back() == '.')
        token.pop_back();
    if (token.empty())
        return false;

    return ParseMoneyTokenCents(token, outCents);
}

static int FindDollarAmountAfterToken(const std::string& text, const char* token, size_t lookaheadMax, bool chooseMax)
{
    if (!token || !*token)
        return -1;
    std::string needle = token;
    int best = -1;

    size_t pos = 0;
    while (true)
    {
        pos = text.find(needle, pos);
        if (pos == std::string::npos)
            break;

        size_t end = (std::min)(text.size(), pos + needle.size() + lookaheadMax);
        size_t dollar = text.find('$', pos + needle.size());
        if (dollar != std::string::npos && dollar < end)
        {
            int cents = 0;
            if (ParseAmountAfterDollar(text, dollar, cents))
            {
                if (!chooseMax)
                    return cents;
                if (cents > best)
                    best = cents;
            }
        }
        pos += needle.size();
    }

    return best;
}

static int FindDollarAmountBeforeToken(const std::string& text, const char* token, size_t lookbackMax, bool chooseMax)
{
    if (!token || !*token)
        return -1;
    std::string needle = token;
    int best = -1;

    size_t pos = 0;
    while (true)
    {
        pos = text.find(needle, pos);
        if (pos == std::string::npos)
            break;

        size_t begin = (pos > lookbackMax) ? (pos - lookbackMax) : 0;
        size_t dollar = text.rfind('$', pos);
        if (dollar != std::string::npos && dollar >= begin && dollar < pos)
        {
            int cents = 0;
            if (ParseAmountAfterDollar(text, dollar, cents))
            {
                if (!chooseMax)
                    return cents;
                if (cents > best)
                    best = cents;
            }
        }
        pos += needle.size();
    }

    return best;
}

static int FindDollarAmountNearToken(const std::string& text, const char* token, size_t lookaheadMax, size_t lookbackMax)
{
    int after = FindDollarAmountAfterToken(text, token, lookaheadMax, false);
    if (after > 0)
        return after;
    return FindDollarAmountBeforeToken(text, token, lookbackMax, false);
}

static bool WindowContainsToken(const std::string& text, size_t begin, size_t end, const char* token)
{
    if (!token || !*token || begin >= end || begin >= text.size())
        return false;
    size_t clampedEnd = (std::min)(end, text.size());
    size_t at = text.find(token, begin);
    return at != std::string::npos && at < clampedEnd;
}

static bool WindowHasCommaName(const std::string& text, size_t begin, size_t end)
{
    if (begin >= end || begin >= text.size())
        return false;
    size_t clampedEnd = (std::min)(end, text.size());
    size_t comma = text.find(',', begin);
    while (comma != std::string::npos && comma < clampedEnd)
    {
        size_t j = comma + 1;
        while (j < clampedEnd && text[j] == ' ')
            j++;
        int letters = 0;
        while (j < clampedEnd && text[j] >= 'a' && text[j] <= 'z')
        {
            letters++;
            j++;
        }
        if (letters >= 3)
            return true;
        comma = text.find(',', comma + 1);
    }
    return false;
}

static bool IsLikelyNpcAmountContext(const std::string& text, size_t dollarPos, const std::string& playerNameHint)
{
    if (dollarPos >= text.size() || text[dollarPos] != '$')
        return false;

    size_t begin = (dollarPos > 22) ? (dollarPos - 22) : 0;
    size_t end = (std::min)(text.size(), dollarPos + 42);

    // Reject action/pot/win contexts that are commonly misread as seat rows.
    const char* rejectTokens[] = {
        "pot", "main pot", "side pot", "wins", "winner", "collect",
        "blind", "called", "check", "checked", "bet", "raised", "raise", "fold", "turn"
    };
    for (const char* tok : rejectTokens)
    {
        if (WindowContainsToken(text, begin, end, tok))
            return false;
    }

    if (!playerNameHint.empty() && WindowContainsToken(text, begin, end, playerNameHint.c_str()))
        return false;
    if (WindowContainsToken(text, begin, end, "you"))
        return false;

    if (WindowContainsToken(text, begin, end, "oc,") ||
        WindowContainsToken(text, begin, end, "0c,") ||
        WindowContainsToken(text, begin, end, "qc,"))
    {
        return true;
    }

    return WindowHasCommaName(text, begin, end);
}

static void UpdateOcrMoneySnapshot(const std::string& rawText, DWORD now)
{
    gOcrMoney.sampleId++;
    gOcrMoney.sampleMs = now;
    gOcrMoney.amountsCents.clear();
    gOcrMoney.potCents = -1;
    gOcrMoney.mainPotCents = -1;
    gOcrMoney.sidePotCents = -1;
    gOcrMoney.genericPotCents = -1;
    gOcrMoney.winsCents = -1;
    gOcrMoney.playerCents = -1;
    gOcrMoney.npcAmountsCents.clear();
    gOcrMoney.potSource = 0;
    std::unordered_map<int, int> npcContextHits;

    for (size_t i = 0; i < rawText.size(); i++)
    {
        if (rawText[i] != '$')
            continue;
        int cents = 0;
        if (ParseAmountAfterDollar(rawText, i, cents))
        {
            gOcrMoney.amountsCents.push_back(cents);
            if (IsLikelyNpcAmountContext(rawText, i, gCfg.ocrPlayerNameHint))
                npcContextHits[cents]++;
        }
    }
    SortUniqueIntVector(gOcrMoney.amountsCents);

    gOcrMoney.mainPotCents = FindDollarAmountAfterToken(rawText, "main pot", 36, false);
    gOcrMoney.sidePotCents = FindDollarAmountAfterToken(rawText, "side pot", 36, false);
    gOcrMoney.genericPotCents = FindDollarAmountAfterToken(rawText, "pot", 28, true);
    gOcrMoney.winsCents = FindDollarAmountNearToken(rawText, "wins", 36, 18);
    if (gOcrMoney.winsCents <= 0)
        gOcrMoney.winsCents = FindDollarAmountNearToken(rawText, "won", 20, 10);
    if (gOcrMoney.winsCents <= 0)
        gOcrMoney.winsCents = FindDollarAmountNearToken(rawText, "collected", 32, 10);
    if (gOcrMoney.winsCents <= 0)
        gOcrMoney.winsCents = FindDollarAmountNearToken(rawText, "collect", 24, 10);
    if (gOcrMoney.winsCents <= 0)
        gOcrMoney.winsCents = FindDollarAmountNearToken(rawText, "winner", 30, 10);

    if (!gCfg.ocrPlayerNameHint.empty())
        gOcrMoney.playerCents = FindDollarAmountNearToken(rawText, gCfg.ocrPlayerNameHint.c_str(), 40, 28);
    if (gOcrMoney.playerCents <= 0)
        gOcrMoney.playerCents = FindDollarAmountNearToken(rawText, "you", 28, 20);

    if (gOcrMoney.mainPotCents > 0 && gOcrMoney.sidePotCents > 0)
    {
        gOcrMoney.potCents = gOcrMoney.mainPotCents + gOcrMoney.sidePotCents;
        gOcrMoney.potSource = 1;
    }
    else if (gOcrMoney.mainPotCents > 0)
    {
        gOcrMoney.potCents = gOcrMoney.mainPotCents;
        gOcrMoney.potSource = 2;
    }
    else if (gOcrMoney.sidePotCents > 0)
    {
        gOcrMoney.potCents = gOcrMoney.sidePotCents;
        gOcrMoney.potSource = 3;
    }
    else if (gOcrMoney.genericPotCents > 0)
    {
        gOcrMoney.potCents = gOcrMoney.genericPotCents;
        gOcrMoney.potSource = 4;
    }

    // Fallback heuristics when explicit token-linking fails.
    if (gOcrMoney.potCents <= 0 && !gOcrMoney.amountsCents.empty())
    {
        gOcrMoney.potCents = gOcrMoney.amountsCents.back();
        gOcrMoney.potSource = 5;
    }

    // Candidate NPC stack amounts are OCR dollars excluding known pot/player/wins references.
    {
        int tol = (std::max)(0, gCfg.moneyOcrMatchToleranceCents);
        int refs[6] = {
            gOcrMoney.potCents,
            gOcrMoney.mainPotCents,
            gOcrMoney.sidePotCents,
            gOcrMoney.genericPotCents,
            gOcrMoney.winsCents,
            gOcrMoney.playerCents
        };
        for (int amount : gOcrMoney.amountsCents)
        {
            if (amount <= 0)
                continue;
            if (amount < gCfg.moneyValueMin || amount > gCfg.moneyValueMax)
                continue;
            auto itCtx = npcContextHits.find(amount);
            if (itCtx == npcContextHits.end() || itCtx->second <= 0)
                continue;

            bool reserved = false;
            for (int ref : refs)
            {
                if (AmountMatchesRefWithTol(amount, ref, tol))
                {
                    reserved = true;
                    break;
                }
            }
            if (!reserved)
                gOcrMoney.npcAmountsCents.push_back(amount);
        }
        SortUniqueIntVector(gOcrMoney.npcAmountsCents);

        int keep = gCfg.moneyNpcTrackMax;
        if (keep > 0 && (int)gOcrMoney.npcAmountsCents.size() > keep)
        {
            gOcrMoney.npcAmountsCents.erase(
                gOcrMoney.npcAmountsCents.begin(),
                gOcrMoney.npcAmountsCents.end() - keep);
        }
    }
}

static bool CandidateMatchesObservedOcrAmount(int value, int amountCents)
{
    if (amountCents <= 0)
        return false;
    int tol = (std::max)(0, gCfg.moneyOcrMatchToleranceCents);

    // Primary: global appears to be cent-based (value and OCR amount are both cents).
    long long diffDirect = (long long)value - (long long)amountCents;
    if (diffDirect < 0)
        diffDirect = -diffDirect;
    if (diffDirect <= (long long)tol)
        return true;

    // Alternate: some globals may be dollar-based while OCR amount is cents.
    long long valueAsCents = (long long)value * 100ll;
    long long diffDollarGlobal = valueAsCents - (long long)amountCents;
    if (diffDollarGlobal < 0)
        diffDollarGlobal = -diffDollarGlobal;
    if (diffDollarGlobal <= (long long)tol)
        return true;

    return false;
}

static void UpdateCandidateOcrMatches(MoneyCandidate& c, int currentValue, DWORD now)
{
    if (gOcrMoney.sampleId <= 0 || gOcrMoney.amountsCents.empty())
        return;

    bool anyMatch = false;
    for (int amount : gOcrMoney.amountsCents)
    {
        if (CandidateMatchesObservedOcrAmount(currentValue, amount))
        {
            anyMatch = true;
            break;
        }
    }

    if (anyMatch && c.lastOcrAnySampleId != gOcrMoney.sampleId)
    {
        c.ocrAnyMatches++;
        c.lastOcrAnySampleId = gOcrMoney.sampleId;
        c.lastOcrMatchMs = now;
    }

    bool playerMatch = false;
    if (gOcrMoney.playerCents > 0 &&
        CandidateMatchesObservedOcrAmount(currentValue, gOcrMoney.playerCents))
    {
        playerMatch = true;
        if (c.lastOcrPlayerSampleId != gOcrMoney.sampleId)
        {
            c.ocrPlayerMatches++;
            c.lastOcrPlayerSampleId = gOcrMoney.sampleId;
            c.lastOcrMatchMs = now;
        }
    }

    bool potMatch = false;
    int potRefs[4] = {
        gOcrMoney.potCents,
        gOcrMoney.mainPotCents,
        gOcrMoney.sidePotCents,
        gOcrMoney.genericPotCents
    };
    for (int ref : potRefs)
    {
        if (ref <= 0)
            continue;
        // Prevent pot/player contamination when both are close in value.
        if (gOcrMoney.playerCents > 0 &&
            CandidateMatchesObservedOcrAmount(ref, gOcrMoney.playerCents) &&
            playerMatch)
        {
            continue;
        }
        if (CandidateMatchesObservedOcrAmount(currentValue, ref))
        {
            potMatch = true;
            break;
        }
    }

    if (potMatch && c.lastOcrPotSampleId != gOcrMoney.sampleId)
    {
        c.ocrPotMatches++;
        c.lastOcrPotSampleId = gOcrMoney.sampleId;
        c.lastOcrMatchMs = now;
    }

    bool npcMatch = false;
    for (int npc : gOcrMoney.npcAmountsCents)
    {
        if (CandidateMatchesObservedOcrAmount(currentValue, npc))
        {
            npcMatch = true;
            break;
        }
    }
    if (npcMatch && c.lastOcrNpcSampleId != gOcrMoney.sampleId)
    {
        c.ocrNpcMatches++;
        c.lastOcrNpcSampleId = gOcrMoney.sampleId;
        c.lastOcrMatchMs = now;
    }
}

static bool IsLikelyMoneyCandidate(const MoneyCandidate& c, DWORD now);

static bool MatchesBetGridUnits(int absDelta, int minUnit, int stepUnit)
{
    if (absDelta <= 0 || minUnit <= 0 || stepUnit <= 0)
        return false;
    if (absDelta < minUnit)
        return false;
    return (absDelta % stepUnit) == 0;
}

static bool MatchesConfiguredBetGridDelta(int absDelta)
{
    int stepDollars = gCfg.moneyBetStepDollars;
    int minDollars = gCfg.moneyBetMinDollars;
    if (stepDollars <= 0)
        return false;
    if (minDollars < stepDollars)
        minDollars = stepDollars;

    bool dollarsMatch = MatchesBetGridUnits(absDelta, minDollars, stepDollars);
    bool centsMatch = MatchesBetGridUnits(absDelta, minDollars * 100, stepDollars * 100);
    return dollarsMatch || centsMatch;
}

static float CandidateBetStepRatio(const MoneyCandidate& c)
{
    int total = c.betStepMatches + c.betStepMismatches;
    if (total <= 0)
        return -1.0f;
    return (float)c.betStepMatches / (float)total;
}

static float CandidateRankScore(const MoneyCandidate& c, DWORD now)
{
    float score = 0.0f;
    score += (float)c.ocrPotMatches * 18.0f;
    score += (float)c.ocrPlayerMatches * 3.0f;
    score += (float)c.ocrNpcMatches * 4.5f;
    score += (float)c.ocrAnyMatches * 1.2f;
    if (c.ocrPlayerMatches > c.ocrPotMatches * 2)
        score -= 6.0f;
    if (IsLikelyMoneyCandidate(c, now))
        score += 3.0f;
    score += (float)((std::min)(c.changes, 64)) * 0.08f;
    if (gCfg.moneyBetStepFilterEnable)
    {
        score += (float)((std::min)(c.betStepMatches, 48)) * 0.35f;
        score -= (float)((std::min)(c.betStepMismatches, 48)) * 0.28f;
        float ratio = CandidateBetStepRatio(c);
        if (ratio >= 0.0f)
            score += (ratio - 0.5f) * 8.0f;
    }
    if (c.lastOcrMatchMs > 0 && now > c.lastOcrMatchMs)
    {
        DWORD ageMs = now - c.lastOcrMatchMs;
        if (ageMs <= 12000)
            score += 2.0f;
    }
    return score;
}

static void ResetMoneyScan(DWORD now)
{
    gMoneyCands.clear();
    gAutoPotGlobal = -1;
    gAutoPlayerGlobal = -1;
    gOcrMoney = OcrMoneySnapshot{};
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

    if (gCfg.moneyBetStepFilterEnable)
    {
        int totalBetDeltas = c.betStepMatches + c.betStepMismatches;
        if (totalBetDeltas >= 5 && c.betStepMatches * 2 < totalBetDeltas)
            return false;
    }

    return true;
}

static bool BuildSortedCandidates(DWORD now, std::vector<const MoneyCandidate*>& sorted)
{
    sorted.clear();
    sorted.reserve(gMoneyCands.size());

    bool hasOcrCorrelated = false;
    for (auto& kv : gMoneyCands)
    {
        bool ocrCorrelated = (kv.second.ocrAnyMatches > 0 || kv.second.ocrPotMatches > 0 || kv.second.ocrPlayerMatches > 0 || kv.second.ocrNpcMatches > 0);
        if (ocrCorrelated)
            hasOcrCorrelated = true;
        if (ocrCorrelated || IsLikelyMoneyCandidate(kv.second, now))
            sorted.push_back(&kv.second);
    }

    bool usingLikely = !sorted.empty();
    if (!usingLikely)
    {
        for (auto& kv : gMoneyCands)
            sorted.push_back(&kv.second);
    }

    std::sort(sorted.begin(), sorted.end(), [now](const MoneyCandidate* a, const MoneyCandidate* b) {
        float sa = CandidateRankScore(*a, now);
        float sb = CandidateRankScore(*b, now);
        if (sa != sb) return sa > sb;
        if (a->ocrPotMatches != b->ocrPotMatches) return a->ocrPotMatches > b->ocrPotMatches;
        if (a->ocrPlayerMatches != b->ocrPlayerMatches) return a->ocrPlayerMatches > b->ocrPlayerMatches;
        if (a->ocrNpcMatches != b->ocrNpcMatches) return a->ocrNpcMatches > b->ocrNpcMatches;
        if (a->ocrAnyMatches != b->ocrAnyMatches) return a->ocrAnyMatches > b->ocrAnyMatches;
        if (a->changes != b->changes) return a->changes > b->changes;
        return a->idx < b->idx;
    });

    return hasOcrCorrelated || usingLikely;
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
static DWORD gHudToastNativeRetryAt = 0;
static int gHudToastNativeFailCount = 0;
static std::string gLegacyHudMessage = "~COLOR_GOLD~Mod Online";
static DWORD gLegacyHudMessageUntil = 0;
static int gOcrStartFailureStreak = 0;
static bool gOcrStartFailureWarned = false;
enum OcrStartFailReason
{
    OCR_START_FAIL_NONE = 0,
    OCR_START_FAIL_NO_FOREGROUND = 1,
    OCR_START_FAIL_CAPTURE = 2,
    OCR_START_FAIL_CREATE_PROCESS = 3
};
static OcrStartFailReason gLastOcrStartFailReason = OCR_START_FAIL_NONE;
static DWORD gLastOcrStartWinErr = 0;
static Hash gHudToastIconHash = 0;
static Hash gHudToastColorHash = 0;

// SET_TEXT_SCALE:        0x4170B650590B3B00
// SET_TEXT_CENTRE:       0xBE5261939FBECB8C
// _SET_TEXT_COLOR:       0x50A41AD966910F03
// _BG_DISPLAY_TEXT:      0x16794E044C9EFB58
// _DISPLAY_TEXT:         0xD79334A4BB99BAD1 (candidate; may differ per native DB)
static void DrawTextBasic(const char* msg, float x, float y, bool center, float scale = 0.6f)
{
    const char* str = MISC::VAR_STRING(10, "LITERAL_STRING", msg);

    float clampedScale = scale;
    if (clampedScale < 0.25f) clampedScale = 0.25f;
    if (clampedScale > 0.85f) clampedScale = 0.85f;
    invoke<Void>(0x4170B650590B3B00, clampedScale, clampedScale); // SET_TEXT_SCALE
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
    DrawTextBasic(msg, x, y, true, 0.6f);
}

static void DrawLeftText(const char* msg, float x, float y)
{
    DrawTextBasic(msg, x, y, false, 0.6f);
}

static float ClampFloat(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

struct HudPanelCursor
{
    float x = 0.72f;
    float y = 0.35f;
    float step = 0.022f;
    float dir = 1.0f;
    float textScale = 0.45f;
    int maxChars = 48;
    int lines = 0;
    int maxLines = 18;
    bool clipped = false;
};

static HudPanelCursor MakeHudPanelCursor(float yOffsetNorm = 0.0f)
{
    HudPanelCursor c;
    c.x = ClampFloat((float)gCfg.hudPanelX / 100.0f, 0.01f, 0.94f);
    c.y = ClampFloat((float)gCfg.hudPanelY / 100.0f + yOffsetNorm, 0.02f, 0.98f);
    c.step = ClampFloat(gCfg.hudPanelLineStep / 100.0f, 0.012f, 0.06f);
    c.maxLines = gCfg.hudPanelMaxLines;
    if (c.maxLines < 1) c.maxLines = 1;
    if (c.maxLines > 128) c.maxLines = 128;
    c.textScale = ClampFloat(0.42f + (c.step - 0.018f) * 6.5f, 0.34f, 0.56f);

    // Keep the panel fully visible even with large line counts.
    if (gCfg.hudPanelAnchorBottom)
    {
        c.dir = -1.0f;
        float minStartY = 0.02f + c.step * (float)((std::max)(0, c.maxLines - 1));
        if (c.y < minStartY)
            c.y = (std::min)(0.98f, minStartY);
    }
    else
    {
        c.dir = 1.0f;
        float maxStartY = 0.98f - c.step * (float)((std::max)(0, c.maxLines - 1));
        if (c.y > maxStartY)
            c.y = (std::max)(0.02f, maxStartY);
    }

    // Approximate character budget from remaining screen width at current scale.
    float usableWidth = 0.99f - c.x;
    int charBudget = (int)std::llround(usableWidth / (0.0095f * (c.textScale / 0.45f)));
    c.maxChars = (std::max)(20, (std::min)(charBudget, 64));
    return c;
}

static bool DrawPanelLine(HudPanelCursor& c, const char* msg)
{
    if (c.lines >= c.maxLines)
    {
        c.clipped = true;
        return false;
    }

    std::string line = msg ? msg : "";
    if ((int)line.size() > c.maxChars)
    {
        int keep = (std::max)(0, c.maxChars - 3);
        line = line.substr(0, (size_t)keep);
        line += "...";
    }

    DrawTextBasic(line.c_str(), c.x, c.y, false, c.textScale);
    c.y += c.step * c.dir;
    c.lines++;
    return true;
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
    bool attemptedToast = HudUsesToastPath();
    bool posted = false;
    if (gCfg.hudUiMode == HUD_UI_MODE_LEGACY_TEXT)
    {
        Log("[HUD] Toast skipped (legacy mode): event=%d title='%s'", (int)eventKind, title ? title : "");
    }
    else if (!gCfg.hudToastEnabled)
    {
        Log("[HUD] Toast skipped (ToastEnabled=0): event=%d title='%s'", (int)eventKind, title ? title : "");
    }

    if (attemptedToast)
    {
        if (gHudToastNativeFailed)
        {
            if (now < gHudToastNativeRetryAt)
            {
                DWORD retryIn = gHudToastNativeRetryAt - now;
                Log("[HUD] Toast native cooldown active: retryIn=%lums failCount=%d event=%d title='%s'",
                    (unsigned long)retryIn, gHudToastNativeFailCount, (int)eventKind, title ? title : "");
            }
            else
            {
                gHudToastNativeFailed = false;
                Log("[HUD] Toast native retrying after cooldown: failCount=%d event=%d title='%s'",
                    gHudToastNativeFailCount, (int)eventKind, title ? title : "");
            }
        }
    }

    if (attemptedToast && !gHudToastNativeFailed)
    {
        const char* iconDict = gCfg.hudToastIconDict.empty()
            ? "ITEMTYPE_TEXTURES"
            : gCfg.hudToastIconDict.c_str();
        const char* soundSet = gCfg.hudToastSoundSet.empty() ? nullptr : gCfg.hudToastSoundSet.c_str();
        const char* soundToPlay = gCfg.hudToastSound.empty() ? nullptr : gCfg.hudToastSound.c_str();

        __try
        {
            const char* toastTitle = MISC::VAR_STRING(10, "LITERAL_STRING", title ? title : "");
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
            if (gHudToastNativeFailCount > 0)
            {
                Log("[HUD] Toast native path recovered after %d failures.", gHudToastNativeFailCount);
            }
            gHudToastNativeFailCount = 0;
            gHudToastNativeWarned = false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            gHudToastNativeFailed = true;
            gHudToastNativeFailCount++;
            int retryMs = (gCfg.hudToastRetryMs < 250) ? 250 : gCfg.hudToastRetryMs;
            gHudToastNativeRetryAt = now + (DWORD)retryMs;
            DWORD exCode = GetExceptionCode();
            if (!gHudToastNativeWarned || (gHudToastNativeFailCount % 5) == 0)
            {
                gHudToastNativeWarned = true;
                Log("[HUD] WARNING: Toast native path failed (SEH=0x%08lX failCount=%d retryMs=%d event=%d title='%s'). Falling back to legacy text.",
                    (unsigned long)exCode, gHudToastNativeFailCount, retryMs, (int)eventKind, title ? title : "");
            }
        }
    }

    if (!posted && attemptedToast && gCfg.hudToastFallbackText && gCfg.hudUiMode != HUD_UI_MODE_LEGACY_TEXT)
    {
        Log("[HUD] Toast fallback text used: nativeFailed=%d failCount=%d event=%d title='%s'",
            gHudToastNativeFailed ? 1 : 0, gHudToastNativeFailCount, (int)eventKind, title ? title : "");
        ShowLegacyHudMessage(title ? title : "", now, gCfg.msgDurationMs);
    }
}

// ---------------- Detection ----------------
enum PokerPhase
{
    POKER_PHASE_OUT_OF_POKER = 0,
    POKER_PHASE_TABLE_IDLE = 1,
    POKER_PHASE_PLAYER_DECISION = 2,
    POKER_PHASE_WAITING_ACTION = 3,
    POKER_PHASE_SHOWDOWN_REVEAL = 4,
    POKER_PHASE_PAYOUT_SETTLEMENT = 5,
    POKER_PHASE_COUNT = 6
};

static const char* PokerPhaseToString(PokerPhase p)
{
    switch (p)
    {
    case POKER_PHASE_OUT_OF_POKER: return "OUT_OF_POKER";
    case POKER_PHASE_TABLE_IDLE: return "TABLE_IDLE";
    case POKER_PHASE_PLAYER_DECISION: return "PLAYER_DECISION";
    case POKER_PHASE_WAITING_ACTION: return "WAITING_ACTION";
    case POKER_PHASE_SHOWDOWN_REVEAL: return "SHOWDOWN_REVEAL";
    case POKER_PHASE_PAYOUT_SETTLEMENT: return "PAYOUT_SETTLEMENT";
    default: return "UNKNOWN";
    }
}

struct DetectionInputs
{
    bool scanOk = false;
    bool seenKeyword = false;
    int keywordHits = 0;
    int anchorHits = 0;
    bool pending = false;
    float opacityHint = 0.5f;
    std::string rawText;
    std::string normalizedText;
};

struct DetectionScore
{
    int total = 0;
    bool gateFail = false;
    const char* gateReason = "ok";
    PokerPhase guessPhase = POKER_PHASE_OUT_OF_POKER;
    float confidence = 0.0f;
    float opacityHint = 0.5f;
    bool pokerAnchor = false;
    DWORD candidateStableMs = 0;
    std::array<float, POKER_PHASE_COUNT> phaseScores{};
    std::string reasons;
};

struct DetectionRuntime
{
    bool inPoker = false;
    PokerPhase phase = POKER_PHASE_OUT_OF_POKER;
    PokerPhase candidatePhase = POKER_PHASE_OUT_OF_POKER;
    DWORD candidateSince = 0;
    float phaseConfidence = 0.0f;
    std::deque<std::array<float, POKER_PHASE_COUNT>> scoreHistory;
};

static DetectionInputs  gLastDetectInputs;
static DetectionScore   gLastDetectScore;
static DetectionRuntime gDetectRuntime;
static std::vector<std::string> gOcrKeywords;
static std::string gLastOcrText;
static char gOcrBmpBottomLeftPath[MAX_PATH]{ 0 };
static char gOcrBmpTopRightPath[MAX_PATH]{ 0 };
static char gOcrOutBaseBottomLeftPath[MAX_PATH]{ 0 };
static char gOcrOutBaseTopRightPath[MAX_PATH]{ 0 };
static char gOcrTxtBottomLeftPath[MAX_PATH]{ 0 };
static char gOcrTxtTopRightPath[MAX_PATH]{ 0 };
static HANDLE gOcrProcess = nullptr;
static DWORD gOcrProcessStartMs = 0;
static DWORD gNextOcrStartAt = 0;
static DWORD gNextOcrLogAt = 0;
static float gPendingOpacityHint = 0.5f;
static float gLastOpacityHint = 0.5f;
static DWORD gLastPokerAnchorSeenAt = 0;
static DWORD gLastPayoutMarkerSeenAt = 0;
static DWORD gPayoutHoldUntilAt = 0;

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

static std::string NormalizeOcrToken(const std::string& token)
{
    if (token == "comunity" || token == "communiry" || token == "communi" || token == "ommunity")
        return "community";
    if (token == "caros" || token == "cars" || token == "carns" || token == "car" || token == "card")
        return "cards";
    if (token == "calied" || token == "cailed")
        return "called";
    if (token == "fould" || token == "foid")
        return "fold";
    if (token == "checl" || token == "chec")
        return "check";
    if (token == "raisedd")
        return "raised";
    return token;
}

static std::string NormalizeOcrText(const std::string& text, std::unordered_map<std::string, int>& tokenCounts)
{
    tokenCounts.clear();
    std::string flat;
    flat.reserve(text.size());

    for (char c : text)
    {
        unsigned char uc = (unsigned char)c;
        if (uc >= 'A' && uc <= 'Z')
            c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '$')
            flat.push_back(c);
        else
            flat.push_back(' ');
    }

    std::string out;
    std::string tok;
    for (size_t i = 0; i <= flat.size(); i++)
    {
        char c = (i < flat.size()) ? flat[i] : ' ';
        if (c != ' ')
        {
            tok.push_back(c);
            continue;
        }

        if (!tok.empty())
        {
            std::string norm = NormalizeOcrToken(tok);
            if (norm.size() >= 2)
            {
                tokenCounts[norm]++;
                if (!out.empty())
                    out.push_back(' ');
                out += norm;
            }
            tok.clear();
        }
    }
    return out;
}

static bool HasToken(const std::unordered_map<std::string, int>& tokenCounts, const char* token)
{
    return tokenCounts.find(token) != tokenCounts.end();
}

static std::string BuildReasonSummary(std::vector<std::pair<float, std::string>>& reasons, int topN = 4)
{
    if (reasons.empty())
        return "-";

    std::sort(reasons.begin(), reasons.end(),
        [](const std::pair<float, std::string>& a, const std::pair<float, std::string>& b) {
            return a.first > b.first;
        });

    std::string out;
    std::unordered_set<std::string> seen;
    int used = 0;
    for (const auto& r : reasons)
    {
        if (seen.count(r.second))
            continue;
        seen.insert(r.second);
        if (!out.empty())
            out += ",";
        out += r.second;
        used++;
        if (used >= topN)
            break;
    }
    if (out.empty())
        out = "-";
    return out;
}

static bool ComputeRegionLumaStdDev(HWND hwnd, int xPctIn, int yPctIn, int wPctIn, int hPctIn, float& outStdDev)
{
    outStdDev = 0.0f;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc))
        return false;

    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0)
        return false;

    int xPct = ClampInt(xPctIn, 0, 100);
    int yPct = ClampInt(yPctIn, 0, 100);
    int wPct = ClampInt(wPctIn, 1, 100);
    int hPct = ClampInt(hPctIn, 1, 100);

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

    bool ok = false;
    if (bltOk)
    {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 24;
        bi.bmiHeader.biCompression = BI_RGB;

        int stride = ((w * 3 + 3) & ~3);
        int dataSize = stride * h;
        std::vector<unsigned char> pixels((size_t)dataSize);
        if (GetDIBits(memdc, bmp, 0, (UINT)h, pixels.data(), &bi, DIB_RGB_COLORS))
        {
            double sum = 0.0;
            double sumSq = 0.0;
            int count = 0;
            for (int yy = 0; yy < h; yy++)
            {
                const unsigned char* row = pixels.data() + yy * stride;
                for (int xx = 0; xx < w; xx++)
                {
                    unsigned char b = row[xx * 3 + 0];
                    unsigned char g = row[xx * 3 + 1];
                    unsigned char r = row[xx * 3 + 2];
                    double luma = 0.114 * b + 0.587 * g + 0.299 * r;
                    sum += luma;
                    sumSq += luma * luma;
                    count++;
                }
            }
            if (count > 0)
            {
                double mean = sum / count;
                double var = (sumSq / count) - (mean * mean);
                if (var < 0.0) var = 0.0;
                outStdDev = (float)std::sqrt(var);
                ok = true;
            }
        }
    }

    DeleteObject(bmp);
    DeleteDC(memdc);
    ReleaseDC(nullptr, screen);
    return ok;
}

static float ComputeOpacityHint(HWND hwnd)
{
    if (!gCfg.ocrOpacityHintEnable)
        return 0.5f;

    float stddev = 0.0f;
    if (!ComputeRegionLumaStdDev(
        hwnd,
        gCfg.ocrOpacityRoiXPct, gCfg.ocrOpacityRoiYPct,
        gCfg.ocrOpacityRoiWPct, gCfg.ocrOpacityRoiHPct,
        stddev))
    {
        return 0.5f;
    }

    float lo = gCfg.ocrOpacityLow;
    float hi = gCfg.ocrOpacityHigh;
    if (hi <= lo + 0.1f)
        hi = lo + 0.1f;
    float norm = (stddev - lo) / (hi - lo);
    return ClampFloat(norm, 0.0f, 1.0f);
}

static void CleanupOcrArtifactsIfNeeded()
{
    if (gCfg.ocrDumpArtifacts)
        return;

    DeleteFileA(gOcrBmpBottomLeftPath);
    DeleteFileA(gOcrBmpTopRightPath);
    DeleteFileA(gOcrTxtBottomLeftPath);
    DeleteFileA(gOcrTxtTopRightPath);
}

static const char* OcrStartFailReasonToString(OcrStartFailReason reason)
{
    switch (reason)
    {
    case OCR_START_FAIL_NO_FOREGROUND:
        return "noForeground";
    case OCR_START_FAIL_CAPTURE:
        return "capture";
    case OCR_START_FAIL_CREATE_PROCESS:
        return "createProcess";
    default:
        return "none";
    }
}

static std::string OcrTextLogSnippet(const std::string& text, size_t maxChars)
{
    std::string out;
    out.reserve((std::min)(maxChars, text.size()));

    bool prevSpace = false;
    for (char c : text)
    {
        unsigned char uc = (unsigned char)c;
        bool isWhitespace = (uc <= ' ');
        if (isWhitespace)
        {
            if (!out.empty() && !prevSpace)
            {
                out.push_back(' ');
                prevSpace = true;
            }
            continue;
        }

        if (uc < 32 || uc > 126)
            c = '?';

        out.push_back(c);
        prevSpace = false;
        if (out.size() >= maxChars)
            break;
    }

    return TrimAscii(out);
}

static const char* OcrPotSourceToString(int src)
{
    switch (src)
    {
    case 1: return "main+side";
    case 2: return "main";
    case 3: return "side";
    case 4: return "pot";
    case 5: return "fallback";
    default: return "none";
    }
}

static std::string OcrAmountListSnippet(const std::vector<int>& amounts, int maxItems = 6)
{
    if (amounts.empty())
        return "-";
    int n = (std::min)((int)amounts.size(), maxItems);
    std::string out;
    char buf[32];
    for (int i = 0; i < n; i++)
    {
        if (!out.empty())
            out += ",";
        _snprintf_s(buf, sizeof(buf), "$%.2f", (double)amounts[i] / 100.0);
        out += buf;
    }
    if ((int)amounts.size() > n)
        out += ",...";
    return out;
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

static bool CaptureOcrRegionToBmp(HWND hwnd, int xPctIn, int yPctIn, int wPctIn, int hPctIn, const char* outPath)
{
    RECT rc{};
    if (!GetClientRect(hwnd, &rc))
        return false;

    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0)
        return false;

    int xPct = ClampInt(xPctIn, 0, 100);
    int yPct = ClampInt(yPctIn, 0, 100);
    int wPct = ClampInt(wPctIn, 1, 100);
    int hPct = ClampInt(hPctIn, 1, 100);

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
        writeOk = SaveBitmap24(outPath, bmp, memdc, w, h);

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

static bool FileExistsPath(const char* path)
{
    if (!path || !*path)
        return false;
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static std::string BuildGamePath(const char* relPath)
{
    if (!relPath || !*relPath)
        return std::string();
    std::string out = gGameDirPath;
    if (!out.empty() && out.back() != '\\' && out.back() != '/')
        out.push_back('\\');
    out += relPath;
    return out;
}

static std::string ResolveOcrExecutablePath(bool& outUsingPortable)
{
    outUsingPortable = false;

    std::string configured = TrimAscii(gCfg.ocrTesseractPath);
    if (configured.empty())
        configured = "tesseract";

    if (FileExistsPath(configured.c_str()))
        return configured;

    // If config points to a relative file path, resolve from game directory.
    bool hasPathSep = configured.find('\\') != std::string::npos || configured.find('/') != std::string::npos;
    if (hasPathSep && gGameDirPath[0] != '\0')
    {
        std::string rel = configured;
        while (!rel.empty() && (rel[0] == '\\' || rel[0] == '/'))
            rel.erase(rel.begin());
        std::string fromGame = BuildGamePath(rel.c_str());
        if (FileExistsPath(fromGame.c_str()))
            return fromGame;
    }

    // Portable OCR fallback locations in game root.
    const char* portableCandidates[] = {
        "highstakes_ocr\\tesseract.exe",
        "ocr\\tesseract.exe",
        "tesseract.exe"
    };
    for (const char* rel : portableCandidates)
    {
        std::string candidate = BuildGamePath(rel);
        if (FileExistsPath(candidate.c_str()))
        {
            outUsingPortable = true;
            return candidate;
        }
    }

    return configured;
}

static bool StartOcrProcess(DWORD now)
{
    if (gOcrProcess)
        return false;

    gLastOcrStartFailReason = OCR_START_FAIL_NONE;
    gLastOcrStartWinErr = 0;

    HWND hwnd = nullptr;
    if (!GetGameForegroundWindow(hwnd))
    {
        gLastOcrStartFailReason = OCR_START_FAIL_NO_FOREGROUND;
        return false;
    }
    gPendingOpacityHint = ComputeOpacityHint(hwnd);

    DeleteFileA(gOcrTxtBottomLeftPath);
    DeleteFileA(gOcrTxtTopRightPath);

    if (!CaptureOcrRegionToBmp(
        hwnd,
        gCfg.ocrBottomLeftXPct, gCfg.ocrBottomLeftYPct,
        gCfg.ocrBottomLeftWPct, gCfg.ocrBottomLeftHPct,
        gOcrBmpBottomLeftPath))
    {
        gLastOcrStartFailReason = OCR_START_FAIL_CAPTURE;
        gLastOcrStartWinErr = GetLastError();
        CleanupOcrArtifactsIfNeeded();
        return false;
    }

    if (!CaptureOcrRegionToBmp(
        hwnd,
        gCfg.ocrTopRightXPct, gCfg.ocrTopRightYPct,
        gCfg.ocrTopRightWPct, gCfg.ocrTopRightHPct,
        gOcrBmpTopRightPath))
    {
        gLastOcrStartFailReason = OCR_START_FAIL_CAPTURE;
        gLastOcrStartWinErr = GetLastError();
        CleanupOcrArtifactsIfNeeded();
        return false;
    }

    bool usingPortableOcr = false;
    std::string ocrExePath = ResolveOcrExecutablePath(usingPortableOcr);
    if (usingPortableOcr)
    {
        static bool warnedPortable = false;
        if (!warnedPortable)
        {
            warnedPortable = true;
            Log("[OCR] Using portable OCR runtime: '%s'", ocrExePath.c_str());
        }
    }

    std::string cmd = "cmd /C \"\"";
    cmd += ocrExePath;
    cmd += "\" \"";
    cmd += gOcrBmpBottomLeftPath;
    cmd += "\" \"";
    cmd += gOcrOutBaseBottomLeftPath;
    cmd += "\" --psm ";
    cmd += std::to_string(gCfg.ocrPsm);
    cmd += " -l eng quiet && \"";
    cmd += ocrExePath;
    cmd += "\" \"";
    cmd += gOcrBmpTopRightPath;
    cmd += "\" \"";
    cmd += gOcrOutBaseTopRightPath;
    cmd += "\" --psm ";
    cmd += std::to_string(gCfg.ocrPsm);
    cmd += " -l eng quiet\"";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const char* workDir = (gGameDirPath[0] != '\0') ? gGameDirPath : nullptr;

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
        workDir,
        &si,
        &pi);

    if (!ok)
    {
        gLastOcrStartFailReason = OCR_START_FAIL_CREATE_PROCESS;
        gLastOcrStartWinErr = GetLastError();
        Log("[OCR] CreateProcess failed for OCR runtime='%s' cmd='%s' err=%lu",
            ocrExePath.c_str(), cmd.c_str(), (unsigned long)gLastOcrStartWinErr);
        CleanupOcrArtifactsIfNeeded();
        return false;
    }

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
            CleanupOcrArtifactsIfNeeded();
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
        std::string leftText;
        std::string rightText;
        bool leftOk = ReadTextFileAll(gOcrTxtBottomLeftPath, leftText);
        bool rightOk = ReadTextFileAll(gOcrTxtTopRightPath, rightText);
        if (!leftOk && !rightOk)
        {
            hasResult = true;
            out.scanOk = false;
            gLastOcrText.clear();
            CleanupOcrArtifactsIfNeeded();
            return true;
        }

        std::string text;
        if (leftOk)
            text += leftText;
        if (rightOk)
        {
            if (!text.empty())
                text += "\n";
            text += rightText;
        }

        text = ToLowerAscii(text);
        gLastOcrText = text;
        out.rawText = text;
        out.opacityHint = gPendingOpacityHint;
        gLastOpacityHint = gPendingOpacityHint;
        std::unordered_map<std::string, int> tokenCounts;
        out.normalizedText = NormalizeOcrText(text, tokenCounts);
        out.scanOk = true;
        for (const auto& kw : gOcrKeywords)
        {
            if (!kw.empty() && text.find(kw) != std::string::npos)
                out.keywordHits++;
        }
        const char* anchors[] = {
            "blind","cards","community","pot","call","fold","raise","bet",
            "check","turn","pair","straight","flush","wins","amount",
            "called","raised","folded","checked","skip","auto"
        };
        for (const char* anchor : anchors)
            if (HasToken(tokenCounts, anchor))
                out.anchorHits++;
        out.seenKeyword = (out.keywordHits > 0);
        CleanupOcrArtifactsIfNeeded();
        hasResult = true;
        return true;
    }

    // WAIT_FAILED / WAIT_ABANDONED: treat as OCR failure.
    StopOcrProcess(true);
    hasResult = true;
    out.scanOk = false;
    CleanupOcrArtifactsIfNeeded();
    return true;
}

static DetectionScore ComputeDetectionScore(const DetectionInputs& in)
{
    DetectionScore out;
    out.opacityHint = in.opacityHint;

    if (!in.scanOk)
    {
        out.gateFail = true;
        out.gateReason = "ocrFail";
        return out;
    }

    out.total = in.keywordHits;
    out.gateReason = in.seenKeyword ? "ocrHit" : "ocrMiss";

    std::unordered_map<std::string, int> tokens;
    std::string normalized = in.normalizedText;
    if (normalized.empty())
        normalized = NormalizeOcrText(in.rawText, tokens);
    else
        (void)NormalizeOcrText(normalized, tokens);

    std::string padded = " " + normalized + " ";
    auto hasToken = [&](const char* t) { return HasToken(tokens, t); };
    auto hasPhrase = [&](const char* p) {
        std::string needle = " ";
        needle += p;
        needle += " ";
        return padded.find(needle) != std::string::npos;
    };

    std::vector<std::pair<float, std::string>> reasons;
    auto add = [&](PokerPhase p, float w, const char* why) {
        out.phaseScores[(size_t)p] += w;
        reasons.emplace_back(w, why);
    };

    // Table idle / seated markers.
    if (hasPhrase("small blind")) add(POKER_PHASE_TABLE_IDLE, 2.2f, "small blind");
    if (hasPhrase("big blind")) add(POKER_PHASE_TABLE_IDLE, 2.2f, "big blind");
    if (hasToken("blind")) add(POKER_PHASE_TABLE_IDLE, 0.8f, "blind");
    if (hasToken("pot")) add(POKER_PHASE_TABLE_IDLE, 1.2f, "pot");

    // Active decision markers.
    if (hasPhrase("your cards")) add(POKER_PHASE_PLAYER_DECISION, 2.4f, "your cards");
    if (hasPhrase("take your turn")) add(POKER_PHASE_PLAYER_DECISION, 2.6f, "take your turn");
    if (hasToken("call") || hasToken("called")) add(POKER_PHASE_PLAYER_DECISION, 1.1f, "call");
    if (hasToken("fold") || hasToken("folded")) add(POKER_PHASE_PLAYER_DECISION, 1.1f, "fold");
    if (hasToken("check") || hasToken("checked")) add(POKER_PHASE_PLAYER_DECISION, 1.1f, "check");
    if (hasToken("raise") || hasToken("raised")) add(POKER_PHASE_PLAYER_DECISION, 1.1f, "raise");
    if (hasToken("bet")) add(POKER_PHASE_PLAYER_DECISION, 1.1f, "bet");
    if (hasToken("amount")) add(POKER_PHASE_PLAYER_DECISION, 0.9f, "amount");

    // Waiting/auto-action markers.
    if (hasToken("skip")) add(POKER_PHASE_WAITING_ACTION, 2.0f, "skip");
    if (hasPhrase("auto bet")) add(POKER_PHASE_WAITING_ACTION, 2.2f, "auto bet");
    if (hasToken("leave")) add(POKER_PHASE_WAITING_ACTION, 0.7f, "leave");
    if (hasToken("waiting")) add(POKER_PHASE_WAITING_ACTION, 1.0f, "waiting");

    // Reveal markers.
    if (hasToken("pair")) add(POKER_PHASE_SHOWDOWN_REVEAL, 1.6f, "pair");
    if (hasToken("straight")) add(POKER_PHASE_SHOWDOWN_REVEAL, 1.8f, "straight");
    if (hasToken("flush")) add(POKER_PHASE_SHOWDOWN_REVEAL, 1.8f, "flush");
    if (hasToken("muck")) add(POKER_PHASE_SHOWDOWN_REVEAL, 1.6f, "muck");
    if (hasToken("reveal")) add(POKER_PHASE_SHOWDOWN_REVEAL, 1.4f, "reveal");
    if (hasPhrase("waiting to reveal")) add(POKER_PHASE_SHOWDOWN_REVEAL, 2.2f, "waiting reveal");
    if (hasPhrase("community cards")) add(POKER_PHASE_SHOWDOWN_REVEAL, 1.2f, "community cards");

    // Payout markers.
    if (in.rawText.find("wins $") != std::string::npos) add(POKER_PHASE_PAYOUT_SETTLEMENT, 3.0f, "wins $");
    if (hasToken("wins")) add(POKER_PHASE_PAYOUT_SETTLEMENT, 1.8f, "wins");

    // Opacity hint weighting (secondary signal only).
    if (gCfg.ocrOpacityHintEnable)
    {
        if (in.opacityHint >= 0.70f)
        {
            add(POKER_PHASE_PLAYER_DECISION, 0.9f, "opacity:active");
            add(POKER_PHASE_TABLE_IDLE, 0.3f, "opacity:active");
        }
        else if (in.opacityHint <= 0.30f)
        {
            add(POKER_PHASE_WAITING_ACTION, 0.6f, "opacity:faded");
            add(POKER_PHASE_SHOWDOWN_REVEAL, 0.6f, "opacity:faded");
            add(POKER_PHASE_PAYOUT_SETTLEMENT, 0.4f, "opacity:faded");
        }
    }

    int anchorCount = in.anchorHits;
    if (anchorCount <= 0)
    {
        const char* anchors[] = {
            "blind","cards","community","pot","call","fold","raise","bet",
            "check","turn","pair","straight","flush","wins","amount",
            "called","raised","folded","checked","skip","auto"
        };
        for (const char* a : anchors)
            if (hasToken(a))
                anchorCount++;
    }
    out.pokerAnchor = anchorCount > 0;

    float outScore = 0.4f;
    if (!out.pokerAnchor)
        outScore += 2.2f;
    if (normalized.size() < 6)
        outScore += 0.7f;
    if (in.opacityHint < 0.25f)
        outScore += 0.3f;
    if (hasToken("leave"))
        outScore += 0.4f;
    out.phaseScores[(size_t)POKER_PHASE_OUT_OF_POKER] += outScore;

    out.reasons = BuildReasonSummary(reasons);
    return out;
}

static bool IsPhaseTransitionAllowed(PokerPhase from, PokerPhase to)
{
    if (from == to)
        return true;
    if (to == POKER_PHASE_OUT_OF_POKER)
        return true;
    if (from == POKER_PHASE_OUT_OF_POKER)
        return (to == POKER_PHASE_TABLE_IDLE || to == POKER_PHASE_PLAYER_DECISION || to == POKER_PHASE_WAITING_ACTION);
    if (from == POKER_PHASE_SHOWDOWN_REVEAL)
        return (to == POKER_PHASE_PAYOUT_SETTLEMENT || to == POKER_PHASE_TABLE_IDLE || to == POKER_PHASE_OUT_OF_POKER);
    if (from == POKER_PHASE_PAYOUT_SETTLEMENT)
        return (to == POKER_PHASE_TABLE_IDLE || to == POKER_PHASE_PLAYER_DECISION || to == POKER_PHASE_WAITING_ACTION || to == POKER_PHASE_OUT_OF_POKER);
    return true;
}

static bool UpdatePokerStateMachine(DetectionScore& score, DWORD now)
{
    if (score.gateFail)
    {
        score.guessPhase = gDetectRuntime.phase;
        score.confidence = gDetectRuntime.phaseConfidence;
        score.candidateStableMs = 0;
        return gDetectRuntime.inPoker;
    }

    gDetectRuntime.scoreHistory.push_back(score.phaseScores);
    while ((int)gDetectRuntime.scoreHistory.size() > 6)
        gDetectRuntime.scoreHistory.pop_front();

    std::array<float, POKER_PHASE_COUNT> smooth{};
    for (const auto& s : gDetectRuntime.scoreHistory)
        for (size_t i = 0; i < smooth.size(); i++)
            smooth[i] += s[i];

    float histN = (float)gDetectRuntime.scoreHistory.size();
    if (histN <= 0.0f)
        histN = 1.0f;
    for (size_t i = 0; i < smooth.size(); i++)
        smooth[i] /= histN;

    int bestIdx = 0;
    float bestScore = smooth[0];
    float scoreSum = smooth[0];
    for (int i = 1; i < POKER_PHASE_COUNT; i++)
    {
        scoreSum += smooth[i];
        if (smooth[i] > bestScore)
        {
            bestScore = smooth[i];
            bestIdx = i;
        }
    }

    if (scoreSum <= 0.0001f)
        scoreSum = 0.0001f;

    score.phaseScores = smooth;
    score.guessPhase = (PokerPhase)bestIdx;
    score.confidence = ClampFloat(bestScore / scoreSum, 0.0f, 1.0f);

    if (gDetectRuntime.candidatePhase != score.guessPhase)
    {
        gDetectRuntime.candidatePhase = score.guessPhase;
        gDetectRuntime.candidateSince = now;
    }
    score.candidateStableMs = (gDetectRuntime.candidateSince > 0) ? (now - gDetectRuntime.candidateSince) : 0;

    bool shouldTransition = false;
    if (score.guessPhase == POKER_PHASE_OUT_OF_POKER)
    {
        DWORD requiredOutStableMs = (DWORD)gCfg.ocrOutStableMs;
        bool fadeLikely = gCfg.ocrBlackoutGuardEnable &&
            score.opacityHint <= gCfg.ocrBlackoutOpacityThreshold;
        if (fadeLikely)
            requiredOutStableMs += (DWORD)gCfg.ocrBlackoutOutExtraMs;

        bool recentPokerAnchor = gCfg.ocrBlackoutGuardEnable &&
            gDetectRuntime.phase != POKER_PHASE_OUT_OF_POKER &&
            gLastPokerAnchorSeenAt > 0 &&
            (now - gLastPokerAnchorSeenAt) <= (DWORD)gCfg.ocrBlackoutAnchorGraceMs;
        bool fadeHoldActive = fadeLikely &&
            recentPokerAnchor &&
            score.candidateStableMs < (requiredOutStableMs + (DWORD)gCfg.ocrBlackoutMaxHoldMs);

        bool payoutHoldWindowActive = gCfg.ocrPayoutGuardEnable &&
            gDetectRuntime.phase != POKER_PHASE_OUT_OF_POKER &&
            gPayoutHoldUntilAt > now;
        if (payoutHoldWindowActive)
            requiredOutStableMs += (DWORD)gCfg.ocrPayoutOutExtraMs;
        bool payoutHoldActive = payoutHoldWindowActive &&
            score.candidateStableMs < requiredOutStableMs;

        if (!score.pokerAnchor &&
            score.confidence >= gCfg.ocrPhaseConfThreshold &&
            score.candidateStableMs >= requiredOutStableMs &&
            !fadeHoldActive &&
            !payoutHoldActive)
        {
            shouldTransition = true;
        }
        else if (fadeHoldActive)
        {
            score.gateReason = "fadeHold";
            if (score.reasons.empty())
                score.reasons = "fadeHold";
            else
                score.reasons += ",fadeHold";
        }
        else if (payoutHoldActive)
        {
            score.gateReason = "payoutHold";
            if (score.reasons.empty())
                score.reasons = "payoutHold";
            else
                score.reasons += ",payoutHold";
        }
    }
    else if (score.confidence >= gCfg.ocrPhaseConfThreshold &&
        score.pokerAnchor &&
        score.candidateStableMs >= (DWORD)gCfg.ocrPhaseStableMs &&
        IsPhaseTransitionAllowed(gDetectRuntime.phase, score.guessPhase))
    {
        shouldTransition = true;
    }

    if (shouldTransition && gDetectRuntime.phase != score.guessPhase)
    {
        Log("[PHASE] transition %s -> %s conf=%.2f",
            PokerPhaseToString(gDetectRuntime.phase),
            PokerPhaseToString(score.guessPhase),
            score.confidence);
        gDetectRuntime.phase = score.guessPhase;
        if (gDetectRuntime.phase == POKER_PHASE_PAYOUT_SETTLEMENT)
        {
            gLastPayoutMarkerSeenAt = now;
            DWORD holdMs = (DWORD)gCfg.ocrPayoutMarkerGraceMs + (DWORD)gCfg.ocrPayoutOutExtraMs;
            DWORD holdUntil = now + holdMs;
            if (holdUntil > gPayoutHoldUntilAt)
                gPayoutHoldUntilAt = holdUntil;
        }
    }

    gDetectRuntime.phaseConfidence = score.confidence;
    gDetectRuntime.inPoker = (gDetectRuntime.phase != POKER_PHASE_OUT_OF_POKER);
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
                bool usingPortableOcr = false;
                std::string ocrExePath = ResolveOcrExecutablePath(usingPortableOcr);
                Log("[OCR] WARNING: Failed to start OCR process repeatedly (reason=%s, winerr=%lu, bl=(%d,%d,%d,%d), tr=(%d,%d,%d,%d), tesseract='%s').",
                    OcrStartFailReasonToString(gLastOcrStartFailReason),
                    (unsigned long)gLastOcrStartWinErr,
                    gCfg.ocrBottomLeftXPct, gCfg.ocrBottomLeftYPct, gCfg.ocrBottomLeftWPct, gCfg.ocrBottomLeftHPct,
                    gCfg.ocrTopRightXPct, gCfg.ocrTopRightYPct, gCfg.ocrTopRightWPct, gCfg.ocrTopRightHPct,
                    ocrExePath.c_str());
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

    if (in.scanOk)
    {
        bool fadeLikely = gCfg.ocrBlackoutGuardEnable &&
            in.opacityHint <= gCfg.ocrBlackoutOpacityThreshold;
        bool hasMoneyGlyph = in.rawText.find('$') != std::string::npos;

        // During blackout/fade with no visible money glyphs, keep last OCR money snapshot.
        if (!(fadeLikely && !hasMoneyGlyph))
            UpdateOcrMoneySnapshot(in.rawText, now);

        bool payoutMarkerNow = false;
        if (in.rawText.find("wins") != std::string::npos ||
            in.rawText.find("winner") != std::string::npos ||
            in.rawText.find("collect") != std::string::npos ||
            in.rawText.find("collected") != std::string::npos ||
            in.rawText.find("payout") != std::string::npos)
        {
            payoutMarkerNow = true;
        }
        if (gOcrMoney.winsCents > 0)
            payoutMarkerNow = true;
        if (payoutMarkerNow)
        {
            gLastPayoutMarkerSeenAt = now;
            DWORD holdMs = (DWORD)gCfg.ocrPayoutMarkerGraceMs + (DWORD)gCfg.ocrPayoutOutExtraMs;
            DWORD holdUntil = now + holdMs;
            if (holdUntil > gPayoutHoldUntilAt)
                gPayoutHoldUntilAt = holdUntil;
        }
    }

    DetectionScore score = ComputeDetectionScore(in);
    if (score.pokerAnchor || in.anchorHits > 0)
        gLastPokerAnchorSeenAt = now;

    gLastDetectInputs = in;
    gLastDetectScore = score;
    bool inPoker = UpdatePokerStateMachine(gLastDetectScore, now);
    gLastDetectScore.opacityHint = in.opacityHint;

    if (gCfg.ocrLogEveryMs > 0 && now >= gNextOcrLogAt)
    {
        gNextOcrLogAt = now + (DWORD)gCfg.ocrLogEveryMs;
        std::string snippet = in.scanOk ? OcrTextLogSnippet(gLastOcrText, 96) : "";
        Log("[OCR] scanOk=%d pending=%d hits=%d anchors=%d score=%d gate=%s text='%s'",
            in.scanOk ? 1 : 0,
            in.pending ? 1 : 0,
            in.keywordHits,
            in.anchorHits,
            gLastDetectScore.total,
            gLastDetectScore.gateReason,
            snippet.c_str());
        Log("[PHASE] guess=%s conf=%.2f stableMs=%lu opacity=%.2f reasons=%s",
            PokerPhaseToString(gLastDetectScore.guessPhase),
            gLastDetectScore.confidence,
            (unsigned long)gLastDetectScore.candidateStableMs,
            gLastDetectScore.opacityHint,
            gLastDetectScore.reasons.empty() ? "-" : gLastDetectScore.reasons.c_str());
        if (in.scanOk)
        {
            Log("[OCR$] pot=%d($%.2f) src=%s main=%d($%.2f) side=%d($%.2f) wins=%d($%.2f) player=%d($%.2f) npc=%s amounts=%s",
                gOcrMoney.potCents, (double)gOcrMoney.potCents / 100.0,
                OcrPotSourceToString(gOcrMoney.potSource),
                gOcrMoney.mainPotCents, (double)gOcrMoney.mainPotCents / 100.0,
                gOcrMoney.sidePotCents, (double)gOcrMoney.sidePotCents / 100.0,
                gOcrMoney.winsCents, (double)gOcrMoney.winsCents / 100.0,
                gOcrMoney.playerCents, (double)gOcrMoney.playerCents / 100.0,
                OcrAmountListSnippet(gOcrMoney.npcAmountsCents).c_str(),
                OcrAmountListSnippet(gOcrMoney.amountsCents).c_str());
        }
    }

    return inPoker;
}
// ---------------- Message + state ----------------
static bool  gWasInPoker = false;
static DWORD gNextAllowedEnterMsg = 0;

// Detection throttling
static DWORD gNextDetectAt = 0;
static bool  gCachedInPoker = false;
static PokerPhase gLastMoneyPhase = POKER_PHASE_OUT_OF_POKER;
static int gSettlementSerial = 0;
static int gLastPaidSettlementSerial = -1;
static DWORD gNextAllowedPayoutAt = 0;

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
    gCfg.hudToastRetryMs     = IniGetInt("HUD", "ToastRetryMs", 4000, gIniPath);
    gCfg.hudToastSoundSet    = IniGetString("HUD", "ToastSoundSet", "", gIniPath);
    gCfg.hudToastSound       = IniGetString("HUD", "ToastSound", "", gIniPath);
    gCfg.hudPanelX           = IniGetInt("HUD", "PanelX", 80, gIniPath);
    gCfg.hudPanelY           = IniGetInt("HUD", "PanelY", 94, gIniPath);
    gCfg.hudPanelLineStep    = IniGetFloat("HUD", "PanelLineStep", 2.2f, gIniPath);
    gCfg.hudPanelMaxLines    = IniGetInt("HUD", "PanelMaxLines", 24, gIniPath);
    gCfg.hudPanelAnchorBottom = IniGetInt("HUD", "PanelAnchorBottom", 1, gIniPath);

    bool hudCfgClamped = false;
    hudCfgClamped |= ClampSectionIntSetting("HUD", "DrawMethod", gDrawMethod, 1, 2);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "HUDUiMode", gCfg.hudUiMode, HUD_UI_MODE_LEGACY_TEXT, HUD_UI_MODE_ROCKSTAR_TOASTS_HYBRID);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "ToastEnabled", gCfg.hudToastEnabled, 0, 1);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "ToastFallbackText", gCfg.hudToastFallbackText, 0, 1);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "ToastDurationMs", gCfg.hudToastDurationMs, 100, 10000);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "ToastRetryMs", gCfg.hudToastRetryMs, 250, 60000);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "PanelX", gCfg.hudPanelX, 0, 100);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "PanelY", gCfg.hudPanelY, 0, 100);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "PanelMaxLines", gCfg.hudPanelMaxLines, 1, 128);
    hudCfgClamped |= ClampSectionIntSetting("HUD", "PanelAnchorBottom", gCfg.hudPanelAnchorBottom, 0, 1);
    if (gCfg.hudPanelLineStep < 0.8f)
    {
        gCfg.hudPanelLineStep = 0.8f;
        hudCfgClamped = true;
        Log("[CFG] WARNING: HUD.PanelLineStep too small. Clamped to 0.8.");
    }
    else if (gCfg.hudPanelLineStep > 8.0f)
    {
        gCfg.hudPanelLineStep = 8.0f;
        hudCfgClamped = true;
        Log("[CFG] WARNING: HUD.PanelLineStep too large. Clamped to 8.0.");
    }

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
    gCfg.ocrProcessTimeoutMs   = IniGetInt("OCR", "ProcessTimeoutMs", 2000, gIniPath);
    gCfg.ocrBottomLeftXPct     = IniGetInt("OCR", "BottomLeftXPct", 0, gIniPath);
    gCfg.ocrBottomLeftYPct     = IniGetInt("OCR", "BottomLeftYPct", 34, gIniPath);
    gCfg.ocrBottomLeftWPct     = IniGetInt("OCR", "BottomLeftWPct", 34, gIniPath);
    gCfg.ocrBottomLeftHPct     = IniGetInt("OCR", "BottomLeftHPct", 66, gIniPath);
    gCfg.ocrTopRightXPct       = IniGetInt("OCR", "TopRightXPct", 72, gIniPath);
    gCfg.ocrTopRightYPct       = IniGetInt("OCR", "TopRightYPct", 0, gIniPath);
    gCfg.ocrTopRightWPct       = IniGetInt("OCR", "TopRightWPct", 28, gIniPath);
    gCfg.ocrTopRightHPct       = IniGetInt("OCR", "TopRightHPct", 30, gIniPath);
    gCfg.ocrPsm                = IniGetInt("OCR", "PSM", 11, gIniPath);
    gCfg.ocrDebugReasonOverlay = IniGetInt("OCR", "DebugReasonOverlay", 0, gIniPath); // compatibility key (forced off)
    gCfg.ocrLogEveryMs         = IniGetInt("OCR", "LogEveryMs", 0, gIniPath);
    gCfg.ocrDumpArtifacts      = IniGetInt("OCR", "DumpArtifacts", 0, gIniPath);
    gCfg.ocrPhaseStableMs      = IniGetInt("OCR", "PhaseStableMs", 1800, gIniPath);
    gCfg.ocrOutStableMs        = IniGetInt("OCR", "OutStableMs", 4200, gIniPath);
    gCfg.ocrPhaseConfThreshold = IniGetFloat("OCR", "PhaseConfThreshold", 0.62f, gIniPath);
    gCfg.ocrOpacityHintEnable  = IniGetInt("OCR", "OpacityHintEnable", 1, gIniPath);
    gCfg.ocrOpacityRoiXPct     = IniGetInt("OCR", "OpacityRoiXPct", 72, gIniPath);
    gCfg.ocrOpacityRoiYPct     = IniGetInt("OCR", "OpacityRoiYPct", 66, gIniPath);
    gCfg.ocrOpacityRoiWPct     = IniGetInt("OCR", "OpacityRoiWPct", 27, gIniPath);
    gCfg.ocrOpacityRoiHPct     = IniGetInt("OCR", "OpacityRoiHPct", 30, gIniPath);
    gCfg.ocrOpacityLow         = IniGetFloat("OCR", "OpacityLow", 8.0f, gIniPath);
    gCfg.ocrOpacityHigh        = IniGetFloat("OCR", "OpacityHigh", 28.0f, gIniPath);
    gCfg.ocrBlackoutGuardEnable = IniGetInt("OCR", "BlackoutGuardEnable", 1, gIniPath);
    gCfg.ocrBlackoutOpacityThreshold = IniGetFloat("OCR", "BlackoutOpacityThreshold", 0.18f, gIniPath);
    gCfg.ocrBlackoutAnchorGraceMs = IniGetInt("OCR", "BlackoutAnchorGraceMs", 6000, gIniPath);
    gCfg.ocrBlackoutOutExtraMs = IniGetInt("OCR", "BlackoutOutExtraMs", 2500, gIniPath);
    gCfg.ocrBlackoutMaxHoldMs = IniGetInt("OCR", "BlackoutMaxHoldMs", 2500, gIniPath);
    gCfg.ocrPayoutGuardEnable = IniGetInt("OCR", "PayoutGuardEnable", 1, gIniPath);
    gCfg.ocrPayoutMarkerGraceMs = IniGetInt("OCR", "PayoutMarkerGraceMs", 9000, gIniPath);
    gCfg.ocrPayoutOutExtraMs = IniGetInt("OCR", "PayoutOutExtraMs", 5000, gIniPath);
    gCfg.ocrPlayerNameHint    = IniGetString("OCR", "PlayerNameHint", "arthur", gIniPath);
    gCfg.ocrTesseractPath      = IniGetString("OCR", "TesseractPath", "tesseract", gIniPath);
    gCfg.ocrKeywords           = IniGetString("OCR", "Keywords", "poker,ante,call,fold,raise,check,bet,pot,blind,cards,community,turn", gIniPath);

    gCfg.ocrEnabled            = ClampInt(gCfg.ocrEnabled, 0, 1);
    gCfg.ocrIntervalMs         = ClampInt(gCfg.ocrIntervalMs, 200, 30000);
    gCfg.ocrProcessTimeoutMs   = ClampInt(gCfg.ocrProcessTimeoutMs, 250, 10000);
    gCfg.ocrBottomLeftXPct     = ClampInt(gCfg.ocrBottomLeftXPct, 0, 100);
    gCfg.ocrBottomLeftYPct     = ClampInt(gCfg.ocrBottomLeftYPct, 0, 100);
    gCfg.ocrBottomLeftWPct     = ClampInt(gCfg.ocrBottomLeftWPct, 1, 100);
    gCfg.ocrBottomLeftHPct     = ClampInt(gCfg.ocrBottomLeftHPct, 1, 100);
    gCfg.ocrTopRightXPct       = ClampInt(gCfg.ocrTopRightXPct, 0, 100);
    gCfg.ocrTopRightYPct       = ClampInt(gCfg.ocrTopRightYPct, 0, 100);
    gCfg.ocrTopRightWPct       = ClampInt(gCfg.ocrTopRightWPct, 1, 100);
    gCfg.ocrTopRightHPct       = ClampInt(gCfg.ocrTopRightHPct, 1, 100);
    gCfg.ocrPsm                = ClampInt(gCfg.ocrPsm, 3, 13);
    gCfg.ocrDebugReasonOverlay = 0;
    gCfg.ocrLogEveryMs         = ClampInt(gCfg.ocrLogEveryMs, 0, 60000);
    gCfg.ocrDumpArtifacts      = ClampInt(gCfg.ocrDumpArtifacts, 0, 1);
    gCfg.ocrPhaseStableMs      = ClampInt(gCfg.ocrPhaseStableMs, 250, 15000);
    gCfg.ocrOutStableMs        = ClampInt(gCfg.ocrOutStableMs, 500, 30000);
    gCfg.ocrOpacityHintEnable  = ClampInt(gCfg.ocrOpacityHintEnable, 0, 1);
    gCfg.ocrOpacityRoiXPct     = ClampInt(gCfg.ocrOpacityRoiXPct, 0, 100);
    gCfg.ocrOpacityRoiYPct     = ClampInt(gCfg.ocrOpacityRoiYPct, 0, 100);
    gCfg.ocrOpacityRoiWPct     = ClampInt(gCfg.ocrOpacityRoiWPct, 1, 100);
    gCfg.ocrOpacityRoiHPct     = ClampInt(gCfg.ocrOpacityRoiHPct, 1, 100);
    gCfg.ocrBlackoutGuardEnable = ClampInt(gCfg.ocrBlackoutGuardEnable, 0, 1);
    gCfg.ocrBlackoutAnchorGraceMs = ClampInt(gCfg.ocrBlackoutAnchorGraceMs, 0, 60000);
    gCfg.ocrBlackoutOutExtraMs = ClampInt(gCfg.ocrBlackoutOutExtraMs, 0, 30000);
    gCfg.ocrBlackoutMaxHoldMs = ClampInt(gCfg.ocrBlackoutMaxHoldMs, 0, 30000);
    gCfg.ocrPayoutGuardEnable = ClampInt(gCfg.ocrPayoutGuardEnable, 0, 1);
    gCfg.ocrPayoutMarkerGraceMs = ClampInt(gCfg.ocrPayoutMarkerGraceMs, 0, 60000);
    gCfg.ocrPayoutOutExtraMs = ClampInt(gCfg.ocrPayoutOutExtraMs, 0, 30000);
    gCfg.ocrPhaseConfThreshold = ClampFloat(gCfg.ocrPhaseConfThreshold, 0.20f, 0.95f);
    gCfg.ocrOpacityLow         = ClampFloat(gCfg.ocrOpacityLow, 0.0f, 255.0f);
    gCfg.ocrOpacityHigh        = ClampFloat(gCfg.ocrOpacityHigh, 0.0f, 255.0f);
    gCfg.ocrBlackoutOpacityThreshold = ClampFloat(gCfg.ocrBlackoutOpacityThreshold, 0.00f, 1.00f);
    gCfg.ocrPlayerNameHint = ToLowerAscii(TrimAscii(gCfg.ocrPlayerNameHint));
    if (gCfg.ocrOpacityHigh <= gCfg.ocrOpacityLow + 0.1f)
        gCfg.ocrOpacityHigh = gCfg.ocrOpacityLow + 0.1f;

    BuildOcrKeywordList();
    StopOcrProcess(true);
    gNextOcrStartAt = 0;
    gNextOcrLogAt = 0;
    gPendingOpacityHint = 0.5f;
    gLastOpacityHint = 0.5f;
    gLastPokerAnchorSeenAt = 0;
    gLastPayoutMarkerSeenAt = 0;
    gPayoutHoldUntilAt = 0;
    gOcrStartFailureStreak = 0;
    gOcrStartFailureWarned = false;
    gLastOcrStartFailReason = OCR_START_FAIL_NONE;
    gLastOcrStartWinErr = 0;
    gDetectRuntime = DetectionRuntime{};
    gLastDetectInputs = DetectionInputs{};
    gLastDetectScore = DetectionScore{};
    gHudToastNativeFailed = false;
    gHudToastNativeWarned = false;
    gHudToastNativeRetryAt = 0;
    gHudToastNativeFailCount = 0;
    gLegacyHudMessage = "~COLOR_GOLD~Mod Online";
    gLegacyHudMessageUntil = 0;
    gOcrMoney = OcrMoneySnapshot{};
    gAutoPotGlobal = -1;
    gAutoPlayerGlobal = -1;
    gLastMoneyPhase = POKER_PHASE_OUT_OF_POKER;
    gSettlementSerial = 0;
    gLastPaidSettlementSerial = -1;
    gNextAllowedPayoutAt = 0;

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
    gCfg.moneyNpcTrackMax       = IniGetInt("Money", "NpcTrackMax", 5, gIniPath);
    gCfg.moneyBetStepFilterEnable = IniGetInt("Money", "BetStepFilterEnable", 1, gIniPath);
    gCfg.moneyBetStepDollars    = IniGetInt("Money", "BetStepDollars", 5, gIniPath);
    gCfg.moneyBetMinDollars     = IniGetInt("Money", "BetMinDollars", 10, gIniPath);
    gCfg.moneyExceptionLogCooldownMs = IniGetInt("Money", "ExceptionLogCooldownMs", 30000, gIniPath);
    gCfg.moneySkipFaultRuns     = IniGetInt("Money", "SkipFaultRuns", 1, gIniPath);
    gCfg.moneyOcrMatchToleranceCents = IniGetInt("Money", "OcrMatchToleranceCents", 6, gIniPath);
    gCfg.moneyAutoLockPot       = IniGetInt("Money", "AutoLockPot", 1, gIniPath);
    gCfg.moneyAutoLockPotMinMatches = IniGetInt("Money", "AutoLockPotMinMatches", 10, gIniPath);
    gCfg.moneyAutoLockPlayer    = IniGetInt("Money", "AutoLockPlayer", 1, gIniPath);
    gCfg.moneyAutoLockPlayerMinMatches = IniGetInt("Money", "AutoLockPlayerMinMatches", 8, gIniPath);
    gCfg.moneyOverlayMultiplier = IniGetFloat("Money", "OverlayMultiplier", 2.0f, gIniPath);
    gCfg.moneyPayoutEnable      = IniGetInt("Money", "PayoutEnable", 0, gIniPath);
    gCfg.moneyPayoutMultiplier  = IniGetFloat("Money", "PayoutMultiplier", 2.0f, gIniPath);
    gCfg.moneyPayoutUseWinsAmount = IniGetInt("Money", "PayoutUseWinsAmount", 1, gIniPath);
    gCfg.moneyPayoutFallbackToPot = IniGetInt("Money", "PayoutFallbackToPot", 1, gIniPath);
    gCfg.moneyPayoutCooldownMs  = IniGetInt("Money", "PayoutCooldownMs", 6000, gIniPath);
    gCfg.moneyPayoutMinPhaseConf = IniGetFloat("Money", "PayoutMinPhaseConf", 0.55f, gIniPath);

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
    moneyCfgClamped |= ClampIntSetting("NpcTrackMax", gCfg.moneyNpcTrackMax, 0, 32);
    moneyCfgClamped |= ClampIntSetting("BetStepFilterEnable", gCfg.moneyBetStepFilterEnable, 0, 1);
    moneyCfgClamped |= ClampIntSetting("BetStepDollars", gCfg.moneyBetStepDollars, 1, 1000);
    moneyCfgClamped |= ClampIntSetting("BetMinDollars", gCfg.moneyBetMinDollars, 1, 100000);
    moneyCfgClamped |= ClampIntSetting("ExceptionLogCooldownMs", gCfg.moneyExceptionLogCooldownMs, 0, 600000);
    moneyCfgClamped |= ClampIntSetting("SkipFaultRuns", gCfg.moneySkipFaultRuns, 0, 1);
    moneyCfgClamped |= ClampIntSetting("LogTopN", gCfg.moneyLogTopN, 0, 64);
    moneyCfgClamped |= ClampIntSetting("OcrMatchToleranceCents", gCfg.moneyOcrMatchToleranceCents, 0, 2500);
    moneyCfgClamped |= ClampIntSetting("AutoLockPot", gCfg.moneyAutoLockPot, 0, 1);
    moneyCfgClamped |= ClampIntSetting("AutoLockPotMinMatches", gCfg.moneyAutoLockPotMinMatches, 1, 1000000);
    moneyCfgClamped |= ClampIntSetting("AutoLockPlayer", gCfg.moneyAutoLockPlayer, 0, 1);
    moneyCfgClamped |= ClampIntSetting("AutoLockPlayerMinMatches", gCfg.moneyAutoLockPlayerMinMatches, 1, 1000000);
    moneyCfgClamped |= ClampIntSetting("PayoutEnable", gCfg.moneyPayoutEnable, 0, 1);
    moneyCfgClamped |= ClampIntSetting("PayoutUseWinsAmount", gCfg.moneyPayoutUseWinsAmount, 0, 1);
    moneyCfgClamped |= ClampIntSetting("PayoutFallbackToPot", gCfg.moneyPayoutFallbackToPot, 0, 1);
    moneyCfgClamped |= ClampIntSetting("PayoutCooldownMs", gCfg.moneyPayoutCooldownMs, 250, 600000);

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

    if (gCfg.moneyOverlayMultiplier < 0.10f)
    {
        gCfg.moneyOverlayMultiplier = 0.10f;
        moneyCfgClamped = true;
        Log("[CFG] WARNING: Money.OverlayMultiplier too small. Clamped to 0.10.");
    }
    else if (gCfg.moneyOverlayMultiplier > 1000.0f)
    {
        gCfg.moneyOverlayMultiplier = 1000.0f;
        moneyCfgClamped = true;
        Log("[CFG] WARNING: Money.OverlayMultiplier too large. Clamped to 1000.");
    }

    if (gCfg.moneyBetMinDollars < gCfg.moneyBetStepDollars)
    {
        int oldMin = gCfg.moneyBetMinDollars;
        gCfg.moneyBetMinDollars = gCfg.moneyBetStepDollars;
        moneyCfgClamped = true;
        Log("[CFG] WARNING: Money.BetMinDollars (%d) < BetStepDollars (%d). Clamped to %d.",
            oldMin, gCfg.moneyBetStepDollars, gCfg.moneyBetMinDollars);
    }

    if (gCfg.moneyPayoutMultiplier < 1.0f)
    {
        gCfg.moneyPayoutMultiplier = 1.0f;
        moneyCfgClamped = true;
        Log("[CFG] WARNING: Money.PayoutMultiplier below 1.0. Clamped to 1.0.");
    }
    else if (gCfg.moneyPayoutMultiplier > 1000.0f)
    {
        gCfg.moneyPayoutMultiplier = 1000.0f;
        moneyCfgClamped = true;
        Log("[CFG] WARNING: Money.PayoutMultiplier too large. Clamped to 1000.");
    }
    gCfg.moneyPayoutMinPhaseConf = ClampFloat(gCfg.moneyPayoutMinPhaseConf, 0.20f, 0.99f);

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
    Log("[CFG] OCR: Enabled=%d IntervalMs=%d ProcTimeoutMs=%d BL=(%d,%d,%d,%d) TR=(%d,%d,%d,%d) PSM=%d DebugReason=%d LogEveryMs=%d DumpArtifacts=%d PhaseStableMs=%d OutStableMs=%d PhaseConf=%.2f OpacityHint=%d OpacityROI=(%d,%d,%d,%d) OpacityRange=[%.1f..%.1f] BlackoutGuard=%d BlackoutOpacity<=%.2f BlackoutGraceMs=%d BlackoutOutExtraMs=%d BlackoutMaxHoldMs=%d PayoutGuard=%d PayoutGraceMs=%d PayoutOutExtraMs=%d PlayerNameHint='%s' Tesseract='%s' Keywords=%d",
        gCfg.ocrEnabled, gCfg.ocrIntervalMs,
        gCfg.ocrProcessTimeoutMs,
        gCfg.ocrBottomLeftXPct, gCfg.ocrBottomLeftYPct, gCfg.ocrBottomLeftWPct, gCfg.ocrBottomLeftHPct,
        gCfg.ocrTopRightXPct, gCfg.ocrTopRightYPct, gCfg.ocrTopRightWPct, gCfg.ocrTopRightHPct,
        gCfg.ocrPsm, gCfg.ocrDebugReasonOverlay, gCfg.ocrLogEveryMs, gCfg.ocrDumpArtifacts,
        gCfg.ocrPhaseStableMs, gCfg.ocrOutStableMs, gCfg.ocrPhaseConfThreshold,
        gCfg.ocrOpacityHintEnable,
        gCfg.ocrOpacityRoiXPct, gCfg.ocrOpacityRoiYPct, gCfg.ocrOpacityRoiWPct, gCfg.ocrOpacityRoiHPct,
        gCfg.ocrOpacityLow, gCfg.ocrOpacityHigh,
        gCfg.ocrBlackoutGuardEnable, gCfg.ocrBlackoutOpacityThreshold, gCfg.ocrBlackoutAnchorGraceMs, gCfg.ocrBlackoutOutExtraMs, gCfg.ocrBlackoutMaxHoldMs,
        gCfg.ocrPayoutGuardEnable, gCfg.ocrPayoutMarkerGraceMs, gCfg.ocrPayoutOutExtraMs,
        gCfg.ocrPlayerNameHint.c_str(),
        gCfg.ocrTesseractPath.c_str(), (int)gOcrKeywords.size());
    {
        bool usingPortableOcr = false;
        std::string ocrExePath = ResolveOcrExecutablePath(usingPortableOcr);
        Log("[CFG] OCR runtime: resolved='%s' portable=%d gameDir='%s'",
            ocrExePath.c_str(), usingPortableOcr ? 1 : 0, gGameDirPath);
    }
    Log("[CFG] HUD: DrawMethod=%d HUDUiMode=%d ToastEnabled=%d ToastFallbackText=%d ToastIconDict='%s' ToastIcon='%s' ToastColor='%s' ToastDurationMs=%d ToastRetryMs=%d Panel=(%d,%d) LineStep=%.2f MaxLines=%d AnchorBottom=%d ToastSoundSet='%s' ToastSound='%s'",
        gDrawMethod, gCfg.hudUiMode, gCfg.hudToastEnabled, gCfg.hudToastFallbackText,
        gCfg.hudToastIconDict.c_str(), gCfg.hudToastIcon.c_str(), gCfg.hudToastColor.c_str(),
        gCfg.hudToastDurationMs, gCfg.hudToastRetryMs, gCfg.hudPanelX, gCfg.hudPanelY, gCfg.hudPanelLineStep, gCfg.hudPanelMaxLines, gCfg.hudPanelAnchorBottom,
        gCfg.hudToastSoundSet.c_str(), gCfg.hudToastSound.c_str());

    Log("[CFG] Money: Overlay=%d ScanEnable=%d Range=[%d..%d) Batch=%d IntervalMs=%d ValueRange=[%d..%d] TopN=%d PruneMs=%d",
        gCfg.moneyOverlay, gCfg.moneyScanEnable,
        gCfg.moneyScanStart, gCfg.moneyScanEnd,
        gCfg.moneyScanBatch, gCfg.moneyScanIntervalMs,
        gCfg.moneyValueMin, gCfg.moneyValueMax,
        gCfg.moneyTopN, gCfg.moneyPruneMs);
    Log("[CFG] Money perf: ScanMaxReadsPerStep=%d ScanMaxStepMs=%d ExceptionLogCooldownMs=%d SkipFaultRuns=%d LikelyMaxChangesPerSec=%.2f BetStepFilter=%d BetStepDollars=%d BetMinDollars=%d",
        gCfg.moneyScanMaxReadsPerStep, gCfg.moneyScanMaxStepMs,
        gCfg.moneyExceptionLogCooldownMs, gCfg.moneySkipFaultRuns, gCfg.moneyLikelyMaxChangesPerSec,
        gCfg.moneyBetStepFilterEnable, gCfg.moneyBetStepDollars, gCfg.moneyBetMinDollars);
    Log("[CFG] Money OCR: OcrMatchToleranceCents=%d NpcTrackMax=%d AutoLockPot=%d AutoLockPotMinMatches=%d AutoLockPlayer=%d AutoLockPlayerMinMatches=%d OverlayMultiplier=%.2f",
        gCfg.moneyOcrMatchToleranceCents, gCfg.moneyNpcTrackMax, gCfg.moneyAutoLockPot, gCfg.moneyAutoLockPotMinMatches,
        gCfg.moneyAutoLockPlayer, gCfg.moneyAutoLockPlayerMinMatches, gCfg.moneyOverlayMultiplier);
    Log("[CFG] Money payout: Enable=%d Multiplier=%.2f UseWinsAmount=%d FallbackToPot=%d CooldownMs=%d MinPhaseConf=%.2f",
        gCfg.moneyPayoutEnable, gCfg.moneyPayoutMultiplier, gCfg.moneyPayoutUseWinsAmount,
        gCfg.moneyPayoutFallbackToPot, gCfg.moneyPayoutCooldownMs, gCfg.moneyPayoutMinPhaseConf);
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

static int GetEffectivePotGlobalIndex()
{
    if (gCfg.potGlobal >= 0)
        return gCfg.potGlobal;
    return gAutoPotGlobal;
}

static int GetEffectivePlayerGlobalIndex()
{
    if (gCfg.stackGlobal0 >= 0)
        return gCfg.stackGlobal0;
    return gAutoPlayerGlobal;
}

static bool TryReadEffectivePotCents(int& outPotCents)
{
    outPotCents = 0;
    int idx = GetEffectivePotGlobalIndex();
    if (idx < 0)
        return false;
    int val = 0;
    if (!ReadGlobalInt(idx, val))
        return false;
    outPotCents = val;
    return true;
}

static bool TryReadEffectivePlayerCents(int& outPlayerCents)
{
    outPlayerCents = 0;
    int idx = GetEffectivePlayerGlobalIndex();
    if (idx < 0)
        return false;
    int val = 0;
    if (!ReadGlobalInt(idx, val))
        return false;
    outPlayerCents = val;
    return true;
}

static bool CandidatePassesPotAutoLockChecks(const MoneyCandidate& c, DWORD now)
{
    if (c.ocrPotMatches < gCfg.moneyAutoLockPotMinMatches)
        return false;
    if (c.changes < 2)
        return false;
    if (!IsLikelyMoneyCandidate(c, now))
        return false;
    if (c.ocrPlayerMatches * 2 > c.ocrPotMatches)
        return false;
    if (c.ocrAnyMatches > 0 && c.ocrPotMatches * 2 < c.ocrAnyMatches)
        return false;
    if (gCfg.moneyBetStepFilterEnable)
    {
        int totalBet = c.betStepMatches + c.betStepMismatches;
        if (totalBet >= 4 && c.betStepMatches * 2 < totalBet)
            return false;
    }
    return true;
}

static bool TryAutoLockPotGlobal(const std::vector<const MoneyCandidate*>& sorted, DWORD now)
{
    if (!gCfg.moneyAutoLockPot)
        return false;
    if (gCfg.potGlobal >= 0 || gAutoPotGlobal >= 0)
        return false;
    if (gOcrMoney.potSource == 5)
        return false; // fallback OCR pot is too ambiguous for auto-lock
    if (sorted.empty())
        return false;

    for (const MoneyCandidate* c : sorted)
    {
        if (!c)
            continue;
        if (!CandidatePassesPotAutoLockChecks(*c, now))
            continue;
        if (gOcrMoney.potCents > 0 && gOcrMoney.potSource != 5 &&
            !CandidateMatchesObservedOcrAmount(c->last, gOcrMoney.potCents))
            continue;
        if (c->lastOcrMatchMs == 0)
            continue;
        if ((now - c->lastOcrMatchMs) > 12000)
            continue;

        gAutoPotGlobal = c->idx;
        Log("[MONEY] AutoLock: Pot global locked to idx=%d (ocrPot=%d ocrAny=%d changes=%d val=%d).",
            c->idx, c->ocrPotMatches, c->ocrAnyMatches, c->changes, c->last);
        char idxBuf[32];
        _snprintf_s(idxBuf, sizeof(idxBuf), "%d", gAutoPotGlobal);
        WritePrivateProfileStringA("Money", "PotGlobal", idxBuf, gIniPath);
        Log("[MONEY] AutoLock: Persisted PotGlobal=%d to %s.", gAutoPotGlobal, gIniPath);
        if (gCfg.hudUiMode != HUD_UI_MODE_LEGACY_TEXT)
        {
            char toast[128];
            _snprintf_s(toast, sizeof(toast), "Pot source locked [%d]", c->idx);
            PostHudToast(toast, HUD_TOAST_EVENT_GENERIC, now);
        }
        return true;
    }

    return false;
}

static bool TryAutoLockPlayerGlobal(DWORD now)
{
    if (!gCfg.moneyAutoLockPlayer)
        return false;
    if (gCfg.stackGlobal0 >= 0 || gAutoPlayerGlobal >= 0)
        return false;
    if (gOcrMoney.playerCents <= 0)
        return false;

    const MoneyCandidate* best = nullptr;
    float bestScore = -1e9f;
    for (const auto& kv : gMoneyCands)
    {
        const MoneyCandidate& c = kv.second;
        if (c.ocrPlayerMatches < gCfg.moneyAutoLockPlayerMinMatches)
            continue;
        if (c.lastOcrMatchMs == 0 || (now - c.lastOcrMatchMs) > 12000)
            continue;

        float score = (float)c.ocrPlayerMatches * 12.0f
            - (float)c.ocrPotMatches * 7.0f
            - (float)c.ocrNpcMatches * 2.5f
            + (float)c.ocrAnyMatches * 0.5f;
        if (score > bestScore)
        {
            bestScore = score;
            best = &c;
        }
    }

    if (!best)
        return false;

    gAutoPlayerGlobal = best->idx;
    Log("[MONEY] AutoLock: Player stack global locked to idx=%d (ocrPlayer=%d ocrPot=%d ocrAny=%d val=%d).",
        best->idx, best->ocrPlayerMatches, best->ocrPotMatches, best->ocrAnyMatches, best->last);
    char idxBuf[32];
    _snprintf_s(idxBuf, sizeof(idxBuf), "%d", gAutoPlayerGlobal);
    WritePrivateProfileStringA("Money", "StackGlobal0", idxBuf, gIniPath);
    Log("[MONEY] AutoLock: Persisted StackGlobal0=%d to %s.", gAutoPlayerGlobal, gIniPath);
    if (gCfg.hudUiMode != HUD_UI_MODE_LEGACY_TEXT)
    {
        char toast[128];
        _snprintf_s(toast, sizeof(toast), "Player source locked [%d]", best->idx);
        PostHudToast(toast, HUD_TOAST_EVENT_GENERIC, now);
    }
    return true;
}

static bool IsLikelyValidPayoutAmount(int cents)
{
    if (cents <= 0)
        return false;
    if (cents > gCfg.moneyValueMax * 4)
        return false;

    // If OCR has a non-fallback pot, reject payout amounts wildly off that anchor.
    if (gOcrMoney.potCents > 0 && gOcrMoney.potSource != 5)
    {
        if (cents > gOcrMoney.potCents * 2)
            return false;
        if (cents * 3 < gOcrMoney.potCents)
            return false;
    }
    return true;
}

static bool TryGetPayoutSourceCents(DWORD now, int& outSourceCents, const char*& outSourceLabel)
{
    outSourceCents = 0;
    outSourceLabel = "none";

    bool ocrFresh = IsOcrMoneyFresh(now);

    if (gCfg.moneyPayoutUseWinsAmount && ocrFresh && gOcrMoney.winsCents > 0 &&
        IsLikelyValidPayoutAmount(gOcrMoney.winsCents))
    {
        outSourceCents = gOcrMoney.winsCents;
        outSourceLabel = "wins";
        return true;
    }

    if (gCfg.moneyPayoutFallbackToPot)
    {
        if (ocrFresh && gOcrMoney.potCents > 0 && gOcrMoney.potSource != 5)
        {
            outSourceCents = gOcrMoney.potCents;
            outSourceLabel = "potOCR";
            return true;
        }

        int potCents = 0;
        if (TryReadEffectivePotCents(potCents) && potCents > 0)
        {
            outSourceCents = potCents;
            outSourceLabel = "potGlobal";
            return true;
        }
    }

    return false;
}

static bool TryApplyPokerPayout(int sourceCents, const char* sourceLabel, DWORD now)
{
    if (sourceCents <= 0)
        return false;

    double scaled = (double)sourceCents * (double)gCfg.moneyPayoutMultiplier;
    int targetCents = (int)std::llround(scaled);
    int bonusCents = targetCents - sourceCents;
    if (bonusCents <= 0)
        return false;

    BOOL ok = MONEY::_MONEY_INCREMENT_CASH_BALANCE(bonusCents, MISC::GET_HASH_KEY("ADD_REASON_DEFAULT"));
    if (!ok)
    {
        Log("[PAYOUT] FAILED source=%s src=%d($%.2f) bonus=%d($%.2f) mul=%.2f",
            sourceLabel ? sourceLabel : "?", sourceCents, (double)sourceCents / 100.0,
            bonusCents, (double)bonusCents / 100.0, gCfg.moneyPayoutMultiplier);
        return false;
    }

    Log("[PAYOUT] Applied source=%s src=%d($%.2f) bonus=%d($%.2f) target=%d($%.2f) mul=%.2f",
        sourceLabel ? sourceLabel : "?",
        sourceCents, (double)sourceCents / 100.0,
        bonusCents, (double)bonusCents / 100.0,
        targetCents, (double)targetCents / 100.0,
        gCfg.moneyPayoutMultiplier);

    if (gCfg.hudUiMode != HUD_UI_MODE_LEGACY_TEXT)
    {
        char toast[128];
        _snprintf_s(toast, sizeof(toast), "Poker bonus +$%.2f", (double)bonusCents / 100.0);
        PostHudToast(toast, HUD_TOAST_EVENT_GENERIC, now);
    }
    return true;
}

static bool DrawWatchLine(const char* label, int idx, HudPanelCursor& panel)
{
    if (idx < 0) return true;
    int val = 0;
    bool ok = ReadGlobalInt(idx, val);
    char buf[128];
    if (ok)
        _snprintf_s(buf, sizeof(buf), "%s [%d] = %d", label, idx, val);
    else
        _snprintf_s(buf, sizeof(buf), "%s [%d] = ???", label, idx);
    return DrawPanelLine(panel, buf);
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

        kv.second.lastSeenMs = now;
        UpdateCandidateOcrMatches(kv.second, val, now);

        // Detect change
        if (val != kv.second.last)
        {
            int delta = val - kv.second.last;
            int absDelta = (delta < 0) ? -delta : delta;
            kv.second.changes++;
            kv.second.lastDelta = delta;
            if (MatchesConfiguredBetGridDelta(absDelta))
                kv.second.betStepMatches++;
            else
                kv.second.betStepMismatches++;
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

    PokerPhase phase = gDetectRuntime.phase;
    if (phase != gLastMoneyPhase)
    {
        if (phase == POKER_PHASE_PAYOUT_SETTLEMENT)
            gSettlementSerial++;
        gLastMoneyPhase = phase;
    }

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
                UpdateCandidateOcrMatches(mc, val, now);
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
            else if (kv.second.ocrAnyMatches == 0 && kv.second.ocrPotMatches == 0 && kv.second.changes > 4)
            {
                float cps = CandidateChangesPerSec(kv.second, now);
                if (gCfg.moneyLikelyMaxChangesPerSec > 0.0f && cps > (gCfg.moneyLikelyMaxChangesPerSec * 6.0f))
                    pruneList.push_back(kv.first);
            }
        }
        for (int idx : pruneList)
            gMoneyCands.erase(idx);
    }

    // ---- Log snapshot ----
    if (gCfg.moneyLogEnable && now >= gNextMoneyLogAt)
    {
        gNextMoneyLogAt = now + gCfg.moneyLogIntervalMs;

        std::vector<const MoneyCandidate*> sorted;
        bool usingRanked = BuildSortedCandidates(now, sorted);

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
                gMoneyScanWrapCount, usingRanked ? "ranked" : "all");

            int logN = (std::min)((int)sorted.size(), gCfg.moneyLogTopN);
            for (int i = 0; i < logN; i++)
            {
                float cps = CandidateChangesPerSec(*sorted[i], now);
                float stepRatio = CandidateBetStepRatio(*sorted[i]);
                Log("[MONEY] Cand idx=%d val=%d (~%.2f if cents) changes=%d rate=%.2f/s step=%d/%d ratio=%.2f lastDelta=%+d ocrAny=%d ocrPot=%d ocrPlayer=%d ocrNpc=%d",
                    sorted[i]->idx, sorted[i]->last, (double)sorted[i]->last / 100.0,
                    sorted[i]->changes, cps, sorted[i]->betStepMatches, sorted[i]->betStepMismatches,
                    stepRatio, sorted[i]->lastDelta,
                    sorted[i]->ocrAnyMatches, sorted[i]->ocrPotMatches, sorted[i]->ocrPlayerMatches, sorted[i]->ocrNpcMatches);
            }
        }
    }

    // Auto-lock pot source as soon as OCR-correlation is strong enough.
    {
        std::vector<const MoneyCandidate*> ranked;
        BuildSortedCandidates(now, ranked);
        TryAutoLockPotGlobal(ranked, now);
        TryAutoLockPlayerGlobal(now);
    }

    // ---- Auto payout ----
    if (gCfg.moneyPayoutEnable &&
        inPoker &&
        gDetectRuntime.phase == POKER_PHASE_PAYOUT_SETTLEMENT &&
        gLastDetectScore.confidence >= gCfg.moneyPayoutMinPhaseConf &&
        gSettlementSerial != gLastPaidSettlementSerial &&
        now >= gNextAllowedPayoutAt)
    {
        int sourceCents = 0;
        const char* sourceLabel = "none";
        if (TryGetPayoutSourceCents(now, sourceCents, sourceLabel))
        {
            if (TryApplyPokerPayout(sourceCents, sourceLabel, now))
            {
                gLastPaidSettlementSerial = gSettlementSerial;
                gNextAllowedPayoutAt = now + (DWORD)gCfg.moneyPayoutCooldownMs;
            }
        }
    }

    // ---- Draw overlay ----
    HudPanelCursor panel = MakeHudPanelCursor();
    char buf[256];

    if (gCfg.hudUiMode >= HUD_UI_MODE_HYBRID_PANEL_TOASTS)
    {
        DrawPanelLine(panel, "Poker Scanner");
    }

    // Watch list (known globals)
    if (!DrawWatchLine("Pot", GetEffectivePotGlobalIndex(), panel)) return;
    if (!DrawWatchLine("Stk0", GetEffectivePlayerGlobalIndex(), panel)) return;
    if (!DrawWatchLine("Stk1", gCfg.stackGlobal1, panel)) return;
    if (!DrawWatchLine("Stk2", gCfg.stackGlobal2, panel)) return;
    if (!DrawWatchLine("Stk3", gCfg.stackGlobal3, panel)) return;
    if (!DrawWatchLine("Stk4", gCfg.stackGlobal4, panel)) return;
    if (!DrawWatchLine("Stk5", gCfg.stackGlobal5, panel)) return;

    if (gOcrMoney.mainPotCents > 0)
    {
        _snprintf_s(buf, sizeof(buf), "OCR MainPot = %d ($%.2f)%s", gOcrMoney.mainPotCents, (double)gOcrMoney.mainPotCents / 100.0, IsOcrMoneyFresh(now) ? "" : " [stale]");
        if (!DrawPanelLine(panel, buf))
            return;
    }
    if (gOcrMoney.sidePotCents > 0)
    {
        _snprintf_s(buf, sizeof(buf), "OCR SidePot = %d ($%.2f)%s", gOcrMoney.sidePotCents, (double)gOcrMoney.sidePotCents / 100.0, IsOcrMoneyFresh(now) ? "" : " [stale]");
        if (!DrawPanelLine(panel, buf))
            return;
    }
    if (gOcrMoney.potCents > 0)
    {
        _snprintf_s(buf, sizeof(buf), "OCR Pot = %d ($%.2f) [%s]%s",
            gOcrMoney.potCents, (double)gOcrMoney.potCents / 100.0, OcrPotSourceToString(gOcrMoney.potSource), IsOcrMoneyFresh(now) ? "" : " [stale]");
        if (!DrawPanelLine(panel, buf))
            return;
    }
    if (gOcrMoney.winsCents > 0)
    {
        _snprintf_s(buf, sizeof(buf), "OCR Wins = %d ($%.2f)", gOcrMoney.winsCents, (double)gOcrMoney.winsCents / 100.0);
        if (!DrawPanelLine(panel, buf))
            return;
    }
    if (gOcrMoney.playerCents > 0)
    {
        _snprintf_s(buf, sizeof(buf), "OCR Player = %d ($%.2f)%s",
            gOcrMoney.playerCents, (double)gOcrMoney.playerCents / 100.0, IsOcrMoneyFresh(now) ? "" : " [stale]");
        if (!DrawPanelLine(panel, buf))
            return;
    }
    if (!gOcrMoney.npcAmountsCents.empty())
    {
        _snprintf_s(buf, sizeof(buf), "OCR NPC$ = %s", OcrAmountListSnippet(gOcrMoney.npcAmountsCents, 6).c_str());
        if (!DrawPanelLine(panel, buf))
            return;
    }
    if (!gOcrMoney.amountsCents.empty())
    {
        _snprintf_s(buf, sizeof(buf), "OCR $ = %s", OcrAmountListSnippet(gOcrMoney.amountsCents, 5).c_str());
        if (!DrawPanelLine(panel, buf))
            return;
    }

    int playerCents = 0;
    bool havePlayer = false;
    if (TryReadEffectivePlayerCents(playerCents))
        havePlayer = true;
    else if (gOcrMoney.playerCents > 0 && IsOcrMoneyFresh(now))
    {
        playerCents = gOcrMoney.playerCents;
        havePlayer = true;
    }
    if (havePlayer)
    {
        int boosted = (int)std::llround((double)playerCents * (double)gCfg.moneyOverlayMultiplier);
        _snprintf_s(buf, sizeof(buf), "Player x%.2f => %d ($%.2f)", gCfg.moneyOverlayMultiplier, boosted, (double)boosted / 100.0);
        if (!DrawPanelLine(panel, buf))
            return;
    }

    int potCents = 0;
    bool haveOverlayPot = false;
    if (TryReadEffectivePotCents(potCents))
        haveOverlayPot = true;
    else if (gOcrMoney.potCents > 0 && IsOcrMoneyFresh(now))
    {
        potCents = gOcrMoney.potCents;
        haveOverlayPot = true;
    }
    if (haveOverlayPot)
    {
        int boosted = (int)std::llround((double)potCents * (double)gCfg.moneyOverlayMultiplier);
        _snprintf_s(buf, sizeof(buf), "Pot x%.2f => %d ($%.2f)", gCfg.moneyOverlayMultiplier, boosted, (double)boosted / 100.0);
        if (!DrawPanelLine(panel, buf))
            return;
    }

    if (gCfg.moneyPayoutEnable)
    {
        int sourceCents = 0;
        const char* sourceLabel = "none";
        if (TryGetPayoutSourceCents(now, sourceCents, sourceLabel))
        {
            int target = (int)std::llround((double)sourceCents * (double)gCfg.moneyPayoutMultiplier);
            int bonus = target - sourceCents;
            if (bonus > 0)
            {
                _snprintf_s(buf, sizeof(buf), "Payout %s x%.2f -> +$%.2f",
                    sourceLabel, gCfg.moneyPayoutMultiplier, (double)bonus / 100.0);
                if (!DrawPanelLine(panel, buf))
                    return;
            }
        }
    }

    // Scanner status
    _snprintf_s(buf, sizeof(buf), "Scanner idx=%d/%d cands=%d wraps=%d autoPot=%d autoPlr=%d",
        gMoneyScanCursor, gCfg.moneyScanEnd,
        (int)gMoneyCands.size(), gMoneyScanWrapCount, gAutoPotGlobal, gAutoPlayerGlobal);
    if (!DrawPanelLine(panel, buf))
        return;

    _snprintf_s(buf, sizeof(buf), "BetRule=%s min=$%d step=$%d",
        gCfg.moneyBetStepFilterEnable ? "on" : "off",
        gCfg.moneyBetMinDollars, gCfg.moneyBetStepDollars);
    if (!DrawPanelLine(panel, buf))
        return;

    if (gCfg.hudUiMode != HUD_UI_MODE_LEGACY_TEXT)
    {
        int retryInMs = 0;
        if (gHudToastNativeFailed && now < gHudToastNativeRetryAt)
            retryInMs = (int)(gHudToastNativeRetryAt - now);
        _snprintf_s(buf, sizeof(buf), "HUD toast native=%s fail=%d retryIn=%dms fb=%d",
            gHudToastNativeFailed ? "cooldown" : "ok",
            gHudToastNativeFailCount,
            retryInMs,
            gCfg.hudToastFallbackText);
        if (!DrawPanelLine(panel, buf))
            return;
    }

    // Top N candidates sorted by likely money behavior first.
    std::vector<const MoneyCandidate*> sorted;
    BuildSortedCandidates(now, sorted);

    bool hasOcrAmounts = !gOcrMoney.amountsCents.empty();
    int groupCount = 0;
    if (gOcrMoney.potCents > 0) groupCount++;
    if (gOcrMoney.playerCents > 0) groupCount++;
    if (!gOcrMoney.npcAmountsCents.empty()) groupCount++;
    if (groupCount <= 0) groupCount = 1;
    int maxPerGroup = (std::max)(1, gCfg.moneyTopN / groupCount);
    int shownPot = 0;
    int shownPlayer = 0;
    int shownNpc = 0;

    if (gOcrMoney.potCents > 0)
    {
        for (int i = 0; i < (int)sorted.size(); i++)
        {
            const MoneyCandidate* c = sorted[i];
            if (c->ocrPotMatches <= 0)
                continue;
            _snprintf_s(buf, sizeof(buf), "P%02d idx=%d v=%d($%.2f) pot=%d player=%d d=%+d step=%d/%d",
                shownPot + 1, c->idx, c->last, (double)c->last / 100.0, c->ocrPotMatches, c->ocrPlayerMatches,
                c->lastDelta, c->betStepMatches, c->betStepMismatches);
            if (!DrawPanelLine(panel, buf))
                break;
            shownPot++;
            if (shownPot >= maxPerGroup)
                break;
        }
    }

    if (gOcrMoney.playerCents > 0)
    {
        for (int i = 0; i < (int)sorted.size(); i++)
        {
            const MoneyCandidate* c = sorted[i];
            if (c->ocrPlayerMatches <= 0)
                continue;
            _snprintf_s(buf, sizeof(buf), "U%02d idx=%d v=%d($%.2f) player=%d pot=%d d=%+d step=%d/%d",
                shownPlayer + 1, c->idx, c->last, (double)c->last / 100.0, c->ocrPlayerMatches, c->ocrPotMatches,
                c->lastDelta, c->betStepMatches, c->betStepMismatches);
            if (!DrawPanelLine(panel, buf))
                break;
            shownPlayer++;
            if (shownPlayer >= maxPerGroup)
                break;
        }
    }

    if (!gOcrMoney.npcAmountsCents.empty())
    {
        for (int i = 0; i < (int)sorted.size(); i++)
        {
            const MoneyCandidate* c = sorted[i];
            if (c->ocrNpcMatches <= 0)
                continue;
            if (c->ocrPotMatches > c->ocrNpcMatches * 2)
                continue;
            if (c->ocrPlayerMatches > c->ocrNpcMatches * 2)
                continue;
            _snprintf_s(buf, sizeof(buf), "N%02d idx=%d v=%d($%.2f) npc=%d pot=%d player=%d",
                shownNpc + 1, c->idx, c->last, (double)c->last / 100.0, c->ocrNpcMatches, c->ocrPotMatches, c->ocrPlayerMatches);
            if (!DrawPanelLine(panel, buf))
                break;
            shownNpc++;
            if (shownNpc >= maxPerGroup)
                break;
        }
    }

    if (shownPot == 0 && shownPlayer == 0 && shownNpc == 0 && hasOcrAmounts)
        DrawPanelLine(panel, "Diag globals: no OCR pot/player/npc matches yet");
}

static void InitPaths()
{
    // Get game EXE directory for INI and log paths
    GetModuleFileNameA(nullptr, gIniPath, MAX_PATH);
    char* lastSlash = strrchr(gIniPath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    strcpy_s(gGameDirPath, MAX_PATH, gIniPath);

    strcpy_s(gLogPath, MAX_PATH, gGameDirPath);
    strcat_s(gLogPath, MAX_PATH, "highstakes.log");
    strcpy_s(gIniPath, MAX_PATH, gGameDirPath);
    strcat_s(gIniPath, MAX_PATH, "highstakes.ini");

    char tempPath[MAX_PATH]{ 0 };
    DWORD tn = GetTempPathA(MAX_PATH, tempPath);
    if (tn > 0 && tn < MAX_PATH)
    {
        strcpy_s(gOcrBmpBottomLeftPath, MAX_PATH, tempPath);
        strcat_s(gOcrBmpBottomLeftPath, MAX_PATH, "highstakes_ocr_bl.bmp");
        strcpy_s(gOcrBmpTopRightPath, MAX_PATH, tempPath);
        strcat_s(gOcrBmpTopRightPath, MAX_PATH, "highstakes_ocr_tr.bmp");

        strcpy_s(gOcrOutBaseBottomLeftPath, MAX_PATH, tempPath);
        strcat_s(gOcrOutBaseBottomLeftPath, MAX_PATH, "highstakes_ocr_bl");
        strcpy_s(gOcrOutBaseTopRightPath, MAX_PATH, tempPath);
        strcat_s(gOcrOutBaseTopRightPath, MAX_PATH, "highstakes_ocr_tr");

        strcpy_s(gOcrTxtBottomLeftPath, MAX_PATH, tempPath);
        strcat_s(gOcrTxtBottomLeftPath, MAX_PATH, "highstakes_ocr_bl.txt");
        strcpy_s(gOcrTxtTopRightPath, MAX_PATH, tempPath);
        strcat_s(gOcrTxtTopRightPath, MAX_PATH, "highstakes_ocr_tr.txt");
    }
    else
    {
        strcpy_s(gOcrBmpBottomLeftPath, MAX_PATH, gGameDirPath);
        strcat_s(gOcrBmpBottomLeftPath, MAX_PATH, "highstakes_ocr_bl.bmp");
        strcpy_s(gOcrBmpTopRightPath, MAX_PATH, gGameDirPath);
        strcat_s(gOcrBmpTopRightPath, MAX_PATH, "highstakes_ocr_tr.bmp");

        strcpy_s(gOcrOutBaseBottomLeftPath, MAX_PATH, gGameDirPath);
        strcat_s(gOcrOutBaseBottomLeftPath, MAX_PATH, "highstakes_ocr_bl");
        strcpy_s(gOcrOutBaseTopRightPath, MAX_PATH, gGameDirPath);
        strcat_s(gOcrOutBaseTopRightPath, MAX_PATH, "highstakes_ocr_tr");

        strcpy_s(gOcrTxtBottomLeftPath, MAX_PATH, gGameDirPath);
        strcat_s(gOcrTxtBottomLeftPath, MAX_PATH, "highstakes_ocr_bl.txt");
        strcpy_s(gOcrTxtTopRightPath, MAX_PATH, gGameDirPath);
        strcat_s(gOcrTxtTopRightPath, MAX_PATH, "highstakes_ocr_tr.txt");
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
        if (gCfg.hudUiMode == HUD_UI_MODE_LEGACY_TEXT)
        {
            DrawCenteredText(gLegacyHudMessage.c_str(), 0.5f, 0.02f);
        }
        else
        {
            HudPanelCursor toastFallbackPanel = MakeHudPanelCursor(-1.6f * (gCfg.hudPanelLineStep / 100.0f));
            DrawPanelLine(toastFallbackPanel, gLegacyHudMessage.c_str());
        }
    }

    // Debug overlay
    if (gCfg.debugOverlay)
    {
        HudPanelCursor debugPanel = MakeHudPanelCursor(-2.5f * (gCfg.hudPanelLineStep / 100.0f));
        if (debugPanel.y < 0.02f)
            debugPanel.y = 0.02f;

        char dbg[256];
        _snprintf_s(dbg, sizeof(dbg),
            "inPoker=%d gate=%s phase=%s conf=%.2f",
            inPoker ? 1 : 0,
            gLastDetectScore.gateReason,
            PokerPhaseToString(gLastDetectScore.guessPhase),
            gLastDetectScore.confidence);
        DrawPanelLine(debugPanel, dbg);

        _snprintf_s(dbg, sizeof(dbg),
            "scan=%d pending=%d hits=%d anchors=%d opacity=%.2f payoutAgeMs=%ld payoutHoldMs=%ld",
            gLastDetectInputs.scanOk ? 1 : 0,
            gLastDetectInputs.pending ? 1 : 0,
            gLastDetectInputs.keywordHits,
            gLastDetectInputs.anchorHits,
            gLastDetectScore.opacityHint,
            (long)((gLastPayoutMarkerSeenAt > 0 && now >= gLastPayoutMarkerSeenAt) ? (now - gLastPayoutMarkerSeenAt) : -1L),
            (long)((gPayoutHoldUntilAt > now) ? (gPayoutHoldUntilAt - now) : 0L));
        DrawPanelLine(debugPanel, dbg);
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

