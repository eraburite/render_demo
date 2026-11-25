// raw2rgb: offline RAW12 -> unpacked RAW16 -> RGB float image.  Both compute
// stages share one asset and one (single-buffered) descriptor layout, and run in
// a single submission (CmdOpGroup) with an explicit compute -> compute barrier
// between them: back-to-back dispatches are ordered for execution, but the SSBO
// writes of the unpack stage only become visible to the demosaic stage after a
// memory dependency.
//
// The GPU output is compared against a CPU reference of the exact same unpack +
// bilinear demosaic, so this is a test and not just a file dump: a wrong word
// order, a swapped Bayer phase, a broken row stride or an off-by-one read all
// show up as a non-zero error in the log.
//
// Command line:
//   --width <n> --height <n>   image size, both even (default 64 x 64)
//   --input <file>             packed RAW12 input; synthetic pattern when omitted
//   --out <dir>                output directory (default <root>/assets/images)
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"
#include "ImageDump.h"

#include "AttachmentDetailConfig.h"
#include "CmdOpComputeBarrier.h"
#include "CmdOpComputeDispatch.h"
#include "CmdOpGroup.h"
#include "CmdSyncDevice.h"
#include "ComputePipeline.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "Raw2RgbLayoutConfig.h"
#include "root_directory.h"

#include "shader/raw2rgb_v2_comp.hpp"
#include "shader/rawunpack_comp.hpp"

namespace {

constexpr uint32_t LOCAL_SIZE = 256;
// Workgroup sizes and dispatch counts are derived from these, so the buffers,
// the CPU reference and the shaders always agree on the same extent.
struct ImageSize {
    uint32_t width = 64;
    uint32_t height = 64;

    uint32_t pixelCount() const { return width * height; }
    uint32_t wordCount() const { return pixelCount() / 2u; }        // two 12/16-bit samples per word
    uint32_t raw12Bytes() const { return pixelCount() * 3u / 2u; }  // 12 bits per sample
    uint32_t rgbFloatCount() const { return pixelCount() * 3u; }
    std::string toString() const { return std::to_string(width) + "x" + std::to_string(height); }
};

struct AppOptions {
    ImageSize size{};
    std::string inputPath;
    std::filesystem::path outputDir;
};

// Push constant payload of both compute stages (see `Params` in the shaders).
struct RawParams {
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(RawParams) == 8, "RawParams must match the push constant block");

uint32_t parseCount(const char *flag, const char *text)
{
    try {
        return static_cast<uint32_t>(std::stoul(text));
    } catch (const std::exception &) {
        throw std::runtime_error(std::string(flag) + " expects a positive integer, got '" + text + "'");
    }
}

AppOptions parseOptions(int argc, char **argv)
{
    AppOptions options;
    options.outputDir = std::filesystem::path(std::string(PROJECT_ROOT_PATH)) / "assets" / "images";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--width" && hasValue) {
            options.size.width = parseCount("--width", argv[++i]);
        } else if (arg == "--height" && hasValue) {
            options.size.height = parseCount("--height", argv[++i]);
        } else if (arg == "--input" && hasValue) {
            options.inputPath = argv[++i];
        } else if (arg == "--out" && hasValue) {
            options.outputDir = std::filesystem::path(argv[++i]);
        } else {
            std::cout << "[raw2rgb] ignoring argument '" << arg << "'\n";
        }
    }
    // The Bayer phase is decided by (x & 1, y & 1) and the packing pairs samples
    // along x, so both dimensions must be even for either stage to be coherent.
    if (options.size.width == 0 || options.size.height == 0 || (options.size.width & 1u) != 0 ||
        (options.size.height & 1u) != 0) {
        throw std::runtime_error("--width/--height must be non-zero and even");
    }
    if (options.size.width < 4 || options.size.height < 4) {
        throw std::runtime_error("--width/--height must be at least 4 (demosaic needs neighbours)");
    }
    return options;
}

