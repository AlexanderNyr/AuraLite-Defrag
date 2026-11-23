# ADR-002: Native C++20/Win32 UI with Direct2D + DirectWrite + WIC

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** Use native Win32 (`PerMonitorV2` DPI aware) with Direct2D offscreen double-buffered rendering (`D2DRenderer`), DirectWrite typography, and WIC (`WicPngExporter`) for PNG cluster-map export.
- **Rationale:** Zero external runtime UI framework dependencies, strict Apache-2.0 compliance, deterministic memory footprint, native accessibility via UIA/standard common controls (`WC_LISTVIEWW`, `WC_TABCONTROLW`).
