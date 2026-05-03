#include "rtp_packet.h"

std::vector<uint8_t> RtpPacket::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(12 + payload.size());

    // Byte 0: V=2, P, X, CC=0
    uint8_t b0 = static_cast<uint8_t>((version & 0x03) << 6);
    if (padding)   b0 |= 0x20;
    if (extension) b0 |= 0x10;
    buf.push_back(b0);

    // Byte 1: M, PT
    uint8_t b1 = payload_type & 0x7F;
    if (marker) b1 |= 0x80;
    buf.push_back(b1);

    // Bytes 2-3: sequence
    buf.push_back(static_cast<uint8_t>(sequence >> 8));
    buf.push_back(static_cast<uint8_t>(sequence & 0xFF));

    // Bytes 4-7: timestamp (big-endian)
    buf.push_back(static_cast<uint8_t>(timestamp >> 24));
    buf.push_back(static_cast<uint8_t>((timestamp >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((timestamp >>  8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(timestamp & 0xFF));

    // Bytes 8-11: SSRC (big-endian)
    buf.push_back(static_cast<uint8_t>(ssrc >> 24));
    buf.push_back(static_cast<uint8_t>((ssrc >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ssrc >>  8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(ssrc & 0xFF));

    buf.insert(buf.end(), payload.begin(), payload.end());
    return buf;
}

bool RtpPacket::parse(const uint8_t* data, size_t len) {
    if (len < 12) return false;

    version       = (data[0] >> 6) & 0x03;
    if (version != 2) return false;
    padding       = (data[0] & 0x20) != 0;
    extension     = (data[0] & 0x10) != 0;
    uint8_t cc    = data[0] & 0x0F;

    marker        = (data[1] & 0x80) != 0;
    payload_type  = data[1] & 0x7F;

    sequence      = static_cast<uint16_t>((data[2] << 8) | data[3]);
    timestamp     = (static_cast<uint32_t>(data[4]) << 24) |
                    (static_cast<uint32_t>(data[5]) << 16) |
                    (static_cast<uint32_t>(data[6]) <<  8) |
                     static_cast<uint32_t>(data[7]);
    ssrc          = (static_cast<uint32_t>(data[8])  << 24) |
                    (static_cast<uint32_t>(data[9])  << 16) |
                    (static_cast<uint32_t>(data[10]) <<  8) |
                     static_cast<uint32_t>(data[11]);

    size_t header_len = 12 + cc * 4;
    if (len < header_len) return false;

    if (extension) {
        if (len < header_len + 4) return false;
        uint16_t ext_len = static_cast<uint16_t>((data[header_len + 2] << 8) | data[header_len + 3]);
        header_len += 4 + ext_len * 4;
    }

    if (len < header_len) return false;

    size_t payload_len = len - header_len;
    if (padding && payload_len > 0) {
        uint8_t pad_count = data[len - 1];
        if (pad_count > payload_len) return false;
        payload_len -= pad_count;
    }

    payload.assign(data + header_len, data + header_len + payload_len);
    return true;
}
