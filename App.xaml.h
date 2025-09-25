#pragma once

#include "App.xaml.g.h"
#include <thread>

namespace winrt::Winvert4::implementation
{
    struct App : AppT<App>
    {
        App();
        ~App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        winrt::Microsoft::UI::Xaml::Window window{ nullptr };
        int m_hotkeyId{ 1 };
        std::thread m_messageLoopThread;
    };
}
