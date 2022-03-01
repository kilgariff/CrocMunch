// CrocMunch.cpp : Defines the entry point for the application.
//

#include "pch.h"
#include "framework.h"
#include "CrocMunch.h"
#include "munch_thread.hpp"

#include "initguid.h"   // include before devpropdef.h
#include "CommCtrl.h" // For combobox
#include "Setupapi.h" // For SetupDiGetClassDevs etc.
#include "winioctl.h" // For IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS 
#include "devpropdef.h"
#include "devpkey.h"
#include "cfgmgr32.h"
#include "dbt.h"

#include <vector>
#include <string>
#include <array>
#include <wchar.h>

#define MAX_LOADSTRING 100

#define IDT_TIMER 1002
#define IDB_MUNCH_BUTTON 1003
#define IDPB_PROGRESS_BAR  1004

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name
HWND hWnd;
HWND hWndComboBox;
HWND hWndInfoLabel;
HWND hWndCrocImage;
HWND hWndFileImage;
HWND hWndProgressBar;
HWND hWndProgressText;
HWND hWndMunchButton;
WNDCLASSEXW wcex;

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

int MessageBoxCentered(HWND hWnd, LPCTSTR lpText, LPCTSTR lpCaption, UINT uType)
{
    // Center message box at its parent window
    static HHOOK hHookCBT{};
    hHookCBT = SetWindowsHookEx(WH_CBT,
                                [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
        if (nCode == HCBT_CREATEWND)
        {
            if (((LPCBT_CREATEWND)lParam)->lpcs->lpszClass == (LPWSTR)(ATOM)32770)  // #32770 = dialog box class
            {
                RECT rcParent{};
                GetWindowRect(((LPCBT_CREATEWND)lParam)->lpcs->hwndParent, &rcParent);
                ((LPCBT_CREATEWND)lParam)->lpcs->x = rcParent.left + ((rcParent.right - rcParent.left) - ((LPCBT_CREATEWND)lParam)->lpcs->cx) / 2;
                ((LPCBT_CREATEWND)lParam)->lpcs->y = rcParent.top + ((rcParent.bottom - rcParent.top) - ((LPCBT_CREATEWND)lParam)->lpcs->cy) / 2;
            }
        }

        return CallNextHookEx(hHookCBT, nCode, wParam, lParam);
    },
                                0, GetCurrentThreadId());

    int iRet{MessageBox(hWnd, lpText, lpCaption, uType)};

    UnhookWindowsHookEx(hHookCBT);

    return iRet;
}

void ShowError()
{
    std::wstring error_str = L"CrocMunch encountered an error and is unable to proceed. Please contact support at https://kilgariff.tech/";
    std::wstring const last_win32_error = GetLastErrorAsString();

    if (last_win32_error.empty() == false)
    {
        error_str += L"\n\nThe error was: ";
        error_str += last_win32_error;
    }

    MessageBoxCentered(hWnd, error_str.c_str(), L"Unable to detect drives", MB_ICONERROR);
}

struct Drive
{
    int device_idx = -1;
    int index_in_dropdown = -1;

    HANDLE handle = INVALID_HANDLE_VALUE;
    std::wstring friendly_name;
    std::wstring description;
    std::wstring manufacturer;

    std::wstring device_path;
    std::wstring physical_drive_path;
    
    STORAGE_DEVICE_NUMBER diskNumber;
    DWORD dwSize;
    bool is_removable = false;
};

std::vector<Drive> drive_vec;

struct MunchProgress
{
    bool is_munching = false;
    bool all_data_gone = false;
    size_t progress_bar_updates = 0;
    size_t progress_bar_stage = 0;
    int file_icon_pos;
};

MunchProgress munch_progress;

MunchThread munch_thread;

Drive * GetDriveByDropdownListIndex(LRESULT list_item_index)
{
    for (auto & drive : drive_vec)
    {
        if (drive.index_in_dropdown == list_item_index)
        {
            return &drive;
        }
    }

    return nullptr;
}

