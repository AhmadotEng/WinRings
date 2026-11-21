#include "ShellController.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../shared/WinRingsApi.h"
#include "../shared/WinRingsConfig.h"
#include "../shared/WinRingsIdentity.h"
#include "AppIndex.h"
#include "InputEngine.h"
#include "RingOverlay.h"
#include "SlotModel.h"
#include "WindowTracker.h"

namespace winrings
{
    void ShellDiagLog(const wchar_t* tag, const wchar_t* fmt, ...) noexcept
    {
        try
        {
            static std::mutex s_logMutex;
            std::lock_guard<std::mutex> lock(s_logMutex);

            wchar_t body[1024]{};
            va_list ap{};
            va_start(ap, fmt);
            _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt ? fmt : L"", ap);
            va_end(ap);

            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t lineW[1280]{};
            _snwprintf_s(lineW, _countof(lineW), _TRUNCATE,
                L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [pid=%lu tid=%lu] [%s] %s\r\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                GetCurrentProcessId(), GetCurrentThreadId(),
                tag ? tag : L"Shell", body);

            const std::wstring dir = SwitcherConfig::AppDataDirectory();
            if (dir.empty())
                return;
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            const std::wstring logPath = dir + L"\\shell_diag.log";

            DWORD disposition = OPEN_ALWAYS;
            WIN32_FILE_ATTRIBUTE_DATA attr{};
            if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &attr))
            {
                const ULONGLONG sz = (static_cast<ULONGLONG>(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
                if (sz > 262144ull)
                    disposition = CREATE_ALWAYS;
            }

            HANDLE file = CreateFileW(logPath.c_str(), FILE_APPEND_DATA | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return;

            LARGE_INTEGER zero{};
            SetFilePointerEx(file, zero, nullptr, FILE_END);
            const std::string utf8 = WideToUtf8(lineW);
            if (!utf8.empty())
            {
                DWORD written = 0;
                WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            }
            CloseHandle(file);
        }
        catch (...) {}
    }

    namespace
    {
        constexpr UINT_PTR kWatchdogTimerId = 1;
        constexpr UINT kWatchdogIntervalMs = 1000;
        constexpr wchar_t kPreviewControlClassW[] = L"WinRings.Preview.Control.v1";

        bool IsCurrentProcessExplorer() noexcept
        {
            wchar_t path[MAX_PATH]{};
            const DWORD len = GetModuleFileNameW(nullptr, path, _countof(path));
            if (!len || len >= _countof(path))
                return false;
            const wchar_t* slash = wcsrchr(path, L'\\');
            const wchar_t* name = slash ? (slash + 1) : path;
            return _wcsicmp(name, L"explorer.exe") == 0;
        }

        std::wstring ResolveSiblingPath(HMODULE mod, const wchar_t* fileName)
        {
            std::wstring path(MAX_PATH, L'\0');
            DWORD len = GetModuleFileNameW(mod, path.data(), static_cast<DWORD>(path.size()));
            while (len >= path.size() && path.size() < 32768)
            {
                path.resize(path.size() * 2);
                len = GetModuleFileNameW(mod, path.data(), static_cast<DWORD>(path.size()));
            }
            if (!len)
                return {};
            path.resize(len);
            const size_t slash = path.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
                return {};
            return path.substr(0, slash + 1) + fileName;
        }

        void LaunchToolsExecutable(HMODULE mod, const std::wstring& args)
        {
            const std::wstring exe = ResolveSiblingPath(mod, WINRINGS_TOOLS_FILENAME_W);
            if (exe.empty())
                return;
            std::wstring cmdLine = L"\"" + exe + L"\"";
            if (!args.empty())
            {
                cmdLine.push_back(L' ');
                cmdLine += args;
            }
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
            mutableCmd.push_back(L'\0');
            if (CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                0, nullptr, nullptr, &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }

        POINT GetActiveMonitorCenterPhysical() noexcept
        {
            POINT pt{};
            GetCursorPos(&pt);
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(mon, &mi))
            {
                return POINT{
                    (mi.rcWork.left + mi.rcWork.right) / 2,
                    (mi.rcWork.top + mi.rcWork.bottom) / 2
                };
            }
            return pt;
        }

        class ControllerRuntime
        {
        public:
            ControllerRuntime(HMODULE shellModule, bool previewMode)
                : m_shellModule(shellModule)
                , m_previewMode(previewMode)
            {
            }

            ~ControllerRuntime()
            {
                CleanupHostHandle();
            }

            int Run()
            {
                const HRESULT hrOle = OleInitialize(nullptr);

                m_statusMessage = RegisterWindowMessageW(WINRINGS_SHELL_STATUS_MESSAGE_W);
                m_commandMessage = RegisterWindowMessageW(WINRINGS_SHELL_COMMAND_MESSAGE_W);
                m_shellHookMessage = RegisterWindowMessageW(L"SHELLHOOK");

                const wchar_t* clsName = m_previewMode ? kPreviewControlClassW : WINRINGS_SHELL_CONTROL_CLASS_W;
                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = ControlWndProc;
                wc.hInstance = m_shellModule;
                wc.lpszClassName = clsName;
                RegisterClassExW(&wc);

                m_controlHwnd = CreateWindowExW(
                    0, clsName, L"", 0,
                    0, 0, 0, 0,
                    HWND_MESSAGE, nullptr, m_shellModule, this);
                if (!m_controlHwnd)
                {
                    if (SUCCEEDED(hrOle)) OleUninitialize();
                    return 1;
                }

                if (m_previewMode)
                {
                    Arm(true);
                    const POINT center = GetActiveMonitorCenterPhysical();
                    OpenRingAt(center.x, center.y, true);
                }
                else
                {
                    AcquireHostHandshake();
                    SetTimer(m_controlHwnd, kWatchdogTimerId, kWatchdogIntervalMs, nullptr);
                    Arm(false);
                }

                MSG msg{};
                while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                Disarm(false);
                if (m_controlHwnd && IsWindow(m_controlHwnd))
                {
                    DestroyWindow(m_controlHwnd);
                    m_controlHwnd = nullptr;
                }
                CleanupHostHandle();
                if (SUCCEEDED(hrOle))
                    OleUninitialize();
                return 0;
            }

        private:
            static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept
            {
                auto* self = reinterpret_cast<ControllerRuntime*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (msg == WM_NCCREATE)
                {
                    self = static_cast<ControllerRuntime*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                    self->m_controlHwnd = hwnd;
                }
                if (self)
                {
                    try { return self->HandleControlMessage(hwnd, msg, wp, lp); }
                    catch (...) {}
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            LRESULT HandleControlMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
            {
                if (msg == m_shellHookMessage)
                {
                    if (m_armed)
                        m_tracker.OnShellHookMessage(wp, lp);
                    return 0;
                }

                if (msg == m_commandMessage && !m_previewMode)
                {
                    switch (static_cast<UINT32>(wp))
                    {
                    case WINRINGS_CMD_TOGGLE_RING:
                        if (m_armed)
                            ToggleStickyRing();
                        return TRUE;

                    case WINRINGS_CMD_EXIT:
                        Disarm(true);
                        return TRUE;

                    case WINRINGS_CMD_SUSPEND_INPUT:
                        m_input.SetSuspended(true);
                        if (m_overlay.IsOpen())
                            m_overlay.Dismiss();
                        return TRUE;

                    case WINRINGS_CMD_REFRESH_INPUT:
                        m_input.SetSuspended(false);
                        m_input.ResetTransientState();
                        return TRUE;

                    case WINRINGS_CMD_RELOAD_CONFIG:
                        ReloadConfiguration();
                        return TRUE;

                    case WINRINGS_CMD_QUERY_SHELL_STATE:
                        return static_cast<LRESULT>(m_armed ? WINRINGS_STATE_RING_READY : 0u);

                    case WINRINGS_CMD_REATTACH:
                        HandleReattach(reinterpret_cast<HWND>(lp));
                        return TRUE;

                    default:
                        return FALSE;
                    }
                }

                if (msg == WM_TIMER)
                {
                    if (wp == 9001)
                    {
                        if (m_armed)
                            m_tracker.OnGlideTimerTick();
                        return 0;
                    }
                    if (wp == kWatchdogTimerId && !m_previewMode)
                    {
                        CheckHostWatchdog();
                        return 0;
                    }
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            }

            void CleanupHostHandle() noexcept
            {
                if (m_hostProcess)
                {
                    CloseHandle(m_hostProcess);
                    m_hostProcess = nullptr;
                }
            }

            void AcquireHostHandshake(HWND explicitHostWindow = nullptr)
            {
                CleanupHostHandle();
                m_hostWindow = explicitHostWindow;
                m_attemptCookie = 0;

                if (!m_hostWindow)
                {
                    for (int i = 0; i < 15 && !m_hostWindow; ++i)
                    {
                        m_hostWindow = FindWindowW(WINRINGS_HOST_WINDOW_CLASS_W, WINRINGS_HOST_WINDOW_TITLE_W);
                        if (!m_hostWindow)
                            Sleep(40);
                    }
                }
                if (m_hostWindow && IsWindow(m_hostWindow))
                {
                    m_attemptCookie = reinterpret_cast<UINT_PTR>(
                        GetPropW(m_hostWindow, WINRINGS_HOST_ATTEMPT_COOKIE_PROP_W));
                    DWORD hostPid = 0;
                    GetWindowThreadProcessId(m_hostWindow, &hostPid);
                    if (hostPid)
                        m_hostProcess = OpenProcess(SYNCHRONIZE, FALSE, hostPid);
                }
            }

            void PostStatusToHost(WinRingsDllStatus status) noexcept
            {
                if (m_previewMode || !m_hostWindow || !IsWindow(m_hostWindow) || !m_attemptCookie || !m_statusMessage)
                    return;
                const UINT64 packed = (static_cast<UINT64>(GetCurrentProcessId()) << 32) | static_cast<UINT64>(status);
                PostMessageW(m_hostWindow, m_statusMessage,
                    static_cast<WPARAM>(packed),
                    static_cast<LPARAM>(m_attemptCookie));
            }

            void CheckHostWatchdog()
            {
                if (!m_armed)
                    return;
                bool hostGone = false;
                if (!m_hostWindow || !IsWindow(m_hostWindow))
                    hostGone = true;
                else if (m_hostProcess && WaitForSingleObject(m_hostProcess, 0) == WAIT_OBJECT_0)
                    hostGone = true;

                if (hostGone)
                    Disarm(false);
            }

            void Arm(bool forPreview)
            {
                if (m_armed)
                    return;

                m_config = SwitcherConfig::LoadOrCreate();
                m_tracker.Initialize(m_controlHwnd);

                RingOverlay::Callbacks overlayCb{};
                overlayCb.launchTools = [this](const std::wstring& args) {
                    LaunchToolsExecutable(m_shellModule, args);
                };
                overlayCb.saveAndReloadConfig = [this](const SwitcherConfig& updated) {
                    updated.Save();
                    m_config = updated;
                    ApplyLoadedConfig();
                };
                overlayCb.onExitRequested = [this]() {
                    if (m_previewMode)
                        PostQuitMessage(0);
                    else if (m_controlHwnd)
                        PostMessageW(m_controlHwnd, m_commandMessage, WINRINGS_CMD_EXIT, 0);
                };
                overlayCb.onOverlayClosed = [this]() {
                    m_input.ResetTransientState();
                    m_tracker.CancelPointerGlide();
                    if (m_previewMode)
                        PostQuitMessage(0);
                };

                if (!m_overlay.Initialize(m_shellModule, m_shellModule, &m_tracker, std::move(overlayCb), &m_appIndex))
                {
                    m_tracker.Shutdown();
                    if (!forPreview)
                        PostStatusToHost(WINRINGS_DLL_STATUS_FAILED);
                    return;
                }
                m_appIndex.StartAsyncRefresh();
                m_overlay.SetConfig(m_config);

                InputEngine::Callbacks inputCb{};
                inputCb.isRingOpen = [this]() { return m_overlay.IsOpen(); };
                inputCb.isRingLockedOpen = [this]() { return m_overlay.IsLockedOpen(); };
                inputCb.isRingDragActive = [this]() { return m_overlay.IsDragOrReorderActive(); };
                inputCb.isPointInOverlay = [this](POINT pt) { return m_overlay.IsPointInActiveBounds(pt); };
                inputCb.getOverlayDpiScale = [this]() { return m_overlay.DpiScale(); };
                inputCb.onTriggerPressed = [this](int x, int y) {
                    const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    POINT openPt{ x, y };
                    if (shiftHeld && m_config.shiftCenterOnActiveMonitor)
                        openPt = GetActiveMonitorCenterPhysical();
                    OpenRingAt(openPt.x, openPt.y, false);
                };
                inputCb.onTriggerReleased = [this](int x, int y, bool isTap) {
                    if (!m_overlay.IsOpen())
                        return;
                    if (isTap)
                    {
                        m_overlay.LockOpen();
                    }
                    else
                    {
                        m_overlay.UpdatePointer(x, y);
                        m_overlay.CommitSelectionAndClose();
                    }
                };
                inputCb.onDoubleTapLastApp = [this]() {
                    if (m_overlay.IsOpen())
                        m_overlay.Dismiss();
                    HWND prev = m_tracker.PreviousActiveWindow();
                    if (!prev || !IsWindow(prev))
                        prev = m_tracker.LastActiveWindow();
                    if (prev && IsWindow(prev))
                    {
                        const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        m_tracker.ActivateWindow(prev,
                            shiftHeld && m_config.shiftCenterOnActiveMonitor,
                            m_config.pointerFollow);
                    }
                };
                inputCb.onMouseMoved = [this](int x, int y) {
                    if (m_overlay.IsOpen())
                        m_overlay.UpdatePointer(x, y);
                };
                inputCb.onMouseWheel = [this](int delta) {
                    if (m_overlay.IsOpen())
                        m_overlay.HandleScrollWheel(delta);
                };
                inputCb.onCancelRequested = [this]() {
                    if (m_overlay.IsOpen())
                        m_overlay.HandleEscapeKey();
                };
                inputCb.onToggleRequested = [this]() {
                    ToggleStickyRing();
                };
                inputCb.onCommitRequested = [this]() {
                    if (m_overlay.IsOpen())
                        m_overlay.CommitSelectionAndClose();
                };
                inputCb.onArrowStep = [this](int delta) {
                    if (m_overlay.IsOpen())
                        m_overlay.HandleScrollWheel(delta);
                };
                inputCb.onFilterChar = [this](wchar_t ch) {
                    if (m_overlay.IsOpen())
                        m_overlay.AppendFilterChar(ch);
                };
                inputCb.onFilterBackspace = [this]() {
                    if (m_overlay.IsOpen())
                        m_overlay.BackspaceFilter();
                };
                inputCb.onUserPointerDelta = [this]() {
                };

                m_input.Install(m_config, std::move(inputCb));
                m_input.SetSuspended(false);
                m_armed = true;

                if (!forPreview)
                {
                    ShellDiagLog(L"ShellCtrl", L"Arm: Radial Switcher ready (theme=%s glass=%d tone=%d)",
                        ToString(m_config.theme), m_config.glassLevel, m_config.lightDarkMode);
                    PostStatusToHost(WINRINGS_DLL_STATUS_READY);
                }
            }

            void Disarm(bool notifyHost)
            {
                if (!m_armed)
                {
                    if (notifyHost)
                        PostStatusToHost(WINRINGS_DLL_STATUS_DISARMED);
                    return;
                }

                ShellDiagLog(L"ShellCtrl", L"Disarm(notifyHost=%d)", notifyHost ? 1 : 0);

                m_armed = false;
                m_input.Uninstall();
                if (m_overlay.IsOpen())
                    m_overlay.Dismiss();
                m_overlay.Shutdown();
                m_appIndex.Shutdown();
                m_tracker.Shutdown();

                if (notifyHost && !m_previewMode)
                    PostStatusToHost(WINRINGS_DLL_STATUS_DISARMED);
            }

            void HandleReattach(HWND newHostWindow)
            {
                Disarm(false);
                AcquireHostHandshake(newHostWindow);
                Arm(false);
            }

            void OpenRingAt(int x, int y, bool lockOpen)
            {
                m_tracker.RecordForegroundBeforeOpen(m_overlay.Hwnd());
                m_appIndex.EnsureFreshAsync();
                auto slots = SlotModel::BuildRingSlots(
                    m_overlay.Hwnd(), m_config, m_tracker, m_tracker.WicFactory(), &m_appIndex);
                m_overlay.OpenAt(x, y, std::move(slots), lockOpen);
            }

            void ToggleStickyRing()
            {
                if (m_overlay.IsOpen())
                {
                    m_overlay.Dismiss();
                    return;
                }
                POINT pt{};
                GetCursorPos(&pt);
                const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                if (shiftHeld && m_config.shiftCenterOnActiveMonitor)
                    pt = GetActiveMonitorCenterPhysical();
                OpenRingAt(pt.x, pt.y, true);
            }

            void ReloadConfiguration()
            {
                if (!m_armed)
                    return;
                m_config = SwitcherConfig::LoadOrCreate();
                ShellDiagLog(L"ShellCtrl", L"ReloadConfiguration: theme=%s glass=%d tone=%d",
                    ToString(m_config.theme), m_config.glassLevel, m_config.lightDarkMode);
                ApplyLoadedConfig();
            }

            void ApplyLoadedConfig()
            {
                m_overlay.SetConfig(m_config);
                m_input.UpdateConfig(m_config);
            }

            HMODULE m_shellModule = nullptr;
            bool m_previewMode = false;
            bool m_armed = false;

            HWND m_controlHwnd = nullptr;
            HWND m_hostWindow = nullptr;
            HANDLE m_hostProcess = nullptr;
            UINT_PTR m_attemptCookie = 0;

            UINT m_statusMessage = 0;
            UINT m_commandMessage = 0;
            UINT m_shellHookMessage = 0;

            SwitcherConfig m_config;
            AppIndex m_appIndex;
            WindowTracker m_tracker;
            RingOverlay m_overlay;
            InputEngine m_input;
        };

        std::atomic<bool> g_explorerStarted{ false };

        DWORD WINAPI ExplorerUiThreadProc(void* param)
        {
            HMODULE shellMod = static_cast<HMODULE>(param);
            ControllerRuntime runtime(shellMod, false);
            return static_cast<DWORD>(runtime.Run());
        }
    } // namespace

    void ShellController::BootstrapInExplorer(HMODULE shellModule)
    {
        if (!IsCurrentProcessExplorer())
            return;

        if (g_explorerStarted.exchange(true, std::memory_order_acq_rel))
            return;

        // Pin WinRingsShell.dll inside explorer.exe for its lifetime; --exit only disarms.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&ShellController::BootstrapInExplorer),
            &pinned);

        HANDLE th = CreateThread(nullptr, 0, ExplorerUiThreadProc, shellModule, 0, nullptr);
        if (th)
            CloseHandle(th);
        else
            g_explorerStarted.store(false, std::memory_order_release);
    }

    int ShellController::RunStandalonePreview(HMODULE shellModule)
    {
        ControllerRuntime runtime(shellModule, true);
        return runtime.Run();
    }

    bool ShellController::RequestShutdownFromApi(HWND controlWindow) noexcept
    {
        if (!controlWindow || !IsWindow(controlWindow))
            return false;
        const UINT cmdMsg = RegisterWindowMessageW(WINRINGS_SHELL_COMMAND_MESSAGE_W);
        if (!cmdMsg)
            return false;
        return PostMessageW(controlWindow, cmdMsg, WINRINGS_CMD_EXIT, 0) != FALSE;
    }
} // namespace winrings
