#include "ui/d2d_renderer.hpp"

namespace auralite {

D2DRenderer::~D2DRenderer() {
#ifdef _WIN32
    DiscardResources();
    if (textFormat_) { textFormat_->Release(); textFormat_ = nullptr; }
    if (dwriteFactory_) { dwriteFactory_->Release(); dwriteFactory_ = nullptr; }
    if (factory_) { factory_->Release(); factory_ = nullptr; }
#endif
}

#ifdef _WIN32

D2D1_COLOR_F D2DRenderer::ToD2D(RgbColor c) noexcept {
    return D2D1::ColorF(static_cast<float>(c.r) / 255.0f,
                        static_cast<float>(c.g) / 255.0f,
                        static_cast<float>(c.b) / 255.0f,
                        1.0f);
}

HRESULT D2DRenderer::Initialize(HWND hwnd) {
    HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_);
    if (FAILED(hr)) return hr;
    hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(&dwriteFactory_));
    if (SUCCEEDED(hr) && dwriteFactory_) {
        dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr,
                                         DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", &textFormat_);
    }
    return EnsureDeviceResources(hwnd);
}

HRESULT D2DRenderer::EnsureDeviceResources(HWND hwnd) {
    if (hwndTarget_) return S_OK;
    if (!factory_) return E_FAIL;
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(rc.right - rc.left > 0 ? rc.right - rc.left : 1),
        static_cast<UINT32>(rc.bottom - rc.top > 0 ? rc.bottom - rc.top : 1));
    HRESULT hr = factory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd, size),
        &hwndTarget_);
    if (SUCCEEDED(hr) && hwndTarget_) {
        hr = hwndTarget_->CreateCompatibleRenderTarget(&backBuffer_);
    }
    return hr;
}

void D2DRenderer::DiscardResources() {
    if (backBuffer_) { backBuffer_->Release(); backBuffer_ = nullptr; }
    if (hwndTarget_) { hwndTarget_->Release(); hwndTarget_ = nullptr; }
}

void D2DRenderer::Resize(UINT width, UINT height) {
    if (hwndTarget_) {
        hwndTarget_->Resize(D2D1::SizeU(width > 0 ? width : 1, height > 0 ? height : 1));
        if (backBuffer_) { backBuffer_->Release(); backBuffer_ = nullptr; }
        hwndTarget_->CreateCompatibleRenderTarget(&backBuffer_);
    }
}

void D2DRenderer::RenderClusterMap(HWND hwnd,
                                   const ClusterGrid& grid,
                                   const ThemePalette& palette,
                                   const std::wstring& bannerText,
                                   int zoomFactor) {
    if (FAILED(EnsureDeviceResources(hwnd)) || !hwndTarget_) return;
    ID2D1RenderTarget* rt = backBuffer_ ? static_cast<ID2D1RenderTarget*>(backBuffer_)
                                        : static_cast<ID2D1RenderTarget*>(hwndTarget_);

    const D2D1_SIZE_F sz = rt->GetSize();
    rt->BeginDraw();
    rt->Clear(ToD2D(palette.panelBg));

    float topOffset = 8.0f;
    if (!bannerText.empty() && textFormat_) {
        ID2D1SolidColorBrush* warnBg = nullptr;
        ID2D1SolidColorBrush* warnTxt = nullptr;
        rt->CreateSolidColorBrush(ToD2D(palette.warningBg), &warnBg);
        rt->CreateSolidColorBrush(ToD2D(palette.warningText), &warnTxt);
        if (warnBg && warnTxt) {
            D2D1_RECT_F bRect = D2D1::RectF(8.0f, 6.0f, sz.width - 8.0f, 30.0f);
            rt->FillRectangle(bRect, warnBg);
            rt->DrawText(bannerText.c_str(), static_cast<UINT32>(bannerText.size()),
                         textFormat_, D2D1::RectF(14.0f, 8.0f, sz.width - 14.0f, 28.0f), warnTxt);
            topOffset = 36.0f;
        }
        if (warnBg) warnBg->Release();
        if (warnTxt) warnTxt->Release();
    }

    if (grid.cols > 0 && grid.rows > 0 && !grid.cells.empty()) {
        std::array<ID2D1SolidColorBrush*, kClusterCategoryCount> brushes{};
        for (std::size_t i = 0; i < kClusterCategoryCount; ++i) {
            rt->CreateSolidColorBrush(ToD2D(palette.clusterColors[i]), &brushes[i]);
        }

        const float pad = 8.0f;
        const float mapW = (sz.width - 2.0f * pad) > 10.0f ? (sz.width - 2.0f * pad) : 10.0f;
        const float mapH = (sz.height - topOffset - 28.0f) > 10.0f ? (sz.height - topOffset - 28.0f) : 10.0f;

        const std::uint32_t visCols = std::max(1U, grid.cols / static_cast<std::uint32_t>(std::max(1, zoomFactor)));
        const std::uint32_t visRows = std::max(1U, grid.rows / static_cast<std::uint32_t>(std::max(1, zoomFactor)));
        const float cellW = mapW / static_cast<float>(visCols);
        const float cellH = mapH / static_cast<float>(visRows);

        for (std::uint32_t r = 0; r < visRows; ++r) {
            for (std::uint32_t c = 0; c < visCols; ++c) {
                const auto* cell = grid.CellAt(c, r);
                if (!cell) continue;
                auto* br = brushes[static_cast<std::size_t>(cell->dominant)];
                if (!br) continue;
                const float x0 = pad + static_cast<float>(c) * cellW;
                const float y0 = topOffset + static_cast<float>(r) * cellH;
                rt->FillRectangle(D2D1::RectF(x0, y0, x0 + cellW - 0.5f, y0 + cellH - 0.5f), br);
            }
        }

        for (auto* b : brushes) {
            if (b) b->Release();
        }
    }

    HRESULT hr = rt->EndDraw();
    if (backBuffer_) {
        ID2D1Bitmap* bmp = nullptr;
        if (SUCCEEDED(backBuffer_->GetBitmap(&bmp)) && bmp) {
            hwndTarget_->BeginDraw();
            hwndTarget_->DrawBitmap(bmp, D2D1::RectF(0, 0, sz.width, sz.height));
            hr = hwndTarget_->EndDraw();
            bmp->Release();
        }
    }
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardResources();
    }
}

#endif

} // namespace auralite
