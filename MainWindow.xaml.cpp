
#include "pch.h"
#include "MainWindow.xaml.h"
#include "Log.h"
#include "OutputManager.h"
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.ApplicationModel.h>
#include <gdiplus.h>
#include <microsoft.ui.xaml.window.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <locale>
#include <iomanip>

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
    constexpr int HOTKEY_FILTER_ID = 2;
    constexpr int HOTKEY_REMOVE_ID = 3;

    // Default luminance weights (BT.709 for sRGB)
    constexpr float kDefaultLumaWeights[3] = { 0.2126f, 0.7152f, 0.0722f };
}

std::vector<RECT> winrt::Winvert4::implementation::MainWindow::s_monitorRects;

HHOOK winrt::Winvert4::implementation::MainWindow::s_mouseHook = nullptr;
HHOOK winrt::Winvert4::implementation::MainWindow::s_keyboardHook = nullptr;
winrt::Winvert4::implementation::MainWindow* winrt::Winvert4::implementation::MainWindow::s_samplingInstance = nullptr;

LRESULT CALLBACK winrt::Winvert4::implementation::MainWindow::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP))
    {
        auto p = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        auto self = MainWindow::s_samplingInstance;
        if (self)
        {
            // Dispatch to UI thread to update XAML safely
            auto dq = self->DispatcherQueue();
            POINT pt = p->pt;
            dq.TryEnqueue([self, pt]() {
                self->OnColorSampled(pt);
                if (auto btn = self->ColorMapSampleButton()) { btn.Content(box_value(L"Sample")); btn.IsEnabled(true); }
                self->HideSampleOverlay();
            });
        }
        if (MainWindow::s_mouseHook)
        {
            UnhookWindowsHookEx(MainWindow::s_mouseHook);
            MainWindow::s_mouseHook = nullptr;
            MainWindow::s_samplingInstance = nullptr;
        }
        // Swallow this click so desktop does not receive it
        return 1;
    }
    else if (nCode >= 0 && wParam == WM_MOUSEMOVE)
    {
        auto p = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        auto self = MainWindow::s_samplingInstance;
        if (self && self->m_isSamplingColor)
        {
            POINT pt = p->pt;
            // Force default cursor while sampling
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            auto dq = self->DispatcherQueue();
            dq.TryEnqueue([self, pt]() { self->MoveSampleOverlay(pt); });
        }
    }
    return CallNextHookEx(MainWindow::s_mouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK winrt::Winvert4::implementation::MainWindow::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN))
    {
        auto p = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (p->vkCode == VK_ESCAPE)
        {
            auto self = MainWindow::s_samplingInstance;
            if (self && self->m_isSamplingColor)
            {
                auto dq = self->DispatcherQueue();
                dq.TryEnqueue([self]() {
                    self->CancelColorSample();
                });
            }
            if (MainWindow::s_keyboardHook)
            {
                UnhookWindowsHookEx(MainWindow::s_keyboardHook);
                MainWindow::s_keyboardHook = nullptr;
            }
            return 1; // swallow ESC
        }
    }
    return CallNextHookEx(MainWindow::s_keyboardHook, nCode, wParam, lParam);
}

namespace winrt::Winvert4::implementation
{
    winrt::Winvert4::implementation::MainWindow::MainWindow()
    {
        winvert4::Log("MainWindow: ctor start");

        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, NULL);

        InitializeComponent();
        this->Title(L"Winvert Control Panel");

        // Populate About panel with app version: "Winvert <major.minor.build[.rev]>"
        try
        {
            using namespace winrt::Windows::ApplicationModel;
            auto ver = Package::Current().Id().Version();
            std::wstringstream ss;
            ss << L"Winvert " << ver.Major << L"." << ver.Minor << L"." << ver.Build;
            if (ver.Revision != 0) ss << L"." << ver.Revision;
            if (auto t = AboutVersionText()) t.Text(hstring{ ss.str() });
        }
        catch (...)
        {
            if (auto t = AboutVersionText()) t.Text(L"Winvert");
        }

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
        m_colorMappingOnIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/mapping_on.svg"));
        m_colorMappingOffIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/mapping_off.svg"));
        m_invertOnIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/invert_on.svg"));
        m_invertOffIconSource = SvgImageSource(Uri(L"ms-appx:///Assets/invert_off.svg"));

        // Keep the filters flyout open while toggling items
        if (auto flyout = FiltersMenuFlyout())
        {
            flyout.Closing({ this, &MainWindow::FiltersMenuFlyout_Closing });
        }

        SetWindowSubclass(m_mainHwnd, &MainWindow::WindowSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

        RegisterAllHotkeys();

        // Populate default filter presets if none exist yet
        if (m_savedFilters.empty())
        {
            auto add = [&](const wchar_t* name, const float m[16], const float off[4])
            {
                SavedFilter sf{}; sf.name = name; sf.isBuiltin = true;
                memcpy(sf.mat, m, sizeof(sf.mat));
                memcpy(sf.offset, off, sizeof(sf.offset));
                m_savedFilters.push_back(sf);
            };

            const float OFF0[4] = {0,0,0,0};

            // Grayscale (luma)
            const float M_GRAY[16] = {
                // Each output channel = dot([wR,wG,wB], [R,G,B,1])
                kDefaultLumaWeights[0], kDefaultLumaWeights[1], kDefaultLumaWeights[2], 0,
                kDefaultLumaWeights[0], kDefaultLumaWeights[1], kDefaultLumaWeights[2], 0,
                kDefaultLumaWeights[0], kDefaultLumaWeights[1], kDefaultLumaWeights[2], 0,
                0,0,0,1
            };
            add(L"Grayscale", M_GRAY, OFF0);

            // Sepia
            const float M_SEPIA[16] = {
                0.393f,0.769f,0.189f,0,
                0.349f,0.686f,0.168f,0,
                0.272f,0.534f,0.131f,0,
                0,0,0,1
            };
            add(L"Sepia", M_SEPIA, OFF0);

            // Warm (mild)
            const float M_WARM[16] = {
                1.10f,0.00f,0.00f,0,
                0.00f,1.00f,0.00f,0,
                0.00f,0.00f,0.90f,0,
                0,0,0,1
            };
            add(L"Warm", M_WARM, OFF0);

            // Cool (mild)
            const float M_COOL[16] = {
                0.90f,0.00f,0.00f,0,
                0.00f,1.00f,0.00f,0,
                0.00f,0.00f,1.10f,0,
                0,0,0,1
            };
            add(L"Cool", M_COOL, OFF0);

            // Night Shift (reduce blue)
            const float M_NIGHT[16] = {
                1.00f,0.00f,0.00f,0,
                0.00f,0.92f,0.00f,0,
                0.00f,0.00f,0.70f,0,
                0,0,0,1
            };
            add(L"Night Shift", M_NIGHT, OFF0);

            // Hue Rotate (+30 degrees) based on CSS/SVG hue-rotate matrix
            // Uses Rec.709/sRGB luma constants (approx 0.213, 0.715, 0.072)
            {
                auto addHueRotate = [&](const wchar_t* name, float degrees)
                {
                    const float PI = 3.1415926535f;
                    float rad = degrees * PI / 180.0f;
                    float c = cosf(rad);
                    float s = sinf(rad);
                    const float a = 0.213f; // luma R
                    const float b = 0.715f; // luma G
                    const float d = 0.072f; // luma B
                    float M[16] = {
                        a + c * (1 - a) + s * (-a),   b + c * (-b) + s * (-b),   d + c * (-d) + s * (1 - d), 0,
                        a + c * (-a) + s * (0.143f),  b + c * (1 - b) + s * (0.140f), d + c * (-d) + s * (-0.283f), 0,
                        a + c * (-a) + s * (-0.787f), b + c * (-b) + s * (0.715f),   d + c * (1 - d) + s * (0.072f), 0,
                        0,0,0,1
                    };
                    add(name, M, OFF0);
                };
                addHueRotate(L"Hue +180", 180.0f);
            }

        // Load app state from blob file, then init saved filters UI
        LoadAppState();
        UpdateSavedFiltersCombo();
        UpdateFilterDropdown();
        }

        // Hide the control panel until the first selection creates a window.
        // We will show it in OnSelectionCompleted() once a region is added.
        ::ShowWindow(m_mainHwnd, SW_HIDE);

        UpdateUIState();

        // Initialize selection color toggle and picker enabled state from loaded settings
        if (auto t = SelectionColorEnableToggle()) t.IsOn(m_useCustomSelectionColor);
        if (auto cp = SelectionColorPicker()) cp.IsEnabled(m_useCustomSelectionColor);
        // Color maps are session-scoped until persistence is reintroduced
        UpdateAllHotkeyText();
        SetWindowSize(360, 120); // Prepare initial size, but keep window hidden

        // Set initial UI values from our master defaults
        LumaRNumberBox().Value(m_lumaWeights[0]);
        LumaGNumberBox().Value(m_lumaWeights[1]);
        LumaBNumberBox().Value(m_lumaWeights[2]);
        // Initialize brightness protection delay UI
        {
            auto root = this->Content().try_as<FrameworkElement>();
            if (root)
            {
                auto nb = root.FindName(L"BrightnessDelayNumberBox").try_as<Controls::NumberBox>();
                if (nb) nb.Value(m_brightnessDelayFrames);
            }
        }

        m_isAppInitialized = true;
        m_isSavingEnabled = true; // enable persistence after initial UI wiring

        // Do not start selection automatically. The global hotkey will start
        // the operation (similar to Snipping Tool) after app launch.

        winvert4::Log("MainWindow: ctor end");
    }

    winrt::Winvert4::implementation::MainWindow::~MainWindow()
    {
        // TODO: Save settings
        RemoveWindowSubclass(m_mainHwnd, &MainWindow::WindowSubclassProc, 1);
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
    }

    int32_t MainWindow::MyProperty() { return 0; }
    void winrt::Winvert4::implementation::MainWindow::MyProperty(int32_t) {}

    void winrt::Winvert4::implementation::MainWindow::ToggleSnipping()
    {
        winvert4::Log("MainWindow: ToggleSnipping");
        StartScreenSelection();
    }

