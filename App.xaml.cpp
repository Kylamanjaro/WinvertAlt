#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

#if __has_include("App.xaml.g.cpp")
#include "App.xaml.g.cpp"
#endif

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace
{
    static bool IsTestOpenSettingsOnLaunch()
    {
        wchar_t buf[32] = { 0 };
        DWORD n = GetEnvironmentVariableW(L"WINVERT_TEST_OPEN_SETTINGS", buf, (DWORD)std::size(buf));
        if (n == 0) return false;
        std::wstring v(buf, buf + wcsnlen_s(buf, std::size(buf)));
        for (auto& ch : v) ch = (wchar_t)towlower(ch);
        return (v == L"1" || v == L"true" || v == L"on" || v == L"yes");
    }
}

namespace winrt::Winvert4::implementation
{
    App::App()
    {
        // Xaml objects should not call InitializeComponent during construction.
        // See https://github.com/microsoft/cppwinrt/tree/master/nuget#initializecomponent
    
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        m_window = make<winrt::Winvert4::implementation::MainWindow>();
        m_window.Activate();

        // Hide the window immediately after activation so the app runs
        // headless until the user presses the global hotkey to begin
        // selection (Snipping Tool–style behavior). In test mode, keep it
        // visible so automation can reach the control panel.
        if (auto windowNative = m_window.try_as<::IWindowNative>())
        {
            HWND hwnd{};
            if (SUCCEEDED(windowNative->get_WindowHandle(&hwnd)) && hwnd)
            {
                if (!IsTestOpenSettingsOnLaunch())
                {
                    ::ShowWindow(hwnd, SW_HIDE);
                }
            }
        }
    }
}
