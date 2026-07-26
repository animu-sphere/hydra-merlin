#include <merlin/materialx/compiler.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("could not read MaterialX document: " +
                             path.string());
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, std::string_view contents) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("could not create generated material module: " +
                             path.string());
  }
  stream.write(contents.data(),
               static_cast<std::streamsize>(contents.size()));
  if (!stream) {
    throw std::runtime_error("could not write generated material module: " +
                             path.string());
  }
}

void Generate(const std::filesystem::path& document_path,
              const std::filesystem::path& data_root,
              std::string renderable_path,
              const std::filesystem::path& output_path) {
  merlin::materialx::CompileOptions options;
  options.renderable_path = std::move(renderable_path);
  options.library_search_paths.push_back(data_root);
  options.source_document = document_path.filename().string();
  const auto result = merlin::materialx::CompileMaterialFunction(
      ReadFile(document_path), options);
  if (!result) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << document_path.string() << ": " << diagnostic.message
                << '\n';
    }
    throw std::runtime_error("MaterialX module generation failed: " +
                             document_path.string());
  }
  WriteFile(output_path, result.module->source);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "usage: merlin-materialx-artifact-generator "
                 "DATA_ROOT PROTOTYPE_DOCUMENT PROTOTYPE_OUTPUT "
                 "STANDARD_DOCUMENT STANDARD_OUTPUT\n";
    return 1;
  }
  try {
    Generate(argv[2], argv[1], "NG_prototype/out", argv[3]);
    Generate(argv[4], argv[1], "NG_standard_surface/surface", argv[5]);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "merlin-materialx-artifact-generator: " << error.what()
              << '\n';
    return 1;
  }
}