    // --- XAML Event Handlers ---
    void winrt::Winvert4::implementation::MainWindow::BrightnessProtection_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        int idx = SelectedTabIndex();
        if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;
        auto& s = m_windowSettings[idx];
        s.isBrightnessProtectionEnabled = !s.isBrightnessProtectionEnabled;
        UpdateUIState();
        // Push to all effect windows in the group immediately
        ApplyGlobalColorMapsToSettings(s);
        UpdateSettingsForGroup(idx, s);
    }

    void winrt::Winvert4::implementation::MainWindow::InvertEffect_Click(IInspectable const&, RoutedEventArgs const&)
    {
        int idx = SelectedTabIndex();
        if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return; // NOLINT(bugprone-branch-clone)
        auto& s = m_windowSettings[idx];
        s.isInvertEffectEnabled = !s.isInvertEffectEnabled;
        UpdateUIState();
        ApplyGlobalColorMapsToSettings(s);
        UpdateSettingsForGroup(idx, s);
    }

    void winrt::Winvert4::implementation::MainWindow::AddWindow_Click(IInspectable const&, RoutedEventArgs const&)
    {
        StartScreenSelection();
    }

    void winrt::Winvert4::implementation::MainWindow::RemoveWindow_Click(IInspectable const&, RoutedEventArgs const&)
    {
        int idx = SelectedTabIndex();
        if (idx < 0) return;
        auto items = RegionsTabView().TabItems();
        if (idx >= static_cast<int>(items.Size())) return;

        // Release effect window resources
        if (idx < static_cast<int>(m_effectWindows.size()))
        {
            if (auto& wnd = m_effectWindows[idx])
            {
                wnd->Hide();
                wnd.reset();
            }
            m_effectWindows.erase(m_effectWindows.begin() + idx);
        }
        if (idx < static_cast<int>(m_effectWindowExtras.size()))
        {
            auto& extras = m_effectWindowExtras[idx];
            for (auto& ew : extras)
            {
                if (ew) { ew->Hide(); ew.reset(); }
            }
            m_effectWindowExtras.erase(m_effectWindowExtras.begin() + idx);
        }
        if (idx < static_cast<int>(m_windowSettings.size()))
        {
            m_windowSettings.erase(m_windowSettings.begin() + idx);
        }
        if (idx < static_cast<int>(m_windowHidden.size()))
        {
            m_windowHidden.erase(m_windowHidden.begin() + idx);
        }
        if (idx < static_cast<int>(m_hasPreviewBackup.size()))
        {
            m_hasPreviewBackup.erase(m_hasPreviewBackup.begin() + idx);
        }
        if (idx < static_cast<int>(m_previewBackup.size()))
        {
            m_previewBackup.erase(m_previewBackup.begin() + idx);
        }

        // Remove tab and renumber headers
        items.RemoveAt(static_cast<uint32_t>(idx));
        for (uint32_t i = 0; i < items.Size(); ++i)
        {
            if (auto tab = items.GetAt(i).try_as<TabViewItem>())
            {
                tab.Header(box_value(L"Region " + std::to_wstring(i + 1)));
            }
        }
        // Adjust selection
        if (items.Size() > 0)
        {
            RegionsTabView().SelectedIndex(static_cast<int>(std::min<uint32_t>(items.Size() - 1, static_cast<uint32_t>(idx))));
        }
        UpdateUIState();
    }

    void winrt::Winvert4::implementation::MainWindow::HideAllWindows_Click(IInspectable const&, RoutedEventArgs const&)
    {
        int idx = SelectedTabIndex();
        if (idx < 0) return;
        if (idx >= static_cast<int>(m_windowSettings.size())) return;
        if (idx >= static_cast<int>(m_windowHidden.size())) m_windowHidden.resize(m_windowSettings.size(), false);
        m_windowHidden[idx] = !m_windowHidden[idx];
        SetHiddenForGroup(idx, m_windowHidden[idx]);
        UpdateUIState();
    }

    void winrt::Winvert4::implementation::MainWindow::SettingsButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_wereWindowsHiddenForSettings = m_areWindowsHidden;
        if (!m_areWindowsHidden)
        {
            m_areWindowsHidden = true;
            SetHiddenForAll(true);
            UpdateUIState();
        }

        RootPanel().Visibility(Visibility::Collapsed);
        SettingsPanel().Visibility(Visibility::Visible);

        // Ensure the editor is visible by default when opening settings
        if (auto panel = FilterEditorPanel()) panel.Visibility(Visibility::Visible);

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

        // Default all settings sections and reset sliders to neutral
        CollapseAllSettingsExpanders();
        // Reset simple sliders to neutral state on entry
        m_isUpdatingSimpleUI = true;
        m_simpleBrightness = 0.0f;
        m_simpleContrast   = 1.0f;
        m_simpleSaturation = 1.0f;
        m_simpleHueAngle   = 0.0f;
        m_simpleTemperature = 0.0f;
        m_simpleTint = 0.0f;
        if (auto s = BrightnessSlider()) s.Value(m_simpleBrightness);
        if (auto s = ContrastSlider())   s.Value(m_simpleContrast);
        if (auto s = SaturationSlider()) s.Value(m_simpleSaturation);
        if (auto s = HueSlider())        s.Value(m_simpleHueAngle);
        if (auto s = TemperatureSlider()) s.Value(m_simpleTemperature);
        if (auto s = TintSlider())        s.Value(m_simpleTint);
        m_isUpdatingSimpleUI = false;
        // Compose and display the neutral matrix in the grid; if preview is active, push it
        {
            float m[16], off[4]; ComposeSimpleMatrix(m, off); WriteMatrixToGrid(m, off);
            if (m_isPreviewActive)
            {
                int idx = SelectedTabIndex();
                if (idx >= 0 && idx < static_cast<int>(m_windowSettings.size()))
                {
                    if (static_cast<int>(m_hasPreviewBackup.size()) <= idx) { m_hasPreviewBackup.resize(idx + 1, false); m_previewBackup.resize(idx + 1); }
                    if (!m_hasPreviewBackup[idx]) { m_previewBackup[idx] = m_windowSettings[idx]; m_hasPreviewBackup[idx] = true; }
                    m_windowSettings[idx].isCustomEffectActive = true;
                    memcpy(m_windowSettings[idx].colorMat, m, sizeof(m));
                    memcpy(m_windowSettings[idx].colorOffset, off, sizeof(off));
                    UpdateSettingsForGroup(idx);
                }
            }
        }
        RefreshColorMapList();
    }

    void winrt::Winvert4::implementation::MainWindow::BackButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_rebindingState != RebindingState::None) {
            m_rebindingState = RebindingState::None;
            RebindStatusText().Text(L"");
            RebindInvertHotkeyButton().IsEnabled(true);
            RebindFilterHotkeyButton().IsEnabled(true);
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
            // Show all windows, then reapply per-tab hidden states
            SetHiddenForAll(false);
            for (int i = 0; i < static_cast<int>(m_windowHidden.size()); ++i)
            {
                if (m_windowHidden[i]) SetHiddenForGroup(i, true);
            }
        }

        // If a preview was active, restore original settings
        if (m_isPreviewActive)
        {
            for (int i = 0; i < static_cast<int>(m_windowSettings.size()); ++i)
            {
                if (i < static_cast<int>(m_hasPreviewBackup.size()) && m_hasPreviewBackup[i])
                {
                    m_windowSettings[i] = m_previewBackup[i];
                    UpdateSettingsForGroup(i);
                    m_hasPreviewBackup[i] = false;
                }
            }
            m_isPreviewActive = false;
        }

        SettingsPanel().Visibility(Visibility::Collapsed);
        RootPanel().Visibility(Visibility::Visible);
        // Fixed control panel size based on whether any windows exist
        {
            bool hasWindows = RegionsTabView().TabItems().Size() > 0;
            SetWindowSize(360, 120);
        }

        // Reapply composite filters for all tabs (in case matrices were edited)
        //auto items = RegionsTabView().TabItems();
        //for (uint32_t i = 0; i < items.Size(); ++i)
        //{
        //    ApplyCompositeCustomFiltersForTab(static_cast<int>(i));
        //}
    }

    void winrt::Winvert4::implementation::MainWindow::SettingsPanel_SizeChanged(IInspectable const&, winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const&)
    {
        UpdateSettingsColumnsForWindowState();
    }

    void winrt::Winvert4::implementation::MainWindow::UpdateSettingsColumnsForWindowState()
    {
        bool isMax = ::IsZoomed(m_mainHwnd) != 0;
        using winrt::Microsoft::UI::Xaml::GridLengthHelper;
        AboutColumn().Width(isMax ? GridLengthHelper::FromPixels(360.0) : GridLengthHelper::FromPixels(0.0));
        AboutScroll().Visibility(isMax ? Visibility::Visible : Visibility::Collapsed);
    }

    void winrt::Winvert4::implementation::MainWindow::CollapseAllSettingsExpanders()
    {
        // Safely collapse any expander if it exists in the tree
        if (auto expander = BrightnessExpander()) expander.IsExpanded(false);
        if (SelectionColorExpander()) SelectionColorExpander().IsExpanded(false);
        //if (UpdateRateExpander()) UpdateRateExpander().IsExpanded(false);
        if (HotkeysExpander()) HotkeysExpander().IsExpanded(false);
        // Custom Filters collapsed by default on entry
        if (CustomFiltersExpander()) CustomFiltersExpander().IsExpanded(false);
        if (FilterEditorPanel()) FilterEditorPanel().Visibility(Visibility::Collapsed);
    }

    void winrt::Winvert4::implementation::MainWindow::CustomFiltersExpander_Collapsed(IInspectable const&, Microsoft::UI::Xaml::Controls::ExpanderCollapsedEventArgs const&)
    {
        // Hide the editor when the Custom Filters expander collapses
        if (auto panel = FilterEditorPanel()) panel.Visibility(Visibility::Collapsed);
    }

    void winrt::Winvert4::implementation::MainWindow::CustomFiltersExpander_Expanding(IInspectable const&, Microsoft::UI::Xaml::Controls::ExpanderExpandingEventArgs const&)
    {
        // When expanded, always show the editor and default to sliders visible
        if (auto panel = FilterEditorPanel()) panel.Visibility(Visibility::Visible);
        // Default to simple sliders (Advanced off)
        if (auto t = AdvancedMatrixToggle()) t.IsOn(false);
        if (auto panelSliders = SimpleSlidersPanel()) panelSliders.Visibility(Visibility::Visible);
        if (auto root = this->Content().try_as<FrameworkElement>())
        {
            if (auto adv = root.FindName(L"AdvancedMatrixPanel").try_as<Controls::StackPanel>())
                adv.Visibility(Visibility::Collapsed);
        }
    }

    // --- Simple vs Advanced filter mode ---
    void winrt::Winvert4::implementation::MainWindow::AdvancedMatrixToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        bool advanced = AdvancedMatrixToggle().IsOn();
        // Keep the header (with toggle) visible; alternate sliders vs advanced matrix panel
        if (auto panel = SimpleSlidersPanel()) panel.Visibility(advanced ? Visibility::Collapsed : Visibility::Visible);
        // Advanced matrix container includes labels + grid so labels don't reserve space when hidden
        Controls::StackPanel advPanel{ nullptr };
        if (auto root = this->Content().try_as<FrameworkElement>())
        {
            advPanel = root.FindName(L"AdvancedMatrixPanel").try_as<Controls::StackPanel>();
        }
        if (advPanel) advPanel.Visibility(advanced ? Visibility::Visible : Visibility::Collapsed);
        // If switching to advanced, generate the corresponding matrix from current slider values
        if (advanced)
        {
            float m[16], off[4]; ComposeSimpleMatrix(m, off); WriteMatrixToGrid(m, off);
        }
        else
        {
            // Switching back to simple: infer slider values from current matrix grid
            float m[16], off[4]; ReadMatrixFromGrid(m, off); UpdateSlidersFromMatrix(m, off);
        }
    }

    void winrt::Winvert4::implementation::MainWindow::ComposeSimpleMatrix(float (&outMat)[16], float (&outOff)[4])
    {
        for (int i = 0; i < 16; ++i) outMat[i] = 0.0f;
        outMat[0] = outMat[5] = outMat[10] = outMat[15] = 1.0f;
        outOff[0] = outOff[1] = outOff[2] = outOff[3] = 0.0f;

        float c = m_simpleContrast;
        outMat[0] *= c; outMat[5] *= c; outMat[10] *= c;
        float co = 0.5f * (1.0f - c);
        outOff[0] += co; outOff[1] += co; outOff[2] += co;

        float s = m_simpleSaturation;
        const float Lr = kDefaultLumaWeights[0], Lg = kDefaultLumaWeights[1], Lb = kDefaultLumaWeights[2];
        float satMat[16] = { 0 };
        satMat[15] = 1.0f;
        satMat[0] = (1 - s) * Lr + s;  satMat[1] = (1 - s) * Lg;      satMat[2] = (1 - s) * Lb;
        satMat[4] = (1 - s) * Lr;      satMat[5] = (1 - s) * Lg + s;  satMat[6] = (1 - s) * Lb;
        satMat[8] = (1 - s) * Lr;      satMat[9] = (1 - s) * Lg;      satMat[10]= (1 - s) * Lb + s;

        float tmp[16] = {0};
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k)
                for (int c2 = 0; c2 < 4; ++c2)
                    tmp[r*4 + c2] += satMat[r*4 + k] * outMat[k*4 + c2];
        for (int i = 0; i < 16; ++i) outMat[i] = tmp[i];

        float t = m_simpleTemperature;
        float ti = m_simpleTint;
        float rScale = 1.0f + 0.15f * t - 0.05f * ti;
        float gScale = 1.0f + 0.10f * ti;
        float bScale = 1.0f - 0.15f * t - 0.05f * ti;
        outMat[0] *= rScale; outMat[5] *= gScale; outMat[10] *= bScale;

        // Apply hue rotation (degrees) using Rec.709 luma constants
        {
            float deg = m_simpleHueAngle;
            const float PI = 3.1415926535f;
            float rad = deg * PI / 180.0f;
            float cH = cosf(rad);
            float sH = sinf(rad);
            const float a = 0.213f; // luma R
            const float bL = 0.715f; // luma G
            const float d = 0.072f; // luma B
            float hue[16] = {0};
            hue[15] = 1.0f;
            hue[0] = a + cH * (1 - a) + sH * (-a);     hue[1] = bL + cH * (-bL) + sH * (-bL);   hue[2]  = d + cH * (-d) + sH * (1 - d);
            hue[4] = a + cH * (-a) + sH * (0.143f);    hue[5] = bL + cH * (1 - bL) + sH * (0.140f); hue[6]  = d + cH * (-d) + sH * (-0.283f);
            hue[8] = a + cH * (-a) + sH * (-0.787f);   hue[9] = bL + cH * (-bL) + sH * (0.715f);  hue[10] = d + cH * (1 - d) + sH * (0.072f);
            float tmpH[16] = {0};
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k)
                    for (int c2 = 0; c2 < 4; ++c2)
                        tmpH[r*4 + c2] += hue[r*4 + k] * outMat[k*4 + c2];
            for (int i = 0; i < 16; ++i) outMat[i] = tmpH[i];
        }

        float b = m_simpleBrightness;
        outOff[0] += b; outOff[1] += b; outOff[2] += b;
    }

    void winrt::Winvert4::implementation::MainWindow::EnsureFilterMatrixGridInitialized()
    {
        auto grid = FilterMatrixGrid();
        if (!grid) return;
        if (grid.Children().Size() > 0) return;

        for (int r = 0; r < 5; ++r)
        {
            for (int c = 0; c < 5; ++c)
            {
                auto tb = Microsoft::UI::Xaml::Controls::TextBox();
                tb.TextAlignment(Microsoft::UI::Xaml::TextAlignment::Center);
                tb.Width(60); tb.Height(32);
                Microsoft::UI::Xaml::Controls::Grid::SetRow(tb, r);
                Microsoft::UI::Xaml::Controls::Grid::SetColumn(tb, c);
                // Lock 5th column to constants 0,0,0,0,1
                if (c == 4)
                {
                    tb.IsReadOnly(true);
                    tb.IsTabStop(false);
                    double v = (r == 4) ? 1.0 : 0.0;
                    wchar_t buf[32]; swprintf_s(buf, L"%.3f", v);
                    tb.Text(buf);
                }
                else
                {
                    // Commit on Enter via shared handler; also commit on focus loss
                    tb.KeyDown({ this, &MainWindow::EnterKeyCommit_Unfocus });
                    tb.LostFocus([this](auto const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
                    {
                        CommitMatrixTextBoxes_();
                    });
                }
                grid.Children().Append(tb);
            }
        }
    }

    void winrt::Winvert4::implementation::MainWindow::CommitMatrixTextBoxes_()
    {
        float m[16], off[4]; ReadMatrixFromGrid(m, off);
        // If preview is active, push live update
        if (m_isPreviewActive)
        {
            int idx = SelectedTabIndex();
            if (idx >= 0 && idx < static_cast<int>(m_windowSettings.size()))
            {
                if (static_cast<int>(m_hasPreviewBackup.size()) <= idx) { m_hasPreviewBackup.resize(idx + 1, false); m_previewBackup.resize(idx + 1); }
                if (!m_hasPreviewBackup[idx]) { m_previewBackup[idx] = m_windowSettings[idx]; m_hasPreviewBackup[idx] = true; }
                m_windowSettings[idx].isCustomEffectActive = true;
                memcpy(m_windowSettings[idx].colorMat, m, sizeof(m));
                memcpy(m_windowSettings[idx].colorOffset, off, sizeof(off));
                UpdateSettingsForGroup(idx);
            }
        }
    }

    void winrt::Winvert4::implementation::MainWindow::EnterKeyCommit_Unfocus(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e)
    {
        using winrt::Windows::System::VirtualKey;
        if (e.Key() != VirtualKey::Enter) return;
        if (auto cb = sender.try_as<Controls::ComboBox>())
        {
            cb.IsDropDownOpen(false);
        }
        if (sender.try_as<Controls::TextBox>())
        {
            CommitMatrixTextBoxes_();
        }
        if (auto root = this->Content().try_as<FrameworkElement>())
        {
            root.Focus(FocusState::Programmatic);
        }
        e.Handled(true);
    }

    void winrt::Winvert4::implementation::MainWindow::ReadMatrixFromGrid(float (&outMat)[16], float (&outOff)[4])
    {
        for (int i = 0; i < 16; ++i) outMat[i] = 0.0f;
        outMat[0] = outMat[5] = outMat[10] = outMat[15] = 1.0f;
        for (int i = 0; i < 4; ++i) outOff[i] = 0.0f;
        auto grid = FilterMatrixGrid(); if (!grid) return;
        EnsureFilterMatrixGridInitialized();
        for (auto child : grid.Children())
        {
            auto tb = child.try_as<TextBox>(); if (!tb) continue;
            int r = Microsoft::UI::Xaml::Controls::Grid::GetRow(tb);
            int c = Microsoft::UI::Xaml::Controls::Grid::GetColumn(tb);
            try {
                float v = std::stof(std::wstring(tb.Text()));
                if (r >= 0 && r < 4 && c >= 0 && c < 4) outMat[r * 4 + c] = v;
                else if (r == 4 && c >= 0 && c < 4) outOff[c] = v; // offsets now in 5th row
            } catch (...) {}
        }
    }

    void winrt::Winvert4::implementation::MainWindow::UpdateSlidersFromMatrix(const float (&mat)[16], const float (&off)[4])
    {
        // Infer simple params from mat/off produced by ComposeSimpleMatrix
        // Using default luminance weights used in composition
        const float Lr = kDefaultLumaWeights[0], Lg = kDefaultLumaWeights[1], Lb = kDefaultLumaWeights[2];
        auto clampf = [](float v, float lo, float hi){ return (v < lo) ? lo : (v > hi ? hi : v); };

        // 3x3 rows
        float A00 = mat[0],  A01 = mat[1],  A02 = mat[2];
        float A10 = mat[4],  A11 = mat[5],  A12 = mat[6];
        float A20 = mat[8],  A21 = mat[9],  A22 = mat[10];

        float kR = A00 + A01 + A02; if (fabs(kR) < 1e-6f) kR = 1e-6f;
        float kG = A10 + A11 + A12; if (fabs(kG) < 1e-6f) kG = 1e-6f;
        float kB = A20 + A21 + A22; if (fabs(kB) < 1e-6f) kB = 1e-6f;

        float vR0 = A00 / kR, vR1 = A01 / kR, vR2 = A02 / kR;
        float vG0 = A10 / kG, vG1 = A11 / kG, vG2 = A12 / kG;
        float vB0 = A20 / kB, vB1 = A21 / kB, vB2 = A22 / kB;

        float oneMinusSR = (vR1 + vR2) / (1.0f - Lr);
        float oneMinusSG = (vG0 + vG2) / (1.0f - Lg);
        float oneMinusSB = (vB0 + vB1) / (1.0f - Lb);
        float sR = 1.0f - oneMinusSR;
        float sG = 1.0f - oneMinusSG;
        float sB = 1.0f - oneMinusSB;
        float sat = clampf((sR + sG + sB) / 3.0f, 0.0f, 1.0f);

        // Solve tint/temperature from row gain ratios
        float Rratio = kR / kG;
        float Bratio = kB / kG;
        // ti (x)
        float denom = (Rratio + Bratio + 1.0f);
        float ti = 0.0f;
        if (fabs(denom) > 1e-6f)
            ti = 10.0f * (2.0f - (Rratio + Bratio)) / denom;
        float t = ((Rratio - Bratio) * (1.0f + 0.10f * ti)) / 0.30f;

        float rScale = 1.0f + 0.15f * t - 0.05f * ti;
        float gScale = 1.0f + 0.10f * ti;
        float bScale = 1.0f - 0.15f * t - 0.05f * ti;
        if (fabs(rScale) < 1e-6f) rScale = (rScale < 0 ? -1e-6f : 1e-6f);
        if (fabs(gScale) < 1e-6f) gScale = (gScale < 0 ? -1e-6f : 1e-6f);
        if (fabs(bScale) < 1e-6f) bScale = (bScale < 0 ? -1e-6f : 1e-6f);

        float cR = kR / rScale;
        float cG = kG / gScale;
        float cB = kB / bScale;
        float contrast = clampf((cR + cG + cB) / 3.0f, 0.0f, 2.0f);

        float offAvg = (off[0] + off[1] + off[2]) / 3.0f;
        float co = 0.5f * (1.0f - contrast);
        float brightness = clampf(offAvg - co, -1.0f, 1.0f);

        // Apply to model + UI with reentrancy guard
        m_isUpdatingSimpleUI = true;
        m_simpleBrightness = brightness;
        m_simpleContrast   = contrast;
        m_simpleSaturation = sat;
        m_simpleTemperature = t;
        m_simpleTint = ti;
        m_simpleHueAngle = 0.0f; // cannot reliably infer hue from arbitrary matrix

        if (auto s = BrightnessSlider()) s.Value(m_simpleBrightness);
        if (auto s = ContrastSlider())   s.Value(m_simpleContrast);
        if (auto s = SaturationSlider()) s.Value(m_simpleSaturation);
        if (auto s = HueSlider())        s.Value(m_simpleHueAngle);
        if (auto s = TemperatureSlider()) s.Value(m_simpleTemperature);
        if (auto s = HueSlider())        s.Value(m_simpleHueAngle);
        if (auto s = TintSlider())        s.Value(m_simpleTint);
        m_isUpdatingSimpleUI = false;
    }
    void winrt::Winvert4::implementation::MainWindow::WriteMatrixToGrid(const float (&mat)[16], const float (&off)[4])
    {
        auto grid = FilterMatrixGrid(); if (!grid) return;
        EnsureFilterMatrixGridInitialized();
        for (auto child : grid.Children())
        {
            auto tb = child.try_as<TextBox>(); if (!tb) continue;
            int r = Microsoft::UI::Xaml::Controls::Grid::GetRow(tb);
            int c = Microsoft::UI::Xaml::Controls::Grid::GetColumn(tb);
            double v = 0.0;
            if (r < 4 && c < 4) v = mat[r * 4 + c];
            else if (r == 4 && c < 4) v = off[c];              // offsets now in last row
            else if (c == 4 && r < 4) v = 0.0;                 // lock last column to 0 for first 4 rows
            else if (r == 4 && c == 4) v = 1.0;                // bottom-right constant 1
            wchar_t buf[32]; swprintf_s(buf, L"%.3f", v);
            tb.Text(buf);
        }
    }

    void winrt::Winvert4::implementation::MainWindow::BrightnessSlider_ValueChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (m_isUpdatingSimpleUI) return;
        m_simpleBrightness = static_cast<float>(e.NewValue());
        float m[16], off[4]; ComposeSimpleMatrix(m, off); WriteMatrixToGrid(m, off);
        if (m_isPreviewActive)
        {
            int idx = SelectedTabIndex();
            if (idx >= 0 && idx < static_cast<int>(m_windowSettings.size()))
            {
                if (static_cast<int>(m_hasPreviewBackup.size()) <= idx) { m_hasPreviewBackup.resize(idx + 1, false); m_previewBackup.resize(idx + 1); }
                if (!m_hasPreviewBackup[idx]) { m_previewBackup[idx] = m_windowSettings[idx]; m_hasPreviewBackup[idx] = true; }
                m_windowSettings[idx].isCustomEffectActive = true;
                memcpy(m_windowSettings[idx].colorMat, m, sizeof(m));
                memcpy(m_windowSettings[idx].colorOffset, off, sizeof(off));
                UpdateSettingsForGroup(idx);
            }
        }
    }

    void winrt::Winvert4::implementation::MainWindow::HueSlider_ValueChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (m_isUpdatingSimpleUI) return;
        m_simpleHueAngle = static_cast<float>(e.NewValue());
        float m[16], off[4]; ComposeSimpleMatrix(m, off); WriteMatrixToGrid(m, off);
        if (m_isPreviewActive)
        {
            int idx = SelectedTabIndex();
            if (idx >= 0 && idx < static_cast<int>(m_windowSettings.size()))
            {
                if (static_cast<int>(m_hasPreviewBackup.size()) <= idx) { m_hasPreviewBackup.resize(idx + 1, false); m_previewBackup.resize(idx + 1); }
                if (!m_hasPreviewBackup[idx]) { m_previewBackup[idx] = m_windowSettings[idx]; m_hasPreviewBackup[idx] = true; }
                m_windowSettings[idx].isCustomEffectActive = true;
                memcpy(m_windowSettings[idx].colorMat, m, sizeof(m));
                memcpy(m_windowSettings[idx].colorOffset, off, sizeof(off));
                UpdateSettingsForGroup(idx);
            }
        }
    }
    void winrt::Winvert4::implementation::MainWindow::ContrastSlider_ValueChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (m_isUpdatingSimpleUI) return;
        m_simpleContrast = static_cast<float>(e.NewValue());
        float m[16], off[4]; ComposeSimpleMatrix(m, off); WriteMatrixToGrid(m, off);
        if (m_isPreviewActive)
        {
            int idx = SelectedTabIndex();
            if (idx >= 0 && idx < static_cast<int>(m_windowSettings.size()))
            {
                if (static_cast<int>(m_hasPreviewBackup.size()) <= idx) { m_hasPreviewBackup.resize(idx + 1, false); m_previewBackup.resize(idx + 1); }
                if (!m_hasPreviewBackup[idx]) { m_previewBackup[idx] = m_windowSettings[idx]; m_hasPreviewBackup[idx] = true; }
                m_windowSettings[idx].isCustomEffectActive = true;
                memcpy(m_windowSettings[idx].colorMat, m, sizeof(m));
                memcpy(m_windowSettings[idx].colorOffset, off, sizeof(off));
                UpdateSettingsForGroup(idx);
            }
        }
    }

    void winrt::Winvert4::implementation::MainWindow::SaturationSlider_ValueChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (m_isUpdatingSimpleUI) return;
        m_simpleSaturation = static_cast<float>(e.NewValue());
        float m[16], off[4]; ComposeSimpleMatrix(m, off); WriteMatrixToGrid(m, off);
        if (m_isPreviewActive)
        {
            int idx = SelectedTabIndex();
            if (idx >= 0 && idx < static_cast<int>(m_windowSettings.size()))
            {
                if (static_cast<int>(m_hasPreviewBackup.size()) <= idx) { m_hasPreviewBackup.resize(idx + 1, false); m_previewBackup.resize(idx + 1); }
                if (!m_hasPreviewBackup[idx]) { m_previewBackup[idx] = m_windowSettings[idx]; m_hasPreviewBackup[idx] = true; }
                m_windowSettings[idx].isCustomEffectActive = true;
                memcpy(m_windowSettings[idx].colorMat, m, sizeof(m));
                memcpy(m_windowSettings[idx].colorOffset, off, sizeof(off));
                UpdateSettingsForGroup(idx);
            }
        }
    }

    void winrt::Winvert4::implementation::MainWindow::TemperatureSlider_ValueChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (m_isUpdatingSimpleUI) return;
        m_simpleTemperature = static_cast<float>(e.NewValue());
        float m[16], off[4]; ComposeSimpleMatrix(m, off); WriteMatrixToGrid(m, off);
        if (m_isPreviewActive)
        {
            int idx = SelectedTabIndex();
            if (idx >= 0 && idx < static_cast<int>(m_windowSettings.size()))
            {
                if (static_cast<int>(m_hasPreviewBackup.size()) <= idx) { m_hasPreviewBackup.resize(idx + 1, false); m_previewBackup.resize(idx + 1); }
                if (!m_hasPreviewBackup[idx]) { m_previewBackup[idx] = m_windowSettings[idx]; m_hasPreviewBackup[idx] = true; }
                m_windowSettings[idx].isCustomEffectActive = true;
                memcpy(m_windowSettings[idx].colorMat, m, sizeof(m));
                memcpy(m_windowSettings[idx].colorOffset, off, sizeof(off));
                UpdateSettingsForGroup(idx);
            }
        }
    }

    void winrt::Winvert4::implementation::MainWindow::TintSlider_ValueChanged(IInspectable const&, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (m_isUpdatingSimpleUI) return;
        m_simpleTint = static_cast<float>(e.NewValue());
        float m[16], off[4]; ComposeSimpleMatrix(m, off); WriteMatrixToGrid(m, off);
        if (m_isPreviewActive)
        {
            int idx = SelectedTabIndex();
            if (idx >= 0 && idx < static_cast<int>(m_windowSettings.size()))
            {
                if (static_cast<int>(m_hasPreviewBackup.size()) <= idx) { m_hasPreviewBackup.resize(idx + 1, false); m_previewBackup.resize(idx + 1); }
                if (!m_hasPreviewBackup[idx]) { m_previewBackup[idx] = m_windowSettings[idx]; m_hasPreviewBackup[idx] = true; }
                m_windowSettings[idx].isCustomEffectActive = true;
                memcpy(m_windowSettings[idx].colorMat, m, sizeof(m));
                memcpy(m_windowSettings[idx].colorOffset, off, sizeof(off));
                UpdateSettingsForGroup(idx);
            }
        }
    }

    void winrt::Winvert4::implementation::MainWindow::SimpleResetButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_simpleBrightness = 0.0f; m_simpleContrast = 1.0f; m_simpleSaturation = 1.0f; m_simpleHueAngle = 0.0f; m_simpleTemperature = 0.0f; m_simpleTint = 0.0f;
        if (auto s = BrightnessSlider()) s.Value(m_simpleBrightness);
        if (auto s = ContrastSlider()) s.Value(m_simpleContrast);
        if (auto s = SaturationSlider()) s.Value(m_simpleSaturation);
        if (auto s = HueSlider())        s.Value(m_simpleHueAngle);
        if (auto s = TemperatureSlider()) s.Value(m_simpleTemperature);
        if (auto s = TintSlider())        s.Value(m_simpleTint);
        float m[16], off[4]; ComposeSimpleMatrix(m, off); WriteMatrixToGrid(m, off);
    }



    void winrt::Winvert4::implementation::MainWindow::ShowFpsToggle_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        // Sync setting and push to existing windows
        m_showFpsOverlay = ShowFpsToggle().IsOn();
        for (size_t i = 0; i < m_effectWindows.size(); ++i)
        {
            m_windowSettings[i].showFpsOverlay = m_showFpsOverlay;
            UpdateSettingsForGroup(static_cast<int>(i));
        }
        SaveAppState();
    }

    void winrt::Winvert4::implementation::MainWindow::RebindInvertHotkeyButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rebindingState = RebindingState::Invert;
        RebindStatusText().Text(L"Press key combo for Invert/Add. ESC to cancel.");
        RebindInvertHotkeyButton().IsEnabled(false);
        RebindFilterHotkeyButton().IsEnabled(true);
        RebindRemoveHotkeyButton().IsEnabled(true);
        SetFocus(m_mainHwnd);
    }

    void winrt::Winvert4::implementation::MainWindow::RebindFilterHotkeyButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rebindingState = RebindingState::Filter;
        RebindStatusText().Text(L"Press key combo for Filter/Add. ESC to cancel.");
        RebindInvertHotkeyButton().IsEnabled(true);
        RebindFilterHotkeyButton().IsEnabled(false);
        RebindRemoveHotkeyButton().IsEnabled(true);
        SetFocus(m_mainHwnd);
    }

    void winrt::Winvert4::implementation::MainWindow::RebindRemoveHotkeyButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_rebindingState = RebindingState::Remove;
        RebindStatusText().Text(L"Press key combo for Remove Last. ESC to cancel.");
        RebindInvertHotkeyButton().IsEnabled(true);
        RebindFilterHotkeyButton().IsEnabled(true);
        RebindRemoveHotkeyButton().IsEnabled(false);
        SetFocus(m_mainHwnd);
    }

    void winrt::Winvert4::implementation::MainWindow::SelectionColorPicker_ColorChanged(ColorPicker const&, ColorChangedEventArgs const& args)
    {
        auto newColor = args.NewColor();
        m_selectionColor = RGB(newColor.R, newColor.G, newColor.B);
        SaveAppState();
    }

    void winrt::Winvert4::implementation::MainWindow::SelectionColorEnable_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        m_useCustomSelectionColor = SelectionColorEnableToggle().IsOn();
        if (auto cp = SelectionColorPicker()) cp.IsEnabled(m_useCustomSelectionColor);
        // Redraw selection overlay if active
        if (m_selectionHwnd) InvalidateRect(m_selectionHwnd, nullptr, FALSE);
        SaveAppState();
    }

    // --- Core Logic ---
    void winrt::Winvert4::implementation::MainWindow::RegisterAllHotkeys()
    {
        UnregisterHotKey(m_mainHwnd, HOTKEY_INVERT_ID);
        UnregisterHotKey(m_mainHwnd, HOTKEY_FILTER_ID);
        UnregisterHotKey(m_mainHwnd, HOTKEY_REMOVE_ID);

        RegisterHotKey(m_mainHwnd, HOTKEY_INVERT_ID, m_hotkeyInvertMod, m_hotkeyInvertVk);
        RegisterHotKey(m_mainHwnd, HOTKEY_FILTER_ID, m_hotkeyFilterMod, m_hotkeyFilterVk);
        RegisterHotKey(m_mainHwnd, HOTKEY_REMOVE_ID, m_hotkeyRemoveMod, m_hotkeyRemoveVk);
        UpdateAllHotkeyText();
    }

    void winrt::Winvert4::implementation::MainWindow::OnInvertHotkeyPressed()
    {
        m_pendingEffect = PendingEffect::Invert;
        StartScreenSelection();
    }

    void winrt::Winvert4::implementation::MainWindow::OnRemoveHotkeyPressed()
    {
        // Mark hotkey-triggered removal so we can close app if it was the last window
        m_lastRemovalInitiatedByHotkey = true;
        m_lastRemovalViaUI = false;
        // Remove the most recent (last) region window
        auto items = RegionsTabView().TabItems();
        if (items.Size() == 0) return;
        int idx = static_cast<int>(items.Size()) - 1;

        // Release effect window resources
        if (idx < static_cast<int>(m_effectWindows.size()))
        {
            if (auto& wnd = m_effectWindows[idx])
            {
                wnd->Hide();
                wnd.reset();
            }
            m_effectWindows.erase(m_effectWindows.begin() + idx);
        }
        if (idx < static_cast<int>(m_effectWindowExtras.size()))
        {
            auto& extras = m_effectWindowExtras[idx];
            for (auto& ew : extras)
            {
                if (ew) { ew->Hide(); ew.reset(); }
            }
            m_effectWindowExtras.erase(m_effectWindowExtras.begin() + idx);
        }
        if (idx < static_cast<int>(m_windowSettings.size()))
        {
            m_windowSettings.erase(m_windowSettings.begin() + idx);
        }
        if (idx < static_cast<int>(m_windowHidden.size()))
        {
            m_windowHidden.erase(m_windowHidden.begin() + idx);
        }
        if (idx < static_cast<int>(m_hasPreviewBackup.size()))
        {
            m_hasPreviewBackup.erase(m_hasPreviewBackup.begin() + idx);
        }
        if (idx < static_cast<int>(m_previewBackup.size()))
        {
            m_previewBackup.erase(m_previewBackup.begin() + idx);
        }

        // Remove tab and renumber headers
        items.RemoveAt(static_cast<uint32_t>(idx));
        for (uint32_t i = 0; i < items.Size(); ++i)
        {
            if (auto tab = items.GetAt(i).try_as<TabViewItem>())
            {
                tab.Header(box_value(L"Region " + std::to_wstring(i + 1)));
            }
        }
        // Adjust selection or close app if no windows remain
        if (items.Size() > 0)
        {
            RegionsTabView().SelectedIndex(static_cast<int>(items.Size()) - 1);
            UpdateUIState();
        }
        else
        {
            // No regions left: close the app when removal was via hotkey
            ::PostMessage(m_mainHwnd, WM_CLOSE, 0, 0);
        }
    }

    void winrt::Winvert4::implementation::MainWindow::OnFilterHotkeyPressed()
    {
        m_pendingEffect = PendingEffect::Filter;
        StartScreenSelection();
    }

    void winrt::Winvert4::implementation::MainWindow::StartScreenSelection()
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

    void winrt::Winvert4::implementation::MainWindow::CaptureScreenBitmap()
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

    void winrt::Winvert4::implementation::MainWindow::EnumerateMonitors()
    {
        s_monitorRects.clear();
        EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
    }

    BOOL CALLBACK MainWindow::MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
    {
        s_monitorRects.push_back(*lprcMonitor);
        return TRUE;
    }

    void winrt::Winvert4::implementation::MainWindow::ReleaseScreenBitmap()
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

    void winrt::Winvert4::implementation::MainWindow::OnSelectionCompleted(RECT sel)
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
        EffectSettings settings{}; /* defaults: colorMat=I, colorOffset=0, isCustomEffectActive=false already */
        if (m_pendingEffect == PendingEffect::Invert)
        {
            settings.isInvertEffectEnabled = true;
        }
        
        else if (m_pendingEffect == PendingEffect::Filter)
        {
            int fav = FavoriteFilterIndex();
            if (fav >= 0 && fav < static_cast<int>(m_savedFilters.size()))
            {
                auto& sf = m_savedFilters[fav];
                settings.isCustomEffectActive = true;
                memcpy(settings.colorMat, sf.mat, sizeof(sf.mat));
                memcpy(settings.colorOffset, sf.offset, sizeof(sf.offset));
            }
        }
        settings.showFpsOverlay = m_showFpsOverlay;
        settings.brightnessProtectionDelayFrames = m_brightnessDelayFrames;
        memcpy(settings.lumaWeights, m_lumaWeights, sizeof(settings.lumaWeights));
        m_windowSettings.push_back(settings);
        m_pendingEffect = PendingEffect::None; // Reset for next time

        // 3) Create one or more effect windows depending on monitor intersections
        // Track if a favorite filter was applied so the flyout reflects it
        int appliedFavIndex = -1;
        if (settings.isCustomEffectActive)
        {
            // If we just applied a favorite filter via hotkey, remember which one
            int fav = FavoriteFilterIndex();
            if (fav >= 0 && fav < static_cast<int>(m_savedFilters.size()))
            {
                appliedFavIndex = fav;
            }
        }
        std::vector<RECT> splits;
        if (m_outputManager) m_outputManager->GetIntersectingRects(sel, splits);
        if (splits.empty()) splits.push_back(sel);

        // Primary window is the first split; extras are any additional splits
        auto newWindow = std::make_unique<EffectWindow>(splits[0], m_outputManager.get());
        newWindow->UpdateSettings(settings);
        winvert4::Log("MainWindow: calling primary EffectWindow::Show()");
        newWindow->Show();
        winvert4::Log("MainWindow: primary EffectWindow::Show() returned");

        // 4) Add a tab for the new window
        auto newTab = TabViewItem();
        newTab.Header(winrt::box_value(L"Region " + std::to_wstring(m_effectWindows.size() + 1)));
        RegionsTabView().TabItems().Append(newTab);
        RegionsTabView().SelectedItem(newTab);
        // If a favorite filter was applied when creating this region, mirror that
        // in the Filters flyout selection state for this new tab so the UI matches.
        if (appliedFavIndex >= 0)
        {
            int newIdx = SelectedTabIndex();
            if (newIdx >= 0)
            {
                if (static_cast<int>(m_tabFilterSelections.size()) <= newIdx)
                    m_tabFilterSelections.resize(newIdx + 1);
                m_tabFilterSelections[newIdx].assign(m_savedFilters.size(), false);
                if (appliedFavIndex < static_cast<int>(m_tabFilterSelections[newIdx].size()))
                {
                    m_tabFilterSelections[newIdx][appliedFavIndex] = true;
                }
                UpdateFilterDropdown();
            }
        }

        // Create extra windows for additional monitors
        std::vector<std::unique_ptr<EffectWindow>> extras;
        for (size_t i = 1; i < splits.size(); ++i)
        {
            auto w = std::make_unique<EffectWindow>(splits[i], m_outputManager.get());
            w->UpdateSettings(settings);
            winvert4::Logf("MainWindow: calling extra EffectWindow::Show() #%zu", i);
            w->Show();
            winvert4::Logf("MainWindow: extra EffectWindow::Show() #%zu returned", i);
            extras.push_back(std::move(w));
        }

        m_effectWindows.push_back(std::move(newWindow));
        m_effectWindowExtras.push_back(std::move(extras));

        // Reveal the control panel window after first selection is created
        if (!m_controlPanelShownYet)
        {
            ::ShowWindow(m_mainHwnd, SW_SHOW);
            winvert4::Log("MainWindow: ShowWindow(SW_SHOW) control panel");
            m_controlPanelShownYet = true;
            UpdateUIState();
        
            SetWindowSize(360, 120);
        }
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
            // If in color sampling mode, sample immediately on click
            if (self->m_isSamplingColor)
            {
                POINTS pts = MAKEPOINTS(lParam);
                POINT pt{ pts.x, pts.y };
                self->OnColorSampled(pt);
                self->m_isSelecting = false;
                self->m_isSamplingColor = false;
                DestroyWindow(hwnd);
                return 0;
            }
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
                    if (self->m_isSamplingColor)
                    {
                        self->m_isSamplingColor = false;
                        if (auto btn = self->ColorMapSampleButton()) { btn.Content(box_value(L"Sample")); btn.IsEnabled(true); }
                    }
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
                COLORREF drawClr = self->m_useCustomSelectionColor ? self->m_selectionColor : RGB(255,0,0);
                Gdiplus::Color penColor(255, GetRValue(drawClr), GetGValue(drawClr), GetBValue(drawClr));
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

    void winrt::Winvert4::implementation::MainWindow::UpdateUIState()
    {
        if (m_isClosing) return;
        bool hasWindows = RegionsTabView().TabItems().Size() > 0;
        int idx = SelectedTabIndex();
        EffectSettings current{};
        if (idx >= 0 && idx < static_cast<int>(m_windowSettings.size()))
        {
            current = m_windowSettings[idx];
        }
        if (auto img = BrightnessProtectionImage()) img.Source(current.isBrightnessProtectionEnabled ? m_brightnessOnIconSource : m_brightnessOffIconSource);
        if (auto img = InvertEffectImage()) img.Source(current.isInvertEffectEnabled ? m_invertOnIconSource : m_invertOffIconSource);
        bool curHidden = (idx >= 0 && idx < static_cast<int>(m_windowHidden.size())) ? m_windowHidden[idx] : false;
        if (auto img = HideAllWindowsImage()) img.Source(curHidden ? m_showIconSource : m_hideIconSource);
        if (auto img = ColorMappingToggleImage()) img.Source(current.isColorMappingEnabled ? m_colorMappingOnIconSource : m_colorMappingOffIconSource);

        if (auto btn = HideAllWindowsButton()) btn.IsEnabled(hasWindows);
        if (auto btn = ColorMappingToggleButton()) btn.IsEnabled(hasWindows);
        if (auto btn = BrightnessProtectionButton()) btn.IsEnabled(hasWindows);
        if (auto btn = InvertEffectButton()) btn.IsEnabled(hasWindows);
        if (auto btn = FiltersDropDownButton()) btn.IsEnabled(hasWindows);

        bool showEmptyPrompt = (!hasWindows) && m_hasEverHadWindows && m_lastRemovalViaUI;
        if (auto bar = InfoBar()) bar.IsOpen(showEmptyPrompt);

        if (SettingsPanel().Visibility() != Visibility::Visible)
        {
            SetWindowSize(360, showEmptyPrompt ? 240 : 120);
        }

        // Sync Color Map preserve toggle with current settings if present
        if (auto root = this->Content().try_as<FrameworkElement>())
        {
            auto ts = root.FindName(L"ColorMapPreserveToggle").try_as<Controls::ToggleSwitch>();
            if (ts) ts.IsOn(current.colorMapPreserveBrightness);
        }
    }

    void winrt::Winvert4::implementation::MainWindow::SetWindowSize(int width, int height)
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

    void winrt::Winvert4::implementation::MainWindow::UpdateAllHotkeyText()
    {
        if (auto tb = InvertHotkeyTextBox()) UpdateHotkeyText(tb, m_hotkeyInvertMod, m_hotkeyInvertVk);
        if (auto tb = FilterHotkeyTextBox()) UpdateHotkeyText(tb, m_hotkeyFilterMod, m_hotkeyFilterVk);
        if (auto tb = RemoveHotkeyTextBox()) UpdateHotkeyText(tb, m_hotkeyRemoveMod, m_hotkeyRemoveVk);
    }

    void winrt::Winvert4::implementation::MainWindow::UpdateHotkeyText(TextBox const& textBox, UINT mod, UINT vk)
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

    int winrt::Winvert4::implementation::MainWindow::SelectedTabIndex()
    {
        return static_cast<int>(RegionsTabView().SelectedIndex());
    }

    void winrt::Winvert4::implementation::MainWindow::ApplyGlobalColorMapsToSettings(EffectSettings& settings)
    {
        if (settings.isColorMappingEnabled) settings.colorMaps = m_globalColorMaps;
        else settings.colorMaps.clear();
    }

    void winrt::Winvert4::implementation::MainWindow::UpdateSettingsForGroup(int idx)
    {
        if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;
        if (idx < static_cast<int>(m_effectWindows.size()))
        {
            if (auto& wnd = m_effectWindows[idx]) wnd->UpdateSettings(m_windowSettings[idx]);
        }
        if (idx < static_cast<int>(m_effectWindowExtras.size()))
        {
            for (auto& ew : m_effectWindowExtras[idx])
            {
                if (ew) ew->UpdateSettings(m_windowSettings[idx]);
            }
        }
    }

    void winrt::Winvert4::implementation::MainWindow::UpdateSettingsForGroup(int idx, const EffectSettings& settings)
    {
        if (idx < 0) return;
        if (idx < static_cast<int>(m_effectWindows.size()))
        {
            if (auto& wnd = m_effectWindows[idx]) wnd->UpdateSettings(settings);
        }
        if (idx < static_cast<int>(m_effectWindowExtras.size()))
        {
            for (auto& ew : m_effectWindowExtras[idx])
            {
                if (ew) ew->UpdateSettings(settings);
            }
        }
    }

    void winrt::Winvert4::implementation::MainWindow::SetHiddenForGroup(int idx, bool hidden)
    {
        if (idx < 0) return;
        if (idx < static_cast<int>(m_effectWindows.size()))
        {
            if (auto& wnd = m_effectWindows[idx]) wnd->SetHidden(hidden);
        }
        if (idx < static_cast<int>(m_effectWindowExtras.size()))
        {
            for (auto& ew : m_effectWindowExtras[idx])
            {
                if (ew) ew->SetHidden(hidden);
            }
        }
    }

    void winrt::Winvert4::implementation::MainWindow::SetHiddenForAll(bool hidden)
    {
        for (int i = 0; i < static_cast<int>(m_effectWindows.size()); ++i)
        {
            SetHiddenForGroup(i, hidden);
        }
    }

    void winrt::Winvert4::implementation::MainWindow::StartColorSample()
    {
        if (m_isSelecting) return;
        m_isSamplingColor = true;
        if (auto btn = ColorMapSampleButton()) { btn.Content(box_value(L"Sampling...")); btn.IsEnabled(false); }
        // Install a low-level mouse hook to capture the next click and swallow it
        s_samplingInstance = this;
        s_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &MainWindow::LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
        // Install a low-level keyboard hook to allow ESC cancellation
        s_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, &MainWindow::LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
        // Show overlay following the cursor
        ShowSampleOverlay();
        POINT pt{}; GetCursorPos(&pt); MoveSampleOverlay(pt);
    }

    void winrt::Winvert4::implementation::MainWindow::CancelColorSample()
    {
        m_isSamplingColor = false;
        HideSampleOverlay();
        if (auto btn = ColorMapSampleButton()) { btn.Content(box_value(L"Sample")); btn.IsEnabled(true); }
        if (MainWindow::s_mouseHook)
        {
            UnhookWindowsHookEx(MainWindow::s_mouseHook);
            MainWindow::s_mouseHook = nullptr;
        }
        if (MainWindow::s_keyboardHook)
        {
            UnhookWindowsHookEx(MainWindow::s_keyboardHook);
            MainWindow::s_keyboardHook = nullptr;
        }
        MainWindow::s_samplingInstance = nullptr;
    }

    void winrt::Winvert4::implementation::MainWindow::OnColorSampled(POINT ptScreen)
    {
        HDC hdc = GetDC(nullptr);
        if (!hdc)
        {
            m_isSamplingColor = false;
            if (auto btn = ColorMapSampleButton()) { btn.Content(box_value(L"Sample")); btn.IsEnabled(true); }
            return;
        }
        COLORREF cr = GetPixel(hdc, ptScreen.x, ptScreen.y);
        ReleaseDC(nullptr, hdc);
        if (cr == CLR_INVALID)
        {
            m_isSamplingColor = false;
            if (auto btn = ColorMapSampleButton()) { btn.Content(box_value(L"Sample")); btn.IsEnabled(true); }
            return;
        }
        ColorMapEntry e{};
        e.srcR = GetRValue(cr); e.srcG = GetGValue(cr); e.srcB = GetBValue(cr);
        // Destination from current picker if present
        if (auto root = this->Content().try_as<FrameworkElement>())
        {
            if (auto picker = root.FindName(L"ColorMapPicker").try_as<Controls::ColorPicker>())
            {
                auto c = picker.Color();
                e.dstR = c.R; e.dstG = c.G; e.dstB = c.B;
            }
        }
        m_globalColorMaps.push_back(e);
        RefreshColorMapList();
        
        int idxSel = SelectedTabIndex();
        if (idxSel >= 0 && idxSel < static_cast<int>(m_windowSettings.size()))
        {
            if (m_windowSettings[idxSel].isColorMappingEnabled)
            {
                ApplyGlobalColorMapsToSettings(m_windowSettings[idxSel]);
                UpdateSettingsForGroup(idxSel);
            }
        }
        m_isSamplingColor = false;
        if (auto btn = ColorMapSampleButton()) { btn.Content(box_value(L"Sample")); btn.IsEnabled(true); }
        HideSampleOverlay();
    }

    // --- Sampling overlay (magnifier) implementation ---
    LRESULT CALLBACK winrt::Winvert4::implementation::MainWindow::SampleOverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_NCHITTEST:
            // Treat as a normal client area so this window owns hit-tests.
            // Low-level hook still consumes clicks, but cursor comes from us.
            return HTCLIENT;
        case WM_SETCURSOR:
            // Force default arrow cursor regardless of underlying app
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    void winrt::Winvert4::implementation::MainWindow::ShowSampleOverlay()
    {
        if (m_sampleOverlayHwnd) return;
        WNDCLASSW wc{}; wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &MainWindow::SampleOverlayWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Winvert4_SampleOverlay";
        static ATOM atom = 0; if (!atom) atom = RegisterClassW(&wc);
        int sz = m_sampleOverlaySize;
        m_sampleOverlayHwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            wc.lpszClassName, L"", WS_POPUP,
            0, 0, sz, sz, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (m_sampleOverlayHwnd)
        {
            HRGN rgn = CreateEllipticRgn(0, 0, sz, sz);
            SetWindowRgn(m_sampleOverlayHwnd, rgn, FALSE);
            // Opaque layered window, excluded from SRCCOPY captures
            SetLayeredWindowAttributes(m_sampleOverlayHwnd, 0, 255, LWA_ALPHA);
            ShowWindow(m_sampleOverlayHwnd, SW_SHOWNOACTIVATE);
        }
        // Init perf timer
        if (m_qpcFreqSample.QuadPart == 0) QueryPerformanceFrequency(&m_qpcFreqSample);
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&m_lastOverlayDrawQpc));
    }

    void winrt::Winvert4::implementation::MainWindow::MoveSampleOverlay(POINT ptScreen)
    {
        if (!m_sampleOverlayHwnd) return;
        int sz = m_sampleOverlaySize;
        // Center window at cursor with small offset so cursor is near center
        int x = ptScreen.x - sz / 2;
        int y = ptScreen.y - sz / 2;
        SetWindowPos(m_sampleOverlayHwnd, HWND_TOPMOST, x, y, sz, sz, SWP_NOACTIVATE | SWP_NOSIZE | SWP_SHOWWINDOW);

        // Throttle updates to reduce overhead
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        const double elapsedMs = (m_qpcFreqSample.QuadPart > 0)
            ? (1000.0 * (now.QuadPart - (LONGLONG)m_lastOverlayDrawQpc) / (double)m_qpcFreqSample.QuadPart)
            : 1000.0;
        if (elapsedMs < 5.0) return; // cap to ~200 FPS
        m_lastOverlayDrawQpc = (unsigned long long)now.QuadPart;

        HDC scrDC = GetDC(nullptr);
        int zoom = (m_sampleOverlayZoom > 0) ? m_sampleOverlayZoom : 4;
        int srcW = (std::max)(1, sz / zoom); int srcH = (std::max)(1, sz / zoom);
        int sx = ptScreen.x - srcW / 2; int sy = ptScreen.y - srcH / 2;

        // Ensure memory surface allocated once (realloc on size change)
        if (!m_sampleMemDC) m_sampleMemDC = CreateCompatibleDC(scrDC);
        if (!m_sampleMemBmp || m_sampleMemW != srcW || m_sampleMemH != srcH)
        {
            if (m_sampleMemBmp) { DeleteObject(m_sampleMemBmp); m_sampleMemBmp = nullptr; }
            m_sampleMemBmp = CreateCompatibleBitmap(scrDC, srcW, srcH);
            m_sampleMemW = srcW; m_sampleMemH = srcH;
        }
        HGDIOBJ oldBmp = SelectObject(m_sampleMemDC, m_sampleMemBmp);
        // Temporarily hide the overlay to avoid self-capture, then use CAPTUREBLT to get composed desktop
        ShowWindow(m_sampleOverlayHwnd, SW_HIDE);
        BitBlt(m_sampleMemDC, 0, 0, srcW, srcH, scrDC, sx, sy, SRCCOPY | 0x40000000 /*CAPTUREBLT*/);
        ShowWindow(m_sampleOverlayHwnd, SW_SHOWNOACTIVATE);

        // Paint into the overlay using fast mode while moving
        HDC wndDC = GetDC(m_sampleOverlayHwnd);
        // Use COLORONCOLOR when motion is recent; HALFTONE if slowed down (>70ms)
        SetStretchBltMode(wndDC, elapsedMs > 70.0 ? HALFTONE : COLORONCOLOR);
        // Clear background
        HBRUSH hbr = CreateSolidBrush(RGB(0,0,0)); RECT rc{0,0,sz,sz}; FillRect(wndDC, &rc, hbr); DeleteObject(hbr);
        StretchBlt(wndDC, 0, 0, sz, sz, m_sampleMemDC, 0, 0, srcW, srcH, SRCCOPY);

        // Border color from pixel under cursor
        COLORREF cr = GetPixel(scrDC, ptScreen.x, ptScreen.y);
        HPEN pen = CreatePen(PS_SOLID, 2, cr == CLR_INVALID ? RGB(255,255,255) : cr);
        HPEN oldPen = (HPEN)SelectObject(wndDC, pen);
        HBRUSH nullb = (HBRUSH)GetStockObject(HOLLOW_BRUSH); HBRUSH oldb = (HBRUSH)SelectObject(wndDC, nullb);
        Ellipse(wndDC, 1, 1, sz-1, sz-1);
        SelectObject(wndDC, oldPen); SelectObject(wndDC, oldb); DeleteObject(pen);

        // Cleanup
        SelectObject(m_sampleMemDC, oldBmp);
        ReleaseDC(m_sampleOverlayHwnd, wndDC);
        ReleaseDC(nullptr, scrDC);
    }

    void winrt::Winvert4::implementation::MainWindow::HideSampleOverlay()
    {
        if (m_sampleOverlayHwnd)
        {
            DestroyWindow(m_sampleOverlayHwnd);
            m_sampleOverlayHwnd = nullptr;
        }
        if (m_sampleMemBmp) { DeleteObject(m_sampleMemBmp); m_sampleMemBmp = nullptr; }
        if (m_sampleMemDC)  { DeleteDC(m_sampleMemDC); m_sampleMemDC = nullptr; }
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

    void winrt::Winvert4::implementation::MainWindow::FiltersMenuFlyout_Closing(IInspectable const&, Controls::Primitives::FlyoutBaseClosingEventArgs const& e)
    {
        if (m_keepFiltersFlyoutOpenNext)
        {
            e.Cancel(true);
            m_keepFiltersFlyoutOpenNext = false;
        }
    }

    void winrt::Winvert4::implementation::MainWindow::InfoBar_Closed(IInspectable const&, Microsoft::UI::Xaml::Controls::InfoBarClosedEventArgs const&)
    {
        if (SettingsPanel().Visibility() != Visibility::Visible)
        {
            SetWindowSize(360, 120);
        }
    }

    void winrt::Winvert4::implementation::MainWindow::RegionsTabView_AddTabButtonClick(IInspectable const&, IInspectable const&)
    {
        StartScreenSelection();
    }

    void winrt::Winvert4::implementation::MainWindow::RegionsTabView_TabCloseRequested(TabView const& sender, TabViewTabCloseRequestedEventArgs const& args)
    {
        m_lastRemovalViaUI = true;
        m_lastRemovalInitiatedByHotkey = false;
        auto items = sender.TabItems();
        uint32_t index;
        if (items.IndexOf(args.Item(), index))
        {
            int idx = static_cast<int>(index);

            // Release effect window resources and remove state
            if (idx < static_cast<int>(m_effectWindows.size()))
            {
                if (auto& wnd = m_effectWindows[idx]) { wnd->Hide(); wnd.reset(); }
                m_effectWindows.erase(m_effectWindows.begin() + idx);
            }
            if (idx < static_cast<int>(m_windowSettings.size()))
                m_windowSettings.erase(m_windowSettings.begin() + idx);
            if (idx < static_cast<int>(m_windowHidden.size()))
                m_windowHidden.erase(m_windowHidden.begin() + idx);
            if (idx < static_cast<int>(m_hasPreviewBackup.size()))
                m_hasPreviewBackup.erase(m_hasPreviewBackup.begin() + idx);
            if (idx < static_cast<int>(m_previewBackup.size()))
                m_previewBackup.erase(m_previewBackup.begin() + idx);

            items.RemoveAt(index);
            // Renumber headers
            for (uint32_t i = 0; i < items.Size(); ++i)
            {
                if (auto tab = items.GetAt(i).try_as<TabViewItem>())
                {
                    tab.Header(box_value(L"Region " + std::to_wstring(i + 1)));
                }
            }
            // Adjust selection
            if (items.Size() > 0)
            {
                RegionsTabView().SelectedIndex(static_cast<int>(std::min<uint32_t>(items.Size() - 1, index)));
            }
            UpdateUIState();
        }
    }

    void winrt::Winvert4::implementation::MainWindow::RegionsTabView_SelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        UpdateUIState();
        RefreshColorMapList();
        
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
                    pThis->RebindFilterHotkeyButton().IsEnabled(true);
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
                    case RebindingState::Filter:    pThis->m_hotkeyFilterMod = newMod; pThis->m_hotkeyFilterVk = static_cast<UINT>(wParam); break;
                    case RebindingState::Remove:    pThis->m_hotkeyRemoveMod = newMod; pThis->m_hotkeyRemoveVk = static_cast<UINT>(wParam); break;
                    }
                    pThis->m_rebindingState = RebindingState::None;
                    pThis->RegisterAllHotkeys();
                    pThis->RebindStatusText().Text(L"Hotkey updated!");
                    pThis->RebindInvertHotkeyButton().IsEnabled(true);
                    pThis->RebindFilterHotkeyButton().IsEnabled(true);
                    pThis->RebindRemoveHotkeyButton().IsEnabled(true);
                    pThis->SaveAppState();
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
                else if (wParam == HOTKEY_FILTER_ID) {
                pThis->OnFilterHotkeyPressed();
                }
                else if (wParam == HOTKEY_REMOVE_ID) {
                pThis->OnRemoveHotkeyPressed();
                }
            break;
        }
        case WM_CLOSE:
            // Save state and teardown
            pThis->SaveAppState();
            pThis->m_isClosing = true;
            break;
        case WM_DESTROY:
            pThis->m_isClosing = true;
            PostQuitMessage(0);
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

