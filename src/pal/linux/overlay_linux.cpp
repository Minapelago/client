// Linux Vulkan/SDL dev overlay.
//
// No libvulkan link at build time: VK_NO_PROTOTYPES, all entry points resolved at
// runtime from the loader the game already opened via dlopen(RTLD_NOLOAD).
// Hooks vkCreateSwapchainKHR/vkQueuePresentKHR to capture state and composite the
// ImGui overlay (loadOp=LOAD) before each present. The instance/device/queue-family
// are read from the game's renderer globals instead: the engine inlines its
// vkCreateInstance/vkCreateDevice calls, so hooks on those never fire.
//
// Input: hooks ProcessSDLEvent(SDL_Event&), resolved here. Toggle key (default F1)
// flips g_console_open; while open, input events are queued (mutex) and drained into
// ImGui IO on the render thread before NewFrame. ImGui is not touched on the event thread.

#define VK_NO_PROTOTYPES
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL_events.h>
#include <SDL_keycode.h>
#include <SDL_mouse.h>
#include <dlfcn.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <link.h> // dl_iterate_phdr (libvulkan presence check without touching dlerror)
#include <vulkan/vulkan.h>

#include "mth/core/data/game_symbols.hpp"
#include "pal/overlay_keys.hpp"
#include "pal/overlay_scale.hpp"
#include "pal/pal_log.hpp"
#include "pal/pal_module.hpp"
#include "pal/pal_overlay.hpp"
#include "pal/scoped_hook.hpp"

namespace pal
{

namespace
{

// ---------------------------------------------------------------------------
// Resolved Vulkan entry points.
// ---------------------------------------------------------------------------

PFN_vkGetInstanceProcAddr g_vkGetInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr g_vkGetDeviceProcAddr = nullptr;

// Device-level functions resolved via vkGetDeviceProcAddr once the device is known.
#define MTHAP_VK_DEVICE_FNS(X)                                                                                                                                 \
    X(GetDeviceQueue)                                                                                                                                          \
    X(GetSwapchainImagesKHR)                                                                                                                                   \
    X(CreateImageView)                                                                                                                                         \
    X(DestroyImageView)                                                                                                                                        \
    X(CreateRenderPass)                                                                                                                                        \
    X(DestroyRenderPass)                                                                                                                                       \
    X(CreateFramebuffer)                                                                                                                                       \
    X(DestroyFramebuffer)                                                                                                                                      \
    X(CreateDescriptorPool)                                                                                                                                    \
    X(DestroyDescriptorPool)                                                                                                                                   \
    X(CreateCommandPool)                                                                                                                                       \
    X(DestroyCommandPool)                                                                                                                                      \
    X(AllocateCommandBuffers)                                                                                                                                  \
    X(ResetCommandBuffer)                                                                                                                                      \
    X(BeginCommandBuffer)                                                                                                                                      \
    X(EndCommandBuffer)                                                                                                                                        \
    X(CmdBeginRenderPass)                                                                                                                                      \
    X(CmdEndRenderPass)                                                                                                                                        \
    X(QueueSubmit)                                                                                                                                             \
    X(DeviceWaitIdle)                                                                                                                                          \
    X(CreateSemaphore)                                                                                                                                         \
    X(DestroySemaphore)                                                                                                                                        \
    X(CreateFence)                                                                                                                                             \
    X(DestroyFence)                                                                                                                                            \
    X(WaitForFences)                                                                                                                                           \
    X(ResetFences)

#define X(name) PFN_vk##name g_vk##name = nullptr;
MTHAP_VK_DEVICE_FNS(X)
#undef X

// Trampolines installed by the hook engine.
PFN_vkCreateSwapchainKHR g_orig_create_swapchain = nullptr;
PFN_vkQueuePresentKHR g_orig_present = nullptr;

// ---------------------------------------------------------------------------
// Captured Vulkan state.
// ---------------------------------------------------------------------------

struct VulkanState
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    bool queue_family_known = false;
    VkQueue queue = VK_NULL_HANDLE;

    // Rebuilt on every vkCreateSwapchainKHR.
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::uint32_t min_image_count = 0;
    std::uint32_t image_count = 0;

    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;

    std::vector<VkImage> images;
    std::vector<VkImageView> image_views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkCommandBuffer> command_buffers;
    std::vector<VkSemaphore> render_finished; // one per swapchain image
    std::vector<VkFence> in_flight;           // one per swapchain image; gates cmd-buffer reuse

