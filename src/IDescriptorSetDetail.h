#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"

// A single component row in a set's static config table.
template <typename ComponentEnum>
struct DescriptorComponentSpec {
    ComponentEnum component;
    vk::DescriptorType descriptorType;
    uint32_t descriptorCount;
    vk::ShaderStageFlags stageFlags;
};

template <typename ComponentEnum, const auto &ConfigTable>
inline DescriptorComponentSpec<ComponentEnum> lookupComponentSpec(ComponentEnum c)
{
    DescriptorComponentSpec<ComponentEnum> result{};
    bool found = false;
    std::apply(
        [&](auto &&...row) {
            ((row.component == c ? (result = row, found = true) : false), ...);
        },
        ConfigTable);
    if (!found) {
        throw std::runtime_error("unknown descriptor component");
    }
    return result;
}

template <typename T>
inline size_t countDescriptorType(const T &table, vk::DescriptorType type)
{
    return std::apply(
        [type](auto &&...row) { return (size_t{0} + ... + (row.descriptorType == type ? row.descriptorCount : 0)); },
        table);
}

// Description of one descriptor set: which components it holds (binding order
// == list order), how often it updates, and whether it is shared across frames.
template <typename ComponentEnum>
struct SetDetailConfig {
    uint32_t setId = 0;
    std::vector<ComponentEnum> components;
    UpdateFrequency frequency = UpdateFrequency::eEachFrame;
    bool shared = false; // eStatic single-copy layer

    SetDetailConfig() = default;
    SetDetailConfig(uint32_t id, std::vector<ComponentEnum> comps, UpdateFrequency freq, bool sharedAcrossFrames) :
        setId(id), components(std::move(comps)), frequency(freq), shared(sharedAcrossFrames)
    {
    }
};

struct PushConstantDetail {
    uint32_t size = 0;
    vk::ShaderStageFlags stageFlags = vk::ShaderStageFlagBits::eVertex;
    uint32_t offset = 0;
};

// Concrete, per-instance detail of one set.  Produced by the template layer
// and consumed by ShaderAssetGroup / GraphicsPipeline / ComputePipeline.
struct SetLayoutDetail {
    uint32_t setId = 0;
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    std::vector<ResourceId> resourceIds; // parallel to bindings
    std::vector<std::variant<GBufferSpec, TextureImageSpec>> specs; // parallel, default-valued
    UpdateFrequency frequency = UpdateFrequency::eEachFrame;
    bool shared = false;

    bool hasSpec(ResourceId id) const
    {
        return std::find(resourceIds.begin(), resourceIds.end(), id) != resourceIds.end();
    }
};

// Non-template interface consumed by pipelines.  Ownership is transferred via
// SharedClone() so the pipeline's config outlives the example-side object.
class IDescriptorSetLayoutGroup {
public:
    virtual ~IDescriptorSetLayoutGroup() = default;
    virtual uint32_t maxFramesInFlight() const = 0;
    virtual uint32_t setCount() const = 0;
    virtual bool isSetShared(uint32_t setId) const = 0;
    virtual UpdateFrequency frequencyOfSet(uint32_t setId) const = 0;
    virtual const std::vector<SetLayoutDetail> &setDetails() const = 0;
    virtual std::vector<vk::DescriptorSetLayoutCreateInfo> layoutCreateInfos() const = 0;
    virtual std::vector<vk::DescriptorPoolSize> poolSizesPerFrame() const = 0;
    virtual const std::vector<vk::PushConstantRange> &pushConstantRanges() const = 0;
    virtual uint32_t pushConstantSize() const = 0;
    virtual std::shared_ptr<const IDescriptorSetLayoutGroup> sharedClone() const = 0;
};

// Template that binds a component table + a list of SetDetailConfig into a
// concrete layout group.  Example code only supplies enums and specs.
template <typename ComponentEnum, const auto &ConfigTable>
class DescriptorSetDetailTemplate : public IDescriptorSetLayoutGroup {
public:
    DescriptorSetDetailTemplate(std::vector<SetDetailConfig<ComponentEnum>> sets, uint32_t maxFrames,
                                const PushConstantDetail &pushConstant = {}) :
        m_maxFrames(maxFrames), m_pushConstant(pushConstant)
    {
        for (const auto &setCfg : sets) {
            SetLayoutDetail detail;
            detail.setId = setCfg.setId;
            detail.frequency = setCfg.frequency;
            detail.shared = setCfg.shared;
            uint32_t binding = 0;
            for (auto component : setCfg.components) {
                auto spec = lookupComponentSpec<ComponentEnum, ConfigTable>(component);
                vk::DescriptorSetLayoutBinding layoutBinding{};
                layoutBinding.binding = binding;
                layoutBinding.descriptorType = spec.descriptorType;
                layoutBinding.descriptorCount = spec.descriptorCount;
                layoutBinding.stageFlags = spec.stageFlags;
                detail.bindings.push_back(layoutBinding);
                detail.resourceIds.emplace_back(setCfg.setId, binding);
                detail.specs.emplace_back(GBufferSpec{1, 1, DataTypes::eCustom});
                ++binding;
            }
            m_details.push_back(std::move(detail));
        }
        if (m_pushConstant.size > 0) {
            vk::PushConstantRange range{};
            range.stageFlags = m_pushConstant.stageFlags;
            range.offset = m_pushConstant.offset;
            range.size = m_pushConstant.size;
            m_pushConstantRanges.push_back(range);
        }
    }

