#pragma once
#include "App.xaml.g.h"

namespace winrt::Winvert4::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        winrt::Microsoft::UI::Xaml::Window m_window{ nullptr };
    };
}

namespace winrt::Winvert4::factory_implementation
{
    struct App : winrt::Winvert4::implementation::AppT<App> {};
}
