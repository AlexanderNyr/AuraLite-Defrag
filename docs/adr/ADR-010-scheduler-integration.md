# ADR-010: Scheduler Integration via Task Scheduler 2.0 COM

- **Status:** Accepted (implementation partially complete)
- **Date:** 2026-11-24
- **Context:** Users want defragmentation to run outside working hours. The
  options were: a Windows service, a scheduled task via the Task Scheduler 2.0
  COM API (`taskschd.h`), or a self-managed timer in the application.

## Decision

Use **Task Scheduler 2.0** through its COM interface. No service, no custom
daemon, no registry Run-key.

## Rationale

- **Elevation is per-task, not global.** A task can be registered with
  `TASK_RUNLEVEL_HIGHEST`, so the operation runs elevated without the GUI ever
  running elevated. A service would hold a privileged process alive permanently.
- **Missed-run policy is built in.** `TASK_INSTANCE_RUN_ONLY_IF_IDLE` and the
  Start-When-Available setting give the OS responsibility for catch-up, which is
  exactly what a background maintenance job needs.
- **Battery and idle conditions** (`TASK_COND_IDLE`, `TASK_COND_DISABLE_ON_BATTERY`)
  are provided by the scheduler rather than reimplemented and got wrong.
- **The user can see and delete the task** in `taskschd.msc`. A service would be
  invisible maintenance the user cannot audit — unacceptable for a tool that
  moves their data.

## Consequences

- The scheduled entry point must enforce the *same* `PolicyGate` as the GUI and
  CLI. SSD refusal, pre-flight checks and exclusions are not optional because
  the invocation came from the scheduler. `P8-T09` exists for this reason.
- Task registration requires elevation; the *operation* therefore runs elevated
  but the interactive app need not.
- COM apartment threading must be initialised (`CoInitializeEx`) on whichever
  thread registers the task.
- Verification is Windows-only. The COM wrapper (`src/scheduler/`) is compiled
  under `_WIN32` and is not exercised by the Linux test suite.

## Alternatives rejected

| Option | Why rejected |
|---|---|
| Windows service | Permanent privileged process; invisible to the user; needs its own IPC and update path. |
| `Run` registry key | No elevation control, no conditions, no missed-run policy. |
| In-app timer | Dies with the app; cannot survive reboot or logoff. |
