#include "scheduler/task_scheduler.hpp"
#include "core/error_catalog.hpp"
#include "platform/win32_platform.hpp"

#ifdef _WIN32
#include <windows.h>
#include <taskschd.h>
#include <comutil.h>
#endif

namespace auralite {

Result<void> TaskSchedulerManager::RegisterTask(const ScheduledTaskSpec& spec) {
    // Rule: never schedule a defrag task on SolidState or Unknown media (R-016 / P8-T09)
    if (IsDefragBlockedMedia(spec.mediaClass)) {
        return Result<void>::Failure(MakeError(ErrorCode::E_PREFLIGHT_SSD_BLOCKED, spec.volumeId));
    }
    if (spec.mode == DefragMode::MftOptimize || spec.mode == DefragMode::MftZoneEvacuate) {
        // P5-T06: no scheduled MFT operations in v1
        return Result<void>::Failure(MakeError(ErrorCode::E_INTERNAL_POLICY_VIOLATION, spec.volumeId));
    }
#ifndef _WIN32
    return Result<void>::Failure(MakeError(ErrorCode::E_INTERNAL_INVALID_STATE, spec.volumeId));
#else
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didInit = SUCCEEDED(hr);

    ITaskService* pService = nullptr;
    hr = ::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                            IID_ITaskService, reinterpret_cast<void**>(&pService));
    if (FAILED(hr) || !pService) {
        if (didInit) ::CoUninitialize();
        return Result<void>::Failure(MakeError(ErrorCode::E_INTERNAL_INVALID_STATE, spec.volumeId));
    }

    VARIANT empty{};
    ::VariantInit(&empty);
    hr = pService->Connect(empty, empty, empty, empty);

    ITaskFolder* pRootFolder = nullptr;
    if (SUCCEEDED(hr)) {
        BSTR rootPath = ::SysAllocString(L"\\");
        hr = pService->GetFolder(rootPath, &pRootFolder);
        ::SysFreeString(rootPath);
    }

    ITaskDefinition* pTask = nullptr;
    if (SUCCEEDED(hr)) {
        hr = pService->NewTask(0, &pTask);
    }

    if (SUCCEEDED(hr) && pTask) {
        IPrincipal* pPrincipal = nullptr;
        if (SUCCEEDED(pTask->get_Principal(&pPrincipal)) && pPrincipal) {
            // S-14: highest run level, interactive token, NO stored password
            pPrincipal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
            pPrincipal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
            pPrincipal->Release();
        }

        ITriggerCollection* pTriggers = nullptr;
        if (SUCCEEDED(pTask->get_Triggers(&pTriggers)) && pTriggers) {
            ITrigger* pTrigger = nullptr;
            TASK_TRIGGER_TYPE2 ttype = TASK_TRIGGER_WEEKLY;
            if (spec.cadence == ScheduleCadence::Daily) ttype = TASK_TRIGGER_DAILY;
            else if (spec.cadence == ScheduleCadence::OnIdle) ttype = TASK_TRIGGER_IDLE;
            if (SUCCEEDED(pTriggers->Create(ttype, &pTrigger)) && pTrigger) {
                BSTR startB = ::SysAllocString(Win32Platform::Utf8ToWide(spec.startBoundaryIso).c_str());
                pTrigger->put_StartBoundary(startB);
                ::SysFreeString(startB);
                pTrigger->Release();
            }
            pTriggers->Release();
        }

        IActionCollection* pActions = nullptr;
        if (SUCCEEDED(pTask->get_Actions(&pActions)) && pActions) {
            IAction* pAction = nullptr;
            if (SUCCEEDED(pActions->Create(TASK_ACTION_EXEC, &pAction)) && pAction) {
                IExecAction* pExec = nullptr;
                if (SUCCEEDED(pAction->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&pExec))) && pExec) {
                    BSTR exeB = ::SysAllocString(Win32Platform::Utf8ToWide(spec.cliExecutablePath).c_str());
                    const std::string verb = (spec.mode == DefragMode::QuickDefrag) ? "quick" : "defrag";
                    const std::string args = verb + " --volume " + spec.volumeId;
                    BSTR argsB = ::SysAllocString(Win32Platform::Utf8ToWide(args).c_str());
                    pExec->put_Path(exeB);
                    pExec->put_Arguments(argsB);
                    ::SysFreeString(exeB);
                    ::SysFreeString(argsB);
                    pExec->Release();
                }
                pAction->Release();
            }
            pActions->Release();
        }

        IRegisteredTask* pRegistered = nullptr;
        BSTR nameB = ::SysAllocString(Win32Platform::Utf8ToWide(spec.taskName).c_str());
        hr = pRootFolder->RegisterTaskDefinition(
            nameB, pTask, TASK_CREATE_OR_UPDATE, empty, empty,
            TASK_LOGON_INTERACTIVE_TOKEN, empty, &pRegistered);
        ::SysFreeString(nameB);
        if (pRegistered) pRegistered->Release();
    }

    if (pTask) pTask->Release();
    if (pRootFolder) pRootFolder->Release();
    pService->Release();
    if (didInit) ::CoUninitialize();

    if (FAILED(hr)) {
        return Result<void>::Failure(MakeError(ErrorCode::E_INTERNAL_INVALID_STATE, spec.volumeId, 0, static_cast<std::uint32_t>(hr)));
    }
    return Result<void>::Success();
#endif
}

Result<void> TaskSchedulerManager::RemoveTask(const std::string& taskName) {
#ifndef _WIN32
    (void)taskName;
    return Result<void>::Failure(MakeError(ErrorCode::E_INTERNAL_INVALID_STATE));
#else
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didInit = SUCCEEDED(hr);
    ITaskService* pService = nullptr;
    hr = ::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                            IID_ITaskService, reinterpret_cast<void**>(&pService));
    if (SUCCEEDED(hr) && pService) {
        VARIANT empty{};
        ::VariantInit(&empty);
        hr = pService->Connect(empty, empty, empty, empty);
        ITaskFolder* pRootFolder = nullptr;
        if (SUCCEEDED(hr)) {
            BSTR rootPath = ::SysAllocString(L"\\");
            hr = pService->GetFolder(rootPath, &pRootFolder);
            ::SysFreeString(rootPath);
        }
        if (SUCCEEDED(hr) && pRootFolder) {
            BSTR nameB = ::SysAllocString(Win32Platform::Utf8ToWide(taskName).c_str());
            hr = pRootFolder->DeleteTask(nameB, 0);
            ::SysFreeString(nameB);
            pRootFolder->Release();
        }
        pService->Release();
    }
    if (didInit) ::CoUninitialize();
    if (FAILED(hr)) {
        return Result<void>::Failure(MakeError(ErrorCode::E_INTERNAL_INVALID_STATE));
    }
    return Result<void>::Success();
#endif
}

} // namespace auralite
