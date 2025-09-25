#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace
{
    constexpr wchar_t kSelectionWndClass[] = L"Winvert4_SelectionOverlayWindow";
}

// Helper: create ARGB color without Colors::
static Windows::UI::Color MakeColor(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    Windows::UI::Color c{};
    c.A = a; c.R = r; c.G = g; c.B = b;
    return c;
}

namespace winrt::Winvert4::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        // If you wired XAML pointer handlers previously, they can remain — but this
        // implementation uses the Win32 overlay for selection, so they're not required.
    }

    int32_t MainWindow::MyProperty() { return 0; }
    void MainWindow::MyProperty(int32_t) {}

    void MainWindow::ToggleSnipping()
    {
        // Preserve your external hotkey entry point; just start the selection now.
        StartScreenSelection();
    }

    void MainWindow::StartScreenSelection()
    {
        if (m_isSelecting) return;
        m_isSelecting = true;

        // 1) Capture the entire virtual screen once, to "freeze" the view
        CaptureScreenBitmap();

        // 2) Register our overlay WNDCLASS with crosshair cursor
        WNDCLASSEXW wcex{ sizeof(WNDCLASSEXW) };
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = &MainWindow::SelectionWndProc;
        wcex.hInstance = GetModuleHandleW(nullptr);
        wcex.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wcex.lpszClassName = kSelectionWndClass;
        wcex.hbrBackground = nullptr;

        static ATOM s_atom = 0;
        if (!s_atom)
        {
            s_atom = RegisterClassExW(&wcex);
        }

        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        // 3) Create a topmost, click-through-to-us, layered popup spanning all monitors
        m_selectionHwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            kSelectionWndClass,
            L"Winvert Selection",
            WS_POPUP,
            vx, vy, vw, vh,
            nullptr, nullptr, GetModuleHandleW(nullptr),
            this // pass 'this' for association in WM_NCCREATE
        );

        if (m_selectionHwnd)
        {
            // 50% alpha so the desktop "looks frozen"
            SetLayeredWindowAttributes(m_selectionHwnd, 0, 160, LWA_ALPHA);
            ShowWindow(m_selectionHwnd, SW_SHOW);
            SetForegroundWindow(m_selectionHwnd);
            SetCapture(m_selectionHwnd); // ensure we see drag across entire overlay
        }
        else
        {
            // Fallback: if creation fails, tear down capture state
            ReleaseScreenBitmap();
            m_isSelecting = false;
        }
    }

    // -------- Win32 overlay helpers --------

    void MainWindow::CaptureScreenBitmap()
    {
        ReleaseScreenBitmap();

        m_virtualOrigin.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        m_virtualOrigin.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        m_screenSize.cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        m_screenSize.cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        HDC screenDC = GetDC(nullptr);
        m_screenMemDC = CreateCompatibleDC(screenDC);
        m_screenBmp = CreateCompatibleBitmap(screenDC, m_screenSize.cx, m_screenSize.cy);
        HGDIOBJ old = SelectObject(m_screenMemDC, m_screenBmp);

        // Copy the entire virtual screen into our bitmap at (0,0)
        BitBlt(m_screenMemDC, 0, 0, m_screenSize.cx, m_screenSize.cy,
            screenDC, m_virtualOrigin.x, m_virtualOrigin.y, SRCCOPY);

        SelectObject(m_screenMemDC, old);
        ReleaseDC(nullptr, screenDC);
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
    }

    RECT MainWindow::MakeRectFromPoints(POINT a, POINT b) const
    {
        RECT r{};
        r.left = (std::min)(a.x, b.x);
        r.top = (std::min)(a.y, b.y);
        r.right = (std::max)(a.x, b.x);
        r.bottom = (std::max)(a.y, b.y);
        return r;
    }

    void MainWindow::OnSelectionCompleted(RECT sel)
    {
        // TODO: Replace this with whatever you want after selection
        // (e.g., capture region, open effect window, etc.)
        m_lastSelection = sel;

        // If you had temporarily changed your WinUI background, restore here as needed.
    }

    // -------- Overlay window proc --------

    LRESULT CALLBACK MainWindow::SelectionWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_NCCREATE:
        {
            auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<MainWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return TRUE;
        }
        case WM_SETCURSOR:
            // Ensure crosshair shows even over the client area
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;

        case WM_LBUTTONDOWN:
        {
            if (!self) break;
            self->m_isDragging = true;

            POINTS pts = MAKEPOINTS(lParam);
            self->m_ptStart = { pts.x, pts.y };
            self->m_ptEnd = self->m_ptStart;

            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            if (!self || !self->m_isDragging) break;
            POINTS pts = MAKEPOINTS(lParam);
            self->m_ptEnd = { pts.x, pts.y };
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            if (!self) break;
            ReleaseCapture();
            self->m_isDragging = false;

            POINTS pts = MAKEPOINTS(lParam);
            self->m_ptEnd = { pts.x, pts.y };

            // Compute selection rect in overlay-client coordinates
            RECT sel = self->MakeRectFromPoints(self->m_ptStart, self->m_ptEnd);

            // Convert to absolute (virtual screen) coordinates
            sel.left += self->m_virtualOrigin.x;
            sel.right += self->m_virtualOrigin.x;
            sel.top += self->m_virtualOrigin.y;
            sel.bottom += self->m_virtualOrigin.y;

            self->OnSelectionCompleted(sel);

            // Tear down the overlay + screenshot
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_KEYDOWN:
        {
            // Escape cancels selection
            if (wParam == VK_ESCAPE)
            {
                if (self) { self->m_isDragging = false; self->m_isSelecting = false; }
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }

        case WM_PAINT:
        {
            if (!self) break;

            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // 1) Draw the captured screen to give a "frozen" effect
            if (self->m_screenMemDC && self->m_screenBmp)
            {
                HGDIOBJ old = SelectObject(self->m_screenMemDC, self->m_screenBmp);
                BitBlt(hdc, 0, 0, self->m_screenSize.cx, self->m_screenSize.cy,
                    self->m_screenMemDC, 0, 0, SRCCOPY);
                SelectObject(self->m_screenMemDC, old);
            }

            // 2) Dim the view slightly via alpha (we already set WS_EX_LAYERED with ~160 alpha)

            // 3) Draw the red selection rectangle while dragging
            if (self->m_isDragging)
            {
                RECT r = self->MakeRectFromPoints(self->m_ptStart, self->m_ptEnd);

                // Use a red pen, 2px, transparent fill
                HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
                HGDIOBJ oldPen = SelectObject(hdc, pen);
                HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

                Rectangle(hdc, r.left, r.top, r.right, r.bottom);

                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(pen);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
        {
            if (self)
            {
                self->m_isSelecting = false;
                self->ReleaseScreenBitmap();
                self->m_selectionHwnd = nullptr;
            }
            return 0;
        }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