void SelectDriveByListIndex(LRESULT list_item_index)
{
    if (Drive * drive = GetDriveByDropdownListIndex(list_item_index))
    {
        std::wstring label_text;
        
        label_text += L"Type: ";
        label_text += drive->description;
        label_text += L"\n";

        label_text += L"Manufacturer: ";
        label_text += drive->manufacturer;
        label_text += L"\n";

        label_text += L"This drive ";
        label_text += drive->is_removable ? L"is" : L"is not";
        label_text += L" removable";

        SetWindowText(hWndInfoLabel, label_text.c_str());
    }
    else
    {
        SetWindowText(hWndInfoLabel, L"No removable drives detected");
    }
}

void ScanDrives()
{
    drive_vec.clear();

    HDEVINFO deviceInfoHandle;
    GUID diskClassDeviceInterfaceGuid = GUID_DEVINTERFACE_DISK;
    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA deviceInterfaceDetailData;
    DWORD requiredSize;
    DWORD deviceIndex;
    DWORD bytesReturned;

    //
    // Get the handle to the device information set for installed
    // disk class devices. Returns only devices that are currently
    // present in the system and have an enabled disk device
    // interface.
    //
    deviceInfoHandle = SetupDiGetClassDevs(&diskClassDeviceInterfaceGuid,
                                           NULL,
                                           NULL,
                                           DIGCF_PRESENT |
                                           DIGCF_DEVICEINTERFACE);

    if (INVALID_HANDLE_VALUE == deviceInfoHandle)
    {
        ShowError();
        exit(1);
    }

    ZeroMemory(&deviceInterfaceData, sizeof(SP_DEVICE_INTERFACE_DATA));
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    deviceIndex = 0;

    while (SetupDiEnumDeviceInterfaces(deviceInfoHandle,
                                       NULL,
                                       &diskClassDeviceInterfaceGuid,
                                       deviceIndex,
                                       &deviceInterfaceData))
    {
        Drive drive;

        SetupDiGetDeviceInterfaceDetail(deviceInfoHandle,
                                        &deviceInterfaceData,
                                        NULL,
                                        0,
                                        &requiredSize,
                                        NULL);

        if (ERROR_INSUFFICIENT_BUFFER != GetLastError())
        {
            ShowError();
            exit(1);
        }

        deviceInterfaceDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(requiredSize);

        if (deviceInterfaceDetailData == nullptr)
        {
            ShowError();
            exit(1);
        }

        ZeroMemory(deviceInterfaceDetailData, requiredSize);
        deviceInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (SetupDiGetDeviceInterfaceDetail(deviceInfoHandle,
                                            &deviceInterfaceData,
                                            deviceInterfaceDetailData,
                                            requiredSize,
                                            NULL,
                                            NULL) == FALSE)
        {
            ShowError();
            exit(1);
        }

        drive.handle = CreateFile(deviceInterfaceDetailData->DevicePath,
                                  GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  NULL);

        if (INVALID_HANDLE_VALUE == drive.handle)
        {
            ShowError();
            exit(1);
        }

        if (DeviceIoControl(drive.handle,
                            IOCTL_STORAGE_GET_DEVICE_NUMBER,
                            NULL,
                            0,
                            &drive.diskNumber,
                            sizeof(STORAGE_DEVICE_NUMBER),
                            &bytesReturned,
                            NULL) == FALSE)
        {
            ShowError();
            exit(1);
        }

        CloseHandle(drive.handle);
        drive.handle = INVALID_HANDLE_VALUE;
        drive.device_path = std::wstring(deviceInterfaceDetailData->DevicePath);
        drive.physical_drive_path = L"\\\\?\\PhysicalDrive" + std::to_wstring(drive.diskNumber.DeviceNumber);
        drive.device_idx = deviceIndex;
        ++deviceIndex;

        drive_vec.emplace_back(std::move(drive));
    }

    if (ERROR_NO_MORE_ITEMS != GetLastError())
    {
        ShowError();
        exit(1);
    }

    SP_DEVINFO_DATA deviceInfoData;
    deviceInfoData.cbSize = sizeof(deviceInfoData);

    deviceIndex = 0;

    while (SetupDiEnumDeviceInfo(deviceInfoHandle,
                                 deviceIndex,
                                 &deviceInfoData))
    {
        auto & drive = drive_vec[deviceIndex];
        ++deviceIndex;

        WCHAR diskFriendlyName[MAX_PATH] = L"";
        DWORD DataT;
        DWORD dwSize;

        if (SetupDiGetDeviceProperty(deviceInfoHandle,
                                             &deviceInfoData,
                                             &DEVPKEY_Device_BusReportedDeviceDesc,
                                             &DataT,
                                             (PBYTE)diskFriendlyName,
                                             ARRAYSIZE(diskFriendlyName),
                                             &dwSize,
                                             NULL) == FALSE)
        {
            ShowError();
            exit(1);
        }

        drive.friendly_name = diskFriendlyName;

        if (SetupDiGetDeviceRegistryProperty(deviceInfoHandle,
                                                &deviceInfoData,
                                                SPDRP_DEVICEDESC,
                                                &DataT,
                                                (PBYTE)diskFriendlyName,
                                                ARRAYSIZE(diskFriendlyName),
                                                NULL) == FALSE)
        {
            ShowError();
            exit(1);
        }

        drive.description = diskFriendlyName;

        if (drive.friendly_name.empty())
        {
            drive.friendly_name = diskFriendlyName;
        }

        DWORD removal_policy = 0;
        if (SetupDiGetDeviceRegistryProperty(deviceInfoHandle,
                                             &deviceInfoData,
                                             SPDRP_REMOVAL_POLICY,
                                             &DataT,
                                             (BYTE *) &removal_policy,
                                             sizeof(DWORD),
                                             NULL) == FALSE)
        {
            ShowError();
            exit(1);
        }

        drive.is_removable =
            removal_policy == CM_REMOVAL_POLICY_EXPECT_SURPRISE_REMOVAL ||
            removal_policy == CM_REMOVAL_POLICY_EXPECT_ORDERLY_REMOVAL;

        // Manufacturer name.
        WCHAR manufacturer_name[4096] = L"";
        if (SetupDiGetDeviceRegistryProperty(deviceInfoHandle,
                                             &deviceInfoData,
                                             SPDRP_MFG,
                                             &DataT,
                                             (BYTE *)&manufacturer_name,
                                             ARRAYSIZE(manufacturer_name),
                                             NULL) == FALSE)
        {
            ShowError();
            exit(1);
        }

        drive.manufacturer = manufacturer_name;
    }

    if (INVALID_HANDLE_VALUE != deviceInfoHandle)
    {
        SetupDiDestroyDeviceInfoList(deviceInfoHandle);
    }

    for (auto & drive : drive_vec)
    {
        if (INVALID_HANDLE_VALUE != drive.handle)
        {
            CloseHandle(drive.handle);
        }
    }
}

