/*
 * ld2pico.exe: the window around the ld2pico core. The conversion runs on a
 * worker thread and reports through posted messages; the window only collects
 * the two paths and shows the progress and the log.
 */
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <dbt.h>
#include <process.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "ldp_core.h"
#include "resource.h"

#ifndef LDP_VERSION
#define LDP_VERSION "dev"
#endif
#define WIDEN_(s) L##s
#define WIDEN(s) WIDEN_(s)
#define LDP_VERSION_W WIDEN(LDP_VERSION)

#define WM_APP_LOG (WM_APP + 1)      /* lParam: malloc'd text */
#define WM_APP_PROGRESS (WM_APP + 2) /* wParam: pictures done, lParam: total */
#define WM_APP_DONE (WM_APP + 3)     /* wParam: ldp_convert's result */

#define OUT_SUBDIR L"\\roms\\arcade\\DLAIR"

/* How each control follows the window when it is resized. */
enum
{
    MOVE_X = 1,
    MOVE_Y = 2,
    GROW_X = 4,
    GROW_Y = 8
};
static const struct
{
    int id, how;
} anchors[] = {
    {IDC_CHD, GROW_X},
    {IDC_CHD_BROWSE, MOVE_X},
    {IDC_OUT, GROW_X},
    {IDC_OUT_BROWSE, MOVE_X},
    {IDC_LOG, GROW_X | GROW_Y},
    {IDC_STATUS, MOVE_Y | GROW_X},
    {IDC_PROGRESS, MOVE_Y | GROW_X},
    {IDOK, MOVE_X | MOVE_Y},
    {IDCANCEL, MOVE_X | MOVE_Y},
};
#define NANCHORS ((int)(sizeof anchors / sizeof anchors[0]))

typedef struct
{
    char *chd, *pld;
} job;

static struct
{
    HWND dlg;
    int ready;
    int running;
    int closing;
    volatile LONG cancel;
    ULONGLONG started; /* GetTickCount64 at the start of the conversion */
    wchar_t pld[MAX_PATH * 2];
    RECT rects[NANCHORS];
    SIZE client0;
    POINT min_track;
} app;

/* ------------------------------------------------------------------------- */
/* Text                                                                      */
/* ------------------------------------------------------------------------- */

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
    if (w)
    {
        w[0] = 0;
        if (n > 0)
            MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    }
    return w;
}

static char *narrow(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *c = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (c)
    {
        c[0] = 0;
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, s, -1, c, n, NULL, NULL);
    }
    return c;
}

/* ------------------------------------------------------------------------- */
/* Worker thread                                                             */
/* ------------------------------------------------------------------------- */

static void hook_log(void *ctx, const char *line)
{
    (void)ctx;
    size_t n = strlen(line);
    char *s = (char *)malloc(n + 3);
    if (!s)
        return;
    memcpy(s, line, n);
    memcpy(s + n, "\r\n", 3);
    wchar_t *w = widen(s);
    free(s);
    if (w && !PostMessageW(app.dlg, WM_APP_LOG, 0, (LPARAM)w))
        free(w);
}

static void hook_progress(void *ctx, int done, int total)
{
    (void)ctx;
    PostMessageW(app.dlg, WM_APP_PROGRESS, (WPARAM)done, (LPARAM)total);
}

static int hook_cancelled(void *ctx)
{
    (void)ctx;
    return app.cancel != 0;
}

static unsigned __stdcall worker(void *arg)
{
    job *j = (job *)arg;
    ldp_settings s;
    ldp_default_settings(&s);
    const ldp_hooks hooks = {NULL, hook_log, hook_progress, hook_cancelled};
    /* a conversion takes minutes on every processor: let the PC stay usable */
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    const int rc = ldp_convert(j->chd, j->pld, &s, &hooks);
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    PostMessageW(app.dlg, WM_APP_DONE, (WPARAM)rc, 0);
    free(j->chd);
    free(j->pld);
    free(j);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Controls                                                                  */
/* ------------------------------------------------------------------------- */

static HWND item(int id)
{
    return GetDlgItem(app.dlg, id);
}

static void log_text(const wchar_t *s)
{
    HWND e = item(IDC_LOG);
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)s);
}

