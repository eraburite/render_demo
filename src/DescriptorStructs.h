#pragma once
#include "typedefs.h"
#include <map>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"

constexpr int MAX_FRAMES_IN_FLIGHT = 2;

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

struct MaterialParams {
    uint32_t baseColorTexId;
    uint32_t diffuseTexIdx;
    float roughness;
    float metallic;
};

struct ViewEyePoint {
    alignas(16) glm::vec4 position;
};

struct ObjectParams {
    alignas(16) glm::mat4 model;
};

template <typename ComponentEnum>
struct ComponentParamConfigure {
    ComponentEnum component;
    vk::DescriptorType descriptorType;
    uint32_t descriptorCount;
    vk::ShaderStageFlags stageFlags;
};

template <typename ComponentEnum, typename Tuple>
inline constexpr auto get_components_from_table(const Tuple &table)
{
    return std::apply([](auto &&...args) { return std::vector<ComponentEnum>{args.component...}; }, table);
}

template <typename Tuple, typename Transformer>
inline constexpr size_t sum_by(const Tuple &t, Transformer trans)
{
    return std::apply([&trans](auto &&...args) { return (size_t(0) + ... + trans(args)); }, t);
}

template <typename Table>
constexpr inline size_t count_type(const Table &table, vk::DescriptorType descriptorType)
{
    return sum_by(table, [descriptorType](const auto &config) {
        return (config.descriptorType == descriptorType) ? config.descriptorCount : 0;
    });
}

template <typename ComponentEnum, uint32_t SetID, const auto &ConfigTable>
class DescriptorParam {
public:
    DescriptorParam(const std::vector<ComponentEnum> &components)
    {
        uint32_t binding = 0;
        for (auto &component : components) {
            auto desc = buildBindingDescription(binding, component);
            auto resourceId = ResourceId(SetID, binding);
            m_compoRscIdMap.emplace(component, resourceId);
            m_allDescriptorTypeMap.emplace(resourceId, desc.descriptorType);
            m_setLayoutBindings.push_back(desc);
            binding++;
        }

        m_descriptorSetLayoutCreateInfo.bindingCount = static_cast<uint32_t>(m_setLayoutBindings.size());
        m_descriptorSetLayoutCreateInfo.pBindings = m_setLayoutBindings.data();
    }

    virtual ~DescriptorParam() = default;

    void SetSpec(ComponentEnum c, GBufferSpec spec)
    {
        auto it = m_compoRscIdMap.find(c);
        if (it != m_compoRscIdMap.end()) {
            m_rscIdGBuffSpecMap.emplace(it->second, std::move(spec));
        } else {
            throw std::runtime_error(" invalid ComponentEnum!");
        }
    }

    void SetSpec(ComponentEnum c, TextureImageSpec spec)
    {
        auto it = m_compoRscIdMap.find(c);
        if (it != m_compoRscIdMap.end()) {
            m_rscIdTextureSpecMap.emplace(it->second, std::move(spec));
        } else {
            throw std::runtime_error(" invalid ComponentEnum!");
        }
    }

    uint32_t getSetId() const
    {
        return SetID;
    }

    size_t getParamComponentBytes(ComponentEnum c) const
    {
        auto it = m_compoRscIdMap.find(c);
        if (it != m_compoRscIdMap.end()) {
            auto tex_it = m_rscIdTextureSpecMap.find(it->second);
            if (tex_it != m_rscIdTextureSpecMap.end()) {
                return tex_it->second.getTotalPixels();
            }
            auto gbuff_it = m_rscIdGBuffSpecMap.find(it->second);
            if (gbuff_it != m_rscIdGBuffSpecMap.end()) {
                return gbuff_it->second.getTotalByte();
            }
        }
        return 0;
    }

