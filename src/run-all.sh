#!/bin/bash
set -e

for o in out/Debug out/Release; do
  vendor/subgn/ninja -C $o
  VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_standard_validation $o/01glfw
  VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_standard_validation $o/04android
  VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_standard_validation $o/05indexbuffer
  vendor/subgn/ninja -C $o 06threepipelines
  VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_standard_validation $o/06threepipelines
  vendor/subgn/ninja -C $o 07mipmaps
  VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_standard_validation $o/07mipmaps
  vendor/subgn/ninja -C $o 09fullscreen
  VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_standard_validation $o/09fullscreen
done