void PopulateComboBox()
{
    LRESULT const count = SendMessage(hWndComboBox, CB_GETCOUNT, NULL, NULL);

    for (int i = 0; i < count; ++i)
    {
        SendMessage(hWndComboBox, CB_DELETESTRING, (WPARAM)0, (LPARAM)0);
    }

    // load the combobox with item list.
    // Send a CB_ADDSTRING message to load each item.

    int index_in_dropdown = 0;
    for (auto & drive : drive_vec)
    {
        if (drive.is_removable)
        {
            // Add string to combobox.
            SendMessage(hWndComboBox, (UINT)CB_ADDSTRING, (WPARAM)0, (LPARAM)drive.friendly_name.c_str());
            drive.index_in_dropdown = index_in_dropdown;
            ++index_in_dropdown;
        }
    }

    if (drive_vec.empty() == false)
    {
        // Send the CB_SETCURSEL message to display an initial item in the selection field.
        SendMessage(hWndComboBox, CB_SETCURSEL, (WPARAM)0, (LPARAM)0);
    }
}

void StartMunching(Drive & drive)
{
    munch_progress.is_munching = true;
    munch_progress.progress_bar_updates = 0;
    munch_progress.file_icon_pos = 0;

    EnableWindow(hWndComboBox, FALSE);
    EnableWindow(hWndMunchButton, FALSE);

    munch_thread.prime(drive.physical_drive_path, drive.diskNumber.DeviceNumber);
    munch_thread.start();
}

