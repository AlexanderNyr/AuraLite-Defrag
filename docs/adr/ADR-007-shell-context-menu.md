# ADR-007: Shell Context Menu Integration (Win10 Static Verb + Win11 Sparse MSIX)

- **Status:** Accepted
- **Date:** 2026-11-23
- **Decision:** Provide per-user HKCU static verbs (`ShellContextMenu::RegisterPerUserStaticVerb`) for Windows 10 and an `IExplorerCommand` COM implementation (`AuraLiteExplorerCommand`) paired with a sparse MSIX identity manifest (`packaging/msix/AppxManifest.xml`) for Windows 11 top-level menu placement.
## Context

Right-clicking a drive and choosing "Defragment" is the natural entry point, but
Windows 10 and Windows 11 use different menus, and the classic mechanism — an
in-process shell extension DLL — loads our code into `explorer.exe`.

## Consequences

- No shell extension DLL means no code of ours running inside Explorer. A crash
  in our DLL would otherwise take the user's desktop with it.
- Windows 10 gets a per-user HKCU static verb: no admin needed to register, easy
  to remove, no orphaned entries.
- Windows 11 gets an `IExplorerCommand` COM object plus a sparse MSIX identity
  manifest. The MSIX identity is the price of appearing in the modern menu.
- The same command line is invoked either way, so the shell path cannot diverge
  from the CLI path — including the policy gate. A context-menu defrag of an SSD
  must be refused exactly like a CLI defrag of an SSD.
- **Unverified on both OS versions** (S-08, V-14).
