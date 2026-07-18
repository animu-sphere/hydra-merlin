cmake_minimum_required(VERSION 3.24)

project(MerlinMaterialXInstallConsumer LANGUAGES CXX)

find_package(Merlin 0.1 REQUIRED COMPONENTS MaterialX)

add_executable(merlin-materialx-install-consumer main.cpp)
target_link_libraries(merlin-materialx-install-consumer
  PRIVATE Merlin::MaterialX)
