cmake_minimum_required(VERSION 3.31)
set(CMAKE_CXX_STANDARD 26)
include(FetchContent)

project(shldnbththrd
  DESCRIPTION "Should be simple"
)

# Lib
FetchContent_Declare(
	SDL3
	GIT_REPOSITORY https://github.com/libsdl-org/SDL
	GIT_TAG release-3.4.18
	GIT_SHALLOW TRUE
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(SDL3)
FetchContent_Declare(
	SDL3_image
	GIT_REPOSITORY https://github.com/libsdl-org/SDL_image
	GIT_TAG release-3.4.8
	GIT_SHALLOW TRUE
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(SDL3_image)
set(SDLIMAGE_AVIF OFF CACHE BOOL "Not needed")
set(SDLIMAGE_BMP OFF CACHE BOOL "Not needed")
set(SDLIMAGE_JPG OFF CACHE BOOL "Not needed")
set(SDLIMAGE_WEBP OFF CACHE BOOL "Not needed")
set(SDLIMAGE_PNG_LIBPNG OFF CACHE BOOL "Not needed")
# sdl3_image too old in apt
#find_package(SDL3 REQUIRED CONFIG REQUIRED COMPONENTS SDL3-shared)
#find_package(SDL3_image REQUIRED CONFIG REQUIRED COMPONENTS SDL3_image-shared)
# Fin
add_executable(shldnbththrd
  main.cpp)
target_link_libraries(shldnbththrd PRIVATE SDL3_image::SDL3_image SDL3::SDL3)
