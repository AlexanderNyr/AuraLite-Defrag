#pragma once

#include "interfaces/i_media_detector.hpp"

namespace auralite {

class RealMediaDetector final : public IMediaDetector {
public:
    MediaClass Classify(const VolumeId& volumeId) override;
    MediaEvidence Detail() const override { return lastEvidence_; }

    // Append a one-line note to the probe trace. Used by the backend to explain
    // failures that happen outside the media probe itself (for example an FSCTL
    // refused for lack of FILE_READ_DATA), so the trace covers the whole
    // preflight rather than only the disk classification.
    void NoteProbe(const std::string& note) { lastEvidence_.probeLog.push_back(note); }

private:
    MediaEvidence lastEvidence_{};
};

} // namespace auralite
