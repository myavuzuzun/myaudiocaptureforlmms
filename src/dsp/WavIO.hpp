#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

namespace LMAC {

class WavIO {
public:
    static inline int16_t floatToInt16(float x) {
        if (x >= 1.0f) return 32767;
        if (x <= -1.0f) return -32768;
        float scaled = x * 32767.0f;
        return static_cast<int16_t>(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
    }

    static inline float int16ToFloat(int16_t s) {
        return static_cast<float>(s) / 32768.0f;
    }

    static bool loadWav(const std::wstring& path,
                        std::vector<float>& outL,
                        std::vector<float>& outR,
                        uint32_t& outSampleRate) {
        outL.clear();
        outR.clear();
        outSampleRate = 48000;

        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;

        auto closeFile = [&]() {
            if (f) { fclose(f); f = nullptr; }
        };

        uint8_t riff[12];
        if (fread(riff, 1, 12, f) != 12) { closeFile(); return false; }
        if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
            closeFile();
            return false;
        }

        bool hasFmt = false;
        uint16_t audioFormat = 0;
        uint16_t numChannels = 0;
        uint32_t sampleRate = 48000;
        uint16_t bitsPerSample = 0;
        uint16_t validBits = 0;

        bool hasData = false;
        std::vector<uint8_t> rawData;

        while (true) {
            uint8_t hdr[8];
            if (fread(hdr, 1, 8, f) != 8) break;
            uint32_t chunkSize = static_cast<uint32_t>(hdr[4]) |
                                 (static_cast<uint32_t>(hdr[5]) << 8) |
                                 (static_cast<uint32_t>(hdr[6]) << 16) |
                                 (static_cast<uint32_t>(hdr[7]) << 24);

            if (std::memcmp(hdr, "fmt ", 4) == 0) {
                if (chunkSize < 16 || chunkSize > 4096) { closeFile(); return false; }
                std::vector<uint8_t> fmt(chunkSize);
                if (fread(fmt.data(), 1, chunkSize, f) != chunkSize) { closeFile(); return false; }

                audioFormat = static_cast<uint16_t>(fmt[0]) | (static_cast<uint16_t>(fmt[1]) << 8);
                numChannels = static_cast<uint16_t>(fmt[2]) | (static_cast<uint16_t>(fmt[3]) << 8);
                sampleRate = static_cast<uint32_t>(fmt[4]) |
                             (static_cast<uint32_t>(fmt[5]) << 8) |
                             (static_cast<uint32_t>(fmt[6]) << 16) |
                             (static_cast<uint32_t>(fmt[7]) << 24);
                bitsPerSample = static_cast<uint16_t>(fmt[14]) | (static_cast<uint16_t>(fmt[15]) << 8);
                validBits = bitsPerSample;

                // WAVE_FORMAT_EXTENSIBLE (0xFFFE)
                if (audioFormat == 0xFFFE && chunkSize >= 40) {
                    validBits = static_cast<uint16_t>(fmt[18]) | (static_cast<uint16_t>(fmt[19]) << 8);
                    uint16_t subFmt = static_cast<uint16_t>(fmt[24]) | (static_cast<uint16_t>(fmt[25]) << 8);
                    audioFormat = subFmt;
                }
                hasFmt = true;
            } else if (std::memcmp(hdr, "data", 4) == 0) {
                // Guard against absurd/corrupt chunks (> 512 MB)
                if (chunkSize > 512u * 1024u * 1024u) { closeFile(); return false; }
                rawData.resize(chunkSize);
                if (chunkSize > 0 && fread(rawData.data(), 1, chunkSize, f) != chunkSize) {
                    closeFile();
                    return false;
                }
                hasData = true;
            } else {
                // Skip unknown chunk
                if (_fseeki64(f, static_cast<int64_t>(chunkSize), SEEK_CUR) != 0) {
                    break;
                }
            }

            // RIFF chunks are word-aligned (even byte boundary)
            if (chunkSize & 1) {
                _fseeki64(f, 1, SEEK_CUR);
            }
            if (hasFmt && hasData) break;
        }
        closeFile();

        if (!hasFmt || !hasData || numChannels == 0 || sampleRate == 0) return false;
        if (audioFormat != 1 && audioFormat != 3) return false; // PCM or IEEE Float only
        if (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) return false;
        if (audioFormat == 3 && bitsPerSample != 32) return false;
        (void)validBits;

        size_t bytesPerSample = bitsPerSample / 8;
        size_t frameSize = bytesPerSample * numChannels;
        if (frameSize == 0) return false;
        size_t numFrames = rawData.size() / frameSize;
        if (numFrames == 0) return false;

        outL.resize(numFrames);
        outR.resize(numFrames);
        outSampleRate = sampleRate;

        const uint8_t* ptr = rawData.data();

        auto readSample = [&](const uint8_t* p) -> float {
            if (audioFormat == 3) {
                float v = 0.0f;
                std::memcpy(&v, p, 4);
                return std::clamp(v, -1.0f, 1.0f);
            }
            if (bitsPerSample == 16) {
                int16_t s = static_cast<int16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
                return int16ToFloat(s);
            }
            if (bitsPerSample == 24) {
                int32_t v = static_cast<int32_t>(p[0]) |
                            (static_cast<int32_t>(p[1]) << 8) |
                            (static_cast<int32_t>(p[2]) << 16);
                if (v & 0x800000) v |= 0xFF000000;
                return static_cast<float>(v) / 8388608.0f;
            }
            // 32-bit PCM
            int32_t v = static_cast<int32_t>(
                static_cast<uint32_t>(p[0]) |
                (static_cast<uint32_t>(p[1]) << 8) |
                (static_cast<uint32_t>(p[2]) << 16) |
                (static_cast<uint32_t>(p[3]) << 24));
            return static_cast<float>(static_cast<double>(v) / 2147483648.0);
        };

        for (size_t i = 0; i < numFrames; ++i) {
            const uint8_t* framePtr = ptr + i * frameSize;
            float l = readSample(framePtr);
            float r = (numChannels >= 2) ? readSample(framePtr + bytesPerSample) : l;
            outL[i] = l;
            outR[i] = r;
        }

        return true;
    }

