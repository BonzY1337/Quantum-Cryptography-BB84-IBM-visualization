#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <stdint.h>
#include <windows.h>
#include <pdh.h>
#include <vector>
#include <TlHelp32.h>
#include <iostream>
#include <fstream>
#include <string>
#include <set>
#include <psapi.h>
#include "dll_bytes.h"
#pragma comment(lib, "pdh.lib")
#include <cstdlib>
#include <ctime>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")


struct Photon {
    int bit;
    int originalBit;
    char basis;
    char originalBasis;   
    float x;
    bool active = true;
    bool intercepted = false;
};

std::vector<Photon> apiPhotons;

struct QC_Row {
    int aliceBit;
    char aliceBasis;
    char bobBasis;
    int bobBit;
    bool match;
    bool valid;
};


//static std::vector<QC_Row> qc_table;

//globals
PDH_HQUERY cpuQuery;
PDH_HCOUNTER cpuTotal;
bool cpuInitialized = false;
float g_cpuUsage = 0.0f;
DWORD64 lastCPUTick = 0;
MEMORYSTATUSEX memInfo;
float g_memoryUsage = 0.0f;
std::vector<QC_Row> qc_table;
float qc_errorRate = 0.0f;
int qc_validBits = 0;
int qc_errors = 0;
std::string qc_finalKey = "";
std::string animatedKey = "";
std::vector<QC_Row> qc_buffer;
float row_timer = 0.0f;
float row_delay = 0.15f; 
float api_timer = 0.0f;
float api_interval = 2.0f;
std::vector<Photon> photons;
bool qc_running = false;
bool eveEnabled = false;
float speed = 400.0f;
float startX = 100.0f;
float endX = 1000.0f;
float yPos = 200.0f;
float lastTime = 0.0f;
size_t nextPhotonIndex = 0;
float spawnTimer = 0.0f;
float spawnDelay = 0.1f;
std::vector<float> mitigationHistory;
float mitigationTimer = 0.0f;
float mitigationInterval = 0.5f;

enum QCMode {
    ModeLocal,
    ModeQiskit,
    ModeIBM
};

QCMode qc_mode = ModeLocal;

enum MitigationAction{
    Mitigate_None,
    Mitigate_Filter,
    Mitigate_ErrorCorrect,
    Mitigate_PrivacyAmplify,
    Mitigate_Rotate,
    Mitigate_Abort
};

MitigationAction currentAction = Mitigate_None;

MitigationAction DecideMitigation(float err)
{
    if (err < 0.05f) return Mitigate_None;
    if (err < 0.10f) return Mitigate_Filter;
    if (err < 0.18f) return Mitigate_ErrorCorrect;
    if (err < 0.25f) return Mitigate_PrivacyAmplify;
    if (err < 0.35f) return Mitigate_Rotate;
    return Mitigate_Abort;
}

bool useManualAttack = false;
float manualErrorPercent = 20.0f;
float effectiveErrorRate = 0.0f;

float MitigationToFloat(MitigationAction action)
{
    switch (action)
    {
    case Mitigate_None: return 0.0f;
    case Mitigate_Filter: return 1.0f;
    case Mitigate_ErrorCorrect: return 2.0f;
    case Mitigate_PrivacyAmplify: return 3.0f;
    case Mitigate_Rotate: return 4.0f;
    case Mitigate_Abort: return 5.0f;
    default: return 0.0f;
    }
}

// Data
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
Microsoft::WRL::ComPtr<IDXGIOutputDuplication> g_duplication;
Microsoft::WRL::ComPtr<ID3D11Texture2D> g_desktopTexture;
ID3D11ShaderResourceView* g_desktopSRV = nullptr;
ImTextureID g_imguiDesktopTexture = 0;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool InitDesktopDuplication()
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)))
        return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    if (FAILED(dxgiDevice->GetAdapter(&dxgiAdapter)))
        return false;

    Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
    if (FAILED(dxgiAdapter->EnumOutputs(0, &dxgiOutput)))
        return false;

    Microsoft::WRL::ComPtr<IDXGIOutput1> dxgiOutput1;
    if (FAILED(dxgiOutput.As(&dxgiOutput1)))
        return false;

    return SUCCEEDED(dxgiOutput1->DuplicateOutput(g_pd3dDevice, &g_duplication));
}

