#include "backend/emulator/emulator_backend.hpp"
#include "core/error_catalog.hpp"

#include <algorithm>
#include <cstring>

namespace auralite {

std::uint64_t DeterministicRng::NextU64() noexcept {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::uint64_t DeterministicRng::Range(std::uint64_t lo, std::uint64_t hi) noexcept {
    if (hi <= lo) return lo;
    return lo + (NextU64() % (hi - lo + 1));
}

EmulatorBackend::EmulatorBackend(EmulatorConfig config)
    : config_(std::move(config)),
      mediaDetector_(config_.mediaClass, config_.mediaEvidence) {
    const ClusterCount total = config_.geometry.totalClusters;
    bitmap_.startLcn = 0;
    bitmap_.totalClusters = total;
    bitmap_.generation = 1;
    bitmap_.bits.assign(static_cast<std::size_t>((total + 7ULL) / 8ULL), 0U);
    config_.geometry.freeClusters = total;

    if (config_.geometry.numberSectors == 0 && config_.geometry.bytesPerSector > 0) {
        config_.geometry.numberSectors =
            (total * config_.geometry.bytesPerCluster) / config_.geometry.bytesPerSector;
    }
    if (config_.geometry.volumeSerialNumber == 0) {
        config_.geometry.volumeSerialNumber = config_.seed ^ 0xA5A55A5A12345678ULL;
    }
    mftInfo_.mftStartLcn = config_.geometry.mftStartLcn;
    mftInfo_.mft2StartLcn = config_.geometry.mft2StartLcn;
    mftInfo_.mftValidDataLength = config_.geometry.mftValidDataLength;
}

std::uint32_t EmulatorBackend::CountFragments(const std::vector<Extent>& extents) noexcept {
    if (extents.empty()) return 0;
    std::uint32_t frags = 1;
    for (std::size_t i = 1; i < extents.size(); ++i) {
        const auto& prev = extents[i - 1];
        const auto& cur = extents[i];
        if (prev.startLcn + prev.clusterCount != cur.startLcn ||
            prev.startVcn + prev.clusterCount != cur.startVcn) {
            ++frags;
        }
    }
    return frags;
}

void EmulatorBackend::MergeContiguousExtents(std::vector<Extent>& extents) {
    if (extents.size() <= 1) return;
    std::sort(extents.begin(), extents.end(), [](const Extent& a, const Extent& b) {
        return a.startVcn < b.startVcn;
    });
    std::vector<Extent> merged;
    merged.reserve(extents.size());
    for (const auto& e : extents) {
        if (e.clusterCount == 0) continue;
        if (!merged.empty()) {
            auto& back = merged.back();
            if (back.startVcn + back.clusterCount == e.startVcn &&
                back.startLcn + back.clusterCount == e.startLcn) {
                back.clusterCount += e.clusterCount;
                continue;
            }
        }
        merged.push_back(e);
    }
    extents = std::move(merged);
}

void EmulatorBackend::AddFile(FileEntry entry) {
    MergeContiguousExtents(entry.extents);
    entry.fragmentCount = entry.flags.resident ? 0 : CountFragments(entry.extents);

    ClusterCount totalAllocated = 0;
    for (const auto& ext : entry.extents) {
        for (ClusterCount i = 0; i < ext.clusterCount; ++i) {
            const Lcn lcn = ext.startLcn + i;
            if (lcn < bitmap_.totalClusters && !bitmap_.IsAllocated(lcn)) {
                bitmap_.SetAllocated(lcn, true);
                if (config_.geometry.freeClusters > 0) {
                    config_.geometry.freeClusters -= 1;
                }
            }
        }
        totalAllocated += ext.clusterCount;
    }
    entry.allocatedClusters = totalAllocated;

    if (entry.contentChecksum == 0 && !entry.flags.resident && totalAllocated > 0) {
        entry.contentChecksum = (entry.id * 11400714819323198485ULL) ^ entry.sizeBytes ^ 0xCAFEBABEULL;
    }

    if (entry.id == kMftFileId) {
        mftInfo_.mftExtents = entry.extents;
        mftInfo_.fragmentCount = entry.fragmentCount;
        if (!entry.extents.empty()) {
            mftInfo_.mftStartLcn = entry.extents.front().startLcn;
            config_.geometry.mftStartLcn = mftInfo_.mftStartLcn;
        }
    }

    const auto it = fileIndex_.find(entry.id);
    if (it != fileIndex_.end()) {
        files_[it->second] = std::move(entry);
    } else {
        fileIndex_[entry.id] = files_.size();
        files_.push_back(std::move(entry));
    }
    bitmap_.generation += 1;
}

void EmulatorBackend::RemoveFile(FileId fileId) {
    const auto it = fileIndex_.find(fileId);
    if (it == fileIndex_.end()) return;
    const std::size_t idx = it->second;
    for (const auto& ext : files_[idx].extents) {
        for (ClusterCount i = 0; i < ext.clusterCount; ++i) {
            const Lcn lcn = ext.startLcn + i;
            if (lcn < bitmap_.totalClusters && bitmap_.IsAllocated(lcn)) {
                bitmap_.SetAllocated(lcn, false);
                config_.geometry.freeClusters += 1;
            }
        }
    }
    if (idx + 1 < files_.size()) {
        files_[idx] = std::move(files_.back());
        fileIndex_[files_[idx].id] = idx;
    }
    files_.pop_back();
    fileIndex_.erase(it);
    bitmap_.generation += 1;
}

void EmulatorBackend::SetScheduledFaults(std::vector<ScheduledFault> faults) {
    faults_ = std::move(faults);
}

Result<VolumeGeometry> EmulatorBackend::Geometry() {
    return Result<VolumeGeometry>::Success(config_.geometry);
}

Result<PreflightResult> EmulatorBackend::Preflight(bool isElevated, double fullDefragThresholdPct) {
    PreflightResult pf{};
    pf.mediaClass = Media();
    pf.evidence = Evidence();
    pf.freePercent = config_.geometry.totalClusters > 0
                         ? (100.0 * static_cast<double>(config_.geometry.freeClusters) /
                            static_cast<double>(config_.geometry.totalClusters))
                         : 0.0;

    if (config_.geometry.fsName != "NTFS") {
        pf.passed = false;
        pf.status = PreflightStatus::UnsupportedFileSystem;
        pf.messageKey = "IDS_PREFLIGHT_UNSUPPORTED_FS";
        return Result<PreflightResult>::Success(pf);
    }
    if (config_.bitLockerLocked) {
        pf.passed = false;
        pf.status = PreflightStatus::BitLockerLocked;
        pf.messageKey = "IDS_PREFLIGHT_BITLOCKER";
        return Result<PreflightResult>::Success(pf);
    }
    if (IsDefragBlockedMedia(pf.mediaClass)) {
        pf.passed = false;
        pf.status = PreflightStatus::SolidStateBlocked;
        pf.messageKey = "IDS_SSD_BLOCKED_BODY";
        return Result<PreflightResult>::Success(pf);
    }
    if (config_.dirtyVolume) {
        pf.passed = false;
        pf.status = PreflightStatus::DirtyVolume;
        pf.messageKey = "IDS_DIRTY_VOLUME";
        return Result<PreflightResult>::Success(pf);
    }
    if (config_.readOnlyVolume) {
        pf.passed = false;
        pf.status = PreflightStatus::ReadOnlyVolume;
        pf.messageKey = "IDS_PREFLIGHT_READ_ONLY";
        return Result<PreflightResult>::Success(pf);
    }
    if (!config_.isSimulationMode && !isElevated) {
        pf.passed = false;
        pf.status = PreflightStatus::NotElevated;
        pf.messageKey = "IDS_ELEVATION_REQUIRED";
        return Result<PreflightResult>::Success(pf);
    }
    if (pf.freePercent < 5.0) {
        pf.passed = false;
        pf.status = PreflightStatus::InsufficientFreeSpace;
        pf.messageKey = "IDS_PREFLIGHT_FAILED_GENERIC";
        return Result<PreflightResult>::Success(pf);
    }
    if (pf.freePercent < fullDefragThresholdPct) {
        pf.passed = true;
        pf.quickOnly = true;
        pf.status = PreflightStatus::QuickOnlyLowSpace;
        pf.messageKey = "IDS_STATE_IDLE";
        return Result<PreflightResult>::Success(pf);
    }

    pf.passed = true;
    pf.status = PreflightStatus::Clean;
    pf.messageKey = "IDS_STATE_IDLE";
    return Result<PreflightResult>::Success(pf);
}

Result<BitmapSnapshot> EmulatorBackend::ReadBitmap(Lcn start, ClusterCount count) {
    if (start >= bitmap_.totalClusters || count == 0) {
        return Result<BitmapSnapshot>::Failure(MakeError(ErrorCode::E_BITMAP_INVALID_RANGE, config_.volumeId));
    }
    // Mirror D-04 starting-LCN rounding down to byte boundary (8 clusters)
    const Lcn roundedStart = (start / 8ULL) * 8ULL;
    const ClusterCount avail = bitmap_.totalClusters - roundedStart;
    const ClusterCount actualCount = std::min<ClusterCount>(count + (start - roundedStart), avail);

    BitmapSnapshot out{};
    out.startLcn = roundedStart;
    out.totalClusters = actualCount;
    out.generation = bitmap_.generation;
    out.bits.assign(static_cast<std::size_t>((actualCount + 7ULL) / 8ULL), 0U);

    if (roundedStart == 0 && actualCount == bitmap_.totalClusters) {
        out.bits = bitmap_.bits;
    } else {
        for (ClusterCount i = 0; i < actualCount; ++i) {
            if (bitmap_.IsAllocated(roundedStart + i)) {
                out.SetAllocated(roundedStart + i, true);
            }
        }
    }
    return Result<BitmapSnapshot>::Success(std::move(out));
}

Result<std::vector<FileEntry>> EmulatorBackend::Enumerate(std::uint64_t maxFiles) {
    std::vector<FileEntry> out;
    const std::size_t n = std::min<std::size_t>(files_.size(), static_cast<std::size_t>(maxFiles));
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(files_[i]);
    }
    return Result<std::vector<FileEntry>>::Success(std::move(out));
}

Result<std::vector<Extent>> EmulatorBackend::Extents(FileId fileId) {
    const auto it = fileIndex_.find(fileId);
    if (it == fileIndex_.end()) {
        return Result<std::vector<Extent>>::Failure(MakeError(ErrorCode::E_MOVE_FILE_VANISHED, config_.volumeId, fileId));
    }
    const auto& f = files_[it->second];
    if (f.flags.resident) {
        return Result<std::vector<Extent>>::Success({});
    }
    return Result<std::vector<Extent>>::Success(f.extents);
}

Result<FileFlags> EmulatorBackend::Flags(FileId fileId) {
    const auto it = fileIndex_.find(fileId);
    if (it == fileIndex_.end()) {
        return Result<FileFlags>::Failure(MakeError(ErrorCode::E_MOVE_FILE_VANISHED, config_.volumeId, fileId));
    }
    return Result<FileFlags>::Success(files_[it->second].flags);
}

bool EmulatorBackend::RelocateFileExtents(FileEntry& file, Vcn startVcn, Lcn targetLcn, ClusterCount count) {
    // Check target free
    if (targetLcn + count > bitmap_.totalClusters) return false;
    for (ClusterCount i = 0; i < count; ++i) {
        if (bitmap_.IsAllocated(targetLcn + i)) return false;
    }

    // Build replacement extent list by splitting intersecting VCN extents
    std::vector<Extent> nextExtents;
    nextExtents.reserve(file.extents.size() + 2);
    const Vcn reqEnd = startVcn + count;
    ClusterCount movedSoFar = 0;

    for (const auto& e : file.extents) {
        const Vcn eEnd = e.startVcn + e.clusterCount;
        if (eEnd <= startVcn || e.startVcn >= reqEnd) {
            nextExtents.push_back(e);
            continue;
        }
        // Left unmoved portion
        if (e.startVcn < startVcn) {
            nextExtents.push_back({e.startVcn, e.startLcn, startVcn - e.startVcn});
        }
        // Overlapping moved portion
        const Vcn ovStart = std::max(e.startVcn, startVcn);
        const Vcn ovEnd = std::min(eEnd, reqEnd);
        const ClusterCount ovCount = ovEnd - ovStart;
        const Lcn oldSrcStart = e.startLcn + (ovStart - e.startVcn);
        const Lcn newDstStart = targetLcn + (ovStart - startVcn);

        for (ClusterCount i = 0; i < ovCount; ++i) {
            bitmap_.SetAllocated(oldSrcStart + i, false);
            bitmap_.SetAllocated(newDstStart + i, true);
        }
        nextExtents.push_back({ovStart, newDstStart, ovCount});
        movedSoFar += ovCount;

        // Right unmoved portion
        if (ovEnd < eEnd) {
            nextExtents.push_back({ovEnd, e.startLcn + (ovEnd - e.startVcn), eEnd - ovEnd});
        }
    }

    if (movedSoFar != count) {
        return false;
    }

    MergeContiguousExtents(nextExtents);
    file.extents = std::move(nextExtents);
    file.fragmentCount = CountFragments(file.extents);
    bitmap_.generation += 1;

    if (file.id == kMftFileId) {
        mftInfo_.mftExtents = file.extents;
        mftInfo_.fragmentCount = file.fragmentCount;
        if (!file.extents.empty()) {
            mftInfo_.mftStartLcn = file.extents.front().startLcn;
            config_.geometry.mftStartLcn = mftInfo_.mftStartLcn;
        }
    }
    return true;
}

MoveResult EmulatorBackend::MoveExtent(FileId fileId, Vcn startVcn, Lcn targetLcn, std::uint32_t clusters) {
    const std::uint64_t opIdx = moveCallCount_++;

    // Check deterministic scheduled faults
    for (auto& fault : faults_) {
        if (fault.triggered) continue;
        const bool matchOp = (fault.atOperation == opIdx);
        const bool matchFile = (fault.targetFileId == kInvalidFileId || fault.targetFileId == fileId);
        if (matchOp && matchFile) {
            fault.triggered = true;
            switch (fault.type) {
                case FaultType::IoError:
                    return {false, 0, fault.win32Error, static_cast<std::uint32_t>(ErrorCode::E_MOVE_IO), false, SkipReason::IoError};
                case FaultType::AccessDenied:
                    return {false, 0, 5, static_cast<std::uint32_t>(ErrorCode::E_MOVE_ACCESS_DENIED), false, SkipReason::AccessDenied};
                case FaultType::LockedInUse:
                    return {false, 0, 32, static_cast<std::uint32_t>(ErrorCode::E_MOVE_LOCKED), false, SkipReason::LockedOrInUse};
                case FaultType::UnsupportedStream:
                    return {false, 0, 50, static_cast<std::uint32_t>(ErrorCode::E_MOVE_UNSUPPORTED_STREAM), false, SkipReason::UnsupportedStream};
                case FaultType::InterruptionMidMove:
                    return {false, 0, 995, static_cast<std::uint32_t>(ErrorCode::E_MOVE_INTERRUPTED), false, SkipReason::None};
                case FaultType::FileVanished:
                    RemoveFile(fileId);
                    return {false, 0, 2, static_cast<std::uint32_t>(ErrorCode::E_MOVE_FILE_VANISHED), false, SkipReason::FileVanished};
                case FaultType::PartialFailure: {
                    const std::uint32_t half = std::max<std::uint32_t>(1U, clusters / 2U);
                    auto it = fileIndex_.find(fileId);
                    if (it != fileIndex_.end() && half < clusters) {
                        (void)RelocateFileExtents(files_[it->second], startVcn, targetLcn, half);
                    }
                    return {false, half, 1117, static_cast<std::uint32_t>(ErrorCode::E_MOVE_PARTIAL), true, SkipReason::IoError};
                }
                case FaultType::None:
                    break;
            }
        }
    }

    if (config_.readOnlyVolume || config_.dirtyVolume || IsDefragBlockedMedia(Media())) {
        return {false, 0, 5, static_cast<std::uint32_t>(ErrorCode::E_PREFLIGHT_SSD_BLOCKED), false, SkipReason::ReadOnlyOrPreFlightBlocked};
    }

    auto it = fileIndex_.find(fileId);
    if (it == fileIndex_.end()) {
        return {false, 0, 2, static_cast<std::uint32_t>(ErrorCode::E_MOVE_FILE_VANISHED), false, SkipReason::FileVanished};
    }
    FileEntry& file = files_[it->second];

    if (file.flags.isLocked) {
        return {false, 0, 32, static_cast<std::uint32_t>(ErrorCode::E_MOVE_LOCKED), false, SkipReason::LockedOrInUse};
    }
    if (file.flags.isPageOrHiber) {
        return {false, 0, 5, static_cast<std::uint32_t>(ErrorCode::E_MOVE_ACCESS_DENIED), false, SkipReason::PageOrSystemFile};
    }
    if (file.flags.isMetadata) {
        if (!(file.id == kMftFileId && config_.capabilities.supportsDirectMftMove)) {
            return {false, 0, 5, static_cast<std::uint32_t>(ErrorCode::E_MOVE_MFT_UNAVAILABLE), false, SkipReason::MetadataFile};
        }
    }
    if ((file.flags.compressed || file.flags.sparse) &&
        ((startVcn % 16ULL) != 0ULL || (targetLcn % 16ULL) != 0ULL)) {
        return {false, 0, 87, static_cast<std::uint32_t>(ErrorCode::E_MOVE_INVALID_PARAM), false, SkipReason::UnsupportedStream};
    }
    if (clusters == 0 || targetLcn + clusters > bitmap_.totalClusters) {
        return {false, 0, 87, static_cast<std::uint32_t>(ErrorCode::E_MOVE_INVALID_PARAM), false, SkipReason::None};
    }
    for (ClusterCount i = 0; i < clusters; ++i) {
        if (bitmap_.IsAllocated(targetLcn + i)) {
            return {false, 0, 183, static_cast<std::uint32_t>(ErrorCode::E_MOVE_STALE_BITMAP), false, SkipReason::StaleBitmapConflict};
        }
    }

    if (!RelocateFileExtents(file, startVcn, targetLcn, clusters)) {
        return {false, 0, 87, static_cast<std::uint32_t>(ErrorCode::E_MOVE_INVALID_PARAM), false, SkipReason::None};
    }

    return {true, clusters, 0, static_cast<std::uint32_t>(ErrorCode::Ok), false, SkipReason::None};
}

Result<MftInfo> EmulatorBackend::Mft() {
    return Result<MftInfo>::Success(mftInfo_);
}

Result<MftZoneInfo> EmulatorBackend::Zone() {
    MftZoneInfo z{};
    z.zoneStartLcn = config_.geometry.mftZoneStart;
    z.zoneEndLcn = config_.geometry.mftZoneEnd;
    z.reservedSettingLevel = reservedZoneLevel_;
    if (z.zoneEndLcn <= z.zoneStartLcn) {
        return Result<MftZoneInfo>::Success(z);
    }
    z.totalZoneClusters = z.zoneEndLcn - z.zoneStartLcn;

    ClusterCount freeInZone = 0;
    for (Lcn l = z.zoneStartLcn; l < z.zoneEndLcn && l < bitmap_.totalClusters; ++l) {
        if (!bitmap_.IsAllocated(l)) ++freeInZone;
    }
    z.freeZoneClusters = freeInZone;

    ClusterCount mftInZone = 0;
    for (const auto& ext : mftInfo_.mftExtents) {
        const Lcn s = std::max(ext.startLcn, z.zoneStartLcn);
        const Lcn e = std::min(ext.startLcn + ext.clusterCount, z.zoneEndLcn);
        if (e > s) mftInZone += (e - s);
    }
    z.occupiedByMftClusters = mftInZone;
    const ClusterCount usedInZone = z.totalZoneClusters > freeInZone ? (z.totalZoneClusters - freeInZone) : 0;
    z.occupiedByUserClusters = usedInZone > mftInZone ? (usedInZone - mftInZone) : 0;
    return Result<MftZoneInfo>::Success(z);
}

Result<std::uint64_t> EmulatorBackend::ComputeWindowChecksum(FileId fileId) {
    const auto it = fileIndex_.find(fileId);
    if (it == fileIndex_.end()) {
        return Result<std::uint64_t>::Failure(MakeError(ErrorCode::E_VERIFY_READBACK_FAILED, config_.volumeId, fileId));
    }
    return Result<std::uint64_t>::Success(files_[it->second].contentChecksum);
}

Result<std::uint32_t> EmulatorBackend::SimulateMftGrowth(ClusterCount additionalClusters) {
    auto it = fileIndex_.find(kMftFileId);
    if (it == fileIndex_.end()) {
        return Result<std::uint32_t>::Failure(MakeError(ErrorCode::E_MOVE_MFT_UNAVAILABLE));
    }
    FileEntry& mft = files_[it->second];
    const Lcn preferredStart = !mft.extents.empty()
                                   ? (mft.extents.back().startLcn + mft.extents.back().clusterCount)
                                   : config_.geometry.mftZoneStart;

    Vcn nextVcn = mft.allocatedClusters;
    ClusterCount rem = additionalClusters;
    Lcn cursor = preferredStart;

    // Prefer contiguous space right after current MFT tail or inside MFT Zone
    while (rem > 0 && cursor < bitmap_.totalClusters) {
        if (!bitmap_.IsAllocated(cursor)) {
            ClusterCount runLen = 0;
            while (runLen < rem && cursor + runLen < bitmap_.totalClusters && !bitmap_.IsAllocated(cursor + runLen)) {
                bitmap_.SetAllocated(cursor + runLen, true);
                if (config_.geometry.freeClusters > 0) config_.geometry.freeClusters -= 1;
                ++runLen;
            }
            mft.extents.push_back({nextVcn, cursor, runLen});
            nextVcn += runLen;
            rem -= runLen;
            cursor += runLen;
        } else {
            ++cursor;
        }
    }
    // Wrap around from 0 if needed
    cursor = 0;
    while (rem > 0 && cursor < bitmap_.totalClusters) {
        if (!bitmap_.IsAllocated(cursor)) {
            ClusterCount runLen = 0;
            while (runLen < rem && cursor + runLen < bitmap_.totalClusters && !bitmap_.IsAllocated(cursor + runLen)) {
                bitmap_.SetAllocated(cursor + runLen, true);
                if (config_.geometry.freeClusters > 0) config_.geometry.freeClusters -= 1;
                ++runLen;
            }
            mft.extents.push_back({nextVcn, cursor, runLen});
            nextVcn += runLen;
            rem -= runLen;
            cursor += runLen;
        } else {
            ++cursor;
        }
    }

    MergeContiguousExtents(mft.extents);
    mft.allocatedClusters = nextVcn;
    mft.fragmentCount = CountFragments(mft.extents);
    mftInfo_.mftExtents = mft.extents;
    mftInfo_.fragmentCount = mft.fragmentCount;
    bitmap_.generation += 1;
    return Result<std::uint32_t>::Success(mft.fragmentCount);
}

bool EmulatorBackend::VerifyModelInvariants(std::string* outReason) const {
    // Verify free cluster count matches bitmap and file extents do not overlap
    std::vector<uint8_t> seen(bitmap_.bits.size(), 0);
    ClusterCount totalFileClusters = 0;

    for (const auto& f : files_) {
        if (f.flags.resident) continue;
        ClusterCount fileSum = 0;
        for (const auto& e : f.extents) {
            if (e.startLcn + e.clusterCount > bitmap_.totalClusters) {
                if (outReason) *outReason = "Extent out of volume bounds";
                return false;
            }
            for (ClusterCount i = 0; i < e.clusterCount; ++i) {
                const Lcn lcn = e.startLcn + i;
                if (!bitmap_.IsAllocated(lcn)) {
                    if (outReason) *outReason = "File extent cluster not marked allocated in bitmap";
                    return false;
                }
                const std::size_t byteIdx = static_cast<std::size_t>(lcn >> 3);
                const std::uint8_t mask = static_cast<std::uint8_t>(1U << (lcn & 7U));
                if ((seen[byteIdx] & mask) != 0) {
                    if (outReason) *outReason = "Overlapping file extent cluster detected";
                    return false;
                }
                seen[byteIdx] |= mask;
            }
            fileSum += e.clusterCount;
        }
        if (fileSum != f.allocatedClusters) {
            if (outReason) *outReason = "File allocatedClusters mismatch";
            return false;
        }
        totalFileClusters += fileSum;
    }

    ClusterCount allocatedInBitmap = 0;
    for (ClusterCount i = 0; i < bitmap_.totalClusters; ++i) {
        if (bitmap_.IsAllocated(i)) ++allocatedInBitmap;
    }
    if (allocatedInBitmap != totalFileClusters) {
        if (outReason) *outReason = "Bitmap allocated count != sum of file clusters";
        return false;
    }
    if (config_.geometry.freeClusters != bitmap_.totalClusters - allocatedInBitmap) {
        if (outReason) *outReason = "Geometry freeClusters mismatch";
        return false;
    }
    return true;
}

} // namespace auralite
