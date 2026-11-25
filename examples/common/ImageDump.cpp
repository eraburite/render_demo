#include "ImageDump.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

// Memory layout of one texel, for the formats the dump path understands.
struct TexelLayout {
    bool floatFormat = false;
    bool bgra = false;
    uint32_t channels = 4;
};

TexelLayout layoutOf(vk::Format format)
{
    switch (format) {
        case vk::Format::eR8G8B8A8Unorm:
        case vk::Format::eR8G8B8A8Srgb: return TexelLayout{false, false, 4};
        case vk::Format::eB8G8R8A8Unorm:
        case vk::Format::eB8G8R8A8Srgb: return TexelLayout{false, true, 4};
        case vk::Format::eR32G32B32A32Sfloat: return TexelLayout{true, false, 4};
        case vk::Format::eR32Sfloat:
        case vk::Format::eD32Sfloat: return TexelLayout{true, false, 1};
        default: break;
    }
    throw std::runtime_error("ImageDump: unsupported format " + vk::to_string(format));
}

glm::vec4 readTexel(const uint8_t *texel, const TexelLayout &layout)
{
    if (layout.floatFormat) {
        const float *values = reinterpret_cast<const float *>(texel);
        if (layout.channels == 1) {
            // Single-channel targets (depth, R32F AOV): replicate for display.
            return glm::vec4(values[0], values[0], values[0], 1.0f);
        }
        return glm::vec4(values[0], values[1], values[2], values[3]);
    }

    const float r = static_cast<float>(texel[0]) / 255.0f;
    const float g = static_cast<float>(texel[1]) / 255.0f;
    const float b = static_cast<float>(texel[2]) / 255.0f;
    const float a = static_cast<float>(texel[3]) / 255.0f;
    return layout.bgra ? glm::vec4(b, g, r, a) : glm::vec4(r, g, b, a);
}

// Same mapping as the shaders (see DebugViewMode.h).
glm::vec3 encodeColor(const glm::vec4 &sample, const DumpRequest &request)
{
    switch (request.encoding) {
        case DumpEncoding::eColor: return glm::vec3(sample);
        case DumpEncoding::eNormal: return glm::vec3(sample) * 0.5f + 0.5f;
        case DumpEncoding::ePosition: return glm::vec3(sample) / request.referenceScale * 0.5f + 0.5f;
        case DumpEncoding::eLinearDepth: {
            const float distance = glm::length(glm::vec3(sample) - request.eyePosition);
            const float span = std::max(request.farPlane - request.nearPlane, 1e-4f);
            const float t = std::clamp((distance - request.nearPlane) / span, 0.0f, 1.0f);
            return glm::vec3(1.0f - t);
        }
    }
    return glm::vec3(sample);
}

uint8_t toByte(float value)
{
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

std::filesystem::path makePath(const std::filesystem::path &directory, uint32_t index, const std::string &label)
{
    std::ostringstream name;
    name << std::setw(4) << std::setfill('0') << index;
    if (!label.empty()) {
        name << '_' << label;
    }
    name << ".ppm";
    return directory / name.str();
}

// PPM (P6) allows '#' comments between the magic number and the dimensions,
// so the file can describe itself: a bare PPM has nowhere else to put the
// encoding rule.  Shared with raw2rgb, which records its own provenance.
void writePpm(const std::filesystem::path &path, uint32_t width, uint32_t height,
              const std::vector<uint8_t> &rgb, const std::vector<std::string> &comments)
{
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    if (rgb.size() != expected) {
        throw std::runtime_error("ImageDump: ppm buffer is " + std::to_string(rgb.size()) + " bytes, expected " +
                                 std::to_string(expected) + " for " + std::to_string(width) + "x" +
                                 std::to_string(height));
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("ImageDump: cannot open " + path.string() + " for writing");
    }
    file << "P6\n";
    for (const std::string &comment : comments) {
        file << "# " << comment << "\n";
    }
    file << width << ' ' << height << "\n";
    file << "255\n";
    file.write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    if (!file) {
        throw std::runtime_error("ImageDump: failed while writing " + path.string());
    }
}

} // namespace

