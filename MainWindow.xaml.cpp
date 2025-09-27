#include "pch.h"
#include "MainWindow.xaml.h"
#include "Log.h"
#include "OutputManager.h"
#include <winrt/Microsoft.UI.Windowing.h>
#include <gdiplus.h>
#include <microsoft.ui.xaml.window.h>

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::Foundation;

namespace
{
    constexpr wchar_t kSelectionWndClass[] = L"Winvert4_SelectionOverlayWindow";
    constexpr int HOTKEY_INVERT_ID = 1;
    constexpr int HOTKEY_GRAYSCALE_ID = 2;
    constexpr int HOTKEY_REMOVE_ID = 3;
}

std::vector<RECT> winrt::Winvert4::implementation::MainWindow::s_monitorRects;

namespace winrt::Winvert4::implementation
{
    MainWindow::MainWindow()
    {
        winvert4::Log("MainWindow: ctor start");

        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, NULL);

        InitializeComponent();
        this->Title(L"Winvert Control Panel");

        auto windowNative{ this->try_as<::IWindowNative>() };
        winrt::check_hresult(windowNative->get_WindowHandle(&m_mainHwnd));
        winvert4::Logf("MainWindow: got HWND=%p", (void*)m_mainHwnd);

        m_outputManager = std::make_unique<OutputManager>();
        HRESULT hrOM = m_outputManager->Initialize();
        winvert4::Logf("MainWindow: OutputManager initialized hr=0x%08X", hrOM);

        // TODO: Load settings from a file
        EnumerateMonitors();

        m_brightnessOnIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/glasses_on.svg"));
        m_brightnessOffIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/glasses_off.svg"));
        m_hideIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/hide.svg"));
        m_showIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/show.svg"));
        m_invertOnIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/invert_on.svg"));
        m_invertOffIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/invert_off.svg"));
        m_grayscaleOnIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/grayscale_on.svg"));
        m_grayscaleOffIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/grayscale_off.svg"));

        // Keep the filters flyout open while toggling items
        if (auto flyout = FiltersMenuFlyout())
        {
            flyout.Closing({ this, &MainWindow::FiltersMenuFlyout_Closing });
        }

        SetWindowSubclass(m_mainHwnd, &MainWindow::WindowSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

        RegisterAllHotkeys();
        UpdateUIState();
        UpdateAllHotkeyText();
        SetWindowSize(360, 120); // Set initial size for the main panel

        m_isAppInitialized = true;
        winvert4::Log("MainWindow: ctor end");
    }

    MainWindow::~MainWindow()
    {
        // TODO: Save settings
        RemoveWindowSubclass(m_mainHwnd, &MainWindow::WindowSubclassProc, 1);
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
    }

    int32_t MainWindow::MyProperty() { return 0; }
    void MainWindow::MyProperty(int32_t) {}

    void MainWindow::ToggleSnipping()
    {
        winvert4::Log("MainWindow: ToggleSnipping");
        StartScreenSelection();
    }

