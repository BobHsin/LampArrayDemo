/*---------------------------------------------------------*\
| LampArrayDemo.cpp                                         |
|                                                           |
|   Windows Dynamic Lighting (Windows.Devices.Lights)       |
|   sample: light ONE selected lamp in WHITE, all others    |
|   off.                                                    |
|                                                           |
|   Build (x64 Native Tools / Developer Command Prompt,     |
|   Windows SDK 10.0.26100 or later, for IsAvailable /      |
|   AvailabilityChanged):                                   |
|     cl /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE       |
|        LampArrayDemo.cpp /link /SUBSYSTEM:WINDOWS         |
|                                                           |
|   Windows only gives lighting control to the app in the  |
|   FOREGROUND: keep this window active while testing.     |
\*---------------------------------------------------------*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <numeric>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Lights.h>
#include <winrt/Windows.UI.h>

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")

using namespace winrt;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Lights;
using winrt::Windows::UI::Color;

/*---------------------------------------------------------*\
| Control IDs and private messages                          |
\*---------------------------------------------------------*/
enum
{
    IDC_DEVICE  = 101,
    IDC_INDEX,
    IDC_LIGHT,
    IDC_PREV,
    IDC_NEXT,
    IDC_OFF,
    IDC_REFRESH,
    IDC_STATUS,
};

#define WM_APP_AVAILABILITY     (WM_APP + 1)

static const Color COLOR_WHITE  = { 255, 255, 255, 255 };   /* A, R, G, B */
static const Color COLOR_OFF    = { 255,   0,   0,   0 };   /* black = off */

/*---------------------------------------------------------*\
| One Dynamic Lighting device                               |
\*---------------------------------------------------------*/
struct LampDevice
{
    LampArray       lamp{ nullptr };
    std::wstring    name;
    event_token     availability_token{};
};

static HWND                     g_hwnd          = NULL;
static HWND                     g_device_box    = NULL;
static HWND                     g_index_edit    = NULL;
static HWND                     g_status        = NULL;
static std::vector<LampDevice>  g_devices;
static int                      g_lamp_index    = 0;
static bool                     g_lit           = false;

/*---------------------------------------------------------*\
| IsAvailable / AvailabilityChanged only exist on newer     |
| Windows 11 builds (UniversalApiContract v15); older       |
| builds still apply colors while this app has focus        |
\*---------------------------------------------------------*/
static bool                     g_has_availability = false;

/*---------------------------------------------------------*\
| Helpers                                                   |
\*---------------------------------------------------------*/
static LampDevice* CurrentDevice()
{
    LRESULT sel = SendMessageW(g_device_box, CB_GETCURSEL, 0, 0);

    if(sel == CB_ERR || sel < 0 || (size_t)sel >= g_devices.size())
    {
        return nullptr;
    }

    return &g_devices[(size_t)sel];
}

static int ReadIndexFromEdit()
{
    wchar_t text[16] = {};
    GetWindowTextW(g_index_edit, text, 16);
    return _wtoi(text);
}

static void WriteIndexToEdit(int index)
{
    SetWindowTextW(g_index_edit, std::to_wstring(index).c_str());
}

static void SetStatus(const std::wstring& text)
{
    SetWindowTextW(g_status, text.c_str());
}

