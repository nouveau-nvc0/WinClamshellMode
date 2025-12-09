#include <windows.h>
#include <powrprof.h>
#include <shlobj.h>
#include <stdio.h>
#include <io.h>
#include <Dbt.h>
#include <initguid.h>
#include <Ntddvdeo.h>

#pragma comment(lib, "powrprof.lib")

// --- Constants ---
#define ID_TRAY_EXIT 1001
#define ID_TRAY_SET_DISPLAY 1002
#define ID_COMBOBOX 1003
#define ID_OK_BUTTON 1004

#define WINDOW_NAME L"Disable sleep when lid closed with external display connected"
#define INI_FILE_NAME L"ClamshellMode.ini"
#define INI_KEY_DISPLAY L"InternalDisplayID"

// --- Global Variables ---
static WCHAR INI_PATH[MAX_PATH] = L"";
static WCHAR INTERNAL_DISPLAY_ID[MAX_PATH] = L"";

// --- Helper Functions ---

// Load Display ID from INI file
static void LoadSettingsFromIni() {
    GetPrivateProfileStringW(L"Settings", INI_KEY_DISPLAY, L"", INTERNAL_DISPLAY_ID, MAX_PATH, INI_PATH);
}

// Check if a specific display device is currently active
static BOOL IsDisplayDeviceActive(const WCHAR* deviceID) {
    DISPLAY_DEVICE device = { .cb = sizeof(DISPLAY_DEVICE) };
    DWORD i = 0;
    while (EnumDisplayDevicesW(NULL, i++, &device, 0) != 0) {
        DISPLAY_DEVICE device2 = { .cb = sizeof(DISPLAY_DEVICE) };
        DWORD j = 0;
        while (EnumDisplayDevicesW(device.DeviceName, j++, &device2, 0) != 0) {
            if (wcscmp(device2.DeviceID, deviceID) == 0) {
                return (device2.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0;
            }
        }
    }
    return FALSE;
}

// Save Display ID to INI file
static DWORD SaveSettingsToIni(const WCHAR* displayID) {
    // Verify the display ID is active only if it's being changed/set
    if (wcslen(displayID) > 0 && !IsDisplayDeviceActive(displayID)) {
        return 1; // Error: Display not active
    }

    // Update global variable if a new ID is provided
    if (wcslen(displayID) > 0) {
        wcscpy_s(INTERNAL_DISPLAY_ID, sizeof(INTERNAL_DISPLAY_ID) / sizeof(WCHAR), displayID);
    }

    // Write Display ID
    WritePrivateProfileStringW(L"Settings", INI_KEY_DISPLAY, INTERNAL_DISPLAY_ID, INI_PATH);

    return 0;
}

// Helper to get AppData path
static HRESULT SetINIPath() {
    HRESULT r = 0;
    if (SUCCEEDED(r = SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, INI_PATH))) {
        wcscat_s(INI_PATH, MAX_PATH, L"\\");
        wcscat_s(INI_PATH, MAX_PATH, INI_FILE_NAME);
    }
    return r;
}

// --- Power Management Logic ---

static GUID* GetActivePowerScheme() {
    GUID* activeScheme = NULL;
    if (PowerGetActiveScheme(NULL, &activeScheme) != ERROR_SUCCESS) {
        printf("Failed to get active power scheme.\n");
        return NULL;
    }
    return activeScheme;
}

// Helper to write power settings to the registry
static void _SetLidAction(GUID* scheme, const DWORD newVal) {
    GUID subgroupGUID = GUID_SYSTEM_BUTTON_SUBGROUP;
    GUID powerSettingGUID = GUID_LIDCLOSE_ACTION;

    // Set DC (Battery) value index
    if (PowerWriteDCValueIndex(NULL, scheme, &subgroupGUID, &powerSettingGUID, newVal) != ERROR_SUCCESS) {
        printf("Failed to set DC value index.\n");
    }
    // Set AC (Plugged in) value index
    if (PowerWriteACValueIndex(NULL, scheme, &subgroupGUID, &powerSettingGUID, newVal) != ERROR_SUCCESS) {
        printf("Failed to set AC value index.\n");
    }
    // Apply changes
    if (PowerSetActiveScheme(NULL, scheme) != ERROR_SUCCESS) {
        printf("Failed to set active power scheme.\n");
    }
    printf("Lid action set to: %d\n", newVal);
}