    // Acquire/release gate: release-stored only after all handles above are written.
    std::atomic<bool> resources_ready{false};
};

VulkanState g_state;

std::atomic<bool> g_imgui_inited{false};
std::atomic<IOverlayUi *> g_ui{nullptr};
bool g_warned_incomplete = false; // rate-limit the "state incomplete" warning

// ---------------------------------------------------------------------------
// SDL input state.
// ---------------------------------------------------------------------------

using PFN_ProcessSDLEvent = void (*)(SDL_Event *);
PFN_ProcessSDLEvent g_orig_process = nullptr;

std::atomic<bool> g_console_open{false};
std::atomic<bool> g_login_open{true};

// Published by the render thread after each frame; read (one frame in arrears) by
// the input hook to decide what to swallow from the game.
std::atomic<bool> g_imgui_want_keyboard{false};
std::atomic<bool> g_imgui_want_mouse{false};

// Events captured on the event thread, drained into ImGui IO on the render thread.
std::mutex g_input_mu;
std::vector<SDL_Event> g_input_queue; // guarded by g_input_mu

SDL_Keycode g_toggle_key = SDLK_F1;
SDL_Keycode g_login_key = SDLK_F2;

// SDL_Keycode to ImGuiKey (header-only, no SDL linkage). Letters, digits, F1..F12 and KP_1..KP_9 are
// contiguous in both enums, which the static_asserts pin.
static_assert(SDLK_z - SDLK_a == ImGuiKey_Z - ImGuiKey_A);
static_assert(SDLK_9 - SDLK_0 == ImGuiKey_9 - ImGuiKey_0);
static_assert(SDLK_F12 - SDLK_F1 == 11 && ImGuiKey_F12 - ImGuiKey_F1 == 11);
static_assert(SDLK_KP_9 - SDLK_KP_1 == 8 && ImGuiKey_Keypad9 - ImGuiKey_Keypad1 == 8);

static ImGuiKey sdl_keycode_to_imgui(SDL_Keycode key)
{
    if (key >= SDLK_a && key <= SDLK_z)
        return static_cast<ImGuiKey>(ImGuiKey_A + (key - SDLK_a));
    if (key >= SDLK_0 && key <= SDLK_9)
        return static_cast<ImGuiKey>(ImGuiKey_0 + (key - SDLK_0));
    if (key >= SDLK_F1 && key <= SDLK_F12)
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (key - SDLK_F1));
    if (key >= SDLK_KP_1 && key <= SDLK_KP_9)
        return static_cast<ImGuiKey>(ImGuiKey_Keypad1 + (key - SDLK_KP_1));

    switch (key)
    {
    case SDLK_TAB:
        return ImGuiKey_Tab;
    case SDLK_LEFT:
        return ImGuiKey_LeftArrow;
    case SDLK_RIGHT:
        return ImGuiKey_RightArrow;
    case SDLK_UP:
        return ImGuiKey_UpArrow;
    case SDLK_DOWN:
        return ImGuiKey_DownArrow;
    case SDLK_PAGEUP:
        return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN:
        return ImGuiKey_PageDown;
    case SDLK_HOME:
        return ImGuiKey_Home;
    case SDLK_END:
        return ImGuiKey_End;
    case SDLK_INSERT:
        return ImGuiKey_Insert;
    case SDLK_DELETE:
        return ImGuiKey_Delete;
    case SDLK_BACKSPACE:
        return ImGuiKey_Backspace;
    case SDLK_SPACE:
        return ImGuiKey_Space;
    case SDLK_RETURN:
        return ImGuiKey_Enter;
    case SDLK_ESCAPE:
        return ImGuiKey_Escape;
    case SDLK_LCTRL:
        return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL:
        return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT:
        return ImGuiKey_LeftShift;
    case SDLK_RSHIFT:
        return ImGuiKey_RightShift;
    case SDLK_LALT:
        return ImGuiKey_LeftAlt;
    case SDLK_RALT:
        return ImGuiKey_RightAlt;
    case SDLK_LGUI:
        return ImGuiKey_LeftSuper;
    case SDLK_RGUI:
        return ImGuiKey_RightSuper;
    case SDLK_MINUS:
        return ImGuiKey_Minus;
    case SDLK_EQUALS:
        return ImGuiKey_Equal;
    case SDLK_LEFTBRACKET:
        return ImGuiKey_LeftBracket;
    case SDLK_RIGHTBRACKET:
        return ImGuiKey_RightBracket;
    case SDLK_BACKSLASH:
        return ImGuiKey_Backslash;
    case SDLK_SEMICOLON:
        return ImGuiKey_Semicolon;
    case SDLK_QUOTE:
        return ImGuiKey_Apostrophe;
    case SDLK_COMMA:
        return ImGuiKey_Comma;
    case SDLK_PERIOD:
        return ImGuiKey_Period;
    case SDLK_SLASH:
        return ImGuiKey_Slash;
    case SDLK_BACKQUOTE:
        return ImGuiKey_GraveAccent;
    case SDLK_KP_0:
        return ImGuiKey_Keypad0;
    case SDLK_KP_PERIOD:
        return ImGuiKey_KeypadDecimal;
    case SDLK_KP_DIVIDE:
        return ImGuiKey_KeypadDivide;
    case SDLK_KP_MULTIPLY:
        return ImGuiKey_KeypadMultiply;
    case SDLK_KP_MINUS:
        return ImGuiKey_KeypadSubtract;
    case SDLK_KP_PLUS:
        return ImGuiKey_KeypadAdd;
    case SDLK_KP_ENTER:
        return ImGuiKey_KeypadEnter;
    case SDLK_KP_EQUALS:
        return ImGuiKey_KeypadEqual;
    default:
        return ImGuiKey_None;
    }
}

