cmake_minimum_required(VERSION 3.24)

project(MerlinMetalInstallConsumer LANGUAGES CXX)

find_package(Merlin 0.1 REQUIRED COMPONENTS Metal)

add_executable(merlin-metal-install-consumer main.cpp)
target_link_libraries(merlin-metal-install-consumer PRIVATE Merlin::Metal)
