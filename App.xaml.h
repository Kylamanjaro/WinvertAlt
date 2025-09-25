#pragma once
#include "App.xaml.g.h"

namespace winrt::Winvert4::implementation
{
    struct App : AppT<App>
    {
        App();
        ~App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        // The app's main window
        Microsoft::UI::Xaml::Window m_window{ nullptr };

        // Hidden message-only window to receive WM_HOTKEY
        HWND m_hotkeyHwnd{ nullptr };
        static ATOM s_hotkeyAtom;

        static LRESULT CALLBACK HotkeyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        void EnsureHotkeyWindow();
        void RegisterHotkey();
        void UnregisterHotkey();
    };
}

namespace winrt::Winvert4::factory_implementation
{
    struct App : winrt::Winvert4::implementation::AppT<App> {};
}
