#pragma once
#include <tuple>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "IDescriptorSetDetail.h"
#include "typedefs.h"

// Shared UBO structs used by the lighting-family examples.
struct CameraMatrix {
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};
struct LightSource {
    alignas(16) glm::vec4 position;
    alignas(16) glm::vec4 ambient;
    alignas(16) glm::vec4 diffuse;
    alignas(16) glm::vec4 specular;
};
struct ViewEyePoint {
    alignas(16) glm::vec4 position;
};

// Light block of the subpass composition pass.  Layout mirrors the reference
// std140 block exactly: vec4 position + vec3 color + float radius, 32 bytes per
// element, 64 elements.  A specialization constant tells the shader the count.
struct SubpassLight {
    alignas(16) glm::vec4 position;
    alignas(16) glm::vec3 color;
    float radius = 1.0f;
};
inline constexpr uint32_t kSubpassLightCount = 64;
struct SubpassLights {
    alignas(16) glm::vec4 viewPos;
    SubpassLight lights[kSubpassLightCount];
};
static_assert(sizeof(SubpassLight) == 32, "std140 array element must be 32 bytes");
static_assert(sizeof(SubpassLights) == 16 + kSubpassLightCount * 32, "lights UBO must match std140");

struct MaterialParams {
    uint32_t baseColorTexId = 0;
    uint32_t specularTexId = 1;
    float roughness = 0.5f;
    float metallic = 0.1f;
};
struct ObjectParams {
    alignas(16) glm::mat4 model;
};
struct ObjectParamsArray {
    std::vector<glm::mat4> models;
};

// ---- set0: eStatic global -------------------------------------------------
enum class GlobalComponent { ebaseColor, eSpecularMap, eMaterialParams, eNormalHeightMap };
using GlobalConfig = DescriptorComponentSpec<GlobalComponent>;
inline constexpr auto GlobalComponentTable = std::make_tuple(
    GlobalConfig{GlobalComponent::ebaseColor, vk::DescriptorType::eCombinedImageSampler, 1,
                 vk::ShaderStageFlagBits::eFragment},
    GlobalConfig{GlobalComponent::eSpecularMap, vk::DescriptorType::eCombinedImageSampler, 1,
                 vk::ShaderStageFlagBits::eFragment},
    GlobalConfig{GlobalComponent::eMaterialParams, vk::DescriptorType::eStorageBuffer, 1,
                 vk::ShaderStageFlagBits::eFragment},
    GlobalConfig{GlobalComponent::eNormalHeightMap, vk::DescriptorType::eCombinedImageSampler, 1,
                 vk::ShaderStageFlagBits::eFragment});

// ---- set1: eEnvironment ----------------------------------------------------
enum class EnvironmentComponent { eCameraMatrix };
using EnvironmentConfig = DescriptorComponentSpec<EnvironmentComponent>;
inline constexpr auto EnvironmentComponentTable = std::make_tuple(EnvironmentConfig{
    EnvironmentComponent::eCameraMatrix, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex});

// ---- set2: eEachFrame ------------------------------------------------------
enum class FrameComponent {
    eViewEyePosition,
    eLightSource,
    eDirectionalLight,
    ePointLights,
    eSpotlight
};
using FrameConfig = DescriptorComponentSpec<FrameComponent>;
inline constexpr auto FrameComponentTable = std::make_tuple(
    FrameConfig{FrameComponent::eViewEyePosition, vk::DescriptorType::eUniformBuffer, 1,
                vk::ShaderStageFlagBits::eFragment},
    FrameConfig{FrameComponent::eLightSource, vk::DescriptorType::eUniformBuffer, 1,
                vk::ShaderStageFlagBits::eFragment},
    FrameConfig{FrameComponent::eDirectionalLight, vk::DescriptorType::eUniformBuffer, 1,
                vk::ShaderStageFlagBits::eFragment},
    FrameConfig{FrameComponent::ePointLights, vk::DescriptorType::eStorageBuffer, 1,
                vk::ShaderStageFlagBits::eFragment},
    FrameConfig{FrameComponent::eSpotlight, vk::DescriptorType::eUniformBuffer, 1,
                vk::ShaderStageFlagBits::eFragment});