    template <typename F>
    void for_each_component(uint32_t currentFrame, F &&callback) const
    {
        for (auto &[component, resourceId] : m_compoRscIdMap) {
            auto type_it = m_allDescriptorTypeMap.find(resourceId);
            auto tex_it = m_rscIdTextureSpecMap.find(resourceId);
            if (tex_it != m_rscIdTextureSpecMap.end() && type_it != m_allDescriptorTypeMap.end()) {
                callback(currentFrame, resourceId, type_it->second, tex_it->second);
            }
            auto gbuff_it = m_rscIdGBuffSpecMap.find(resourceId);
            if (gbuff_it != m_rscIdGBuffSpecMap.end() && type_it != m_allDescriptorTypeMap.end()) {
                callback(currentFrame, resourceId, type_it->second, gbuff_it->second);
            }
        }
    }

    vk::DescriptorSetLayoutCreateInfo getSetLayoutCreateInfo() const
    {
        return m_descriptorSetLayoutCreateInfo;
    }

private:
    auto buildBindingDescription(uint32_t binding, ComponentEnum component)
    {
        vk::DescriptorSetLayoutBinding componentLayoutBinding{};
        bool found = false;
        std::apply(
            [&](auto &&...args) {
                ((args.component == component ? (componentLayoutBinding.binding = binding,
                                                 componentLayoutBinding.descriptorType = args.descriptorType,
                                                 componentLayoutBinding.descriptorCount = args.descriptorCount,
                                                 componentLayoutBinding.stageFlags = args.stageFlags,
                                                 componentLayoutBinding.pImmutableSamplers = nullptr, found = true) :
                                                false),
                 ...);
            },
            ConfigTable);
        if (!found) {
            throw std::runtime_error(" failed to create DescriptorSetLayoutBinding!");
        }
        return componentLayoutBinding;
    }

private:
    std::unordered_map<ComponentEnum, ResourceId> m_compoRscIdMap{};
    std::unordered_map<ResourceId, TextureImageSpec, ResourceIdHasher> m_rscIdTextureSpecMap{};
    std::unordered_map<ResourceId, GBufferSpec, ResourceIdHasher> m_rscIdGBuffSpecMap{};
    std::unordered_map<ResourceId, vk::DescriptorType, ResourceIdHasher> m_allDescriptorTypeMap{};
    std::vector<vk::DescriptorSetLayoutBinding> m_setLayoutBindings{};
    vk::DescriptorSetLayoutCreateInfo m_descriptorSetLayoutCreateInfo{};
};

// NOTE: all config need match shader-spv
// set0, eStat1c for Global
enum class GlobalComponent { ebaseColor, eSpecularMap, eNormalMap, eHeightMap, eMaterialParams };
// Static configuration table of GlobalParam.
using GlobalParamConfig = ComponentParamConfigure<GlobalComponent>;
inline constexpr auto GlobalParamComponentTable =
    std::make_tuple(GlobalParamConfig{GlobalComponent::ebaseColor, vk::DescriptorType::eCombinedImageSampler, 1,
                                      vk::ShaderStageFlagBits::eFragment},
                    GlobalParamConfig{GlobalComponent::eSpecularMap, vk::DescriptorType::eCombinedImageSampler, 1,
                                      vk::ShaderStageFlagBits::eFragment},
                    GlobalParamConfig{GlobalComponent::eNormalMap, vk::DescriptorType::eCombinedImageSampler, 1,
                                      vk::ShaderStageFlagBits::eFragment},
                    GlobalParamConfig{GlobalComponent::eHeightMap, vk::DescriptorType::eCombinedImageSampler, 1,
                                      vk::ShaderStageFlagBits::eFragment},
                    GlobalParamConfig{GlobalComponent::eMaterialParams, vk::DescriptorType::eStorageBuffer, 1,
                                      vk::ShaderStageFlagBits::eFragment});

class GlobalParam : public DescriptorParam<GlobalComponent, 0, GlobalParamComponentTable> {
public:
    GlobalParam(const std::vector<GlobalComponent> &components) : DescriptorParam(components) {}
    virtual ~GlobalParam() = default;
};

// set1. eEnvironment for Occasional updates
enum class OccaUpdateComponent { eCameraMatrix };
// Static configuration table of OccaUpdateParam
using OccaUpdateParamConfig = ComponentParamConfigure<OccaUpdateComponent>;
inline constexpr auto OccaUpdateParamComponentTable = std::make_tuple(OccaUpdateParamConfig{
    OccaUpdateComponent::eCameraMatrix, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex});

