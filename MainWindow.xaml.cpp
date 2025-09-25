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

namespace winrt::Winvert4::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
    }

    int32_t MainWindow::MyProperty() { return 0; }
    void MainWindow::MyProperty(int32_t) {}

    void MainWindow::ToggleSnipping()
    {
        StartScreenSelection();
    }

    void MainWindow::StartScreenSelection()
    {
        if (m_isSelecting) return;
        m_isSelecting = true;

        // 1) Capture the entire virtual screen once (freeze effect)
        CaptureScreenBitmap();

        // 2) Register overlay class with crosshair cursor
        WNDCLASSEXW wcex{ sizeof(WNDCLASSEXW) };
        wcex.style         = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc   = &MainWindow::SelectionWndProc;
        wcex.hInstance     = GetModuleHandleW(nullptr);
        wcex.hCursor       = LoadCursorW(nullptr, IDC_CROSS);
        wcex.lpszClassName = kSelectionWndClass;
        wcex.hbrBackground = nullptr;

        static ATOM s_atom = 0;
        if (!s_atom) s_atom = RegisterClassExW(&wcex);

        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        // 3) Create topmost, layered overlay across all monitors
        m_selectionHwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            kSelectionWndClass,
            L"Winvert Selection",
            WS_POPUP,
            vx, vy, vw, vh,
            nullptr, nullptr, GetModuleHandleW(nullptr),
            this);

        if (m_selectionHwnd)
        {
            SetLayeredWindowAttributes(m_selectionHwnd, 0, 160, LWA_ALPHA); // ~62% alpha
            ShowWindow(m_selectionHwnd, SW_SHOW);
            SetForegroundWindow(m_selectionHwnd);
            SetCapture(m_selectionHwnd);
        }
        else
        {
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
        m_screenSize.cx   = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        m_screenSize.cy   = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        HDC screenDC = GetDC(nullptr);
        m_screenMemDC = CreateCompatibleDC(screenDC);
        m_screenBmp   = CreateCompatibleBitmap(screenDC, m_screenSize.cx, m_screenSize.cy);
        HGDIOBJ old   = SelectObject(m_screenMemDC, m_screenBmp);

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
        r.left   = (std::min)(a.x, b.x);
        r.top    = (std::min)(a.y, b.y);
        r.right  = (std::max)(a.x, b.x);
        r.bottom = (std::max)(a.y, b.y);
        return r;
    }

    void MainWindow::OnSelectionCompleted(RECT sel)
    {
        // TODO: your post-selection action
        m_lastSelection = sel;
    }

    // -------- Overlay WndProc --------

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
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;

        case WM_LBUTTONDOWN:
        {
            if (!self) break;
            self->m_isDragging = true;

            POINTS pts = MAKEPOINTS(lParam);
            self->m_ptStart = { pts.x, pts.y };
            self->m_ptEnd   = self->m_ptStart;

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

            RECT sel = self->MakeRectFromPoints(self->m_ptStart, self->m_ptEnd);

            // Convert to absolute virtual-screen coordinates
            sel.left   += self->m_virtualOrigin.x;
            sel.right  += self->m_virtualOrigin.x;
            sel.top    += self->m_virtualOrigin.y;
            sel.bottom += self->m_virtualOrigin.y;

            self->OnSelectionCompleted(sel);

            DestroyWindow(hwnd); // tear down overlay
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                if (self) { self->m_isDragging = false; self->m_isSelecting = false; }
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_PAINT:
        {
            if (!self) break;

            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Draw frozen desktop
            if (self->m_screenMemDC && self->m_screenBmp)
            {
                HGDIOBJ old = SelectObject(self->m_screenMemDC, self->m_screenBmp);
                BitBlt(hdc, 0, 0, self->m_screenSize.cx, self->m_screenSize.cy,
                       self->m_screenMemDC, 0, 0, SRCCOPY);
                SelectObject(self->m_screenMemDC, old);
            }

            // Draw red rectangle
            if (self->m_isDragging)
            {
                RECT r = self->MakeRectFromPoints(self->m_ptStart, self->m_ptEnd);

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
            if (self)
            {
                self->m_isSelecting = false;
                self->ReleaseScreenBitmap();
                self->m_selectionHwnd = nullptr;
            }
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
