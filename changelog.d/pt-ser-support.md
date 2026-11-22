### Fixed
- Path tracing on devices without shader execution reordering (VK_EXT_ray_tracing_invocation_reorder), such as Mesa's CPU Vulkan driver: the reordering pipeline failed to build and the frame fell back to an untraced, unshadowed image. Reordering is now used only where the device supports it (`RenderingDevice.SUPPORTS_RAYTRACING_INVOCATION_REORDER`).
