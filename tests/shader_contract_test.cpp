#include <merlin/core/shader_contract.hpp>

#include <cassert>

int main() {
  using merlin::ShaderCapability;
  using merlin::ShaderPermutation;
  using merlin::ShaderStage;

  constexpr auto conventional = ShaderPermutation{
      "forward", "forward_vertex", ShaderStage::Vertex,
      ShaderCapability::MaterialConstants |
          ShaderCapability::BaseColorTexture,
      1};
  constexpr auto same = conventional;
  constexpr auto fragment = ShaderPermutation{
      "forward", "forward_fragment", ShaderStage::Fragment,
      conventional.capabilities, 1};
  constexpr auto bindless = ShaderPermutation{
      "forward", "forward_vertex", ShaderStage::Vertex,
      conventional.capabilities | ShaderCapability::BindlessResources |
          ShaderCapability::NonUniformResourceIndexing,
      1};

  static_assert(merlin::MakeShaderPermutationKey(conventional) ==
                merlin::MakeShaderPermutationKey(same));
  static_assert(merlin::MakeShaderPermutationKey(conventional) !=
                merlin::MakeShaderPermutationKey(fragment));
  static_assert(merlin::MakeShaderPermutationKey(conventional) !=
                merlin::MakeShaderPermutationKey(bindless));
  assert(merlin::MakeShaderPermutationKey(conventional) ==
         merlin::MakeShaderPermutationKey(same));
}