/* A path typed or pasted into a field, without surrounding blanks or the
   quotes Explorer's "Copy as path" adds, and without a trailing backslash. */
static void field_path(int id, wchar_t *out, int size)
{
    wchar_t text[MAX_PATH * 2];
    GetDlgItemTextW(app.dlg, id, text, MAX_PATH * 2);
    wchar_t *s = text;
    while (*s == L' ' || *s == L'"')
        s++;
    size_t n = wcslen(s);
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'"'))
        s[--n] = 0;
    out[0] = 0;
    if (*s && !GetFullPathNameW(s, (DWORD)size, out, NULL))
        swprintf(out, (size_t)size, L"%ls", s);
    n = wcslen(out);
    if (n > 3 && out[n - 1] == L'\\')
        out[n - 1] = 0;
}

static int is_dir(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int is_file(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* the folder part of a path */
static void folder_of(const wchar_t *path, wchar_t *out, size_t size)
{
    swprintf(out, size, L"%ls", path);
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash)
        *(slash == out + 2 ? slash + 1 : slash) = 0;
}

/* Fills the folder list: the DLAIR folder on every SD card that has a
   \roms\arcade folder (the cards of this family), and the folder of the
   laserdisc image. With choose set, or when the field is empty, picks the
   first card, else the image's folder. */
static void fill_folders(int choose)
{
    HWND combo = item(IDC_OUT);
    wchar_t keep[MAX_PATH * 2];
    GetWindowTextW(combo, keep, MAX_PATH * 2);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);

    DWORD mask = GetLogicalDrives();
    for (int d = 2; d < 26; d++)
    {
        wchar_t root[4] = {(wchar_t)(L'A' + d), L':', L'\\', 0};
        if (!(mask & (1u << d)) || GetDriveTypeW(root) != DRIVE_REMOVABLE)
            continue;
        wchar_t arcade[32], dlair[32];
        swprintf(arcade, 32, L"%c:\\roms\\arcade", root[0]);
        swprintf(dlair, 32, L"%c:" OUT_SUBDIR, root[0]);
        if (is_dir(arcade))
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)dlair);
    }
    wchar_t chd[MAX_PATH * 2], dir[MAX_PATH * 2];
    field_path(IDC_CHD, chd, MAX_PATH * 2);
    if (*chd)
    {
        folder_of(chd, dir, MAX_PATH * 2);
        if (SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)dir) == CB_ERR)
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)dir);
    }
    if (!choose && *keep)
    {
        SetWindowTextW(combo, keep);
        return;
    }
    if (SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0)
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

static void update_buttons(void)
{
    wchar_t chd[MAX_PATH * 2], out[MAX_PATH * 2];
    field_path(IDC_CHD, chd, MAX_PATH * 2);
    field_path(IDC_OUT, out, MAX_PATH * 2);
    EnableWindow(item(IDOK), !app.running && *chd && *out);
}

static void set_running(int running)
{
    app.running = running;
    static const int inputs[] = {IDC_CHD, IDC_CHD_BROWSE, IDC_OUT, IDC_OUT_BROWSE};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++)
        EnableWindow(item(inputs[i]), !running);
    SetDlgItemTextW(app.dlg, IDCANCEL, running ? L"Cancel" : L"Close");
    EnableWindow(item(IDCANCEL), TRUE);
    update_buttons();
}

static void set_chd(const wchar_t *path)
{
    SetDlgItemTextW(app.dlg, IDC_CHD, path);
    wchar_t keep[MAX_PATH * 2];
    GetWindowTextW(item(IDC_OUT), keep, MAX_PATH * 2);
    fill_folders(*keep == 0);
    update_buttons();
}

