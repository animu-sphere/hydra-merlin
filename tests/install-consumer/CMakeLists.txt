cmake_minimum_required(VERSION 3.24)

project(MerlinInstallConsumer LANGUAGES CXX)

find_package(Merlin 0.1 REQUIRED COMPONENTS RenderExtraction)

add_executable(merlin-install-consumer main.cpp)
target_link_libraries(merlin-install-consumer PRIVATE Merlin::RenderExtraction)
