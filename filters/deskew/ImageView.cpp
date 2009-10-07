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

#include "ImageView.h.moc"
#include "ImageTransformation.h"
#include "imageproc/Constants.h"
#include <QRect>
#include <QSizeF>
#include <QPainter>
#include <QMouseEvent>
#include <QVector>
#include <QLineF>
#include <Qt>
#include <math.h>

namespace deskew
{

double const ImageView::m_maxRotationDeg = 45.0;
double const ImageView::m_maxRotationSin = sin(
	m_maxRotationDeg * imageproc::constants::DEG2RAD
);
int const ImageView::m_cellSize = 20;

ImageView::ImageView(
	QImage const& image, QImage const& downscaled_image,
	ImageTransformation const& xform)
:	ImageViewBase(image, downscaled_image, xform.transform(), xform.resultingCropArea()),
	TaggedDraggablePixmap<1>(QPixmap(":/icons/aqua-sphere.png"), 1),
	TaggedDraggablePixmap<2>(QPixmap(":/icons/aqua-sphere.png"), 1),
	m_dragHandler(*this),
	m_zoomHandler(*this),
	m_handle1DragHandler(leftHandle()),
	m_handle2DragHandler(rightHandle()),
	m_xform(xform)
{
	setMouseTracking(true);

	rootInteractionHandler().makeLastFollower(*this);
	rootInteractionHandler().makeLastFollower(m_handle1DragHandler);
	rootInteractionHandler().makeLastFollower(m_handle2DragHandler);
	rootInteractionHandler().makeLastFollower(m_dragHandler);
	rootInteractionHandler().makeLastFollower(m_zoomHandler);
	m_zoomHandler.setFocus(ZoomHandler::CENTER);

	leftHandle().setHitAreaRadius(15.0);
	rightHandle().setHitAreaRadius(15.0);

	QString const tip(tr("Drag this handle to rotate the image."));
	m_handle1DragHandler.setProximityStatusTip(tip);
	m_handle2DragHandler.setProximityStatusTip(tip);
}

ImageView::~ImageView()
{
}

void
ImageView::manualDeskewAngleSetExternally(double const degrees)
{
	if (m_xform.postRotation() == degrees) {
		return;
	}
	
	m_xform.setPostRotation(degrees);
	updateTransform(m_xform.transform(), m_xform.resultingCropArea());
}

void
ImageView::onPaint(QPainter& painter, InteractionState const& interaction)
{
	painter.setWorldMatrixEnabled(false);
	painter.setRenderHints(QPainter::Antialiasing, false);

	int const w = width();
	int const h = height();
	QPointF const center(getImageRotationOrigin());

	// Draw the semi-transparent grid.
	QPen pen(QColor(0, 0, 255, 90));
	pen.setCosmetic(true);
	pen.setWidth(1);
	painter.setPen(pen);
	QVector<QLineF> lines;
	for (double y = center.y(); (y -= m_cellSize) > 0.0;) {
		lines.push_back(QLineF(0.5, y, w - 0.5, y));
	}
	for (double y = center.y(); (y += m_cellSize) < h;) {
		lines.push_back(QLineF(0.5, y, w - 0.5, y));
	}
	for (double x = center.x(); (x -= m_cellSize) > 0.0;) {
		lines.push_back(QLineF(x, 0.5, x, h - 0.5));
	}
	for (double x = center.x(); (x += m_cellSize) < w;) {
		lines.push_back(QLineF(x, 0.5, x, h - 0.5));
	}
	painter.drawLines(lines);

	// Draw the horizontal and vertical line crossing at the center.
	pen.setColor(QColor(0, 0, 255));
	painter.setPen(pen);
	painter.setBrush(Qt::NoBrush);
	painter.drawLine(
		QPointF(0.5, center.y()),
		QPointF(w - 0.5, center.y())
	);
	painter.drawLine(
		QPointF(center.x(), 0.5),
		QPointF(center.x(), h - 0.5)
	);

	// Draw the rotation arcs.
	// Those will look like this (  )
	QRectF const arc_square(getRotationArcSquare());

	painter.setRenderHints(QPainter::Antialiasing, true);
	pen.setWidthF(1.5);
	painter.setPen(pen);
	painter.setBrush(Qt::NoBrush);
	painter.drawArc(
		arc_square,
		qRound(16 * -m_maxRotationDeg),
		qRound(16 * 2 * m_maxRotationDeg)
	);
	painter.drawArc(
		arc_square,
		qRound(16 * (180 - m_maxRotationDeg)),
		qRound(16 * 2 * m_maxRotationDeg)
	);
}

bool
ImageView::isPixmapToBeDrawn(int, InteractionState const&) const
{
	return true;
}

QPointF
ImageView::pixmapPosition(int id, InteractionState const&) const
{
	std::pair<QPointF, QPointF> const handles(getRotationHandles(getRotationArcSquare()));
	if (id == 1) {
		return handles.first;
	} else {
		return handles.second;
	}
}

void
ImageView::pixmapMoveRequest(int id, QPointF const& widget_pos)
{
	QRectF const arc_square(getRotationArcSquare());
	double const arc_radius = 0.5 * arc_square.width();
	double const abs_y = widget_pos.y();
	double rel_y = abs_y - arc_square.center().y();
	rel_y = qBound(-arc_radius, rel_y, arc_radius);

	double angle_rad = asin(rel_y / arc_radius);
	if (id == 1) {
		angle_rad = -angle_rad;
	}
	double angle_deg = angle_rad * imageproc::constants::RAD2DEG;
	angle_deg = qBound(-m_maxRotationDeg, angle_deg, m_maxRotationDeg);

	m_xform.setPostRotation(angle_deg);
	updateTransformPreservingScale(m_xform.transform(), m_xform.resultingCropArea());
}

void
ImageView::onDragFinished()
{
	emit manualDeskewAngleSet(m_xform.postRotation());
}

DraggablePixmap&
ImageView::leftHandle()
{
	return static_cast<TaggedDraggablePixmap<1>&>(*this);
}

DraggablePixmap const&
ImageView::leftHandle() const
{
	return static_cast<TaggedDraggablePixmap<1> const&>(*this);
}

DraggablePixmap&
ImageView::rightHandle()
{
	return static_cast<TaggedDraggablePixmap<2>&>(*this);
}

DraggablePixmap const&
ImageView::rightHandle() const
{
	return static_cast<TaggedDraggablePixmap<2> const&>(*this);
}

/**
 * Get the point at the center of the widget, in widget coordinates.
 * The point may be adjusted to to ensure it's at the center of a pixel.
 */
QPointF
ImageView::getImageRotationOrigin() const
{
	return QPointF(
		floor(0.5 * width()) + 0.5,
		floor(0.5 * height()) + 0.5
	);
}

/**
 * Get the square in widget coordinates where two rotation arcs will be drawn.
 */
QRectF
ImageView::getRotationArcSquare() const
{
	double const h_margin = leftHandle().handleRadius();
	double const v_margin = h_margin;

	QRectF reduced_screen_rect(rect());
	reduced_screen_rect.adjust(h_margin, v_margin, -h_margin, -v_margin);

	QSizeF arc_size(1.0, m_maxRotationSin);
	arc_size.scale(reduced_screen_rect.size(), Qt::KeepAspectRatio);
	arc_size.setHeight(arc_size.width());

	QRectF arc_square(QPointF(0, 0), arc_size);
	arc_square.moveCenter(reduced_screen_rect.center());

	return arc_square;
}

std::pair<QPointF, QPointF>
ImageView::getRotationHandles(QRectF const& arc_square) const
{
	double const rot_sin = m_xform.postRotationSin();
	double const rot_cos = m_xform.postRotationCos();
	double const arc_radius = 0.5 * arc_square.width();
	QPointF const arc_center(arc_square.center());
	QPointF left_handle(-rot_cos * arc_radius, -rot_sin * arc_radius);
	left_handle += arc_center;
	QPointF right_handle(rot_cos * arc_radius, rot_sin * arc_radius);
	right_handle += arc_center;
	return std::make_pair(left_handle, right_handle);
}

} // namespace deskew
