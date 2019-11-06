<table cellspacing="0" cellpadding="0"><tbody>
<tr valign="top"><td width="60%" colspan="2">

# Volcano Sample 20: Compute

Several samples show different uses of compute pipelines.

</td></tr><tr valign="top"><td width="60%">

[View source code](./)

1. [Goals](#goals)
1. [Texture Compression](#texture-compression)
   1. [What problems are a good fit for GPU compute?](#what-problems-are-a-good-fit-for-gpu-compute)
1. [The End](#the-end)

</td><td width="40%">

![Screenshot](screenshot.png)

These samples are not automatically built.

To build a sample type:<br/>
`vendor/subgn/ninja -C out/Debug 20texture`

(Substitute other sample names show below)

Run it by typing:<br/>`out/Debug/20texture`

Vulkan Validation layers are enabled by setting the `VK_INSTANCE_LAYERS`
environment variable.</td></tr>
</tbody></table>

## Goals

This sample digs into questions you might face when just starting out
with GPU compute:

1. How to break down a GPU compute problem
1. How to debug a GPU compute task

## Texture Compression

Because texture compression is very sensitive to the invisible errors
introduced by normal JPEG compression, be careful not to evaluate the
compressed texture when feeding it a JPEG file - the output may be
suddenly larger, smaller, or noisier.

But texture compression is quite useful - it can save memory, which can
save memory bandwidth (especially on mobile devices), which can even lead
to better rendering performance! Compressed texture formats, unfortunately,
lack a good choice for universal compatibility. Also, texture compression
tools are lacking. Many of the freely available tools use the CPU to do
texture compression.

Textures are completely parallelizable, or "embarassingly parallel," to use
standard terminology. They are a natural fit for a GPU compute task.

### What problems are a good fit for GPU compute?

Anything you can imagine.

However, the following limitations often mean GPU compute isn't a good
choice (until these problems are solved):

1. Lack of cross platform support. OpenCL implementations are buggy. CUDA
   is locked to nVidia devices. Maybe Vulkan then?

1. Lack of debugging tools. CUDA has the best developer environment.
   Vulkan is perhaps the hardest to debug. A good strategy for a Vulkan
   compute task is to implement it on the host first (first get it to
   run correctly), then implement it on the GPU (to run fast). Bugs show up
   quickly by comparing the output of the two.

1. Lack of task scheduling. A heavy GPU compute task often makes the OS
   UI start to lag. If you've ever seen the screen freeze, go black, then
   come back but your app has crashed, that was a GPU task that took too
   long (but it could be a render pipeline, not compute).

### How does Vulkan model GPU execution flow?

There are some programs that simply can't be run in parallel. The GPU could
still run the program, but its lack of task scheduling would mean a lot of
work to change the program to have regular "task switch" points.
Scheduling a compute task on the GPU that runs for more than about 2 seconds
of real time often trips a "device reset" or "device lost" error from the
OS. The compute task must therefore be broken into smaller pieces.

It's actually hard to give an example of a program that truly can't be
rewritten to take advantage of multiple threads. Some encryption can't be
parallelized because each step depends on the complete set of steps before
it.

If your problem can use multiple threads, it still might not be ready for
the GPU. [Amdahl's law](https://en.wikipedia.org/wiki/Amdahl%27s_law) asks,
if you actually had an infinite number of CPUs, how fast could your problem
run? If you don't need more cores than what a desktop can provide, and it
doesn't feed any other code already on the GPU, then the CPU likely can
deliver good enough performance.

Vulkan uses
[OpenGL's Compute Shader](https://www.khronos.org/opengl/wiki/Compute_Shader)
with only minor changes. [This youtube video](https://youtu.be/V-yqiLyU27U)
gives a brief intro to how execution works inside the GPU. You pick the
dimensions of an imaginary grid. The GPU then runs the same code for each
cell of the grid, completely in parallel. (Note: video is in C# and HLSL, but
is simple enough language doesn't matter. Here's a
[vulkan-specific intro](https://stackoverflow.com/questions/54750009/compute-shader-and-workgroup).)

The GPU's execution model forces you to choose right away what can be solved
without knowing what the other parts of the GPU are doing, because asking the
code to wait and talk to another cell usually takes quite a long time.

### Takeaways

Here are the two key takeaways from all this:

1. A compute shader can't run for too long, so the code must be broken into
   fairly small problems for the GPU. Otherwise the system UI starts to lag.

1. A compute shader is happiest if each "cell" doesn't need to communicate with
   the cells around it.

This sample shows a texture compression tool, using these takeaways in the
design. After splitting the image into texture blocks (16 x 16 texels), one
shader "cell" can compress its data without talking to the other cells.
The compression algorithm can be broken into hundreds of small tasks. The
app directs the work, feeding a constant stream of small tasks to the GPU.

## The other kind of compute

Texture compression in this sample aims to max out the GPU's time spent
doing productive work. This is a shader designed for "throughput."

The other kind of compute task, such as a physics engine, would still
aim for as much productive GPU work as possible, but there is a strict
time limit. For 60 fps that is 16 milliseconds. This is a shader
designed for "low latency."

## Buffers

Compute shaders come with very little built in. You define everything about
where it reads, where it writes, and how much.

## Keeping the GPU fed

Maximum throughput is all about keeping the GPU fed.

1. The CPU prepares as much as possible in advance, while the GPU is busy.
   Then as soon as the GPU signals a fence, the CPU submits more work to
   the GPU.

1. Auto scaling of work sizes.

## Corner cases

Because the same code runs everywhere, think about the corner cases.
In this sample, an image might not have dimensions that are an integer
multiple of 16.

The compute shader can either be given dummy data, and the CPU just knows
that the extra work done on the "out of bounds" data is later ignored, or
the shader can be given another variable as input that signals how much
work to do.

## The End

### Homework


Copyright (c) 2017-2018 the Volcano Authors. All rights reserved.

Hidden Treasure scene used in this example
[Copyright Laurynas Jurgila](http://www.blendswap.com/user/PigArt).
