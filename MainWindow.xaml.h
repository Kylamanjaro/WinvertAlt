#pragma once
#include "MainWindow.g.h"
#include "EffectWindow.h"
#include <vector>
#include <memory>

namespace winrt::Winvert4::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        int32_t MyProperty();
        void MyProperty(int32_t value);

        void ToggleSnipping();
        void StartScreenSelection();

        void OnPointerPressed(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {}
        void OnPointerMoved(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {}
        void OnPointerReleased(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {}

    private:
        static LRESULT CALLBACK SelectionWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        bool   m_isSelecting{ false };
        bool   m_isDragging{ false };
        HWND   m_selectionHwnd{ nullptr };
        POINT  m_ptStart{ 0, 0 };
        POINT  m_ptEnd{ 0, 0 };

        HBITMAP m_screenBmp{ nullptr };
        HDC     m_screenMemDC{ nullptr };
        SIZE    m_screenSize{ 0, 0 };
        POINT   m_virtualOrigin{ 0, 0 };

        void CaptureScreenBitmap();
        void ReleaseScreenBitmap();
        RECT MakeRectFromPoints(POINT a, POINT b) const;
        void OnSelectionCompleted(RECT sel);

        std::vector<std::unique_ptr<EffectWindow>> m_effectWindows;
    };
}

namespace winrt::Winvert4::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
