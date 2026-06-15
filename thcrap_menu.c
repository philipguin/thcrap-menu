// Build (MinGW-w64):
// gcc -std=c11 -O2 -Wall -Wextra -mwindows -o thcrap_menu.exe thcrap_menu.c -lshell32 -lgdi32

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <wctype.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

// Minimal XInput ABI used by the launcher. The DLL is loaded dynamically, so
// older MinGW installations do not need to provide xinput.h or an import lib.
#define XUSER_MAX_COUNT              4
#define XINPUT_GAMEPAD_DPAD_UP       0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN     0x0002
#define XINPUT_GAMEPAD_A             0x1000
#define XINPUT_GAMEPAD_B             0x2000

typedef struct XINPUT_GAMEPAD {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD;

typedef struct XINPUT_STATE {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;

#define ID_CONFIG_LIST 1001
#define ID_LAUNCH      1002
#define ID_CANCEL      1003
#define GAMEPAD_TIMER  1

typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);
typedef BOOL (WINAPI *SetProcessDPIAwareFn)(void);

typedef struct ConfigEntry {
    WCHAR *filename;
    WCHAR *display_name;
} ConfigEntry;

static HINSTANCE g_instance;
static HWND g_window;
static HWND g_heading;
static HWND g_list;
static HWND g_help;
static HWND g_launch;
static HWND g_cancel;
static HFONT g_ui_font;
static HFONT g_heading_font;
static WCHAR g_exe_dir[32768];
static WCHAR *g_game_id;
static WCHAR *g_config_prefix;
static ConfigEntry *g_configs;
static size_t g_config_count;
static HMODULE g_xinput_module;
static XInputGetStateFn g_xinput_get_state;
static WORD g_gamepad_buttons;
static DWORD g_repeat_at;
static int g_dpi = 96;
static int g_ui_line_height;
static int g_heading_line_height;
static int g_min_window_width;
static int g_min_window_height;

static int scaled(int value)
{
    return MulDiv(value, g_dpi, 96);
}

static int font_line_height(HWND control, HFONT font, int fallback)
{
    HDC dc = GetDC(control);
    HFONT previous_font;
    TEXTMETRICW metrics;
    int height = scaled(fallback);

    if (dc == NULL) {
        return height;
    }
    previous_font = (HFONT)SelectObject(dc, font);
    if (GetTextMetricsW(dc, &metrics)) {
        height = metrics.tmHeight + metrics.tmExternalLeading;
    }
    SelectObject(dc, previous_font);
    ReleaseDC(control, dc);
    return height;
}

static WCHAR *duplicate_string(const WCHAR *text)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(*text);
    WCHAR *copy = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (copy != NULL) {
        CopyMemory(copy, text, bytes);
    }
    return copy;
}

static int wide_nicmp(const WCHAR *left, const WCHAR *right, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        WCHAR left_char = (WCHAR)towlower(left[i]);
        WCHAR right_char = (WCHAR)towlower(right[i]);

        if (left_char != right_char) {
            return left_char < right_char ? -1 : 1;
        }
        if (left[i] == L'\0') {
            return 0;
        }
    }
    return 0;
}

static int wide_icmp(const WCHAR *left, const WCHAR *right)
{
    size_t left_length = wcslen(left);
    size_t right_length = wcslen(right);
    size_t count = left_length > right_length ? left_length : right_length;
    return wide_nicmp(left, right, count + 1);
}

static void enable_dpi_awareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetProcessDPIAwareFn set_process_dpi_aware;

    if (user32 == NULL) {
        return;
    }
    set_process_dpi_aware = (SetProcessDPIAwareFn)(void *)
        GetProcAddress(user32, "SetProcessDPIAware");
    if (set_process_dpi_aware != NULL) {
        set_process_dpi_aware();
    }
}

static void show_last_error(const WCHAR *message)
{
    WCHAR detail[512] = L"Unknown error.";
    WCHAR output[768];
    DWORD error = GetLastError();

    FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, 0, detail, (DWORD)ARRAY_COUNT(detail), NULL);
    _snwprintf(output, ARRAY_COUNT(output), L"%ls\n\n%ls (error %lu)",
               message, detail, error);
    output[ARRAY_COUNT(output) - 1] = L'\0';
    MessageBoxW(g_window, output, L"thcrap launcher", MB_OK | MB_ICONERROR);
}