// Feed one SDL event into ImGui IO. Returns true for input events ImGui consumed.
static bool feed_imgui(const SDL_Event &e)
{
    if (ImGui::GetCurrentContext() == nullptr)
        return false;

    ImGuiIO &io = ImGui::GetIO();

    switch (e.type)
    {
    case SDL_MOUSEMOTION:
        io.AddMousePosEvent(static_cast<float>(e.motion.x), static_cast<float>(e.motion.y));
        return true;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    {
        int btn = -1;
        if (e.button.button == SDL_BUTTON_LEFT)
            btn = 0;
        else if (e.button.button == SDL_BUTTON_RIGHT)
            btn = 1;
        else if (e.button.button == SDL_BUTTON_MIDDLE)
            btn = 2;
        if (btn >= 0)
            io.AddMouseButtonEvent(btn, e.type == SDL_MOUSEBUTTONDOWN);
        return true;
    }

    case SDL_MOUSEWHEEL:
        io.AddMouseWheelEvent(e.wheel.preciseX, e.wheel.preciseY);
        return true;

    case SDL_TEXTINPUT:
        io.AddInputCharactersUTF8(e.text.text);
        return true;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
    {
        const bool down = (e.type == SDL_KEYDOWN);
        const SDL_Keymod mod = static_cast<SDL_Keymod>(e.key.keysym.mod);
        io.AddKeyEvent(ImGuiMod_Ctrl, (mod & KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (mod & KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (mod & KMOD_ALT) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (mod & KMOD_GUI) != 0);
        ImGuiKey imgui_key = sdl_keycode_to_imgui(e.key.keysym.sym);
        if (imgui_key != ImGuiKey_None)
            io.AddKeyEvent(imgui_key, down);
        return true;
    }

    default:
        return false;
    }
}

// Classifier (no ImGui): mirrors the event types feed_imgui() handles.
// Used on the event thread where ImGui must not be touched.
static bool is_imgui_input_event(const SDL_Event &e)
{
    switch (e.type)
    {
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
    case SDL_TEXTINPUT:
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        return true;
    default:
        return false;
    }
}

// Selects which WantCapture flag gates the swallow decision for this event.
static bool is_mouse_event(const SDL_Event &e)
{
    switch (e.type)
    {
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// ProcessSDLEvent detour.
// ---------------------------------------------------------------------------

extern "C" void repl_process_sdl_event(SDL_Event *e)
{
    if (e != nullptr)
    {
        // Swallow both keydown and keyup for the toggle key so the game never sees it.
        if ((e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) && e->key.keysym.sym == g_toggle_key)
        {
            if (e->type == SDL_KEYDOWN && e->key.repeat == 0)
                g_console_open.store(!g_console_open.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return; // swallow: do not forward to the game
        }

        if ((e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) && e->key.keysym.sym == g_login_key)
        {
            if (e->type == SDL_KEYDOWN && e->key.repeat == 0)
                g_login_open.store(!g_login_open.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return; // swallow: do not forward to the game
        }

        if ((g_console_open.load(std::memory_order_relaxed) || g_login_open.load(std::memory_order_relaxed)) && is_imgui_input_event(*e))
        {
            {
                std::lock_guard<std::mutex> lk(g_input_mu);
                g_input_queue.push_back(*e);
            }
            const bool want = is_mouse_event(*e) ? g_imgui_want_mouse.load(std::memory_order_acquire) : g_imgui_want_keyboard.load(std::memory_order_acquire);
            if (want)
                return; // ImGui owns this input; swallow it from the game
        }
    }

    if (g_orig_process)
        g_orig_process(e);
}

// ---------------------------------------------------------------------------
// Function resolution helpers.
// ---------------------------------------------------------------------------

template <typename Fn> void resolve_device_fn(Fn &slot, VkDevice device, const char *name)
{
    slot = reinterpret_cast<Fn>(g_vkGetDeviceProcAddr(device, name));
    if (slot == nullptr)
        pal::logf(pal::LogLevel::Error, "overlay: failed to resolve device fn %s", name);
}

bool resolve_device_functions(VkDevice device)
{
    if (g_vkGetDeviceProcAddr == nullptr)
    {
        g_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(g_vkGetInstanceProcAddr(g_state.instance, "vkGetDeviceProcAddr"));
        if (g_vkGetDeviceProcAddr == nullptr)
        {
            pal::logf(pal::LogLevel::Error, "overlay: vkGetDeviceProcAddr unresolved");
            return false;
        }
    }

#define X(name) resolve_device_fn(g_vk##name, device, "vk" #name);
    MTHAP_VK_DEVICE_FNS(X)
#undef X

    bool all_resolved = true;
#define X(name) all_resolved = all_resolved && g_vk##name != nullptr;
    MTHAP_VK_DEVICE_FNS(X)
#undef X
    return all_resolved;
}

// Loader thunk for ImGui_ImplVulkan_LoadFunctions.
PFN_vkVoidFunction imgui_loader_thunk(const char *name, void * /*user*/)
{
    if (g_vkGetInstanceProcAddr == nullptr)
        return nullptr;
    return g_vkGetInstanceProcAddr(g_state.instance, name);
}

// ---------------------------------------------------------------------------
// Swapchain resource teardown/build.
// ---------------------------------------------------------------------------

void destroy_swapchain_resources()
{
    if (g_state.device == VK_NULL_HANDLE)
        return;

    if (g_vkDeviceWaitIdle)
        g_vkDeviceWaitIdle(g_state.device);

    if (g_vkDestroySemaphore)
        for (VkSemaphore s : g_state.render_finished)
            if (s != VK_NULL_HANDLE)
                g_vkDestroySemaphore(g_state.device, s, nullptr);
    g_state.render_finished.clear();

    if (g_vkDestroyFence)
        for (VkFence f : g_state.in_flight)
            if (f != VK_NULL_HANDLE)
                g_vkDestroyFence(g_state.device, f, nullptr);
    g_state.in_flight.clear();

    if (g_vkDestroyFramebuffer)
        for (VkFramebuffer fb : g_state.framebuffers)
            if (fb != VK_NULL_HANDLE)
                g_vkDestroyFramebuffer(g_state.device, fb, nullptr);
    g_state.framebuffers.clear();

    if (g_vkDestroyImageView)
        for (VkImageView v : g_state.image_views)
            if (v != VK_NULL_HANDLE)
                g_vkDestroyImageView(g_state.device, v, nullptr);
    g_state.image_views.clear();

    if (g_state.command_pool != VK_NULL_HANDLE && g_vkDestroyCommandPool)
    {
        g_vkDestroyCommandPool(g_state.device, g_state.command_pool, nullptr);
        g_state.command_pool = VK_NULL_HANDLE;
    }
    g_state.command_buffers.clear();

    if (g_state.render_pass != VK_NULL_HANDLE && g_vkDestroyRenderPass)
    {
        g_vkDestroyRenderPass(g_state.device, g_state.render_pass, nullptr);
        g_state.render_pass = VK_NULL_HANDLE;
    }

    g_state.images.clear();
    g_state.resources_ready.store(false, std::memory_order_release);
}

bool build_render_pass()
{
    VkAttachmentDescription color{};
    color.format = g_state.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // composite over the game's frame
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;

    if (g_vkCreateRenderPass(g_state.device, &rp, nullptr, &g_state.render_pass) != VK_SUCCESS)
    {
        pal::logf(pal::LogLevel::Error, "overlay: vkCreateRenderPass failed");
        return false;
    }
    return true;
}

bool build_swapchain_resources()
{
    g_state.resources_ready.store(false, std::memory_order_release);
    destroy_swapchain_resources();

    if (!resolve_device_functions(g_state.device))
    {
        pal::logf(pal::LogLevel::Error, "overlay: device function resolution incomplete; not rendering");
        return false;
    }

    std::uint32_t count = 0;
    if (g_vkGetSwapchainImagesKHR(g_state.device, g_state.swapchain, &count, nullptr) != VK_SUCCESS || count == 0)
    {
        pal::logf(pal::LogLevel::Error, "overlay: vkGetSwapchainImagesKHR (count) failed");
        return false;
    }
    g_state.images.resize(count);
    if (g_vkGetSwapchainImagesKHR(g_state.device, g_state.swapchain, &count, g_state.images.data()) != VK_SUCCESS)
    {
        pal::logf(pal::LogLevel::Error, "overlay: vkGetSwapchainImagesKHR (data) failed");
        return false;
    }
    g_state.image_count = count;

    if (!build_render_pass())
        return false;

    g_state.image_views.resize(count, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        VkImageViewCreateInfo iv{};
        iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image = g_state.images[i];
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = g_state.format;
        iv.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv.subresourceRange.baseMipLevel = 0;
        iv.subresourceRange.levelCount = 1;
        iv.subresourceRange.baseArrayLayer = 0;
        iv.subresourceRange.layerCount = 1;
        if (g_vkCreateImageView(g_state.device, &iv, nullptr, &g_state.image_views[i]) != VK_SUCCESS)
        {
            pal::logf(pal::LogLevel::Error, "overlay: vkCreateImageView[%u] failed", i);
            return false;
        }
    }

    g_state.framebuffers.resize(count, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        VkImageView attach = g_state.image_views[i];
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = g_state.render_pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &attach;
        fb.width = g_state.extent.width;
        fb.height = g_state.extent.height;
        fb.layers = 1;
        if (g_vkCreateFramebuffer(g_state.device, &fb, nullptr, &g_state.framebuffers[i]) != VK_SUCCESS)
        {
            pal::logf(pal::LogLevel::Error, "overlay: vkCreateFramebuffer[%u] failed", i);
            return false;
        }
    }

    if (!g_state.queue_family_known)
        pal::logf(pal::LogLevel::Warn, "overlay: assuming graphics queue family 0 (vkCreateDevice was not intercepted)");

    VkCommandPoolCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = g_state.queue_family;
    if (g_vkCreateCommandPool(g_state.device, &cp, nullptr, &g_state.command_pool) != VK_SUCCESS)
    {
        pal::logf(pal::LogLevel::Error, "overlay: vkCreateCommandPool failed");
        return false;
    }

    g_state.command_buffers.resize(count, VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo cba{};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool = g_state.command_pool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = count;
    if (g_vkAllocateCommandBuffers(g_state.device, &cba, g_state.command_buffers.data()) != VK_SUCCESS)
    {
        pal::logf(pal::LogLevel::Error, "overlay: vkAllocateCommandBuffers failed");
        return false;
    }

    g_state.render_finished.resize(count, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (g_vkCreateSemaphore(g_state.device, &si, nullptr, &g_state.render_finished[i]) != VK_SUCCESS)
        {
            pal::logf(pal::LogLevel::Error, "overlay: vkCreateSemaphore[%u] failed", i);
            return false;
        }
    }

    // Created signaled so the first wait returns immediately.
    g_state.in_flight.resize(count, VK_NULL_HANDLE);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (g_vkCreateFence(g_state.device, &fi, nullptr, &g_state.in_flight[i]) != VK_SUCCESS)
        {
            pal::logf(pal::LogLevel::Error, "overlay: vkCreateFence[%u] failed", i);
            return false;
        }
    }

    g_state.resources_ready.store(true, std::memory_order_release);
    pal::logf(pal::LogLevel::Info, "overlay: swapchain resources built (%ux%u, %u images, fmt=%d)", g_state.extent.width, g_state.extent.height,
              g_state.image_count, static_cast<int>(g_state.format));
    return true;
}

// ---------------------------------------------------------------------------
// ImGui-on-Vulkan lazy init.
// ---------------------------------------------------------------------------

bool init_imgui()
{
    if (g_imgui_inited.load(std::memory_order_acquire))
        return true;

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 64;

    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 64;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &pool_size;
    if (g_vkCreateDescriptorPool(g_state.device, &dp, nullptr, &g_state.descriptor_pool) != VK_SUCCESS)
    {
        pal::logf(pal::LogLevel::Error, "overlay: vkCreateDescriptorPool failed");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    pal::scale_overlay_for_height(g_state.extent.height);

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // never write imgui.ini into the game dir
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.DisplaySize = ImVec2(static_cast<float>(g_state.extent.width), static_cast<float>(g_state.extent.height));

    if (!ImGui_ImplVulkan_LoadFunctions(&imgui_loader_thunk, nullptr))
    {
        pal::logf(pal::LogLevel::Error, "overlay: ImGui_ImplVulkan_LoadFunctions failed");
        g_vkDestroyDescriptorPool(g_state.device, g_state.descriptor_pool, nullptr);
        g_state.descriptor_pool = VK_NULL_HANDLE;
        ImGui::DestroyContext();
        return false;
    }

    ImGui_ImplVulkan_InitInfo init{};
    init.Instance = g_state.instance;
    init.PhysicalDevice = g_state.physical_device;
    init.Device = g_state.device;
    init.QueueFamily = g_state.queue_family;
    init.Queue = g_state.queue;
    init.DescriptorPool = g_state.descriptor_pool;
    init.RenderPass = g_state.render_pass;
    init.MinImageCount = g_state.min_image_count < 2 ? 2 : g_state.min_image_count;
    init.ImageCount = g_state.image_count;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.PipelineCache = VK_NULL_HANDLE;
    init.Subpass = 0;
    init.UseDynamicRendering = false;
    init.Allocator = nullptr;
    init.CheckVkResultFn = nullptr;

    if (!ImGui_ImplVulkan_Init(&init))
    {
        pal::logf(pal::LogLevel::Error, "overlay: ImGui_ImplVulkan_Init failed");
        g_vkDestroyDescriptorPool(g_state.device, g_state.descriptor_pool, nullptr);
        g_state.descriptor_pool = VK_NULL_HANDLE;
        ImGui::DestroyContext();
        return false;
    }

    g_imgui_inited.store(true, std::memory_order_release);
    pal::logf(pal::LogLevel::Info, "overlay: ImGui Vulkan backend initialized");
    return true;
}

void shutdown_imgui()
{
    if (!g_imgui_inited.load(std::memory_order_acquire))
        return;
    if (g_state.device != VK_NULL_HANDLE && g_vkDeviceWaitIdle)
        g_vkDeviceWaitIdle(g_state.device);
    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext();
    if (g_state.descriptor_pool != VK_NULL_HANDLE && g_vkDestroyDescriptorPool)
    {
        g_vkDestroyDescriptorPool(g_state.device, g_state.descriptor_pool, nullptr);
        g_state.descriptor_pool = VK_NULL_HANDLE;
    }
    g_imgui_inited.store(false, std::memory_order_release);
}

bool state_complete_for_render()
{
    return g_state.instance != VK_NULL_HANDLE && g_state.physical_device != VK_NULL_HANDLE && g_state.device != VK_NULL_HANDLE &&
           g_state.queue != VK_NULL_HANDLE && g_state.resources_ready.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Hook replacements.
// ---------------------------------------------------------------------------

// The game stores its Vulkan handles as named globals in the main module. Under native
// modding we load after ycEngine::Run has already created the instance/device, so the
// vkCreateInstance/vkCreateDevice hooks never fire - read the globals instead.
// g_vkInst/g_vkPhysDev/g_vkDev are handles; g_vkGfxQueueIdx is the graphics queue family.
constexpr const char *kSymVkInstance = "g_vkInst";
constexpr const char *kSymVkPhysDev = "g_vkPhysDev";
constexpr const char *kSymVkDevice = "g_vkDev";
constexpr const char *kSymVkGfxFamily = "g_vkGfxQueueIdx";

// The game's active render window (ycRenderer::s_mainWindow -> ycRenderWindow*) holds the swapchain and
// its format/extent. Under native modding the game creates the swapchain before our vkCreateSwapchainKHR
// hook is installed, so that hook never fires and resources are never built; read these from the window
// and build lazily on first present instead. Offsets verified on build 8de7a6b5 (re-verify on game update).
constexpr const char *kSymMainWindow = "_ZN10ycRenderer12s_mainWindowE"; // ycRenderWindow** (single deref)
constexpr std::ptrdiff_t kRWExtentWidthOff = 0x18;
constexpr std::ptrdiff_t kRWExtentHeightOff = 0x1c;
constexpr std::ptrdiff_t kRWSwapchainOff = 0xb0; // VkSwapchainKHR
constexpr std::ptrdiff_t kRWFormatOff = 0xb8;    // VkFormat (u32)

bool g_globals_warned = false;

// Populate instance/physical-device/device/queue-family from the game's globals.
// Idempotent and cheap: symbol addresses are resolved once and cached; each call only
// fills fields still unset (the game may set the globals slightly after we first look).
void populate_state_from_globals()
{
    if (g_state.instance != VK_NULL_HANDLE && g_state.physical_device != VK_NULL_HANDLE && g_state.device != VK_NULL_HANDLE && g_state.queue_family_known)
        return;

    static const std::uintptr_t inst_addr = pal::resolve_game_symbol(kSymVkInstance);
    static const std::uintptr_t phys_addr = pal::resolve_game_symbol(kSymVkPhysDev);
    static const std::uintptr_t dev_addr = pal::resolve_game_symbol(kSymVkDevice);
    static const std::uintptr_t fam_addr = pal::resolve_game_symbol(kSymVkGfxFamily);

    if (inst_addr == 0 || phys_addr == 0 || dev_addr == 0 || fam_addr == 0)
    {
        if (!g_globals_warned)
        {
            pal::logf(pal::LogLevel::Error, "overlay: could not resolve Vulkan handle globals (inst=%#zx phys=%#zx dev=%#zx fam=%#zx); overlay disabled",
                      inst_addr, phys_addr, dev_addr, fam_addr);
            g_globals_warned = true;
        }
        return;
    }

    if (g_state.instance == VK_NULL_HANDLE)
        g_state.instance = *reinterpret_cast<VkInstance *>(inst_addr);
    if (g_state.physical_device == VK_NULL_HANDLE)
        g_state.physical_device = *reinterpret_cast<VkPhysicalDevice *>(phys_addr);
    if (g_state.device == VK_NULL_HANDLE)
        g_state.device = *reinterpret_cast<VkDevice *>(dev_addr);
    if (!g_state.queue_family_known)
    {
        const std::uint32_t fam = *reinterpret_cast<std::uint32_t *>(fam_addr);
        if (fam < 64) // sanity: a real family index, not garbage from an uninitialized global
        {
            g_state.queue_family = fam;
            g_state.queue_family_known = true;
        }
    }

    static bool logged = false;
    if (!logged && g_state.instance != VK_NULL_HANDLE && g_state.physical_device != VK_NULL_HANDLE && g_state.device != VK_NULL_HANDLE &&
        g_state.queue_family_known)
    {
        pal::logf(pal::LogLevel::Info, "overlay: Vulkan handles read from game globals (queue family %u)", g_state.queue_family);
        logged = true;
    }
}

// Recover the game's swapchain from ycRenderer::s_mainWindow and build our render resources, for the case
// where we loaded after the game already created the swapchain (so repl_vkCreateSwapchainKHR never fired).
// No-op once resources are ready; the create hook takes over for later recreates (resize).
void try_adopt_game_swapchain()
{
    if (g_state.resources_ready.load(std::memory_order_acquire) || g_state.device == VK_NULL_HANDLE)
        return;
    static const std::uintptr_t mw_addr = pal::resolve_game_symbol(kSymMainWindow);
    if (mw_addr == 0)
        return;
    void *win = *reinterpret_cast<void **>(mw_addr);
    if (win == nullptr)
        return;
    const VkSwapchainKHR sc = *reinterpret_cast<VkSwapchainKHR *>(static_cast<char *>(win) + kRWSwapchainOff);
    const std::uint32_t fmt = *reinterpret_cast<std::uint32_t *>(static_cast<char *>(win) + kRWFormatOff);
    const std::uint32_t w = *reinterpret_cast<std::uint32_t *>(static_cast<char *>(win) + kRWExtentWidthOff);
    const std::uint32_t h = *reinterpret_cast<std::uint32_t *>(static_cast<char *>(win) + kRWExtentHeightOff);
    if (sc == VK_NULL_HANDLE || w == 0 || h == 0)
        return; // swapchain not created yet

    g_state.swapchain = sc;
    g_state.format = static_cast<VkFormat>(fmt);
    g_state.extent = VkExtent2D{w, h};
    pal::logf(pal::LogLevel::Info, "overlay: adopting game swapchain from ycRenderWindow (fmt=%u %ux%u)", fmt, w, h);
    if (g_imgui_inited.load(std::memory_order_acquire))
        shutdown_imgui(); // rebuild ImGui against the recovered render pass
    if (!build_swapchain_resources())
        pal::logf(pal::LogLevel::Warn, "overlay: adopted-swapchain resource build failed; overlay inactive");
    g_warned_incomplete = false;
}

VKAPI_ATTR VkResult VKAPI_CALL repl_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                                         VkSwapchainKHR *pSwapchain)
{
    VkResult r = g_orig_create_swapchain ? g_orig_create_swapchain(device, pCreateInfo, pAllocator, pSwapchain) : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS || pSwapchain == nullptr || pCreateInfo == nullptr)
        return r;

    populate_state_from_globals(); // instance/phys/device/family come from game globals, not vkCreateDevice

    if (g_state.device == VK_NULL_HANDLE)
    {
        g_state.device = device;
        pal::logf(pal::LogLevel::Warn, "overlay: VkDevice recovered from vkCreateSwapchainKHR (missed vkCreateDevice)");
    }

    g_state.swapchain = *pSwapchain;
    g_state.format = pCreateInfo->imageFormat;
    g_state.extent = pCreateInfo->imageExtent;
    g_state.min_image_count = pCreateInfo->minImageCount;

    if (g_imgui_inited.load(std::memory_order_acquire))
        shutdown_imgui();

    if (!build_swapchain_resources())
        pal::logf(pal::LogLevel::Warn, "overlay: swapchain resource build failed; overlay inactive for this swapchain");

    g_warned_incomplete = false;
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL repl_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    if (g_state.queue == VK_NULL_HANDLE)
        g_state.queue = queue;

    populate_state_from_globals();
    try_adopt_game_swapchain(); // native modload: recover the pre-created swapchain the create hook missed

    if (!state_complete_for_render() || pPresentInfo == nullptr)
    {
        if (!g_warned_incomplete)
        {
            pal::logf(pal::LogLevel::Warn, "overlay: state incomplete (instance=%p device=%p phys=%p queue=%p ready=%d); forwarding present untouched",
                      reinterpret_cast<void *>(g_state.instance), reinterpret_cast<void *>(g_state.device), reinterpret_cast<void *>(g_state.physical_device),
                      reinterpret_cast<void *>(g_state.queue), static_cast<int>(g_state.resources_ready.load(std::memory_order_acquire)));
            g_warned_incomplete = true;
        }
        return g_orig_present(queue, pPresentInfo);
    }

    const bool console_open = g_console_open.load(std::memory_order_acquire);
    const bool login_open = g_login_open.load(std::memory_order_acquire);
    const bool any_open = console_open || login_open;
    if (!any_open)
    {
        g_imgui_want_keyboard.store(false, std::memory_order_release);
        g_imgui_want_mouse.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lk(g_input_mu);
        g_input_queue.clear();
    }

    if (!init_imgui())
        return g_orig_present(queue, pPresentInfo);

    std::uint32_t our_idx = UINT32_MAX;
    std::uint32_t image_index = 0;
    for (std::uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
    {
        if (pPresentInfo->pSwapchains[i] == g_state.swapchain)
        {
            our_idx = i;
            image_index = pPresentInfo->pImageIndices[i];
            break;
        }
    }
    if (our_idx == UINT32_MAX || image_index >= g_state.image_count)
        return g_orig_present(queue, pPresentInfo);

    // Drain SDL input (captured on the event thread) into ImGui IO on the render thread.
    if (any_open)
    {
        std::lock_guard<std::mutex> lk(g_input_mu);
        for (const SDL_Event &e : g_input_queue)
            feed_imgui(e);
        g_input_queue.clear();
    }

    ImGui_ImplVulkan_NewFrame();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_state.extent.width), static_cast<float>(g_state.extent.height));
    io.DeltaTime = 1.0f / 60.0f;
    io.MouseDrawCursor = any_open;
    ImGui::NewFrame();

    if (IOverlayUi *ui = g_ui.load(std::memory_order_acquire))
        ui->draw(pal::OverlayVisibility{console_open, login_open});

    ImGui::Render();

    g_imgui_want_keyboard.store(any_open && io.WantCaptureKeyboard, std::memory_order_release);
    g_imgui_want_mouse.store(any_open && io.WantCaptureMouse, std::memory_order_release);

    VkFence in_flight = g_state.in_flight[image_index];
    g_vkWaitForFences(g_state.device, 1, &in_flight, VK_TRUE, UINT64_MAX);
    g_vkResetFences(g_state.device, 1, &in_flight);

    VkCommandBuffer cmd = g_state.command_buffers[image_index];
    g_vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_vkBeginCommandBuffer(cmd, &begin);

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = g_state.render_pass;
    rp.framebuffer = g_state.framebuffers[image_index];
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = g_state.extent;
    rp.clearValueCount = 0; // loadOp=LOAD, nothing to clear
    rp.pClearValues = nullptr;
    g_vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    g_vkCmdEndRenderPass(cmd);
    g_vkEndCommandBuffer(cmd);

    VkSemaphore overlay_sem = g_state.render_finished[image_index];

    std::vector<VkPipelineStageFlags> wait_stages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
    submit.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
    submit.pWaitDstStageMask = wait_stages.empty() ? nullptr : wait_stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &overlay_sem;

    if (g_vkQueueSubmit(queue, 1, &submit, in_flight) != VK_SUCCESS)
    {
        // Submit failed: restore in_flight to signaled so the next wait for this image
        // does not block on an unsignaled fence.
        VkSubmitInfo signal_only{};
        signal_only.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        g_vkQueueSubmit(queue, 1, &signal_only, in_flight);
        pal::logf(pal::LogLevel::Warn, "overlay: vkQueueSubmit failed; forwarding original present");
        return g_orig_present(queue, pPresentInfo);
    }

    VkPresentInfoKHR modified = *pPresentInfo;
    modified.waitSemaphoreCount = 1;
    modified.pWaitSemaphores = &overlay_sem;
    return g_orig_present(queue, &modified);
}

// ---------------------------------------------------------------------------
// Deferred hook installation.
//
// In the Steam pressure-vessel container libvulkan loads after our constructor, and the
// vkCreate* hooks must be in place before the game's create calls (a game tick is too
// late - it runs after renderer init). A background thread polls for libvulkan and
// installs the instant it appears (the libvulkan-load -> vkCreateInstance gap is ~150ms).
// We deliberately do NOT hook dlopen: interposing it makes glibc resolve $ORIGIN/RUNPATH
// against our .so instead of the caller, breaking the game's relative dlopen of Steam.
// ---------------------------------------------------------------------------

std::mutex g_install_mtx;
std::atomic<bool> g_vk_hooks_installed{false};
void *g_libvulkan_handle = nullptr;
std::uintptr_t g_process_sdl_event_addr = 0;

std::thread g_vk_watch_thread;
std::atomic<bool> g_vk_watch_stop{false};

[[clang::no_destroy]] ScopedHook g_create_swapchain_hook;
[[clang::no_destroy]] ScopedHook g_present_hook;
[[clang::no_destroy]] ScopedHook g_process_sdl_event_hook;

// True if libvulkan is already mapped. Uses dl_iterate_phdr, which (unlike a failing
// dlopen probe) never sets dlerror - critical because we run inside the game's own
// dlopen and it checks dlerror() afterward (e.g. for the Steam library).
bool libvulkan_loaded()
{
    bool found = false;
    dl_iterate_phdr(
        [](struct dl_phdr_info *info, std::size_t, void *data) -> int
        {
            if (info->dlpi_name != nullptr && std::strstr(info->dlpi_name, "libvulkan") != nullptr)
            {
                *static_cast<bool *>(data) = true;
                return 1; // stop iterating
            }
            return 0;
        },
        &found);
    return found;
}

// Install the Vulkan + SDL hooks once libvulkan is loaded. Called from the constructor
// and the watch thread; the try_lock + installed flag make concurrent calls a no-op.
bool try_install_vulkan_hooks()
{
    if (g_vk_hooks_installed.load(std::memory_order_acquire))
        return true;

    std::unique_lock<std::mutex> lk(g_install_mtx, std::try_to_lock);
    if (!lk.owns_lock() || g_vk_hooks_installed.load(std::memory_order_relaxed))
        return false;

    if (!libvulkan_loaded())
        return false; // not loaded yet

    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD); // present -> succeeds
    if (vk == nullptr)
        return false;

    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk, "vkGetInstanceProcAddr"));
    if (gipa == nullptr)
    {
        pal::logf(pal::LogLevel::Error, "overlay: dlsym(vkGetInstanceProcAddr) failed - overlay inert");
        return false;
    }
    g_vkGetInstanceProcAddr = gipa;
    g_libvulkan_handle = vk;

    g_vk_hooks_installed.store(true, std::memory_order_release);

    struct Spec
    {
        const char *name;
        void *replacement;
        void **trampoline;
        ScopedHook *hook;
    };
    const Spec specs[] = {
        {"vkCreateSwapchainKHR", reinterpret_cast<void *>(&repl_vkCreateSwapchainKHR), reinterpret_cast<void **>(&g_orig_create_swapchain),
         &g_create_swapchain_hook},
        {"vkQueuePresentKHR", reinterpret_cast<void *>(&repl_vkQueuePresentKHR), reinterpret_cast<void **>(&g_orig_present), &g_present_hook},
    };
    for (const auto &s : specs)
    {
        void *target = dlsym(vk, s.name);
        if (target == nullptr)
        {
            pal::logf(pal::LogLevel::Error, "overlay: dlsym(%s) failed; not hooked", s.name);
            continue;
        }
        *s.hook = ScopedHook(reinterpret_cast<std::uintptr_t>(target), s.replacement, s.trampoline);
        if (!s.hook->installed())
            pal::logf(pal::LogLevel::Error, "overlay: failed to hook %s at %p", s.name, target);
        else
            pal::logf(pal::LogLevel::Info, "overlay: hooked %s at %p", s.name, target);
    }

    if (g_process_sdl_event_addr != 0)
    {
        void *evt = reinterpret_cast<void *>(g_process_sdl_event_addr);
        g_process_sdl_event_hook = ScopedHook(g_process_sdl_event_addr, &repl_process_sdl_event, &g_orig_process);
        if (!g_process_sdl_event_hook.installed())
            pal::logf(pal::LogLevel::Error, "overlay: failed to hook ProcessSDLEvent at %p; input unavailable", evt);
        else
            pal::logf(pal::LogLevel::Info, "overlay: hooked ProcessSDLEvent at %p", evt);
    }

    pal::logf(pal::LogLevel::Info, "overlay: Vulkan hooks installed (libvulkan ready)");
    return true;
}

// libvulkan loads after we initialize (in the pressure-vessel container the game maps it
// during renderer init, and the loader is inlined into ycEngine::Run - which is already
// executing by the time we run, so its entry can't be hooked). So we poll for libvulkan
// and install the moment it maps; the load -> vkCreateInstance gap is ~150ms, plenty of
// margin. We deliberately do NOT hook dlopen: interposing it makes glibc resolve
// $ORIGIN/RUNPATH against our .so instead of the caller, breaking the game's relative
// dlopen of the Steam library.
void watch_for_libvulkan()
{
    using namespace std::chrono_literals;
    // Renderer init is within a couple seconds of launch; poll generously for slow loads.
    for (int i = 0; i < 12000 && !g_vk_watch_stop.load(std::memory_order_relaxed); ++i) // ~60s @ 5ms
    {
        if (try_install_vulkan_hooks())
            return;
        std::this_thread::sleep_for(5ms);
    }
    if (!g_vk_hooks_installed.load(std::memory_order_acquire))
        pal::logf(pal::LogLevel::Warn, "overlay: libvulkan did not appear; overlay inert");
}

} // namespace

void overlay_start(IOverlayUi *ui)
{
    g_process_sdl_event_addr = resolve_game_symbol(mth::sym::process_sdl_event);
    pal::logf(pal::LogLevel::Info, "overlay: VulkanOverlay constructing (event_proc=0x%llx)", static_cast<unsigned long long>(g_process_sdl_event_addr));
    if (g_process_sdl_event_addr == 0)
        pal::logf(pal::LogLevel::Warn, "overlay: process_sdl_event_addr == 0; input unavailable for this build (render-only mode)");
    g_toggle_key = pal::parse_console_key<SDL_Keycode>(std::getenv("MTHAP_CONSOLE_KEY"), SDLK_F1, SDLK_BACKQUOTE);
    pal::logf(pal::LogLevel::Info, "overlay: console toggle key = 0x%x (SDLK)", static_cast<int>(g_toggle_key));
    g_login_key = pal::parse_login_key<SDL_Keycode>(std::getenv("MTHAP_LOGIN_KEY"), g_toggle_key, SDLK_F1, SDLK_BACKQUOTE);
    pal::logf(pal::LogLevel::Info, "overlay: login toggle key = 0x%x (SDLK)", static_cast<int>(g_login_key));

    // Fast path: libvulkan already loaded (native run, or loaded before us). Otherwise watch for
    // libvulkan on a background thread and install the moment it maps (before the engine's inlined
    // vkCreateInstance).
    if (!try_install_vulkan_hooks())
    {
        pal::logf(pal::LogLevel::Info, "overlay: libvulkan not loaded yet; watching for it");
        g_vk_watch_thread = std::thread(watch_for_libvulkan);
    }
    g_ui.store(ui, std::memory_order_release);
}

void overlay_stop()
{
    g_vk_watch_stop.store(true, std::memory_order_release);
    if (g_vk_watch_thread.joinable())
        g_vk_watch_thread.join();

    for (ScopedHook *hook : {&g_create_swapchain_hook, &g_present_hook, &g_process_sdl_event_hook})
        hook->reset();
    g_vk_hooks_installed.store(false, std::memory_order_release);

    shutdown_imgui();
    destroy_swapchain_resources();

    g_ui.store(nullptr, std::memory_order_release);

    if (g_libvulkan_handle != nullptr)
    {
        dlclose(g_libvulkan_handle);
        g_libvulkan_handle = nullptr;
    }
    pal::logf(pal::LogLevel::Info, "overlay: VulkanOverlay destroyed");
}

} // namespace pal