// Smooth per-channel ramps, mosaiced as RGGB.  Smoothness matters: the shader's
// green estimate is a 4-tap average, so on a smooth image it is almost exact and
// the CPU/GPU comparison can use a tight tolerance.  A checkerboard would make
// the two differ legitimately at the edges.
std::vector<uint8_t> makeSyntheticRaw12(const ImageSize &size)
{
    std::vector<uint16_t> samples(size.pixelCount());
    for (uint32_t y = 0; y < size.height; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(size.height - 1);
        for (uint32_t x = 0; x < size.width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(size.width - 1);
            const float r = 0.05f + 0.90f * u;
            const float g = 0.05f + 0.90f * v;
            const float b = 0.05f + 0.90f * (1.0f - 0.5f * (u + v));
            const bool evenX = (x & 1u) == 0u;
            const bool evenY = (y & 1u) == 0u;
            // RGGB: (even,even) = R, (odd,odd) = B, everything else = G.
            const float value = (evenX && evenY) ? r : ((!evenX && !evenY) ? b : g);
            samples[static_cast<size_t>(y) * size.width + x] =
                static_cast<uint16_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 4095.0f));
        }
    }

    // Packed RAW12: two 12-bit samples per 3 bytes, little-endian, low sample
    // first -- the layout a sensor dump actually has on disk.
    std::vector<uint8_t> raw(static_cast<size_t>(size.raw12Bytes()), 0u);
    for (uint32_t word = 0; word < size.wordCount(); ++word) {
        const uint32_t a = samples[static_cast<size_t>(word) * 2u + 0u];
        const uint32_t b = samples[static_cast<size_t>(word) * 2u + 1u];
        const uint32_t packed = a | (b << 12u);
        raw[static_cast<size_t>(word) * 3u + 0u] = static_cast<uint8_t>(packed & 0xFFu);
        raw[static_cast<size_t>(word) * 3u + 1u] = static_cast<uint8_t>((packed >> 8u) & 0xFFu);
        raw[static_cast<size_t>(word) * 3u + 2u] = static_cast<uint8_t>((packed >> 16u) & 0xFFu);
    }
    return raw;
}

// The shaders address the packed input as uint32 words (one word per two
// samples), so the compact 3-byte-per-word stream is expanded to one word per
// pair before upload: `a` in bits 0..11, `b` in bits 12..23, top byte zero.
// Doing this on the host keeps rawunpack.comp assuming a word array, which is
// what the SSBO binding guarantees anyway.
std::vector<uint8_t> expandRaw12ToWords(const std::vector<uint8_t> &raw12, uint32_t wordCount)
{
    std::vector<uint8_t> words(static_cast<size_t>(wordCount) * sizeof(uint32_t), 0u);
    for (uint32_t word = 0; word < wordCount; ++word) {
        const size_t byteIndex = static_cast<size_t>(word) * 3u;
        const uint32_t a = static_cast<uint32_t>(raw12[byteIndex + 0u]) |
                           ((static_cast<uint32_t>(raw12[byteIndex + 1u]) & 0x0Fu) << 8u);
        const uint32_t b = (static_cast<uint32_t>(raw12[byteIndex + 1u]) >> 4u) |
                           (static_cast<uint32_t>(raw12[byteIndex + 2u]) << 4u);
        const uint32_t packed = a | (b << 12u);
        words[static_cast<size_t>(word) * 4u + 0u] = static_cast<uint8_t>(packed & 0xFFu);
        words[static_cast<size_t>(word) * 4u + 1u] = static_cast<uint8_t>((packed >> 8u) & 0xFFu);
        words[static_cast<size_t>(word) * 4u + 2u] = static_cast<uint8_t>((packed >> 16u) & 0xFFu);
        words[static_cast<size_t>(word) * 4u + 3u] = 0u;
    }
    return words;
}

std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("cannot open input '" + path + "'");
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("input '" + path + "' is empty");
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char *>(bytes.data()), size)) {
        throw std::runtime_error("failed while reading input '" + path + "'");
    }
    return bytes;
}

// ---- CPU reference: mirrors the two shaders operation for operation --------

std::vector<uint32_t> unpackReference(const std::vector<uint8_t> &raw)
{
    std::vector<uint32_t> words(raw.size() / 4u, 0u);
    std::memcpy(words.data(), raw.data(), words.size() * sizeof(uint32_t));
    std::vector<uint32_t> unpacked(words.size(), 0u);
    for (size_t i = 0; i < words.size(); ++i) {
        const uint32_t word = words[i];
        const uint32_t a = (word & 0x00000FFFu) << 4u;
        const uint32_t b = (word >> 12u) << 4u;
        unpacked[i] = a | (b << 16u);
    }
    return unpacked;
}

