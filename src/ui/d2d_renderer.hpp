#pragma once

#include "reporting/cluster_aggregator.hpp"
#include "reporting/statistics.hpp"
#include "ui/theme.hpp"

#ifdef _WIN32
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#endif

namespace auralite {

class D2DRenderer {
public:
    D2DRenderer() = default;
    ~D2DRenderer();

#ifdef _WIN32
    HRESULT Initialize(HWND hwnd);
    void Resize(UINT width, UINT height);
    void DiscardResources();
    void RenderClusterMap(HWND hwnd,
                          const ClusterGrid& grid,
                          const ThemePalette& palette,
                          const std::wstring& bannerText,
                          int zoomFactor);
    ID2D1Factory* Factory() const noexcept { return factory_; }
#endif

private:
#ifdef _WIN32
    HRESULT EnsureDeviceResources(HWND hwnd);
    static D2D1_COLOR_F ToD2D(RgbColor c) noexcept;

    ID2D1Factory* factory_{nullptr};
    IDWriteFactory* dwriteFactory_{nullptr};
    IDWriteTextFormat* textFormat_{nullptr};
    ID2D1HwndRenderTarget* hwndTarget_{nullptr};
    ID2D1BitmapRenderTarget* backBuffer_{nullptr}; // Double-buffered offscreen bitmap (R-075)
#endif
};

} // namespace auralite
