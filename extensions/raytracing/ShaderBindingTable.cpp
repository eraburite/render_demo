#include "ShaderBindingTable.h"

#include <algorithm>

namespace rt {

ShaderBindingLayout ShaderBindingLayout::Compute(const Capabilities &caps, const RegionSpec &raygen,
                                                 const RegionSpec &miss, const RegionSpec &hit,
                                                 const RegionSpec &callable)
{
    ShaderBindingLayout layout;
    layout.handleSize = caps.shaderGroupHandleSize();
    layout.handleAlignment = std::max<uint32_t>(1, caps.shaderGroupHandleAlignment());
    layout.baseAlignment = std::max<uint32_t>(1, caps.shaderGroupBaseAlignment());
    layout.handleSizeAligned = AlignUp(layout.handleSize, layout.handleAlignment);

    const auto fill = [&layout](Region &region, const RegionSpec &spec, uint32_t firstGroup) {
        region.firstGroup = firstGroup;
        region.groupCount = spec.groupCount;
        uint32_t largestRecordData = 0;
        for (uint32_t size : spec.recordDataSizes) {
            largestRecordData = std::max(largestRecordData, size);
        }
        // 1) leave room for the handle, 2) append the record data, 3) round the
        // whole record up to the handle alignment so the next record also starts
        // on an aligned boundary.
        region.recordStride = AlignUp(layout.handleSizeAligned + largestRecordData, layout.handleAlignment);
    };

    fill(layout.raygen, raygen, 0);
    fill(layout.miss, miss, raygen.groupCount);
    fill(layout.hit, hit, raygen.groupCount + miss.groupCount);
    fill(layout.callable, callable, raygen.groupCount + miss.groupCount + hit.groupCount);
    layout.totalGroupCount = raygen.groupCount + miss.groupCount + hit.groupCount + callable.groupCount;

    uint64_t cursor = 0;
    const auto place = [&layout, &cursor](Region &region) {
        region.offset = AlignUp64(cursor, layout.baseAlignment);
        region.byteSize = static_cast<uint64_t>(region.recordStride) * region.groupCount;
        cursor = region.offset + region.byteSize;
    };
    place(layout.raygen);
    place(layout.miss);
    place(layout.hit);
    place(layout.callable);
    layout.totalSize = AlignUp64(cursor, layout.baseAlignment);
    return layout;
}

bool ShaderBindingLayout::IsValid() const
{
    if (handleAlignment == 0 || baseAlignment == 0) {
        return false;
    }
    if (handleSizeAligned % handleAlignment != 0 || handleSizeAligned < handleSize) {
        return false;
    }
    const Region *regions[4] = {&raygen, &miss, &hit, &callable};
    for (const Region *region : regions) {
        if (region->offset % baseAlignment != 0) {
            return false;
        }
        if (region->groupCount > 0 && region->recordStride < handleSizeAligned) {
            return false;
        }
    }
    return totalSize >= callable.offset + callable.byteSize;
}

} // namespace rt
