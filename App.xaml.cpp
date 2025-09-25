#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::Winvert4::implementation
{
    App::App()
    {
        InitializeComponent();
    }

    App::~App() {}

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        window = make<MainWindow>();
        //window.Activate();
    }
}