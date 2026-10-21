#include "s4/infrastructure/ringbuffer/frame_log.hpp"

#include <chrono>
#include <iterator>
#include <sstream>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

namespace {

std::string index_path(const std::string& base) { return base + ".idx"; }
std::string blob_path(const std::string& base) { return base + ".bin"; }

} // namespace

// ------------------------------------------------------------------ gravacao

FrameLogRecorder::FrameLogRecorder(const std::string& basePath)
    : m_index(index_path(basePath)), m_blob(blob_path(basePath), std::ios::binary) {
    if (!m_index || !m_blob) {
        throw domain::FrameSourceError("cannot create scenario at " + basePath);
    }
}

FrameLogRecorder::~FrameLogRecorder() {
    close();
}

// Uma linha por quadro: ts offset length keyframe session
void FrameLogRecorder::record(const domain::CapturedFrame& frame) {
    m_index << frame.captureTsNs << ' ' << m_offset << ' ' << frame.length << ' '
            << (frame.isKeyframe ? 1 : 0) << ' ' << frame.sessionId << '\n';
    m_blob.write(reinterpret_cast<const char*>(frame.data),
                 static_cast<std::streamsize>(frame.length));
    m_offset += frame.length;
}

void FrameLogRecorder::close() {
    if (m_index.is_open()) {
        m_index.close();
    }
    if (m_blob.is_open()) {
        m_blob.close();
    }
}

// -------------------------------------------------------------------- replay

RecordedFrameSource::RecordedFrameSource(
    const std::string& basePath,
    const std::string& sessionOverride,
    bool realtime
) : m_basePath(basePath), m_sessionOverride(sessionOverride), m_realtime(realtime) {
    std::ifstream blob(blob_path(basePath), std::ios::binary);
    if (!blob) {
        throw domain::FrameSourceError("recorded scenario not found at " + basePath);
    }
    m_blob.assign(std::istreambuf_iterator<char>(blob), std::istreambuf_iterator<char>());
    m_entries = loadIndex();
}

RecordedFrameSource::~RecordedFrameSource() {
    stop();
}

void RecordedFrameSource::start(domain::FrameCallback onFrame) {
    if (m_running.exchange(true)) {
        return;
    }
    m_stopRequested.store(false);
    m_thread = std::thread([this, onFrame] { replay(onFrame); });
}

void RecordedFrameSource::stop() {
    m_stopRequested.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false);
}

void RecordedFrameSource::waitUntilFinished() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false);
}

void RecordedFrameSource::replay(domain::FrameCallback onFrame) {
    domain::Nanoseconds previousTs = 0;
    bool hasPrevious = false;
    for (const Entry& entry : m_entries) {
        if (m_stopRequested.load()) {
            return;
        }
        if (m_realtime && hasPrevious) {
            const double gap = static_cast<double>(entry.captureTsNs - previousTs) /
                               domain::kNanosecondsPerSecond;
            std::this_thread::sleep_for(std::chrono::duration<double>(gap));
        }
        previousTs = entry.captureTsNs;
        hasPrevious = true;

        domain::CapturedFrame frame;
        frame.captureTsNs = entry.captureTsNs;
        frame.data = m_blob.data() + entry.offset;
        frame.length = entry.length;
        frame.isKeyframe = entry.isKeyframe;
        frame.sessionId = m_sessionOverride.empty() ? entry.sessionId : m_sessionOverride;
        onFrame(frame);
    }
}

std::vector<RecordedFrameSource::Entry> RecordedFrameSource::loadIndex() const {
    std::ifstream index(index_path(m_basePath));
    if (!index) {
        throw domain::FrameSourceError("scenario index not found at " + m_basePath);
    }
    std::vector<Entry> entries;
    std::string line;
    while (std::getline(index, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream fields(line);
        Entry entry;
        int keyframeFlag = 0;
        fields >> entry.captureTsNs >> entry.offset >> entry.length >> keyframeFlag;
        const bool hasNumericFields = !fields.fail();
        fields >> entry.sessionId;  // pode faltar: sessao vazia e gravada como nada
        const bool isInsideBlob =
            entry.offset <= m_blob.size() && entry.length <= m_blob.size() - entry.offset;
        if (!hasNumericFields || entry.length == 0 || !isInsideBlob) {
            throw domain::FrameSourceError("corrupt scenario index at " + m_basePath + ": " + line);
        }
        entry.isKeyframe = keyframeFlag != 0;
        entries.push_back(entry);
    }
    return entries;
}

} // namespace ods::s4::infrastructure