bool CaptureScreenToTexture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView** outTextureSRV)
{
    int screenX = GetSystemMetrics(SM_CXSCREEN);
    int screenY = GetSystemMetrics(SM_CYSCREEN);

    HDC hScreen = GetDC(NULL);
    HDC hCapture = CreateCompatibleDC(hScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = screenX;
    bmi.bmiHeader.biHeight = -screenY;  // negative to avoid vertical flip
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pPixels = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pPixels, NULL, 0);
    if (!hBitmap || !pPixels)
        return false;

    SelectObject(hCapture, hBitmap);
    BitBlt(hCapture, 0, 0, screenX, screenY, hScreen, 0, 0, SRCCOPY);

    // Describe D3D11 texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = screenX;
    desc.Height = screenY;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)))
    {
        DeleteObject(hBitmap);
        DeleteDC(hCapture);
        ReleaseDC(NULL, hScreen);
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        BYTE* src = (BYTE*)pPixels;
        for (int y = 0; y < screenY; ++y)
        {
            memcpy((BYTE*)mapped.pData + y * mapped.RowPitch,
                src + y * screenX * 4,
                screenX * 4);
        }
        context->Unmap(texture, 0);
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    if (FAILED(device->CreateShaderResourceView(texture, &srvDesc, outTextureSRV)))
    {
        texture->Release();
        DeleteObject(hBitmap);
        DeleteDC(hCapture);
        ReleaseDC(NULL, hScreen);
        return false;
    }

    texture->Release();
    DeleteObject(hBitmap);
    DeleteDC(hCapture);
    ReleaseDC(NULL, hScreen);

    return true;
}

static ULARGE_INTEGER lastCPU, lastSysCPU, lastUserCPU;
static int numProcessors;
static HANDLE self;

void InitCPUUsage()
{
    SYSTEM_INFO sysInfo;
    FILETIME ftime, fsys, fuser;

    GetSystemInfo(&sysInfo);
    numProcessors = sysInfo.dwNumberOfProcessors;

    GetSystemTimeAsFileTime(&ftime);
    self = GetCurrentProcess();
    GetProcessTimes(self, &ftime, &ftime, &fsys, &fuser);

    memcpy(&lastCPU, &ftime, sizeof(FILETIME));
    memcpy(&lastSysCPU, &fsys, sizeof(FILETIME));
    memcpy(&lastUserCPU, &fuser, sizeof(FILETIME));
}

void UpdateCPUUsage() {
    if (!cpuInitialized) return;

    DWORD64 now = GetTickCount64();
    if (now - lastCPUTick >= 1000) {  // update every 1 second
        PdhCollectQueryData(cpuQuery);
        PDH_FMT_COUNTERVALUE counterVal;
        if (PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS) {
            g_cpuUsage = static_cast<float>(counterVal.doubleValue);
        }
        lastCPUTick = now;
    }
}

float GetCPUUsage() {
    PDH_FMT_COUNTERVALUE counterVal;

    PdhCollectQueryData(cpuQuery);
    Sleep(100);  // short delay to measure usage over time
    PdhCollectQueryData(cpuQuery);
    PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, NULL, &counterVal);

    return static_cast<float>(counterVal.doubleValue);
}

void UpdateMemoryUsage()
{
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo))
    {
        DWORDLONG totalPhys = memInfo.ullTotalPhys;
        DWORDLONG usedPhys = memInfo.ullTotalPhys - memInfo.ullAvailPhys;

        g_memoryUsage = (float(usedPhys) / float(totalPhys)) * 100.0f;
    }
}

inline float ImLengthSqr(const ImVec2& v) {
    return v.x * v.x + v.y * v.y;
}

inline ImVec2 ImVec2Subtract(const ImVec2& a, const ImVec2& b) {
    return ImVec2(a.x - b.x, a.y - b.y);
}

struct ScopedStyleVar {
    ScopedStyleVar(ImGuiStyleVar idx, float val) {
        ImGui::PushStyleVar(idx, val);
    }
    ~ScopedStyleVar() {
        ImGui::PopStyleVar();
    }
};

void generatePhotons(int count) {
    photons.clear();

    for (int i = 0; i < count; i++) {
        Photon p;
        p.bit = rand() % 2;
        p.originalBit = p.bit;
        p.basis = (rand() % 2) ? '+' : 'x';
        p.originalBasis = p.basis;
        p.x = startX - i * 40.0f; // staggered start
        photons.push_back(p);
    }
}

