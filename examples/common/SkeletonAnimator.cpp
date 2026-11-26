#include "SkeletonAnimator.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace {

// Keyframe tracks are short (tens to a few hundred keys), so the segment search
// is a plain binary search over the time column.
template <typename Key>
float segmentFactor(const std::vector<Key> &keys, size_t hi, float tick)
{
    const float span = keys[hi].time - keys[hi - 1].time;
    if (span <= 0.0f) {
        return 0.0f;
    }
    return (tick - keys[hi - 1].time) / span;
}

glm::vec3 sampleVector(const std::vector<VectorKey> &keys, float tick, const glm::vec3 &fallback)
{
    if (keys.empty()) {
        return fallback;
    }
    if (keys.size() == 1 || tick <= keys.front().time) {
        return keys.front().value;
    }
    if (tick >= keys.back().time) {
        return keys.back().value;
    }
    const auto it = std::upper_bound(keys.begin(), keys.end(), tick,
                                     [](float value, const VectorKey &key) { return value < key.time; });
    const size_t hi = static_cast<size_t>(it - keys.begin());
    if (hi == 0) {
        return keys.front().value;
    }
    return glm::mix(keys[hi - 1].value, keys[hi].value, segmentFactor(keys, hi, tick));
}

glm::quat sampleRotation(const std::vector<QuaternionKey> &keys, float tick, const glm::quat &fallback)
{
    if (keys.empty()) {
        return fallback;
    }
    if (keys.size() == 1 || tick <= keys.front().time) {
        return glm::normalize(keys.front().value);
    }
    if (tick >= keys.back().time) {
        return glm::normalize(keys.back().value);
    }
    const auto it = std::upper_bound(keys.begin(), keys.end(), tick,
                                     [](float value, const QuaternionKey &key) { return value < key.time; });
    const size_t hi = static_cast<size_t>(it - keys.begin());
    if (hi == 0) {
        return glm::normalize(keys.front().value);
    }
    // glm::slerp takes the short path when the two orientations are more than
    // half a turn apart; taking the long way would spin a joint the wrong way
    // for one key interval, which is the classic imported-animation flicker.
    return glm::normalize(glm::slerp(keys[hi - 1].value, keys[hi].value, segmentFactor(keys, hi, tick)));
}

} // namespace

void SkeletonAnimator::bind(const Model &model, int clipIndex)
{
    m_model = &model;
    m_clip = (clipIndex >= 0 && clipIndex < static_cast<int>(model.clips.size())) ? &model.clips[clipIndex] : nullptr;

    m_locals.assign(model.nodes.size(), glm::mat4(1.0f));
    m_nodeWorld.assign(model.nodes.size(), glm::mat4(1.0f));
    m_jointMatrices.assign(model.skins.empty() ? 0u : model.skins[0].joints.size(), glm::mat4(1.0f));

    m_bindTranslation.assign(model.nodes.size(), glm::vec3(0.0f));
    m_bindRotation.assign(model.nodes.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    m_bindScale.assign(model.nodes.size(), glm::vec3(1.0f));
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        glm::vec3 skew(0.0f);
        glm::vec4 perspective(0.0f);
        // Only animated nodes are ever recomposed from these, and those are bone
        // transforms, i.e. TRS by construction.  Nodes the clip does not touch
        // keep the exact matrix the file carried, decomposition or not.
        (void)glm::decompose(model.nodes[i].transform, m_bindScale[i], m_bindRotation[i], m_bindTranslation[i], skew,
                             perspective);
    }

    m_timeSeconds = 0.0f;
    m_playing = true;

    if (m_clip) {
        sampleFromTracks();
    } else {
        bindPose();
    }
}

void SkeletonAnimator::bindPose()
{
    if (!m_model) {
        return;
    }
    for (size_t i = 0; i < m_model->nodes.size(); ++i) {
        m_locals[i] = m_model->nodes[i].transform;
    }
    resolveWorldAndJoints();
}

float SkeletonAnimator::duration() const
{
    return m_clip ? m_clip->durationSeconds() : 0.0f;
}