class OccaUpdateParam : public DescriptorParam<OccaUpdateComponent, 1, OccaUpdateParamComponentTable> {
public:
    OccaUpdateParam(const std::vector<OccaUpdateComponent> &components) : DescriptorParam(components) {}
    virtual ~OccaUpdateParam() = default;
};

// set2, eFachFrame for Updated every frame
enum class SceneFrameComponent { eLightSource, eViewEyePosition };

// Static configuration table of SceneFrameParam
using SceneFrameParamConfig = ComponentParamConfigure<SceneFrameComponent>;
inline constexpr auto SceneFrameParamComponentTable =
    std::make_tuple(SceneFrameParamConfig{SceneFrameComponent::eLightSource, vk::DescriptorType::eUniformBuffer, 1,
                                          vk::ShaderStageFlagBits::eFragment},
                    SceneFrameParamConfig{SceneFrameComponent::eViewEyePosition, vk::DescriptorType::eUniformBuffer, 1,
                                          vk::ShaderStageFlagBits::eFragment});

class SceneFrameParam : public DescriptorParam<SceneFrameComponent, 2, SceneFrameParamComponentTable> {
public:
    SceneFrameParam(const std::vector<SceneFrameComponent> &components) : DescriptorParam(components) {}
    virtual ~SceneFrameParam() = default;
};

// set3, eEachInstance for Object
class ObjectParam //: public DescriptorParam<ObjectComponent, 3,
                  // ObjectParamComponentTable>
{
public:
    ObjectParam() = default;
    virtual ~ObjectParam() = default;
};

// all param_is in pushconstant here
enum class PushConstantComponent { eModelMatrix };
class PushConstantParam {
public:
    PushConstantParam(const std::vector<PushConstantComponent> &components)
    {
        for (auto &component : components) {
            m_allComponentsSpecMap.emplace(component, GBufferSpec{});
        }
    }
    virtual ~PushConstantParam() = default;
    void SetSpec(PushConstantComponent c, GBufferSpec spec)
    {
        auto it = m_allComponentsSpecMap.find(c);
        if (it != m_allComponentsSpecMap.end()) {
            // std:: cout << " spec. getTotalByte(): " << spec. getTotalByte() <<
            // "\n",
            it->second = std::move(spec);
            m_pushConstantRanges.push_back(buildPushConstantRanges(it->second));
        }
    }

    std::vector<vk::PushConstantRange> getPushConstantRanges() const
    {
        return m_pushConstantRanges;
    }

    vk::PushConstantRange buildPushConstantRanges(GBufferSpec bufferspec) const
    {
        vk::PushConstantRange pushConstantRange{};
        pushConstantRange.setStageFlags(vk::ShaderStageFlagBits::eVertex);
        pushConstantRange.setOffset(0);
        pushConstantRange.setSize(bufferspec.getTotalByte());
        return pushConstantRange;
    }

private:
    std::unordered_map<PushConstantComponent, GBufferSpec> m_allComponentsSpecMap; // pushconstant
    std::vector<vk::PushConstantRange> m_pushConstantRanges{};
};

class PipelineDescriptorParams {
public:
    PipelineDescriptorParams(GlobalParam globalMeta, OccaUpdateParam occaUpdateMeta, SceneFrameParam sceneMeta,
                             PushConstantParam pushconstantMeta) :
        m_globalMeta(std::move(globalMeta)), m_occaUpdateMeta(std::move(occaUpdateMeta)),
        m_sceneMeta(std::move(sceneMeta)), m_pushconstantMeta(std::move(pushconstantMeta))
    {
        m_setsLayoutCreateInfos.push_back(m_globalMeta.getSetLayoutCreateInfo());
        m_setsLayoutCreateInfos.push_back(m_occaUpdateMeta.getSetLayoutCreateInfo());
        m_setsLayoutCreateInfos.push_back(m_sceneMeta.getSetLayoutCreateInfo());

        buildPoolCreateInfo();
    }

