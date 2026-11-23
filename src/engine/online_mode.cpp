#include "engine/online_mode.hpp"
#include "core/error_catalog.hpp"

#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace auralite {

OnlineMode::OnlineMode(IVolumeProvider& volume,
                       IFileSystemAdapter& fs,
                       const PolicyGate& policy,
                       StructuredLogger& logger,
                       bool isElevated,
                       bool verifyChecksumsSample)
    : volume_(volume),
      fs_(fs),
      policy_(policy),
      logger_(logger),
      isElevated_(isElevated),
      verifyChecksumsSample_(verifyChecksumsSample) {}

CapabilityBits OnlineMode::Caps() const {
    return fs_.Capabilities();
}

bool OnlineMode::VerifyMovedExtent(FileId fileId,
                                   Vcn startVcn,
                                   Lcn expectedTargetLcn,
                                   ClusterCount count,
                                   std::uint64_t expectedChecksum,
                                   bool checkChecksum,
                                   ErrorCode& outVerifyError) {
    auto extRes = fs_.Extents(fileId);
    if (!extRes) {
        outVerifyError = ErrorCode::E_VERIFY_READBACK_FAILED;
        return false;
    }

    bool covered = false;
    const Vcn targetEndVcn = startVcn + count;
    for (const auto& ext : extRes.Value()) {
        const Vcn extEndVcn = ext.startVcn + ext.clusterCount;
        if (ext.startVcn <= startVcn && extEndVcn >= targetEndVcn) {
            const Lcn offset = startVcn - ext.startVcn;
            if (ext.startLcn + offset == expectedTargetLcn) {
                covered = true;
                break;
            }
        }
    }
    if (!covered) {
        outVerifyError = ErrorCode::E_VERIFY_EXTENT_MISMATCH;
        return false;
    }

    if (checkChecksum && expectedChecksum != 0) {
        auto csum = fs_.ComputeWindowChecksum(fileId);
        if (!csum || csum.Value() != expectedChecksum) {
            outVerifyError = ErrorCode::E_VERIFY_CHECKSUM_MISMATCH;
            return false;
        }
    }

    outVerifyError = ErrorCode::Ok;
    return true;
}

Result<void> OnlineMode::Execute(const DefragPlan& plan,
                                 IProgressSink& sink,
                                 IJournal& journal,
                                 CancelToken& token) {
    auto res = ExecuteWithOutcome(plan, sink, journal, token, nullptr);
    if (!res) return Result<void>::Failure(res.Error());
    return Result<void>::Success();
}

