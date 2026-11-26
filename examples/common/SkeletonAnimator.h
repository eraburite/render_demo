#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "ModelAsset.h"

// Plays one AnimationClip against a Model and produces the per-joint matrices a
// skinned vertex shader needs.
//
// The reference implementation (LearnOpenGL's Animator + Animation + Bone) walks
// its own private copy of the node tree, rebuilt by importing the file a second
// time.  Here the clip and the hierarchy both come out of the single import that
// produced the geometry, so this class only has to do what a renderer needs:
// sample the tracks, rebuild local matrices, rebuild world matrices, multiply by
// the inverse bind matrices.
//
//     finalJoint[j] = worldMatrix(skins[0].joints[j].nodeIndex) * inverseBindMatrix[j]
//
// which is the identity at bind pose -- that property is what the static
// cross-check in DESIGN.md §21.5 exploits.
class SkeletonAnimator {
public:
    SkeletonAnimator() = default;

    // clipIndex < 0 binds no clip: the animator then only reports the bind pose.
    SkeletonAnimator(const Model &model, int clipIndex = 0) { bind(model, clipIndex); }

    // The model must outlive the animator; only its nodes/skins/clips are read.
    void bind(const Model &model, int clipIndex);

    // No clip applied: locals are each node's static transform, so every joint
    // matrix is the identity and a skinned draw reproduces the unskinned mesh.
    void bindPose();

    // Advance playback and resample.  Does nothing while paused or clip-less.
    void update(float deltaSeconds);

    // Jump to an absolute time in seconds (wrapped into [0, duration)).
    void setTime(float seconds);
    void setPlaying(bool playing) { m_playing = playing; }
    bool playing() const { return m_playing; }

    bool hasClip() const { return m_clip != nullptr; }
    float time() const { return m_timeSeconds; }
    // 0 when there is no clip.
    float duration() const;
    uint32_t jointCount() const { return static_cast<uint32_t>(m_jointMatrices.size()); }

    const std::vector<glm::mat4> &jointMatrices() const { return m_jointMatrices; }
    const std::vector<glm::mat4> &nodeWorldMatrices() const { return m_nodeWorld; }

    // Bounds of the geometry at the current pose, computed by skinning every
    // vertex on the CPU with the same weights the shader uses.
    //
    // Neither Model::minBounds (mesh space) nor computeWorldBounds (mesh space
    // pushed through the node hierarchy) describes what a skinned model draws: a
    // file may author its scene pose somewhere else entirely -- this vampire's
    // node transforms place the character a full unit to the left of the mesh it
    // deforms.  Only the skinned result is where the pixels land.
    void computeSkinnedBounds(glm::vec3 &outMin, glm::vec3 &outMax) const;

private:
    void sampleFromTracks();
    void resolveWorldAndJoints();
    float wrapTime(float seconds) const;

    const Model *m_model = nullptr;
    const AnimationClip *m_clip = nullptr;

    std::vector<glm::mat4> m_locals;
    std::vector<glm::mat4> m_nodeWorld;
    std::vector<glm::mat4> m_jointMatrices;

    // Bind-pose TRS per node, used for whichever track a channel does not carry.
    std::vector<glm::vec3> m_bindTranslation;
    std::vector<glm::quat> m_bindRotation;
    std::vector<glm::vec3> m_bindScale;

    float m_timeSeconds = 0.0f;
    bool m_playing = true;
};
