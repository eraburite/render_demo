#pragma once
#include <cstdint>
#include <tuple>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "ComputeLayoutConfig.h"
#include "IDescriptorSetDetail.h"
#include "typedefs.h"

// Three single-buffered SSBOs on set 0: packed RAW12 words, unpacked 16-bit
// samples (two per uint32) and the final 3-float RGB image.
enum class Raw2RgbComponent { eRawImage, eUnpackedImage, eRgbImage };

using Raw2RgbConfig = DescriptorComponentSpec<Raw2RgbComponent>;

inline constexpr auto Raw2RgbComponentTable = std::make_tuple(
    Raw2RgbConfig{Raw2RgbComponent::eRawImage, vk::DescriptorType::eStorageBuffer, 1,
                  vk::ShaderStageFlagBits::eCompute},
    Raw2RgbConfig{Raw2RgbComponent::eUnpackedImage, vk::DescriptorType::eStorageBuffer, 1,
                  vk::ShaderStageFlagBits::eCompute},
    Raw2RgbConfig{Raw2RgbComponent::eRgbImage, vk::DescriptorType::eStorageBuffer, 1,
                  vk::ShaderStageFlagBits::eCompute});

using Raw2RgbLayoutAtlas = ComputeLayoutAtlasTemplate<Raw2RgbComponent, Raw2RgbComponentTable>;
