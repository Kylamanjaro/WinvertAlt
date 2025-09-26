#pragma once
#include "App.xaml.g.h"
#include "OutputManager.h"

namespace winrt::Winvert4::implementation
{
    struct App : AppT<App>
    {
        App();
        ~App();

        void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

        static App* Current();
        OutputManager* GetOutputManager();

    private:
        winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };
        HWND m_hotkeyHwnd{ nullptr };
        static ATOM s_hotkeyAtom;

        static App* s_current;
        std::unique_ptr<OutputManager> m_outputManager;

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
