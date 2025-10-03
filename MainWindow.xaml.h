#pragma once
#include "MainWindow.g.h"
#include "EffectWindow.h"
#include "OutputManager.h"
#include <vector>
#include <memory>
#include "EffectSettings.h"

namespace winrt::Winvert4::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        int32_t MyProperty();
        void MyProperty(int32_t value);

        void ToggleSnipping();
        void StartScreenSelection();

        // --- XAML Event Handlers ---
        void BrightnessProtection_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void InvertEffect_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void AddWindow_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RemoveWindow_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void HideAllWindows_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SettingsButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void BackButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SettingsPanel_SizeChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void CollapseAllSettingsExpanders();
        void UpdateSettingsColumnsForWindowState();
        void CustomFiltersExpander_Collapsed(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::ExpanderCollapsedEventArgs const&);
        void CustomFiltersExpander_Expanding(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::ExpanderExpandingEventArgs const&);
        void LumaWeight_ValueChanged(winrt::Microsoft::UI::Xaml::Controls::NumberBox const&, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void BrightnessDelay_ValueChanged(winrt::Microsoft::UI::Xaml::Controls::NumberBox const&, winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void ShowFpsToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RebindInvertHotkeyButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RebindFilterHotkeyButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RebindRemoveHotkeyButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SelectionColorPicker_ColorChanged(winrt::Microsoft::UI::Xaml::Controls::ColorPicker const&, winrt::Microsoft::UI::Xaml::Controls::ColorChangedEventArgs const&);
        void FavoriteFilterComboBox_SelectionChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        // Color Mapping
        void ColorMappingToggle_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ColorMapAddButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ColorMapPicker_ColorChanged(winrt::Microsoft::UI::Xaml::Controls::ColorPicker const&, winrt::Microsoft::UI::Xaml::Controls::ColorChangedEventArgs const&);
        void ColorMapEnable_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ColorMapSourceSwatch_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ColorMapDestSwatch_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ColorMapToleranceSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void ColorMapRemoveButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PreviewColorMapButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ColorMapSampleButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PreviewFilterToggle_Checked(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PreviewFilterToggle_Unchecked(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

        void RefreshColorMapList();

        // --- Tab and Flyout Handlers ---
        void InfoBar_Closed(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::InfoBarClosedEventArgs const&);
        void FiltersMenuFlyout_Closing(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutBaseClosingEventArgs const&);
        void RegionsTabView_AddTabButtonClick(winrt::Windows::Foundation::IInspectable const&, winrt::Windows::Foundation::IInspectable const&);
        void RegionsTabView_TabCloseRequested(winrt::Microsoft::UI::Xaml::Controls::TabView const&, winrt::Microsoft::UI::Xaml::Controls::TabViewTabCloseRequestedEventArgs const&);
        void RegionsTabView_SelectionChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);

        // --- Custom Filter Handlers ---
        void AddNewFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SaveFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void DeleteFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ClearFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void PreviewFilterButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void AdvancedMatrixToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void BrightnessSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void ContrastSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void SaturationSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void TemperatureSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void TintSlider_ValueChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void SimpleResetButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void SavedFiltersComboBox_SelectionChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void EnterKeyCommit_Unfocus(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
        void FilterMenuItem_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);



        void OnPointerPressed(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {}
        void OnPointerMoved(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {}
        void OnPointerReleased(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&) {}

    private:
        // Global low-level mouse hook for one-shot sampling
        static HHOOK s_mouseHook;
        static MainWindow* s_samplingInstance;
        static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

        static LRESULT CALLBACK SelectionWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData);
        static LRESULT CALLBACK WindowSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

        static std::vector<RECT> s_monitorRects;

        bool   m_isSelecting{ false };
        bool   m_isDragging{ false };
        HWND   m_selectionHwnd{ nullptr };
        POINT  m_ptStart{ 0, 0 }; // In client coords of selection window
        POINT  m_ptEnd{ 0, 0 };   // In client coords of selection window

        ULONG_PTR m_gdiplusToken;
        HBITMAP m_screenBmp{ nullptr };
        HDC     m_screenMemDC{ nullptr };
        SIZE    m_screenSize{ 0, 0 };
        POINT   m_virtualOrigin{ 0, 0 };

        void CaptureScreenBitmap();
        void EnumerateMonitors();
        void ReleaseScreenBitmap();
        RECT MakeRectFromPoints(POINT a, POINT b) const;
        void OnSelectionCompleted(RECT sel);

        // Primary window per tab index
        std::vector<std::unique_ptr<EffectWindow>> m_effectWindows;
        // Additional windows for the same logical region when spanning multiple monitors
        std::vector<std::vector<std::unique_ptr<EffectWindow>>> m_effectWindowExtras;
        std::unique_ptr<OutputManager> m_outputManager;
        HWND m_mainHwnd{ nullptr };

        // --- UI State ---
        bool m_isAppInitialized{ false };
        bool m_areWindowsHidden{ false };
        bool m_wereWindowsHiddenForSettings{ false };
        bool m_hasEverHadWindows{ false };
        bool m_lastRemovalViaUI{ false };
        bool m_lastRemovalInitiatedByHotkey{ false };
        bool m_keepFiltersFlyoutOpenNext{ false };
        float m_lumaWeights[3]{ 0.2126f, 0.7152f, 0.0722f }; // Global setting (BT.709), copied to new windows
        bool m_showSelectionInstructions{ true }; // From reference

        // --- Settings ---
        int m_fpsSetting{ 0 };
        bool m_showFpsOverlay{ false };
        COLORREF m_selectionColor{ RGB(255, 0, 0) };
        bool m_controlPanelShownYet{ false };
        int  m_brightnessDelayFrames{ 0 }; // default 0 frames

        // --- Hotkeys ---
        enum class RebindingState { None, Invert, Filter, Remove };
        RebindingState m_rebindingState{ RebindingState::None };
        UINT m_hotkeyInvertMod{ MOD_WIN | MOD_SHIFT };
        UINT m_hotkeyInvertVk{ 'I' };
        UINT m_hotkeyFilterMod{ MOD_WIN | MOD_SHIFT };
        UINT m_hotkeyFilterVk{ 'F' };
        UINT m_hotkeyRemoveMod{ MOD_WIN | MOD_SHIFT };
        UINT m_hotkeyRemoveVk{ 'R' };

        enum class PendingEffect { None, Invert, Filter };

        // Favorite filter index (tracked independently of UI accessor to avoid header deps)
        int FavoriteFilterIndex() const;
        int m_favoriteFilterIndex{ -1 };
        PendingEffect m_pendingEffect{ PendingEffect::None };

        void RegisterAllHotkeys();
        void OnInvertHotkeyPressed();
        void OnFilterHotkeyPressed();
        void OnRemoveHotkeyPressed();

        // --- Per-window state ---
        std::vector<EffectSettings> m_windowSettings;
        std::vector<bool> m_windowHidden;

        // --- Saved Filters ---
        struct SavedFilter { std::wstring name; float mat[16]; float offset[4]; bool isBuiltin{ false }; };
        std::vector<SavedFilter> m_savedFilters;
        void UpdateFilterDropdown();
        void UpdateSavedFiltersCombo();
        void FilterToggleMenuItem_Click(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ApplyCompositeCustomFiltersForTab(int idx);

        // Per-tab selection of saved filters (for checked state in flyout)
        std::vector<std::vector<bool>> m_tabFilterSelections;

        // --- Preview state (temporary apply while on settings page) ---
        bool m_isPreviewActive{ false };
        std::vector<bool> m_hasPreviewBackup;
        std::vector<EffectSettings> m_previewBackup;

        // Simple (normal) mode filter parameters
        float m_simpleBrightness{ 0.0f };   // [-1..1]
        float m_simpleContrast{ 1.0f };     // [0..2]
        float m_simpleSaturation{ 1.0f };   // [0..2]
        float m_simpleTemperature{ 0.0f };  // [-1..1]
        float m_simpleTint{ 0.0f };         // [-1..1]

        void ComposeSimpleMatrix(float (&outMat)[16], float (&outOff)[4]);
        void WriteMatrixToGrid(const float (&mat)[16], const float (&off)[4]);
        void EnsureFilterMatrixGridInitialized();
        void ReadMatrixFromGrid(float (&outMat)[16], float (&outOff)[4]);
        void UpdateSlidersFromMatrix(const float (&mat)[16], const float (&off)[4]);
        void CommitMatrixTextBoxes_();

        // --- UI Helpers ---
        void UpdateUIState();
        void SetWindowSize(int width, int height);
        void UpdateAllHotkeyText();
        void UpdateHotkeyText(winrt::Microsoft::UI::Xaml::Controls::TextBox const& textBox, UINT mod, UINT vk);
        int SelectedTabIndex();
        HWND SelectedWindowHwnd();
        void ApplyGlobalColorMapsToSettings(EffectSettings& settings);
        // Apply current or provided settings to the primary + any extra windows for a tab index
        void UpdateSettingsForGroup(int idx);
        void UpdateSettingsForGroup(int idx, const EffectSettings& settings);
        void SetHiddenForGroup(int idx, bool hidden);
        void SetHiddenForAll(bool hidden);

        // --- Icon Sources ---
        winrt::Microsoft::UI::Xaml::Media::ImageSource m_brightnessOnIconSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::ImageSource m_brightnessOffIconSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::ImageSource m_hideIconSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::ImageSource m_showIconSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::ImageSource m_colorMappingOnIconSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::ImageSource m_colorMappingOffIconSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::ImageSource m_invertOnIconSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::ImageSource m_invertOffIconSource{ nullptr };

        // --- Color Mapping UI state ---
        int m_selectedColorMapRowIndex{ -1 };
        bool m_selectedSwatchIsSource{ true };
        // Global color maps applied to all windows when enabled
        std::vector<ColorMapEntry> m_globalColorMaps;

        // --- Color sampling state ---
        bool m_isSamplingColor{ false };
        void StartColorSample();
        void OnColorSampled(POINT ptClient);

        // --- UI reentrancy guards ---
        bool m_isProgrammaticColorPickerChange{ false };
        bool m_isUpdatingColorMapUI{ false };
        bool m_isUpdatingSimpleUI{ false };
    };
}

namespace winrt::Winvert4::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