std::wstring human_readable_bytes(uint64_t bytes)
{
    std::array<std::wstring, 5> suffixes = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};

    int i = 0;
    double dblBytes = double(bytes);

    if (bytes > 1024)
    {
        for (i = 0; (bytes / 1024) > 0 && i < suffixes.size() - 1; i++, bytes /= 1024)
            dblBytes = bytes / 1024.0;
    }

    TCHAR output[200];
    _snwprintf_s(output, 200, L"%.02lf %s", dblBytes, suffixes[i].c_str());
    return std::wstring(output);
}

void UpdateProgress()
{
    uint64_t const stage = munch_thread.stage;
    uint64_t const munched = munch_thread.bytes_munched;
    uint64_t const total_to_munch = munch_thread.total_bytes_to_munch;

    if (total_to_munch > 0)
    {
        float const progress = munched / (float)total_to_munch;

        DWORD const progress_segments = DWORD(progress * 100.f);
        SendMessage(hWndProgressBar, PBM_SETPOS, progress_segments, 0);

        std::wstring progress_msg;
        progress_msg = L"Munching is in progress...\n";
        progress_msg += L"stage " + std::to_wstring(stage) + L" of 4\n";
        progress_msg += std::to_wstring(int(progress * 100.f)) + L"% of current stage\n";
        progress_msg += L"(";
        progress_msg += human_readable_bytes(munched) + L" of " + human_readable_bytes(total_to_munch);
        progress_msg += L")";
        SetWindowText(hWndProgressText, progress_msg.c_str());
    }

    int file_icon_xpos_start = 80;      // Horizontal position of the window.
    int file_icon_ypos = 90;            // Vertical position of the window.
    int file_icon_width = 200;          // Width of the window
    int file_icon_height = 50;          // Height of the window

    munch_progress.file_icon_pos += 10;
    float file_icon_x = file_icon_xpos_start + float(munch_progress.file_icon_pos % 100);

    MoveWindow(hWndFileImage,
               int(file_icon_x),
               file_icon_ypos,
               file_icon_width,
               file_icon_height,
               FALSE);

    int croc_icon_xpos = 210;            // Horizontal position of the window.
    int croc_icon_ypos = 90;            // Vertical position of the window.
    int croc_icon_nwidth = 200;          // Width of the window
    int croc_icon_nheight = 50;         // Height of the window

    MoveWindow(hWndCrocImage,
               int(croc_icon_xpos),
               croc_icon_ypos + int(sin((double) munch_progress.file_icon_pos) * 5),
               croc_icon_nwidth,
               croc_icon_nheight,
               FALSE);

    RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_INTERNALPAINT | RDW_UPDATENOW);

    munch_progress.is_munching = munch_thread.is_running;
    munch_progress.all_data_gone = munched == total_to_munch;
}

