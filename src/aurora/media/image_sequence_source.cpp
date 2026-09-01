#include "aurora/media/image_sequence_source.h"

#include <algorithm>
#include <string>

namespace aurora {

namespace {

auto split_paths(std::string_view uri) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::string cur;
    for (const char ch : uri) {
        if (ch == ';' || ch == '|') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

} // namespace

auto ImageSequenceSource::open(std::string_view uri) -> Result<bool> {
    m_frames.clear();
    m_pos = std::chrono::microseconds{ 0 };
    m_playing = false;
    auto paths = split_paths(uri);
    if (paths.empty()) {
        return Error{ .code = "ImageSequenceSource: empty uri" };
    }
    for (auto &p : paths) {
        auto r = Image::load(p);
        if (!r) {
            return r.error(); // 传播加载错误
        }
        m_frames.push_back(std::move(r.value()));
    }
    return true;
}

auto ImageSequenceSource::natural_size() const -> Size {
    if (m_frames.empty()) {
        return Size{ .width = 0.0f, .height = 0.0f };
    }
    return Size{ .width = static_cast<float>(m_frames[0].width), .height = static_cast<float>(m_frames[0].height) };
}

auto ImageSequenceSource::duration() const -> std::chrono::microseconds {
    if (m_frames.empty() || m_fps <= 0.0) {
        return std::chrono::microseconds{ 0 };
    }
    const double secs = static_cast<double>(m_frames.size()) / m_fps;
    return std::chrono::microseconds{ static_cast<long long>(secs * 1'000'000.0) };
}

auto ImageSequenceSource::seek(std::chrono::microseconds pos) -> void {
    const auto dur = duration();
    if (pos < std::chrono::microseconds{ 0 }) {
        pos = std::chrono::microseconds{ 0 };
    }
    if (dur.count() > 0 && pos > dur) {
        pos = dur;
    }
    m_pos = pos;
}

auto ImageSequenceSource::frame_at(std::chrono::microseconds pos) -> Result<VideoFrame> {
    if (m_frames.empty()) {
        return Error{ .code = "ImageSequenceSource: no frames" };
    }
    const auto dur = duration();
    if (pos < std::chrono::microseconds{ 0 }) {
        pos = std::chrono::microseconds{ 0 };
    }
    if (dur.count() > 0 && pos > dur) {
        pos = dur;
    }
    const double t = static_cast<double>(pos.count()) / 1'000'000.0;
    auto idx = static_cast<size_t>(t * m_fps);
    if (idx >= m_frames.size()) {
        idx = m_frames.size() - 1;
    }
    const double pts_secs = static_cast<double>(idx) / m_fps;
    return VideoFrame{ .image = m_frames[idx],
                       .pts = std::chrono::microseconds{ static_cast<long long>(pts_secs * 1'000'000.0) } };
}

} // namespace aurora