int measure(int bit, char sendBasis, char recvBasis) {
    if (sendBasis == recvBasis)
        return bit;
    return rand() % 2;
}

Photon eveIntercept(Photon p) {
    char eveBasis = (rand() % 2) ? '+' : 'x';
    int newBit = measure(p.bit, p.basis, eveBasis);

    p.bit = newBit;
    p.basis = eveBasis;
    p.intercepted = true;

    return p;
}

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output)
{
    output->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string FetchBB84Data(bool eve)
{
    std::string result;

    HINTERNET hSession = WinHttpOpen(L"BB84 Client",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", 5000, 0);

    std::wstring path = L"/bb84?n=50&eve=";
    path += eve ? L"true" : L"false";

    path += L"&backend=";

    if (qc_mode == ModeQiskit)
        path += L"simulator";
    else if (qc_mode == ModeIBM)
        path += L"ibm";
    else
        path += L"local";

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
        path.c_str(),
        NULL, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);

    WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0,
        0, 0);

    WinHttpReceiveResponse(hRequest, NULL);

    DWORD size = 0;
    do {
        DWORD downloaded = 0;
        WinHttpQueryDataAvailable(hRequest, &size);

        if (size == 0) break;

        std::vector<char> buffer(size);
        WinHttpReadData(hRequest, buffer.data(), size, &downloaded);

        result.append(buffer.begin(), buffer.begin() + downloaded);

    } while (size > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return result;
}

#include "json.hpp"
using json = nlohmann::json;

void LoadFromAPI(bool eve)
{
    std::string response = FetchBB84Data(eve);

    if (response.empty())
        return;

    qc_table.clear();
    qc_buffer.clear();
    animatedKey.clear();
    row_timer = 0.0f;
    auto parsed = json::parse(response);

   

    for (auto& item : parsed["data"])
    {
        QC_Row row;

        row.aliceBit = item["alice_bit"];
        row.aliceBasis = item["alice_basis"].get<std::string>()[0];
        row.bobBasis = item["bob_basis"].get<std::string>()[0];
        row.bobBit = item["bob_bit"];
        row.match = (row.aliceBasis == row.bobBasis);
        row.valid = row.match;

        qc_buffer.push_back(row);
    }

    qc_errorRate = parsed["error_rate"];
    qc_validBits = parsed["valid_bits"];
    qc_errors = parsed["errors"];
    qc_finalKey = parsed["final_key"];
}


int main(int, char**)
{
    ShowWindow(GetConsoleWindow(), SW_HIDE);

    if (!cpuInitialized) {
        PdhOpenQuery(NULL, NULL, &cpuQuery);
        PdhAddCounter(cpuQuery, L"\\Processor(_Total)\\% Processor Time", NULL, &cpuTotal);
        PdhCollectQueryData(cpuQuery);
        cpuInitialized = true;
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
    ::RegisterClassExW(&wc);

    RECT desktop;
    GetWindowRect(GetDesktopWindow(), &desktop);
    int width = desktop.right;
    int height = desktop.bottom;
    static bool show_window = true;

    HWND hwnd = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        wc.lpszClassName, L"ImGui - Anti-Cheat",
        WS_POPUP, 0, 0, width, height, nullptr, nullptr, wc.hInstance, nullptr);

    
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    if (!InitDesktopDuplication()) {
        MessageBoxA(nullptr, "Failed to init desktop duplication", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
     
        if (show_window)
        {
            ScopedStyleVar alpha(ImGuiStyleVar_Alpha, 1.0f);
            ImGui::Begin("Application is running!", &show_window);

            UpdateCPUUsage();
            ImGui::Text("CPU Usage: %.1f%%", g_cpuUsage);

            UpdateMemoryUsage();
            ImGui::Text("Memory Usage: %.1f%%", g_memoryUsage);

            ImGui::Text("Frametime: %.3f ms (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);

            if (qc_mode == ModeLocal)
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Mode: Local Simulation");
            else if (qc_mode == ModeQiskit)
                ImGui::TextColored(ImVec4(0, 0.7f, 1, 1), "Mode: Qiskit Simulator");
            else
                ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "Mode: IBM Quantum Hardware");
            const char* qc_modes[] = { "Local Simulation", "Qiskit Backend", "IBM Quantum Hardware" };
            ImGui::Combo("Quantum Mode", (int*)&qc_mode, qc_modes, IM_ARRAYSIZE(qc_modes));
            ImGui::Checkbox("Enable Eve (Eavesdropping)", &eveEnabled);
            if (qc_mode == ModeLocal)
            {
                if (!qc_running)
                {
                    if (ImGui::Button("Start Transmission"))
                    {
                        generatePhotons(20);
                        qc_table.clear();
                        qc_running = true;
                    }
                }
                else
                {
                    if (ImGui::Button("Stop Transmission"))
                    {
                        qc_running = false;
                    }
                }
            }
            else
            {
                if (ImGui::Button("Run Qiskit BB84"))
                {
                    qc_table.clear();
                    qc_buffer.clear();
                    apiPhotons.clear();
                    animatedKey.clear();
                    nextPhotonIndex = 0;
                    spawnTimer = 0.0f;
                    LoadFromAPI(eveEnabled);
                }
            }

                ImDrawList* draw = ImGui::GetWindowDrawList();
                ImVec2 origin = ImGui::GetCursorScreenPos();

                float currentTime = ImGui::GetTime();
                float deltaTime = currentTime - lastTime;
                lastTime = currentTime;

                draw->AddText(ImVec2(origin.x + startX, origin.y + yPos - 40),
                    IM_COL32_WHITE, "Alice");

                draw->AddText(ImVec2(origin.x + endX, origin.y + yPos - 40),
                    IM_COL32_WHITE, "Bob");

                if (qc_mode == ModeQiskit || qc_mode == ModeIBM)
                {
                    spawnTimer += ImGui::GetIO().DeltaTime;

                    if (nextPhotonIndex < qc_buffer.size() && spawnTimer >= spawnDelay)
                    {
                        spawnTimer = 0.0f;

                        Photon p;
                        p.x = startX;
                        p.active = true;
                        p.intercepted = false;

                        p.bit = qc_buffer[nextPhotonIndex].aliceBit;
                        p.basis = qc_buffer[nextPhotonIndex].aliceBasis;

                        apiPhotons.push_back(p);

                        nextPhotonIndex++;
                    }
                }

                if (qc_mode == ModeQiskit || qc_mode == ModeIBM)
                {
                    for (size_t i = 0; i < apiPhotons.size(); i++)
                    {
                        auto& p = apiPhotons[i];

                        if (!p.active) continue;

                        p.x += speed * ImGui::GetIO().DeltaTime;
                        float midX = (startX + endX) / 2.0f;

                        if (eveEnabled && !p.intercepted && p.x > midX)
                        {
                            char eveBasis = (rand() % 2) ? '+' : 'x';

                            if (eveBasis != p.basis)
                            {
                                p.bit = rand() % 2; // disturbance
                            }

                            p.basis = eveBasis;
                            p.intercepted = true;
                        }

                        ImVec2 pos(origin.x + p.x, origin.y + yPos);

                        ImU32 color;

                        // before Eve
                        if (!p.intercepted)
                        {
                            color = (p.basis == '+')
                                ? IM_COL32(100, 200, 255, 255)
                                : IM_COL32(200, 100, 255, 255);
                        }
                        else
                        {
                            color = IM_COL32(255, 80, 80, 255); // 🔴 intercepted
                        }

                        // glow
                        draw->AddCircleFilled(pos, 6.0f, color);
                        draw->AddCircle(pos, 9.0f, IM_COL32(255, 255, 255, 100));

                        // show bit
                        char text[2];
                        sprintf_s(text, "%d", p.bit);

                        draw->AddText(
                            ImVec2(pos.x - 4, pos.y - 6),
                            IM_COL32_WHITE,
                            text
                        );

                        if (p.intercepted)
                        {
                            draw->AddText(
                                ImVec2(pos.x - 10, pos.y - 20),
                                IM_COL32(255, 80, 80, 255),
                                "EVE"
                            );
                        }

                        // photon arrival
                        if (p.x > endX && !qc_buffer.empty())
                        {
                            QC_Row row = qc_buffer.front();
                            qc_buffer.erase(qc_buffer.begin());
                            qc_table.push_back(row);

                            // 🔐 KEY BUILD (SYNCED)
                            if (row.match)
                            {
                                animatedKey += std::to_string(row.bobBit);
                            }

                            p.active = false; // consume photon
                        }
                    }
                }

                if (eveEnabled)
                {
                    draw->AddText(
                        ImVec2(origin.x + (startX + endX) / 2 - 20, origin.y + yPos - 60),
                        IM_COL32(255, 100, 100, 255),
                        "Eve"
                    );
                }

                if (qc_mode == ModeLocal)
                {
                    for (auto& p : photons)
                    {
                        if (qc_running)
                            p.x += speed * deltaTime;

                        if (eveEnabled && !p.intercepted && p.x > (startX + endX) / 2)
                        {
                            p = eveIntercept(p);
                        }

                        ImVec2 pos(origin.x + p.x, origin.y + yPos);

                        ImU32 color = (p.basis == '+')
                            ? IM_COL32(100, 200, 255, 255)
                            : IM_COL32(200, 100, 255, 255);

                        if (p.intercepted)
                            color = IM_COL32(255, 80, 80, 255);

                        draw->AddCircleFilled(pos, 6.0f, color);

                        char text[2];
                        sprintf_s(text, "%d", p.bit);

                        draw->AddText(
                            ImVec2(pos.x - 4, pos.y - 6),
                            IM_COL32_WHITE,
                            text
                        );

                        if (p.x > endX)
                        {
                            char bobBasis = (rand() % 2) ? '+' : 'x';
                            int bobBit = measure(p.bit, p.basis, bobBasis);

                            QC_Row row;
                            row.aliceBit = p.originalBit;
                            row.aliceBasis = p.originalBasis;
                            row.bobBasis = bobBasis;
                            row.bobBit = bobBit;
                            row.match = (p.basis == bobBasis);

                            qc_table.push_back(row);

                            if (qc_table.size() > 50)
                                qc_table.erase(qc_table.begin());

                            p.x = startX;
                            p.intercepted = false;
                        }
                    }
                }


                ImGui::Dummy(ImVec2(800, 300));
                ImGui::Separator();
                ImGui::Text("BB84 Transmission Table");

                int valid = 0;
                int errors = 0;

                if (ImGui::BeginTable("bb84_table", 5))
                {
                    ImGui::TableSetupColumn("A Bit");
                    ImGui::TableSetupColumn("A Basis");
                    ImGui::TableSetupColumn("B Basis");
                    ImGui::TableSetupColumn("B Bit");
                    ImGui::TableSetupColumn("Match");
                    ImGui::TableHeadersRow();

                    for (size_t i = 0; i < qc_table.size(); i++)
                    {
                        auto& r = qc_table[i];

                        ImGui::TableNextRow();

                        if (r.match && r.aliceBit != r.bobBit)
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 80, 80, 120));

                        if (!r.match)
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(80, 80, 80, 60));

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%d", r.aliceBit);

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%c", r.aliceBasis);

                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%c", r.bobBasis);

                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%d", r.bobBit);

                        ImGui::TableSetColumnIndex(4);
                        ImGui::Text(r.match ? "Yes" : "No");

                        
                    }
                    ImGui::EndTable();
                }

                ImGui::Separator();

                
           
            ImGui::End();

            ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(1200, 200), ImGuiCond_FirstUseEver);
            ImGui::Begin("QKD Analysis Panel");
            ImGui::Text("BB84 Analysis");
            ImGui::Text("Buffer: %d | Table: %d", qc_buffer.size(), qc_table.size());
            if (qc_mode == ModeIBM)
            {
                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1),
                    "Running on real quantum hardware (noise and delays are normal)");
            }
            ImGui::Separator();
            // 🔥 MODE-BASED METRICS
            if (qc_mode == ModeQiskit || qc_mode == ModeIBM)
            {
                // ✅ USE BACKEND VALUES
                ImGui::Text("Valid Key Bits: %d", qc_validBits);
                ImGui::Text("Errors: %d", qc_errors);
                ImGui::Text("Error Rate: %.2f%%", qc_errorRate * 100.0f);

                if (qc_errorRate > 0.10f)
                {
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "⚠ EAVESDROPPING DETECTED");
                }
                else
                {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "SECURE CHANNEL");
                }
                // choose error source
                if (useManualAttack)
                    effectiveErrorRate = manualErrorPercent / 100.0f;
                else
                    effectiveErrorRate = qc_errorRate;

                currentAction = DecideMitigation(effectiveErrorRate);

                ImGui::Separator();
                ImGui::Text("Attack Simulation");

                ImGui::Checkbox("Manual Error Override", &useManualAttack);

                if (useManualAttack)
                {
                    ImGui::SliderFloat("Error %", &manualErrorPercent, 0.0f, 50.0f);
                }

                ImGui::Text("Effective Error: %.2f%%", effectiveErrorRate * 100.0f);
                switch (currentAction)
                {
                case Mitigate_None:
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Secure");
                    break;
                case Mitigate_Filter:
                    ImGui::Text("Filtering bits");
                    break;
                case Mitigate_ErrorCorrect:
                    ImGui::Text("Error correction");
                    break;
                case Mitigate_PrivacyAmplify:
                    ImGui::Text("Privacy amplification");
                    break;
                case Mitigate_Rotate:
                    ImGui::Text("Key rotation");
                    break;
                case Mitigate_Abort:
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "ABORT");
                    break;
                }
                if (currentAction == Mitigate_Abort)
                {
                    animatedKey.clear();
                }
                else if (currentAction == Mitigate_Rotate)
                {
                    if (ImGui::Button("Rotate Key"))
                    {
                        qc_table.clear();
                        qc_buffer.clear();
                        apiPhotons.clear();
                        animatedKey.clear();

                        nextPhotonIndex = 0;
                        spawnTimer = 0.0f;

                        LoadFromAPI(eveEnabled);
                    }
                }
                mitigationTimer += ImGui::GetIO().DeltaTime;

                if (mitigationTimer >= mitigationInterval)
                {
                    mitigationTimer = 0.0f;

                    mitigationHistory.push_back(MitigationToFloat(currentAction));

                    if (mitigationHistory.size() > 100)
                        mitigationHistory.erase(mitigationHistory.begin());
                }
                ImGui::Separator();
                ImGui::Text("Final Shared Key (Live):");
                static float blink = 0.0f;
                blink += ImGui::GetIO().DeltaTime;

                std::string display = animatedKey;
                if (((int)(blink * 2)) % 2 == 0)
                    display += "_";

                ImGui::TextWrapped("%s", display.c_str());
                ImGui::Separator();
                ImGui::Text("Mitigation Timeline");

                if (!mitigationHistory.empty())
                {
                    ImGui::PlotLines(
                        "##mitigation_graph",
                        mitigationHistory.data(),
                        mitigationHistory.size(),
                        0,
                        NULL,
                        0.0f,
                        5.0f,
                        ImVec2(350, 120)
                    );
                }
                ImGui::Text("Legend:");
                ImGui::Text("0 = Secure");
                ImGui::Text("1 = Filter");
                ImGui::Text("2 = Error Correction");
                ImGui::Text("3 = Privacy Amplification");
                ImGui::Text("4 = Key Rotation");
                ImGui::Text("5 = Abort");
            }
            else
            {
                // ✅ LOCAL CALCULATION (your original logic)
                int sampleSize = qc_table.size() / 3;

                if (qc_table.size() < 3)
                    sampleSize = qc_table.size();

                int valid = 0;
                int errors = 0;

                for (int i = 0; i < sampleSize; i++)
                {
                    auto& r = qc_table[i];

                    if (r.match)
                    {
                        valid++;
                        if (r.aliceBit != r.bobBit)
                            errors++;
                    }
                }

                float errorRate = (valid > 0) ? (float)errors / valid : 0.0f;

                ImGui::Text("Valid Key Bits: %d", valid);
                ImGui::Text("Errors: %d", errors);
                ImGui::Text("Error Rate: %.2f%%", errorRate * 100.0f);
               
                if (errorRate > 0.10f)
                {
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "⚠ EAVESDROPPING DETECTED");
                }
                else
                {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "SECURE CHANNEL");
                }
                if (eveEnabled)
                {
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Eve: ACTIVE (Interception ON)");
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "Eve: OFF (Secure Channel)");
                }
            }

            ImGui::End();

        }

            if (!show_window)
                PostQuitMessage(0);

        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_desktopSRV) { g_desktopSRV->Release(); g_desktopSRV = nullptr; }
    g_desktopTexture.Reset();
    g_duplication.Reset();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
