/*
** Copyright (c) 2018 Valve Corporation
** Copyright (c) 2018 LunarG, Inc.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#ifndef GFXRECON_APPLICATION_WAYLAND_WINDOW_H
#define GFXRECON_APPLICATION_WAYLAND_WINDOW_H

#include "application/wayland_application.h"
#include "decode/window.h"
#include "util/defines.h"

#include <wayland-client.h>

GFXRECON_BEGIN_NAMESPACE(gfxrecon)
GFXRECON_BEGIN_NAMESPACE(application)

class WaylandWindow : public decode::Window
{
  public:
    enum HandleId : uint32_t
    {
        kDisplay = 0,
        kSurface = 1
    };

  public:
    WaylandWindow(WaylandApplication* application);

    virtual ~WaylandWindow() override;

    struct wl_surface* GetSurface() const { return surface_; }

    struct wl_shell_surface* GetShellSurface() const { return shell_surface_; }

    virtual bool Create(const std::string& title,
                        const int32_t      x,
                        const int32_t      y,
                        const uint32_t     width,
                        const uint32_t     height) override;

    virtual bool Destroy() override;

    virtual void SetTitle(const std::string& title) override;

    virtual void SetPosition(const int32_t x, const int32_t y) override;

    virtual void SetSize(const uint32_t width, const uint32_t height) override;

    virtual void SetVisibility(bool show) override;

    virtual void SetForeground() override;

    virtual bool GetNativeHandle(uint32_t id, void** handle) override;

    virtual VkResult CreateSurface(const encode::InstanceTable* table,
                                   VkInstance                   instance,
                                   VkFlags                      flags,
                                   VkSurfaceKHR*                pSurface) override;

  private:
    static void HandlePing(void* data, wl_shell_surface* shell_surface, uint32_t serial);

    static void
    HandleConfigure(void* data, wl_shell_surface* shell_surface, uint32_t edges, int32_t width, int32_t height);

    static void HandlePopupDone(void* data, wl_shell_surface* shell_surface);

  private:
    static struct wl_shell_surface_listener shell_surface_listener_;
    WaylandApplication*                     wayland_application_;
    struct wl_surface*                      surface_;
    struct wl_shell_surface*                shell_surface_;
};

class WaylandWindowFactory : public decode::WindowFactory
{
  public:
    WaylandWindowFactory(WaylandApplication* application);

    virtual const char* GetSurfaceExtensionName() const override { return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME; }

    virtual decode::Window*
    Create(const int32_t x, const int32_t y, const uint32_t width, const uint32_t height) override;

    void Destroy(decode::Window* window) override;

    virtual VkBool32 GetPhysicalDevicePresentationSupport(const encode::InstanceTable* table,
                                                          VkPhysicalDevice             physical_device,
                                                          uint32_t                     queue_family_index) override;

  private:
    WaylandApplication* wayland_application_;
};

GFXRECON_END_NAMESPACE(util)
GFXRECON_END_NAMESPACE(gfxrecon)

#endif // GFXRECON_APPLICATION_WAYLAND_WINDOW_H