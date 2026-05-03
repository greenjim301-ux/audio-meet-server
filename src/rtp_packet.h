#pragma once
#include <cstdint>
#include <vector>

// Minimal RTP packet builder/parser
// RFC 3550

struct RtpPacket {
    uint8_t  version   = 2;
    bool     padding   = false;
    bool     extension = false;
    bool     marker    = false;
    uint8_t  payload_type = 8; // PCMA (G.711 A-law)
    uint16_t sequence  = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc      = 0;
    std::vector<uint8_t> payload;

    // Serialize to bytes (fixed header, no CSRC, no extension)
    std::vector<uint8_t> serialize() const;

    // Parse from bytes; returns false if malformed
    bool parse(const uint8_t* data, size_t len);
};