    // --- XAML Event Handlers ---
    void MainWindow::BrightnessProtection_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        int idx = SelectedTabIndex();
        if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;
        m_windowSettings[idx].isBrightnessProtectionEnabled = !m_windowSettings[idx].isBrightnessProtectionEnabled;
        UpdateUIState();
        //SendSelectedWindowSettings();
    }

    void MainWindow::InvertEffect_Click(IInspectable const&, RoutedEventArgs const&)
    {
        int idx = SelectedTabIndex();
        if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return; // NOLINT(bugprone-branch-clone)
        auto& s = m_windowSettings[idx];
        s.isInvertEffectEnabled = !s.isInvertEffectEnabled;
        UpdateUIState();
        if (idx < static_cast<int>(m_effectWindows.size()))
        {
            if (auto& wnd = m_effectWindows[idx])
                wnd->UpdateSettings(s);
        }
    }

    void MainWindow::GrayscaleEffect_Click(IInspectable const&, RoutedEventArgs const&)
    {
        int idx = SelectedTabIndex();
        if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return; // NOLINT(bugprone-branch-clone)
        auto& s = m_windowSettings[idx];
        s.isGrayscaleEffectEnabled = !s.isGrayscaleEffectEnabled;
        UpdateUIState();
        if (idx < static_cast<int>(m_effectWindows.size()))
        {
            if (auto& wnd = m_effectWindows[idx])
                wnd->UpdateSettings(s);
        }
    }

    void MainWindow::AddWindow_Click(IInspectable const&, RoutedEventArgs const&)
    {
        StartScreenSelection();
    }

    void MainWindow::RemoveWindow_Click(IInspectable const&, RoutedEventArgs const&)
    {
        HWND hwnd = SelectedWindowHwnd();
        // TODO: RequestRemoveWindowByHwnd
    }

    void MainWindow::HideAllWindows_Click(IInspectable const&, RoutedEventArgs const&)
    {
        int idx = SelectedTabIndex();
        if (idx < 0) return;
        if (idx >= static_cast<int>(m_windowSettings.size())) return;
        if (idx >= static_cast<int>(m_windowHidden.size())) m_windowHidden.resize(m_windowSettings.size(), false);
        m_windowHidden[idx] = !m_windowHidden[idx];
        HWND hwnd = SelectedWindowHwnd();
        // TODO: RequestSetWindowHiddenByHwnd
        UpdateUIState();
    }

    void MainWindow::SettingsButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_wereWindowsHiddenForSettings = m_areWindowsHidden;
        if (!m_areWindowsHidden)
        {
            m_areWindowsHidden = true;
            // TODO: RequestHideAllWindows
            UpdateUIState();
        }

        RootPanel().Visibility(Visibility::Collapsed);
        SettingsPanel().Visibility(Visibility::Visible);

        if (auto panel = FilterEditorPanel()) panel.Visibility(Visibility::Collapsed);

        auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(m_mainHwnd);
        auto appWindow = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
        if (appWindow)
        {
            if (auto presenter = appWindow.Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
            {
                presenter.Maximize();
            }
        }
        // Adjust About panel visibility/width based on maximize state
        UpdateSettingsColumnsForWindowState();

        // Default all settings sections to collapsed on entry
        CollapseAllSettingsExpanders();
    }

    void MainWindow::BackButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_rebindingState != RebindingState::None) {
            m_rebindingState = RebindingState::None;
            RebindStatusText().Text(L"");
            RebindInvertHotkeyButton().IsEnabled(true);
            RebindGrayscaleHotkeyButton().IsEnabled(true);
            RebindRemoveHotkeyButton().IsEnabled(true);
        }

        auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(m_mainHwnd);
        auto appWindow = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
        if (appWindow)
        {
            if (auto presenter = appWindow.Presenter().try_as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>())
            {
                presenter.Restore();
            }
        }

        if (!m_wereWindowsHiddenForSettings)
        {
            m_areWindowsHidden = false;
            // TODO: RequestHideAllWindows
        }

        SettingsPanel().Visibility(Visibility::Collapsed);
        RootPanel().Visibility(Visibility::Visible);
        // Fixed control panel size based on whether any windows exist
        {
            bool hasWindows = RegionsTabView().TabItems().Size() > 0;
            SetWindowSize(360, hasWindows ? 120 : 240);
        }

        // Reapply composite filters for all tabs (in case matrices were edited)
        //auto items = RegionsTabView().TabItems();
        //for (uint32_t i = 0; i < items.Size(); ++i)
        //{
        //    ApplyCompositeCustomFiltersForTab(static_cast<int>(i));
        //}
    }

    void MainWindow::SettingsPanel_SizeChanged(IInspectable const&, winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const&)
    {
        UpdateSettingsColumnsForWindowState();
    }

    void MainWindow::UpdateSettingsColumnsForWindowState()
    {
        bool isMax = ::IsZoomed(m_mainHwnd) != 0;
        using winrt::Microsoft::UI::Xaml::GridLengthHelper;
        AboutColumn().Width(isMax ? GridLengthHelper::FromPixels(360.0) : GridLengthHelper::FromPixels(0.0));
        AboutScroll().Visibility(isMax ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::CollapseAllSettingsExpanders()
    {
        // Safely collapse any expander if it exists in the tree
        if (auto expander = BrightnessExpander()) expander.IsExpanded(false);
        if (SelectionColorExpander()) SelectionColorExpander().IsExpanded(false);
        if (UpdateRateExpander()) UpdateRateExpander().IsExpanded(false);
        if (HotkeysExpander()) HotkeysExpander().IsExpanded(false);
        if (CustomFiltersExpander()) CustomFiltersExpander().IsExpanded(false);

        // Hide editor panel by default
        if (FilterEditorPanel()) FilterEditorPanel().Visibility(Visibility::Collapsed);
    }

    void MainWindow::CustomFiltersExpander_Collapsed(IInspectable const&, Microsoft::UI::Xaml::Controls::ExpanderCollapsedEventArgs const&)
    {
        // Hide the editor when the Custom Filters expander collapses
        if (auto panel = FilterEditorPanel()) panel.Visibility(Visibility::Collapsed);
    }

    // --- Settings Handlers ---
    void MainWindow::BrightnessThresholdNumberBox_ValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const& args)
    {
        m_brightnessThreshold = static_cast<int>(args.NewValue());
        //SendSettingsToWorker();
    }

    void MainWindow::BrightnessDelayNumberBox_ValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const& args)
    {
        m_brightnessProtectionDelay = static_cast<int>(args.NewValue());
        //SendSettingsToWorker();
    }


    void MainWindow::DownsampleTargetPixelsNumberBox_ValueChanged(NumberBox const&, NumberBoxValueChangedEventArgs const& args)
    {
        m_downsampleTargetPixels = static_cast<int>(args.NewValue());
        if (m_downsampleTargetPixels < 16) m_downsampleTargetPixels = 16;
        //SendSettingsToWorker();
    }

    void MainWindow::FpsComboBox_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        m_fpsSetting = FpsComboBox().SelectedIndex();
        if (!m_isAppInitialized)
        {
            return;
        }
        //SendSettingsToWorker();
    }

    void MainWindow::ShowFpsToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        // Sync setting and push to worker
        m_showFpsOverlay = ShowFpsToggle().IsOn();
        if (!m_isAppInitialized)
        {
            return;
        }
        //SendSettingsToWorker();
    }

    void MainWindow::RebindInvertHotkeyButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rebindingState = RebindingState::Invert;
        RebindStatusText().Text(L"Press key combo for Invert/Add. ESC to cancel.");
        RebindInvertHotkeyButton().IsEnabled(false);
        RebindGrayscaleHotkeyButton().IsEnabled(true);
        RebindRemoveHotkeyButton().IsEnabled(true);
        SetFocus(m_mainHwnd);
    }

    void MainWindow::RebindGrayscaleHotkeyButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rebindingState = RebindingState::Grayscale;
        RebindStatusText().Text(L"Press key combo for Grayscale/Add. ESC to cancel.");
        RebindInvertHotkeyButton().IsEnabled(true);
        RebindGrayscaleHotkeyButton().IsEnabled(false);
        RebindRemoveHotkeyButton().IsEnabled(true);
        SetFocus(m_mainHwnd);
    }

    void MainWindow::RebindRemoveHotkeyButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rebindingState = RebindingState::Remove;
        RebindStatusText().Text(L"Press key combo for Remove Last. ESC to cancel.");
        RebindInvertHotkeyButton().IsEnabled(true);
        RebindGrayscaleHotkeyButton().IsEnabled(true);
        RebindRemoveHotkeyButton().IsEnabled(false);
        SetFocus(m_mainHwnd);
    }

    void MainWindow::SelectionColorPicker_ColorChanged(ColorPicker const&, ColorChangedEventArgs const& args)
    {
        auto newColor = args.NewColor();
        m_selectionColor = RGB(newColor.R, newColor.G, newColor.B);
    }

    // --- Core Logic ---
    void MainWindow::RegisterAllHotkeys()
    {
        UnregisterHotKey(m_mainHwnd, HOTKEY_INVERT_ID);
        UnregisterHotKey(m_mainHwnd, HOTKEY_GRAYSCALE_ID);
        UnregisterHotKey(m_mainHwnd, HOTKEY_REMOVE_ID);

        RegisterHotKey(m_mainHwnd, HOTKEY_INVERT_ID, m_hotkeyInvertMod, m_hotkeyInvertVk);
        RegisterHotKey(m_mainHwnd, HOTKEY_GRAYSCALE_ID, m_hotkeyGrayscaleMod, m_hotkeyGrayscaleVk);
        RegisterHotKey(m_mainHwnd, HOTKEY_REMOVE_ID, m_hotkeyRemoveMod, m_hotkeyRemoveVk);
        UpdateAllHotkeyText();
    }

    void MainWindow::OnInvertHotkeyPressed()
    {
        m_pendingEffect = PendingEffect::Invert;
        StartScreenSelection();
    }

    void MainWindow::OnGrayscaleHotkeyPressed()
    {
        m_pendingEffect = PendingEffect::Grayscale;
        StartScreenSelection();
    }

    void MainWindow::OnRemoveHotkeyPressed()
    {
        // Mark hotkey-triggered removal so we can close app if it was the last window
        m_lastRemovalInitiatedByHotkey = true;
        m_lastRemovalViaUI = false;
        // TODO: RequestRemoveLastEffectWindow
    }

    void MainWindow::StartScreenSelection()
    {
        if (m_isSelecting) return;
        m_isSelecting = true;
        winvert4::Log("MainWindow: StartScreenSelection");

        WNDCLASSEXW wcex{ sizeof(WNDCLASSEXW) };
        wcex.style         = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc   = &MainWindow::SelectionWndProc;
        wcex.hInstance     = GetModuleHandleW(nullptr);
        wcex.hCursor       = LoadCursorW(nullptr, IDC_CROSS);
        wcex.lpszClassName = kSelectionWndClass;
        wcex.hbrBackground = nullptr;

        static ATOM s_atom = 0;
        if (!s_atom) s_atom = RegisterClassExW(&wcex);
        winvert4::Logf("MainWindow: selection wnd class atom=%u", (unsigned)s_atom);

        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        // Note: Do not use WS_EX_LAYERED here because we draw the dimming/freeze
        // effect directly into the window via GDI+. Layered windows typically
        // require UpdateLayeredWindow/SetLayeredWindowAttributes to present
        // content; removing it ensures our WM_PAINT drawing is visible.
        m_selectionHwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kSelectionWndClass,
            L"Winvert Selection",
            WS_POPUP,
            vx, vy, vw, vh,
            nullptr, nullptr, GetModuleHandleW(nullptr),
            this);

        if (m_selectionHwnd)
        {
            winvert4::Logf("MainWindow: selection HWND=%p rect=(%d,%d %dx%d)", (void*)m_selectionHwnd, vx, vy, vw, vh);
            ShowWindow(m_selectionHwnd, SW_SHOW);
            SetForegroundWindow(m_selectionHwnd);
            // Capture the screen AFTER the window is visible.
            CaptureScreenBitmap();
            InvalidateRect(m_selectionHwnd, nullptr, FALSE); // Trigger a repaint with the new bitmap.
        }
        else
        {
            winvert4::Log("MainWindow: selection CreateWindowExW failed");
            m_isSelecting = false;
        }
    }

    void MainWindow::CaptureScreenBitmap()
    {
        ReleaseScreenBitmap();

        m_virtualOrigin.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        m_virtualOrigin.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        m_screenSize.cx   = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        m_screenSize.cy   = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        winvert4::Logf("MainWindow: virtual origin=(%ld,%ld) size=(%ldx%ld)", m_virtualOrigin.x, m_virtualOrigin.y, m_screenSize.cx, m_screenSize.cy);

        HDC screenDC = GetDC(nullptr);
        m_screenMemDC = CreateCompatibleDC(screenDC);
        m_screenBmp   = CreateCompatibleBitmap(screenDC, m_screenSize.cx, m_screenSize.cy);
        HGDIOBJ old   = SelectObject(m_screenMemDC, m_screenBmp);

        BitBlt(m_screenMemDC, 0, 0, m_screenSize.cx, m_screenSize.cy,
               screenDC, m_virtualOrigin.x, m_virtualOrigin.y, SRCCOPY);

        SelectObject(m_screenMemDC, old);
        ReleaseDC(nullptr, screenDC);
        winvert4::Log("MainWindow: captured screen bitmap");
    }

    void MainWindow::EnumerateMonitors()
    {
        s_monitorRects.clear();
        EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
    }

    BOOL CALLBACK MainWindow::MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
    {
        s_monitorRects.push_back(*lprcMonitor);
        return TRUE;
    }

    void MainWindow::ReleaseScreenBitmap()
    {
        if (m_screenMemDC)
        {
            DeleteDC(m_screenMemDC);
            m_screenMemDC = nullptr;
        }
        if (m_screenBmp)
        {
            DeleteObject(m_screenBmp);
            m_screenBmp = nullptr;
        }
        m_screenSize = { 0,0 };
        m_virtualOrigin = { 0,0 };
        winvert4::Log("MainWindow: released screen bitmap");
    }

    RECT MainWindow::MakeRectFromPoints(POINT a, POINT b) const
    {
        RECT r{};
        r.left   = (std::min)(a.x, b.x);
        r.top    = (std::min)(a.y, b.y);
        r.right  = (std::max)(a.x, b.x);
        r.bottom = (std::max)(a.y, b.y);
        return r;
    }

    void MainWindow::OnSelectionCompleted(RECT sel)
    {
        // Convert selection into virtual-screen coordinates already done by caller

        // 1) Tear down selection overlay BEFORE starting duplication/effect window
        if (m_selectionHwnd) {
            ::DestroyWindow(m_selectionHwnd);
            m_selectionHwnd = nullptr;
            winvert4::Log("MainWindow: selection overlay destroyed (pre-dup)");
        }

        // 2) Give DWM one present interval to composite without the overlay
        ::Sleep(16);

        winvert4::Logf("MainWindow: selection completed rect=(%ld,%ld,%ld,%ld)",
                       sel.left, sel.top, sel.right, sel.bottom);

        // 3) Create a settings object for the new window
        EffectSettings settings{};
        if (m_pendingEffect == PendingEffect::Invert)
        {
            settings.isInvertEffectEnabled = true;
        }
        else if (m_pendingEffect == PendingEffect::Grayscale)
        {
            settings.isGrayscaleEffectEnabled = true;
        }
        m_windowSettings.push_back(settings);
        m_pendingEffect = PendingEffect::None; // Reset for next time

        // 3) Create and show the effect window AFTER overlay is gone
        auto newWindow = std::make_unique<EffectWindow>(sel, m_outputManager.get());
        newWindow->UpdateSettings(settings);
        newWindow->Show();

        // 4) Add a tab for the new window
        auto newTab = TabViewItem();
        newTab.Header(winrt::box_value(L"Region " + std::to_wstring(m_effectWindows.size() + 1)));
        RegionsTabView().TabItems().Append(newTab);
        RegionsTabView().SelectedItem(newTab);

        m_effectWindows.push_back(std::move(newWindow));
    }


    LRESULT CALLBACK MainWindow::SelectionWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_ERASEBKGND:
            // Prevent background erase to avoid flicker; we fully paint in WM_PAINT
            return 1;
        case WM_NCCREATE:
        {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<MainWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }

        case WM_SETCURSOR:
        {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        }

        case WM_LBUTTONDOWN:
        {
            if (!self) break;
            self->m_isDragging = true;

            POINTS pts = MAKEPOINTS(lParam);
            // Use client coordinates, which are relative to the virtual screen origin
            self->m_ptStart = { pts.x, pts.y }; // These are client coords
            self->m_ptEnd = self->m_ptStart;

            SetCapture(hwnd);
            // Use FALSE to avoid erasing the background, which we handle ourselves
            InvalidateRect(hwnd, nullptr, FALSE);
            winvert4::Log("MainWindow: selection WM_LBUTTONDOWN");
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            if (!self || !self->m_isDragging) break;
            POINTS pts = MAKEPOINTS(lParam);
            self->m_ptEnd = { pts.x, pts.y }; // Client coords
            InvalidateRect(hwnd, nullptr, FALSE); // Use FALSE to avoid flicker
            return 0;
        }

        case WM_LBUTTONUP:
        {
            if (!self) break;
            ReleaseCapture();
            self->m_isDragging = false;

            POINTS pts = MAKEPOINTS(lParam);
            self->m_ptEnd = { pts.x, pts.y }; // Client coords

            RECT sel = self->MakeRectFromPoints(self->m_ptStart, self->m_ptEnd);

            // Adjust from overlay-window coordinates to virtual desktop coords
            sel.left   += self->m_virtualOrigin.x;
            sel.right  += self->m_virtualOrigin.x;
            sel.top    += self->m_virtualOrigin.y;
            sel.bottom += self->m_virtualOrigin.y;

            self->OnSelectionCompleted(sel);

            DestroyWindow(hwnd);
            winvert4::Log("MainWindow: selection WM_LBUTTONUP -> DestroyWindow");
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                if (self) {
                    self->m_isDragging = false;
                    self->m_isSelecting = false;
                    self->m_pendingEffect = PendingEffect::None;
                }
                DestroyWindow(hwnd);
                winvert4::Log("MainWindow: selection cancelled via ESC");
                return 0;
            }
            else if (wParam >= '1' && wParam <= '9')
            {
                if (!self) break;
                int monitorIndex = static_cast<int>(wParam - '1');
                if (monitorIndex >= 0 && monitorIndex < static_cast<int>(s_monitorRects.size()))
                {
                    self->m_isDragging = false;
                    self->m_isSelecting = false;
                    ReleaseCapture();

                    // The monitor rect is already in virtual screen coordinates
                    self->OnSelectionCompleted(s_monitorRects[monitorIndex]);

                    DestroyWindow(hwnd);
                    winvert4::Logf("MainWindow: selection via monitor #%d", monitorIndex + 1);
                    return 0;
                }
            }
            break;


        case WM_PAINT:
        {
            if (!self) break;

            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Double-buffered painting to eliminate flicker
            const int bufW = self->m_screenSize.cx;
            const int bufH = self->m_screenSize.cy;
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, bufW, bufH);
            HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

            Gdiplus::Graphics graphics(memDC);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            // 1. Draw the captured screen bitmap as the base layer.
            if (self->m_screenBmp)
            {
                Gdiplus::Bitmap bitmap(self->m_screenBmp, NULL);
                graphics.DrawImage(&bitmap, 0, 0);
            }

            // 2. Draw a semi-transparent overlay on top to dim the screen.
            Gdiplus::SolidBrush overlayBrush(Gdiplus::Color(128, 0, 0, 0));
            graphics.FillRectangle(&overlayBrush, 0, 0, bufW, bufH);

            // 3. Draw monitor numbers and instructions.
            Gdiplus::Font numFont(L"Arial", 72, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font instructionFont(L"Segoe UI", 24, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 255, 255, 255));
            Gdiplus::StringFormat stringFormat;
            stringFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
            stringFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);

            if (self->m_showSelectionInstructions && !s_monitorRects.empty())
            {
                RECT instructionRect = s_monitorRects[0];
                OffsetRect(&instructionRect, -self->m_virtualOrigin.x, -self->m_virtualOrigin.y);
                Gdiplus::RectF instructionRectF(
                    (Gdiplus::REAL)instructionRect.left, (Gdiplus::REAL)instructionRect.top,
                    (Gdiplus::REAL)(instructionRect.right - instructionRect.left), (Gdiplus::REAL)100);
                graphics.DrawString(L"Click and drag to select a region, or press a number to select a display.", -1, &instructionFont, instructionRectF, &stringFormat, &textBrush);
            }

            for (size_t i = 0; i < s_monitorRects.size(); ++i)
            {
                RECT textRect = s_monitorRects[i];
                OffsetRect(&textRect, -self->m_virtualOrigin.x, -self->m_virtualOrigin.y);
                Gdiplus::RectF textRectF((Gdiplus::REAL)textRect.left, (Gdiplus::REAL)textRect.top, (Gdiplus::REAL)(textRect.right - textRect.left), (Gdiplus::REAL)(textRect.bottom - textRect.top));
                std::wstring monitorText = std::to_wstring(i + 1);
                graphics.DrawString(monitorText.c_str(), -1, &numFont, textRectF, &stringFormat, &textBrush);
            }

            // 4. If dragging, draw the selection rectangle on top.
            if (self->m_isDragging)
            {
                RECT r = self->MakeRectFromPoints(self->m_ptStart, self->m_ptEnd);
                Gdiplus::Color penColor(255, GetRValue(self->m_selectionColor), GetGValue(self->m_selectionColor), GetBValue(self->m_selectionColor));
                Gdiplus::Pen selectionPen(penColor, 2.0f);
                graphics.DrawRectangle(&selectionPen,
                    static_cast<INT>(r.left),
                    static_cast<INT>(r.top),
                    static_cast<INT>(r.right - r.left),
                    static_cast<INT>(r.bottom - r.top));
            }

            // Present the back buffer
            BitBlt(hdc, 0, 0, bufW, bufH, memDC, 0, 0, SRCCOPY);

            // Cleanup
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            if (self)
            {
                self->m_isSelecting = false;
                self->ReleaseScreenBitmap();
                self->m_selectionHwnd = nullptr;
                winvert4::Log("MainWindow: selection overlay destroyed");
            }
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // --- UI Helpers from MainWindowAlt ---

    void MainWindow::UpdateUIState()
    {
        bool hasWindows = RegionsTabView().TabItems().Size() > 0;
        int idx = SelectedTabIndex();
        EffectSettings current{};
        if (idx >= 0 && idx < static_cast<int>(m_windowSettings.size()))
        {
            current = m_windowSettings[idx];
        }
        if (auto img = BrightnessProtectionImage()) img.Source(current.isBrightnessProtectionEnabled ? m_brightnessOnIconSource : m_brightnessOffIconSource);
        if (auto img = InvertEffectImage()) img.Source(current.isInvertEffectEnabled ? m_invertOnIconSource : m_invertOffIconSource);
        if (auto img = GrayscaleEffectImage()) img.Source(current.isGrayscaleEffectEnabled ? m_grayscaleOnIconSource : m_grayscaleOffIconSource);
        bool curHidden = (idx >= 0 && idx < static_cast<int>(m_windowHidden.size())) ? m_windowHidden[idx] : false;
        if (auto img = HideAllWindowsImage()) img.Source(curHidden ? m_showIconSource : m_hideIconSource);

        if (auto btn = HideAllWindowsButton()) btn.IsEnabled(hasWindows);
        if (auto btn = BrightnessProtectionButton()) btn.IsEnabled(hasWindows);
        if (auto btn = InvertEffectButton()) btn.IsEnabled(hasWindows);
        if (auto btn = GrayscaleEffectButton()) btn.IsEnabled(hasWindows);
        if (auto btn = FiltersDropDownButton()) btn.IsEnabled(hasWindows);

        bool showEmptyPrompt = (!hasWindows) && m_hasEverHadWindows && m_lastRemovalViaUI;
        if (auto bar = InfoBar()) bar.IsOpen(showEmptyPrompt);

        if (SettingsPanel().Visibility() != Visibility::Visible)
        {
            SetWindowSize(360, showEmptyPrompt ? 240 : 120);
        }
    }

    void MainWindow::SetWindowSize(int width, int height)
    {
        auto windowId = winrt::Microsoft::UI::GetWindowIdFromWindow(m_mainHwnd);
        auto appWindow = winrt::Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);
        if (appWindow)
        {
            auto dpi = GetDpiForWindow(m_mainHwnd);
            float scale = static_cast<float>(dpi) / 96.0f;
            appWindow.ResizeClient({ static_cast<int>(width * scale), static_cast<int>(height * scale) });
        }
    }

    void MainWindow::UpdateAllHotkeyText()
    {
        if (auto tb = InvertHotkeyTextBox()) UpdateHotkeyText(tb, m_hotkeyInvertMod, m_hotkeyInvertVk);
        if (auto tb = GrayscaleHotkeyTextBox()) UpdateHotkeyText(tb, m_hotkeyGrayscaleMod, m_hotkeyGrayscaleVk);
        if (auto tb = RemoveHotkeyTextBox()) UpdateHotkeyText(tb, m_hotkeyRemoveMod, m_hotkeyRemoveVk);
    }

    void MainWindow::UpdateHotkeyText(TextBox const& textBox, UINT mod, UINT vk)
    {
        std::wstring hotkeyString;
        if (mod & MOD_WIN) hotkeyString += L"Win + ";
        if (mod & MOD_CONTROL) hotkeyString += L"Ctrl + ";
        if (mod & MOD_ALT) hotkeyString += L"Alt + ";
        if (mod & MOD_SHIFT) hotkeyString += L"Shift + ";
        wchar_t keyName[50];
        UINT scanCode = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
        if (GetKeyNameTextW(scanCode << 16, keyName, sizeof(keyName) / sizeof(wchar_t))) {
            hotkeyString += keyName;
        }
        else {
            hotkeyString += L"Unknown";
        }
        textBox.Text(hotkeyString);
    }

    int MainWindow::SelectedTabIndex()
    {
        return static_cast<int>(RegionsTabView().SelectedIndex());
    }

    HWND MainWindow::SelectedWindowHwnd()
    {
        int idx = SelectedTabIndex();
        if (idx < 0) return nullptr;
        auto items = RegionsTabView().TabItems();
        if (idx >= static_cast<int>(items.Size())) return nullptr;
        auto tab = items.GetAt(idx).as<Microsoft::UI::Xaml::Controls::TabViewItem>();
        if (!tab) return nullptr;
        uint64_t tag = 0;
        if (auto boxed = tab.Tag()) { tag = unbox_value<uint64_t>(boxed); }
        return reinterpret_cast<HWND>(static_cast<uintptr_t>(tag));
    }

    void MainWindow::FiltersMenuFlyout_Closing(IInspectable const&, Controls::Primitives::FlyoutBaseClosingEventArgs const& e)
    {
        if (m_keepFiltersFlyoutOpenNext)
        {
            e.Cancel(true);
            m_keepFiltersFlyoutOpenNext = false;
        }
    }

    void MainWindow::InfoBar_Closed(IInspectable const&, Microsoft::UI::Xaml::Controls::InfoBarClosedEventArgs const&)
    {
        if (SettingsPanel().Visibility() != Visibility::Visible)
        {
            SetWindowSize(360, 120);
        }
    }

    void MainWindow::RegionsTabView_AddTabButtonClick(IInspectable const&, IInspectable const&)
    {
        StartScreenSelection();
    }

    void MainWindow::RegionsTabView_TabCloseRequested(TabView const& sender, TabViewTabCloseRequestedEventArgs const& args)
    {
        m_lastRemovalViaUI = true;
        m_lastRemovalInitiatedByHotkey = false;
        auto items = sender.TabItems();
        uint32_t index;
        if (items.IndexOf(args.Item(), index))
        {
            auto tab = args.Item().as<TabViewItem>();
            uint64_t tag = 0;
            if (auto boxed = tab.Tag()) { tag = unbox_value<uint64_t>(boxed); }
            HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(tag));
            // TODO: RequestRemoveWindowByHwnd(hwnd)
            items.RemoveAt(index);
            UpdateUIState();
        }
    }

    void MainWindow::RegionsTabView_SelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        UpdateUIState();
    }

    LRESULT CALLBACK MainWindow::WindowSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
    {
        MainWindow* pThis = reinterpret_cast<MainWindow*>(dwRefData);
        if (!pThis)
        {
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        if (pThis->m_rebindingState != RebindingState::None) {
            if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
                if (wParam == VK_ESCAPE) {
                    pThis->m_rebindingState = RebindingState::None;
                    pThis->RebindStatusText().Text(L"Rebind cancelled.");
                    pThis->RebindInvertHotkeyButton().IsEnabled(true);
                    pThis->RebindGrayscaleHotkeyButton().IsEnabled(true);
                    pThis->RebindRemoveHotkeyButton().IsEnabled(true);
                    return 0;
                }

                UINT newMod = 0;
                if (GetKeyState(VK_SHIFT) & 0x8000) newMod |= MOD_SHIFT;
                if (GetKeyState(VK_CONTROL) & 0x8000) newMod |= MOD_CONTROL;
                if (GetKeyState(VK_MENU) & 0x8000) newMod |= MOD_ALT;
                if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) newMod |= MOD_WIN;

                if (newMod == 0) {
                    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
                }

                if (wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_MENU && wParam != VK_LWIN && wParam != VK_RWIN) {
                    switch (pThis->m_rebindingState) {
                    case RebindingState::Invert:    pThis->m_hotkeyInvertMod = newMod; pThis->m_hotkeyInvertVk = static_cast<UINT>(wParam); break;
                    case RebindingState::Grayscale: pThis->m_hotkeyGrayscaleMod = newMod; pThis->m_hotkeyGrayscaleVk = static_cast<UINT>(wParam); break;
                    case RebindingState::Remove:    pThis->m_hotkeyRemoveMod = newMod; pThis->m_hotkeyRemoveVk = static_cast<UINT>(wParam); break;
                    }
                    pThis->m_rebindingState = RebindingState::None;
                    pThis->RegisterAllHotkeys();
                    pThis->RebindStatusText().Text(L"Hotkey updated!");
                    pThis->RebindInvertHotkeyButton().IsEnabled(true);
                    pThis->RebindGrayscaleHotkeyButton().IsEnabled(true);
                    pThis->RebindRemoveHotkeyButton().IsEnabled(true);
                    return 0;
                }
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);
            }
        }

        switch (uMsg) {
        case WM_HOTKEY:
        {
            if (wParam == HOTKEY_INVERT_ID) {
                pThis->OnInvertHotkeyPressed();
            }
            else if (wParam == HOTKEY_GRAYSCALE_ID) {
                pThis->OnGrayscaleHotkeyPressed();
            }
            else if (wParam == HOTKEY_REMOVE_ID) {
                pThis->OnRemoveHotkeyPressed();
            }
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

void winrt::Winvert4::implementation::MainWindow::AddNewFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    // TODO: Implement filter logic
}

void winrt::Winvert4::implementation::MainWindow::SaveFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    // TODO: Implement filter logic
}