void FinishMunching(bool drive_removed)
{
    munch_progress.is_munching = false;
    munch_thread.force_stop();
    KillTimer(hWnd, IDT_TIMER);

    EnableWindow(hWndComboBox, TRUE);
    EnableWindow(hWndMunchButton, TRUE);

    MoveWindow(hWndFileImage, 0, 0, 0, 0, FALSE);

    ScanDrives();
    PopulateComboBox();

    SendMessage(hWndProgressBar, PBM_SETPOS, 0, 0);

    SetWindowText(hWndProgressText, L"");

    RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_INTERNALPAINT | RDW_UPDATENOW);

    if (munch_progress.all_data_gone)
    {
        MessageBoxCentered(hWnd, L"Munching has finished! All the data on your drive has been eaten by the croc. Have a nice day.", L"Finished", MB_OK | MB_ICONINFORMATION);
    }
    else if (drive_removed)
    {
        MessageBoxCentered(hWnd, L"A drive was removed while the croc was munching! Please re-insert drive and try munching again.", L"DRIVE REMOVED", MB_OK | MB_ICONERROR);
    }
    else
    {
        MessageBoxCentered(hWnd, L"Munching was interrupted! There may still be data left on your drive. Please try munching again.", L"INTERRUPTED", MB_OK | MB_ICONERROR);
    }
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR    lpCmdLine,
                      _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_CROCMUNCH, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_CROCMUNCH));

    ScanDrives();

    // Create drop-down list for drive selection.
    {
        int xpos = 10;            // Horizontal position of the window.
        int ypos = 10;            // Vertical position of the window.
        int nwidth = 470;          // Width of the window
        int nheight = 200;         // Height of the window

        hWndComboBox = CreateWindow(WC_COMBOBOX, TEXT(""),
                                    CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_CHILD | WS_VISIBLE,
                                    xpos, ypos, nwidth, nheight, hWnd, NULL, hInstance,
                                    NULL);

        PopulateComboBox();
    }

    // Info label.
    {
        int xpos = 10;            // Horizontal position of the window.
        int ypos = 40;            // Vertical position of the window.
        int nwidth = 470;          // Width of the window
        int nheight = 50;         // Height of the window

        hWndInfoLabel = CreateWindow(L"Static", L"",
                                     WS_CHILD | WS_VISIBLE,
                                     xpos, ypos, nwidth, nheight, hWnd, NULL, hInstance,
                                     NULL);

        SelectDriveByListIndex(0);
    }

    // Crocodile.
    {
        int xpos = 210;            // Horizontal position of the window.
        int ypos = 90;            // Vertical position of the window.
        int nwidth = 200;          // Width of the window
        int nheight = 50;         // Height of the window

        hWndCrocImage = CreateWindow(L"Static", L"",
                                     WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE,
                                     xpos, ypos, nwidth, nheight, hWnd, NULL, hInstance,
                                     NULL);

        HICON icon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CROCMUNCH));
        SendMessage(hWndCrocImage, STM_SETIMAGE, IMAGE_ICON, (LPARAM) icon);
    }

    // File icon.
    {
        hWndFileImage = CreateWindow(L"Static", L"",
                                     WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE,
                                     0, 0, 0, 0, hWnd, NULL, hInstance,
                                     NULL);

        HICON icon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CROC_DOCUMENT));
        SendMessage(hWndFileImage, STM_SETIMAGE, IMAGE_ICON, (LPARAM)icon);
    }

    // Progress Bar.
    {
        int xpos = 20;            // Horizontal position of the window.
        int ypos = 140;            // Vertical position of the window.
        int nwidth = 450;          // Width of the window
        int nheight = 20;         // Height of the window

        InitCommonControls();

        hWndProgressBar = CreateWindowEx(0, PROGRESS_CLASS, (LPCWSTR) NULL,
                                       WS_CHILD | WS_VISIBLE,
                                       xpos, ypos, nwidth, nheight, hWnd, (HMENU) IDPB_PROGRESS_BAR, hInstance,
                                       NULL);

        SendMessage(hWndProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessage(hWndProgressBar, PBM_SETSTEP, (WPARAM)1, 0);
    }

    // Progress Text.
    {
        int xpos = 10;            // Horizontal position of the window.
        int ypos = 170;            // Vertical position of the window.
        int nwidth = 470;          // Width of the window
        int nheight = 80;         // Height of the window

        hWndProgressText = CreateWindow(L"Static", L"",
                                        WS_CHILD | WS_VISIBLE,
                                        xpos, ypos, nwidth, nheight, hWnd, NULL, hInstance,
                                        NULL);

        SelectDriveByListIndex(0);
    }

    // Munch button.
    {
        int xpos = 140;            // Horizontal position of the window.
        int ypos = 260;            // Vertical position of the window.
        int nwidth = 200;          // Width of the window
        int nheight = 25;         // Height of the window

        hWndMunchButton = CreateWindow(WC_BUTTON, L"Munch",
                                       WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                       xpos, ypos, nwidth, nheight, hWnd, (HMENU) IDB_MUNCH_BUTTON, hInstance,
                                       NULL);

        SelectDriveByListIndex(0);
    }

    // Message pump:
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int) msg.wParam;
}

