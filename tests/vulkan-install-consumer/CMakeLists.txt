cmake_minimum_required(VERSION 3.24)

project(MerlinVulkanInstallConsumer LANGUAGES CXX)

find_package(Merlin 0.1 REQUIRED COMPONENTS Vulkan)

add_executable(merlin-vulkan-install-consumer main.cpp)
target_link_libraries(merlin-vulkan-install-consumer PRIVATE Merlin::Vulkan)
