# ADR-002: Native C++20/Win32 UI with Direct2D + DirectWrite + WIC

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** Use native Win32 (`PerMonitorV2` DPI aware) with Direct2D offscreen double-buffered rendering (`D2DRenderer`), DirectWrite typography, and WIC (`WicPngExporter`) for PNG cluster-map export.
- **Rationale:** Zero external runtime UI framework dependencies, strict Apache-2.0 compliance, deterministic memory footprint, native accessibility via UIA/standard common controls (`WC_LISTVIEWW`, `WC_TABCONTROLW`).
## Context

The cluster map is the product's centrepiece: millions of clusters must render
at interactive rates, with PNG export and correct DPI behaviour. A UI toolkit
(Qt, .NET, Electron, a web view) is explicitly forbidden by R-004 — it would be
the only third-party runtime dependency in an otherwise dependency-free product.

## Consequences

- Zero redistributables. The executable needs only what Windows ships.
- Direct2D interops with WIC, so the same code that draws the screen encodes the
  PNG — export and display cannot disagree.
- Every control is ours to draw, which means **accessibility is ours too**: UI
  Automation annotations, keyboard navigation, focus rectangles and contrast
  are explicit work, not framework defaults. Tracked as R-086/UI-01.
- PMv2 correctness on Windows 10 1809 is assumed, not verified (V-12).
- Direct2D vs GDI+ performance is unmeasured (S-06). The chart data model is
  renderer-agnostic precisely so this can be revisited without rework.

## Alternatives rejected

| Option | Why rejected |
|---|---|
| GDI+ | Legacy, CPU-bound on multi-million-cluster bitmaps, weaker PMv2. Remains the fallback if S-06 fails. |
| Qt / .NET / Electron | Forbidden by R-004; would add the only runtime dependency. |
| Skia | Extra dependency and a licence to audit; no benefit once D2D/WIC are available. |
