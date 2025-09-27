#include "pch.h"
#include "MainWindow.xaml.h"
#include "Log.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

using namespace winrt;

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
        winvert4::Log("MainWindow: ToggleSnipping");
        StartScreenSelection();
    }

    void MainWindow::StartScreenSelection()
    {
        if (m_isSelecting) return;
        m_isSelecting = true;
        winvert4::Log("MainWindow: StartScreenSelection");

        CaptureScreenBitmap();

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
            winvert4::Logf("MainWindow: selection HWND=%p rect=(%d,%d %dx%d)", (void*)m_selectionHwnd, vx, vy, vw, vh);
            SetLayeredWindowAttributes(m_selectionHwnd, 0, 160, LWA_ALPHA);
            ShowWindow(m_selectionHwnd, SW_SHOW);
            SetForegroundWindow(m_selectionHwnd);
            SetCapture(m_selectionHwnd);
        }
        else
        {
            winvert4::Log("MainWindow: selection CreateWindowExW failed");
            ReleaseScreenBitmap();
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

        // 3) Create and show the effect window AFTER overlay is gone
        auto newWindow = std::make_unique<EffectWindow>(sel);
        newWindow->Show();
        m_effectWindows.push_back(std::move(newWindow));
    }

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
            winvert4::Log("MainWindow: selection WM_LBUTTONDOWN");
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
                if (self) { self->m_isDragging = false; self->m_isSelecting = false; }
                DestroyWindow(hwnd);
                winvert4::Log("MainWindow: selection cancelled via ESC");
                return 0;
            }
            break;

        case WM_PAINT:
        {
            if (!self) break;

            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            if (self->m_screenMemDC && self->m_screenBmp)
            {
                HGDIOBJ old = SelectObject(self->m_screenMemDC, self->m_screenBmp);
                BitBlt(hdc, 0, 0, self->m_screenSize.cx, self->m_screenSize.cy,
                       self->m_screenMemDC, 0, 0, SRCCOPY);
                SelectObject(self->m_screenMemDC, old);
            }

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
                winvert4::Log("MainWindow: selection overlay destroyed");
            }
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
