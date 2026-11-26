#pragma once

#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace LMAC {

struct AudioBuffer {
    static constexpr size_t PEAK_BLOCK = 256;
    struct Peak {
        float mn = 0.0f;
        float mx = 0.0f;
    };

    std::vector<float> L;
    std::vector<float> R;
    std::vector<Peak> peaks;

    void buildPeaks() {
        size_t n = std::min(L.size(), R.size());
        if (n == 0) {
            peaks.clear();
            return;
        }
        size_t numBlocks = (n + PEAK_BLOCK - 1) / PEAK_BLOCK;
        peaks.resize(numBlocks);
        for (size_t b = 0; b < numBlocks; ++b) {
            size_t s = b * PEAK_BLOCK;
            size_t e = std::min(n, s + PEAK_BLOCK);
            float mn = 1.0f, mx = -1.0f;
            for (size_t i = s; i < e; ++i) {
                float v = 0.5f * (L[i] + R[i]);
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
            peaks[b] = { mn, mx };
        }
    }
};

struct ClipSnapshot {
    std::shared_ptr<const AudioBuffer> audio = std::make_shared<AudioBuffer>();
    uint32_t sampleRate = 48000;
    size_t trimStart = 0;
    size_t trimEnd = 0;
    uint64_t version = 0;

    const std::vector<float>& L() const { return audio->L; }
    const std::vector<float>& R() const { return audio->R; }
    size_t numFrames() const { return audio ? audio->L.size() : 0; }
    size_t byteSize() const {
        if (!audio) return 0;
        return audio->L.size() * 2 * sizeof(float) + audio->peaks.size() * sizeof(AudioBuffer::Peak);
    }
    bool empty() const { return numFrames() == 0 || trimEnd <= trimStart; }
    double durationSeconds() const {
        return (sampleRate > 0) ? static_cast<double>(numFrames()) / sampleRate : 0.0;
    }
    double trimmedDurationSeconds() const {
        if (sampleRate == 0 || trimEnd <= trimStart) return 0.0;
        return static_cast<double>(trimEnd - trimStart) / sampleRate;
    }
};

class AudioClip {
public:
    static constexpr size_t MAX_UNDO = 20;
    static constexpr size_t MAX_UNDO_BYTES = 256u * 1024u * 1024u;

    AudioClip() {
        auto snap = std::make_shared<ClipSnapshot>();
        snap->version = 1;
        currentSnap = snap;
        versionAtomic.store(1, std::memory_order_release);
    }

    uint64_t getVersion() const {
        return versionAtomic.load(std::memory_order_acquire);
    }

    std::shared_ptr<const ClipSnapshot> getSnapshot() const {
        std::lock_guard<std::mutex> lock(mtx);
        return currentSnap;
    }

    std::shared_ptr<const ClipSnapshot> tryGetSnapshot() const {
        std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
        if (lock.owns_lock()) {
            return currentSnap;
        }
        return nullptr;
    }

    void setAudio(std::vector<float> inL, std::vector<float> inR, uint32_t sampleRate, bool recordUndo = true) {
        std::lock_guard<std::mutex> lock(mtx);
        if (recordUndo && currentSnap->numFrames() > 0) {
            pushUndoLocked();
        } else if (!recordUndo) {
            undoStack.clear();
            redoStack.clear();
        }

        size_t n = std::min(inL.size(), inR.size());
        inL.resize(n);
        inR.resize(n);

        auto buf = std::make_shared<AudioBuffer>();
        buf->L = std::move(inL);
        buf->R = std::move(inR);
        buf->buildPeaks();

        auto next = std::make_shared<ClipSnapshot>();
        next->audio = std::move(buf);
        next->sampleRate = (sampleRate > 0) ? sampleRate : 48000;
        next->trimStart = 0;
        next->trimEnd = n;
        publishLocked(next);
    }

    void setAudioWithTrim(std::vector<float> inL, std::vector<float> inR, uint32_t sampleRate,
                          size_t trimStart, size_t trimEnd) {
        std::lock_guard<std::mutex> lock(mtx);
        undoStack.clear();
        redoStack.clear();
        size_t n = std::min(inL.size(), inR.size());
        inL.resize(n);
        inR.resize(n);

        auto buf = std::make_shared<AudioBuffer>();
        buf->L = std::move(inL);
        buf->R = std::move(inR);
        buf->buildPeaks();

        auto next = std::make_shared<ClipSnapshot>();
        next->audio = std::move(buf);
        next->sampleRate = (sampleRate > 0) ? sampleRate : 48000;
        next->trimStart = std::min(trimStart, n);
        next->trimEnd = std::clamp(trimEnd, next->trimStart, n);
        publishLocked(next);
    }

