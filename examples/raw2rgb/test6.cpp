// raw2rgb: offline RAW12 -> unpacked RAW16 -> RGB float image.  Both compute
// stages share one asset and one (single-buffered) descriptor layout.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Camera.h"
#include "GLFWwindow.h"

#include "AttachmentDetailConfig.h"
#include "CmdOpBase.h"
#include "CmdSyncDevice.h"
#include "ComputePipeline.h"
#include "DescriptorBindingState.h"
#include "IRenderPass.h"
#include "LoopSequence.h"
#include "OnceSequence.h"
#include "RenderManager.h"

#include "Raw2RgbLayoutConfig.h"
#include "root_directory.h"

#include "shader/raw2rgb_v2_comp.hpp"
#include "shader/rawunpack_comp.hpp"

namespace {

constexpr uint32_t IMAGE_WIDTH = 64;
constexpr uint32_t IMAGE_HEIGHT = 64;
constexpr uint32_t PIXEL_COUNT = IMAGE_WIDTH * IMAGE_HEIGHT;
constexpr uint32_t RAW12_BYTES = PIXEL_COUNT * 3u / 2u;          // 6144
constexpr uint32_t UNPACKED_WORDS = PIXEL_COUNT / 2u;            // 2048
constexpr uint32_t RGB_FLOAT_COUNT = PIXEL_COUNT * 3u;           // 12288

struct RawParams {
    uint32_t width;
    uint32_t height;
};

// Minimal static dispatch operation: bind a compute pipeline and dispatch.
class RawDispatchOp : public CmdOpBase {
public:
    RawDispatchOp(std::shared_ptr<ComputePipeline> pipeline, DispatchSize size) :
        m_pipeline(std::move(pipeline)), m_size(size)
    {
    }

    void OnUpdateRawData(ResourceId, uint32_t, const void *, size_t) override {}
    void OnUpdateRawData(ResourceTypes, uint32_t, const void *, size_t) override {}
    void OnUpdateRawData(ResourceTypes, const void *, size_t) override {}

    UpdateFrequency GetFrequency() const override { return UpdateFrequency::eStatic; }

    void Record(const vk::CommandBuffer &cmd, uint32_t, DescriptorBindingState &state) override
    {
        m_pipeline->recordDispatchCommand(cmd, 0, m_size, state);
    }

private:
    std::shared_ptr<ComputePipeline> m_pipeline;
    DispatchSize m_size;
};

std::vector<uint8_t> makeRaw12()
{
    std::vector<uint8_t> raw(RAW12_BYTES);
    for (uint32_t i = 0; i < RAW12_BYTES; ++i) {
        raw[i] = static_cast<uint8_t>((i * 17u + 11u) & 0xFFu);
    }
    return raw;
}

uint8_t toByte(float value)
{
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
}

} // namespace

