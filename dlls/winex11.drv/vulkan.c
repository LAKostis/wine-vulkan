/* X11DRV Vulkan implementation
 *
 * Copyright 2017 Roderick Colenbrander
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"

#include "wine/debug.h"
#include "wine/heap.h"
#include "wine/library.h"
#include "x11drv.h"

/* We only want host compatible structures and don't need alignment. */
#define WINE_VK_ALIGN(x)

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

#ifdef SONAME_LIBVULKAN

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#endif

typedef VkFlags VkXlibSurfaceCreateFlagsKHR;
#define VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR 1000004000

/* All Vulkan structures use this structure for the first elements. */
struct wine_vk_structure_header
{
    VkStructureType sType;
    const void *pNext;
};

struct wine_vk_surface
{
    Window window;
    VkSurfaceKHR surface; /* native surface */
};
/* Macro to help conversion from VkSurfaceKHR (uint64_t) to a surface pointer. */
#define SURFACE_FROM_HANDLE(surface) ((struct wine_vk_surface *)(uintptr_t)surface)

typedef struct VkXlibSurfaceCreateInfoKHR {
    VkStructureType                sType;
    const void*                    pNext;
    VkXlibSurfaceCreateFlagsKHR    flags;
    Display*                       dpy;
    Window                         window;
} VkXlibSurfaceCreateInfoKHR;