static void convert(void)
{
    wchar_t chd[MAX_PATH * 2], out[MAX_PATH * 2];
    field_path(IDC_CHD, chd, MAX_PATH * 2);
    field_path(IDC_OUT, out, MAX_PATH * 2);
    if (!is_file(chd))
    {
        MessageBoxW(app.dlg, L"The laserdisc image cannot be found.", L"ld2pico", MB_ICONWARNING);
        return;
    }
    if (!is_dir(out))
    {
        int r = SHCreateDirectoryExW(app.dlg, out, NULL);
        if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS)
        {
            MessageBoxW(app.dlg, L"The folder for lair.pld cannot be created.", L"ld2pico", MB_ICONWARNING);
            return;
        }
    }
    swprintf(app.pld, MAX_PATH * 2, L"%ls\\lair.pld", out);
    if (is_file(app.pld))
    {
        wchar_t q[MAX_PATH * 2 + 80];
        swprintf(q, MAX_PATH * 2 + 80, L"%ls already exists. Replace it?", app.pld);
        if (MessageBoxW(app.dlg, q, L"ld2pico", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
            return;
    }

    job *j = (job *)calloc(1, sizeof(job));
    if (!j)
        return;
    j->chd = narrow(chd);
    j->pld = narrow(app.pld);
    app.cancel = 0;
    app.started = GetTickCount64();
    SendMessageW(item(IDC_PROGRESS), PBM_SETPOS, 0, 0);
    SetDlgItemTextW(app.dlg, IDC_STATUS, L"Reading the laserdisc image...");
    log_text(L"\r\n");
    set_running(1);
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, worker, j, 0, NULL);
    if (h)
    {
        CloseHandle(h);
    }
    else
    {
        free(j->chd);
        free(j->pld);
        free(j);
        set_running(0);
    }
}

static void show_progress(int done, int total)
{
    if (total <= 0)
        return;
    SendMessageW(item(IDC_PROGRESS), PBM_SETPOS, (WPARAM)((long long)done * 1000 / total), 0);
    wchar_t text[160];
    const double secs = (GetTickCount64() - app.started) / 1000.0;
    if (done > total / 50 && done < total)
    {
        const int left = (int)(secs * (total - done) / done + 0.5);
        if (left >= 90)
            swprintf(text, 160, L"%d of %d pictures, about %d minutes left", done, total, (left + 30) / 60);
        else
            swprintf(text, 160, L"%d of %d pictures, less than %d seconds left", done, total,
                     ((left + 9) / 10) * 10 + 10);
    }
    else
    {
        swprintf(text, 160, L"%d of %d pictures", done, total);
    }
    SetDlgItemTextW(app.dlg, IDC_STATUS, text);
}

static void done(int rc)
{
    set_running(0);
    if (rc == 0)
    {
        SendMessageW(item(IDC_PROGRESS), PBM_SETPOS, 1000, 0);
        SetDlgItemTextW(app.dlg, IDC_STATUS, L"Done.");
        wchar_t line[MAX_PATH * 2 + 300];
        size_t n = wcslen(app.pld), m = wcslen(OUT_SUBDIR L"\\lair.pld");
        if (n > m && !_wcsicmp(app.pld + n - m, OUT_SUBDIR L"\\lair.pld"))
            swprintf(line, MAX_PATH * 2 + 300,
                     L"\r\n%ls is ready on the SD card. MAME's dlair.zip goes into the same folder.\r\n",
                     app.pld);
        else
            swprintf(line, MAX_PATH * 2 + 300,
                     L"\r\n%ls is ready. Copy it, together with MAME's dlair.zip, to the folder "
                     L"\\roms\\arcade\\DLAIR on the SD card.\r\n",
                     app.pld);
        log_text(line);
    }
    else
    {
        SetDlgItemTextW(app.dlg, IDC_STATUS, rc == 2 ? L"Cancelled." : L"The conversion failed: see the log.");
    }
    if (app.closing)
        EndDialog(app.dlg, 0);
}

static void close_or_cancel(void)
{
    if (!app.running)
    {
        EndDialog(app.dlg, 0);
        return;
    }
    app.cancel = 1;
    SetDlgItemTextW(app.dlg, IDCANCEL, L"Cancelling...");
    EnableWindow(item(IDCANCEL), FALSE);
}