    static bool saveWav16(const std::wstring& path,
                          const std::vector<float>& L,
                          const std::vector<float>& R,
                          uint32_t sampleRate) {
        size_t numFrames = std::min(L.size(), R.size());
        if (numFrames == 0 || sampleRate == 0) return false;

        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return false;

        uint32_t dataBytes = static_cast<uint32_t>(numFrames * 2 * sizeof(int16_t));
        uint32_t riffSize = 36 + dataBytes;
        uint32_t byteRate = sampleRate * 2 * sizeof(int16_t);
        uint16_t blockAlign = 4;
        uint16_t bitsPerSample = 16;
        uint16_t pcmFormat = 1;
        uint16_t channels = 2;
        uint32_t fmtSize = 16;

        auto writeU32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
        auto writeU16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };

        fwrite("RIFF", 1, 4, f);
        writeU32(riffSize);
        fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f);
        writeU32(fmtSize);
        writeU16(pcmFormat);
        writeU16(channels);
        writeU32(sampleRate);
        writeU32(byteRate);
        writeU16(blockAlign);
        writeU16(bitsPerSample);
        fwrite("data", 1, 4, f);
        writeU32(dataBytes);

        constexpr size_t CHUNK = 4096;
        std::vector<int16_t> buf(CHUNK * 2);
        size_t pos = 0;
        while (pos < numFrames) {
            size_t count = std::min(CHUNK, numFrames - pos);
            for (size_t i = 0; i < count; ++i) {
                buf[i * 2] = floatToInt16(L[pos + i]);
                buf[i * 2 + 1] = floatToInt16(R[pos + i]);
            }
            fwrite(buf.data(), sizeof(int16_t) * 2, count, f);
            pos += count;
        }

        fclose(f);
        return true;
    }
};

} // namespace LMAC