void winrt::Winvert4::implementation::MainWindow::AddNewFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    // Show the editor and seed identity matrix
    if (auto panel = FilterEditorPanel()) panel.Visibility(Visibility::Visible);
    if (auto exp = CustomFiltersExpander()) exp.IsExpanded(true);

    // Initialize 5x5 TextBoxes if not present
    auto grid = FilterMatrixGrid();
    if (grid && grid.Children().Size() == 0)
    {
        for (int r = 0; r < 5; ++r)
        {
            for (int c = 0; c < 5; ++c)
            {
                auto tb = Microsoft::UI::Xaml::Controls::TextBox();
                tb.TextAlignment(Microsoft::UI::Xaml::TextAlignment::Center);
                tb.Width(60); tb.Height(32);
                Microsoft::UI::Xaml::Controls::Grid::SetRow(tb, r);
                Microsoft::UI::Xaml::Controls::Grid::SetColumn(tb, c);
                grid.Children().Append(tb);
            }
        }
    }

    // Seed identity 5x5: diag 1s for RGBA, last column offsets 0, last row 0,0,0,0,1
        if (grid)
        {
            for (auto child : grid.Children())
            {
            auto tb = child.try_as<Microsoft::UI::Xaml::Controls::TextBox>();
            if (!tb) continue;
            int r = Microsoft::UI::Xaml::Controls::Grid::GetRow(tb);
            int c = Microsoft::UI::Xaml::Controls::Grid::GetColumn(tb);
            double v = 0.0;
            if (r < 4 && c < 4 && r == c) v = 1.0; // 1 on main diag
            if (r == 4 && c == 4) v = 1.0;         // constant row
            wchar_t buf[32]; swprintf_s(buf, L"%.3f", v);
            tb.Text(buf);
        }
        // Seed default proposed name by selecting matching item if present (editable text not assumed)
        if (auto combo = SavedFiltersComboBox())
        {
            std::wstring base = L"Filter ";
            base += std::to_wstring(m_savedFilters.size() + 1);
            // If an item with this name exists, select it; otherwise leave selection unchanged
            for (int i = 0; i < static_cast<int>(m_savedFilters.size()); ++i)
            {
                if (m_savedFilters[i].name == base) { combo.SelectedIndex(i); break; }
            }
        }
    }
}

    void winrt::Winvert4::implementation::MainWindow::SaveFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
    // Read 5x5 grid -> map to 4x4+offset used by shader
    auto grid = FilterMatrixGrid();
    if (!grid) return;

    float mat4[16] = { 0 };
    float offset[4] = { 0 };
    // initialize mat as identity, offset 0
    for (int i = 0; i < 4; ++i) mat4[i * 4 + i] = 1.0f;

    for (auto child : grid.Children())
    {
        auto tb = child.try_as<Microsoft::UI::Xaml::Controls::TextBox>();
        if (!tb) continue;
        int r = Microsoft::UI::Xaml::Controls::Grid::GetRow(tb);
        int c = Microsoft::UI::Xaml::Controls::Grid::GetColumn(tb);
        std::wstring s = std::wstring(tb.Text());
        try {
            float v = std::stof(s);
            if (r >= 0 && r < 4 && c >= 0 && c < 4)
            {
                mat4[r * 4 + c] = v; // 4x4 coefficients
            }
            else if (r == 4 && c >= 0 && c < 4)
            {
                offset[c] = v; // last row is offset for each channel
            }
            // row 4 (constant row) ignored; we keep it as 0,0,0,0,1 visually
        } catch (...) { /* ignore parse errors */ }
    }

    // Save or update saved filter (no auto-apply)
    std::wstring name;
    int selIndex = -1;
    Controls::ComboBox combo{ nullptr };
    if ((combo = SavedFiltersComboBox())) selIndex = combo.SelectedIndex();
    // Prefer user-typed text if present (editable ComboBox)
    if (combo)
    {
        try {
            std::wstring typed = std::wstring(combo.Text());
            // Trim whitespace
            auto ltrim = [](std::wstring& s){ s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](wchar_t ch){ return !iswspace(ch); })); };
            auto rtrim = [](std::wstring& s){ s.erase(std::find_if(s.rbegin(), s.rend(), [](wchar_t ch){ return !iswspace(ch); }).base(), s.end()); };
            ltrim(typed); rtrim(typed);
            if (!typed.empty()) name = typed;
        } catch (...) {}
    }
    // If no typed text, fall back to selected item's name
    if (name.empty() && selIndex >= 0 && selIndex < static_cast<int>(m_savedFilters.size()))
    {
        name = m_savedFilters[selIndex].name;
    }
    // If still empty, generate a new unique name so we don't overwrite defaults
    if (name.empty())
    {
        std::wstring base = L"Custom ";
        int n = 1;
        while (true)
        {
            std::wstring candidate = base + std::to_wstring(n);
            bool exists = false;
            for (auto& f : m_savedFilters) { if (f.name == candidate) { exists = true; break; } }
            if (!exists) { name = candidate; break; }
            ++n;
        }
    }

    
    // If name matches a built-in preset, append a copy suffix to make it distinct
    {
        bool conflictsBuiltin = false;
        for (auto& f : m_savedFilters) { if (f.name == name && f.isBuiltin) { conflictsBuiltin = true; break; } }
        if (conflictsBuiltin)
        {
            int suffix = 0;
            while (true)
            {
                std::wstring candidate = name;
                candidate.push_back(L' ');
                candidate.push_back(L'(');
                candidate += L"copy";
                if (suffix > 0)
                {
                    candidate.push_back(L' ');
                    candidate += std::to_wstring(suffix + 1);
                }
                candidate.push_back(L')');
                bool exists = false;
                for (auto& f : m_savedFilters) { if (f.name == candidate) { exists = true; break; } }
                if (!exists) { name = candidate; break; }
                ++suffix;
            }
        }
    }

    int existing = -1;
    for (int i = 0; i < static_cast<int>(m_savedFilters.size()); ++i)
        if (m_savedFilters[i].name == name && !m_savedFilters[i].isBuiltin) { existing = i; break; }

    SavedFilter sf{}; sf.name = name; sf.isBuiltin = false;
    memcpy(sf.mat, mat4, sizeof(mat4));
    memcpy(sf.offset, offset, sizeof(offset));

    if (existing >= 0) m_savedFilters[existing] = sf;
    else m_savedFilters.push_back(sf);

    UpdateSavedFiltersCombo();
    UpdateFilterDropdown();
    SaveAppState();
    if (auto combo2 = SavedFiltersComboBox())
    {
        combo2.Text(winrt::hstring{name});
        for (int i = 0; i < static_cast<int>(m_savedFilters.size()); ++i)
            if (m_savedFilters[i].name == name) { combo2.SelectedIndex(i); break; }
    }
    
    // Toggle buttons: hide Save, show Apply; enable Delete for custom filters
    if (auto root = this->Content().try_as<FrameworkElement>())
    {
        if (auto btn = root.FindName(L"DeleteFilterButton").try_as<Controls::Button>()) btn.IsEnabled(true);
    }
}

