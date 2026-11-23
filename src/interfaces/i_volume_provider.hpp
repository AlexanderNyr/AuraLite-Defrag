#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

namespace auralite {

class IVolumeProvider {
public:
    virtual ~IVolumeProvider() = default;

    virtual VolumeId Id() const = 0;
    virtual Result<VolumeGeometry> Geometry() = 0;
    virtual MediaClass Media() = 0;
    virtual MediaEvidence Evidence() = 0;
    virtual Result<PreflightResult> Preflight(bool isElevated, double fullDefragThresholdPct) = 0;
    virtual Result<BitmapSnapshot> ReadBitmap(Lcn start, ClusterCount count) = 0;
    virtual bool IsSimulation() const noexcept = 0;
};

} // namespace auralite
