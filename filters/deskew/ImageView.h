/*
    Scan Tailor - Interactive post-processing tool for scanned pages.
    Copyright (C) 2007-2009  Joseph Artsimovich <joseph_a@mail.ru>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DESKEW_IMAGEVIEW_H_
#define DESKEW_IMAGEVIEW_H_

#include "ImageViewBase.h"
#include "ImageTransformation.h"
#include "DragHandler.h"
#include "ZoomHandler.h"
#include "ObjectDragHandler.h"
#include "DraggablePixmap.h"
#include <QPolygonF>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QPixmap>
#include <QString>
#include <utility>

class QRect;

namespace deskew
{

class ImageView :
	public ImageViewBase,
	private InteractionHandler,
	private TaggedDraggablePixmap<1>, // Left handle
	private TaggedDraggablePixmap<2>  // Right handle
{
	Q_OBJECT
public:
	ImageView(
		QImage const& image, QImage const& downscaled_image,
		ImageTransformation const& xform);
	
	virtual ~ImageView();
signals:
	void manualDeskewAngleSet(double degrees);
public slots:
	void manualDeskewAngleSetExternally(double degrees);
protected:
	virtual void onPaint(
		QPainter& painter, InteractionState const& interaction);

	virtual bool isPixmapToBeDrawn(int id, InteractionState const& interaction) const;

	virtual QPointF pixmapPosition(int id, InteractionState const& interaction) const;

	virtual void pixmapMoveRequest(int id, QPointF const& widget_pos);

	virtual void onDragFinished();
private:
	DraggablePixmap& leftHandle();

	DraggablePixmap const& leftHandle() const;

	DraggablePixmap& rightHandle();

	DraggablePixmap const& rightHandle() const;

	QPointF getImageRotationOrigin() const;

	QRectF getRotationArcSquare() const;

	std::pair<QPointF, QPointF> getRotationHandles(QRectF const& arc_square) const;

	static int const m_cellSize;
	static double const m_maxRotationDeg;
	static double const m_maxRotationSin;

	DragHandler m_dragHandler;
	ZoomHandler m_zoomHandler;
	ObjectDragHandler m_handle1DragHandler; // Left handle.
	ObjectDragHandler m_handle2DragHandler; // Right handle.
	ImageTransformation m_xform;
};

} // namespace deskew

#endif