static void browse_chd(void)
{
    IFileOpenDialog *d;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
                                (void **)&d)))
        return;
    DWORD opts;
    IFileOpenDialog_GetOptions(d, &opts);
    IFileOpenDialog_SetOptions(d, opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    IFileOpenDialog_SetTitle(d, L"Choose MAME's laserdisc image of Dragon's Lair");
    const COMDLG_FILTERSPEC types[] = {{L"Laserdisc image (*.chd)", L"*.chd"}, {L"All files", L"*.*"}};
    IFileOpenDialog_SetFileTypes(d, 2, types);
    wchar_t current[MAX_PATH * 2], dir[MAX_PATH * 2];
    field_path(IDC_CHD, current, MAX_PATH * 2);
    if (*current)
    {
        folder_of(current, dir, MAX_PATH * 2);
        IShellItem *folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir, NULL, &IID_IShellItem, (void **)&folder)))
        {
            IFileOpenDialog_SetFolder(d, folder);
            IShellItem_Release(folder);
        }
    }
    IShellItem *result;
    if (SUCCEEDED(IFileOpenDialog_Show(d, app.dlg)) && SUCCEEDED(IFileOpenDialog_GetResult(d, &result)))
    {
        wchar_t *path;
        if (SUCCEEDED(IShellItem_GetDisplayName(result, SIGDN_FILESYSPATH, &path)))
        {
            set_chd(path);
            CoTaskMemFree(path);
        }
        IShellItem_Release(result);
    }
    IFileOpenDialog_Release(d);
}

static void browse_out(void)
{
    IFileOpenDialog *d;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
                                (void **)&d)))
        return;
    DWORD opts;
    IFileOpenDialog_GetOptions(d, &opts);
    IFileOpenDialog_SetOptions(d, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(d, L"Choose the folder for lair.pld");
    wchar_t current[MAX_PATH * 2];
    field_path(IDC_OUT, current, MAX_PATH * 2);
    IShellItem *folder;
    if (*current && is_dir(current) &&
        SUCCEEDED(SHCreateItemFromParsingName(current, NULL, &IID_IShellItem, (void **)&folder)))
    {
        IFileOpenDialog_SetFolder(d, folder);
        IShellItem_Release(folder);
    }
    IShellItem *result;
    if (SUCCEEDED(IFileOpenDialog_Show(d, app.dlg)) && SUCCEEDED(IFileOpenDialog_GetResult(d, &result)))
    {
        wchar_t *path;
        if (SUCCEEDED(IShellItem_GetDisplayName(result, SIGDN_FILESYSPATH, &path)))
        {
            SetDlgItemTextW(app.dlg, IDC_OUT, path);
            CoTaskMemFree(path);
            update_buttons();
        }
        IShellItem_Release(result);
    }
    IFileOpenDialog_Release(d);
}

/* ------------------------------------------------------------------------- */
/* Set-up and layout                                                         */
/* ------------------------------------------------------------------------- */