static BOOL set_working_directory_to_exe(void)
{
    DWORD length = GetModuleFileNameW(
        NULL, g_exe_dir, (DWORD)ARRAY_COUNT(g_exe_dir));
    WCHAR *slash;

    if (length == 0 || length >= (DWORD)ARRAY_COUNT(g_exe_dir)) {
        return FALSE;
    }

    slash = wcsrchr(g_exe_dir, L'\\');
    if (slash == NULL) {
        SetLastError(ERROR_BAD_PATHNAME);
        return FALSE;
    }
    *slash = L'\0';
    return SetCurrentDirectoryW(g_exe_dir);
}

static BOOL valid_game_id(const WCHAR *game_id)
{
    static const WCHAR invalid[] = L"\\/:*?\"<>|;";
    return game_id[0] != L'\0' && wcslen(game_id) <= 240 &&
           wcspbrk(game_id, invalid) == NULL;
}

static BOOL valid_config_prefix(const WCHAR *prefix)
{
    static const WCHAR invalid[] = L"\\/:*?\"<>|";
    return prefix[0] != L'\0' && wcslen(prefix) <= 240 &&
           wcspbrk(prefix, invalid) == NULL;
}

static BOOL add_config(const WCHAR *filename, size_t prefix_length)
{
    size_t filename_length = wcslen(filename);
    size_t display_length = filename_length - prefix_length - 3;
    ConfigEntry *resized;
    WCHAR *display;
    WCHAR *full_filename;

    if (g_configs == NULL) {
        resized = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                            sizeof(*g_configs));
    } else {
        resized = HeapReAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, g_configs,
            (g_config_count + 1) * sizeof(*g_configs));
    }
    if (resized == NULL) {
        return FALSE;
    }
    g_configs = resized;

    display = HeapAlloc(GetProcessHeap(), 0,
                        (display_length + 1) * sizeof(*display));
    full_filename = duplicate_string(filename);
    if (display == NULL || full_filename == NULL) {
        HeapFree(GetProcessHeap(), 0, display);
        HeapFree(GetProcessHeap(), 0, full_filename);
        return FALSE;
    }

    CopyMemory(display, filename + prefix_length,
               display_length * sizeof(*display));
    display[display_length] = L'\0';
    g_configs[g_config_count].filename = full_filename;
    g_configs[g_config_count].display_name = display;
    ++g_config_count;
    return TRUE;
}

static BOOL find_configs(void)
{
    WIN32_FIND_DATAW find_data;
    HANDLE find_handle;
    WCHAR pattern[32768];
    size_t prefix_length;
    BOOL ok = TRUE;

    if (_snwprintf(pattern, ARRAY_COUNT(pattern), L"%ls\\config\\%ls*.js",
                   g_exe_dir, g_config_prefix) < 0) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    pattern[ARRAY_COUNT(pattern) - 1] = L'\0';
    prefix_length = wcslen(g_config_prefix);

    find_handle = FindFirstFileW(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND ||
            GetLastError() == ERROR_PATH_NOT_FOUND) {
            SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
        return FALSE;
    }

    do {
        size_t length = wcslen(find_data.cFileName);
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            length > prefix_length + 3 &&
            wide_nicmp(find_data.cFileName, g_config_prefix, prefix_length) == 0 &&
            wide_icmp(find_data.cFileName + length - 3, L".js") == 0) {
            if (!add_config(find_data.cFileName, prefix_length)) {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                ok = FALSE;
                break;
            }
        }
    } while (FindNextFileW(find_handle, &find_data));

    if (ok && GetLastError() != ERROR_NO_MORE_FILES) {
        ok = FALSE;
    }
    FindClose(find_handle);
    return ok;
}

static void free_configs(void)
{
    size_t i;
    for (i = 0; i < g_config_count; ++i) {
        HeapFree(GetProcessHeap(), 0, g_configs[i].filename);
        HeapFree(GetProcessHeap(), 0, g_configs[i].display_name);
    }
    HeapFree(GetProcessHeap(), 0, g_configs);
}

static void move_selection(int direction)
{
    LRESULT count = SendMessageW(g_list, LB_GETCOUNT, 0, 0);
    LRESULT selected = SendMessageW(g_list, LB_GETCURSEL, 0, 0);

    if (count <= 0) {
        return;
    }
    if (selected == LB_ERR) {
        selected = 0;
    } else {
        selected += direction;
    }
    if (selected < 0) {
        selected = 0;
    } else if (selected >= count) {
        selected = count - 1;
    }
    SendMessageW(g_list, LB_SETCURSEL, (WPARAM)selected, 0);
    SetFocus(g_list);
}

