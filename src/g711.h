#pragma once
#include <cstdint>
#include <vector>

class G711Alaw
{
public:
    // Decodes a single A-law byte to a 16-bit signed PCM sample
    static inline int16_t decodeSample(uint8_t alaw)
    {
        alaw ^= 0x55; // Standard toggle of even bits
        int t = (alaw & 0x0F) << 4;
        int seg = (alaw & 0x70) >> 4;
        switch (seg)
        {
        case 0:
            t += 8;
            break;
        case 1:
            t += 0x108;
            break;
        default:
            t += 0x108;
            t <<= seg - 1;
        }
        return (alaw & 0x80) ? t : -t;
    }

    // Encodes a 16-bit signed PCM sample to an A-law byte
    static inline uint8_t encodeSample(int16_t pcm)
    {
        int sign = (pcm & 0x8000) >> 8;
        if (sign != 0)
            pcm = -pcm;
        if (pcm > 32635)
            pcm = 32635;

        int seg = 0;
        int low_level_pcm = pcm;
        if (low_level_pcm >= 256)
        {
            static const int seg_end[8] = {
                0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};
            for (seg = 1; seg < 8; seg++)
            {
                if (low_level_pcm <= seg_end[seg])
                    break;
            }
            low_level_pcm >>= (seg + 3);
        }
        else
        {
            low_level_pcm >>= 4;
        }

        uint8_t alaw = (uint8_t)(sign | (seg << 4) | (low_level_pcm & 0x0F));
        return alaw ^ 0x55;
    }

    // Buffer processing for maximum throughput
    static void decodeBuffer(const uint8_t *src, int16_t *dst, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            dst[i] = decodeSample(src[i]);
        }
    }

    static void encodeBuffer(const int16_t *src, uint8_t *dst, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            dst[i] = encodeSample(src[i]);
        }
    }
};