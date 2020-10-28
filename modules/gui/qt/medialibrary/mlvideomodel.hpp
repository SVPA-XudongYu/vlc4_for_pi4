/*****************************************************************************
 * Copyright (C) 2019 VLC authors and VideoLAN
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

#ifndef MCVIDEOMODEL_H
#define MCVIDEOMODEL_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_media_library.h>

#include "mlbasemodel.hpp"
#include "mlvideo.hpp"

#include <QObject>

class MLVideoModel : public MLSlidingWindowModel<MLVideo>
{
    Q_OBJECT

public:
    enum Role {
        VIDEO_ID = Qt::UserRole + 1,
        VIDEO_TITLE,
        VIDEO_THUMBNAIL,
        VIDEO_DURATION,
        VIDEO_DURATION_SHORT,
        VIDEO_PROGRESS,
        VIDEO_PLAYCOUNT,
        VIDEO_RESOLUTION,
        VIDEO_CHANNEL,
        VIDEO_MRL,
        VIDEO_DISPLAY_MRL,
        VIDEO_VIDEO_TRACK,
        VIDEO_AUDIO_TRACK,

        VIDEO_TITLE_FIRST_SYMBOL,
    };


public:
    explicit MLVideoModel(QObject* parent = nullptr);
    virtual ~MLVideoModel() = default;

    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    std::vector<std::unique_ptr<MLVideo>> fetch() override;
    size_t countTotalElements() const override;
    vlc_ml_sorting_criteria_t roleToCriteria(int role) const override;
    vlc_ml_sorting_criteria_t nameToCriteria(QByteArray name) const override;
    virtual void onVlcMlEvent( const vlc_ml_event_t* event ) override;
    virtual void thumbnailUpdated( int ) override;

    static QHash<QByteArray, vlc_ml_sorting_criteria_t> M_names_to_criteria;
    QByteArray criteriaToName(vlc_ml_sorting_criteria_t criteria) const override;
};

#endif // MCVIDEOMODEL_H