void winrt::Winvert4::implementation::MainWindow::DeleteFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    // Delete selected saved filter
    std::wstring name;
    if (auto combo = SavedFiltersComboBox())
    {
        if (combo.SelectedIndex() >= 0)
        {
            auto hs = unbox_value<winrt::hstring>(combo.SelectedItem());
            name = std::wstring(hs);
        }
    }
    if (name.empty()) return;
    // Do not delete built-in filters
    m_savedFilters.erase(std::remove_if(m_savedFilters.begin(), m_savedFilters.end(), [&](const SavedFilter& f){return f.name==name && !f.isBuiltin;}), m_savedFilters.end());
    UpdateSavedFiltersCombo();
    UpdateFilterDropdown();
    SaveAppState();
    // After delete, disable the delete button
    if (auto root = this->Content().try_as<FrameworkElement>())
    {
        if (auto btn = root.FindName(L"DeleteFilterButton").try_as<Controls::Button>()) btn.IsEnabled(false);
    }
    
}

void winrt::Winvert4::implementation::MainWindow::ClearFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    // Reset grid to identity and offsets to 0, keep editor visible
    auto grid = FilterMatrixGrid();
    if (!grid) return;
    for (auto child : grid.Children())
    {
        auto tb = child.try_as<Microsoft::UI::Xaml::Controls::TextBox>();
        if (!tb) continue;
        int r = Microsoft::UI::Xaml::Controls::Grid::GetRow(tb);
        int c = Microsoft::UI::Xaml::Controls::Grid::GetColumn(tb);
        double v = 0.0;
        if (r < 4 && c < 4 && r == c) v = 1.0;
        if (r == 4 && c == 4) v = 1.0;
        wchar_t buf[32]; swprintf_s(buf, L"%.3f", v);
        tb.Text(buf);
    }
    // Also reset simple sliders to defaults to keep modes in sync
    m_simpleBrightness = 0.0f;
    m_simpleContrast = 1.0f;
    m_simpleSaturation = 1.0f;
    m_simpleTemperature = 0.0f;
    m_simpleTint = 0.0f;
        if (auto s = BrightnessSlider()) s.Value(m_simpleBrightness);
        if (auto s = ContrastSlider()) s.Value(m_simpleContrast);
        if (auto s = SaturationSlider()) s.Value(m_simpleSaturation);
        if (auto s = HueSlider())        s.Value(m_simpleHueAngle);
        if (auto s = TemperatureSlider()) s.Value(m_simpleTemperature);
        if (auto s = TintSlider()) s.Value(m_simpleTint);
}