// Wrapper to set lid action
// 0 = Do Nothing
// 1 = Sleep (Standard Windows Default)
static void SetLidAction(const BOOL sleep) {
    GUID* activeScheme = GetActivePowerScheme();
    if (activeScheme != NULL) {
        _SetLidAction(activeScheme, sleep);
        LocalFree(activeScheme);
    }
}

// Check if any external monitor (not the internal one) is active
static BOOL ExternalDisplayConnected() {
    DISPLAY_DEVICE device = { .cb = sizeof(DISPLAY_DEVICE) };
    DWORD i = 0;
    while (EnumDisplayDevicesW(NULL, i++, &device, 0) != 0) {
        DISPLAY_DEVICE device2 = { .cb = sizeof(DISPLAY_DEVICE) };
        DWORD j = 0;
        while (EnumDisplayDevicesW(device.DeviceName, j++, &device2, 0) != 0) {
            // Skip the internal display
            if (wcscmp(device2.DeviceID, INTERNAL_DISPLAY_ID) == 0) {
                continue;
            }
            // Found an active external display
            if (device2.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

// Fill combo box with currently active displays
static void PopulateDisplayDevices(HWND hComboBox) {
    DISPLAY_DEVICE device = { .cb = sizeof(DISPLAY_DEVICE) };
    DWORD i = 0;
    while (EnumDisplayDevicesW(NULL, i++, &device, 0) != 0) {
        DISPLAY_DEVICE device2 = { .cb = sizeof(DISPLAY_DEVICE) };
        DWORD j = 0;
        while (EnumDisplayDevicesW(device.DeviceName, j++, &device2, 0) != 0) {
            if (device2.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                SendMessageW(hComboBox, CB_ADDSTRING, 0, (LPARAM)device2.DeviceID);
            }
        }
    }
}

// Updates the Windows Power Scheme based on monitor presence
static void HandleDisplayChange() {
    if (ExternalDisplayConnected()) {
        SetLidAction(FALSE); // 0 = Do Nothing
    }
    else {
        SetLidAction(TRUE);  // 1 = Sleep
    }
}

// Edge Case Handler:
// Checks if the user is in a state where they SHOULD be sleeping but aren't.
// (e.g., disconnected the monitor while the lid was already closed)
static void CheckAndSuspendIfNeeded(DWORD currentLidState) {
    // 1. If lid is open, do nothing.
    if (currentLidState != 0) {
        return;
    }

    // 2. If external display is still connected, do nothing.
    if (ExternalDisplayConnected()) {
        return;
    }

    // 3. Lid is closed AND no external display -> Force Suspend.
    printf("Condition met: Lid Closed + No External Display. Suspending...\n");

    // Ensure Lid Action is set back to Sleep (1) so subsequent events work natively
    SetLidAction(TRUE);

    // Manually trigger a system suspend.
    // FALSE, FALSE, FALSE = Standard Sleep (Suspend). 
    // If the user has "Hybrid Sleep" enabled in Windows, this will perform a Hybrid Sleep.
    SetSuspendState(FALSE, FALSE, FALSE);
}

// --- Window Procedures ---

static LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static NOTIFYICONDATA nid = { 0 };
    static HICON hIcon = NULL;
    static DWORD lidState = 1; // Default to 1 (Open)

    switch (msg) {
    case WM_CREATE:
        hIcon = LoadIconW(GetModuleHandleW(L"shell32.dll"), MAKEINTRESOURCEW(284));
        nid.cbSize = sizeof(NOTIFYICONDATA);
        nid.hWnd = hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_APP + 1;
        nid.hIcon = hIcon;
        wcscpy_s(nid.szTip, sizeof(nid.szTip) / sizeof(nid.szTip[0]), WINDOW_NAME);
        Shell_NotifyIconW(NIM_ADD, &nid);

        LoadSettingsFromIni();

        // Check if INI exists. If not, open settings window.
        WCHAR iniPath[MAX_PATH] = L"";
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, iniPath))) {
            wcscat_s(iniPath, MAX_PATH, L"\\");
            wcscat_s(iniPath, MAX_PATH, INI_FILE_NAME);
            if (_waccess(iniPath, 0) == -1) {
                PostMessageW(hwnd, WM_COMMAND, ID_TRAY_SET_DISPLAY, 0);
            }
            else {
                // Apply initial state based on connected devices
                HandleDisplayChange();
            }
        }
        break;

    case WM_APP + 1: // Tray Icon Interaction
        if (LOWORD(lParam) == WM_RBUTTONDOWN) {
            POINT cursor;
            GetCursorPos(&cursor);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_SET_DISPLAY, L"Settings...");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT) {
            PostQuitMessage(0);
        }
        else if (LOWORD(wParam) == ID_TRAY_SET_DISPLAY) {
            // Create Settings Window
            HWND hDisplayWindow = CreateWindowExW(0, L"DISPLAY_WINDOW_CLASS", L"Clamshell Settings",
                WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 400, 150,
                NULL, NULL, GetModuleHandleW(NULL), NULL);
            SendMessageW(hDisplayWindow, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            SendMessageW(hDisplayWindow, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            ShowWindow(hDisplayWindow, SW_SHOW);
        }
        break;

    case WM_DISPLAYCHANGE:
        // Handles resolution changes and software disconnects (Win+P)
        HandleDisplayChange();
        CheckAndSuspendIfNeeded(lidState);
        break;

    case WM_POWERBROADCAST:
        if (wParam == PBT_POWERSETTINGCHANGE) {
            // Track Lid State
            POWERBROADCAST_SETTING* pbs = (POWERBROADCAST_SETTING*)lParam;
            if (IsEqualGUID(&pbs->PowerSetting, &GUID_LIDSWITCH_STATE_CHANGE)) {
                lidState = *(DWORD*)pbs->Data;
                printf("Lid state changed: %d\n", lidState);

                // If lid closes and no monitor is present, ensure correct scheme is set
                // (Though Windows usually handles this, we enforce it)
                if (lidState == 0) {
                    HandleDisplayChange();
                }
            }
        }
        else if (wParam == PBT_APMRESUMEAUTOMATIC) {
            // System Woke Up.
            // Scenario: Laptop slept with HDMI. User unplugged HDMI while sleeping. User woke up laptop with Lid Closed.
            printf("System resumed. Re-validating state...\n");

            // Re-apply correct power scheme
            HandleDisplayChange();

            // Check if we need to go back to sleep immediately
            CheckAndSuspendIfNeeded(lidState);
        }
        break;

    case WM_DEVICECHANGE:
        // Handles Physical Plug/Unplug events
        if (wParam == DBT_DEVICEREMOVECOMPLETE)
        {
            PDEV_BROADCAST_HDR pHdr = (PDEV_BROADCAST_HDR)lParam;
            if (pHdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
            {
                // A device interface was removed. Re-check monitors.
                HandleDisplayChange();
                CheckAndSuspendIfNeeded(lidState);
            }
        }
        else if (wParam == DBT_DEVICEARRIVAL)
        {
            // Device connected, update scheme immediately to prevent sleeping
            HandleDisplayChange();
        }
        break;

    case WM_DESTROY:
        Shell_NotifyIcon(NIM_DELETE, &nid);
        if (hIcon) {
            DestroyIcon(hIcon);
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Procedure for the Settings Window
static LRESULT CALLBACK DisplayWindowProcedure(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hComboBox = NULL;
    static WCHAR tempDisplayID[MAX_PATH] = L"";

    switch (msg) {
    case WM_CREATE:
        // Label
        CreateWindowW(L"STATIC", L"Select Internal Display:",
            WS_CHILD | WS_VISIBLE, 10, 10, 300, 20, hwnd, NULL, GetModuleHandleW(NULL), NULL);

        // Combo Box
        hComboBox = CreateWindowW(L"COMBOBOX", NULL,
            CBS_DROPDOWN | WS_CHILD | WS_VISIBLE,
            10, 30, 360, 120, hwnd, (HMENU)ID_COMBOBOX,
            GetModuleHandleW(NULL), NULL);
        PopulateDisplayDevices(hComboBox);

        // Save Button
        CreateWindowW(L"BUTTON", L"Save & Close",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            130, 70, 120, 30, hwnd, (HMENU)ID_OK_BUTTON,
            GetModuleHandleW(NULL), NULL);
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_COMBOBOX && HIWORD(wParam) == CBN_SELCHANGE) {
            WPARAM index = SendMessageW(hComboBox, CB_GETCURSEL, 0, 0);
            SendMessageW(hComboBox, CB_GETLBTEXT, index, (LPARAM)tempDisplayID);
        }
        else if (LOWORD(wParam) == ID_OK_BUTTON) {
            // If user didn't select a new display in combo, use the existing ID
            const WCHAR* idToSave = (wcslen(tempDisplayID) > 0) ? tempDisplayID : INTERNAL_DISPLAY_ID;

            if (SaveSettingsToIni(idToSave) == 0) {
                tempDisplayID[0] = L'\0';
                ShowWindow(hwnd, SW_HIDE);
            }
            else {
                MessageBoxW(hwnd, L"Display not found or invalid selection!", L"Error", MB_ICONERROR | MB_OK);
            }
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));
        EndPaint(hwnd, &ps);
        break;
    }

    case WM_DESTROY:
        if (wcslen(INTERNAL_DISPLAY_ID) == 0) {
            PostQuitMessage(0); // Exit if setup was never completed
        }
        else {
            ShowWindow(hwnd, SW_HIDE); // Just hide settings window, don't kill app
        }
        break;

    case WM_DISPLAYCHANGE:
        // Refresh list if monitors change while settings window is open
        SendMessage(hComboBox, CB_RESETCONTENT, 0, 0);
        PopulateDisplayDevices(hComboBox);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// --- Entry Point ---

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nCmdShow)
{
#if NDEBUG == 0
    // Console for debugging
    AllocConsole();
    FILE* file = NULL;
    freopen_s(&file, "CONOUT$", "w", stdout);
    freopen_s(&file, "CONIN$", "r", stdin);
    freopen_s(&file, "CONOUT$", "w", stderr);
#endif

    HWND hwnd = NULL;
    HANDLE mutex = NULL;
    HPOWERNOTIFY hLidNotify = NULL;
    HDEVNOTIFY hDeviceNotify = NULL;

    // Prevent multiple instances
    mutex = CreateMutexW(NULL, TRUE, L"Global\\ClamshellMode");
    if (GetLastError() == ERROR_ALREADY_EXISTS || mutex == NULL) {
        return 1;
    }

    const WCHAR className[] = L"ClamshellModeWindow";
    const WCHAR displayClassName[] = L"DISPLAY_WINDOW_CLASS";

    if (!SUCCEEDED(SetINIPath())) {
        return 1;
    }

    // Register Main Window Class
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WindowProcedure;
    wc.hInstance = hInstance;
    wc.lpszClassName = className;
    RegisterClassW(&wc);

    // Register Settings Window Class
    wc.lpfnWndProc = DisplayWindowProcedure;
    wc.lpszClassName = displayClassName;
    wc.hIcon = LoadIconW(GetModuleHandleW(L"shell32.dll"), MAKEINTRESOURCEW(284));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); // Standard window background color
    RegisterClassW(&wc);

    // Create Main Window (Invisible)
    hwnd = CreateWindowExW(0, className, WINDOW_NAME,
        0, CW_USEDEFAULT, CW_USEDEFAULT,
        0, 0, NULL, NULL, hInstance, NULL);

    // Register for Power Notifications (Lid Switch)
    hLidNotify = RegisterPowerSettingNotification(hwnd, &GUID_LIDSWITCH_STATE_CHANGE, DEVICE_NOTIFY_WINDOW_HANDLE);

    // Register for Device Notifications (Monitor Hotplug)
    DEV_BROADCAST_DEVICEINTERFACE filter = { 0 };
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    filter.dbcc_classguid = GUID_DEVINTERFACE_MONITOR;
    hDeviceNotify = RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    // Message Loop
    MSG msg = { 0 };
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup
    UnregisterPowerSettingNotification(hLidNotify);
    UnregisterDeviceNotification(hDeviceNotify);
    CloseHandle(mutex);

    return 0;
}

int main(void) {
    return WinMain(GetModuleHandleW(NULL), NULL, NULL, 0);
}
