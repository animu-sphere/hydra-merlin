# Using the CMake package

hdMerlin installs a versioned CMake package named `Merlin`. Consumers use only
the components they need, keeping Vulkan and OpenUSD out of Core-only dependency
graphs.

## Install a producer build

```powershell
cmake -S . -B build-core -DMERLIN_ENABLE_VULKAN=OFF
cmake --build build-core --config Release
cmake --install build-core --config Release --prefix C:/merlin
```

Then point `CMAKE_PREFIX_PATH` at the install prefix, or set `Merlin_DIR` to the
directory containing `MerlinConfig.cmake`.

## Consume a component

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyRenderer LANGUAGES CXX)

find_package(Merlin 0.1 REQUIRED COMPONENTS RenderExtraction)

add_executable(my-renderer main.cpp)
target_link_libraries(my-renderer PRIVATE Merlin::RenderExtraction)
```

Configure the consumer with:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/merlin
cmake --build build --config Release
```

## Components and targets

| Component | Imported target | Installed when | Public dependency |
| --- | --- | --- | --- |
| `RenderWorld` | `Merlin::RenderWorld` | Always | C++20 only |
| `RenderExtraction` | `Merlin::RenderExtraction` | Always | `Merlin::RenderWorld` |
| `Vulkan` | `Merlin::Vulkan` | Vulkan enabled | Vulkan 1.4 and `Merlin::RenderExtraction` |

Requesting `Vulkan` makes the installed package discover Vulkan 1.4. A consumer
requesting only Core components does not need Vulkan, even when the producer was
built with Vulkan enabled.

Unknown or unavailable required components make `find_package(Merlin ...)`
fail. The package exposes `Merlin_VERSION`, `Merlin_WITH_VULKAN`, and
`Merlin_VULKAN_MIN_VERSION` for inspection after discovery.

## Runtime-only products

In the current pre-v0.1.0 package, `merlin-headless` and the `hdMerlin` plugin
are installed runtime products, not imported CMake targets. Their executable,
plugin resources, and shaders are installed together so they can run without
the source tree. Whether future packages export `Merlin::Headless` or
`Merlin::Hydra2` remains an explicit v0.1.0 roadmap decision.

OpenUSD is therefore not a transitive dependency of the exported Core or Vulkan
targets.