void winrt::Winvert4::implementation::MainWindow::SavedFiltersComboBox_SelectionChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    // Load selected saved filter into the grid for editing (no auto-apply)
    auto combo = SavedFiltersComboBox();
    if (!combo) return;
    int sel = combo.SelectedIndex();
    if (sel < 0 || sel >= static_cast<int>(m_savedFilters.size())) return;

    auto& sf = m_savedFilters[sel];
    // Toggle Delete enablement based on built-in status
    if (auto root = this->Content().try_as<FrameworkElement>())
    {
        if (auto btn = root.FindName(L"DeleteFilterButton").try_as<Controls::Button>()) btn.IsEnabled(!sf.isBuiltin);
    }
    auto grid = FilterMatrixGrid();
    if (!grid) return;
    EnsureFilterMatrixGridInitialized();

    // Ensure editor visible
    if (auto panel = FilterEditorPanel()) panel.Visibility(Visibility::Visible);
    if (auto exp = CustomFiltersExpander()) exp.IsExpanded(true);
    // Ensure the editor is tied to selected filter; editable combo will show selection automatically

    // Write values into 5x5 grid
    for (auto child : grid.Children())
    {
        auto tb = child.try_as<TextBox>();
        if (!tb) continue;
        int r = Microsoft::UI::Xaml::Controls::Grid::GetRow(tb);
        int c = Microsoft::UI::Xaml::Controls::Grid::GetColumn(tb);
        double v = 0.0;
        if (r < 4 && c < 4) v = sf.mat[r * 4 + c];
        else if (r == 4 && c < 4) v = sf.offset[c];
        else if (r == 4 && c == 4) v = 1.0; // const row
        wchar_t buf[32]; swprintf_s(buf, L"%.3f", v);
        tb.Text(buf);
    }
    // Also push corresponding slider values so modes correlate
    UpdateSlidersFromMatrix(sf.mat, sf.offset);
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
            // Clear selection flags for this tab
            if (static_cast<int>(m_tabFilterSelections.size()) <= idx)
                m_tabFilterSelections.resize(idx + 1);
            m_tabFilterSelections[idx].assign(m_savedFilters.size(), false);

            // Reset to identity
            m_windowSettings[idx].isCustomEffectActive = false;
            float I[16] = {1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1};
            float Z[4]  = {0,0,0,0};
            memcpy(m_windowSettings[idx].colorMat, I, sizeof(I));
            memcpy(m_windowSettings[idx].colorOffset, Z, sizeof(Z));
            UpdateSettingsForGroup(idx);
        }
        m_keepFiltersFlyoutOpenNext = false; // allow close
        UpdateFilterDropdown();
        return;
    }
    // Toggle items are handled separately
    return;
}

