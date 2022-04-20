/*****************************************************************************
 * Copyright (C) 2020 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * ( at your option ) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifndef QMLMENUWRAPPER_HPP
#define QMLMENUWRAPPER_HPP

#include "qt.hpp"

#include <QObject>
#include <QPoint>
#include <QQuickItem>
#include "menus.hpp"
#include "maininterface/mainctx.hpp"

class MediaLib;
class MLAlbumModel;
class MLGenreModel;
class MLArtistModel;
class MLAlbumTrackModel;
class MLUrlModel;
class MLVideoModel;
class MLVideoGroupsModel;
class MLVideoFoldersModel;
class MLPlaylistListModel;
class MLPlaylistModel;
class NetworkDeviceModel;
class NetworkMediaModel;
class MainCtx;
namespace vlc {
namespace playlist {
class PlaylistControllerModel;
class PlaylistListModel;
}
}

#define SIMPLE_MENU_PROPERTY(type, name, defaultValue) \
    Q_PROPERTY(type name READ get##name WRITE set##name FINAL) \
    public: \
    inline void set##name( type data) { m_##name = data; } \
    inline type get##name() const { return m_##name; } \
    private: \
    type m_##name = defaultValue;


class StringListMenu : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    Q_INVOKABLE void popup(const QPoint &point, const QVariantList &stringList);

signals:
    void selected(int index, const QString &str);
};


class SortMenu : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    ~SortMenu();

    Q_INVOKABLE void popup(const QPoint &point, bool popupAbovePoint, const QVariantList &model);

    Q_INVOKABLE void close();

protected:
    virtual void onPopup(QMenu * menu);

signals:
    void selected(int index);

private:
    QMenu *m_menu = nullptr;
};

class SortMenuVideo : public SortMenu
{
    Q_OBJECT

    SIMPLE_MENU_PROPERTY(MainCtx *, ctx, nullptr)

protected: // SortMenu reimplementation
    void onPopup(QMenu * menu) override;

signals:
    void grouping(MainCtx::Grouping grouping);
};

//inherit VLCMenuBar so we can access menu creation functions
class QmlGlobalMenu : public VLCMenuBar
{
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(MainCtx*, ctx, nullptr)
public:
    explicit QmlGlobalMenu(QObject *parent = nullptr);
    ~QmlGlobalMenu();

signals:
    void aboutToShow();
    void aboutToHide();

public slots:
    void popup( QPoint pos );
private:
    QMenu* m_menu = nullptr;
};

//inherit VLCMenuBar so we can access menu creation functions
class QmlMenuBarMenu;
class QmlMenuBar : public VLCMenuBar
{
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(MainCtx*, ctx, nullptr)
    SIMPLE_MENU_PROPERTY(QQuickItem*, menubar, nullptr)
    SIMPLE_MENU_PROPERTY(bool, openMenuOnHover, false)

public:
    explicit QmlMenuBar(QObject *parent = nullptr);
    ~QmlMenuBar();

signals:
    //navigate to the left(-1)/right(1) menu
    void navigateMenu(int direction);

    void menuClosed();

public slots:
    void popupMediaMenu( QQuickItem* button);
    void popupPlaybackMenu( QQuickItem* button);
    void popupAudioMenu( QQuickItem* button );
    void popupVideoMenu( QQuickItem* button );
    void popupSubtitleMenu( QQuickItem* button );
    void popupToolsMenu( QQuickItem* button );
    void popupViewMenu( QQuickItem* button );
    void popupHelpMenu( QQuickItem* button );

private slots:
    void onMenuClosed();

private:
    typedef QMenu* (*CreateMenuFunc)();
    void popupMenuCommon( QQuickItem* button, std::function<void(QMenu*)> createMenuFunc);
    QMenu* m_menu = nullptr;
    QQuickItem* m_button = nullptr;
    friend class QmlMenuBarMenu;
};

//specialized QMenu for QmlMenuBar
class QmlMenuBarMenu : public QMenu
{
    Q_OBJECT
public:
    QmlMenuBarMenu(QmlMenuBar* menubar, QWidget* parent = nullptr);
    ~QmlMenuBarMenu();
protected:
    void mouseMoveEvent(QMouseEvent* mouseEvent) override;
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
private:
    QmlMenuBar* m_menubar = nullptr;
};

class QmlBookmarkMenu : public QObject
{
    Q_OBJECT

    SIMPLE_MENU_PROPERTY(MainCtx *, ctx, nullptr)

    SIMPLE_MENU_PROPERTY(PlayerController *, player, nullptr)

public:
    explicit QmlBookmarkMenu(QObject * parent = nullptr);

    ~QmlBookmarkMenu();

public: // Interface
    Q_INVOKABLE void popup(QPoint pos);

signals:
    void aboutToHide();
    void aboutToShow();

private:
    QMenu * m_menu = nullptr;
};

class QmlRendererMenu : public QObject
{
    Q_OBJECT

    SIMPLE_MENU_PROPERTY(MainCtx *, ctx, nullptr)

public:
    explicit QmlRendererMenu(QObject * parent = nullptr);

    ~QmlRendererMenu();

public: // Interface
    Q_INVOKABLE void popup(QPoint pos);

signals:
    void aboutToHide();
    void aboutToShow();

private:
    RendererMenu * m_menu = nullptr;
};

class BaseMedialibMenu : public QObject
{
    Q_OBJECT
public:
    BaseMedialibMenu(QObject* parent = nullptr);
    virtual ~BaseMedialibMenu();
signals:
    void showMediaInformation(int index);

protected:
    void medialibAudioContextMenu(MediaLib* ml, const QVariantList& mlId, const QPoint& pos, const QVariantMap& options);

    template<typename ModelType>
    void popup(ModelType* model, typename ModelType::Roles role,  const QModelIndexList &selected, const  QPoint& pos, const QVariantMap& options) {
        if (!model)
            return;

        MediaLib* ml= model->ml();
        if (!ml)
            return;

        QVariantList itemIdList;
        for (const QModelIndex& modelIndex : selected)
            itemIdList.push_back(model->data(modelIndex, role));
        medialibAudioContextMenu(ml, itemIdList, pos, options);
    }

private:
    QMenu* m_menu = nullptr;
};

class AlbumContextMenu : public BaseMedialibMenu {
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(MLAlbumModel*, model, nullptr)
public:
    AlbumContextMenu(QObject* parent = nullptr);
public slots:
    void popup(const QModelIndexList& selected, QPoint pos, QVariantMap options = {});
};


class ArtistContextMenu : public BaseMedialibMenu {
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(MLArtistModel*, model, nullptr)
public:
    ArtistContextMenu(QObject* parent = nullptr);
public slots:
    void popup(const QModelIndexList& selected, QPoint pos, QVariantMap options = {});
};


class GenreContextMenu : public BaseMedialibMenu {
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(MLGenreModel*, model, nullptr)
public:
    GenreContextMenu(QObject* parent = nullptr);
public slots:
    void popup(const QModelIndexList& selected, QPoint pos, QVariantMap options = {});
};


class AlbumTrackContextMenu : public BaseMedialibMenu {
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(MLAlbumTrackModel*, model, nullptr)
public:
    AlbumTrackContextMenu(QObject* parent = nullptr);
public slots:
    void popup(const QModelIndexList& selected, QPoint pos, QVariantMap options = {});
};

class URLContextMenu : public BaseMedialibMenu {
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(MLUrlModel*, model, nullptr)
public:
    URLContextMenu(QObject* parent = nullptr);
public slots:
    void popup(const QModelIndexList& selected, QPoint pos, QVariantMap options = {});
};

class VideoContextMenu : public QObject {
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(MLVideoModel*, model, nullptr)
public:
    VideoContextMenu(QObject* parent = nullptr);
    ~VideoContextMenu();

public slots:
    void popup(const QModelIndexList& selected, QPoint pos, QVariantMap options = {} );
signals:
    void showMediaInformation(int index);
private:
    QMenu* m_menu = nullptr;
};

//-------------------------------------------------------------------------------------------------
// Groups
//-------------------------------------------------------------------------------------------------

class VideoGroupsContextMenu : public QObject {
    Q_OBJECT

    SIMPLE_MENU_PROPERTY(MLVideoGroupsModel *, model, nullptr)

public:
    VideoGroupsContextMenu(QObject * parent = nullptr);

    ~VideoGroupsContextMenu(); /* override */

