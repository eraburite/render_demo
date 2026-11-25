#include "ComputePipeline.h"

#include <functional>
#include <sstream>
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

const std::unordered_map<std::string, std::vector<uint32_t>> &emptyShaderCodes()
{
    static const std::unordered_map<std::string, std::vector<uint32_t>> codes;
    return codes;
}

std::string groupCountsToString(const DispatchSize &size)
{
    std::ostringstream out;
    out << '(' << size.groupCountX << ", " << size.groupCountY << ", " << size.groupCountZ << ')';
    return out.str();
}

} // namespace

ComputePipeline::ComputePipeline(std::shared_ptr<vk::Device> device,
                                 std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout,
                                 std::shared_ptr<ShaderAssetGroup> asset, const std::string &spirvEntry,
                                 const vk::PhysicalDeviceLimits &limits, ComputePipelineOptions options) :
    m_device(std::move(device)), m_descriptorLayout(std::move(descriptorLayout)), m_asset(std::move(asset)),
    m_spirvEntry(spirvEntry), m_options(std::move(options))
{
    for (uint32_t axis = 0; axis < 3; ++axis) {
        m_maxWorkGroupCount[axis] = limits.maxComputeWorkGroupCount[axis];
    }
}

void ComputePipeline::builtUp()
{
    if (m_built) {
        return;
    }
    createSetLayouts();
    createDescriptorPool();
    allocateAndWriteSets();
    createPipeline();
    m_built = true;
}

void ComputePipeline::addSpecializationConstant(uint32_t constantId, const void *data, size_t size)
{
    if (m_built) {
        throw std::runtime_error("ComputePipeline: cannot add specialization constant " +
                                 std::to_string(constantId) + " after builtUp()");
    }
    if (data == nullptr || (size != 4 && size != 8)) {
        throw std::runtime_error("ComputePipeline: specialization constant " + std::to_string(constantId) +
                                 " must be 4 or 8 bytes");
    }
    for (const SpecializationConstant &existing : m_options.specializationConstants) {
        if (existing.constantId == constantId) {
            throw std::runtime_error("ComputePipeline: specialization constant " + std::to_string(constantId) +
                                     " already set");
        }
    }
    SpecializationConstant constant;
    constant.constantId = constantId;
    const auto *bytes = static_cast<const std::byte *>(data);
    constant.data.assign(bytes, bytes + size);
    m_options.specializationConstants.push_back(std::move(constant));
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

    // Specialization constants travel as one contiguous blob; the map entries
    // carry (offset, size) into it, so the blob must stay alive until the
    // pipeline is created - both live in this scope.
    std::vector<vk::SpecializationMapEntry> specEntries;
    std::vector<std::byte> specData;
    if (!m_options.specializationConstants.empty()) {
        specEntries.reserve(m_options.specializationConstants.size());
        for (const SpecializationConstant &constant : m_options.specializationConstants) {
            specEntries.emplace_back(constant.constantId, static_cast<uint32_t>(specData.size()),
                                     constant.data.size());
            specData.insert(specData.end(), constant.data.begin(), constant.data.end());
        }
    }

    vk::SpecializationInfo specInfo{};
    specInfo.mapEntryCount = static_cast<uint32_t>(specEntries.size());
    specInfo.pMapEntries = specEntries.empty() ? nullptr : specEntries.data();
    specInfo.dataSize = specData.size();
    specInfo.pData = specData.empty() ? nullptr : specData.data();

    vk::PipelineShaderStageCreateInfo computeStage{};
    computeStage.stage = vk::ShaderStageFlagBits::eCompute;
    computeStage.module = static_cast<vk::ShaderModule>(computeModule);
    computeStage.pName = "main";
    computeStage.pSpecializationInfo = specEntries.empty() ? nullptr : &specInfo;

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

    // The C-style overload reports a per-pipeline result: a driver may fail to
    // compile this one pipeline while the call itself "succeeds", so the result
    // is checked and the handle is verified before it is wrapped.
    vk::Pipeline pipeline{};
    const vk::Result result = m_device->createComputePipelines(m_options.pipelineCache, 1, &pipelineInfo, nullptr,
                                                               &pipeline);
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error("ComputePipeline: vkCreateComputePipelines failed for entry \"" + m_spirvEntry +
                                 "\": " + vk::to_string(result));
    }
    if (!pipeline) {
        throw std::runtime_error("ComputePipeline: vkCreateComputePipelines returned a null pipeline for entry \"" +
                                 m_spirvEntry + "\"");
    }
    m_pipeline = decltype(m_pipeline)(pipeline, [device = m_device](vk::Pipeline handle) {
        device->destroyPipeline(handle);
    });
}

