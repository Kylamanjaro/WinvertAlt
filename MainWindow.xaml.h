#pragma once
#include "MainWindow.g.h"

namespace winrt::Winvert4::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        // Optional placeholders if referenced elsewhere
        int32_t MyProperty();
        void MyProperty(int32_t value);

        // Entry point to start snipping; called from App hotkey
        void ToggleSnipping();

        // Snipping-tool style selection (Win32 overlay)
        void StartScreenSelection();

        // (If your XAML still references these, keep as no-ops)
        void OnPointerPressed(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {}
        void OnPointerMoved(winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {}
        void OnPointerReleased(winrt::Windows::Foundation::IInspectable const&,
                               winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {}

    private:
        // ----- Selection overlay (Win32) -----
        static LRESULT CALLBACK SelectionWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        // State
        bool   m_isSelecting{ false };
        bool   m_isDragging{ false };
        HWND   m_selectionHwnd{ nullptr };
        POINT  m_ptStart{ 0, 0 };
        POINT  m_ptEnd{ 0, 0 };
        RECT   m_lastSelection{ 0,0,0,0 };

        // Screenshot backing for the "freeze" effect
        HBITMAP m_screenBmp{ nullptr };
        HDC     m_screenMemDC{ nullptr };
        SIZE    m_screenSize{ 0, 0 };
        POINT   m_virtualOrigin{ 0, 0 }; // SM_XVIRTUALSCREEN/SM_YVIRTUALSCREEN

        void CaptureScreenBitmap();
        void ReleaseScreenBitmap();
        RECT MakeRectFromPoints(POINT a, POINT b) const;
        void OnSelectionCompleted(RECT sel); // post-selection hook
    };
}

namespace winrt::Winvert4::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
