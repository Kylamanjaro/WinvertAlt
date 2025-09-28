#pragma once
#include "pch.h"
#include "Subscription.h"
#include "EffectSettings.h"
#include <mutex>
#include <condition_variable>

// Forward decl
class DuplicationThread;
class OutputManager;

class EffectWindow : public ISubscriber
{
public:
    explicit EffectWindow(RECT desktopRect, OutputManager* outputManager);
    ~EffectWindow();

    void Show();
    void Hide();
    void UpdateSettings(const EffectSettings& settings);

    // Called by DuplicationThread to render a frame
    void Render(ID3D11Texture2D* frame, unsigned long long lastPresentQpc);

    // ISubscriber implementation (now a no-op, but required to compile)
    void OnFrameReady(::Microsoft::WRL::ComPtr<ID3D11Texture2D> texture) override;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void CreateAndShow();

    void EnsureSRVLocked_(ID3D11Texture2D* currentTex);
    void UpdateCBs_();

private:
    // Geometry/placement
    RECT m_desktopRect{};
    HWND m_hwnd{};

    // Device & DXGI
    ::Microsoft::WRL::ComPtr<ID3D11Device>        m_d3d;
    ::Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_deferredCtx; // Per-thread deferred context
    ::Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_immediateCtx; // Shared immediate context
    ::Microsoft::WRL::ComPtr<IDXGIFactory2>       m_factory;
    ::Microsoft::WRL::ComPtr<IDXGISwapChain1>     m_swapChain;

    // Pipeline
    ::Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    ::Microsoft::WRL::ComPtr<ID3D11VertexShader>      m_vs;
    ::Microsoft::WRL::ComPtr<ID3D11PixelShader>       m_ps;
    ::Microsoft::WRL::ComPtr<ID3D11InputLayout>       m_il;
    ::Microsoft::WRL::ComPtr<ID3D11Buffer>            m_vb;
    ::Microsoft::WRL::ComPtr<ID3D11Buffer>            m_cb;
    ::Microsoft::WRL::ComPtr<ID3D11SamplerState>      m_samp;
    ::Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;

    struct VertexCB { float scale[2]; float offset[2]; };
    static constexpr uint32_t kMaxColorMaps = 64;
    struct PixelCB {
        uint32_t enableInvert;
        uint32_t enableGrayscale;
        uint32_t enableMatrix;
        uint32_t enableColorMap;
        float colorMat[16];
        float colorOffset[4];
        uint32_t colorMapCount;
        float _pad1[3];
        // Pack src RGB in xyz and tolerance^2 in w
        float colorMapSrc[kMaxColorMaps][4];
        // Pack dst RGB in xyz; w unused
        float colorMapDst[kMaxColorMaps][4];
    };
    ::Microsoft::WRL::ComPtr<ID3D11Buffer> m_pixelCb;
    EffectSettings m_settings{};

    ID3D11Texture2D* m_srvSourceRaw{ nullptr }; // track which texture SRV is built from

    // Threading
    std::atomic<bool> m_run{ false };

    // Source duplication thread (not owned)
    DuplicationThread* m_thread{ nullptr };

    // Owning manager (not owned)
    OutputManager* m_outputManager{ nullptr };

    // D2D/DirectWrite for overlays
    ::Microsoft::WRL::ComPtr<ID2D1Factory1>      m_d2dFactory;
    ::Microsoft::WRL::ComPtr<IDWriteFactory>     m_dwriteFactory;
    ::Microsoft::WRL::ComPtr<IDWriteTextFormat>  m_textFormat;
    ::Microsoft::WRL::ComPtr<ID2D1Device>        m_d2dDevice;
    ::Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2dCtx;
    ::Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;

    // FPS tracking
    LARGE_INTEGER m_qpcFreq{};
    unsigned long long m_prevDupPresentQpc{ 0 };
    float  m_fps{ 0.0f };
    double m_fpsAccum{ 0.0 };
    int    m_fpsFrames{ 0 };

    // GPU timing (our draw cost)
    struct GpuTimerSlot {
        ::Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        ::Microsoft::WRL::ComPtr<ID3D11Query> start;
        ::Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool inFlight{ false };
    } m_gpuTimer[8];
    int   m_gpuTimerIndex{ 0 };
    double m_gpuMsLast{ 0.0 };
    float  m_procFps{ 0.0f };

    // Brightness protection (GPU-averaged luminance)
    ::Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_mipTex;          // region-sized, mipped, SRV|RTV, GENERATE_MIPS
    ::Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_mipSrv;
    ::Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_mipReadback1x1;  // staging 1x1
    UINT m_mipLastLevel{ 0 };
    float m_avgLuma{ 0.0f };
    bool  m_effectiveInvert{ false };
};