float SkeletonAnimator::wrapTime(float seconds) const
{
    const float length = duration();
    if (length <= 0.0f) {
        return 0.0f;
    }
    float wrapped = std::fmod(seconds, length);
    if (wrapped < 0.0f) {
        wrapped += length;
    }
    return wrapped;
}

void SkeletonAnimator::setTime(float seconds)
{
    m_timeSeconds = wrapTime(seconds);
    if (m_clip) {
        sampleFromTracks();
    } else {
        bindPose();
    }
}

void SkeletonAnimator::update(float deltaSeconds)
{
    if (!m_clip || !m_playing) {
        return;
    }
    m_timeSeconds = wrapTime(m_timeSeconds + deltaSeconds);
    sampleFromTracks();
}

void SkeletonAnimator::sampleFromTracks()
{
    if (!m_model || !m_clip) {
        return;
    }

    // Seed every node with the transform the file gave it, then let the clip
    // overwrite only the nodes it actually drives.  Skipping this makes every
    // non-animated node identity -- including the armature root, whose 0.01 scale
    // is what converts this .dae's 270-unit mesh into a 2.7-unit character.
    for (size_t i = 0; i < m_locals.size(); ++i) {
        m_locals[i] = m_model->nodes[i].transform;
    }

    // Track times are in ticks, so the seconds the caller deals in have to be
    // converted before they can be compared against a key.
    const float tick = m_timeSeconds * m_clip->ticksPerSecond;

    for (const AnimationChannel &channel : m_clip->channels) {
        if (channel.nodeIndex < 0 || channel.nodeIndex >= static_cast<int>(m_locals.size())) {
            continue;
        }
        const size_t node = static_cast<size_t>(channel.nodeIndex);
        const glm::vec3 translation = sampleVector(channel.positions, tick, m_bindTranslation[node]);
        const glm::quat rotation = sampleRotation(channel.rotations, tick, m_bindRotation[node]);
        const glm::vec3 scale = sampleVector(channel.scales, tick, m_bindScale[node]);
        m_locals[node] = glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation)
                         * glm::scale(glm::mat4(1.0f), scale);
    }

    resolveWorldAndJoints();
}

void SkeletonAnimator::resolveWorldAndJoints()
{
    m_nodeWorld = computeNodeWorldMatrices(*m_model, m_locals);

    static const std::vector<Joint> kNoJoints;
    const std::vector<Joint> &joints = m_model->skins.empty() ? kNoJoints : m_model->skins[0].joints;
    m_jointMatrices.resize(joints.size());
    for (size_t j = 0; j < joints.size(); ++j) {
        const Joint &joint = joints[j];
        if (joint.nodeIndex < 0 || joint.nodeIndex >= static_cast<int>(m_nodeWorld.size())) {
            // The loader reports these; a joint with no node cannot move, so the
            // least surprising fallback is "leave its vertices alone".
            m_jointMatrices[j] = glm::mat4(1.0f);
            continue;
        }
        m_jointMatrices[j] = m_nodeWorld[joint.nodeIndex] * joint.inverseBindMatrix;
    }
}

void SkeletonAnimator::computeSkinnedBounds(glm::vec3 &outMin, glm::vec3 &outMax) const
{
    outMin = glm::vec3(0.0f);
    outMax = glm::vec3(0.0f);
    if (!m_model || !m_model->isSkinned() || m_jointMatrices.empty()) {
        return;
    }

    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    const size_t count = m_model->vertices.size();
    for (size_t i = 0; i < count; ++i) {
        const glm::uvec4 &indices = m_model->boneIndices[i];
        const glm::vec4 &weights = m_model->boneWeights[i];
        const glm::vec4 position(m_model->vertices[i].position, 1.0f);
        glm::vec4 skinned(0.0f);
        float total = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float weight = weights[k];
            if (weight <= 0.0f || indices[k] >= m_jointMatrices.size()) {
                continue;
            }
            skinned += (m_jointMatrices[indices[k]] * position) * weight;
            total += weight;
        }
        const glm::vec3 p = total > 0.0f ? glm::vec3(skinned) / total : m_model->vertices[i].position;
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    outMin = lo;
    outMax = hi;
}

