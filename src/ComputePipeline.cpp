#include "ComputePipeline.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(vk::Result result, const char *what)
{
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error(std::string(what) + " failed: " + vk::to_string(result));
    }
}

using ShaderModuleHandle = VulkanHandle<vk::ShaderModule, std::function<void(vk::ShaderModule)>>;

ShaderModuleHandle createShaderModule(const std::shared_ptr<vk::Device> &device,
                                      const std::vector<uint32_t> &code)
{
    vk::ShaderModuleCreateInfo createInfo{};
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();

    vk::ShaderModule shaderModule{};
    check(device->createShaderModule(&createInfo, nullptr, &shaderModule), "vkCreateShaderModule");
    return ShaderModuleHandle(shaderModule,
                              [device](vk::ShaderModule module) { device->destroyShaderModule(module); });
}

vk::DescriptorSet descriptorSetFor(const std::vector<std::vector<vk::DescriptorSet>> &sets,
                                   const std::shared_ptr<const IDescriptorSetLayoutGroup> &layout, uint32_t frame,
                                   uint32_t setId)
{
    if (sets.empty()) {
        return {};
    }
    const uint32_t activeFrame = (layout && layout->isSetShared(setId)) ? 0u : frame;
    if (activeFrame >= sets.size() || setId >= sets[activeFrame].size()) {
        return {};
    }
    return sets[activeFrame][setId];
}

const std::unordered_map<std::string, std::vector<uint32_t>> &emptyShaderCodes()
{
    static const std::unordered_map<std::string, std::vector<uint32_t>> codes;
    return codes;
}

} // namespace

ComputePipeline::ComputePipeline(std::shared_ptr<vk::Device> device,
                                 std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout,
                                 std::shared_ptr<ShaderAssetGroup> asset, const std::string &spirvEntry) :
    m_device(std::move(device)), m_descriptorLayout(std::move(descriptorLayout)), m_asset(std::move(asset)),
    m_spirvEntry(spirvEntry)
{
}

void ComputePipeline::builtUp()
{
    createSetLayouts();
    createDescriptorPool();
    allocateAndWriteSets();
    createPipeline();
}

void ComputePipeline::createSetLayouts()
{
    if (!m_descriptorLayout) {
        return;
    }
    const std::vector<vk::DescriptorSetLayoutCreateInfo> infos = m_descriptorLayout->layoutCreateInfos();
    m_setLayouts.clear();
    m_setLayouts.reserve(infos.size());
    for (const vk::DescriptorSetLayoutCreateInfo &info : infos) {
        vk::DescriptorSetLayout layout{};
        check(m_device->createDescriptorSetLayout(&info, nullptr, &layout), "vkCreateDescriptorSetLayout");
        m_setLayouts.emplace_back(layout, [device = m_device](vk::DescriptorSetLayout handle) {
            device->destroyDescriptorSetLayout(handle);
        });
    }
}