static VkResult (*pvkCreateInstance)(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
static VkResult (*pvkCreateXlibSurfaceKHR)(VkInstance, const VkXlibSurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *);
static void (*pvkDestroyInstance)(VkInstance, const VkAllocationCallbacks *);
static void (*pvkDestroySurfaceKHR)(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *);
static void * (*pvkGetDeviceProcAddr)(VkDevice, const char *);
static void * (*pvkGetInstanceProcAddr)(VkInstance, const char *);
static VkBool32 (*pvkGetPhysicalDeviceXlibPresentationSupportKHR)(VkPhysicalDevice, uint32_t, Display *, VisualID);

struct VkExtensionProperties winex11_vk_instance_extensions[] = {
    { "VK_KHR_surface", 1 },
    { "VK_KHR_win32_surface", 1},
};

static BOOL wine_vk_init(void)
{
    static BOOL init_done = FALSE;
    static void *vulkan_handle;

    if (init_done) return (vulkan_handle != NULL);
    init_done = TRUE;

    if (!(vulkan_handle = wine_dlopen(SONAME_LIBVULKAN, RTLD_NOW, NULL, 0))) return FALSE;

#define LOAD_FUNCPTR(f) if((p##f = wine_dlsym(vulkan_handle, #f, NULL, 0)) == NULL) return FALSE;
LOAD_FUNCPTR(vkCreateInstance)
LOAD_FUNCPTR(vkCreateXlibSurfaceKHR)
LOAD_FUNCPTR(vkDestroyInstance)
LOAD_FUNCPTR(vkDestroySurfaceKHR)
LOAD_FUNCPTR(vkGetDeviceProcAddr)
LOAD_FUNCPTR(vkGetInstanceProcAddr)
LOAD_FUNCPTR(vkGetPhysicalDeviceXlibPresentationSupportKHR)
#undef LOAD_FUNCPTR

    return TRUE;
}

/* Helper function for converting between win32 and X11 compatible VkInstanceCreateInfo.
 * Caller is responsible for allocation and cleanup of 'dst'.
 */
static VkResult wine_vk_instance_convert_create_info(const VkInstanceCreateInfo *src, VkInstanceCreateInfo *dst)
{
    int i;
    const char **enabled_extensions = NULL;

    dst->sType = src->sType;
    dst->flags = src->flags;
    dst->pApplicationInfo = src->pApplicationInfo;

    /* Application + loader can pass in a chain of extensions through pNext e.g. VK_EXT_debug_report
     * and layers (not sure why loader doesn't filter out loaders to ICD). We need to see how to handle
     * these as we can't just blindly pass structures through as some like VK_EXT_debug_report have
     * callbacks. Mesa ANV / Radv are ignoring pNext at the moment, unclear what binary blobs do.
     * Since in our case we are going through the Linux vulkan loader, the loader itself will add
     * some duplicate layers, so for now it is probably best to ignore extra extensions.
     */
    if (src->pNext)
    {
        const struct wine_vk_structure_header *header;

        for (header = src->pNext; header; header = header->pNext)
        {
            FIXME("Application requested a linked structure of type %d\n", header->sType);
        }
    }
    /* For now don't support anything. */
    dst->pNext = NULL;

    /* ICDs don't support any layers (at least at time of writing). The loader seems to not
     * filter out layer information when it reaches us. To avoid confusion by the native loader
     * we should filter.
     */
    dst->enabledLayerCount = 0;
    dst->ppEnabledLayerNames = NULL;

    if (src->enabledExtensionCount > 0)
    {
        enabled_extensions = heap_alloc(src->enabledExtensionCount * sizeof(*src->ppEnabledExtensionNames));
        if (!enabled_extensions)
        {
            ERR("Failed to allocate memory for enabled extensions\n");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        for (i = 0; i < src->enabledExtensionCount; i++)
        {
            /* Substitute extension with X11 else copy. Long-term when we support more
             * extenions we should store these translations in a list.
             */
            if (!strcmp(src->ppEnabledExtensionNames[i], "VK_KHR_win32_surface"))
            {
                enabled_extensions[i] = "VK_KHR_xlib_surface";
            }
            else
            {
                enabled_extensions[i] = src->ppEnabledExtensionNames[i];
            }
        }
        dst->ppEnabledExtensionNames = (const char**)enabled_extensions;
    }
    dst->enabledExtensionCount = src->enabledExtensionCount;

    return VK_SUCCESS;
}

static VkResult X11DRV_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
        uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *index)
{
    FIXME("stub: %p, 0x%s, 0x%s, 0x%s, 0x%s, %p\n", device,
            wine_dbgstr_longlong(swapchain), wine_dbgstr_longlong(timeout),
            wine_dbgstr_longlong(semaphore), wine_dbgstr_longlong(fence), index);

    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult X11DRV_vkCreateInstance(const VkInstanceCreateInfo *create_info,
        const VkAllocationCallbacks *allocator, VkInstance *instance)
{
    VkInstanceCreateInfo create_info_host;
    VkResult res;
    TRACE("create_info %p, allocator %p, instance %p\n", create_info, allocator, instance);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    res = wine_vk_instance_convert_create_info(create_info, &create_info_host);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to convert instance create info, res=%d\n", res);
        return res;
    }

    res = pvkCreateInstance(&create_info_host, NULL /* allocator */, instance);

    if (create_info_host.ppEnabledExtensionNames)
        heap_free((void*)create_info_host.ppEnabledExtensionNames);

    return res;
}

static VkResult X11DRV_vkCreateSwapchainKHR(VkDevice device,
        const VkSwapchainCreateInfoKHR *create_info,
        const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain)
{
    FIXME("stub: %p %p %p %p\n", device, create_info, allocator, swapchain);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult X11DRV_vkCreateWin32SurfaceKHR(VkInstance instance,
        const VkWin32SurfaceCreateInfoKHR *create_info,
        const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface)
{
    VkResult res;
    VkXlibSurfaceCreateInfoKHR create_info_host;
    struct wine_vk_surface *x11_surface;
    Window win;

    TRACE("%p %p %p %p\n", instance, create_info, allocator, surface);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    /* Don't deal with child window rendering just yet. */
    if (GetAncestor(create_info->hwnd, GA_PARENT) != GetDesktopWindow())
    {
        FIXME("Application requires child window rendering, which is not implemented yet!\n");
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }

    win = create_client_window(create_info->hwnd, &default_visual);
    if (!win) return VK_ERROR_OUT_OF_HOST_MEMORY;

    x11_surface = heap_alloc(sizeof(*x11_surface));
    if (!x11_surface) return VK_ERROR_OUT_OF_HOST_MEMORY;
    x11_surface->window = win;

    create_info_host.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    create_info_host.pNext = NULL;
    create_info_host.flags = 0; /* reserved */
    create_info_host.dpy = gdi_display;
    create_info_host.window = x11_surface->window;

    res = pvkCreateXlibSurfaceKHR(instance, &create_info_host, NULL /* allocator */, &x11_surface->surface);
    if (res != VK_SUCCESS)
    {
        heap_free(surface);
        return res;
    }

    *surface = (uintptr_t)x11_surface;

    TRACE("Created surface=0x%s\n", wine_dbgstr_longlong(*surface));
    return VK_SUCCESS;
}

static void X11DRV_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
    TRACE("%p %p\n", instance, allocator);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    pvkDestroyInstance(instance, NULL /* allocator */);
}

static void X11DRV_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
        const VkAllocationCallbacks *allocator)
{
    struct wine_vk_surface *x11_surface = SURFACE_FROM_HANDLE(surface);

    TRACE("%p 0x%s %p\n", instance, wine_dbgstr_longlong(surface), allocator);

    if (allocator)
        FIXME("Support for allocation callbacks not implemented yet\n");

    pvkDestroySurfaceKHR(instance, x11_surface->surface, NULL /* allocator */);
    heap_free(x11_surface);
}

static void X11DRV_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
         const VkAllocationCallbacks *allocator)
{
    FIXME("stub: %p, 0x%s %p\n", device, wine_dbgstr_longlong(swapchain), allocator);
}

