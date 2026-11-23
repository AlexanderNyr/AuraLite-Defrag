#pragma once

#include "interfaces/i_media_detector.hpp"

namespace auralite {

class RealMediaDetector final : public IMediaDetector {
public:
    MediaClass Classify(const VolumeId& volumeId) override;
    MediaEvidence Detail() const override { return lastEvidence_; }

private:
    MediaEvidence lastEvidence_{};
};

} // namespace auralite