    template <typename F>
    void for_each_binding(uint32_t currentFrame,
                          F &&callback) const // for each binding (in each set)
    {
        m_globalMeta.for_each_component(currentFrame, callback);
        m_occaUpdateMeta.for_each_component(currentFrame, callback);
        m_sceneMeta.for_each_component(currentFrame, callback);
    }

    template <typename F>
    void for_each_set(uint32_t currentFrame, F &&callback) const // for each set
    {
        callback(currentFrame, m_globalMeta.getSetId());
        callback(currentFrame, m_occaUpdateMeta.getSetId());
        callback(currentFrame, m_sceneMeta.getSetId());
    }

    std::vector<vk::PushConstantRange> getPushConstantRanges() const
    {
        return m_pushconstantMeta.getPushConstantRanges();
    }

    std::vector<vk::DescriptorSetLayoutCreateInfo> getSetsLayoutCreateInfo() const
    {
        return m_setsLayoutCreateInfos;
    }

    vk::DescriptorPoolCreateInfo getDescriptorPoolCreateInfo() const
    {
        return m_descriptorPoolInfo;
    }

    uint32_t MaxFrameCountInFlight() const
    {
        return static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    }

    uint32_t totalSetsCountEachFrame() const
    {
        return m_setsLayoutCreateInfos.size(); // set0, set1, set2 not need descriptor now
    }

    uint32_t maxSets() const
    {
        return MaxFrameCountInFlight() * totalSetsCountEachFrame();
    }

private:
    void getDescriptorPoolSizes()
    {
        // for vk::DescrıptorType type, descriptorCount
        // all set0, set1, set2 need how much UBO/ Sampler/SSBO total
        size_t uboCount = count_type(GlobalParamComponentTable, vk::DescriptorType::eUniformBuffer);
        uboCount += count_type(OccaUpdateParamComponentTable, vk::DescriptorType::eUniformBuffer);
        uboCount += count_type(SceneFrameParamComponentTable, vk::DescriptorType::eUniformBuffer);

        size_t samplerCount = count_type(GlobalParamComponentTable, vk::DescriptorType::eCombinedImageSampler);
        samplerCount += count_type(OccaUpdateParamComponentTable, vk::DescriptorType::eCombinedImageSampler);
        samplerCount += count_type(SceneFrameParamComponentTable, vk::DescriptorType::eCombinedImageSampler);

        size_t ssboCount = count_type(GlobalParamComponentTable, vk::DescriptorType::eStorageBuffer);
        ssboCount += count_type(OccaUpdateParamComponentTable, vk::DescriptorType::eStorageBuffer);
        ssboCount += count_type(SceneFrameParamComponentTable, vk::DescriptorType::eStorageBuffer);

        std::cout << " total uboCount: " << uboCount << ", total sampler Count: " << samplerCount
                  << ", total ssboCount: " << ssboCount << "\n";

        m_descriptorPoolSizes.push_back(
            vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, MaxFrameCountInFlight() * uboCount));
        m_descriptorPoolSizes.push_back(
            vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, MaxFrameCountInFlight() * samplerCount));
        m_descriptorPoolSizes.push_back(
            vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, MaxFrameCountInFlight() * ssboCount));
    }

    void buildPoolCreateInfo()
    {
        getDescriptorPoolSizes();
        m_descriptorPoolInfo.poolSizeCount = static_cast<uint32_t>(m_descriptorPoolSizes.size());
        m_descriptorPoolInfo.pPoolSizes = m_descriptorPoolSizes.data();
        m_descriptorPoolInfo.maxSets = maxSets();
    }

private:
    GlobalParam m_globalMeta;         // sel0
    OccaUpdateParam m_occaUpdateMeta; // set1
    SceneFrameParam m_sceneMeta;      // sel2
    PushConstantParam m_pushconstantMeta;

    std::vector<vk::DescriptorSetLayoutCreateInfo> m_setsLayoutCreateInfos;
    std::vector<vk::DescriptorPoolSize> m_descriptorPoolSizes{};
    vk::DescriptorPoolCreateInfo m_descriptorPoolInfo{};
};