// ---- set3: eEachInstance ---------------------------------------------------
enum class InstanceComponent { eModelMatrix };
using InstanceConfig = DescriptorComponentSpec<InstanceComponent>;
inline constexpr auto InstanceComponentTable = std::make_tuple(InstanceConfig{
    InstanceComponent::eModelMatrix, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex});

// ---- set4: offscreen sampling ----------------------------------------------
enum class InputAttachmentComponent { ebaseColor, ePosition, eNormalMap, eAlbedo };
using InputAttachmentConfig = DescriptorComponentSpec<InputAttachmentComponent>;
inline constexpr auto InputAttachmentComponentTable = std::make_tuple(
    InputAttachmentConfig{InputAttachmentComponent::ebaseColor, vk::DescriptorType::eCombinedImageSampler, 1,
                          vk::ShaderStageFlagBits::eFragment},
    InputAttachmentConfig{InputAttachmentComponent::ePosition, vk::DescriptorType::eCombinedImageSampler, 1,
                          vk::ShaderStageFlagBits::eFragment},
    InputAttachmentConfig{InputAttachmentComponent::eNormalMap, vk::DescriptorType::eCombinedImageSampler, 1,
                          vk::ShaderStageFlagBits::eFragment},
    InputAttachmentConfig{InputAttachmentComponent::eAlbedo, vk::DescriptorType::eCombinedImageSampler, 1,
                          vk::ShaderStageFlagBits::eFragment});

// ---- subpass inputs: read inside the pass that wrote them -------------------
// Different from the table above: those entries are *sampled* views of a
// resolved offscreen image, these are VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT and
// are read with subpassLoad() by a later subpass of the same render pass.
enum class SubpassInputComponent { ePosition, eNormal, eAlbedo };
using SubpassInputConfig = DescriptorComponentSpec<SubpassInputComponent>;
inline constexpr auto SubpassInputComponentTable = std::make_tuple(
    SubpassInputConfig{SubpassInputComponent::ePosition, vk::DescriptorType::eInputAttachment, 1,
                       vk::ShaderStageFlagBits::eFragment},
    SubpassInputConfig{SubpassInputComponent::eNormal, vk::DescriptorType::eInputAttachment, 1,
                       vk::ShaderStageFlagBits::eFragment},
    SubpassInputConfig{SubpassInputComponent::eAlbedo, vk::DescriptorType::eInputAttachment, 1,
                       vk::ShaderStageFlagBits::eFragment});

// Deferred composition subpass: three input attachments plus the light array.
// One set may mix descriptor types, which is exactly why this table exists.
enum class SubpassCompositionComponent { ePosition, eNormal, eAlbedo, eLights };
using SubpassCompositionConfig = DescriptorComponentSpec<SubpassCompositionComponent>;
inline constexpr auto SubpassCompositionComponentTable = std::make_tuple(
    SubpassCompositionConfig{SubpassCompositionComponent::ePosition, vk::DescriptorType::eInputAttachment, 1,
                             vk::ShaderStageFlagBits::eFragment},
    SubpassCompositionConfig{SubpassCompositionComponent::eNormal, vk::DescriptorType::eInputAttachment, 1,
                             vk::ShaderStageFlagBits::eFragment},
    SubpassCompositionConfig{SubpassCompositionComponent::eAlbedo, vk::DescriptorType::eInputAttachment, 1,
                             vk::ShaderStageFlagBits::eFragment},
    SubpassCompositionConfig{SubpassCompositionComponent::eLights, vk::DescriptorType::eUniformBuffer, 1,
                             vk::ShaderStageFlagBits::eFragment});

// Transparent subpass: camera UBO (vertex), depth input attachment and the
// glass texture (both fragment).
enum class SubpassTransparentComponent { eCameraMatrix, ePosition, eGlass };
using SubpassTransparentConfig = DescriptorComponentSpec<SubpassTransparentComponent>;
inline constexpr auto SubpassTransparentComponentTable = std::make_tuple(
    SubpassTransparentConfig{SubpassTransparentComponent::eCameraMatrix, vk::DescriptorType::eUniformBuffer, 1,
                             vk::ShaderStageFlagBits::eVertex},
    SubpassTransparentConfig{SubpassTransparentComponent::ePosition, vk::DescriptorType::eInputAttachment, 1,
                             vk::ShaderStageFlagBits::eFragment},
    SubpassTransparentConfig{SubpassTransparentComponent::eGlass, vk::DescriptorType::eCombinedImageSampler, 1,
                             vk::ShaderStageFlagBits::eFragment});

