#include <merlin/render/surface_geometry.hpp>
#include <merlin/render/forward_lighting.hpp>

#include <limits>
#include <string>
#include <unordered_map>

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
  return PackForwardSurfacePolicy(camera, ndc_y, view, MeshDisplayMode::Smooth);
}

Vec4 PackForwardSurfacePolicy(FrontFaceWinding camera, NdcYDirection ndc_y,
    DebugView view, MeshDisplayMode mode) {
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
  float geometry_color{};
  if (mode == MeshDisplayMode::Geometry) {
    geometry_color = 1.0F;
  } else if (mode == MeshDisplayMode::HiddenSurfaceWireframe) {
    geometry_color = 2.0F;
  }
  return {winding * direction, code,
      mode == MeshDisplayMode::Flat ? 1.0F : 0.0F, geometry_color};
}

MeshDisplayBatch PrepareMeshDisplay(const extraction::FrameSnapshot& snapshot,
    std::uint32_t width, std::uint32_t height, const MeshDisplaySettings& display) {
  MeshDisplayBatch batch;
  if (display.draws.empty()) {
    return batch;
  }
  const auto view_projection = MultiplyForwardMatrices(
      snapshot.projection, snapshot.view);
  const auto extent = display.mode == MeshDisplayMode::Points
      ? Vec2{display.point_size / static_cast<float>(width),
            display.point_size / static_cast<float>(height)}
      : Vec2{};
  // All instances of one prototype share a single upload within this frame.
  std::unordered_map<const std::vector<MeshDisplayVertex>*, std::size_t> offsets;
  for (const auto& draw : display.draws) {
    if (!draw.vertices || draw.vertices->empty()) {
      continue;
    }
    if (draw.vertices->size() > std::numeric_limits<std::uint32_t>::max()) {
      throw RendererError(RendererErrorCode::Unsupported,
          "prepare mesh display", "display vertex count exceeds uint32 range");
    }
    const auto [offset, added] = offsets.emplace(draw.vertices.get(),
        batch.vertices.size());
    if (added) {
      batch.vertices.insert(batch.vertices.end(), draw.vertices->begin(),
          draw.vertices->end());
    }
    batch.draws.push_back({offset->second,
        static_cast<std::uint32_t>(draw.vertices->size()),
        {MultiplyForwardMatrices(view_projection, draw.transform),
            display.wire_color, extent,
            draw.prim_id, draw.instance_id}});
  }
  return batch;
}

} // namespace merlin::render