// Build flyout items from saved filters
void winrt::Winvert4::implementation::MainWindow::UpdateFilterDropdown()
{
    auto flyout = FiltersMenuFlyout();
    if (!flyout) return;
    flyout.Items().Clear();

    auto noneItem = MenuFlyoutItem();
    noneItem.Text(L"None");
    noneItem.Tag(box_value(-1));
    noneItem.Click({ this, &MainWindow::FilterMenuItem_Click });
    flyout.Items().Append(noneItem);

    int idx = SelectedTabIndex();
    bool haveSel = (idx >= 0 && idx < static_cast<int>(m_tabFilterSelections.size()) && static_cast<int>(m_tabFilterSelections[idx].size()) == static_cast<int>(m_savedFilters.size()));
    for (int i = 0; i < static_cast<int>(m_savedFilters.size()); ++i)
    {
        auto t = ToggleMenuFlyoutItem();
        t.Text(m_savedFilters[i].name);
        t.Tag(box_value(i));
        if (haveSel) t.IsChecked(m_tabFilterSelections[idx][i]);
        t.Click({ this, &MainWindow::FilterToggleMenuItem_Click });
        flyout.Items().Append(t);
    }

}
// ToggleMenuFlyoutItem handler: recompute composite from all checked items
void winrt::Winvert4::implementation::MainWindow::FilterToggleMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    auto item = sender.as<ToggleMenuFlyoutItem>();
    if (!item) return;
    int tagIndex = -1;
    if (auto tag = item.Tag()) tagIndex = unbox_value<int>(tag);
    if (tagIndex < 0 || tagIndex >= static_cast<int>(m_savedFilters.size())) return;

    int idx = SelectedTabIndex();
    if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;

    if (static_cast<int>(m_tabFilterSelections.size()) <= idx)
        m_tabFilterSelections.resize(idx + 1);
    if (static_cast<int>(m_tabFilterSelections[idx].size()) != static_cast<int>(m_savedFilters.size()))
        m_tabFilterSelections[idx].assign(m_savedFilters.size(), false);

    m_tabFilterSelections[idx][tagIndex] = item.IsChecked();

    ApplyCompositeCustomFiltersForTab(idx);

    // Keep open for multi-selection
    m_keepFiltersFlyoutOpenNext = true;
    UpdateFilterDropdown();
}

void winrt::Winvert4::implementation::MainWindow::ApplyCompositeCustomFiltersForTab(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;
    // Identity
    float M[16] = {1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1};
    float O[4]  = {0,0,0,0};
    bool any = false;
    if (static_cast<int>(m_tabFilterSelections.size()) > idx && static_cast<int>(m_tabFilterSelections[idx].size()) == static_cast<int>(m_savedFilters.size()))
    {
        for (int i = 0; i < static_cast<int>(m_savedFilters.size()); ++i)
        {
            if (!m_tabFilterSelections[idx][i]) continue;
            any = true;
            auto const& sf = m_savedFilters[i];
            // M' = Ms * M
            float R[16];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    R[r*4+c] = sf.mat[r*4+0]*M[0*4+c] + sf.mat[r*4+1]*M[1*4+c] + sf.mat[r*4+2]*M[2*4+c] + sf.mat[r*4+3]*M[3*4+c];
            // O' = Ms * O + Os
            float Rv[4];
            for (int r = 0; r < 4; ++r)
                Rv[r] = sf.mat[r*4+0]*O[0] + sf.mat[r*4+1]*O[1] + sf.mat[r*4+2]*O[2] + sf.mat[r*4+3]*O[3];
            for (int r = 0; r < 4; ++r) O[r] = Rv[r] + sf.offset[r];
            memcpy(M, R, sizeof(R));
        }
    }
    m_windowSettings[idx].isCustomEffectActive = any;
    memcpy(m_windowSettings[idx].colorMat, M, sizeof(M));
    memcpy(m_windowSettings[idx].colorOffset, O, sizeof(O));
    UpdateSettingsForGroup(idx);
}

void winrt::Winvert4::implementation::MainWindow::UpdateSavedFiltersCombo()
{
    auto combo = SavedFiltersComboBox();
    Controls::ComboBox favCombo{ nullptr };
    {
        auto root = this->Content().try_as<FrameworkElement>();
        if (root) favCombo = root.FindName(L"FavoriteFilterComboBox").try_as<Controls::ComboBox>();
    }
    if (combo)
    {
        combo.Items().Clear();
        for (auto& sf : m_savedFilters)
        {
        combo.Items().Append(box_value(winrt::hstring{ sf.name }));
        }
    }
    if (favCombo)
    {
        favCombo.Items().Clear();
        for (auto& sf : m_savedFilters)
        {
            favCombo.Items().Append(box_value(winrt::hstring{ sf.name }));
        }
        // Clamp favorite index and apply selection
        if (m_favoriteFilterIndex < 0 || m_favoriteFilterIndex >= static_cast<int>(m_savedFilters.size()))
        {
            m_favoriteFilterIndex = m_savedFilters.empty() ? -1 : 0;
        }
        favCombo.SelectedIndex(m_favoriteFilterIndex);
    }
}

void winrt::Winvert4::implementation::MainWindow::FavoriteFilterComboBox_SelectionChanged(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&)
{
    auto favCombo = sender.try_as<Controls::ComboBox>();
    if (!favCombo) return;
    int sel = favCombo.SelectedIndex();
    if (sel < 0 || sel >= static_cast<int>(m_savedFilters.size())) return;
    m_favoriteFilterIndex = sel;
    SaveAppState();
}

void winrt::Winvert4::implementation::MainWindow::PreviewFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    // Apply current grid as a temporary preview to the selected window
    auto grid = FilterMatrixGrid();
    if (!grid) return;

    float mat4[16] = { 0 };
    float offset[4] = { 0 };
    for (int i = 0; i < 4; ++i) mat4[i * 4 + i] = 1.0f;
    for (auto child : grid.Children())
    {
        auto tb = child.try_as<TextBox>();
        if (!tb) continue;
        int r = Microsoft::UI::Xaml::Controls::Grid::GetRow(tb);
        int c = Microsoft::UI::Xaml::Controls::Grid::GetColumn(tb);
        try {
            float v = std::stof(std::wstring(tb.Text()));
            if (r >= 0 && r < 4 && c >= 0 && c < 4) mat4[r * 4 + c] = v;
            else if (r == 4 && c >= 0 && c < 4) offset[c] = v; // last row holds offsets
        } catch (...) {}
    }

    int idx = SelectedTabIndex();
    if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;

    // Ensure backup vectors sized
    if (static_cast<int>(m_hasPreviewBackup.size()) < static_cast<int>(m_windowSettings.size()))
    {
        m_hasPreviewBackup.resize(m_windowSettings.size(), false);
        m_previewBackup.resize(m_windowSettings.size());
    }

    if (!m_hasPreviewBackup[idx])
    {
        m_previewBackup[idx] = m_windowSettings[idx];
        m_hasPreviewBackup[idx] = true;
    }

    m_windowSettings[idx].isCustomEffectActive = true;
    memcpy(m_windowSettings[idx].colorMat, mat4, sizeof(mat4));
    memcpy(m_windowSettings[idx].colorOffset, offset, sizeof(offset));
    UpdateSettingsForGroup(idx);
    m_isPreviewActive = true;
    // Ensure the target windows are visible during preview (even on settings page)
    SetHiddenForGroup(idx, false);
}

void winrt::Winvert4::implementation::MainWindow::PreviewFilterToggle_Checked(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    // Start preview: backup current settings, disable other effects, apply matrix from grid
    auto grid = FilterMatrixGrid(); if (!grid) return;
    float mat4[16] = { 0 }; float offset[4] = { 0 };
    for (int i = 0; i < 4; ++i) mat4[i * 4 + i] = 1.0f;
    for (auto child : grid.Children())
    {
        auto tb = child.try_as<TextBox>(); if (!tb) continue;
        int r = Microsoft::UI::Xaml::Controls::Grid::GetRow(tb);
        int c = Microsoft::UI::Xaml::Controls::Grid::GetColumn(tb);
        try { float v = std::stof(std::wstring(tb.Text())); if (r < 4 && c < 4) mat4[r*4+c] = v; else if (r < 4 && c == 4) offset[r] = v; } catch (...) {}
    }
    int idx = SelectedTabIndex(); if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;
    if (static_cast<int>(m_hasPreviewBackup.size()) < static_cast<int>(m_windowSettings.size())) { m_hasPreviewBackup.resize(m_windowSettings.size(), false); m_previewBackup.resize(m_windowSettings.size()); }
    if (!m_hasPreviewBackup[idx]) { m_previewBackup[idx] = m_windowSettings[idx]; m_hasPreviewBackup[idx] = true; }
    // Disable other effects during preview
    m_windowSettings[idx].isInvertEffectEnabled = false;
    m_windowSettings[idx].isBrightnessProtectionEnabled = false;
    m_windowSettings[idx].isColorMappingEnabled = false;
    m_windowSettings[idx].isCustomEffectActive = true;
    memcpy(m_windowSettings[idx].colorMat, mat4, sizeof(mat4));
    memcpy(m_windowSettings[idx].colorOffset, offset, sizeof(offset));
    UpdateSettingsForGroup(idx);
    m_isPreviewActive = true;
    SetHiddenForGroup(idx, false);
}