std::vector<float> demosaicReference(const std::vector<uint32_t> &unpacked, const ImageSize &size)
{
    const auto sample16 = [&](uint32_t x, uint32_t y) {
        const uint32_t wordIndex = (y * size.width + x) / 2u;
        const uint32_t word = unpacked[wordIndex];
        const uint32_t value = ((x & 1u) == 0u) ? (word & 0x0000FFFFu) : (word >> 16u);
        return static_cast<float>(value) / 65535.0f;
    };

    std::vector<float> rgb(static_cast<size_t>(size.rgbFloatCount()), 0.0f);
    for (uint32_t y = 0; y < size.height; ++y) {
        for (uint32_t x = 0; x < size.width; ++x) {
            const uint32_t xm = x > 0u ? x - 1u : x;
            const uint32_t xp = x < size.width - 1u ? x + 1u : x;
            const uint32_t ym = y > 0u ? y - 1u : y;
            const uint32_t yp = y < size.height - 1u ? y + 1u : y;

            const float g = 0.25f * (sample16(xm, y) + sample16(xp, y) + sample16(x, ym) + sample16(x, yp));
            const float base = sample16(x, y);
            float r = 0.0f;
            float gr = 0.0f;
            float b = 0.0f;
            if ((y & 1u) == 0u) {
                if ((x & 1u) == 0u) {
                    r = base;
                    b = g;
                    gr = g;
                } else {
                    r = g;
                    b = g;
                    gr = base;
                }
            } else {
                if ((x & 1u) == 0u) {
                    r = g;
                    b = g;
                    gr = base;
                } else {
                    r = g;
                    b = base;
                    gr = g;
                }
            }
            const size_t index = (static_cast<size_t>(y) * size.width + x) * 3u;
            rgb[index + 0u] = r;
            rgb[index + 1u] = gr;
            rgb[index + 2u] = b;
        }
    }
    return rgb;
}

struct ChannelDelta {
    float maxAbs = 0.0f;
    double sumSquares = 0.0;
    size_t count = 0;
    size_t worstIndex = 0;

    float rms() const { return count > 0 ? static_cast<float>(std::sqrt(sumSquares / static_cast<double>(count))) : 0.0f; }
};

ChannelDelta compareFloats(const std::vector<float> &gpu, const std::vector<float> &reference)
{
    ChannelDelta delta;
    const size_t count = std::min(gpu.size(), reference.size()) / 3u * 3u;
    delta.count = count;
    for (size_t i = 0; i < count; ++i) {
        const double diff = static_cast<double>(gpu[i]) - static_cast<double>(reference[i]);
        delta.sumSquares += diff * diff;
        const float absDiff = std::fabs(gpu[i] - reference[i]);
        if (absDiff > delta.maxAbs) {
            delta.maxAbs = absDiff;
            delta.worstIndex = i;
        }
    }
    return delta;
}

uint8_t toByte(float value)
{
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
}

std::string formatFloat(float value)
{
    std::ostringstream out;
    out << std::setprecision(3) << std::scientific << value;
    return out.str();
}

} // namespace

