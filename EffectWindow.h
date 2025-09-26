#pragma once
#include "pch.h"
#include "Subscription.h"
#include <mutex>
#include <condition_variable>

class EffectWindow : public ISubscriber
{
public:
    EffectWindow(RECT desktopRect);
    ~EffectWindow();

    void Show();
    void Hide();

    // ISubscriber interface
    void OnFrameReady(::Microsoft::WRL::ComPtr<ID3D11Texture2D> texture) override;

private:
    void RenderThreadProc();

    HWND m_hwnd{ nullptr };
    RECT m_desktopRect{ 0,0,0,0 };

    // D3D11 resources
    ::Microsoft::WRL::ComPtr<ID3D11Device> m_d3d;
    ::Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
    ::Microsoft::WRL::ComPtr<IDXGIFactory2> m_factory;
    ::Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;

    // Pipeline
    ::Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    ::Microsoft::WRL::ComPtr<ID3D11PixelShader>  m_ps;
    ::Microsoft::WRL::ComPtr<ID3D11InputLayout>  m_il;
    ::Microsoft::WRL::ComPtr<ID3D11Buffer>       m_vb;
    ::Microsoft::WRL::ComPtr<ID3D11Buffer>       m_cb;
    ::Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samp;

    struct CBData { float scale[2]; float offset[2]; };

    std::thread m_renderThread;
    std::atomic<bool> m_run{ false };

    // Threading
    std::mutex m_textureMutex;
    std::condition_variable m_textureCv;
    ::Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sharedTexture;
    ::Microsoft::WRL::ComPtr<ID3D11Texture2D> m_lastTexture;
    // Backref to duplication thread for synchronizing immediate context usage
    class DuplicationThread* m_thread{ nullptr };
};