vk::DescriptorSet ComputePipeline::descriptorSetFor(uint32_t frame, uint32_t setIndex) const
{
    if (m_descriptorSets.empty()) {
        return {};
    }
    // m_descriptorSets is laid out [frameSlot][setIndex]; a shared set is only
    // written for frame 0, so every frame binds that copy.
    uint32_t activeFrame = frame;
    if (m_descriptorLayout) {
        const std::vector<SetLayoutDetail> &details = m_descriptorLayout->setDetails();
        if (setIndex < details.size() &&
            (details[setIndex].shared || m_descriptorLayout->isSetShared(details[setIndex].setId))) {
            activeFrame = 0;
        }
    }
    if (activeFrame >= m_descriptorSets.size() || setIndex >= m_descriptorSets[activeFrame].size()) {
        return {};
    }
    return m_descriptorSets[activeFrame][setIndex];
}

void ComputePipeline::validateDispatchSize(const DispatchSize &size) const
{
    if (m_maxWorkGroupCount[0] == 0) {
        return;
    }
    const uint32_t requested[3] = {size.groupCountX, size.groupCountY, size.groupCountZ};
    for (uint32_t axis = 0; axis < 3; ++axis) {
        if (requested[axis] > m_maxWorkGroupCount[axis]) {
            throw std::runtime_error("ComputePipeline(" + m_spirvEntry + "): dispatch size " +
                                     groupCountsToString(size) + " exceeds maxComputeWorkGroupCount (" +
                                     std::to_string(m_maxWorkGroupCount[0]) + ", " +
                                     std::to_string(m_maxWorkGroupCount[1]) + ", " +
                                     std::to_string(m_maxWorkGroupCount[2]) + ")");
        }
    }
}

void ComputePipeline::bindResources(const vk::CommandBuffer &cmd, uint32_t frame,
                                    DescriptorBindingState &bindingState)
{
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
    if (firstSet >= setCount) {
        return;
    }

    std::vector<vk::DescriptorSet> sets;
    sets.reserve(setCount - firstSet);
    bool allBound = true;
    for (uint32_t index = firstSet; index < setCount; ++index) {
        const vk::DescriptorSet set = descriptorSetFor(frame, index);
        allBound = allBound && static_cast<bool>(set);
        sets.push_back(set);
    }

    if (allBound) {
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, firstSet,
                               static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        bindingState.MarkBoundRange(firstSet, setCount - firstSet);
        return;
    }

    // A set without resources cannot be bound; bind the others one by one so the
    // missing one is skipped instead of poisoning the whole range.
    for (uint32_t index = firstSet; index < setCount; ++index) {
        const vk::DescriptorSet set = sets[index - firstSet];
        if (!set) {
            continue;
        }
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, index, 1, &set, 0, nullptr);
        bindingState.MarkBoundRange(index, 1);
    }
}

void ComputePipeline::recordDispatchCommand(const vk::CommandBuffer &cmd, uint32_t frame, const DispatchSize &size,
                                            DescriptorBindingState &bindingState)
{
    if (!m_pipeline) {
        return;
    }
    // Zero groups is a legal no-op, but recording the dispatch anyway costs a
    // command and hides "the CPU computed an empty workload" from the caller.
    if (size.groupCountX == 0 || size.groupCountY == 0 || size.groupCountZ == 0) {
        return;
    }
    validateDispatchSize(size);
    bindResources(cmd, frame, bindingState);
    cmd.dispatch(size.groupCountX, size.groupCountY, size.groupCountZ);
}

void ComputePipeline::recordDispatchCommand(const vk::CommandBuffer &cmd, uint32_t frame,
                                            const IndirectDispatch &indirect, DescriptorBindingState &bindingState)
{
    if (!m_pipeline) {
        return;
    }
    if (!indirect.valid()) {
        throw std::runtime_error("ComputePipeline(" + m_spirvEntry +
                                 "): indirect dispatch needs a buffer holding VkDispatchIndirectCommand");
    }
    bindResources(cmd, frame, bindingState);
    cmd.dispatchIndirect(indirect.buffer, indirect.offset);
}
