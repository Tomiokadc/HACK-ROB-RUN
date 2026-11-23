#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

struct FrameBuffer {
    Microsoft::WRL::ComPtr<ID3D11Device> gpu;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap;
    Microsoft::WRL::ComPtr<IDCompositionDevice> composition;
    Microsoft::WRL::ComPtr<IDCompositionTarget> windowTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> font;
    LONG width{}, height{};
    unsigned maxFps = 60;
    float maxDistance = 500.f;
    double submitMs{};
    bool fontFallback{};

    bool Init(HWND window) {
        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        ComPtr<ID2D1Device> d2d;
        ComPtr<IDWriteFactory> text;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                &gpu, nullptr, nullptr)) ||
            FAILED(gpu.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = desc.Height = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(factory->CreateSwapChainForComposition(gpu.Get(), &desc, nullptr, &swap)) ||
            FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&composition))) ||
            FAILED(composition->CreateTargetForHwnd(window, TRUE, &windowTarget)) ||
            FAILED(composition->CreateVisual(&visual)) ||
            FAILED(visual->SetContent(swap.Get())) || FAILED(windowTarget->SetRoot(visual.Get())) ||
            FAILED(composition->Commit()) ||
            FAILED(D2D1CreateDevice(dxgi.Get(), nullptr, &d2d)) ||
            FAILED(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) ||
            FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush)) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(text.GetAddressOf())))) return false;

        wchar_t config[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, config, MAX_PATH);
        if (!length || length >= MAX_PATH) return false;
        auto* filename = wcsrchr(config, L'\\');
        if (!filename || wcscpy_s(filename + 1, MAX_PATH - (filename + 1 - config), L"RobRunESP.ini")) return false;
        const UINT fps = GetPrivateProfileIntW(L"Overlay", L"MaxFPS", 60, config);
        maxFps = fps >= 15 && fps <= 240 ? fps : 60;
        const UINT distance = GetPrivateProfileIntW(L"Overlay", L"MaxDistance", 500, config);
        maxDistance = distance >= 1 && distance <= 10000 ? static_cast<float>(distance) : 500.f;
        wchar_t family[128]{};
        const float size = static_cast<float>(GetPrivateProfileIntW(L"Font", L"Size", 16, config));
        GetPrivateProfileStringW(L"Font", L"Family", L"Segoe UI", family, 128, config);
        wchar_t fontPath[MAX_PATH]{};
        GetPrivateProfileStringW(L"Font", L"File", L"", fontPath, MAX_PATH, config);
        ComPtr<IDWriteFontCollection1> collection;
        if (*fontPath) {
            ComPtr<IDWriteFactory5> customFactory;
            ComPtr<IDWriteFontFile> file;
            ComPtr<IDWriteFontSetBuilder1> builder;
            ComPtr<IDWriteFontSet> set;
            if (FAILED(text.As(&customFactory)) ||
                FAILED(customFactory->CreateFontFileReference(fontPath, nullptr, &file)) ||
                FAILED(customFactory->CreateFontSetBuilder(&builder)) ||
                FAILED(builder->AddFontFile(file.Get())) ||
                FAILED(builder->CreateFontSet(&set)) ||
                FAILED(customFactory->CreateFontCollectionFromFontSet(set.Get(), &collection))) collection.Reset();
        }
        const float fontSize = size >= 8 && size <= 48 ? size : 16;
        if ((!*fontPath || collection) && SUCCEEDED(text->CreateTextFormat(family, collection.Get(),
                DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", &font))) return true;
        fontFallback = true;
        OutputDebugStringW(L"RobRunESP: configured font unavailable; using Segoe UI.\n");
        return SUCCEEDED(text->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"", &font));
    }

    bool Resize(LONG w, LONG h) {
        if (w <= 0 || h <= 0) return false;
        if (width == w && height == h) return true;
        dc->SetTarget(nullptr);
        width = height = 0;
        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> target;
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)) ||
            FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&surface))) ||
            FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &target))) return false;
        dc->SetTarget(target.Get());
        dc->SetDpi(96, 96);
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        width = w;
        height = h;
        return true;
    }

    void Text(const wchar_t* value, float x, float y) {
        const auto rect = D2D1::RectF(x, y, x + 550.f, y + 60.f);
        brush->SetColor(D2D1::ColorF(0x111820));
        dc->DrawText(value, lstrlenW(value), font.Get(),
            D2D1::RectF(rect.left + 1, rect.top + 1, rect.right + 1, rect.bottom + 1), brush.Get());
        brush->SetColor(D2D1::ColorF(0xE6EFF4));
        dc->DrawText(value, lstrlenW(value), font.Get(), rect, brush.Get());
    }
};