static void layout(void)
{
    RECT rc;
    GetClientRect(app.dlg, &rc);
    int dx = rc.right - app.client0.cx, dy = rc.bottom - app.client0.cy;
    HDWP dwp = BeginDeferWindowPos(NANCHORS);
    for (int i = 0; i < NANCHORS && dwp; i++)
    {
        RECT r = app.rects[i];
        int x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
        if (anchors[i].how & MOVE_X)
            x += dx;
        if (anchors[i].how & MOVE_Y)
            y += dy;
        if (anchors[i].how & GROW_X)
            w += dx;
        if (anchors[i].how & GROW_Y)
            h += dy;
        if (anchors[i].id == IDC_OUT)
            h += 200; /* a combo box's height includes its list */
        dwp = DeferWindowPos(dwp, item(anchors[i].id), NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (dwp)
        EndDeferWindowPos(dwp);
}

static void init_dialog(HWND dlg, const wchar_t *arg)
{
    app.dlg = dlg;
    SetWindowTextW(dlg, L"ld2pico " LDP_VERSION_W L" - Dragon's Lair laserdisc for picoDragonsLair");
    HINSTANCE inst = GetModuleHandleW(NULL);
    SendMessageW(dlg, WM_SETICON, ICON_BIG,
                 (LPARAM)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                    GetSystemMetrics(SM_CYICON), 0));
    SendMessageW(dlg, WM_SETICON, ICON_SMALL,
                 (LPARAM)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0));

    RECT rc;
    GetClientRect(dlg, &rc);
    app.client0.cx = rc.right;
    app.client0.cy = rc.bottom;
    for (int i = 0; i < NANCHORS; i++)
    {
        GetWindowRect(item(anchors[i].id), &app.rects[i]);
        MapWindowPoints(NULL, dlg, (POINT *)&app.rects[i], 2);
    }
    GetWindowRect(dlg, &rc);
    app.min_track.x = rc.right - rc.left;
    app.min_track.y = rc.bottom - rc.top;

    SendMessageW(item(IDC_LOG), EM_SETLIMITTEXT, 0, 0);
    SendMessageW(item(IDC_PROGRESS), PBM_SETRANGE32, 0, 1000);
    SendMessageW(item(IDC_CHD), EM_SETCUEBANNER, TRUE, (LPARAM)L"MAME's dlair.chd");
    SendMessageW(item(IDC_OUT), CB_SETCUEBANNER, 0, (LPARAM)L"The SD card's \\roms\\arcade\\DLAIR, or any folder");
    DragAcceptFiles(dlg, TRUE);

    log_text(L"ld2pico converts MAME's laserdisc image of Dragon's Lair (dlair.chd) into lair.pld, the "
             L"laserdisc file picoDragonsLair plays from the SD card. It takes a few minutes and writes "
             L"about 260 MB.\r\n\r\n"
             L"The laserdisc is copyright Cinematronics and Don Bluth and is not included with this "
             L"program.\r\n");

    fill_folders(1);
    if (arg && *arg)
        set_chd(arg);
    update_buttons();
    app.ready = 1;
}

static INT_PTR CALLBACK dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        init_dialog(dlg, (const wchar_t *)lp);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDOK:
            if (IsWindowEnabled(item(IDOK)))
                convert();
            return TRUE;
        case IDCANCEL:
            close_or_cancel();
            return TRUE;
        case IDC_CHD_BROWSE:
            browse_chd();
            return TRUE;
        case IDC_OUT_BROWSE:
            browse_out();
            return TRUE;
        case IDC_CHD:
            if (HIWORD(wp) == EN_CHANGE)
                update_buttons();
            return TRUE;
        case IDC_OUT:
            if (HIWORD(wp) == CBN_EDITCHANGE || HIWORD(wp) == CBN_SELCHANGE)
                PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(0, 0), 0); /* update after the edit field */
            return TRUE;
        case 0:
            update_buttons();
            return TRUE;
        }
        break;

    case WM_DROPFILES:
    {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH * 2];
        if (!app.running && DragQueryFileW(drop, 0, path, MAX_PATH * 2))
            set_chd(path);
        DragFinish(drop);
        return TRUE;
    }

    case WM_DEVICECHANGE:
        if ((wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) && !app.running)
        {
            fill_folders(0);
            update_buttons();
        }
        return TRUE;

    case WM_APP_LOG:
        log_text((const wchar_t *)lp);
        free((void *)lp);
        return TRUE;

    case WM_APP_PROGRESS:
        show_progress((int)wp, (int)lp);
        return TRUE;

    case WM_APP_DONE:
        done((int)wp);
        return TRUE;

    case WM_CLOSE:
        /* The window's close button: a running conversion is cancelled first. */
        if (app.running)
        {
            app.closing = 1;
            close_or_cancel();
        }
        else
        {
            EndDialog(dlg, 0);
        }
        return TRUE;

    case WM_SIZE:
        if (app.ready)
            layout();
        return TRUE;

    case WM_GETMINMAXINFO:
        if (app.ready)
            ((MINMAXINFO *)lp)->ptMinTrackSize = app.min_track;
        return TRUE;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev, (void)show;
    /* No "insert a disk" boxes for card readers without a card. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    /* a laserdisc image dropped onto the program's icon */
    wchar_t arg[MAX_PATH * 2] = L"";
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1)
        swprintf(arg, MAX_PATH * 2, L"%ls", argv[1]);
    if (argv)
        LocalFree(argv);
    (void)cmd;

    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, dlg_proc, (LPARAM)arg);
    CoUninitialize();
    /* ExitProcess also ends a conversion that may still be stopping. */
    ExitProcess(0);
}
