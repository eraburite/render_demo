#include "VmdAnimationLoader.h"

#include "ModelBinaryReader.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct MotionKey {
    std::string boneName;
    uint32_t frame = 0;
    glm::vec3 translate{};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

} // namespace

int VmdAnimationLoader::probe(const uint8_t *data, size_t size) const
{
    // "Vocaloid Motion Data" is 20 bytes; both "0002" and the older header
    // share this prefix.  A 100 score beats any extension-based guess.
    return modelio::startsWith(data, size, "Vocaloid Motion Data") ? 100 : 0;
}

Model VmdAnimationLoader::load(const std::string &path, const ModelLoadOptions &) const
{
    throw std::runtime_error("VMD '" + path + "' contains motion only; use loadModelAnimation()");
}

AnimationClip VmdAnimationLoader::loadAnimation(const std::string &path) const
{
    const std::vector<uint8_t> bytes = modelio::readBinaryFile(path);
    modelio::BinaryReader r(bytes.data(), bytes.size());

    const std::string header = r.fixedString(30);
    if (header.rfind("Vocaloid Motion Data", 0) != 0) {
        throw std::runtime_error("VMD: bad header in '" + path + "'");
    }
    const std::string modelName = r.fixedString(20);

    const uint32_t motionCount = r.u32();
    if (motionCount > 1000000) throw std::runtime_error("VMD: implausible motion count");
    std::vector<MotionKey> keys(motionCount);
    uint32_t maxFrame = 0;
    for (MotionKey &key : keys) {
        key.boneName = r.fixedString(15);
        key.frame = r.u32();
        key.translate = r.vec3();
        const float x = r.f32(), y = r.f32(), z = r.f32(), w = r.f32();
        key.rotation = glm::quat(w, x, y, z); // file order is x,y,z,w
        r.skip(64);                            // bezier interpolation, unused
        maxFrame = std::max(maxFrame, key.frame);
    }
    // Morph / camera / light / shadow / IK sections are not part of skeletal
    // animation and are ignored (their counts are read so an offset is at least
    // validated when present).
    for (int section = 0; section < 5 && !r.eof(); ++section) {
        const uint32_t count = r.u32();
        (void)count;
        if (count != 0) break; // a non-empty optional section: stop, we do not decode it
    }

    AnimationClip clip;
    clip.name = modelName.empty() ? "vmd" : modelName;
    clip.ticksPerSecond = 30.0f; // VMD's fixed frame rate
    clip.duration = static_cast<float>(maxFrame);

    // Group by bone name while preserving first-seen order.
    std::unordered_map<std::string, size_t> channelByBone;
    for (const MotionKey &key : keys) {
        size_t channelIndex = 0;
        auto found = channelByBone.find(key.boneName);
        if (found == channelByBone.end()) {
            channelIndex = clip.channels.size();
            channelByBone.emplace(key.boneName, channelIndex);
            AnimationChannel channel;
            channel.nodeName = key.boneName;
            channel.nodeIndex = -1; // bound later against a Model
            clip.channels.push_back(std::move(channel));
        } else {
            channelIndex = found->second;
        }
        AnimationChannel &channel = clip.channels[channelIndex];
        channel.positions.push_back(VectorKey{static_cast<float>(key.frame), key.translate});
        channel.rotations.push_back(QuaternionKey{static_cast<float>(key.frame), key.rotation});
    }
    for (AnimationChannel &channel : clip.channels) {
        std::sort(channel.positions.begin(), channel.positions.end(),
                  [](const VectorKey &a, const VectorKey &b) { return a.time < b.time; });
        std::sort(channel.rotations.begin(), channel.rotations.end(),
                  [](const QuaternionKey &a, const QuaternionKey &b) { return a.time < b.time; });
    }
    return clip;
}