void ComputePipeline::createDescriptorPool()
{
    const uint32_t frameCount = m_asset ? m_asset->maxFrames() : MAX_FRAMES_IN_FLIGHT;
    const uint32_t setCount = m_descriptorLayout ? m_descriptorLayout->setCount() : 0;

    std::vector<vk::DescriptorPoolSize> poolSizes;
    if (m_descriptorLayout) {
        for (vk::DescriptorPoolSize size : m_descriptorLayout->poolSizesPerFrame()) {
            size.descriptorCount *= frameCount;
            poolSizes.push_back(size);
        }
    }

    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = frameCount * setCount;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    vk::DescriptorPool pool{};
    check(m_device->createDescriptorPool(&poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    m_descriptorPool = decltype(m_descriptorPool)(pool, [device = m_device](vk::DescriptorPool handle) {
        device->destroyDescriptorPool(handle);
    });
}

void ComputePipeline::allocateAndWriteSets()
{
    const uint32_t frameCount = m_asset ? m_asset->maxFrames() : MAX_FRAMES_IN_FLIGHT;
    const uint32_t setCount = static_cast<uint32_t>(m_setLayouts.size());
    if (setCount == 0 || frameCount == 0) {
        return;
    }

    std::vector<vk::DescriptorSetLayout> layouts;
    layouts.reserve(static_cast<size_t>(frameCount) * setCount);
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        for (const auto &setLayout : m_setLayouts) {
            layouts.push_back(static_cast<vk::DescriptorSetLayout>(setLayout));
        }
    }

    vk::DescriptorSetAllocateInfo allocInfo{};
    allocInfo.descriptorPool = static_cast<vk::DescriptorPool>(m_descriptorPool);
    allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocInfo.pSetLayouts = layouts.data();

    std::vector<vk::DescriptorSet> allocated(layouts.size());
    check(m_device->allocateDescriptorSets(&allocInfo, allocated.data()), "vkAllocateDescriptorSets");

    m_descriptorSets.assign(frameCount, std::vector<vk::DescriptorSet>(setCount));
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        for (uint32_t set = 0; set < setCount; ++set) {
            m_descriptorSets[frame][set] = allocated[static_cast<size_t>(frame) * setCount + set];
        }
    }

    if (!m_asset || !m_descriptorLayout) {
        return;
    }
    const std::vector<SetLayoutDetail> &details = m_descriptorLayout->setDetails();

    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        std::vector<vk::WriteDescriptorSet> writes;
        for (uint32_t setIndex = 0; setIndex < details.size(); ++setIndex) {
            const SetLayoutDetail &detail = details[setIndex];
            const bool shared = detail.shared || m_descriptorLayout->isSetShared(detail.setId);
            if (shared && frame != 0) {
                continue;
            }
            if (setIndex >= m_descriptorSets[frame].size()) {
                continue;
            }
            const vk::DescriptorSet targetSet = m_descriptorSets[frame][setIndex];
            if (!targetSet) {
                continue;
            }

            for (uint32_t bindingIndex = 0; bindingIndex < detail.bindings.size(); ++bindingIndex) {
                const vk::DescriptorSetLayoutBinding &binding = detail.bindings[bindingIndex];
                const ResourceId id = bindingIndex < detail.resourceIds.size()
                                          ? detail.resourceIds[bindingIndex]
                                          : ResourceId(detail.setId, binding.binding);
                const uint32_t resourceFrame = shared ? 0u : frame;
                const std::shared_ptr<IDeviceResource> resource =
                    m_asset->DescriptorResource(ResourceKey::make_key(id, resourceFrame));
                if (!resource) {
                    continue;
                }
                writes.push_back(resource->constructWriteDescriptorSet(targetSet, binding.descriptorType,
                                                                       binding.binding));
            }
        }
        if (!writes.empty()) {
            m_device->updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

void ComputePipeline::createPipeline()
{
    const auto &codes = m_asset ? m_asset->shaderCodes() : emptyShaderCodes();
    const std::vector<uint32_t> *computeCode = nullptr;
    for (const std::string key : {m_spirvEntry, " " + m_spirvEntry}) {
        const auto it = codes.find(key);
        if (it != codes.end() && !it->second.empty()) {
            computeCode = &it->second;
            break;
        }
    }
    if (!computeCode) {
        throw std::runtime_error("ComputePipeline: missing \"" + m_spirvEntry + "\" SPIR-V code");
    }

    ShaderModuleHandle computeModule = createShaderModule(m_device, *computeCode);

    vk::PipelineShaderStageCreateInfo computeStage{};
    computeStage.stage = vk::ShaderStageFlagBits::eCompute;
    computeStage.module = static_cast<vk::ShaderModule>(computeModule);
    computeStage.pName = "main";

    std::vector<vk::DescriptorSetLayout> setLayouts;
    setLayouts.reserve(m_setLayouts.size());
    for (const auto &layout : m_setLayouts) {
        setLayouts.push_back(static_cast<vk::DescriptorSetLayout>(layout));
    }

    const std::vector<vk::PushConstantRange> emptyRanges;
    const std::vector<vk::PushConstantRange> &pushConstantRanges =
        m_descriptorLayout ? m_descriptorLayout->pushConstantRanges() : emptyRanges;

    vk::PipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size());
    layoutInfo.pPushConstantRanges = pushConstantRanges.data();

    vk::PipelineLayout layout{};
    check(m_device->createPipelineLayout(&layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    m_pipelineLayout = decltype(m_pipelineLayout)(layout, [device = m_device](vk::PipelineLayout handle) {
        device->destroyPipelineLayout(handle);
    });

    vk::ComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.stage = computeStage;
    pipelineInfo.layout = static_cast<vk::PipelineLayout>(m_pipelineLayout);
    pipelineInfo.basePipelineHandle = vk::Pipeline{};
    pipelineInfo.basePipelineIndex = -1;

    vk::Pipeline pipeline{};
    check(m_device->createComputePipelines(nullptr, 1, &pipelineInfo, nullptr, &pipeline),
          "vkCreateComputePipelines");
    m_pipeline = decltype(m_pipeline)(pipeline, [device = m_device](vk::Pipeline handle) {
        device->destroyPipeline(handle);
    });
}

void ComputePipeline::recordDispatchCommand(const vk::CommandBuffer &cmd, uint32_t frame, const DispatchSize &size,
                                            DescriptorBindingState &bindingState)
{
    if (!m_pipeline) {
        return;
    }
    const vk::PipelineLayout layout = static_cast<vk::PipelineLayout>(m_pipelineLayout);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, static_cast<vk::Pipeline>(m_pipeline));

    const std::vector<vk::PushConstantRange> emptyRanges;
    const std::vector<vk::PushConstantRange> &ranges =
        m_descriptorLayout ? m_descriptorLayout->pushConstantRanges() : emptyRanges;
    if (m_asset) {
        m_asset->PushConstantData(cmd, layout, ranges, frame);
    }

    bindingState.OnPipelineLayoutBound(layout);
    const uint32_t setCount = m_descriptorLayout ? m_descriptorLayout->setCount() : 0;
    const uint32_t firstSet = bindingState.FirstUnboundSet(setCount);
    if (firstSet < setCount) {
        std::vector<vk::DescriptorSet> sets;
        sets.reserve(setCount - firstSet);
        const std::vector<SetLayoutDetail> &details = m_descriptorLayout->setDetails();
        for (uint32_t index = firstSet; index < setCount; ++index) {
            const uint32_t setId = index < details.size() ? details[index].setId : index;
            sets.push_back(descriptorSetFor(m_descriptorSets, m_descriptorLayout, frame, setId));
        }
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, firstSet,
                               static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        bindingState.MarkBoundRange(firstSet, setCount - firstSet);
    }

    cmd.dispatch(size.groupCountX, size.groupCountY, size.groupCountZ);
}