Result<ExecutionOutcome> OnlineMode::ExecuteWithOutcome(const DefragPlan& plan,
                                                        IProgressSink& sink,
                                                        IJournal& journal,
                                                        CancelToken& token,
                                                        const std::vector<JournalRecord>* recoveredRecords) {
    ExecutionOutcome outcome{};
    auto geomRes = volume_.Geometry();
    if (!geomRes) return Result<ExecutionOutcome>::Failure(geomRes.Error());
    const auto& geom = geomRes.Value();

    // Dry-run emits zero moves and touches zero journal records
    if (plan.dryRun) {
        ProgressSnapshot snap{};
        snap.state = volume_.IsSimulation() ? OperationState::Demo : OperationState::Idle;
        snap.mode = plan.mode;
        snap.volumeId = plan.volumeId;
        snap.totalSteps = plan.moves.size();
        snap.totalClusters = plan.totalClustersToMove;
        snap.percentComplete = 100.0;
        snap.statusKey = "IDS_DRY_RUN_HEADER";
        snap.isSimulation = volume_.IsSimulation();
        sink.OnSnapshot(snap);

        LogEvent ev{};
        ev.level = LogLevel::Info;
        ev.opId = plan.planId;
        ev.volumeId = plan.volumeId;
        ev.stage = ErrorStage::Planning;
        ev.clusters = plan.totalClustersToMove;
        ev.messageKey = "IDS_DRY_RUN_HEADER";
        logger_.Log(ev);
        sink.OnEvent(ev);
        return Result<ExecutionOutcome>::Success(std::move(outcome));
    }

    // Re-check preflight & SSD fail-closed gate right before any write
    auto pfRes = volume_.Preflight(isElevated_, policy_.Config().fullDefragThresholdPct);
    if (!pfRes) return Result<ExecutionOutcome>::Failure(pfRes.Error());
    auto gate = policy_.CanWriteToVolume(pfRes.Value(), plan.mode, isElevated_, volume_.IsSimulation());
    if (!gate) {
        LogEvent ev{};
        ev.level = LogLevel::Error;
        ev.opId = plan.planId;
        ev.volumeId = plan.volumeId;
        ev.errorCode = gate.Error().code;
        ev.stage = ErrorStage::Preflight;
        ev.messageKey = gate.Error().userMessageKey;
        logger_.Log(ev);
        sink.OnEvent(ev);
        return Result<ExecutionOutcome>::Failure(gate.Error());
    }

    const std::uint64_t opId = plan.planHash ^ geom.volumeSerialNumber;
    if (recoveredRecords == nullptr) {
        auto openRes = journal.OpenOrCreate(opId, geom.volumeSerialNumber, plan.planHash);
        if (!openRes) return Result<ExecutionOutcome>::Failure(openRes.Error());
    }

    std::unordered_set<std::uint64_t> doneSteps;
    std::unordered_set<std::uint64_t> skippedSteps;
    std::unordered_map<std::uint64_t, JournalRecord> pendingIntents;
    std::uint32_t seq = 1;

    if (recoveredRecords != nullptr) {
        for (const auto& rec : *recoveredRecords) {
            if (rec.sequence >= seq) seq = rec.sequence + 1;
            if (rec.type == JournalRecordType::MoveIntent) {
                pendingIntents[rec.stepIndex] = rec;
            } else if (rec.type == JournalRecordType::MoveDone) {
                pendingIntents.erase(rec.stepIndex);
                doneSteps.insert(rec.stepIndex);
            } else if (rec.type == JournalRecordType::Skip) {
                pendingIntents.erase(rec.stepIndex);
                skippedSteps.insert(rec.stepIndex);
            }
        }
    } else {
        JournalRecord begin{};
        begin.sequence = seq++;
        begin.opId = opId;
        begin.type = JournalRecordType::PlanBegin;
        begin.planHash = plan.planHash;
        begin.volumeSerial = geom.volumeSerialNumber;
        auto app = journal.Append(begin, true);
        if (!app) return Result<ExecutionOutcome>::Failure(app.Error());
    }

    const auto startTime = std::chrono::steady_clock::now();
    auto lastPublish = startTime;
    auto lastCheckpoint = startTime;
    std::uint64_t movesSinceCheckpoint = 0;

    auto publishProgress = [&](FileId activeFile, Lcn activeTarget, std::string statusKey, bool force) {
        const auto now = std::chrono::steady_clock::now();
        const auto sincePub = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPublish).count();
        if (!force && sincePub < 100) return; // 10 Hz coalescing (§3.5 / R-074)
        lastPublish = now;

        const auto elapsedMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count());
        const double elapsedSec = std::max(0.001, static_cast<double>(elapsedMs) / 1000.0);
        const double mbMoved = static_cast<double>(outcome.bytesMoved) / (1024.0 * 1024.0);
        const double mbps = mbMoved / elapsedSec;
        const double pct = plan.totalClustersToMove > 0
                               ? (100.0 * static_cast<double>(outcome.clustersMoved) /
                                  static_cast<double>(plan.totalClustersToMove))
                               : 100.0;
        const ClusterCount remClusters = plan.totalClustersToMove > outcome.clustersMoved
                                             ? (plan.totalClustersToMove - outcome.clustersMoved)
                                             : 0;
        const double remMB = static_cast<double>(remClusters * geom.bytesPerCluster) / (1024.0 * 1024.0);
        const double etaSec = mbps > 0.01 ? (remMB / mbps) : 0.0;

        ProgressSnapshot snap{};
        snap.state = token.IsPaused() ? OperationState::Paused : OperationState::Defragmenting;
        snap.mode = plan.mode;
        snap.volumeId = plan.volumeId;
        snap.totalSteps = plan.moves.size();
        snap.completedSteps = outcome.stepsCompleted;
        snap.totalClusters = plan.totalClustersToMove;
        snap.clustersMoved = outcome.clustersMoved;
        snap.bytesMoved = outcome.bytesMoved;
        snap.percentComplete = pct;
        snap.throughputMBps = mbps;
        snap.etaSeconds = etaSec;
        snap.elapsedMs = elapsedMs;
        snap.activeFileId = activeFile;
        snap.activeTargetLcn = activeTarget;
        snap.statusKey = std::move(statusKey);
        snap.skippedCount = outcome.stepsSkipped;
        snap.verifiedCount = outcome.stepsVerified;
        snap.isSimulation = volume_.IsSimulation();
        sink.OnSnapshot(snap);
    };

    for (const auto& m : plan.moves) {
        if (doneSteps.count(m.stepIndex) > 0) {
            outcome.stepsCompleted += 1;
            outcome.clustersMoved += m.clusterCount;
            outcome.bytesMoved += m.clusterCount * geom.bytesPerCluster;
            continue;
        }
        if (skippedSteps.count(m.stepIndex) > 0) {
            outcome.stepsSkipped += 1;
            continue;
        }

        // Verify-before-retry for pending MOVE_INTENT without MOVE_DONE
        if (pendingIntents.count(m.stepIndex) > 0) {
            ErrorCode vErr = ErrorCode::Ok;
            if (VerifyMovedExtent(m.fileId, m.startVcn, m.targetLcn, m.clusterCount,
                                  m.expectedChecksum, verifyChecksumsSample_, vErr)) {
                JournalRecord reconciled{};
                reconciled.sequence = seq++;
                reconciled.opId = opId;
                reconciled.type = JournalRecordType::MoveDone;
                reconciled.stepIndex = m.stepIndex;
                reconciled.fileId = m.fileId;
                reconciled.startVcn = m.startVcn;
                reconciled.sourceLcn = m.sourceLcn;
                reconciled.targetLcn = m.targetLcn;
                reconciled.clusterCount = m.clusterCount;
                reconciled.planHash = plan.planHash;
                reconciled.volumeSerial = geom.volumeSerialNumber;
                auto app = journal.Append(reconciled, true);
                if (!app) return Result<ExecutionOutcome>::Failure(app.Error());

                outcome.stepsCompleted += 1;
                outcome.stepsVerified += 1;
                outcome.clustersMoved += m.clusterCount;
                outcome.bytesMoved += m.clusterCount * geom.bytesPerCluster;
                continue;
            }
        }

        // Cooperative pause / cancel barrier at move boundary
        if (token.IsPaused()) {
            (void)journal.Commit();
            publishProgress(m.fileId, m.targetLcn, "IDS_STATE_PAUSED", true);
            while (token.IsPaused() && !token.IsCancelled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (token.IsCancelled()) {
            (void)journal.Seal();
            return Result<ExecutionOutcome>::Failure(MakeError(ErrorCode::E_INTERNAL_CANCELLED, plan.volumeId));
        }

        // Periodic media re-probe (every 250 steps): if device ever becomes SSD/Unknown, abort immediately
        if ((m.stepIndex % 250ULL) == 0ULL && IsDefragBlockedMedia(volume_.Media())) {
            (void)journal.Seal();
            return Result<ExecutionOutcome>::Failure(MakeError(ErrorCode::E_PREFLIGHT_SSD_BLOCKED, plan.volumeId));
        }

        // Flush MOVE_INTENT BEFORE calling MoveExtent (§3.7)
        JournalRecord intent{};
        intent.sequence = seq++;
        intent.opId = opId;
        intent.type = JournalRecordType::MoveIntent;
        intent.stepIndex = m.stepIndex;
        intent.fileId = m.fileId;
        intent.startVcn = m.startVcn;
        intent.sourceLcn = m.sourceLcn;
        intent.targetLcn = m.targetLcn;
        intent.clusterCount = m.clusterCount;
        intent.planHash = plan.planHash;
        intent.volumeSerial = geom.volumeSerialNumber;
        auto intentRes = journal.Append(intent, true);
        if (!intentRes) return Result<ExecutionOutcome>::Failure(intentRes.Error());

        // Single serialised move with bounded retry
        MoveResult mr{};
        ErrorCode mappedErr = ErrorCode::Ok;
        for (int attempt = 0; attempt < 3; ++attempt) {
            mr = fs_.MoveExtent(m.fileId, m.startVcn, m.targetLcn, static_cast<std::uint32_t>(m.clusterCount));
            if (mr.success) {
                mappedErr = ErrorCode::Ok;
                break;
            }
            mappedErr = mr.errorCodeValue != 0
                            ? static_cast<ErrorCode>(mr.errorCodeValue)
                            : MapWin32ToMoveError(mr.win32Error);
            const auto& desc = DescribeError(mappedErr);
            if (!desc.retryable) break;
            if (token.IsCancelled()) break;
        }

        if (!mr.success) {
            if (mappedErr == ErrorCode::E_MOVE_INTERRUPTED || token.IsCancelled()) {
                (void)journal.Commit();
                return Result<ExecutionOutcome>::Failure(MakeError(ErrorCode::E_MOVE_INTERRUPTED, plan.volumeId, m.fileId));
            }
            if (mappedErr == ErrorCode::E_INTERNAL_VOLUME_REMOVED) {
                (void)journal.Seal();
                return Result<ExecutionOutcome>::Failure(MakeError(ErrorCode::E_INTERNAL_VOLUME_REMOVED, plan.volumeId, m.fileId));
            }

            // MFT safeguard (P5-T06): any failure on an MFT move aborts further metadata writes
            if (m.isMftMove) {
                (void)journal.Seal();
                return Result<ExecutionOutcome>::Failure(MakeError(ErrorCode::E_MOVE_MFT_UNAVAILABLE, plan.volumeId, m.fileId));
            }

            const SkipReason sr = mr.suggestedSkip != SkipReason::None ? mr.suggestedSkip : SkipReason::IoError;
            JournalRecord skipRec{};
            skipRec.sequence = seq++;
            skipRec.opId = opId;
            skipRec.type = JournalRecordType::Skip;
            skipRec.stepIndex = m.stepIndex;
            skipRec.fileId = m.fileId;
            skipRec.extraCode = static_cast<std::uint32_t>(sr);
            skipRec.planHash = plan.planHash;
            skipRec.volumeSerial = geom.volumeSerialNumber;
            (void)journal.Append(skipRec, false);

            outcome.stepsSkipped += 1;
            outcome.runtimeSkips.push_back({m.fileId, m.fileName, sr});

            LogEvent ev{};
            ev.level = LogLevel::Warn;
            ev.opId = plan.planId;
            ev.volumeId = plan.volumeId;
            ev.errorCode = mappedErr;
            ev.stage = ErrorStage::Move;
            ev.clusters = m.clusterCount;
            ev.messageKey = DescribeError(mappedErr).userMessageKey;
            ev.arguments = {HashPathForPrivacy(m.fileName)};
            logger_.Log(ev);
            sink.OnEvent(ev);

            publishProgress(m.fileId, m.targetLcn, "IDS_STATE_DEFRAGMENTING", false);
            continue;
        }

        // Post-move verification (P4-T04): re-read retrieval pointers (+ optional checksum sample)
        const bool doChecksum = verifyChecksumsSample_ && ((m.stepIndex % 20ULL) == 0ULL);
        ErrorCode verifyErr = ErrorCode::Ok;
        if (!VerifyMovedExtent(m.fileId, m.startVcn, m.targetLcn, m.clusterCount,
                               m.expectedChecksum, doChecksum, verifyErr)) {
            (void)journal.Seal();
            auto err = MakeError(verifyErr, plan.volumeId, m.fileId, 0, "Post-move verification mismatch");
            LogEvent ev{};
            ev.level = LogLevel::Error;
            ev.opId = plan.planId;
            ev.volumeId = plan.volumeId;
            ev.errorCode = verifyErr;
            ev.stage = ErrorStage::Verification;
            ev.messageKey = err.userMessageKey;
            logger_.Log(ev);
            sink.OnEvent(ev);
            return Result<ExecutionOutcome>::Failure(err);
        }

        JournalRecord done{};
        done.sequence = seq++;
        done.opId = opId;
        done.type = JournalRecordType::MoveDone;
        done.stepIndex = m.stepIndex;
        done.fileId = m.fileId;
        done.startVcn = m.startVcn;
        done.sourceLcn = m.sourceLcn;
        done.targetLcn = m.targetLcn;
        done.clusterCount = m.clusterCount;
        done.planHash = plan.planHash;
        done.volumeSerial = geom.volumeSerialNumber;
        auto doneRes = journal.Append(done, false);
        if (!doneRes) return Result<ExecutionOutcome>::Failure(doneRes.Error());

        outcome.stepsCompleted += 1;
        outcome.stepsVerified += 1;
        outcome.clustersMoved += m.clusterCount;
        outcome.bytesMoved += m.clusterCount * geom.bytesPerCluster;
        movesSinceCheckpoint += 1;

        const auto now = std::chrono::steady_clock::now();
        if (movesSinceCheckpoint >= 1000 ||
            std::chrono::duration_cast<std::chrono::seconds>(now - lastCheckpoint).count() >= 30) {
            auto cpRes = journal.Commit();
            if (!cpRes) return Result<ExecutionOutcome>::Failure(cpRes.Error());
            movesSinceCheckpoint = 0;
            lastCheckpoint = now;
        }

        publishProgress(m.fileId, m.targetLcn, "IDS_STATE_DEFRAGMENTING", false);
    }

    auto sealRes = journal.Seal();
    if (!sealRes) return Result<ExecutionOutcome>::Failure(sealRes.Error());

    publishProgress(kInvalidFileId, kInvalidLcn, "IDS_OP_COMPLETE", true);
    return Result<ExecutionOutcome>::Success(std::move(outcome));
}

} // namespace auralite