void test(int argc, char **argv)
{
    const AppOptions options = parseOptions(argc, argv);
    const ImageSize &size = options.size;

    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
    GLFWwindow window(800, 600, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // ---- single-buffered compute layout ----------------------------------
    PushConstantDetail push{static_cast<uint32_t>(sizeof(RawParams)), vk::ShaderStageFlagBits::eCompute, 0};
    auto layout = std::make_shared<Raw2RgbLayoutAtlas>(
        std::vector<Raw2RgbComponent>{Raw2RgbComponent::eRawImage, Raw2RgbComponent::eUnpackedImage,
                                      Raw2RgbComponent::eRgbImage},
        MAX_FRAMES_IN_FLIGHT, true, push);
    layout->setSpec(0, GBufferSpec{size.wordCount(), static_cast<uint32_t>(sizeof(uint32_t)), DataTypes::eUnsignedInt});
    layout->setSpec(1, GBufferSpec{size.wordCount(), static_cast<uint32_t>(sizeof(uint32_t)), DataTypes::eUnsignedInt});
    layout->setSpec(2, GBufferSpec{size.rgbFloatCount(), static_cast<uint32_t>(sizeof(float)), DataTypes::eFloat});

    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("rawunpack", std::vector<uint32_t>(shader::RAWUNPACK_COMP_SPV.begin(),
                                                                shader::RAWUNPACK_COMP_SPV.end()));
    asset->addShaderSpirvCode("raw2rgb", std::vector<uint32_t>(shader::RAW2RGB_V2_COMP_SPV.begin(),
                                                              shader::RAW2RGB_V2_COMP_SPV.end()));

    // ---- input ------------------------------------------------------------
    std::vector<uint8_t> raw12;
    const bool synthetic = options.inputPath.empty();
    if (synthetic) {
        raw12 = makeSyntheticRaw12(size);
    } else {
        raw12 = readFile(options.inputPath);
        const size_t expected = size.raw12Bytes();
        if (raw12.size() < expected) {
            throw std::runtime_error("input '" + options.inputPath + "' holds " + std::to_string(raw12.size()) +
                                     " bytes, " + std::to_string(expected) + " are needed for " + size.toString());
        }
        if (raw12.size() > expected) {
            std::cout << "[raw2rgb] input has " << (raw12.size() - expected) << " trailing bytes, ignored\n";
            raw12.resize(expected);
        }
    }

    // Expand the compact RAW12 stream to word pairs once; the shader (and the
    // CPU reference below) then work on the same word buffer that is uploaded.
    const std::vector<uint8_t> rawWords = expandRaw12ToWords(raw12, size.wordCount());

    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceId(0, 0), 0u, rawWords);
    upload->submit(syncOp, asset);

    const RawParams params{size.width, size.height};
    asset->UpdateConstantsResource(0, &params, static_cast<uint32_t>(sizeof(params)));

    // ---- two compute pipelines over the same asset -----------------------
    auto unpackPipeline = mgr->CreateComputePipeline(layout, asset, "rawunpack");
    auto rgbPipeline = mgr->CreateComputePipeline(layout, asset, "raw2rgb");

    // One submission for both stages; the barrier in between is what makes the
    // unpacked SSBO visible to the demosaic stage.  Without it the chain relies
    // on submission order alone, which only orders execution.
    auto chain = std::make_shared<CmdOpGroup>();
    chain->add(std::make_shared<CmdOpComputeDispatch>(
        unpackPipeline, DispatchSize::FromInvocationCount(size.wordCount(), LOCAL_SIZE)));
    chain->add(std::make_shared<CmdOpComputeBarrier>());
    chain->add(std::make_shared<CmdOpComputeDispatch>(
        rgbPipeline, DispatchSize::FromInvocationCount(size.pixelCount(), LOCAL_SIZE)));

    auto once = mgr->UploadSequence();
    once->submit(chain, asset);

    // ---- read back results ------------------------------------------------
    const auto rgbBytes = once->ReadBack(asset, ResourceKey::make_key(ResourceId(0, 2), 0),
                                        size.rgbFloatCount() * static_cast<uint32_t>(sizeof(float)));
    const auto unpackedBytes = once->ReadBack(asset, ResourceKey::make_key(ResourceId(0, 1), 0),
                                              size.wordCount() * static_cast<uint32_t>(sizeof(uint32_t)));
    if (rgbBytes.size() < size.rgbFloatCount() * sizeof(float) || unpackedBytes.empty()) {
        throw std::runtime_error("raw2rgb readback returned no data");
    }

    // ---- verify against the CPU reference --------------------------------
    const std::vector<uint32_t> referenceUnpacked = unpackReference(rawWords);
    size_t unpackMismatches = 0;
    {
        const auto *gpuWords = reinterpret_cast<const uint32_t *>(unpackedBytes.data());
        const size_t wordCount = std::min(unpackedBytes.size() / sizeof(uint32_t),
                                          static_cast<size_t>(size.wordCount()));
        for (size_t i = 0; i < wordCount; ++i) {
            if (gpuWords[i] != referenceUnpacked[i]) {
                ++unpackMismatches;
            }
        }
    }

    std::vector<float> gpuRgb(size.rgbFloatCount(), 0.0f);
    std::memcpy(gpuRgb.data(), rgbBytes.data(),
                std::min(rgbBytes.size(), gpuRgb.size() * sizeof(float)));
    const std::vector<float> referenceRgb = demosaicReference(referenceUnpacked, size);
    const ChannelDelta delta = compareFloats(gpuRgb, referenceRgb);

    std::cout << "[raw2rgb] " << size.toString() << " (" << (synthetic ? "synthetic RGGB" : options.inputPath)
              << ")\n"
              << "[raw2rgb] unpack: " << unpackMismatches << " / " << size.wordCount() << " words differ\n"
              << "[raw2rgb] demosaic: max |gpu-cpu| = " << formatFloat(delta.maxAbs)
              << ", rms = " << formatFloat(delta.rms()) << " over " << delta.count << " channels\n";

    // A driver is allowed to contract a*b+c into an FMA, so bit-identical output
    // is not guaranteed; anything beyond 2e-5 already means the values are being
    // combined differently.  Past 1e-3 it is an algorithmic mismatch, not
    // rounding, and the example fails instead of writing a plausible-looking file.
    constexpr float kRoundingTolerance = 2e-5f;
    constexpr float kFailureTolerance = 1e-3f;
    if (unpackMismatches != 0) {
        throw std::runtime_error("rawunpack output does not match the reference word for word");
    }
    if (delta.maxAbs > kFailureTolerance) {
        throw std::runtime_error("demosaic output differs from the CPU reference (max |gpu-cpu| = " +
                                 formatFloat(delta.maxAbs) + ")");
    }
    if (delta.maxAbs > kRoundingTolerance) {
        std::cout << "[raw2rgb] warning: max error exceeds the rounding tolerance " << kRoundingTolerance
                  << " (still under " << kFailureTolerance << ")\n";
    }

    // ---- write the image out ---------------------------------------------
    std::error_code error;
    std::filesystem::create_directories(options.outputDir, error);
    if (error) {
        throw std::runtime_error("cannot create output directory " + options.outputDir.string() + ": " +
                                 error.message());
    }

    std::vector<uint8_t> rgb8(static_cast<size_t>(size.pixelCount()) * 3u, 0u);
    for (size_t i = 0; i < rgb8.size(); ++i) {
        rgb8[i] = toByte(gpuRgb[i]);
    }
    std::vector<std::string> comments = {
        "render_demo raw2rgb",
        std::string("source: ") + (synthetic ? "synthetic RGGB pattern" : options.inputPath),
        "layout: RAW12 packed 2-per-word -> RAW16 2-per-word -> 3 x float32",
        "demosaic: bilinear green, base sample for the pixel colour",
        "demosaic max |gpu-cpu|: " + formatFloat(delta.maxAbs),
        "row order: image row 0 first (no vertical flip)",
    };
    const std::string stem = "output_rgb_" + size.toString();
    const std::filesystem::path ppmPath = options.outputDir / (stem + ".ppm");
    WritePpmRgb8(ppmPath, size.width, size.height, rgb8, comments);

    const std::filesystem::path raw16Path = options.outputDir / ("output_unpack_raw16_" + size.toString() + ".raw");
    {
        std::ofstream raw16(raw16Path, std::ios::binary);
        if (!raw16) {
            throw std::runtime_error("cannot write " + raw16Path.string());
        }
        raw16.write(reinterpret_cast<const char *>(unpackedBytes.data()),
                    static_cast<std::streamsize>(unpackedBytes.size()));
    }

    // Keep the synthetic input around so it can be fed back through --input.
    if (synthetic) {
        const std::filesystem::path inputCopy = options.outputDir / ("input_raw12_" + size.toString() + ".raw");
        std::ofstream inputFile(inputCopy, std::ios::binary);
        if (inputFile) {
            inputFile.write(reinterpret_cast<const char *>(raw12.data()),
                            static_cast<std::streamsize>(raw12.size()));
        }
    }

    std::cout << "[raw2rgb] wrote " << ppmPath.string() << "\n[raw2rgb] wrote " << raw16Path.string() << "\n";

    // ---- one empty present pass so the window/surface stays valid --------
    auto presentPass = mgr->CreateRenderPass(std::make_shared<SimpleAttachmentDetail>(
        SimpleAttachmentDetail::Mode::eDirect, *mgr->getSwapChain()->getImageFormat(), mgr->DefaultDepthFormat()));
    auto sequence = mgr->DrawFrameSequence();
    if (sequence->prepareFrame()) {
        sequence->beginRecording();
        sequence->recordPass(presentPass, []() {});
        sequence->endRecording();
        sequence->submitFrame();
        (void)sequence->presentFrame();
    }
}

int main(int argc, char **argv)
{
    try {
        test(argc, argv);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
