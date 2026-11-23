# ADR-007: Shell Context Menu Integration (Win10 Static Verb + Win11 Sparse MSIX)

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** Provide per-user HKCU static verbs (`ShellContextMenu::RegisterPerUserStaticVerb`) for Windows 10 and an `IExplorerCommand` COM implementation (`AuraLiteExplorerCommand`) paired with a sparse MSIX identity manifest (`packaging/msix/AppxManifest.xml`) for Windows 11 top-level menu placement.