static void launch_selected(void)
{
    LRESULT selected = SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    ConfigEntry *config;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    WCHAR loader[32768];
    WCHAR *command_line;
    size_t command_chars;

    if (selected == LB_ERR) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    config = (ConfigEntry *)SendMessageW(g_list, LB_GETITEMDATA,
                                         (WPARAM)selected, 0);
    if (config == (ConfigEntry *)LB_ERR) {
        return;
    }

    if (_snwprintf(loader, ARRAY_COUNT(loader), L"%ls\\thcrap_loader.exe",
                   g_exe_dir) < 0) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        show_last_error(L"The thcrap loader path is too long.");
        return;
    }
    loader[ARRAY_COUNT(loader) - 1] = L'\0';

    command_chars = wcslen(loader) + wcslen(config->filename) +
                    wcslen(g_game_id) + 10;
    command_line = HeapAlloc(GetProcessHeap(), 0,
                             command_chars * sizeof(*command_line));
    if (command_line == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        show_last_error(L"Could not prepare the loader command.");
        return;
    }
    _snwprintf(command_line, command_chars, L"\"%ls\" \"%ls\" \"%ls\"",
               loader, config->filename, g_game_id);
    command_line[command_chars - 1] = L'\0';

    startup.cb = sizeof(startup);
    if (!CreateProcessW(loader, command_line, NULL, NULL, FALSE, 0, NULL,
                        g_exe_dir, &startup, &process)) {
        HeapFree(GetProcessHeap(), 0, command_line);
        show_last_error(L"Could not start thcrap_loader.exe.");
        return;
    }

    HeapFree(GetProcessHeap(), 0, command_line);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    DestroyWindow(g_window);
}

static void load_xinput(void)
{
    static const WCHAR *dlls[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"
    };
    size_t i;

    for (i = 0; i < ARRAY_COUNT(dlls); ++i) {
        g_xinput_module = LoadLibraryW(dlls[i]);
        if (g_xinput_module != NULL) {
            g_xinput_get_state = (XInputGetStateFn)(void *)
                GetProcAddress(g_xinput_module, "XInputGetState");
            if (g_xinput_get_state != NULL) {
                return;
            }
            FreeLibrary(g_xinput_module);
            g_xinput_module = NULL;
        }
    }
}

static void poll_gamepad(void)
{
    XINPUT_STATE state;
    DWORD controller;
    WORD buttons = 0;
    WORD pressed;
    WORD direction;
    DWORD now = GetTickCount();

    if (g_xinput_get_state == NULL || GetForegroundWindow() != g_window) {
        g_gamepad_buttons = 0;
        return;
    }

    for (controller = 0; controller < XUSER_MAX_COUNT; ++controller) {
        ZeroMemory(&state, sizeof(state));
        if (g_xinput_get_state(controller, &state) == ERROR_SUCCESS) {
            buttons = state.Gamepad.wButtons;
            break;
        }
    }

    pressed = buttons & (WORD)~g_gamepad_buttons;
    direction = buttons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN);

    if (pressed & XINPUT_GAMEPAD_DPAD_UP) {
        move_selection(-1);
        g_repeat_at = now + 350;
    } else if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) {
        move_selection(1);
        g_repeat_at = now + 350;
    } else if (direction != 0 && (LONG)(now - g_repeat_at) >= 0) {
        move_selection((direction & XINPUT_GAMEPAD_DPAD_UP) ? -1 : 1);
        g_repeat_at = now + 110;
    }

    if (pressed & XINPUT_GAMEPAD_A) {
        launch_selected();
    } else if (pressed & XINPUT_GAMEPAD_B) {
        DestroyWindow(g_window);
    }
    g_gamepad_buttons = buttons;
}