    // Zero-copy trim update: shares the immutable AudioBuffer
    void setTrim(size_t start, size_t end) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t n = currentSnap->numFrames();
        if (n == 0) return;
        size_t s = std::min(start, n - 1);
        size_t e = std::clamp(end, s + 1, n);
        if (s == currentSnap->trimStart && e == currentSnap->trimEnd) return;

        auto next = std::make_shared<ClipSnapshot>();
        next->audio = currentSnap->audio;
        next->sampleRate = currentSnap->sampleRate;
        next->trimStart = s;
        next->trimEnd = e;
        publishLocked(next);
    }

    void cropToRange(size_t start, size_t end) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t n = currentSnap->numFrames();
        if (n == 0) return;

        if (end <= start) {
            start = currentSnap->trimStart;
            end = currentSnap->trimEnd;
        }
        start = std::min(start, n);
        end = std::clamp(end, start, n);
        if (end <= start || (start == 0 && end == n)) return;

        pushUndoLocked();
        const auto& srcL = currentSnap->L();
        const auto& srcR = currentSnap->R();

        auto buf = std::make_shared<AudioBuffer>();
        buf->L.assign(srcL.begin() + start, srcL.begin() + end);
        buf->R.assign(srcR.begin() + start, srcR.begin() + end);
        buf->buildPeaks();