/*---------------------------------------------------------*\
| Status text: device, lamp count, control availability     |
\*---------------------------------------------------------*/
static void UpdateStatus()
{
    LampDevice* dev = CurrentDevice();

    if(dev == nullptr)
    {
        SetStatus(L"找不到動態照明（LampArray）裝置。\r\n"
                  L"請確認：設定 → 個人化 → 動態照明 已開啟，而且裝置有出現在該頁面。");
        return;
    }

    try
    {
        int32_t count = dev->lamp.LampCount();

        std::wstring text = L"裝置：" + dev->name + L"\r\n";
        text += L"燈數：" + std::to_wstring(count) + L"（燈號 0 ~ " + std::to_wstring(count - 1) + L"）\r\n";

        if(g_has_availability)
        {
            text += dev->lamp.IsAvailable() ? L"控制權：可用（本視窗在前景時可以控制燈光）"
                                            : L"控制權：不可用 —— 請把本視窗切到前景，並確認動態照明已開啟";
        }
        else
        {
            text += L"控制權：此 Windows 版本無法查詢（本視窗在前景時才會套用燈光）";
        }

        if(g_lit && g_lamp_index < count)
        {
            LampInfo info = dev->lamp.GetLampInfo(g_lamp_index);
            auto     pos  = info.Position();                    /* meters */
            wchar_t  line[160];

            swprintf(line, 160, L"\r\n目前點亮：第 %d 顆（白色），位置 X=%.1f mm, Y=%.1f mm, Z=%.1f mm",
                     g_lamp_index, pos.x * 1000.0f, pos.y * 1000.0f, pos.z * 1000.0f);
            text += line;
        }

        SetStatus(text);
    }
    catch(hresult_error const& e)
    {
        SetStatus(L"讀取裝置狀態失敗：" + std::wstring(e.message()));
    }
}

/*---------------------------------------------------------*\
| Light only the selected lamp (white), all others off.     |
| One SetColorsForIndices call updates every lamp at once,  |
| so there is no flicker between "all off" and "one on".    |
\*---------------------------------------------------------*/
static void LightOneLamp(int index)
{
    LampDevice* dev = CurrentDevice();

    if(dev == nullptr)
    {
        UpdateStatus();
        return;
    }

    try
    {
        int32_t count = dev->lamp.LampCount();

        if(count <= 0)
        {
            return;
        }

        if(index < 0)       index = 0;
        if(index >= count)  index = count - 1;

        g_lamp_index = index;
        WriteIndexToEdit(index);

        std::vector<Color>   colors(count, COLOR_OFF);
        std::vector<int32_t> indices(count);

        colors[index] = COLOR_WHITE;
        std::iota(indices.begin(), indices.end(), 0);

        dev->lamp.SetColorsForIndices(
            array_view<Color const>(colors.data(), colors.data() + colors.size()),
            array_view<int32_t const>(indices.data(), indices.data() + indices.size()));

        g_lit = true;
        UpdateStatus();
    }
    catch(hresult_error const& e)
    {
        SetStatus(L"點燈失敗：" + std::wstring(e.message()));
    }
}

static void TurnAllOff()
{
    LampDevice* dev = CurrentDevice();

    if(dev == nullptr)
    {
        return;
    }

    try
    {
        dev->lamp.SetColor(COLOR_OFF);
        g_lit = false;
        UpdateStatus();
    }
    catch(hresult_error const& e)
    {
        SetStatus(L"關燈失敗：" + std::wstring(e.message()));
    }
}

/*---------------------------------------------------------*\
| (Re)enumerate Dynamic Lighting devices                    |
\*---------------------------------------------------------*/
static void ClearDevices()
{
    for(LampDevice& dev : g_devices)
    {
        if(dev.lamp && g_has_availability)
        {
            dev.lamp.AvailabilityChanged(dev.availability_token);
        }
    }

    g_devices.clear();
    SendMessageW(g_device_box, CB_RESETCONTENT, 0, 0);
}

static void RefreshDevices()
{
    ClearDevices();
    g_lit = false;

    try
    {
        DeviceInformationCollection infos = DeviceInformation::FindAllAsync(LampArray::GetDeviceSelector()).get();

        for(DeviceInformation const& info : infos)
        {
            LampArray lamp = LampArray::FromIdAsync(info.Id()).get();

            if(!lamp)
            {
                continue;
            }

            LampDevice dev;
            dev.lamp = lamp;
            dev.name = std::wstring(info.Name());

            /*---------------------------------------------*\
            | Control moves between apps (foreground app,   |
            | Settings priority); called on a thread pool   |
            | thread, so hand it to the UI thread           |
            \*---------------------------------------------*/
            if(g_has_availability)
            {
                dev.availability_token = lamp.AvailabilityChanged(
                    [](LampArray const&, winrt::Windows::Foundation::IInspectable const&)
                    {
                        PostMessageW(g_hwnd, WM_APP_AVAILABILITY, 0, 0);
                    });
            }

            std::wstring item = dev.name + L"  （" + std::to_wstring(lamp.LampCount()) + L" 顆燈）";
            SendMessageW(g_device_box, CB_ADDSTRING, 0, (LPARAM)item.c_str());

            g_devices.push_back(dev);
        }
    }
    catch(hresult_error const& e)
    {
        SetStatus(L"列舉裝置失敗：" + std::wstring(e.message()));
        return;
    }

    if(!g_devices.empty())
    {
        SendMessageW(g_device_box, CB_SETCURSEL, 0, 0);
    }

    UpdateStatus();
}