static void layout_controls(HWND window)
{
    RECT client;
    int width;
    int height;
    int margin = scaled(24);
    int button_width = scaled(150);
    int button_height;
    int heading_height;
    int list_top;
    int list_height;
    int help_height;
    int bottom_height;
    int bottom_top;
    int help_width;

    GetClientRect(window, &client);
    width = client.right;
    height = client.bottom;
    heading_height = g_heading_line_height + scaled(8);
    help_height = g_ui_line_height * 2 + scaled(8);
    button_height = g_ui_line_height + scaled(18);
    if (button_height < scaled(46)) {
        button_height = scaled(46);
    }
    bottom_height = help_height > button_height ? help_height : button_height;
    bottom_top = height - margin - bottom_height;
    list_top = scaled(18) + heading_height + scaled(6);
    list_height = bottom_top - scaled(14) - list_top;
    help_width = width - margin * 2 - button_width * 2 - scaled(20);

    MoveWindow(g_heading, margin, scaled(18), width - margin * 2,
               heading_height, TRUE);
    MoveWindow(g_list, margin, list_top, width - margin * 2,
               list_height, TRUE);
    MoveWindow(g_help, margin, bottom_top, help_width, help_height, TRUE);
    MoveWindow(g_launch, width - margin - button_width * 2 - scaled(10),
               bottom_top + (bottom_height - button_height) / 2,
               button_width, button_height, TRUE);
    MoveWindow(g_cancel, width - margin - button_width,
               bottom_top + (bottom_height - button_height) / 2,
               button_width, button_height, TRUE);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                    WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE:
        return 0;
    case WM_SIZE:
        layout_controls(window);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lparam)->ptMinTrackSize.x = g_min_window_width;
        ((MINMAXINFO *)lparam)->ptMinTrackSize.y = g_min_window_height;
        return 0;
    case WM_TIMER:
        if (wparam == GAMEPAD_TIMER) {
            poll_gamepad();
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_LAUNCH ||
            (LOWORD(wparam) == ID_CONFIG_LIST &&
             HIWORD(wparam) == LBN_DBLCLK)) {
            launch_selected();
            return 0;
        }
        if (LOWORD(wparam) == ID_CANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(window, GAMEPAD_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static void center_window(HWND window)
{
    RECT rect;
    MONITORINFO monitor;
    HMONITOR handle;
    int x;
    int y;

    ZeroMemory(&monitor, sizeof(monitor));
    monitor.cbSize = sizeof(monitor);
    GetWindowRect(window, &rect);
    handle = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(handle, &monitor);
    x = monitor.rcWork.left +
        (monitor.rcWork.right - monitor.rcWork.left - (rect.right - rect.left)) / 2;
    y = monitor.rcWork.top +
        (monitor.rcWork.bottom - monitor.rcWork.top - (rect.bottom - rect.top)) / 2;
    SetWindowPos(window, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static BOOL create_main_window(void)
{
    WNDCLASSEXW window_class = {0};
    RECT window_rect = {0, 0, 0, 0};
    WCHAR title[640];
    WCHAR heading[640];
    DWORD style = WS_OVERLAPPEDWINDOW;
    size_t i;

    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = g_instance;
    window_class.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = L"ThcrapMenuWindow";
    if (!RegisterClassExW(&window_class)) {
        return FALSE;
    }

    window_rect.right = scaled(760);
    window_rect.bottom = scaled(500);
    AdjustWindowRectEx(&window_rect, style, FALSE, 0);
    g_min_window_width = window_rect.right - window_rect.left;
    g_min_window_height = window_rect.bottom - window_rect.top;
    _snwprintf(title, ARRAY_COUNT(title), L"thcrap launcher - %ls", g_game_id);
    title[ARRAY_COUNT(title) - 1] = L'\0';
    g_window = CreateWindowExW(
        0, window_class.lpszClassName, title, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        NULL, NULL, g_instance, NULL);
    if (g_window == NULL) {
        return FALSE;
    }

    _snwprintf(heading, ARRAY_COUNT(heading), L"Select a configuration for %ls",
               g_game_id);
    heading[ARRAY_COUNT(heading) - 1] = L'\0';
    g_heading = CreateWindowExW(0, L"STATIC", heading,
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
        g_window, NULL, g_instance, NULL);
    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
        LBS_NOTIFY | LBS_SORT | LBS_NOINTEGRALHEIGHT,
        0, 0, 0, 0, g_window, (HMENU)ID_CONFIG_LIST, g_instance, NULL);
    g_help = CreateWindowExW(0, L"STATIC",
        L"Arrows / D-pad: select\r\nEnter / A: launch    Esc / B: close",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
        g_window, NULL, g_instance, NULL);
    g_launch = CreateWindowExW(0, L"BUTTON", L"Launch",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, g_window, (HMENU)ID_LAUNCH, g_instance, NULL);
    g_cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, g_window, (HMENU)ID_CANCEL, g_instance, NULL);
    if (g_heading == NULL || g_list == NULL || g_help == NULL ||
        g_launch == NULL || g_cancel == NULL) {
        return FALSE;
    }

    g_ui_font = CreateFontW(-scaled(22), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_heading_font = CreateFontW(-scaled(28), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(g_heading, WM_SETFONT, (WPARAM)g_heading_font, TRUE);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(g_help, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(g_launch, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(g_cancel, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    g_ui_line_height = font_line_height(g_help, g_ui_font, 26);
    g_heading_line_height = font_line_height(
        g_heading, g_heading_font, 34);

    for (i = 0; i < g_config_count; ++i) {
        LRESULT index = SendMessageW(g_list, LB_ADDSTRING, 0,
                                     (LPARAM)g_configs[i].display_name);
        if (index != LB_ERR && index != LB_ERRSPACE) {
            SendMessageW(g_list, LB_SETITEMDATA, (WPARAM)index,
                         (LPARAM)&g_configs[i]);
        }
    }
    SendMessageW(g_list, LB_SETCURSEL, 0, 0);
    layout_controls(g_window);
    center_window(g_window);
    load_xinput();
    SetTimer(g_window, GAMEPAD_TIMER, 40, NULL);
    ShowWindow(g_window, SW_SHOW);
    UpdateWindow(g_window);
    SetFocus(g_list);
    return TRUE;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous_instance,
                   LPSTR command_line, int show_command)
{
    int argc;
    WCHAR **argv;
    MSG message;
    HDC screen;
    BOOL message_result;

    (void)previous_instance;
    (void)command_line;
    (void)show_command;
    g_instance = instance;
    enable_dpi_awareness();
    screen = GetDC(NULL);
    if (screen != NULL) {
        g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(NULL, screen);
    }

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == NULL) {
        show_last_error(L"Could not read the command line.");
        return 1;
    }
    if (argc != 3 || !valid_game_id(argv[1]) ||
        !valid_config_prefix(argv[2])) {
        MessageBoxW(NULL,
            L"Usage: thcrap_menu.exe <game_id> <config_prefix>\n\nExample: thcrap_menu.exe th06 th06;",
            L"thcrap launcher", MB_OK | MB_ICONINFORMATION);
        LocalFree(argv);
        return 1;
    }
    g_game_id = duplicate_string(argv[1]);
    g_config_prefix = duplicate_string(argv[2]);
    LocalFree(argv);
    if (g_game_id == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        show_last_error(L"Could not store the game ID.");
        return 1;
    }
    if (g_config_prefix == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        show_last_error(L"Could not store the config prefix.");
        return 1;
    }

    if (!set_working_directory_to_exe()) {
        show_last_error(L"Could not use the launcher directory.");
        HeapFree(GetProcessHeap(), 0, g_game_id);
        HeapFree(GetProcessHeap(), 0, g_config_prefix);
        return 1;
    }
    if (!find_configs()) {
        show_last_error(L"Could not search the config directory.");
        free_configs();
        HeapFree(GetProcessHeap(), 0, g_game_id);
        HeapFree(GetProcessHeap(), 0, g_config_prefix);
        return 1;
    }
    if (g_config_count == 0) {
        WCHAR message_text[768];
        _snwprintf(message_text, ARRAY_COUNT(message_text),
            L"No configs matched:\n\n.\\config\\%ls*.js", g_config_prefix);
        message_text[ARRAY_COUNT(message_text) - 1] = L'\0';
        MessageBoxW(NULL, message_text, L"thcrap launcher",
                    MB_OK | MB_ICONINFORMATION);
        free_configs();
        HeapFree(GetProcessHeap(), 0, g_game_id);
        HeapFree(GetProcessHeap(), 0, g_config_prefix);
        return 1;
    }
    if (!create_main_window()) {
        show_last_error(L"Could not create the launcher window.");
        free_configs();
        HeapFree(GetProcessHeap(), 0, g_game_id);
        HeapFree(GetProcessHeap(), 0, g_config_prefix);
        return 1;
    }

    while ((message_result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            DestroyWindow(g_window);
            continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            launch_selected();
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_xinput_module != NULL) {
        FreeLibrary(g_xinput_module);
    }
    DeleteObject(g_ui_font);
    DeleteObject(g_heading_font);
    free_configs();
    HeapFree(GetProcessHeap(), 0, g_game_id);
    HeapFree(GetProcessHeap(), 0, g_config_prefix);
    return message_result == (BOOL)-1 ? 1 : (int)message.wParam;
}
