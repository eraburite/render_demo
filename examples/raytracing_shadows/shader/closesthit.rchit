#version 460
#extension GL_EXT_ray_tracing : require

// The closest-hit shader traces a second ray, so it needs the acceleration
// structure too (the descriptor binding lists eClosestHitKHR in its stageFlags).
layout(binding = 0, set = 0) uniform accelerationStructureEXT topLevelAS;

layout(location = 0) rayPayloadInEXT vec3 hitValue;
// Payload of the *child* ray.  Declaring it here is what makes the shadow ray
// result visible to this stage.
layout(location = 1) rayPayloadEXT float shadowVisibility;

// Both quads face the camera, so the shading normal is a constant.  Keeping it
// constant is deliberate: the shadow test is about the secondary ray, not about
// interpolation.
const vec3 kNormal = vec3(0.0, 0.0, 1.0);
// Direction *towards* the light.  Mirrored by kLightDirection in test43.cpp so
// --verify can predict where the shadow lands.
const vec3 kLightDirection = normalize(vec3(0.5, 0.6, 0.7));

void main()
{
    // The BLAS holds two quads: primitives 0-1 are the back panel, 2-3 the
    // floating blocker that casts the shadow.
    const vec3 albedo = (gl_PrimitiveID < 2) ? vec3(0.86, 0.52, 0.32) : vec3(0.45, 0.62, 0.86);
    const vec3 hitPosition = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;

    // Occluded unless the shadow miss shader proves otherwise.
    shadowVisibility = 0.0;
    // sbtRecordOffset 1 selects the occlusion hit group (whose closest-hit is
    // intentionally empty) and missIndex 1 the shadow miss shader.
    // TerminateOnFirstHit stops at the first blocker instead of shading it.
    traceRayEXT(topLevelAS, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, 1, 0, 1,
                hitPosition + kNormal * 0.001, 0.001, kLightDirection, 1000.0, 1);

    hitValue = albedo * mix(0.12, 1.0, shadowVisibility);
}
