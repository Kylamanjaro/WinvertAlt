#pragma once
#include "pch.h"
#include "Subscription.h"
#include <mutex>
#include <condition_variable>

// Forward decl
class DuplicationThread;

class EffectWindow : public ISubscriber
{
public:
    explicit EffectWindow(RECT desktopRect);
    ~EffectWindow();

    void Show();
    void Hide();

    // Called by DuplicationThread to render a frame
    void Render(ID3D11Texture2D* frame);

    // ISubscriber implementation (now a no-op, but required to compile)
    void OnFrameReady(::Microsoft::WRL::ComPtr<ID3D11Texture2D> texture) override;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void CreateAndShow();

    void EnsureSRVLocked_(ID3D11Texture2D* currentTex);
    void UpdateCBForRegion_();

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

    struct CBData { float scale[2]; float offset[2]; };

    ID3D11Texture2D* m_srvSourceRaw{ nullptr }; // track which texture SRV is built from

    // Threading
    std::atomic<bool> m_run{ false };

    // Source duplication thread (not owned)
    DuplicationThread* m_thread{ nullptr };
};
