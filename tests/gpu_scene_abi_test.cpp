#include <merlin/render/gpu_scene_abi.hpp>

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct FieldExpectation {
  std::string_view name;
  std::size_t offset;
  std::size_t size;
};

std::string Read(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot read GPU Scene reflection: " +
                             path.string());
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

std::string CompactJson(std::string_view text) {
  std::string compact;
  compact.reserve(text.size());
  bool in_string{};
  bool escaped{};
  for (const char character : text) {
    if (in_string) {
      compact.push_back(character);
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
    } else if (character == '"') {
      in_string = true;
      compact.push_back(character);
    } else if (!std::isspace(static_cast<unsigned char>(character))) {
      compact.push_back(character);
    }
  }
  return compact;
}

std::string_view StructFields(std::string_view json, std::string_view name) {
  const std::string marker =
      "\"kind\":\"struct\",\"name\":\"" + std::string(name) +
      "\",\"fields\":[";
  const auto marker_position = json.find(marker);
  if (marker_position == std::string_view::npos) {
    throw std::runtime_error("GPU Scene reflection has no " +
                             std::string(name));
  }
  const auto start = marker_position + marker.size();
  std::size_t depth{1};
  bool in_string{};
  bool escaped{};
  for (auto position = start; position < json.size(); ++position) {
    const char character = json[position];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
      continue;
    }
    if (character == '"') {
      in_string = true;
    } else if (character == '[') {
      ++depth;
    } else if (character == ']' && --depth == 0) {
      return json.substr(start, position - start);
    }
  }
  throw std::runtime_error("GPU Scene reflection fields are truncated for " +
                           std::string(name));
}

void RequireField(std::string_view fields, const FieldExpectation& expected,
                  std::string_view record) {
  const std::string field =
      "\"name\":\"" + std::string(expected.name) + "\"";
  const auto position = fields.find(field);
  if (position == std::string_view::npos) {
    throw std::runtime_error(std::string(record) + " has no field " +
                             std::string(expected.name));
  }
  const std::string binding =
      "\"binding\":{\"kind\":\"uniform\",\"offset\":" +
      std::to_string(expected.offset) + ",\"size\":" +
      std::to_string(expected.size);
  const auto next_field = fields.find("\"name\":", position + field.size());
  const auto reflected_field = fields.substr(position, next_field - position);
  if (reflected_field.find(binding) == std::string_view::npos) {
    throw std::runtime_error(std::string(record) + "." +
                             std::string(expected.name) +
                             " has the wrong reflected offset or size");
  }
}

template <std::size_t Size>
void RequireLayout(std::string_view json, std::string_view record,
                   const std::array<FieldExpectation, Size>& fields) {
  const auto reflected = StructFields(json, record);
  std::size_t reflected_count{};
  auto position = reflected.find("\"name\":");
  while (position != std::string_view::npos) {
    ++reflected_count;
    position = reflected.find("\"name\":", position + 7);
  }
  if (reflected_count != fields.size()) {
    throw std::runtime_error(std::string(record) +
                             " has the wrong reflected field count");
  }
  for (const auto& field : fields) {
    RequireField(reflected, field, record);
  }
}

void RequireAbi(std::string_view json) {
  RequireLayout(json, "GpuGeometry",
                std::array{FieldExpectation{"vertex_offset", 0, 4},
                           FieldExpectation{"vertex_count", 4, 4},
                           FieldExpectation{"index_offset", 8, 4},
                           FieldExpectation{"index_count", 12, 4},
                           FieldExpectation{"index_type", 16, 4},
                           FieldExpectation{"attribute_mask", 20, 4},
                           FieldExpectation{"meshlet_offset", 24, 4},
                           FieldExpectation{"meshlet_count", 28, 4},
                           FieldExpectation{"bounds_min", 32, 16},
                           FieldExpectation{"bounds_max", 48, 16}});
  RequireLayout(json, "GpuInstance",
                std::array{FieldExpectation{"transform", 0, 64},
                           FieldExpectation{"normal_matrix_columns", 64, 48},
                           FieldExpectation{"object_id", 112, 4},
                           FieldExpectation{"instance_id", 116, 4},
                           FieldExpectation{"visibility_mask", 120, 4},
                           FieldExpectation{"flags", 124, 4}});
  RequireLayout(json, "GpuMaterial",
                std::array{FieldExpectation{"base_color", 0, 16},
                           FieldExpectation{"surface_factors", 16, 16},
                           FieldExpectation{"material_class_flags", 32, 4},
                           FieldExpectation{"base_color_texture_index", 36, 4},
                           FieldExpectation{"base_color_sampler_index", 40, 4},
                           FieldExpectation{"base_color_texcoord_set", 44, 4}});
  RequireLayout(json, "GpuDraw",
                std::array{FieldExpectation{"geometry_index", 0, 4},
                           FieldExpectation{"material_index", 4, 4},
                           FieldExpectation{"instance_index", 8, 4},
                           FieldExpectation{"primitive_base", 12, 4},
                           FieldExpectation{"primitive_count", 16, 4},
                           FieldExpectation{"flags", 20, 4},
                           FieldExpectation{"reserved0", 24, 4},
                           FieldExpectation{"reserved1", 28, 4}});
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      throw std::runtime_error(
          "usage: gpu-scene-abi-test spirv-reflection metal-reflection");
    }
    RequireAbi(CompactJson(Read(argv[1])));
    RequireAbi(CompactJson(Read(argv[2])));

    using namespace merlin::render;
    static_assert(kGpuSceneAbiVersion == 1);
    if (GpuGeometry{}.meshlet_offset != kInvalidGpuSceneTableIndex ||
        GpuMaterial{}.base_color_texture_index !=
            kInvalidGpuSceneTableIndex ||
        GpuMaterial{}.base_color_sampler_index !=
            kInvalidGpuSceneTableIndex ||
        GpuDraw{}.geometry_index != kInvalidGpuSceneTableIndex ||
        GpuDraw{}.material_index != kInvalidGpuSceneTableIndex ||
        GpuDraw{}.instance_index != kInvalidGpuSceneTableIndex) {
      throw std::runtime_error("GPU Scene absent-reference sentinel mismatch");
    }
  } catch (const std::exception& error) {
    std::cerr << "GPU Scene ABI contract failure: " << error.what() << '\n';
    return 1;
  }
}