/*---------------------------------------------------------*\
| Window                                                    |
\*---------------------------------------------------------*/
static HWND AddControl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                       int x, int y, int w, int h, int id)
{
    HWND ctl = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                               parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return ctl;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch(msg)
    {
        case WM_CREATE:
            AddControl(hwnd, L"STATIC", L"裝置：", 0, 12, 16, 50, 20, 0);
            g_device_box = AddControl(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                      64, 12, 330, 200, IDC_DEVICE);
            AddControl(hwnd, L"BUTTON", L"重新整理", BS_PUSHBUTTON | WS_TABSTOP, 402, 11, 90, 26, IDC_REFRESH);

            AddControl(hwnd, L"STATIC", L"燈號：", 0, 12, 56, 50, 20, 0);
            g_index_edit = AddControl(hwnd, L"EDIT", L"0", WS_BORDER | ES_NUMBER | WS_TABSTOP,
                                      64, 52, 70, 24, IDC_INDEX);
            AddControl(hwnd, L"BUTTON", L"點亮（白色）", BS_DEFPUSHBUTTON | WS_TABSTOP, 142, 51, 110, 26, IDC_LIGHT);
            AddControl(hwnd, L"BUTTON", L"◀ 上一顆",    BS_PUSHBUTTON | WS_TABSTOP, 260, 51, 76, 26, IDC_PREV);
            AddControl(hwnd, L"BUTTON", L"下一顆 ▶",    BS_PUSHBUTTON | WS_TABSTOP, 342, 51, 76, 26, IDC_NEXT);
            AddControl(hwnd, L"BUTTON", L"全部關閉",     BS_PUSHBUTTON | WS_TABSTOP, 424, 51, 68, 26, IDC_OFF);

            g_status = AddControl(hwnd, L"STATIC", L"", SS_LEFT, 12, 94, 480, 90, IDC_STATUS);
            return 0;

        case WM_COMMAND:
            switch(LOWORD(wparam))
            {
                case IDC_LIGHT:     LightOneLamp(ReadIndexFromEdit());  break;
                case IDC_PREV:      LightOneLamp(g_lamp_index - 1);     break;
                case IDC_NEXT:      LightOneLamp(g_lamp_index + 1);     break;
                case IDC_OFF:       TurnAllOff();                       break;
                case IDC_REFRESH:   RefreshDevices();                   break;
                case IDC_DEVICE:
                    if(HIWORD(wparam) == CBN_SELCHANGE)
                    {
                        g_lit = false;
                        UpdateStatus();
                    }
                    break;
            }
            return 0;

        case WM_APP_AVAILABILITY:
        {
            /*---------------------------------------------*\
            | Got control back: show our lamp again         |
            \*---------------------------------------------*/
            LampDevice* dev = CurrentDevice();

            if(dev != nullptr && g_lit && g_has_availability && dev->lamp.IsAvailable())
            {
                LightOneLamp(g_lamp_index);
            }
            else
            {
                UpdateStatus();
            }
            return 0;
        }

        case WM_DESTROY:
            ClearDevices();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    init_apartment(apartment_type::multi_threaded);

    g_has_availability = winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                             L"Windows.Devices.Lights.LampArray", L"IsAvailable");

    WNDCLASSW wc     = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"LampArrayDemoWindow";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Dynamic Lighting 單顆點燈範例",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 520, 240, NULL, NULL, instance, NULL);
    ShowWindow(g_hwnd, show);

    RefreshDevices();

    MSG msg;
    while(GetMessageW(&msg, NULL, 0, 0))
    {
        if(!IsDialogMessageW(g_hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return (int)msg.wParam;
}
