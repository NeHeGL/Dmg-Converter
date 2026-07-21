/*
 * gui.cpp — Win32 drag-and-drop GUI for dmgconverter
 * 2026 Jeff Molofee (NeHe)
 *
 * Dark themed window with:
 *  - Drag & drop via IDropTarget (OLE)
 *  - Click-to-browse fallback
 *  - Custom-drawn progress bar
 *  - Worker thread for conversion
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <ole2.h>
#include <shlobj.h>
#include <commdlg.h>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include "dmg-converter.h"

// ── Colours ───────────────────────────────────────────────────────────────────
#define C_BG_DARK    RGB(26,  32, 44)
#define C_BG_PANEL   RGB(45,  55, 72)
#define C_ACCENT     RGB(49, 130,206)
#define C_ACCENT_DK  RGB(43, 108,176)
#define C_BORDER     RGB(74,  85,104)
#define C_DROP_BDR   RGB(99, 179,237)
#define C_TEXT_MAIN  RGB(255,255,255)
#define C_TEXT_DIM   RGB(160,174,192)
#define C_TEXT_STAT  RGB(113,128,150)
#define C_ERROR      RGB(252,129,129)
#define C_TRACK      RGB(61,  74, 92)
#define C_FILL       RGB(43, 108,176)

// ── Custom window messages ────────────────────────────────────────────────────
#define WM_PROGRESS  (WM_USER + 1)   // wParam=pct, lParam=msg ptr (heap str*)
#define WM_CONV_DONE (WM_USER + 2)   // wParam=0 ok / 1 error, lParam=msg ptr
// ── IDropTarget implementation ────────────────────────────────────────────────
static HWND g_hwnd = nullptr;

class DropTarget : public IDropTarget {
    LONG m_ref = 1;
    bool m_allowDrop = false;
public:
    ULONG STDMETHODCALLTYPE AddRef()  override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    static bool hasDmg(IDataObject* pDO) {
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM sm{};
        if (FAILED(pDO->GetData(&fe, &sm))) return false;
        HDROP hDrop = (HDROP)sm.hGlobal;
        UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        bool ok = false;
        for (UINT i = 0; i < n && !ok; ++i) {
            wchar_t buf[MAX_PATH];
            DragQueryFileW(hDrop, i, buf, MAX_PATH);
            std::wstring ws(buf);
            if (ws.size() >= 4 && _wcsicmp(ws.c_str() + ws.size() - 4, L".dmg") == 0)
                ok = true;
        }
        ReleaseStgMedium(&sm);
        return ok;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDO, DWORD, POINTL, DWORD* pdwEffect) override {
        m_allowDrop = hasDmg(pDO);
        *pdwEffect = m_allowDrop ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* pdwEffect) override {
        *pdwEffect = m_allowDrop ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        m_allowDrop = false;
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDO, DWORD, POINTL, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_NONE;
        m_allowDrop = false;
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM sm{};
        if (FAILED(pDO->GetData(&fe, &sm))) return S_OK;
        HDROP hDrop = (HDROP)sm.hGlobal;
        wchar_t buf[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, buf, MAX_PATH)) {
            std::wstring ws(buf);
            if (ws.size() >= 4 && _wcsicmp(ws.c_str()+ws.size()-4, L".dmg") == 0)
                PostMessageW(g_hwnd, WM_DROPFILES, (WPARAM)SysAllocString(ws.c_str()), 0);
        }
        ReleaseStgMedium(&sm);
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }
};
// ── App state ─────────────────────────────────────────────────────────────────
static int        g_progress   = 0;
static bool       g_converting = false;
static bool       g_dragOver   = false;
static COLORREF   g_statusCol  = C_TEXT_STAT;
static wchar_t    g_statusMsg[512] = L"Ready \x2014 drop a DMG file to begin";
static HFONT      g_fontTitle  = nullptr;
static HFONT      g_fontSub    = nullptr;
static HFONT      g_fontSmall  = nullptr;
static HFONT      g_fontIcon   = nullptr;

// UTF-8 → wide
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n-1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
// wide → UTF-8
static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n-1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static void setStatus(const wchar_t* msg, COLORREF col) {
    wcsncpy_s(g_statusMsg, msg, 511);
    g_statusCol = col;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Draw a filled rounded-rect (pill) using GDI
static void drawPill(HDC hdc, HBRUSH br, int x0, int y0, int x1, int y1) {
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, br);
    int r = (y1 - y0);
    RoundRect(hdc, x0, y0, x1, y1, r, r);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(pen);
}

// Draw dashed border rectangle
static void drawDashedRect(HDC hdc, int x0, int y0, int x1, int y1, COLORREF col) {
    HPEN pen = CreatePen(PS_DASH, 2, col);
    HPEN old = (HPEN)SelectObject(hdc, pen);
    HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, nb);
    SetBkMode(hdc, TRANSPARENT);
    RoundRect(hdc, x0, y0, x1, y1, 18, 18);
    SelectObject(hdc, old);
    SelectObject(hdc, ob);
    DeleteObject(pen);
}

static void startConversion(const std::wstring& dmgPath) {
    if (g_converting) return;
    g_converting = true;
    g_progress   = 0;

    std::wstring statusMsg = L"Converting  " + dmgPath.substr(dmgPath.rfind(L'\\')+1) + L"\u2026";
    setStatus(statusMsg.c_str(), C_TEXT_DIM);

    std::string dmgUtf8  = toUtf8(dmgPath);
    std::wstring outPath = dmgPath.substr(0, dmgPath.rfind(L'.')) + L".img";
    std::string  outUtf8 = toUtf8(outPath);

    std::thread([dmgUtf8, outUtf8, outPath]() {
        try {
            convertDmgToIso(dmgUtf8, outUtf8, [](int pct, const std::string& msg) {
                std::wstring* ws = new std::wstring(toWide(msg));
                PostMessageW(g_hwnd, WM_PROGRESS, (WPARAM)pct, (LPARAM)ws);
            });

            std::wstring outName = outPath.substr(outPath.rfind(L'\\')+1);
            std::wstring* ws = new std::wstring(outName);
            PostMessageW(g_hwnd, WM_CONV_DONE, 0, (LPARAM)ws);
        } catch (const std::exception& e) {
            std::wstring* ws = new std::wstring(toWide(e.what()));
            PostMessageW(g_hwnd, WM_CONV_DONE, 1, (LPARAM)ws);
        }
    }).detach();
}

static void onPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    // Double-buffer
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);

    // Background
    HBRUSH bgBr = CreateSolidBrush(C_BG_DARK);
    FillRect(memDC, &rc, bgBr);
    DeleteObject(bgBr);

    SetBkMode(memDC, TRANSPARENT);

    int pad = 24;

    // ── Title ─────────────────────────────────────────────────────────────────
    SelectObject(memDC, g_fontTitle);
    SetTextColor(memDC, C_TEXT_MAIN);
    RECT titleRc = { pad, 20, W-pad, 50 };
    DrawTextW(memDC, L"DMG Converter", -1, &titleRc, DT_LEFT|DT_SINGLELINE|DT_VCENTER);

    SelectObject(memDC, g_fontSub);
    SetTextColor(memDC, C_TEXT_DIM);
    RECT subRc = { pad, 50, W-pad, 72 };
    DrawTextW(memDC, L"Apple Disk Image \x2192 IMG", -1, &subRc, DT_LEFT|DT_SINGLELINE|DT_VCENTER);

    // ── Drop zone ─────────────────────────────────────────────────────────────
    int dzTop = 80, dzBot = H - 80;
    HBRUSH panelBr = CreateSolidBrush(C_BG_PANEL);
    RECT dzRc = { pad, dzTop, W-pad, dzBot };
    HBRUSH old = (HBRUSH)SelectObject(memDC, panelBr);
    HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
    HPEN oldPen = (HPEN)SelectObject(memDC, nullPen);
    RoundRect(memDC, pad, dzTop, W-pad, dzBot, 16, 16);
    SelectObject(memDC, old);
    SelectObject(memDC, oldPen);
    DeleteObject(panelBr);

    // Dashed border
    COLORREF bdrCol = g_dragOver ? C_DROP_BDR : C_BORDER;
    drawDashedRect(memDC, pad+10, dzTop+10, W-pad-10, dzBot-10, bdrCol);

    // Centre content
    int cx = W/2;
    int cy = (dzTop + dzBot) / 2;

    // Icon (folder emoji via Segoe UI Emoji)
    SelectObject(memDC, g_fontIcon);
    SetTextColor(memDC, C_TEXT_MAIN);
    RECT iconRc = { cx-40, cy-60, cx+40, cy-10 };
    DrawTextW(memDC, L"\U0001F4C2", -1, &iconRc, DT_CENTER|DT_SINGLELINE|DT_VCENTER);

    SelectObject(memDC, g_fontTitle);
    SetTextColor(memDC, C_TEXT_MAIN);
    RECT dropRc = { pad+20, cy-8, W-pad-20, cy+24 };
    DrawTextW(memDC, L"Drop your DMG file here", -1, &dropRc, DT_CENTER|DT_SINGLELINE);

    SelectObject(memDC, g_fontSmall);
    SetTextColor(memDC, C_TEXT_DIM);
    RECT hintRc = { pad+20, cy+26, W-pad-20, cy+48 };
    DrawTextW(memDC, L"or click to browse  \x2014  conversion starts automatically", -1, &hintRc, DT_CENTER|DT_SINGLELINE);

    RECT brandRc = { pad+20, cy+56, W-pad-20, cy+76 };
    DrawTextW(memDC, L"2026 Jeff Molofee", -1, &brandRc, DT_CENTER|DT_SINGLELINE);

    // ── Progress bar ──────────────────────────────────────────────────────────
    int barY = H - 60, barH = 14;
    // Track
    HBRUSH trackBr = CreateSolidBrush(C_TRACK);
    RECT barRc = { pad, barY, W-pad, barY+barH };
    HBRUSH ob2 = (HBRUSH)SelectObject(memDC, trackBr);
    HPEN np2 = (HPEN)GetStockObject(NULL_PEN);
    HPEN op2 = (HPEN)SelectObject(memDC, np2);
    RoundRect(memDC, pad, barY, W-pad, barY+barH, barH, barH);
    SelectObject(memDC, ob2); SelectObject(memDC, op2);
    DeleteObject(trackBr);

    // Fill
    if (g_progress > 0) {
        int totalW = W - pad*2;
        int fillW = (int)((long long)totalW * g_progress / 100);
        fillW = std::max(fillW, barH);
        HBRUSH fillBr = CreateSolidBrush(C_FILL);
        HBRUSH ob3 = (HBRUSH)SelectObject(memDC, fillBr);
        HPEN op3 = (HPEN)SelectObject(memDC, (HPEN)GetStockObject(NULL_PEN));
        RoundRect(memDC, pad, barY, pad+fillW, barY+barH, barH, barH);
        SelectObject(memDC, ob3); SelectObject(memDC, op3);
        DeleteObject(fillBr);
    }

    // ── Status text ───────────────────────────────────────────────────────────
    SelectObject(memDC, g_fontSmall);
    SetTextColor(memDC, g_statusCol);
    RECT statRc = { pad, H-44, W-pad, H-20 };
    DrawTextW(memDC, g_statusMsg, -1, &statRc, DT_CENTER|DT_SINGLELINE|DT_VCENTER);

    BitBlt(hdc, 0, 0, W, H, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

static void doBrowse() {
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFilter = L"Apple Disk Image (*.dmg)\0*.dmg\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        startConversion(buf);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
        onPaint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONUP:
        if (!g_converting) doBrowse();
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;

    case WM_DROPFILES: {
        // Sent from DropTarget::Drop via PostMessage with SysAllocString path
        BSTR bs = (BSTR)wParam;
        if (bs && !g_converting)
            startConversion(bs);
        SysFreeString(bs);
        return 0;
    }

    case WM_PROGRESS: {
        int pct = (int)wParam;
        std::wstring* ws = (std::wstring*)lParam;
        g_progress = pct;
        setStatus(ws->c_str(), C_TEXT_DIM);
        delete ws;
        return 0;
    }

    case WM_CONV_DONE: {
        g_converting = false;
        std::wstring* ws = (std::wstring*)lParam;
        if (wParam == 0) {
            g_progress = 100;
            std::wstring statusText = L"Done \x2014  " + *ws;
            setStatus(statusText.c_str(), C_TEXT_MAIN);
            MessageBoxW(hwnd, (L"Conversion complete!\n\nSaved to:\n" + *ws).c_str(),
                        L"Done", MB_OK|MB_ICONINFORMATION);
        } else {
            setStatus(ws->c_str(), C_ERROR);
            MessageBoxW(hwnd, ws->c_str(), L"Conversion Failed", MB_OK|MB_ICONERROR);
        }
        delete ws;
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
// ── RunGui ────────────────────────────────────────────────────────────────────
int RunGui(HINSTANCE hInstance) {
    OleInitialize(nullptr);

    // Create fonts
    g_fontTitle = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontSub   = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontIcon  = CreateFontW(-36, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Emoji");

    // Load application icon from embedded resources (resource ID 1, defined in app.rc)
    HICON hIconBig   = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1),
                                         IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR);
    HICON hIconSmall = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1),
                                         IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    // Fall back to system default if resource not found
    if (!hIconBig)   hIconBig   = LoadIconW(nullptr, IDI_APPLICATION);
    if (!hIconSmall) hIconSmall = LoadIconW(nullptr, IDI_APPLICATION);

    // Register window class
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = hIconBig;
    wc.hIconSm       = hIconSmall;
    wc.hCursor       = LoadCursorW(nullptr, IDC_HAND);
    wc.hbrBackground = CreateSolidBrush(C_BG_DARK);
    wc.lpszClassName = L"DmgConverterWnd";
    RegisterClassExW(&wc);

    // Create window
    RECT wr = { 0, 0, 540, 460 };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(
        0, L"DmgConverterWnd", L"DmgConverter",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) { OleUninitialize(); return 1; }

    // Explicitly set the icons on the window (taskbar + title bar)
    SendMessageW(g_hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIconBig);
    SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

    // Register OLE drop target
    DropTarget* dt = new DropTarget();
    RegisterDragDrop(g_hwnd, dt);
    dt->Release();

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RevokeDragDrop(g_hwnd);
    DeleteObject(g_fontTitle);
    DeleteObject(g_fontSub);
    DeleteObject(g_fontSmall);
    DeleteObject(g_fontIcon);
    OleUninitialize();
    return (int)msg.wParam;
}
