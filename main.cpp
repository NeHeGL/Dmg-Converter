/*
 * main.cpp — Entry point for dmgconverter
 * No args  → GUI mode (no console window)
 * Any args → CLI mode (uses existing console)
 *
 * 2026 Jeff Molofee (NeHe)
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include "dmg-converter.h"

// Declared in gui.cpp
int RunGui(HINSTANCE hInstance);

// ── CLI helpers ───────────────────────────────────────────────────────────────

// When a GUI-subsystem exe attaches to the parent console with
// AttachConsole(), cmd.exe has already printed its prompt and is blocked
// waiting for the child process to finish.  It won't re-issue a new prompt
// until it sees input in the console input buffer.
//
// We inject a synthetic Enter keypress into the console input buffer.
// Because cmd is blocked on WaitForSingleObject waiting for our process to
// exit, it won't actually read that Enter until AFTER we return from WinMain
// and the process truly exits.  So the ordering is always:
//   1. Our output (written synchronously to the screen buffer)
//   2. Process exits
//   3. Cmd wakes up, reads the injected Enter, re-prompts
//
// We open the CONIN$ handle before doing anything else so we have a valid
// kernel object reference regardless of what happens later.
static HANDLE g_hConIn = INVALID_HANDLE_VALUE;

static void injectEnterKeystroke() {
    if (g_hConIn == INVALID_HANDLE_VALUE) return;

    INPUT_RECORD ir[2] = {};
    ir[0].EventType                        = KEY_EVENT;
    ir[0].Event.KeyEvent.bKeyDown          = TRUE;
    ir[0].Event.KeyEvent.wRepeatCount      = 1;
    ir[0].Event.KeyEvent.wVirtualKeyCode   = VK_RETURN;
    ir[0].Event.KeyEvent.wVirtualScanCode  = static_cast<WORD>(
        MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC));
    ir[0].Event.KeyEvent.uChar.UnicodeChar = L'\r';
    ir[0].Event.KeyEvent.dwControlKeyState = 0;

    ir[1]                                  = ir[0];
    ir[1].Event.KeyEvent.bKeyDown          = FALSE;

    DWORD written = 0;
    WriteConsoleInputW(g_hConIn, ir, 2, &written);
    CloseHandle(g_hConIn);
    g_hConIn = INVALID_HANDLE_VALUE;
}

static void printUsage(const char* exe) {
    printf("Usage:\n");
    printf("  %s input.dmg                 Convert to .img\n", exe);
    printf("  %s input.dmg output.img      Convert to specified output\n", exe);
    printf("  %s -l input.dmg              List partitions\n", exe);
    printf("  %s -h                        Show this help\n", exe);
}

static std::string replaceExt(const std::string& path, const std::string& newExt) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return path + newExt;
    return path.substr(0, dot) + newExt;
}

// ── WinMain ───────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int) {
    // Parse command line into argc/argv
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // Convert wide args to UTF-8 strings
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
        args.push_back(s);
    }
    LocalFree(wargv);

    // No arguments (just the exe name) → GUI
    if (argc <= 1)
        return RunGui(hInstance);

    // Has arguments → CLI mode.
    // We're a GUI-subsystem exe (no console by default). Attach to parent console
    // (the terminal that launched us), or allocate a new one.
    bool attachedToParent = AttachConsole(ATTACH_PARENT_PROCESS);
    if (!attachedToParent)
        AllocConsole();

    // Reopen CRT streams to the console.
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    freopen_s(&dummy, "CONIN$",  "r", stdin);

    // Grab the CONIN$ handle immediately so we can inject Enter at the end.
    // cmd is blocked waiting for us; it will only process the injected Enter
    // after our process exits — so output always appears before the prompt.
    if (attachedToParent) {
        g_hConIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        // cmd has already rendered its next prompt on the current line.
        // Overwrite it with spaces then carriage-return back to column 0,
        // so the first line of our output starts cleanly on a fresh line.
        printf("\r%*s\r", 79, "");
    }

    // ── Parse arguments ───────────────────────────────────────────────────────
    bool listMode = false;
    std::string inputPath, outputPath;

    for (int i = 1; i < argc; ++i) {
        const std::string& a = args[i];
        if (a == "-l" || a == "--list") {
            listMode = true;
        } else if (a == "-h" || a == "--help") {
            printUsage(args[0].c_str());
            fflush(stdout);
            if (attachedToParent) injectEnterKeystroke();
            return 0;
        } else if (inputPath.empty()) {
            inputPath = a;
        } else if (outputPath.empty()) {
            outputPath = a;
        }
    }

    if (inputPath.empty()) {
        printUsage(args[0].c_str());
        fflush(stdout);
        if (attachedToParent) injectEnterKeystroke();
        return 1;
    }

    // ── List mode ─────────────────────────────────────────────────────────────
    if (listMode) {
        try {
            auto parts = listPartitions(inputPath);
            printf("Partitions in %s:\n\n", inputPath.c_str());
            printf("  %-4s  %10s  %10s  %6s  %s\n", "#", "Size (MB)", "Sectors", "Runs", "Name");
            printf("  %-4s  %10s  %10s  %6s  %s\n", "----", "----------", "----------", "------", "----------------------------------------");
            for (auto& p : parts)
                printf("  %-4d  %10.1f  %10llu  %6d  %s\n",
                       p.index, p.sizeMB, (unsigned long long)p.sectors, p.runs, p.name.c_str());
            printf("\n");
        } catch (const std::exception& e) {
            fprintf(stderr, "Error: %s\n", e.what());
            fflush(stderr);
            if (attachedToParent) injectEnterKeystroke();
            return 1;
        }
        fflush(stdout);
        if (attachedToParent) injectEnterKeystroke();
        return 0;
    }

    // ── Convert mode ──────────────────────────────────────────────────────────
    if (outputPath.empty())
        outputPath = replaceExt(inputPath, ".img");

    try {
        convertDmgToIso(inputPath, outputPath, [](int pct, const std::string& msg) {
            printf("[%3d%%] %s\n", pct, msg.c_str());
            fflush(stdout);
        });
        printf("\nDone! Output: %s\n", outputPath.c_str());
        fflush(stdout);

        if (attachedToParent) injectEnterKeystroke();
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        fflush(stderr);
        if (attachedToParent) injectEnterKeystroke();
        return 1;
    }
}