static VkResult X11DRV_vkEnumerateInstanceExtensionProperties(const char *layer_name,
        uint32_t *count, VkExtensionProperties* properties)
{
    VkResult res;
    int i, num_copies;

    TRACE("layer_name %p, count %p, properties %p\n", debugstr_a(layer_name), count, properties);

    /* This shouldn't get called with layer_name set, the ICD loader prevents it. */
    if (layer_name)
    {
        ERR("Layer enumeration not supported from ICD.\n");
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    if (!properties)
    {
        /* When properties is NULL, we need to return the number of extensions
         * supported. For now report 0 until we add some e.g.
         * VK_KHR_win32_surface. Long-term this needs to be an intersection
         * between what the native library supports and what thunks we have.
         */
        *count = ARRAY_SIZE(winex11_vk_instance_extensions);
        return VK_SUCCESS;
    }

    if (*count < ARRAY_SIZE(winex11_vk_instance_extensions))
    {
        /* Incomplete is a type of success used to signal the application
         * that not all devices got copied.
         */
        num_copies = *count;
        res = VK_INCOMPLETE;
    }
    else
    {
        num_copies = ARRAY_SIZE(winex11_vk_instance_extensions);
        res = VK_SUCCESS;
    }

    for (i = 0; i < num_copies; i++)
    {
        memcpy(&properties[i], &winex11_vk_instance_extensions[i], sizeof(winex11_vk_instance_extensions[i]));
    }

    TRACE("Result %d, extensions copied %d\n", res, num_copies);
    return res;
}

static void * X11DRV_vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    TRACE("%p, %s\n", device, debugstr_a(name));
    return pvkGetDeviceProcAddr(device, name);
}

static void * X11DRV_vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    TRACE("%p, %s\n", instance, debugstr_a(name));
    return pvkGetInstanceProcAddr(instance, name);
}

static VkResult X11DRV_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice phys_dev,
        VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *capabilities)
{
    FIXME("stub: %p, 0x%s, %p\n", phys_dev, wine_dbgstr_longlong(surface), capabilities);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult X11DRV_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice phys_dev,
        VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats)
{
    FIXME("stub: %p, 0x%s, %p, %p\n", phys_dev, wine_dbgstr_longlong(surface), count, formats);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult X11DRV_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice phys_dev,
        VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes)
{
    FIXME("stub: %p, 0x%s, %p, %p\n", phys_dev, wine_dbgstr_longlong(surface), count, modes);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult X11DRV_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice phys_dev,
        uint32_t index, VkSurfaceKHR surface, VkBool32 *supported)
{
    FIXME("stub: %p, %u, 0x%s, %p\n", phys_dev, index, wine_dbgstr_longlong(surface), supported);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkBool32 X11DRV_vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice phys_dev,
        uint32_t index)
{
    TRACE("%p %u\n", phys_dev, index);

    return pvkGetPhysicalDeviceXlibPresentationSupportKHR(phys_dev, index, gdi_display,
            default_visual.visual->visualid);
}

static VkResult X11DRV_vkGetSwapchainImagesKHR(VkDevice device,
        VkSwapchainKHR swapchain, uint32_t *count, VkImage *images)
{
    FIXME("stub: %p, 0x%s %p %p\n", device, wine_dbgstr_longlong(swapchain), count, images);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VkResult X11DRV_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *present_info)
{
    FIXME("stub: %p, %p\n", queue, present_info);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}


static struct vulkan_funcs vulkan_funcs =
{
    X11DRV_vkAcquireNextImageKHR,
    X11DRV_vkCreateInstance,
    X11DRV_vkCreateSwapchainKHR,
    X11DRV_vkCreateWin32SurfaceKHR,
    X11DRV_vkDestroyInstance,
    X11DRV_vkDestroySurfaceKHR,
    X11DRV_vkDestroySwapchainKHR,
    X11DRV_vkEnumerateInstanceExtensionProperties,
    X11DRV_vkGetDeviceProcAddr,
    X11DRV_vkGetInstanceProcAddr,
    X11DRV_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
    X11DRV_vkGetPhysicalDeviceSurfaceFormatsKHR,
    X11DRV_vkGetPhysicalDeviceSurfacePresentModesKHR,
    X11DRV_vkGetPhysicalDeviceSurfaceSupportKHR,
    X11DRV_vkGetPhysicalDeviceWin32PresentationSupportKHR,
    X11DRV_vkGetSwapchainImagesKHR,
    X11DRV_vkQueuePresentKHR
};

const struct vulkan_funcs *get_vulkan_driver(UINT version)
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR("version mismatch, vulkan wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION);
        return NULL;
    }

    if (wine_vk_init())
        return &vulkan_funcs;

    return NULL;
}

#else /* No vulkan */

const struct vulkan_funcs *get_vulkan_driver(UINT version)
{
    return NULL;
}

#endif /* SONAME_LIBVULKAN */
