#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// ---------------------------------------------------------------------------
// Shared little-endian binary reader for the MMD formats (PMX / PMD / VMD).
//
// Saba has one hand-rolled `File::Read<T>` per format.  Folding those into a
// single cursor buys three things the originals did not have:
//   * every read is bounds-checked and throws with the byte offset, so a
//     truncated/corrupt file cannot walk off the buffer;
//   * the 1/2/4-byte "index size" indirection that PMX puts in its header is
//     one method instead of a switch at every call site;
//   * string decoding (UTF-16LE for PMX, Shift-JIS for PMD/VMD) is shared.
// ---------------------------------------------------------------------------
namespace modelio {

inline void appendUtf8(std::string &out, uint32_t codepoint)
{
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

// Decode a Shift-JIS (CP932-ish) byte string into UTF-8 without pulling a
// conversion table into the tree.  ASCII passes through; half-width katakana
// maps exactly; other multi-byte sequences become U+FFFD.  MMD names are only
// used for material/bone labels and logs, so losing the odd CJK glyph is
// harmless while keeping every std::string valid UTF-8.
inline std::string decodeShiftJis(const uint8_t *data, size_t len)
{
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        if (b == 0) break;
        if (b < 0x80) {
            if (b >= 0x20) out.push_back(static_cast<char>(b));
            continue;
        }
        if (b >= 0xA1 && b <= 0xDF) {
            appendUtf8(out, 0xFF61u + (b - 0xA1u)); // half-width katakana
            continue;
        }
        if (((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xEF)) && i + 1 < len) {
            ++i; // consume the trail byte
            appendUtf8(out, 0xFFFDu);
        } else {
            appendUtf8(out, 0xFFFDu);
        }
    }
    return out;
}

inline std::string utf16LeToUtf8(const uint8_t *data, size_t len)
{
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t codepoint = static_cast<uint32_t>(data[i]) | (static_cast<uint32_t>(data[i + 1]) << 8);
        if (codepoint == 0) break;
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF && i + 3 < len) {
            const uint32_t low = static_cast<uint32_t>(data[i + 2]) | (static_cast<uint32_t>(data[i + 3]) << 8);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
                i += 2;
            }
        }
        appendUtf8(out, codepoint);
    }
    return out;
}

class BinaryReader {
public:
    BinaryReader(const uint8_t *data, size_t size) : m_data(data), m_size(size) {}

    void require(size_t bytes) const
    {
        if (m_pos + bytes > m_size) {
            throw std::runtime_error("model binary reader: truncated at byte " + std::to_string(m_pos) +
                                     " (needed " + std::to_string(bytes) + ", have " +
                                     std::to_string(m_size - m_pos) + ")");
        }
    }

    template <typename T>
    T pod()
    {
        require(sizeof(T));
        T value{};
        std::memcpy(&value, m_data + m_pos, sizeof(T));
        m_pos += sizeof(T);
        return value;
    }

    uint8_t u8() { return pod<uint8_t>(); }
    int8_t i8() { return pod<int8_t>(); }
    uint16_t u16() { return pod<uint16_t>(); }
    int16_t i16() { return pod<int16_t>(); }
    uint32_t u32() { return pod<uint32_t>(); }
    int32_t i32() { return pod<int32_t>(); }
    float f32() { return pod<float>(); }

    glm::vec2 vec2() { return glm::vec2(f32(), f32()); }
    glm::vec3 vec3() { return glm::vec3(f32(), f32(), f32()); }
    glm::vec4 vec4() { return glm::vec4(f32(), f32(), f32(), f32()); }
    glm::quat quat() { return glm::quat(f32(), f32(), f32(), f32()); } // memory order x,y,z,w

    void skip(size_t bytes)
    {
        require(bytes);
        m_pos += bytes;
    }

    // PMD/VMD: fixed-width, NUL-padded Shift-JIS field.
    std::string fixedString(size_t bytes)
    {
        require(bytes);
        const uint8_t *start = m_data + m_pos;
        size_t length = 0;
        while (length < bytes && start[length] != 0) ++length;
        std::string result = decodeShiftJis(start, length);
        m_pos += bytes;
        return result;
    }

    // PMX: int32 byte length followed by that many bytes, encoded per header.
    std::string lengthString(bool utf16)
    {
        const int32_t byteLength = i32();
        if (byteLength < 0) throw std::runtime_error("model binary reader: negative PMX string length");
        require(static_cast<size_t>(byteLength));
        const uint8_t *start = m_data + m_pos;
        std::string result = utf16 ? utf16LeToUtf8(start, static_cast<size_t>(byteLength))
                                   : decodeShiftJis(start, static_cast<size_t>(byteLength));
        m_pos += static_cast<size_t>(byteLength);
        return result;
    }

    // PMX: index size is 1, 2 or 4 bytes; the all-ones pattern means "none".
    int32_t signedIndex(uint8_t indexSize)
    {
        switch (indexSize) {
        case 1: {
            const uint8_t v = u8();
            return v == 0xFF ? -1 : static_cast<int32_t>(v);
        }
        case 2: {
            const uint16_t v = u16();
            return v == 0xFFFF ? -1 : static_cast<int32_t>(v);
        }
        case 4: {
            const uint32_t v = u32();
            return v == 0xFFFFFFFFu ? -1 : static_cast<int32_t>(v);
        }
        default:
            throw std::runtime_error("model binary reader: invalid PMX index size " +
                                    std::to_string(indexSize));
        }
    }

    uint32_t unsignedIndex(uint8_t indexSize)
    {
        switch (indexSize) {
        case 1: return u8();
        case 2: return u16();
        case 4: return u32();
        default:
            throw std::runtime_error("model binary reader: invalid PMX index size " +
                                    std::to_string(indexSize));
        }
    }

    size_t offset() const { return m_pos; }
    size_t remaining() const { return m_size - m_pos; }
    size_t size() const { return m_size; }
    bool eof() const { return m_pos >= m_size; }
    const uint8_t *data() const { return m_data; }

private:
    const uint8_t *m_data;
    size_t m_size;
    size_t m_pos = 0;
};

inline std::vector<uint8_t> readBinaryFile(const std::string &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("model loader: cannot open '" + path + "'");
    const std::streamoff size = file.tellg();
    if (size <= 0) throw std::runtime_error("model loader: empty file '" + path + "'");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(bytes.data()), size);
    if (file.gcount() != size) throw std::runtime_error("model loader: short read on '" + path + "'");
    return bytes;
}

inline bool startsWith(const uint8_t *data, size_t size, const char *magic)
{
    const size_t n = std::strlen(magic);
    return size >= n && std::memcmp(data, magic, n) == 0;
}

} // namespace modelio
