// Renderer policy is shared by native Vulkan and Metal Forward passes.
[[vk::constant_id(0)]] const bool forward_capture = false;
#define MERLIN_FORWARD_OPAQUE_CAPTURE forward_capture
#ifdef MERLIN_FORWARD_SCENE_SET
[[vk::binding(1, MERLIN_FORWARD_SCENE_SET)]] Texture2D<float4> opaque_scene_color;
[[vk::binding(2, MERLIN_FORWARD_SCENE_SET)]] Texture2D<float> opaque_scene_depth;
#endif
#include "../../../core/merlin-render-backend/shaders/forward-policy.slang"
