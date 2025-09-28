#pragma once
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

// Avoid conflict with Storyboard::GetCurrentTime
#undef GetCurrentTime

// ---- Link required Win32 libs ----
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "gdiplus.lib")

// ---- C++/WinRT & Windows App SDK ----
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>

#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
// Optional per-control headers (present in some SDKs)
#if __has_include(<winrt/Microsoft.UI.Xaml.Controls.NumberBox.h>)
#include <winrt/Microsoft.UI.Xaml.Controls.NumberBox.h>
#endif
#if __has_include(<winrt/Microsoft.UI.Xaml.Controls.ColorPicker.h>)
#include <winrt/Microsoft.UI.Xaml.Controls.ColorPicker.h>
#endif
#if __has_include(<winrt/Microsoft.UI.Xaml.Controls.InfoBar.h>)
#include <winrt/Microsoft.UI.Xaml.Controls.InfoBar.h>
#endif
#if __has_include(<winrt/Microsoft.UI.Xaml.Controls.TabView.h>)
#include <winrt/Microsoft.UI.Xaml.Controls.TabView.h>
#endif
#if __has_include(<winrt/Microsoft.UI.Xaml.Controls.Expander.h>)
#include <winrt/Microsoft.UI.Xaml.Controls.Expander.h>
#endif
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Navigation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Capture.h>

#include <Microsoft.UI.Xaml.media.dxinterop.h>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Interop.h>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#pragma comment(lib, "D3DCompiler.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#include <algorithm>
#include <cmath>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>
#include <memory>
#include <map>
#include <string>
#include <debugapi.h>
#include <mutex>
#include <condition_variable>


// Some SDKs may not yet expose this newer DWM attribute
#ifndef DWMWA_EXCLUDED_FROM_CAPTURE
#define DWMWA_EXCLUDED_FROM_CAPTURE 25
#endif

// Some SDKs may not expose the newer display affinity
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
