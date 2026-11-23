#include "ui/wic_exporter.hpp"
#include "core/error_catalog.hpp"
#include "platform/win32_platform.hpp"

#ifdef _WIN32
#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#endif

namespace auralite {

Result<void> WicPngExporter::ExportClusterMapPng(const std::filesystem::path& targetPath,
                                                 const ClusterGrid& grid,
                                                 const ThemePalette& palette,
                                                 std::uint32_t widthPx,
                                                 std::uint32_t heightPx,
                                                 double dpi) {
    if (targetPath.empty() || targetPath.string().find("..") != std::string::npos) {
        return Result<void>::Failure(MakeError(ErrorCode::E_EXPORT_INVALID_PATH));
    }
#ifndef _WIN32
    (void)grid;
    (void)palette;
    (void)widthPx;
    (void)heightPx;
    (void)dpi;
    return Result<void>::Failure(MakeError(ErrorCode::E_EXPORT_WIC));
#else
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool didInit = SUCCEEDED(hr);

    IWICImagingFactory* wicFactory = nullptr;
    IWICBitmap* wicBitmap = nullptr;
    ID2D1Factory* d2dFactory = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;

    auto cleanup = [&]() {
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        if (rt) rt->Release();
        if (d2dFactory) d2dFactory->Release();
        if (wicBitmap) wicBitmap->Release();
        if (wicFactory) wicFactory->Release();
        if (didInit) ::CoUninitialize();
    };

    hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(&wicFactory));
    if (SUCCEEDED(hr)) {
        hr = wicFactory->CreateBitmap(widthPx, heightPx, GUID_WICPixelFormat32bppPBGRA,
                                      WICBitmapCacheOnDemand, &wicBitmap);
    }
    if (SUCCEEDED(hr)) {
        hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory);
    }
    if (SUCCEEDED(hr)) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi), static_cast<float>(dpi));
        hr = d2dFactory->CreateWicBitmapRenderTarget(wicBitmap, props, &rt);
    }
    if (SUCCEEDED(hr) && rt) {
        auto toD2D = [](RgbColor c) {
            return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
        };
        rt->BeginDraw();
        rt->Clear(toD2D(palette.panelBg));
        if (grid.cols > 0 && grid.rows > 0 && !grid.cells.empty()) {
            std::array<ID2D1SolidColorBrush*, kClusterCategoryCount> brushes{};
            for (std::size_t i = 0; i < kClusterCategoryCount; ++i) {
                rt->CreateSolidColorBrush(toD2D(palette.clusterColors[i]), &brushes[i]);
            }
            const float cellW = static_cast<float>(widthPx) / static_cast<float>(grid.cols);
            const float cellH = static_cast<float>(heightPx) / static_cast<float>(grid.rows);
            for (std::uint32_t r = 0; r < grid.rows; ++r) {
                for (std::uint32_t c = 0; c < grid.cols; ++c) {
                    const auto* cell = grid.CellAt(c, r);
                    if (!cell) continue;
                    if (auto* br = brushes[static_cast<std::size_t>(cell->dominant)]) {
                        rt->FillRectangle(D2D1::RectF(c * cellW, r * cellH, (c + 1) * cellW, (r + 1) * cellH), br);
                    }
                }
            }
            for (auto* b : brushes) if (b) b->Release();
        }
        hr = rt->EndDraw();
    }

    const auto tmpPath = std::filesystem::path(targetPath.wstring() + L".tmp");
    if (SUCCEEDED(hr)) {
        hr = wicFactory->CreateStream(&stream);
    }
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromFilename(tmpPath.c_str(), GENERIC_WRITE);
    }
    if (SUCCEEDED(hr)) {
        hr = wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(&frame, nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Initialize(nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->SetSize(widthPx, heightPx);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->SetResolution(dpi, dpi);
    }
    if (SUCCEEDED(hr)) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
        hr = frame->SetPixelFormat(&fmt);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->WriteSource(wicBitmap, nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
    }

    cleanup();
    if (FAILED(hr)) {
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return Result<void>::Failure(MakeError(ErrorCode::E_EXPORT_WIC, {}, 0, static_cast<std::uint32_t>(hr)));
    }
    std::error_code ec;
    std::filesystem::rename(tmpPath, targetPath, ec);
    if (ec) {
        std::filesystem::remove(tmpPath, ec);
        return Result<void>::Failure(MakeError(ErrorCode::E_EXPORT_IO));
    }
    return Result<void>::Success();
#endif
}

} // namespace auralite
