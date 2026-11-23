#pragma once

#include "core/types.hpp"

namespace auralite {

class IMediaDetector {
public:
    virtual ~IMediaDetector() = default;

    // Fail-closed: Unknown == SolidState for policy purposes
    virtual MediaClass Classify(const VolumeId& volumeId) = 0;
    virtual MediaEvidence Detail() const = 0;
};

} // namespace auralite