//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CROCMUNCH));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_CROCMUNCH);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CROCMUNCH));

    return RegisterClassExW(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // Store instance handle in our global variable

   hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
      CW_USEDEFAULT, 0, 512, 350, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   }

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE: Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
        case WM_DEVICECHANGE:
        {
            if (munch_progress.is_munching)
            {
                FinishMunching(true);
            }

            ScanDrives();
            PopulateComboBox();
            SelectDriveByListIndex(0);
            
            break;
        }
        
        case WM_TIMER:
        {
            switch ((UINT)wParam)
            {
                case IDT_TIMER:
                {
                    UpdateProgress();

                    if (munch_progress.is_munching == false)
                    {
                        FinishMunching(false);
                    }
                }
                break;
            }
        }

        break;


        case WM_COMMAND:
        {
            if (HIWORD(wParam) == CBN_SELCHANGE || HIWORD(wParam) == CBN_SELENDCANCEL)
            {
                if (munch_progress.is_munching == false)
                {
                    LRESULT ItemIndex = SendMessage((HWND)lParam, (UINT)CB_GETCURSEL,
                                                    (WPARAM)0, (LPARAM)0);
                    TCHAR  ListItem[256];
                    (TCHAR)SendMessage((HWND)lParam, (UINT)CB_GETLBTEXT,
                                       (WPARAM)ItemIndex, (LPARAM)ListItem);

                    SelectDriveByListIndex(ItemIndex);
                }
            }
            else if (HIWORD(wParam) == CBN_DROPDOWN)
            {
                if (munch_progress.is_munching == false)
                {
                    SetWindowText(hWndInfoLabel, L"");
                }
            }
            else if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == IDB_MUNCH_BUTTON)
            {
                if (munch_progress.is_munching == false)
                {
                    LRESULT ItemIndex = SendMessage((HWND)lParam, (UINT)CB_GETCURSEL,
                                                    (WPARAM)0, (LPARAM)0);

                    if (Drive * drive = GetDriveByDropdownListIndex(ItemIndex))
                    {
                        std::wstring msg = L"If you allow the croc to munch the drive '";
                        msg += drive->friendly_name;
                        msg += L"', ALL DATA ON THIS DRIVE WILL BE ERASED. Are you sure?";

                        int msgbox_result = MessageBoxCentered(hWnd, msg.c_str(), L"WARNING", MB_YESNO | MB_ICONWARNING);
                        if (msgbox_result == IDYES)
                        {
                            if (SetTimer(hWnd, IDT_TIMER, 100, (TIMERPROC)NULL))
                            {
                                StartMunching(*drive);
                            }
                            else
                            {
                                ShowError();
                                exit(1);
                            }
                        }
                    }
                }
            }
            else
            {
                int wmId = LOWORD(wParam);
                // Parse the menu selections:
                switch (wmId)
                {
                    case IDM_ABOUT:
                    DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                    break;
                    case IDM_EXIT:
                    DestroyWindow(hWnd);
                    break;
                    default:
                    return DefWindowProc(hWnd, message, wParam, lParam);
                }
            }
        }
        break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            // TODO: Add any drawing code that uses hdc here...
            EndPaint(hWnd, &ps);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}