void test()
{
    auto camera = std::make_shared<Camera>(glm::vec3(0.0f, 0.0f, 3.0f));
    GLFWwindow window(800, 600, camera);
    auto mgr = RenderManager::Create(window, MAX_FRAMES_IN_FLIGHT);

    // ---- single-buffered compute layout ----------------------------------
    PushConstantDetail push{static_cast<uint32_t>(sizeof(RawParams)), vk::ShaderStageFlagBits::eCompute, 0};
    auto layout = std::make_shared<Raw2RgbLayoutAtlas>(
        std::vector<Raw2RgbComponent>{Raw2RgbComponent::eRawImage, Raw2RgbComponent::eUnpackedImage,
                                      Raw2RgbComponent::eRgbImage},
        MAX_FRAMES_IN_FLIGHT, true, push);
    layout->setSpec(0, GBufferSpec{UNPACKED_WORDS, static_cast<uint32_t>(sizeof(uint32_t)), DataTypes::eUnsignedInt});
    layout->setSpec(1, GBufferSpec{UNPACKED_WORDS, static_cast<uint32_t>(sizeof(uint32_t)), DataTypes::eUnsignedInt});
    layout->setSpec(2, GBufferSpec{RGB_FLOAT_COUNT, static_cast<uint32_t>(sizeof(float)), DataTypes::eFloat});

    auto asset = mgr->CreateShaderAssetGroup(layout);
    asset->addShaderSpirvCode("rawunpack", std::vector<uint32_t>(shader::RAWUNPACK_COMP_SPV.begin(),
                                                                shader::RAWUNPACK_COMP_SPV.end()));
    asset->addShaderSpirvCode("raw2rgb", std::vector<uint32_t>(shader::RAW2RGB_V2_COMP_SPV.begin(),
                                                              shader::RAW2RGB_V2_COMP_SPV.end()));

    // ---- upload raw12 data ------------------------------------------------
    const std::vector<uint8_t> raw12 = makeRaw12();
    // The shader addresses the packed input as uint32 words (one word per two
    // samples), so back the 6144 raw12 bytes with a zero-padded 2048-word
    // buffer to keep every access in bounds.
    std::vector<uint8_t> rawPadded(UNPACKED_WORDS * static_cast<uint32_t>(sizeof(uint32_t)), 0u);
    std::copy(raw12.begin(), raw12.end(), rawPadded.begin());

    auto upload = mgr->UploadSequence();
    auto syncOp = std::make_shared<CmdSyncDevice>(asset);
    syncOp->OnUpdateRawData(ResourceId(0, 0), 0u, rawPadded);
    upload->submit(syncOp, asset);

    const RawParams params{IMAGE_WIDTH, IMAGE_HEIGHT};
    asset->UpdateConstantsResource(0, &params, static_cast<uint32_t>(sizeof(params)));

    // ---- two compute pipelines over the same asset -----------------------
    auto unpackPipeline = mgr->CreateComputePipeline(layout, asset, "rawunpack");
    auto rgbPipeline = mgr->CreateComputePipeline(layout, asset, "raw2rgb");

    auto once = mgr->UploadSequence();
    auto unpackOp = std::make_shared<RawDispatchOp>(unpackPipeline,
                                                   DispatchSize::FromInvocationCount(UNPACKED_WORDS, 256));
    auto rgbOp = std::make_shared<RawDispatchOp>(rgbPipeline,
                                                 DispatchSize::FromInvocationCount(PIXEL_COUNT, 256));
    once->submit(unpackOp, asset);
    once->submit(rgbOp, asset);

    // ---- read back results ------------------------------------------------
    const auto rgbBytes =
        once->ReadBack(asset, ResourceKey::make_key(ResourceId(0, 2), 0), RGB_FLOAT_COUNT * static_cast<uint32_t>(sizeof(float)));
    const auto unpackedBytes =
        once->ReadBack(asset, ResourceKey::make_key(ResourceId(0, 1), 0), UNPACKED_WORDS * static_cast<uint32_t>(sizeof(uint32_t)));
    if (rgbBytes.size() < RGB_FLOAT_COUNT * static_cast<uint32_t>(sizeof(float)) || unpackedBytes.empty()) {
        throw std::runtime_error("raw2rgb readback returned no data");
    }

    const std::filesystem::path outputDir = std::string(PROJECT_ROOT_PATH) + "/assets/images";
    std::filesystem::create_directories(outputDir);

    {
        std::ofstream ppm(outputDir / "output_rgb_64x64.ppm", std::ios::binary);
        if (!ppm) {
            throw std::runtime_error("cannot write output PPM file");
        }
        ppm << "P6\n" << IMAGE_WIDTH << " " << IMAGE_HEIGHT << "\n255\n";
        for (uint32_t pixel = 0; pixel < PIXEL_COUNT; ++pixel) {
            float channels[3] = {0.0f, 0.0f, 0.0f};
            for (uint32_t c = 0; c < 3; ++c) {
                const size_t offset = (static_cast<size_t>(pixel) * 3u + c) * static_cast<uint32_t>(sizeof(float));
                if (offset + static_cast<uint32_t>(sizeof(float)) <= rgbBytes.size()) {
                    std::memcpy(&channels[c], rgbBytes.data() + offset, static_cast<uint32_t>(sizeof(float)));
                }
            }
            const char out[3] = {static_cast<char>(toByte(channels[0])), static_cast<char>(toByte(channels[1])),
                                 static_cast<char>(toByte(channels[2]))};
            ppm.write(out, 3);
        }
    }
    {
        std::ofstream raw16(outputDir / "output_unpack_raw16_64x64.raw", std::ios::binary);
        if (!raw16) {
            throw std::runtime_error("cannot write output raw16 file");
        }
        raw16.write(reinterpret_cast<const char *>(unpackedBytes.data()),
                    static_cast<std::streamsize>(unpackedBytes.size()));
    }

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

int main()
{
    try {
        test();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