void winrt::Winvert4::implementation::MainWindow::PreviewFilterToggle_Unchecked(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    // End preview: restore backup for this tab if present
    int idx = SelectedTabIndex(); if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;
    if (idx < static_cast<int>(m_hasPreviewBackup.size()) && m_hasPreviewBackup[idx])
    {
        m_windowSettings[idx] = m_previewBackup[idx];
        m_hasPreviewBackup[idx] = false;
        UpdateSettingsForGroup(idx);
    }
    bool anyPreview = false; for (bool b : m_hasPreviewBackup) { if (b) { anyPreview = true; break; } }
    m_isPreviewActive = anyPreview;
    // Re-hide the windows shown for preview
    SetHiddenForGroup(idx, true);
}


    void winrt::Winvert4::implementation::MainWindow::LumaWeight_ValueChanged(winrt::Microsoft::UI::Xaml::Controls::NumberBox const&, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&)
    {
        // Defer handling until after the window is fully initialized to avoid race conditions.
        if (!m_isAppInitialized) return;

        m_lumaWeights[0] = static_cast<float>(LumaRNumberBox().Value());
        m_lumaWeights[1] = static_cast<float>(LumaGNumberBox().Value());
        m_lumaWeights[2] = static_cast<float>(LumaBNumberBox().Value());

        // Apply to all windows immediately
        for (size_t i = 0; i < m_windowSettings.size(); ++i)
        {
            memcpy(m_windowSettings[i].lumaWeights, m_lumaWeights, sizeof(m_lumaWeights));
            UpdateSettingsForGroup(static_cast<int>(i));
        }
        SaveAppState();
    }

    void winrt::Winvert4::implementation::MainWindow::BrightnessDelay_ValueChanged(winrt::Microsoft::UI::Xaml::Controls::NumberBox const&, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&)
    {
        if (!m_isAppInitialized) return;
        // Read the current value from the UI safely via FindName
        int newDelay = 0;
        {
            auto root = this->Content().try_as<FrameworkElement>();
            if (root)
            {
                auto nb = root.FindName(L"BrightnessDelayNumberBox").try_as<Controls::NumberBox>();
                if (nb) newDelay = static_cast<int>(nb.Value());
            }
        }
        if (newDelay < 0) newDelay = 0;
        if (newDelay == m_brightnessDelayFrames) return;
        m_brightnessDelayFrames = newDelay;
        // Apply to all windows immediately
        for (size_t i = 0; i < m_windowSettings.size(); ++i)
        {
            m_windowSettings[i].brightnessProtectionDelayFrames = m_brightnessDelayFrames;
            UpdateSettingsForGroup(static_cast<int>(i));
        }
        SaveAppState();
    }



    // --- Color Mapping UI ---
    void winrt::Winvert4::implementation::MainWindow::RefreshColorMapList()
    {
        Controls::ItemsControl listCtrl{ nullptr };
        DataTemplate rowTemplate{ nullptr };
        Media::Brush accentBrush{ nullptr };

        // Resolve UI elements and resources
        if (auto root = this->Content().try_as<FrameworkElement>())
        {
            listCtrl = root.FindName(L"ColorMapList").try_as<Controls::ItemsControl>();
            auto obj = root.Resources().TryLookup(box_value(L"ColorMapRowTemplate"));
            rowTemplate = obj.try_as<DataTemplate>();
        }
        if (!listCtrl || !rowTemplate) return;

        // Accent brush (fallback to system blue)
        if (auto app = Application::Current())
        {
            auto obj = app.Resources().TryLookup(box_value(L"AccentStrokeColorDefaultBrush"));
            accentBrush = obj.try_as<Media::Brush>();
        }
        if (!accentBrush)
        {
            Media::SolidColorBrush sb; winrt::Windows::UI::Color cc{}; cc.A=255; cc.R=0; cc.G=120; cc.B=215; sb.Color(cc); accentBrush = sb;
        }

        // Rebuild with UI-update guard to prevent spurious events
        m_isUpdatingColorMapUI = true;
        listCtrl.Items().Clear();

        auto& maps = m_globalColorMaps;
        for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        {
            auto row = rowTemplate.LoadContent().try_as<FrameworkElement>();
            if (!row) continue;

            // Enable checkbox
            if (auto cb = row.FindName(L"RowEnableCheckBox").try_as<Controls::CheckBox>())
            {
                cb.IsChecked(maps[i].enabled);
                cb.Tag(box_value(i));
                cb.HorizontalContentAlignment(HorizontalAlignment::Center);
                cb.Padding(ThicknessHelper::FromUniformLength(0));
                cb.MinWidth(0);
                cb.Content(box_value(L""));
                cb.Checked([this](auto const& s, auto const& e){ ColorMapEnable_Toggled(s, e); });
                cb.Unchecked([this](auto const& s, auto const& e){ ColorMapEnable_Toggled(s, e); });
            }

            // Source swatch
            bool isSelSrc = (i == m_selectedColorMapRowIndex) && m_selectedSwatchIsSource;
            if (auto srcBtn = row.FindName(L"RowSrcButton").try_as<Controls::Button>())
            {
                srcBtn.Tag(box_value(i));
                srcBtn.Click([this](auto const& s, auto const& e){ ColorMapSourceSwatch_Click(s, e); });
                srcBtn.BorderThickness(ThicknessHelper::FromUniformLength(isSelSrc ? 3 : 1));
                if (isSelSrc && accentBrush) srcBtn.BorderBrush(accentBrush);
                winrt::Windows::UI::Color c{}; c.A = 255; c.R = maps[i].srcR; c.G = maps[i].srcG; c.B = maps[i].srcB;
                Media::SolidColorBrush brush; brush.Color(c);
                srcBtn.Background(brush);
            }

            // Destination swatch
            bool isSelDst = (i == m_selectedColorMapRowIndex) && !m_selectedSwatchIsSource;
            if (auto dstBtn = row.FindName(L"RowDstButton").try_as<Controls::Button>())
            {
                dstBtn.Tag(box_value(i));
                dstBtn.Click([this](auto const& s, auto const& e){ ColorMapDestSwatch_Click(s, e); });
                dstBtn.BorderThickness(ThicknessHelper::FromUniformLength(isSelDst ? 3 : 1));
                if (isSelDst && accentBrush) dstBtn.BorderBrush(accentBrush);
                winrt::Windows::UI::Color c{}; c.A = 255; c.R = maps[i].dstR; c.G = maps[i].dstG; c.B = maps[i].dstB;
                Media::SolidColorBrush brush; brush.Color(c);
                dstBtn.Background(brush);
            }

            // Tolerance slider
            if (auto tol = row.FindName(L"RowToleranceSlider").try_as<Controls::Slider>())
            {
                tol.Tag(box_value(i));
                tol.Value(maps[i].tolerance);
                tol.ValueChanged([this](auto const& s, auto const& e){ ColorMapToleranceSlider_ValueChanged(s, e); });
            }

            // Remove button
            if (auto rm = row.FindName(L"RowRemoveButton").try_as<Controls::Button>())
            {
                rm.Tag(box_value(i));
                rm.Click([this](auto const& s, auto const& e){ ColorMapRemoveButton_Click(s, e); });
            }

            listCtrl.Items().Append(row);
        }
        m_isUpdatingColorMapUI = false;
    }


// --- Unified AppState to blob file ---
void winrt::Winvert4::implementation::MainWindow::SaveAppState()
{
    if (!m_isSavingEnabled) { winvert4::Log("SaveAppState: suppressed (init)"); return; }
    using winrt::Windows::Storage::ApplicationData;
    try
    {
        auto folder = ApplicationData::Current().LocalFolder();
        std::wstring path = std::wstring(folder.Path().c_str()) + L"\\appstate.blob";

        winvert4::Logf("SaveAppState: path=%ls", path.c_str());
        std::wofstream ofs(path, std::ios::trunc | std::ios::binary);
        if (!ofs) { winvert4::Log("SaveAppState: open failed"); return; }
        ofs.imbue(std::locale::classic());
        ofs << L"V=1\n";
        ofs << L"FPS=" << (m_showFpsOverlay ? 1 : 0) << L"\n";
        ofs << L"SELCLREN=" << (m_useCustomSelectionColor ? 1 : 0) << L"\n";
        ofs << L"SELCLR=" << (int)GetRValue(m_selectionColor) << L"," << (int)GetGValue(m_selectionColor) << L"," << (int)GetBValue(m_selectionColor) << L"\n";
        ofs << L"BRIGHTDELAY=" << m_brightnessDelayFrames << L"\n";
        ofs << L"LUMA=" << std::fixed << std::setprecision(6) << m_lumaWeights[0] << L"," << m_lumaWeights[1] << L"," << m_lumaWeights[2] << L"\n";
        ofs << L"HKI=" << m_hotkeyInvertMod << L"," << m_hotkeyInvertVk << L"\n";
        ofs << L"HKF=" << m_hotkeyFilterMod << L"," << m_hotkeyFilterVk << L"\n";
        ofs << L"HKR=" << m_hotkeyRemoveMod << L"," << m_hotkeyRemoveVk << L"\n";
        ofs << L"FAV=" << m_favoriteFilterIndex << L"\n";

        // Saved custom filters
        int countCustom = 0; for (auto& f : m_savedFilters) if (!f.isBuiltin) ++countCustom;
        ofs << L"FILTERS=" << countCustom << L"\n";
        for (auto& f : m_savedFilters)
        {
            if (f.isBuiltin) continue;
            std::wstring safe = f.name;
            for (auto& ch : safe) if (ch == L'\n' || ch == L'\r') ch = L' ';
            ofs << L"FN=" << safe << L"\n";
            ofs << L"FM=";
            for (int i = 0; i < 16; ++i) { ofs << std::fixed << std::setprecision(6) << f.mat[i]; if (i != 15) ofs << L","; }
            ofs << L"\nFO=";
            for (int i = 0; i < 4; ++i) { ofs << std::fixed << std::setprecision(6) << f.offset[i]; if (i != 3) ofs << L","; }
            ofs << L"\n";
        }

        // Color maps
        ofs << L"COLORMAPS=" << (int)m_globalColorMaps.size() << L"\n";
        for (auto& e : m_globalColorMaps)
        {
            ofs << L"CM=" << (e.enabled ? 1 : 0) << L","
                << (int)e.srcR << L"," << (int)e.srcG << L"," << (int)e.srcB << L","
                << (int)e.dstR << L"," << (int)e.dstG << L"," << (int)e.dstB << L"," << e.tolerance << L"\n";
        }
    }
    catch (...) { }
}

void winrt::Winvert4::implementation::MainWindow::LoadAppState()
{
    using winrt::Windows::Storage::ApplicationData;
    try
    {
        auto folder = ApplicationData::Current().LocalFolder();
        std::wstring path = std::wstring(folder.Path().c_str()) + L"\\appstate.blob";
        winvert4::Logf("LoadAppState: path=%ls", path.c_str());
        std::wifstream ifs(path, std::ios::binary);
        if (!ifs) { winvert4::Log("LoadAppState: open failed"); return; }
        ifs.imbue(std::locale::classic());
        std::wstring line;
        m_savedFilters.erase(std::remove_if(m_savedFilters.begin(), m_savedFilters.end(), [](const SavedFilter& f){ return !f.isBuiltin; }), m_savedFilters.end());
        m_globalColorMaps.clear();

        bool hasSelClr = false; int selR = 255, selG = 0, selB = 0; bool selClrEn = false;
        while (std::getline(ifs, line))
        {
            if (line.rfind(L"FPS=", 0) == 0) { m_showFpsOverlay = (line.size() > 4 && line[4] == L'1'); }
            else if (line.rfind(L"SELCLREN=", 0) == 0) { selClrEn = (line.size() > 10 && line[10] == L'1'); }
            else if (line.rfind(L"SELCLR=", 0) == 0)
            {
                std::wstring rest = line.substr(7);
                int vals[3]{}; int idx = 0; size_t last = 0; size_t p2;
                while ((p2 = rest.find(L',', last)) != std::wstring::npos && idx < 2) { vals[idx++] = _wtoi(rest.substr(last, p2 - last).c_str()); last = p2 + 1; }
                if (last < rest.size()) vals[idx++] = _wtoi(rest.substr(last).c_str());
                if (idx >= 3) { selR = vals[0] & 0xFF; selG = vals[1] & 0xFF; selB = vals[2] & 0xFF; hasSelClr = true; }
            }
            else if (line.rfind(L"BRIGHTDELAY=", 0) == 0) { try { m_brightnessDelayFrames = std::stoi(line.substr(12)); } catch (...) {} }
            else if (line.rfind(L"LUMA=", 0) == 0)
            {
                std::wstring rest = line.substr(5); int idx = 0; size_t last = 0;
                while (idx < 3 && last <= rest.size()) { size_t comma = rest.find(L',', last); std::wstring tok = rest.substr(last, (comma == std::wstring::npos) ? std::wstring::npos : (comma - last)); if (!tok.empty()) { try { m_lumaWeights[idx] = std::stof(tok); } catch (...) {} } ++idx; if (comma == std::wstring::npos) break; last = comma + 1; }
            }
            else if (line.rfind(L"HKI=", 0) == 0)
            {
                std::wstring rest = line.substr(4); size_t comma = rest.find(L','); if (comma != std::wstring::npos) { try { m_hotkeyInvertMod = static_cast<UINT>(std::stoul(rest.substr(0, comma))); } catch (...) {} try { m_hotkeyInvertVk = static_cast<UINT>(std::stoul(rest.substr(comma + 1))); } catch (...) {} }
            }
            else if (line.rfind(L"HKF=", 0) == 0)
            {
                std::wstring rest = line.substr(4); size_t comma = rest.find(L','); if (comma != std::wstring::npos) { try { m_hotkeyFilterMod = static_cast<UINT>(std::stoul(rest.substr(0, comma))); } catch (...) {} try { m_hotkeyFilterVk = static_cast<UINT>(std::stoul(rest.substr(comma + 1))); } catch (...) {} }
            }
            else if (line.rfind(L"HKR=", 0) == 0)
            {
                std::wstring rest = line.substr(4); size_t comma = rest.find(L','); if (comma != std::wstring::npos) { try { m_hotkeyRemoveMod = static_cast<UINT>(std::stoul(rest.substr(0, comma))); } catch (...) {} try { m_hotkeyRemoveVk = static_cast<UINT>(std::stoul(rest.substr(comma + 1))); } catch (...) {} }
            }
            else if (line.rfind(L"FAV=", 0) == 0) { try { m_favoriteFilterIndex = std::stoi(line.substr(4)); } catch (...) {} }
            else if (line.rfind(L"FILTERS=", 0) == 0)
            {
                int n = 0; try { n = std::stoi(line.substr(9)); } catch (...) { n = 0; }
                for (int k = 0; k < n; ++k)
                {
                    std::wstring fn, fm, fo;
                    if (!std::getline(ifs, fn)) break; if (!std::getline(ifs, fm)) break; if (!std::getline(ifs, fo)) break;
                    SavedFilter sf{}; sf.isBuiltin = false; if (fn.rfind(L"FN=", 0) == 0) sf.name = fn.substr(3);
                    if (fm.rfind(L"FM=", 0) == 0) { std::wstring mpart = fm.substr(3); int mi = 0; size_t last = 0; while (mi < 16 && last <= mpart.size()) { size_t comma = mpart.find(L',', last); std::wstring tok = mpart.substr(last, (comma == std::wstring::npos) ? std::wstring::npos : (comma - last)); if (!tok.empty()) { try { sf.mat[mi] = std::stof(tok); } catch (...) {} } ++mi; if (comma == std::wstring::npos) break; last = comma + 1; } }
                    if (fo.rfind(L"FO=", 0) == 0) { std::wstring opart = fo.substr(3); int oi = 0; size_t last = 0; while (oi < 4 && last <= opart.size()) { size_t comma = opart.find(L',', last); std::wstring tok = opart.substr(last, (comma == std::wstring::npos) ? std::wstring::npos : (comma - last)); if (!tok.empty()) { try { sf.offset[oi] = std::stof(tok); } catch (...) {} } ++oi; if (comma == std::wstring::npos) break; last = comma + 1; } }
                    m_savedFilters.push_back(sf);
                }
            }
            else if (line.rfind(L"COLORMAPS=", 0) == 0)
            {
                int m = 0; try { m = std::stoi(line.substr(10)); } catch (...) { m = 0; }
                for (int k = 0; k < m; ++k)
                {
                    std::wstring cm; if (!std::getline(ifs, cm)) break;
                    if (cm.rfind(L"CM=", 0) == 0)
                    {
                        std::wstring row = cm.substr(3);
                        int vals[8]{}; int idx = 0; size_t last = 0; size_t p2; while ((p2 = row.find(L',', last)) != std::wstring::npos && idx < 7) { vals[idx++] = _wtoi(row.substr(last, p2 - last).c_str()); last = p2 + 1; }
                        if (last < row.size()) vals[idx++] = _wtoi(row.substr(last).c_str());
                        if (idx >= 8) { ColorMapEntry e{}; e.enabled = vals[0] != 0; e.srcR = (uint8_t)vals[1]; e.srcG = (uint8_t)vals[2]; e.srcB = (uint8_t)vals[3]; e.dstR = (uint8_t)vals[4]; e.dstG = (uint8_t)vals[5]; e.dstB = (uint8_t)vals[6]; e.tolerance = vals[7]; m_globalColorMaps.push_back(e); }
                    }
                }
            }
        }

        // Apply loaded values to UI
        m_useCustomSelectionColor = selClrEn;
        if (auto t = ShowFpsToggle()) t.IsOn(m_showFpsOverlay);
        if (hasSelClr)
        {
            winrt::Windows::UI::Color c{}; c.A = 255; c.R = static_cast<uint8_t>(selR); c.G = static_cast<uint8_t>(selG); c.B = static_cast<uint8_t>(selB);
            if (auto picker = SelectionColorPicker()) picker.Color(c);
            m_selectionColor = RGB(selR, selG, selB);
        }
        // Apply hotkeys and favorites UI
        RegisterAllHotkeys();
        UpdateAllHotkeyText();
        if (auto root = this->Content().try_as<FrameworkElement>())
        {
            auto fav = root.FindName(L"FavoriteFilterComboBox").try_as<Controls::ComboBox>(); if (fav) fav.SelectedIndex(m_favoriteFilterIndex);
            auto nb = root.FindName(L"BrightnessDelayNumberBox").try_as<Controls::NumberBox>(); if (nb) nb.Value(m_brightnessDelayFrames);
        }
        LumaRNumberBox().Value(m_lumaWeights[0]);
        LumaGNumberBox().Value(m_lumaWeights[1]);
        LumaBNumberBox().Value(m_lumaWeights[2]);
        RefreshColorMapList();
        UpdateSavedFiltersCombo();
        UpdateFilterDropdown();
    }
    catch (...) { }
}
    void winrt::Winvert4::implementation::MainWindow::ColorMappingToggle_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        int idx = SelectedTabIndex();
        if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;
        auto& s = m_windowSettings[idx];
        s.isColorMappingEnabled = !s.isColorMappingEnabled;
        ApplyGlobalColorMapsToSettings(s);
        UpdateSettingsForGroup(idx, s);
        UpdateUIState();
    }

