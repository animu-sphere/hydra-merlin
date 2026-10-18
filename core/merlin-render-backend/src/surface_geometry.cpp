#include <merlin/render/surface_geometry.hpp>

#include <string>

namespace merlin::render {

FrontFaceWinding ForwardDrawFrontFace(FrontFaceWinding camera,
    bool mirrored) noexcept {
  if (!mirrored) {
    return camera;
  }
  return camera == FrontFaceWinding::Clockwise
             ? FrontFaceWinding::CounterClockwise
             : FrontFaceWinding::Clockwise;
}

bool ForwardNormalMapDisabled(const extraction::GeometryRecord& geometry,
    const extraction::MaterialRecord& material) noexcept {
  return material.normal_texture.has_value() &&
         HasMaterialFeature(material.features,
             MaterialFeature::NormalTexture) &&
         !geometry.has_texcoords;
}

std::size_t CountForwardNormalMapDisabledDraws(
    const extraction::FrameSnapshot& snapshot) noexcept {
  std::size_t count{};
  for (const auto& draw : snapshot.draws) {
    if (draw.geometry_index < snapshot.geometries.size() &&
        draw.material_index < snapshot.materials.size() &&
        ForwardNormalMapDisabled(snapshot.geometries[draw.geometry_index],
            snapshot.materials[draw.material_index])) {
      ++count;
    }
  }
  return count;
}

bool IsForwardSurfaceDebugView(DebugView view) noexcept {
  switch (view) {
  case DebugView::None:
  case DebugView::Normal:
  case DebugView::GeometricNormal:
  case DebugView::ShadingNormalBase:
  case DebugView::FrontFacing:
  case DebugView::Tangent:
  case DebugView::Bitangent:
    return true;
  case DebugView::Color:
  case DebugView::Depth:
  case DebugView::PrimId:
  case DebugView::InstanceId:
    return false;
  }
  return false;
}

Vec4 PackForwardSurfacePolicy(FrontFaceWinding camera, NdcYDirection ndc_y,
    DebugView view) {
  float code{};
  switch (view) {
  case DebugView::None:
    code = 0.0F;
    break;
  case DebugView::GeometricNormal:
    code = 1.0F;
    break;
  case DebugView::ShadingNormalBase:
    code = 2.0F;
    break;
  case DebugView::Normal:
    code = 3.0F;
    break;
  case DebugView::FrontFacing:
    code = 4.0F;
    break;
  case DebugView::Tangent:
    code = 5.0F;
    break;
  case DebugView::Bitangent:
    code = 6.0F;
    break;
  default:
    throw RendererError(RendererErrorCode::InvalidRequest,
        "pack Forward surface policy",
        "debug view " + std::string(DebugViewName(view)) +
            " is not executed by the Forward surface context");
  }
  // cross(ddx(p), ddy(p)) is the triangle's oriented normal divided by its
  // signed framebuffer area. A front face has the area sign of the effective
  // winding, and mirroring reverses both terms, so only the camera winding
  // and the NDC Y direction remain.
  const float winding = camera == FrontFaceWinding::Clockwise ? 1.0F : -1.0F;
  const float direction = ndc_y == NdcYDirection::Down ? 1.0F : -1.0F;
  return {winding * direction, code, 0.0F, 0.0F};
}

} // namespace merlin::render