public slots:
    void popup(const QModelIndexList & selected, QPoint pos, QVariantMap options = {});

signals:
    void showMediaInformation(int index);

private:
    QMenu * m_menu = nullptr;
};

// Folders

class VideoFoldersContextMenu : public QObject {
    Q_OBJECT

    SIMPLE_MENU_PROPERTY(MLVideoFoldersModel *, model, nullptr)

public:
    VideoFoldersContextMenu(QObject * parent = nullptr);

    ~VideoFoldersContextMenu(); /* override */

public slots:
    void popup(const QModelIndexList & selected, QPoint pos, QVariantMap options = {});

signals:
    // FIXME: This signal is required for VideoAll Connections.
    void showMediaInformation(int index);

private:
    QMenu * m_menu = nullptr;
};

//-------------------------------------------------------------------------------------------------

class PlaylistListContextMenu : public QObject {
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(MLPlaylistListModel *, model, nullptr)
public:
    PlaylistListContextMenu(QObject * parent = nullptr);
    ~PlaylistListContextMenu() /* override */;

public slots:
    void popup(const QModelIndexList & selected, QPoint pos, QVariantMap options = {});
private:
    QMenu* m_menu = nullptr;
};

class PlaylistMediaContextMenu : public QObject {
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(MLPlaylistModel *, model, nullptr)
public:
    PlaylistMediaContextMenu(QObject * parent = nullptr);
    ~PlaylistMediaContextMenu() /* override */;

public slots:
    void popup(const QModelIndexList & selected, QPoint pos, QVariantMap options = {});
signals:
    void showMediaInformation(int index);
private:
    QMenu* m_menu = nullptr;
};

class NetworkMediaContextMenu : public QObject {
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(NetworkMediaModel*, model, nullptr)
public:
    NetworkMediaContextMenu(QObject* parent = nullptr);
    ~NetworkMediaContextMenu();

public slots:
    void popup(const QModelIndexList& selected, QPoint pos );
private:
    QMenu* m_menu = nullptr;
};

class NetworkDeviceContextMenu : public QObject {
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(NetworkDeviceModel*, model, nullptr)
public:
    NetworkDeviceContextMenu(QObject* parent = nullptr);
    ~NetworkDeviceContextMenu();
public slots:
    void popup(const QModelIndexList& selected, QPoint pos );
private:
    QMenu* m_menu = nullptr;
};


class PlaylistContextMenu : public QObject {
    Q_OBJECT
    SIMPLE_MENU_PROPERTY(vlc::playlist::PlaylistListModel*, model, nullptr)
    SIMPLE_MENU_PROPERTY(vlc::playlist::PlaylistControllerModel*, controler, nullptr)
public:
    PlaylistContextMenu(QObject* parent = nullptr);
    ~PlaylistContextMenu();

public slots:
    void popup(int currentIndex, QPoint pos );
private:
    QMenu* m_menu = nullptr;
};

#undef SIMPLE_MENU_PROPERTY

#endif // QMLMENUWRAPPER_HPP