void winrt::Winvert4::implementation::MainWindow::DeleteFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    // TODO: Implement filter logic
}

void winrt::Winvert4::implementation::MainWindow::ClearFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    // TODO: Implement filter logic
}

void winrt::Winvert4::implementation::MainWindow::SavedFiltersComboBox_SelectionChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    // TODO: Implement filter logic
}

void winrt::Winvert4::implementation::MainWindow::FilterMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    auto menuItem = sender.as<MenuFlyoutItem>();
    int tagIndex = -2;
    if (auto tag = menuItem.Tag())
    {
        tagIndex = unbox_value<int>(tag);
    }

    // "None" clears all selections and disables custom effects
    if (tagIndex == -1)
    {
        int idx = SelectedTabIndex();
        if (idx >= 0 && idx < static_cast<int>(m_windowSettings.size()))
        {
            //if (static_cast<int>(m_tabFilterSelections.size()) <= idx)
            //{
            //    m_tabFilterSelections.resize(idx + 1);
            //}
            //m_tabFilterSelections[idx].assign(m_savedFilters.size(), false);
            m_windowSettings[idx].isCustomEffectActive = false;
            //ApplyCompositeCustomFiltersForTab(idx);
        }
        m_keepFiltersFlyoutOpenNext = false; // allow close
        //UpdateFilterDropdown();
        return;
    }

    // Toggle item by index and apply composite; keep flyout open
    //if (tagIndex >= 0 && tagIndex < static_cast<int>(m_savedFilters.size()))
    //{
    //    int idx = SelectedTabIndex();
    //    if (idx >= 0)
    //    {
    //        if (static_cast<int>(m_tabFilterSelections.size()) <= idx)
    //        {
    //            m_tabFilterSelections.resize(idx + 1);
    //        }
    //        if (static_cast<int>(m_tabFilterSelections[idx].size()) != static_cast<int>(m_savedFilters.size()))
    //        {
    //            m_tabFilterSelections[idx].assign(m_savedFilters.size(), false);
    //        }
    //        if (auto toggleItem = menuItem.try_as<Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem>())
    //        {
    //            m_tabFilterSelections[idx][tagIndex] = toggleItem.IsChecked();
    //        }
    //        ApplyCompositeCustomFiltersForTab(idx);
    //    }
    //    m_keepFiltersFlyoutOpenNext = true;
    //    UpdateFilterDropdown();
    //}
}
