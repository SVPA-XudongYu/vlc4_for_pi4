﻿/*****************************************************************************
 * Copyright (C) 2021 the VideoLAN team
 *
 * Authors: Prince Gupta <guptaprince8832@gmail.com>
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

#include "compositor_dcomp_acrylicsurface.hpp"

#include <QWindow>
#include <QScreen>
#include <QLibrary>
#include <versionhelpers.h>

#include "compositor_dcomp.hpp"

namespace
{

bool isTransparencyEnabled()
{
    static const char *TRANSPARENCY_SETTING_PATH = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    static const char *TRANSPARENCY_SETTING_KEY = "EnableTransparency";

    QSettings settings(QLatin1String {TRANSPARENCY_SETTING_PATH}, QSettings::NativeFormat);
    return settings.value(TRANSPARENCY_SETTING_KEY).toBool();
}

template <typename F>
F loadFunction(QLibrary &library, const char *symbol)
{
    vlc_assert(library.isLoaded());

    auto f = library.resolve(symbol);
    if (!f)
    {
        const auto err = GetLastError();
        throw std::runtime_error(QString("failed to load %1, code %2").arg(QString(symbol), QString::number(err)).toStdString());
    }

    return reinterpret_cast<F>(f);
}

bool isWinPreIron()
{
    typedef NTSTATUS(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    auto GetVersionInfo = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(ntdll, "RtlGetVersion"));

    if (GetVersionInfo)
    {
        RTL_OSVERSIONINFOW versionInfo = { };
        versionInfo.dwOSVersionInfoSize = sizeof(versionInfo);
        if (!GetVersionInfo(&versionInfo))
            return versionInfo.dwMajorVersion <= 10
                    && versionInfo.dwBuildNumber < 20000;
    }

    return false;
}

}

namespace vlc
{

CompositorDCompositionAcrylicSurface::CompositorDCompositionAcrylicSurface(qt_intf_t *intf_t, ID3D11Device *device, QObject *parent)
    : QObject(parent)
    , m_intf {intf_t}
{
    if (!init(device))
    {
        m_intf = nullptr;
        return;
    }

    if (auto w = window())
        setActive(m_transparencyEnabled && w->isActive());

    qApp->installNativeEventFilter(this);
}

CompositorDCompositionAcrylicSurface::~CompositorDCompositionAcrylicSurface()
{
    setActive(false);

    if (m_dummyWindow)
        DestroyWindow(m_dummyWindow);
}

bool CompositorDCompositionAcrylicSurface::nativeEventFilter(const QByteArray &eventType, void *message, long *result)
{
    MSG* msg = static_cast<MSG*>( message );

    if (!m_intf || msg->hwnd != hwnd())
        return false;

    switch (msg->message)
    {
    case WM_WINDOWPOSCHANGED:
    {
        if (!m_active)
            break;

        sync();
        commitChanges();

        requestReset(); // incase z-order changed
        break;
    }
    case WM_ACTIVATE:
    {
        if (!m_transparencyEnabled)
            break;

        const int activeType = LOWORD(msg->wParam);
        if ((activeType == WA_ACTIVE) || (activeType == WA_CLICKACTIVE))
            setActive(true);
        else if (activeType == WA_INACTIVE)
            setActive(false);
        break;
    }
    case WM_SETTINGCHANGE:
    {
        if (!lstrcmpW(LPCWSTR(msg->lParam), L"ImmersiveColorSet"))
        {
            const auto transparencyEnabled = isTransparencyEnabled();
            if (m_transparencyEnabled == transparencyEnabled)
                break;

            m_transparencyEnabled = transparencyEnabled;
            if (const auto w = window())
                setActive(m_transparencyEnabled && w->isActive());
        }
        break;
    }
    }

    return false;
}


bool CompositorDCompositionAcrylicSurface::init(ID3D11Device *device)
{
    if (!loadFunctions())
        return false;

    if (!createDevice(device))
        return false;

    if (!createDesktopVisual())
        return false;

    if (!createBackHostVisual())
        return false;

    m_transparencyEnabled = isTransparencyEnabled();

    m_leftMostScreenX = 0;
    m_topMostScreenY = 0;
    for (const auto screen : qGuiApp->screens())
    {
        const auto geometry = screen->geometry();
        m_leftMostScreenX = std::min<int>(geometry.left(), m_leftMostScreenX);
        m_topMostScreenY = std::min<int>(geometry.top(), m_topMostScreenY);
    }

    return true;
}

bool CompositorDCompositionAcrylicSurface::loadFunctions()
try
{
    QLibrary dwmapi("dwmapi.dll");
    if (!dwmapi.load())
        throw std::runtime_error("failed to dwmapi.dll, reason: " + dwmapi.errorString().toStdString());

    lDwmpCreateSharedThumbnailVisual = loadFunction<DwmpCreateSharedThumbnailVisual>(dwmapi, MAKEINTRESOURCEA(147));
    lDwmpCreateSharedMultiWindowVisual = loadFunction<DwmpCreateSharedMultiWindowVisual>(dwmapi, MAKEINTRESOURCEA(163));

    if (isWinPreIron())
        lDwmpUpdateSharedVirtualDesktopVisual = loadFunction<DwmpUpdateSharedVirtualDesktopVisual>(dwmapi, MAKEINTRESOURCEA(164)); //PRE-IRON
    else
        lDwmpUpdateSharedMultiWindowVisual = loadFunction<DwmpUpdateSharedMultiWindowVisual>(dwmapi, MAKEINTRESOURCEA(164)); //20xxx+


    QLibrary user32("user32.dll");
    if (!user32.load())
        throw std::runtime_error("failed to user32.dll, reason: " + user32.errorString().toStdString());

    lSetWindowCompositionAttribute = loadFunction<SetWindowCompositionAttribute>(user32, "SetWindowCompositionAttribute");
    lGetWindowCompositionAttribute = loadFunction<GetWindowCompositionAttribute>(user32, "GetWindowCompositionAttribute");

    return true;
}
catch (std::exception &err)
{
    msg_Err(m_intf, err.what());
    return false;
}

bool CompositorDCompositionAcrylicSurface::createDevice(ID3D11Device *device)
try
{
    QLibrary dcompDll("DCOMP.dll");
    if (!dcompDll.load())
        throw DXError("failed to load DCOMP.dll",  static_cast<HRESULT>(GetLastError()));

    DCompositionCreateDeviceFun myDCompositionCreateDevice3 =
            reinterpret_cast<DCompositionCreateDeviceFun>(dcompDll.resolve("DCompositionCreateDevice3"));
    if (!myDCompositionCreateDevice3)
        throw DXError("failed to load DCompositionCreateDevice3 function",  static_cast<HRESULT>(GetLastError()));

    using namespace Microsoft::WRL;

    ComPtr<IDXGIDevice> dxgiDevice;
    HR(device->QueryInterface(dxgiDevice.GetAddressOf()), "query dxgi device");

    ComPtr<IDCompositionDevice> dcompDevice1;
    HR(myDCompositionCreateDevice3(
                dxgiDevice.Get(),
                __uuidof(IDCompositionDevice),
                (void**)dcompDevice1.GetAddressOf()), "create composition device");

    HR(dcompDevice1->QueryInterface(m_dcompDevice.GetAddressOf()), "dcompdevice not an IDCompositionDevice3");

    HR(m_dcompDevice->CreateVisual(m_rootVisual.GetAddressOf()), "create root visual");

    HR(m_dcompDevice->CreateRectangleClip(m_rootClip.GetAddressOf()), "create root clip");

    HR(m_dcompDevice->CreateTranslateTransform(m_translateTransform.GetAddressOf()), "create translate transform");

    HR(m_dcompDevice->CreateSaturationEffect(m_saturationEffect.GetAddressOf()), "create saturation effect");

    HR(m_dcompDevice->CreateGaussianBlurEffect(m_gaussianBlur.GetAddressOf()), "create gaussian effect");

    m_saturationEffect->SetSaturation(2);

    m_gaussianBlur->SetBorderMode(D2D1_BORDER_MODE_HARD);
    m_gaussianBlur->SetStandardDeviation(20);
    m_gaussianBlur->SetInput(0, m_saturationEffect.Get(), 0);
    m_rootVisual->SetEffect(m_gaussianBlur.Get());

    return true;
}
catch (const DXError &err)
{
    msg_Err(m_intf, "failed to initialise compositor acrylic surface: '%s' code: 0x%lX", err.what(), err.code());
    return false;
}


bool CompositorDCompositionAcrylicSurface::createDesktopVisual()
try
{
    vlc_assert(!m_desktopVisual);
    auto desktopWindow = GetShellWindow();
    if (!desktopWindow)
        throw DXError("failed to get desktop window",  static_cast<HRESULT>(GetLastError()));

    const int desktopWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int desktopHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    DWM_THUMBNAIL_PROPERTIES thumbnail;
    thumbnail.dwFlags = DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_ENABLE3D;
    thumbnail.opacity = 255;
    thumbnail.fVisible = TRUE;
    thumbnail.fSourceClientAreaOnly = FALSE;
    thumbnail.rcDestination = RECT{ 0, 0, desktopWidth, desktopHeight };
    thumbnail.rcSource = RECT{ 0, 0, desktopWidth, desktopHeight };

    HTHUMBNAIL desktopThumbnail;
    HR(lDwmpCreateSharedThumbnailVisual(hwnd(), desktopWindow, 2, &thumbnail, m_dcompDevice.Get(), (void**)m_desktopVisual.GetAddressOf(), &desktopThumbnail), "create desktop visual");
    HR(m_rootVisual->AddVisual(m_desktopVisual.Get(), FALSE, nullptr), "Add desktop visual");

    return true;
}
catch (const DXError &err)
{
    msg_Err(m_intf, "failed to create desktop visual: '%s' code: 0x%lX", err.what(), err.code());
    return false;
}

bool CompositorDCompositionAcrylicSurface::createBackHostVisual()
try
{
    vlc_assert(!m_dummyWindow);
    // lDwmpCreateSharedMultiWindowVisual requires a window with disabled live (thumbnail) preview
    // use a hidden dummy window to avoid disabling live preview of main window
    m_dummyWindow = ::CreateWindowExA(WS_EX_TOOLWINDOW, "STATIC", "dummy", WS_VISIBLE, 0, 0, 0, 0, NULL, NULL, NULL, NULL);
    if (!m_dummyWindow)
        throw DXError("failed to create dummy window",  static_cast<HRESULT>(GetLastError()));

    int attr = DWM_CLOAKED_APP;
    DwmSetWindowAttribute(m_dummyWindow, DWMWA_CLOAK, &attr, sizeof attr);

    BOOL enable = TRUE;
    WINDOWCOMPOSITIONATTRIBDATA CompositionAttribute{};
    CompositionAttribute.Attrib = WCA_EXCLUDED_FROM_LIVEPREVIEW;
    CompositionAttribute.pvData = &enable;
    CompositionAttribute.cbData = sizeof(BOOL);
    lSetWindowCompositionAttribute(m_dummyWindow, &CompositionAttribute);

    vlc_assert(!m_backHostVisual);
    HR(lDwmpCreateSharedMultiWindowVisual(m_dummyWindow, m_dcompDevice.Get(), (void**)m_backHostVisual.GetAddressOf(), &m_backHostThumbnail)
       , "failed to create shared multi visual");

    updateVisual();

    HR(m_rootVisual->AddVisual(m_backHostVisual.Get(), TRUE, m_desktopVisual.Get()), "Add backhost visual");

    return true;
}
catch (const DXError &err)
{
    msg_Err(m_intf, "failed to create acrylic back host visual: '%s' code: 0x%lX", err.what(), err.code());
    return false;
}

void CompositorDCompositionAcrylicSurface::sync()
{
    if (!m_intf || !hwnd())
        return;

    const int dx = std::abs(m_leftMostScreenX);
    const int dy = std::abs(m_topMostScreenY);

    // window()->geometry()/frameGeometry() returns incorrect rect with CSD
    RECT rect;
    GetWindowRect(hwnd(), &rect);
    m_rootClip->SetLeft((float)rect.left + dx);
    m_rootClip->SetRight((float)rect.right + dx);
    m_rootClip->SetTop((float)rect.top);
    m_rootClip->SetBottom((float)rect.bottom);
    m_rootVisual->SetClip(m_rootClip.Get());

    int frameX = 0;
    int frameY = 0;

    if (m_intf->p_mi && !m_intf->p_mi->useClientSideDecoration())
    {
        frameX = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
        frameY = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CYCAPTION)
                    + GetSystemMetrics(SM_CXPADDEDBORDER);
    }
    else if (window()->visibility() & QWindow::Maximized)
    {
        // in maximized state CSDWin32EventHandler re-adds border
        frameX = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
        frameY = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    }

    m_translateTransform->SetOffsetX(-1 * (float)rect.left - frameX - dx);
    m_translateTransform->SetOffsetY(-1 * (float)rect.top - frameY - dy);
    m_rootVisual->SetTransform(m_translateTransform.Get());
}

void CompositorDCompositionAcrylicSurface::updateVisual()
{
    const auto w = window();
    if (!w || !w->screen())
        return;

    const auto screenRect = w->screen()->availableVirtualGeometry();
    RECT sourceRect {screenRect.left(), screenRect.top(), screenRect.right(), screenRect.bottom()};
    SIZE destinationSize {screenRect.width(), screenRect.height()};

    HWND hwndExclusionList[2];
    hwndExclusionList[0] = hwnd();
    hwndExclusionList[1] = m_dummyWindow;

    HRESULT hr = S_FALSE;

    if (lDwmpUpdateSharedVirtualDesktopVisual)
        hr = lDwmpUpdateSharedVirtualDesktopVisual(m_backHostThumbnail, NULL, 0, hwndExclusionList, 2, &sourceRect, &destinationSize);
    else if (lDwmpUpdateSharedMultiWindowVisual)
        hr = lDwmpUpdateSharedMultiWindowVisual(m_backHostThumbnail, NULL, 0, hwndExclusionList, 2, &sourceRect, &destinationSize, 1);
    else
        vlc_assert_unreachable();

    if (FAILED(hr))
        qDebug("failed to update shared multi window visual");
}

void CompositorDCompositionAcrylicSurface::commitChanges()
{
    m_dcompDevice->Commit();
    DwmFlush();
}

void CompositorDCompositionAcrylicSurface::requestReset()
{
    if (m_resetPending)
        return;

    m_resetPending = true;
    m_resetTimer.start(5, Qt::PreciseTimer, this);
}

void CompositorDCompositionAcrylicSurface::setActive(const bool newActive)
{
    if (newActive == m_active)
        return;

    m_active = newActive;
    if (m_active)
    {
        auto dcompositor = static_cast<vlc::CompositorDirectComposition *>(m_intf->p_compositor);
        dcompositor->addVisual(m_rootVisual);

        updateVisual();
        sync();
        commitChanges();

        // delay propagating changes to avoid flickering
        QMetaObject::invokeMethod(this, [this]()
        {
            m_intf->p_mi->setHasAcrylicSurface(true);
        }, Qt::QueuedConnection);
    }
    else
    {
        m_intf->p_mi->setHasAcrylicSurface(false);

        // delay propagating changes to avoid flickering
        QMetaObject::invokeMethod(this, [this]()
        {
            auto dcompositor = static_cast<vlc::CompositorDirectComposition *>(m_intf->p_compositor);
            dcompositor->removeVisual(m_rootVisual);
        }, Qt::QueuedConnection);
    }
}

QWindow *CompositorDCompositionAcrylicSurface::window()
{
    return m_intf ? m_intf->p_compositor->interfaceMainWindow() : nullptr;
}

HWND CompositorDCompositionAcrylicSurface::hwnd()
{
    auto w = window();
    return w->handle() ? (HWND)w->winId() : nullptr;
}

void CompositorDCompositionAcrylicSurface::timerEvent(QTimerEvent *event)
{
    if (!event)
        return;

    if (event->timerId() == m_resetTimer.timerId())
    {
        m_resetPending = false;
        m_resetTimer.stop();

        updateVisual();
        sync();
        commitChanges();
    }
}

}