enum class PushConstantComponent { eModelMatrix };
// Concrete atlas used by graphics examples.  Combines all six set tables under
// one IDescriptorSetLayoutGroup via a type-erasing wrapper.
class DescriptorLayoutAtlas : public IDescriptorSetLayoutGroup {
public:
    explicit DescriptorLayoutAtlas(uint32_t maxFrames = MAX_FRAMES_IN_FLIGHT) : m_maxFrames(maxFrames) {}

    // Build from a set of concrete SetLayoutDetails (filled by helper below).
    void setDetails(std::vector<SetLayoutDetail> details) { m_details = std::move(details); }
    void setPushConstant(uint32_t size, vk::ShaderStageFlags stages)
    {
        m_pushSize = size;
        m_pushRanges.clear();
        if (size > 0) {
            vk::PushConstantRange range{};
            range.stageFlags = stages;
            range.offset = 0;
            range.size = size;
            m_pushRanges.push_back(range);
        }
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
        m_bindingStorage.clear();
        m_bindingStorage.reserve(m_details.size());
        for (const auto &d : m_details) {
            m_bindingStorage.push_back(d.bindings);
        }
        std::vector<vk::DescriptorSetLayoutCreateInfo> infos;
        infos.reserve(m_bindingStorage.size());
        for (auto &bindings : m_bindingStorage) {
            vk::DescriptorSetLayoutCreateInfo info{};
            info.bindingCount = static_cast<uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            infos.push_back(info);
        }
        return infos;
    }

    std::vector<vk::DescriptorPoolSize> poolSizesPerFrame() const override
    {
        // DescriptorType enum values are sparse (eUniformBuffer=6,
        // eStorageBuffer=7, ...), so accumulate in a map instead of indexing
        // a fixed-size array.
        std::map<uint32_t, uint32_t> counts;
        for (const auto &d : m_details) {
            for (const auto &b : d.bindings) {
                counts[static_cast<uint32_t>(b.descriptorType)] += b.descriptorCount;
            }
        }
        std::vector<vk::DescriptorPoolSize> sizes;
        for (const auto &[type, count] : counts) {
            if (count > 0) {
                sizes.emplace_back(static_cast<vk::DescriptorType>(type), count);
            }
        }
        return sizes;
    }

    const std::vector<vk::PushConstantRange> &pushConstantRanges() const override { return m_pushRanges; }
    uint32_t pushConstantSize() const override { return m_pushSize; }
    std::shared_ptr<const IDescriptorSetLayoutGroup> sharedClone() const override
    {
        return std::make_shared<DescriptorLayoutAtlas>(*this);
    }

    // Convenience: build a SetLayoutDetail from a component table.
    template <typename ComponentEnum, const auto &Table>
    static SetLayoutDetail makeSet(uint32_t setId, std::vector<ComponentEnum> components, UpdateFrequency freq,
                                   bool shared)
    {
        SetLayoutDetail detail;
        detail.setId = setId;
        detail.frequency = freq;
        detail.shared = shared;
        uint32_t binding = 0;
        for (auto component : components) {
            auto spec = lookupComponentSpec<ComponentEnum, Table>(component);
            vk::DescriptorSetLayoutBinding b{};
            b.binding = binding;
            b.descriptorType = spec.descriptorType;
            b.descriptorCount = spec.descriptorCount;
            b.stageFlags = spec.stageFlags;
            detail.bindings.push_back(b);
            detail.resourceIds.emplace_back(setId, binding);
            detail.specs.emplace_back(GBufferSpec{1, 1, DataTypes::eCustom});
            ++binding;
        }
        return detail;
    }

private:
    uint32_t m_maxFrames = 2;
    std::vector<SetLayoutDetail> m_details;
    uint32_t m_pushSize = 0;
    std::vector<vk::PushConstantRange> m_pushRanges;
    mutable std::vector<std::vector<vk::DescriptorSetLayoutBinding>> m_bindingStorage;
};