void winrt::Winvert4::implementation::MainWindow::ColorMapAddButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    ColorMapEntry e{};
    // Insert new mappings at the top so most-recent appears first
    m_globalColorMaps.insert(m_globalColorMaps.begin(), e);
    // Select the newly inserted row and mark source swatch as active for editing
    m_selectedColorMapRowIndex = 0;
    m_selectedSwatchIsSource = true;
    RefreshColorMapList();
    SaveAppState();
}

    void winrt::Winvert4::implementation::MainWindow::ColorMapEnable_Toggled(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (m_isUpdatingColorMapUI) return;
        auto cb = sender.try_as<Controls::CheckBox>();
        if (!cb) return;
        auto tag = cb.Tag();
        if (!tag) return;
        int row = unbox_value<int>(tag);
        auto& maps = m_globalColorMaps;
        if (row >= 0 && row < static_cast<int>(maps.size()))
        {
            maps[row].enabled = cb.IsChecked().GetBoolean();
            // Live push if mapping is enabled on the selected window
            int idxSel = SelectedTabIndex();
            if (idxSel >= 0 && idxSel < static_cast<int>(m_windowSettings.size()))
            {
                if (m_windowSettings[idxSel].isColorMappingEnabled)
                {
                    ApplyGlobalColorMapsToSettings(m_windowSettings[idxSel]);
                    UpdateSettingsForGroup(idxSel);
                }
            }
        }
        SaveAppState();
    }

    void winrt::Winvert4::implementation::MainWindow::ColorMapSourceSwatch_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto btn = sender.try_as<Controls::Button>(); if (!btn) return;
        int row = -1; auto tagObj = btn.Tag(); if (tagObj) row = unbox_value<int>(tagObj);
        auto& maps = m_globalColorMaps; if (row < 0 || row >= static_cast<int>(maps.size())) return;
        m_selectedColorMapRowIndex = row; m_selectedSwatchIsSource = true;
        winrt::Windows::UI::Color c{}; c.A = 255; c.R = maps[row].srcR; c.G = maps[row].srcG; c.B = maps[row].srcB;
        Controls::ColorPicker picker{ nullptr };
        {
            auto root = this->Content().try_as<FrameworkElement>();
            if (root) picker = root.FindName(L"ColorMapPicker").try_as<Controls::ColorPicker>();
        }
        if (picker) { m_isProgrammaticColorPickerChange = true; picker.Color(c); m_isProgrammaticColorPickerChange = false; }
        // Update highlight to indicate selection
        RefreshColorMapList();
        SaveAppState();
    }

    void winrt::Winvert4::implementation::MainWindow::ColorMapDestSwatch_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto btn = sender.try_as<Controls::Button>(); if (!btn) return;
        int row = -1; auto tagObj = btn.Tag(); if (tagObj) row = unbox_value<int>(tagObj);
        auto& maps = m_globalColorMaps; if (row < 0 || row >= static_cast<int>(maps.size())) return;
        m_selectedColorMapRowIndex = row; m_selectedSwatchIsSource = false;
        winrt::Windows::UI::Color c{}; c.A = 255; c.R = maps[row].dstR; c.G = maps[row].dstG; c.B = maps[row].dstB;
        Controls::ColorPicker picker{ nullptr };
        {
            auto root = this->Content().try_as<FrameworkElement>();
            if (root) picker = root.FindName(L"ColorMapPicker").try_as<Controls::ColorPicker>();
        }
        if (picker) { m_isProgrammaticColorPickerChange = true; picker.Color(c); m_isProgrammaticColorPickerChange = false; }
        // Update highlight to indicate selection
        RefreshColorMapList();
        SaveAppState();
    }

    void winrt::Winvert4::implementation::MainWindow::ColorMapPicker_ColorChanged(Controls::ColorPicker const&, Controls::ColorChangedEventArgs const& args)
    {
        if (m_isProgrammaticColorPickerChange || m_isUpdatingColorMapUI) return; // ignore programmatic/UI rebuild
        auto& maps = m_globalColorMaps;
        int row = m_selectedColorMapRowIndex;
        if (row < 0 || row >= static_cast<int>(maps.size())) return;
        auto c = args.NewColor();
        if (m_selectedSwatchIsSource)
        {
            if (maps[row].srcR == c.R && maps[row].srcG == c.G && maps[row].srcB == c.B) return;
            maps[row].srcR = c.R; maps[row].srcG = c.G; maps[row].srcB = c.B;
        }
        else
        {
            if (maps[row].dstR == c.R && maps[row].dstG == c.G && maps[row].dstB == c.B) return;
            maps[row].dstR = c.R; maps[row].dstG = c.G; maps[row].dstB = c.B;
        }
        SaveAppState();
        // Update only the affected swatch button's background to avoid rebuilding the whole list
        {
            Controls::StackPanel listPanel{ nullptr };
            if (auto root = this->Content().try_as<FrameworkElement>())
                listPanel = root.FindName(L"ColorMapListPanel").try_as<Controls::StackPanel>();
            if (listPanel)
            {
                uint32_t size = listPanel.Children().Size();
                // Account for header row if present (header + rows)
                uint32_t baseIndex = (size == maps.size() + 1) ? 1u : 0u;
                uint32_t childIndex = baseIndex + static_cast<uint32_t>(row);
                if (childIndex < size)
                {
                    auto rowGrid = listPanel.Children().GetAt(childIndex).try_as<Controls::Grid>();
                    if (rowGrid)
                    {
                        int targetCol = m_selectedSwatchIsSource ? 1 : 3;
                        for (auto child : rowGrid.Children())
                        {
                            auto btn = child.try_as<Controls::Button>();
                            if (!btn) continue;
                            int col = Controls::Grid::GetColumn(btn);
                            if (col == targetCol)
                            {
                                Media::SolidColorBrush brush;
                                winrt::Windows::UI::Color cc{}; cc.A = 255; cc.R = c.R; cc.G = c.G; cc.B = c.B;
                                brush.Color(cc);
                                btn.Background(brush);
                                // Emphasize selected swatch border after change with accent color
                                btn.BorderThickness(ThicknessHelper::FromUniformLength(3));
                                {
                                    Media::Brush accentBrush{ nullptr };
                                    auto app2 = Application::Current();
                                    if (app2)
                                    {
                                        auto res2 = app2.Resources();
                                        auto obj2 = res2.TryLookup(box_value(L"AccentStrokeColorDefaultBrush"));
                                        accentBrush = obj2.try_as<Media::Brush>();
                                    }
                                    if (!accentBrush)
                                    {
                                        Media::SolidColorBrush sb2; winrt::Windows::UI::Color ac{}; ac.A=255; ac.R=0; ac.G=120; ac.B=215; sb2.Color(ac); accentBrush = sb2;
                                    }
                                    btn.BorderBrush(accentBrush);
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        // Push live update if mapping is enabled for the selected window
        {
            int idxSel = SelectedTabIndex();
            if (idxSel >= 0 && idxSel < static_cast<int>(m_windowSettings.size()))
            {
            if (m_windowSettings[idxSel].isColorMappingEnabled)
            {
                ApplyGlobalColorMapsToSettings(m_windowSettings[idxSel]);
                UpdateSettingsForGroup(idxSel);
            }
            }
        }
    }

    void winrt::Winvert4::implementation::MainWindow::ColorMapToleranceSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (m_isUpdatingColorMapUI) return;
        int row = -1;
        if (auto fe = sender.try_as<FrameworkElement>())
        {
            auto tagObj = fe.Tag();
            if (tagObj) row = unbox_value<int>(tagObj);
        }
        SaveAppState();
        auto& maps = m_globalColorMaps; if (row < 0 || row >= static_cast<int>(maps.size())) return;
        int newTol = static_cast<int>(e.NewValue());
        if (maps[row].tolerance == newTol) return;
        maps[row].tolerance = newTol;
        // Push live update if mapping is enabled for the selected window
        {
            int idxSel = SelectedTabIndex();
            if (idxSel >= 0 && idxSel < static_cast<int>(m_windowSettings.size()))
            {
                if (m_windowSettings[idxSel].isColorMappingEnabled)
                {
                    ApplyGlobalColorMapsToSettings(m_windowSettings[idxSel]);
                    UpdateSettingsForGroup(idxSel);
                }
            }
        }
        
    }

    void winrt::Winvert4::implementation::MainWindow::ColorMapRemoveButton_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        auto btn = sender.try_as<Controls::Button>(); if (!btn) return;
        int row = -1; auto tagObj = btn.Tag(); if (tagObj) row = unbox_value<int>(tagObj);
        auto& maps = m_globalColorMaps; if (row < 0 || row >= static_cast<int>(maps.size())) return;
        maps.erase(maps.begin() + row);
        if (m_selectedColorMapRowIndex == row) { m_selectedColorMapRowIndex = -1; }
        else if (m_selectedColorMapRowIndex > row) { m_selectedColorMapRowIndex--; }
        RefreshColorMapList();
        SaveAppState();
// Push live update if mapping is enabled for the selected window
        {
            int idxSel = SelectedTabIndex();
            if (idxSel >= 0 && idxSel < static_cast<int>(m_windowSettings.size()))
            {
                if (m_windowSettings[idxSel].isColorMappingEnabled)
                {
                    ApplyGlobalColorMapsToSettings(m_windowSettings[idxSel]);
                    UpdateSettingsForGroup(idxSel);
                }
            }
        }
        
    }

void winrt::Winvert4::implementation::MainWindow::PreviewColorMapButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        int idx = SelectedTabIndex();
        if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;
        if (static_cast<int>(m_hasPreviewBackup.size()) <= idx)
        {
            m_hasPreviewBackup.resize(idx + 1, false);
            m_previewBackup.resize(idx + 1);
        }
        if (!m_hasPreviewBackup[idx])
        {
            m_previewBackup[idx] = m_windowSettings[idx];
            m_hasPreviewBackup[idx] = true;
        }
        m_windowSettings[idx].isColorMappingEnabled = true;
        UpdateSettingsForGroup(idx);
        m_isPreviewActive = true;
    }

    void winrt::Winvert4::implementation::MainWindow::ColorMapSampleButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!m_isSamplingColor)
        {
            StartColorSample();
        }
    }

    void winrt::Winvert4::implementation::MainWindow::ColorMapPreserveToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        int idx = SelectedTabIndex();
        if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;
        Controls::ToggleSwitch toggle{ nullptr };
        if (auto root = this->Content().try_as<FrameworkElement>())
        {
            toggle = root.FindName(L"ColorMapPreserveToggle").try_as<Controls::ToggleSwitch>();
        }
        if (!toggle) return;
        m_windowSettings[idx].colorMapPreserveBrightness = toggle.IsOn();
        if (m_windowSettings[idx].isColorMappingEnabled)
        {
            ApplyGlobalColorMapsToSettings(m_windowSettings[idx]);
            UpdateSettingsForGroup(idx);
        }
        SaveAppState();
    }




int winrt::Winvert4::implementation::MainWindow::FavoriteFilterIndex() const
{
    return m_favoriteFilterIndex;
}
void winrt::Winvert4::implementation::MainWindow::PreviewColorMapToggle_Checked(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    int idx = SelectedTabIndex();
    if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;
    if (static_cast<int>(m_hasPreviewBackup.size()) <= idx) { m_hasPreviewBackup.resize(idx + 1, false); m_previewBackup.resize(idx + 1); }
    if (!m_hasPreviewBackup[idx]) { m_previewBackup[idx] = m_windowSettings[idx]; m_hasPreviewBackup[idx] = true; }
    m_windowSettings[idx].isInvertEffectEnabled = false;
    m_windowSettings[idx].isBrightnessProtectionEnabled = false;
    m_windowSettings[idx].isCustomEffectActive = false;
    m_windowSettings[idx].isColorMappingEnabled = true;
    ApplyGlobalColorMapsToSettings(m_windowSettings[idx]);
    UpdateSettingsForGroup(idx);
    m_isPreviewActive = true;
    SetHiddenForGroup(idx, false);
}
void winrt::Winvert4::implementation::MainWindow::PreviewColorMapToggle_Unchecked(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    int idx = SelectedTabIndex(); if (idx < 0 || idx >= static_cast<int>(m_windowSettings.size())) return;
    if (idx < static_cast<int>(m_hasPreviewBackup.size()) && m_hasPreviewBackup[idx])
    {
        m_windowSettings[idx] = m_previewBackup[idx];
        m_hasPreviewBackup[idx] = false;
        UpdateSettingsForGroup(idx);
    }
    bool anyPreview = false; for (bool b : m_hasPreviewBackup) { if (b) { anyPreview = true; break; } }
    m_isPreviewActive = anyPreview;
    SetHiddenForGroup(idx, true);
}