    // Set the concrete size spec for one component in one set.
    void setSpec(uint32_t setId, uint32_t binding, GBufferSpec spec)
    {
        auto *slot = findSpecSlot(setId, binding);
        if (slot) {
            *slot = spec;
        }
    }
    void setSpec(uint32_t setId, uint32_t binding, TextureImageSpec spec)
    {
        auto *slot = findSpecSlot(setId, binding);
        if (slot) {
            *slot = spec;
        }
    }

    size_t specBytes(uint32_t setId, uint32_t binding) const
    {
        for (const auto &detail : m_details) {
            if (detail.setId != setId || binding >= detail.specs.size()) {
                continue;
            }
            return std::visit(
                [](const auto &s) -> size_t {
                    using T = std::decay_t<decltype(s)>;
                    if constexpr (std::is_same_v<T, GBufferSpec>) {
                        return s.getTotalByte();
                    } else {
                        return s.getTotalPixels();
                    }
                },
                detail.specs[binding]);
        }
        return 0;
    }

    uint32_t maxFramesInFlight() const override { return m_maxFrames; }
    uint32_t setCount() const override { return static_cast<uint32_t>(m_details.size()); }

    bool isSetShared(uint32_t setId) const override
    {
        for (const auto &d : m_details) {
            if (d.setId == setId) {
                return d.shared;
            }
        }
        return false;
    }

    UpdateFrequency frequencyOfSet(uint32_t setId) const override
    {
        for (const auto &d : m_details) {
            if (d.setId == setId) {
                return d.frequency;
            }
        }
        return UpdateFrequency::eEachFrame;
    }

    const std::vector<SetLayoutDetail> &setDetails() const override { return m_details; }

    std::vector<vk::DescriptorSetLayoutCreateInfo> layoutCreateInfos() const override
    {
        std::vector<vk::DescriptorSetLayoutCreateInfo> result;
        m_layoutBindingsStorage.clear();
        m_layoutBindingsStorage.reserve(m_details.size());
        for (const auto &detail : m_details) {
            m_layoutBindingsStorage.push_back(detail.bindings);
        }
        for (auto &bindings : m_layoutBindingsStorage) {
            vk::DescriptorSetLayoutCreateInfo info{};
            info.bindingCount = static_cast<uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            result.push_back(info);
        }
        return result;
    }

    std::vector<vk::DescriptorPoolSize> poolSizesPerFrame() const override
    {
        std::unordered_map<uint32_t, uint32_t> counts;
        for (const auto &detail : m_details) {
            for (const auto &b : detail.bindings) {
                counts[static_cast<uint32_t>(b.descriptorType)] += b.descriptorCount;
            }
        }
        std::vector<vk::DescriptorPoolSize> result;
        for (const auto &[type, count] : counts) {
            result.emplace_back(static_cast<vk::DescriptorType>(type), count);
        }
        return result;
    }

    const std::vector<vk::PushConstantRange> &pushConstantRanges() const override { return m_pushConstantRanges; }
    uint32_t pushConstantSize() const override { return m_pushConstant.size; }

    std::shared_ptr<const IDescriptorSetLayoutGroup> sharedClone() const override
    {
        return std::make_shared<DescriptorSetDetailTemplate<ComponentEnum, ConfigTable>>(*this);
    }

private:
    std::variant<GBufferSpec, TextureImageSpec> *findSpecSlot(uint32_t setId, uint32_t binding)
    {
        for (auto &detail : m_details) {
            if (detail.setId != setId || binding >= detail.specs.size()) {
                continue;
            }
            return &detail.specs[binding];
        }
        return nullptr;
    }

private:
    uint32_t m_maxFrames = 2;
    PushConstantDetail m_pushConstant{};
    std::vector<SetLayoutDetail> m_details;
    std::vector<vk::PushConstantRange> m_pushConstantRanges;
    mutable std::vector<std::vector<vk::DescriptorSetLayoutBinding>> m_layoutBindingsStorage;
};