        auto next = std::make_shared<ClipSnapshot>();
        next->audio = std::move(buf);
        next->sampleRate = currentSnap->sampleRate;
        next->trimStart = 0;
        next->trimEnd = next->numFrames();
        publishLocked(next);
    }

    void deleteRange(size_t start, size_t end) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t n = currentSnap->numFrames();
        if (n == 0) return;
        start = std::min(start, n);
        end = std::clamp(end, start, n);
        if (end <= start) return;

        if (start == 0 && end == n) {
            pushUndoLocked();
            auto next = std::make_shared<ClipSnapshot>();
            next->sampleRate = currentSnap->sampleRate;
            publishLocked(next);
            return;
        }

        pushUndoLocked();
        const auto& srcL = currentSnap->L();
        const auto& srcR = currentSnap->R();

        auto buf = std::make_shared<AudioBuffer>();
        buf->L.reserve(n - (end - start));
        buf->R.reserve(n - (end - start));
        buf->L.insert(buf->L.end(), srcL.begin(), srcL.begin() + start);
        buf->L.insert(buf->L.end(), srcL.begin() + end, srcL.end());
        buf->R.insert(buf->R.end(), srcR.begin(), srcR.begin() + start);
        buf->R.insert(buf->R.end(), srcR.begin() + end, srcR.end());

        size_t fadeLen = std::min<size_t>(
            static_cast<size_t>(currentSnap->sampleRate * 0.002f),
            std::min(start, buf->L.size() - start)
        );
        for (size_t i = 0; i < fadeLen; ++i) {
            float t = static_cast<float>(i + 1) / static_cast<float>(fadeLen + 1);
            size_t idxL = start - fadeLen + i;
            size_t idxR = start + i;
            if (idxL < buf->L.size()) {
                buf->L[idxL] *= (1.0f - 0.5f * t);
                buf->R[idxL] *= (1.0f - 0.5f * t);
            }
            if (idxR < buf->L.size()) {
                buf->L[idxR] *= (0.5f + 0.5f * t);
                buf->R[idxR] *= (0.5f + 0.5f * t);
            }
        }

        auto next = std::make_shared<ClipSnapshot>();
        next->sampleRate = currentSnap->sampleRate;
        next->trimStart = (currentSnap->trimStart <= start)
            ? currentSnap->trimStart
            : (currentSnap->trimStart >= end ? currentSnap->trimStart - (end - start) : start);
        next->trimEnd = (currentSnap->trimEnd <= start)
            ? currentSnap->trimEnd
            : (currentSnap->trimEnd >= end ? currentSnap->trimEnd - (end - start) : start);
        size_t newLen = buf->L.size();
        next->trimStart = std::min(next->trimStart, newLen);
        next->trimEnd = std::clamp(next->trimEnd, next->trimStart, newLen);
        if (next->trimEnd <= next->trimStart && newLen > 0) {
            next->trimStart = 0;
            next->trimEnd = newLen;
        }
        buf->buildPeaks();
        next->audio = std::move(buf);
        publishLocked(next);
    }

    void silenceRange(size_t start, size_t end) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t n = currentSnap->numFrames();
        if (n == 0) return;
        resolveRange(start, end, n);
        if (end <= start) return;

        pushUndoLocked();
        auto buf = cloneBufferLocked();
        size_t fade = std::min<size_t>(static_cast<size_t>(currentSnap->sampleRate * 0.002f), (end - start) / 4);
        for (size_t i = start; i < end; ++i) {
            float g = 0.0f;
            if (fade > 0 && i < start + fade) {
                float t = static_cast<float>(i - start) / static_cast<float>(fade);
                g = 1.0f - t;
            } else if (fade > 0 && i >= end - fade) {
                float t = static_cast<float>(i - (end - fade)) / static_cast<float>(fade);
                g = t;
            }
            buf->L[i] *= g;
            buf->R[i] *= g;
        }
        publishWithBufferLocked(std::move(buf));
    }

    void fadeIn(size_t start, size_t end) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t n = currentSnap->numFrames();
        if (n == 0) return;
        if (end <= start) {
            start = currentSnap->trimStart;
            size_t trimLen = (currentSnap->trimEnd > currentSnap->trimStart)
                ? (currentSnap->trimEnd - currentSnap->trimStart) : n;
            size_t defaultLen = std::min<size_t>(trimLen / 4, static_cast<size_t>(currentSnap->sampleRate * 0.25f));
            end = std::min(n, start + std::max<size_t>(defaultLen, 1));
        } else {
            start = std::min(start, n);
            end = std::clamp(end, start, n);
        }
        if (end <= start) return;

        pushUndoLocked();
        auto buf = cloneBufferLocked();
        size_t len = end - start;
        constexpr float PI = 3.14159265358979323846f;
        for (size_t i = 0; i < len; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(std::max<size_t>(len - 1, 1));
            float gain = 0.5f * (1.0f - std::cos(PI * t));
            buf->L[start + i] *= gain;
            buf->R[start + i] *= gain;
        }
        publishWithBufferLocked(std::move(buf));
    }

    void fadeOut(size_t start, size_t end) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t n = currentSnap->numFrames();
        if (n == 0) return;
        if (end <= start) {
            end = (currentSnap->trimEnd > currentSnap->trimStart) ? currentSnap->trimEnd : n;
            size_t trimLen = (currentSnap->trimEnd > currentSnap->trimStart)
                ? (currentSnap->trimEnd - currentSnap->trimStart) : n;
            size_t defaultLen = std::min<size_t>(trimLen / 4, static_cast<size_t>(currentSnap->sampleRate * 0.25f));
            start = (end > defaultLen) ? (end - std::max<size_t>(defaultLen, 1)) : 0;
        } else {
            start = std::min(start, n);
            end = std::clamp(end, start, n);
        }
        if (end <= start) return;

        pushUndoLocked();
        auto buf = cloneBufferLocked();
        size_t len = end - start;
        constexpr float PI = 3.14159265358979323846f;
        for (size_t i = 0; i < len; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(std::max<size_t>(len - 1, 1));
            float gain = 0.5f * (1.0f + std::cos(PI * t));
            buf->L[start + i] *= gain;
            buf->R[start + i] *= gain;
        }
        publishWithBufferLocked(std::move(buf));
    }

    void normalize(size_t start, size_t end, float targetDb = -0.5f) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t n = currentSnap->numFrames();
        if (n == 0) return;
        resolveRange(start, end, n);
        if (end <= start) return;

        const auto& srcL = currentSnap->L();
        const auto& srcR = currentSnap->R();
        float peak = 0.0f;
        for (size_t i = start; i < end; ++i) {
            peak = std::max(peak, std::max(std::abs(srcL[i]), std::abs(srcR[i])));
        }
        if (peak < 1e-6f) return;

        float targetLin = std::pow(10.0f, targetDb / 20.0f);
        float scale = targetLin / peak;
        if (std::abs(scale - 1.0f) < 1e-4f) return;

        pushUndoLocked();
        auto buf = cloneBufferLocked();
        for (size_t i = start; i < end; ++i) {
            buf->L[i] = std::clamp(buf->L[i] * scale, -1.0f, 1.0f);
            buf->R[i] = std::clamp(buf->R[i] * scale, -1.0f, 1.0f);
        }
        publishWithBufferLocked(std::move(buf));
    }

    void reverse(size_t start, size_t end) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t n = currentSnap->numFrames();
        if (n == 0) return;
        resolveRange(start, end, n);
        if (end <= start + 1) return;

        pushUndoLocked();
        auto buf = cloneBufferLocked();
        std::reverse(buf->L.begin() + start, buf->L.begin() + end);
        std::reverse(buf->R.begin() + start, buf->R.begin() + end);
        publishWithBufferLocked(std::move(buf));
    }

    void removeDC(size_t start, size_t end) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t n = currentSnap->numFrames();
        if (n == 0) return;
        resolveRange(start, end, n);
        if (end <= start) return;

        const auto& srcL = currentSnap->L();
        const auto& srcR = currentSnap->R();
        double sumL = 0.0, sumR = 0.0;
        for (size_t i = start; i < end; ++i) {
            sumL += srcL[i];
            sumR += srcR[i];
        }
        float dcL = static_cast<float>(sumL / static_cast<double>(end - start));
        float dcR = static_cast<float>(sumR / static_cast<double>(end - start));
        if (std::abs(dcL) < 1e-7f && std::abs(dcR) < 1e-7f) return;

        pushUndoLocked();
        auto buf = cloneBufferLocked();
        for (size_t i = start; i < end; ++i) {
            buf->L[i] = std::clamp(buf->L[i] - dcL, -1.0f, 1.0f);
            buf->R[i] = std::clamp(buf->R[i] - dcR, -1.0f, 1.0f);
        }
        publishWithBufferLocked(std::move(buf));
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        if (currentSnap->numFrames() == 0) return;
        pushUndoLocked();
        auto next = std::make_shared<ClipSnapshot>();
        next->sampleRate = currentSnap->sampleRate;
        publishLocked(next);
    }

    bool canUndo() const {
        std::lock_guard<std::mutex> lock(mtx);
        return !undoStack.empty();
    }

    bool canRedo() const {
        std::lock_guard<std::mutex> lock(mtx);
        return !redoStack.empty();
    }

    bool undo() {
        std::lock_guard<std::mutex> lock(mtx);
        if (undoStack.empty()) return false;
        redoStack.push_back(currentSnap);
        auto prev = undoStack.back();
        undoStack.pop_back();
        publishLocked(prev);
        return true;
    }

    bool redo() {
        std::lock_guard<std::mutex> lock(mtx);
        if (redoStack.empty()) return false;
        undoStack.push_back(currentSnap);
        auto next = redoStack.back();
        redoStack.pop_back();
        publishLocked(next);
        return true;
    }

