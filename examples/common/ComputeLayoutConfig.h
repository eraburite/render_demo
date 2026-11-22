#pragma once
#include <memory>
#include <tuple>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "IDescriptorSetDetail.h"

// compute set0 components (particle sim): parameters UBO, input/output SSBO.
enum class ComputeComponent { eParameter, eParticleInput, eParticleOutput };
using ComputeConfig = DescriptorComponentSpec<ComputeComponent>;
inline constexpr auto ComputeComponentTable = std::make_tuple(
    ComputeConfig{ComputeComponent::eParameter, vk::DescriptorType::eUniformBuffer, 1,
                  vk::ShaderStageFlagBits::eCompute},
    ComputeConfig{ComputeComponent::eParticleInput, vk::DescriptorType::eStorageBuffer, 1,
                  vk::ShaderStageFlagBits::eCompute},
    ComputeConfig{ComputeComponent::eParticleOutput, vk::DescriptorType::eStorageBuffer, 1,
                  vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eVertex});

// Generic single-layer compute atlas template (component table + optional
// push constant + optional cross-frame sharing).
template <typename ComponentEnum, const auto &Table>
class ComputeLayoutAtlasTemplate : public IDescriptorSetLayoutGroup {
public:
    ComputeLayoutAtlasTemplate(std::vector<ComponentEnum> components, uint32_t maxFrames, bool sharedAcrossFrames,
                               const PushConstantDetail &pushConstant = {}) :
        m_maxFrames(maxFrames), m_push(pushConstant)
    {
        SetLayoutDetail detail;
        detail.setId = 0;
        detail.frequency = sharedAcrossFrames ? UpdateFrequency::eStatic : UpdateFrequency::eEachFrame;
        detail.shared = sharedAcrossFrames;
        uint32_t binding = 0;
        for (auto c : components) {
            auto spec = lookupComponentSpec<ComponentEnum, Table>(c);
            vk::DescriptorSetLayoutBinding b{};
            b.binding = binding;
            b.descriptorType = spec.descriptorType;
            b.descriptorCount = spec.descriptorCount;
            b.stageFlags = spec.stageFlags;
            detail.bindings.push_back(b);
            detail.resourceIds.emplace_back(0, binding);
            detail.specs.emplace_back(GBufferSpec{1, 1, DataTypes::eCustom});
            ++binding;
        }
        m_details.push_back(std::move(detail));
        if (m_push.size > 0) {
            vk::PushConstantRange range{};
            range.stageFlags = m_push.stageFlags;
            range.offset = m_push.offset;
            range.size = m_push.size;
            m_pushRanges.push_back(range);
        }
    }

    void setSpec(uint32_t binding, GBufferSpec spec)
    {
        if (binding < m_details[0].specs.size()) {
            m_details[0].specs[binding] = spec;
        }
    }

    uint32_t maxFramesInFlight() const override { return m_maxFrames; }
    uint32_t setCount() const override { return static_cast<uint32_t>(m_details.size()); }
    bool isSetShared(uint32_t setId) const override { return m_details[0].shared; }
    UpdateFrequency frequencyOfSet(uint32_t) const override { return m_details[0].frequency; }
    const std::vector<SetLayoutDetail> &setDetails() const override { return m_details; }

    std::vector<vk::DescriptorSetLayoutCreateInfo> layoutCreateInfos() const override
    {
        m_bindingStorage.clear();
        m_bindingStorage.push_back(m_details[0].bindings);
        vk::DescriptorSetLayoutCreateInfo info{};
        info.bindingCount = static_cast<uint32_t>(m_bindingStorage[0].size());
        info.pBindings = m_bindingStorage[0].data();
        return {info};
    }

    std::vector<vk::DescriptorPoolSize> poolSizesPerFrame() const override
    {
        std::vector<vk::DescriptorPoolSize> sizes;
        for (const auto &b : m_details[0].bindings) {
            auto it = std::find_if(sizes.begin(), sizes.end(), [&](const vk::DescriptorPoolSize &s) {
                return s.type == b.descriptorType;
            });
            if (it == sizes.end()) {
                sizes.emplace_back(b.descriptorType, b.descriptorCount);
            } else {
                it->descriptorCount += b.descriptorCount;
            }
        }
        return sizes;
    }

    const std::vector<vk::PushConstantRange> &pushConstantRanges() const override { return m_pushRanges; }
    uint32_t pushConstantSize() const override { return m_push.size; }
    std::shared_ptr<const IDescriptorSetLayoutGroup> sharedClone() const override
    {
        return std::make_shared<ComputeLayoutAtlasTemplate<ComponentEnum, Table>>(*this);
    }

private:
    uint32_t m_maxFrames = 2;
    PushConstantDetail m_push{};
    std::vector<SetLayoutDetail> m_details;
    std::vector<vk::PushConstantRange> m_pushRanges;
    mutable std::vector<std::vector<vk::DescriptorSetLayoutBinding>> m_bindingStorage;
};

using ComputeLayoutAtlas = ComputeLayoutAtlasTemplate<ComputeComponent, ComputeComponentTable>;