void WritePpmRgb8(const std::filesystem::path &path, uint32_t width, uint32_t height,
                  const std::vector<uint8_t> &rgb, const std::vector<std::string> &comments)
{
    writePpm(path, width, height, rgb, comments);
}

std::vector<DumpResult> DumpImages(ImageReadback &readback, const std::vector<DumpRequest> &requests,
                                   const std::filesystem::path &directory, uint32_t firstIndex)
{
    if (requests.empty()) {
        return {};
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::runtime_error("ImageDump: cannot create directory " + directory.string() + ": " + error.message());
    }

    std::vector<DumpResult> results;
    results.reserve(requests.size());

    uint32_t index = firstIndex;
    for (const DumpRequest &request : requests) {
        if (!request.image) {
            throw std::runtime_error("ImageDump: request '" + request.label + "' has a null image");
        }

        const ImageReadback::Capture capture =
            readback.capture(request.image, request.format, request.extent, request.layout);
        const TexelLayout texelLayout = layoutOf(request.format);
        const uint32_t texelSize = ImageReadback::texelSize(request.format);

        DumpResult result;
        result.path = makePath(directory, index, request.label);
        result.extent = request.extent;

        const size_t texelCount = static_cast<size_t>(request.extent.width) * request.extent.height;
        std::vector<uint8_t> rgb(texelCount * 3);

        double covered = 0.0;
        double normalLengthSum = 0.0;
        glm::vec3 colorSum(0.0f);
        for (size_t i = 0; i < texelCount; ++i) {
            const glm::vec4 sample = readTexel(capture.pixels.data() + i * texelSize, texelLayout);

            // The G-Buffer writes alpha 1 for covered pixels and the clear colour
            // uses alpha 0, so alpha doubles as a coverage mask.  Uncovered texels
            // are written as black instead of their cleared value: a cleared
            // position channel encodes to mid-grey, which makes the background of
            // a position / depth dump look like geometry.
            const bool isCovered = sample.a >= 0.5f;
            const glm::vec3 encoded = isCovered ? encodeColor(sample, request) : glm::vec3(0.0f);

            rgb[i * 3 + 0] = toByte(encoded.r);
            rgb[i * 3 + 1] = toByte(encoded.g);
            rgb[i * 3 + 2] = toByte(encoded.b);
            colorSum += glm::clamp(encoded, 0.0f, 1.0f);

            if (isCovered) {
                covered += 1.0;
                if (request.encoding == DumpEncoding::eNormal) {
                    normalLengthSum += static_cast<double>(glm::length(glm::vec3(sample)));
                }
            }
        }

        const float invCount = texelCount > 0 ? 1.0f / static_cast<float>(texelCount) : 0.0f;
        result.alphaCoverage = static_cast<float>(covered) * invCount;
        result.meanColor = colorSum * invCount;
        if (request.encoding == DumpEncoding::eNormal && covered > 0.0) {
            result.meanNormalLength = static_cast<float>(normalLengthSum / covered);
        }

        const std::vector<std::string> comments = {
            "render_demo debug capture",
            "channel: " + request.label,
            std::string("encoding: ") + DumpEncodingName(request.encoding),
            "source: " + vk::to_string(request.format),
            "background: alpha < 0.5 written as black",
            "row order: image row 0 first (no vertical flip)",
        };
        writePpm(result.path, result.extent.width, result.extent.height, rgb, comments);
        results.push_back(std::move(result));
        ++index;
    }
    return results;
}

DebugCaptureOptions ParseDebugCaptureOptions(int argc, char **argv)
{
    DebugCaptureOptions options;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string flag = argv[i];
        if (flag != "--capture" && flag != "--view") {
            continue;
        }
        unsigned long value = 0;
        try {
            value = std::stoul(argv[i + 1]);
        } catch (const std::exception &) {
            throw std::runtime_error(flag + " expects a number, got '" + argv[i + 1] + "'");
        }
        if (flag == "--capture") {
            // A few frames are enough for the pipeline to be warm; the upper
            // bound only guards against a typo turning into a very long run.
            options.framesBeforeCapture = static_cast<uint32_t>(std::clamp<unsigned long>(value, 1, 600));
        } else {
            options.initialViewIndex = static_cast<uint32_t>(std::min<unsigned long>(value, 9));
            options.viewSpecified = true;
        }
    }
    return options;
}
