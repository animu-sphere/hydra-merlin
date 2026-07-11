#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <merlin/core/change_set.hpp>
#include <merlin/core/types.hpp>

namespace merlin {
class RenderWorld;
}

namespace merlin::extraction {

struct DrawVertex {
  Vec3 position;
};

struct DrawCommand {
  std::uint32_t first_index{};
  std::uint32_t index_count{};
  std::int32_t vertex_offset{};
  Mat4 transform;
  Vec4 base_color{1.0F, 1.0F, 1.0F, 1.0F};
  std::uint64_t instance_handle{};
  std::uint64_t sort_key{};
};

struct ExtractedScene {
  std::uint64_t revision{};
  std::vector<DrawVertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<DrawCommand> draws;
  Mat4 view;
  Mat4 projection;
};

class SceneExtractor {
 public:
  SceneExtractor();
  ~SceneExtractor();

  SceneExtractor(SceneExtractor&&) noexcept;
  SceneExtractor& operator=(SceneExtractor&&) noexcept;
  SceneExtractor(const SceneExtractor&) = delete;
  SceneExtractor& operator=(const SceneExtractor&) = delete;

  void Apply(const RenderWorld& world, const ChangeSet& changes);
  void SetActiveCamera(CameraHandle camera);
  [[nodiscard]] const ExtractedScene& scene() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace merlin::extraction
