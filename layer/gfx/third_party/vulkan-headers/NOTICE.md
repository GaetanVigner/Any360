# Vulkan-Headers (vendored subset)

`include/vulkan/{vulkan.h, vulkan_core.h, vk_platform.h}` and `include/vk_video/` from the Khronos Vulkan-Headers
(header version 357), as shipped in the ReXGlue SDK source tree (`thirdparty/vulkan-headers`). Unchanged. License:
Apache-2.0 or MIT (`LICENSE.md`, `LICENSES/`). any360::gfx loads the Vulkan loader at run time (`VK_NO_PROTOTYPES`),
so only the headers are needed.
