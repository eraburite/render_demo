#pragma once
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "RayTracingCommon.h"

namespace rt {

enum class ShaderGroupType {
    eGeneral,       // raygen / miss / callable
    eTrianglesHit,  // closest-hit (+ optional any-hit) for triangle geometry
    eProceduralHit,
};

// One shader group as the caller declares it.  The integers index into the
// PipelineDesc::stages array, so a stage can be shared by several groups.
// recordData is the shaderRecordEXT payload appended after the group handle in
// the SBT record; raytracingsbtdata uses it to colour background/hit per record.
struct ShaderGroupDesc {
    ShaderGroupType type = ShaderGroupType::eGeneral;
    int general = -1;
    int closestHit = -1;
    int anyHit = -1;
    int intersection = -1;
    std::vector<uint8_t> recordData;
};

// Pure-CPU description of the four SBT regions.  Kept free of Vulkan objects so
// --self-test can validate the arithmetic (alignment, stride, total size) on a
// machine with no ray tracing support at all (see DESIGN.md 31.4).
//
// Group order is fixed: raygen, then miss, then hit, then callable -- the same
// order the pipeline builds its group array in, which is what makes the SBT
// handle index equal to the group index.
struct ShaderBindingLayout {
    struct Region {
        uint32_t firstGroup = 0;
        uint32_t groupCount = 0;
        // Pitch between consecutive records: aligned handle + the largest
        // recordData in the region, rounded up to the handle alignment.
        uint32_t recordStride = 0;
        // Byte offset of the region inside the single SBT buffer.  Must be a
        // multiple of shaderGroupBaseAlignment or vkCmdTraceRaysKHR is invalid.
        uint64_t offset = 0;
        uint64_t byteSize = 0;
    };

    Region raygen;
    Region miss;
    Region hit;
    Region callable;

    uint32_t handleSize = 0;
    uint32_t handleSizeAligned = 0;
    uint32_t handleAlignment = 0;
    uint32_t baseAlignment = 0;
    uint32_t totalGroupCount = 0;
    uint64_t totalSize = 0;

    struct RegionSpec {
        uint32_t groupCount = 0;
        // One entry per group; a region uses the largest value as its stride.
        std::vector<uint32_t> recordDataSizes;
    };

    static ShaderBindingLayout Compute(const Capabilities &caps, const RegionSpec &raygen, const RegionSpec &miss,
                                       const RegionSpec &hit, const RegionSpec &callable);

    // True when every region offset is base-aligned and every stride can hold an
    // aligned handle -- the two invariants that make the table valid.
    bool IsValid() const;
};

} // namespace rt
