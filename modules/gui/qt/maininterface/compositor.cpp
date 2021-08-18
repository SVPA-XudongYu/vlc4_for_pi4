/*****************************************************************************
 * Copyright (C) 2020 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "compositor.hpp"
#include "compositor_dummy.hpp"

#ifdef _WIN32
#ifdef HAVE_DCOMP_H
#  include "compositor_dcomp.hpp"
#endif
#  include "compositor_win7.hpp"
#endif

namespace vlc {

template<typename T>
static Compositor* instanciateCompositor(qt_intf_t *p_intf) {
    return new T(p_intf);
}

template<typename T>
static bool preInit(qt_intf_t *p_intf) {
    return T::preInit(p_intf);
}

struct {
    const char* name;
    Compositor* (*instanciate)(qt_intf_t *p_intf);
    bool (*preInit)(qt_intf_t *p_intf);
} static compositorList[] = {
#ifdef _WIN32
#ifdef HAVE_DCOMP_H
    {"dcomp", &instanciateCompositor<CompositorDirectComposition>, &preInit<CompositorDirectComposition> },
#endif
    {"win7", &instanciateCompositor<CompositorWin7>, &preInit<CompositorWin7> },
#endif
    {"dummy", &instanciateCompositor<CompositorDummy>, &preInit<CompositorDummy> }
};

CompositorFactory::CompositorFactory(qt_intf_t *p_intf, const char* compositor)
    : m_intf(p_intf)
    , m_compositorName(compositor)
{
}

bool CompositorFactory::preInit()
{
    for (; m_compositorIndex < ARRAY_SIZE(compositorList); m_compositorIndex++)
    {
        if (m_compositorName == "auto" || m_compositorName == compositorList[m_compositorIndex].name)
        {
            if (compositorList[m_compositorIndex].preInit(m_intf))
                return true;
        }
    }
    return false;
}

Compositor* CompositorFactory::createCompositor()
{
    for (; m_compositorIndex < ARRAY_SIZE(compositorList); m_compositorIndex++)
    {
        if (m_compositorName == "auto" || m_compositorName == compositorList[m_compositorIndex].name)
        {
            Compositor* compositor = compositorList[m_compositorIndex].instanciate(m_intf);
            if (compositor->init())
                return compositor;
        }
    }
    msg_Err(m_intf, "no suitable compositor found");
    return nullptr;
}

void Compositor::onWindowDestruction(vout_window_t *p_wnd)
{
    if (m_destroyCb)
        m_destroyCb(p_wnd);
}

}