private:
    std::shared_ptr<AudioBuffer> cloneBufferLocked() const {
        auto buf = std::make_shared<AudioBuffer>();
        if (currentSnap && currentSnap->audio) {
            buf->L = currentSnap->audio->L;
            buf->R = currentSnap->audio->R;
        }
        return buf;
    }

    void publishWithBufferLocked(std::shared_ptr<AudioBuffer> buf) {
        buf->buildPeaks();
        auto next = std::make_shared<ClipSnapshot>();
        next->audio = std::move(buf);
        next->sampleRate = currentSnap->sampleRate;
        next->trimStart = currentSnap->trimStart;
        next->trimEnd = currentSnap->trimEnd;
        publishLocked(next);
    }

    void resolveRange(size_t& start, size_t& end, size_t n) const {
        if (end <= start) {
            start = currentSnap->trimStart;
            end = currentSnap->trimEnd;
            if (end <= start) {
                start = 0;
                end = n;
            }
        } else {
            start = std::min(start, n);
            end = std::clamp(end, start, n);
        }
    }

    void pushUndoLocked() {
        undoStack.push_back(currentSnap);
        size_t totalBytes = 0;
        for (const auto& s : undoStack) {
            totalBytes += s ? s->byteSize() : 0;
        }
        while (undoStack.size() > 1 && (undoStack.size() > MAX_UNDO || totalBytes > MAX_UNDO_BYTES)) {
            totalBytes -= undoStack.front() ? undoStack.front()->byteSize() : 0;
            undoStack.erase(undoStack.begin());
        }
        redoStack.clear();
    }

    void publishLocked(std::shared_ptr<ClipSnapshot> next) {
        uint64_t v = versionAtomic.load(std::memory_order_relaxed) + 1;
        next->version = v;
        currentSnap = std::move(next);
        versionAtomic.store(v, std::memory_order_release);
    }

    mutable std::mutex mtx;
    std::shared_ptr<ClipSnapshot> currentSnap;
    std::atomic<uint64_t> versionAtomic{0};
    std::vector<std::shared_ptr<ClipSnapshot>> undoStack;
    std::vector<std::shared_ptr<ClipSnapshot>> redoStack;
};

} // namespace LMAC
