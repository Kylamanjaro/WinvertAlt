#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "Log.h"

#if __has_include("App.xaml.g.cpp")
#include "App.xaml.g.cpp"
#endif

using namespace winrt;

namespace
{
    constexpr wchar_t kHotkeyWndClass[] = L"Winvert4_HotkeySink";
    constexpr int kHotkeyId = 1;
}

ATOM winrt::Winvert4::implementation::App::s_hotkeyAtom = 0;
winrt::Winvert4::implementation::App* winrt::Winvert4::implementation::App::s_current = nullptr;

namespace winrt::Winvert4::implementation
{
    App::App()
    {
        winvert4::Log("App: ctor start");
        s_current = this;
        m_outputManager = std::make_unique<OutputManager>();
        HRESULT hrOM = m_outputManager->Initialize();
        winvert4::Logf("App: OutputManager initialized hr=0x%08X", hrOM);

        InitializeComponent();
        winvert4::Log("App: ctor end");
    }

    App::~App()
    {
        UnregisterHotkey();
        if (m_hotkeyHwnd)
        {
            DestroyWindow(m_hotkeyHwnd);
            m_hotkeyHwnd = nullptr;
        }
    }

    App* App::Current()
    {
        return s_current;
    }

    OutputManager* App::GetOutputManager()
    {
        return m_outputManager.get();
    }

    void App::OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        winvert4::Log("App: OnLaunched");
        // Create and show the main window
        m_window = make<winrt::Winvert4::implementation::MainWindow>();
        winvert4::Log("App: MainWindow created");
        //m_window.Activate();

        // Create a hidden receiver for WM_HOTKEY and register Win+Shift+I
        EnsureHotkeyWindow();
        RegisterHotkey();
        winvert4::Log("App: OnLaunched done");
    }

    void App::EnsureHotkeyWindow()
    {
        if (!s_hotkeyAtom)
        {
            WNDCLASSEXW wcex{ sizeof(WNDCLASSEXW) };
            wcex.style         = CS_HREDRAW | CS_VREDRAW;
            wcex.lpfnWndProc   = &App::HotkeyWndProc;
            wcex.hInstance     = GetModuleHandleW(nullptr);
            wcex.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
            wcex.lpszClassName = kHotkeyWndClass;
            wcex.hbrBackground = nullptr;
            s_hotkeyAtom = RegisterClassExW(&wcex);
            winvert4::Logf("App: registered hotkey wnd class atom=%u", (unsigned)s_hotkeyAtom);
        }

        if (!m_hotkeyHwnd)
        {
            // Message-only window so it never shows
            m_hotkeyHwnd = CreateWindowExW(
                0, kHotkeyWndClass, L"", 0,
                0, 0, 0, 0,
                HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
                this // pass App* for association
            );
            winvert4::Logf("App: created hotkey HWND=%p", (void*)m_hotkeyHwnd);
        }
    }

    void App::RegisterHotkey()
    {
        if (!m_hotkeyHwnd) return;

        // Win + Shift + I
        const UINT fsModifiers = MOD_WIN | MOD_SHIFT;
        const UINT vk = 'I';
        BOOL ok = RegisterHotKey(m_hotkeyHwnd, kHotkeyId, fsModifiers, vk);
        winvert4::Logf("App: RegisterHotKey result=%d hwnd=%p", (int)ok, (void*)m_hotkeyHwnd);
    }

    void App::UnregisterHotkey()
    {
        if (m_hotkeyHwnd)
        {
            UnregisterHotKey(m_hotkeyHwnd, kHotkeyId);
        }
    }

    LRESULT CALLBACK App::HotkeyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_NCCREATE:
        {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<App*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            winvert4::Logf("App: HotkeyWndProc NCCREATE self=%p", (void*)self);
            return TRUE;
        }

        case WM_HOTKEY:
        {
            if (!self) break;

            if (static_cast<int>(wParam) == kHotkeyId)
            {
                winvert4::Log("App: WM_HOTKEY received, enqueue ToggleSnipping");
                // Invoke snipping on the UI thread
                self->m_window.DispatcherQueue().TryEnqueue([w = self->m_window]()
                {
                    if (!w) return;
                    auto mw = w.as<winrt::Winvert4::MainWindow>(); // projected type
                    auto impl = winrt::get_self<winrt::Winvert4::implementation::MainWindow>(mw);
                    impl->ToggleSnipping();
                });
            }
            return 0;
        }

        case WM_DESTROY:
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
