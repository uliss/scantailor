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

#include "ImageViewBase.h.moc"
#include "NonCopyable.h"
#include "PixmapRenderer.h"
#include "BackgroundExecutor.h"
#include "Dpm.h"
#include "Dpi.h"
#include "imageproc/PolygonUtils.h"
#include "imageproc/Transform.h"
#include <QPointer>
#include <QAtomicInt>
#include <QPainter>
#include <QPainterPath>
#include <QBrush>
#include <QLineF>
#include <QPolygonF>
#include <QPalette>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QStatusTipEvent>
#include <QApplication>
#include <Qt>
#include <QDebug>
#include <algorithm>
#include <assert.h>
#include <math.h>

using namespace imageproc;

class ImageViewBase::HqTransformTask :
	public AbstractCommand0<IntrusivePtr<AbstractCommand0<void> > >,
	public QObject
{
	DECLARE_NON_COPYABLE(HqTransformTask)
public:
	HqTransformTask(
		ImageViewBase* image_view,
		QImage const& image, QTransform const& xform,
		QSize const& target_size);
	
	void cancel() { m_ptrResult->cancel(); }
	
	bool const isCancelled() const { return m_ptrResult->isCancelled(); }
	
	virtual IntrusivePtr<AbstractCommand0<void> > operator()();
private:
	class Result : public AbstractCommand0<void>
	{
	public:
		Result(ImageViewBase* image_view);
		
		void setData(QPoint const& origin, QImage const& hq_image);
		
		void cancel() { m_cancelFlag.fetchAndStoreRelaxed(1); }
		
		bool isCancelled() const { return m_cancelFlag.fetchAndAddRelaxed(0) != 0; }
		
		virtual void operator()();
	private:
		QPointer<ImageViewBase> m_ptrImageView;
		QPoint m_origin;
		QImage m_hqImage;
		mutable QAtomicInt m_cancelFlag;
	};
	
	IntrusivePtr<Result> m_ptrResult;
	QImage m_image;
	QTransform m_xform;
	QSize m_targetSize;
};


/**
 * \brief Temporarily adjust the widget focal point, then change it back.
 *
 * When adjusting and restoring the widget focal point, the pixmap
 * focal point is recalculated accordingly.
 */
class ImageViewBase::TempFocalPointAdjuster
{
public:
	/**
	 * Change the widget focal point to obj.centeredWidgetFocalPoint().
	 */
	TempFocalPointAdjuster(ImageViewBase& obj);
	
	/**
	 * Change the widget focal point to \p temp_widget_fp
	 */
	TempFocalPointAdjuster(ImageViewBase& obj, QPointF temp_widget_fp);
	
	/**
	 * Restore the widget focal point.
	 */
	~TempFocalPointAdjuster();
private:
	ImageViewBase& m_rObj;
	QPointF m_origWidgetFP;
};


class ImageViewBase::TransformChangeWatcher
{
public:
	TransformChangeWatcher(ImageViewBase& owner);

	~TransformChangeWatcher();
private:
	ImageViewBase& m_rOwner;
	QTransform m_imageToVirtual;
	QTransform m_virtualToWidget;
};


ImageViewBase::ImageViewBase(
	QImage const& image, QImage const& downscaled_image,
	QTransform const& image_to_virt, QPolygonF const& virt_display_area,
	Margins const& margins)
:	m_defaultStatusTip(tr("Use the mouse wheel to zoom.  When zoomed, dragging is possible.")),
	m_unrestrictedDragStatusTip(tr("Unrestricted dragging is possible by holding down the Shift key.")),
	m_image(image),
	m_virtualDisplayArea(virt_display_area),
	m_imageToVirtual(image_to_virt),
	m_virtualToImage(image_to_virt.inverted()),
	m_margins(margins),
	m_zoom(1.0),
	m_transformChangeWatchersActive(0),
	m_currentCursorShape(Qt::ArrowCursor),
	m_hqTransformEnabled(true)
{
	setFocusPolicy(Qt::WheelFocus);

	if (downscaled_image.isNull()) {
		m_pixmap = QPixmap::fromImage(createDownscaledImage(image));
	} else {
		m_pixmap = QPixmap::fromImage(downscaled_image);
	}
	
	m_pixmapToImage.scale(
		(double)m_image.width() / m_pixmap.width(),
		(double)m_image.height() / m_pixmap.height()
	);
	
	m_widgetFocalPoint = centeredWidgetFocalPoint();
	m_pixmapFocalPoint = m_virtualToImage.map(virtualDisplayRect().center());
	
	ensureStatusTip(defaultStatusTip());
	
	m_timer.setSingleShot(true);
	m_timer.setInterval(150); // msec
	connect(
		&m_timer, SIGNAL(timeout()),
		this, SLOT(initiateBuildingHqVersion())
	);
	
	updateWidgetTransformAndFixFocalPoint(CENTER_IF_FITS);
}

ImageViewBase::~ImageViewBase()
{
}

void
ImageViewBase::hqTransformSetEnabled(bool const enabled)
{
	if (!enabled && m_hqTransformEnabled) {
		// Turning off.
		m_hqTransformEnabled = false;
		if (m_ptrHqTransformTask.get()) {
			m_ptrHqTransformTask->cancel();
			m_ptrHqTransformTask.reset();
		}
		if (!m_hqPixmap.isNull()) {
			m_hqPixmap = QPixmap();
			update();
		}
	} else if (enabled && !m_hqTransformEnabled) {
		// Turning on.
		m_hqTransformEnabled = true;
		update();
	}
}

QImage
ImageViewBase::createDownscaledImage(QImage const& image)
{
	assert(!image.isNull());
	
	// Original and downscaled DPM.
	Dpm const o_dpm(image);
	Dpm const d_dpm(Dpi(300, 300));
	
	int const o_w = image.width();
	int const o_h = image.height();
	
	int d_w = o_w * d_dpm.horizontal() / o_dpm.horizontal();
	int d_h = o_h * d_dpm.vertical() / o_dpm.vertical();
	d_w = qBound(1, d_w, o_w);
	d_h = qBound(1, d_h, o_h);
	
	if (d_w * 1.2 > o_w || d_h * 1.2 > o_h) {
		// Sizes are close - no point in downscaling.
		return image;
	}
	
	QTransform xform;
	xform.scale((double)d_w / o_w, (double)d_h / o_h);
	return transform(image, xform, QRect(0, 0, d_w, d_h), Qt::white);
}

void
ImageViewBase::paintEvent(QPaintEvent* const event)
{
	QPainter painter(this);
	painter.save();
	
#if !defined(Q_WS_X11)
	double const xscale = m_virtualToWidget.m11();
	
	// Width of a source pixel in mm, as it's displayed on screen.
	double const pixel_width = widthMM() * xscale / width();
	
	// On X11 SmoothPixmapTransform is too slow.
	painter.setRenderHint(QPainter::SmoothPixmapTransform, pixel_width < 0.5);
#endif

	if (validateHqPixmap()) {
		painter.drawPixmap(m_hqPixmapPos, m_hqPixmap);
	} else {
		scheduleHqVersionRebuild();

		painter.setWorldTransform(
			m_pixmapToImage * m_imageToVirtual * m_virtualToWidget
		);
		PixmapRenderer::drawPixmap(painter, m_pixmap);
	}
	
	painter.setRenderHints(QPainter::Antialiasing, true);
	painter.setWorldMatrixEnabled(false);
	
	// Cover parts of the image that should not be visible with background.
	// Note that because of Qt::WA_OpaquePaintEvent attribute, we need
	// to paint the whole widget, which we do here.
	
	QPolygonF const image_area(
		PolygonUtils::round(
			m_virtualToWidget.map(
				m_imageToVirtual.map(QRectF(m_image.rect()))
			)
		)
	);
	QPolygonF const crop_area(
		PolygonUtils::round(m_virtualToWidget.map(m_virtualDisplayArea))
	);
	
	QPolygonF const intersected_area(
		PolygonUtils::round(image_area.intersected(crop_area))
	);
	
	QPainterPath intersected_path;
	intersected_path.addPolygon(intersected_area);
	
	QPainterPath containing_path;
	containing_path.addRect(rect());
	
	QBrush const brush(palette().color(QPalette::Window));
	QPen pen(brush, 1.0);
	pen.setCosmetic(true);
	
	// By using a pen with the same color as the brush, we essentially
	// expanding the area we are going to draw.  It's necessary because
	// XRender doesn't provide subpixel accuracy.
	
	painter.setPen(pen);
	painter.setBrush(brush);
	painter.drawPath(containing_path.subtracted(intersected_path));
	
	painter.restore();
	
	painter.setWorldTransform(m_virtualToWidget);
	paintOverImage(painter);

	// TODO: only if transformation changed since last proximityUpdate
	m_interactionState.resetProximity();
	if (!m_interactionState.captured()) {
		m_rootInteractionHandler.proximityUpdate(
			QPointF(0.5, 0.5) + mapFromGlobal(QCursor::pos()), m_interactionState
		);
		updateStatusTipAndCursor();
	}

	m_rootInteractionHandler.paint(painter, m_interactionState);
}

void
ImageViewBase::paintOverImage(QPainter& painter)
{
}

void
ImageViewBase::resizeEvent(QResizeEvent* event)
{
	if (event->oldSize().isEmpty()) {
		m_widgetFocalPoint = centeredWidgetFocalPoint();
	} else {
		double const x_fraction = m_widgetFocalPoint.x()
				/ event->oldSize().width();
		double const y_fraction = m_widgetFocalPoint.y()
				/ event->oldSize().height();
		m_widgetFocalPoint.setX(x_fraction * event->size().width());
		m_widgetFocalPoint.setY(y_fraction * event->size().height());
	}
	
	updateWidgetTransform();
}

void
ImageViewBase::handleImageDragging(QMouseEvent* const event)
{
	switch (event->type()) {
		case QEvent::MouseButtonPress:
			if (event->button() == Qt::LeftButton) {
				m_lastMousePos = event->pos();
			}
			break;
		case QEvent::MouseButtonRelease:
			if (event->button() == Qt::LeftButton) {
				ensureStatusTip(defaultStatusTip());
			}
			break;
		case QEvent::MouseMove:
			if (event->buttons() & Qt::LeftButton) {
				QPoint movement(event->pos());
				movement -= m_lastMousePos;
				m_lastMousePos = event->pos();

				QPointF adjusted_fp(m_widgetFocalPoint);
				adjusted_fp += movement;

				if (event->modifiers() & Qt::ShiftModifier) {
					setNewWidgetFP(adjusted_fp);
				} else {
					adjustAndSetNewWidgetFP(adjusted_fp);
				}

				update();
			}
		default:;
	}
}

bool
ImageViewBase::isDraggingPossible() const
{
	QRectF const widget_rect(m_virtualToWidget.mapRect(virtualDisplayRect()));
	if (widget_rect.top() <= -1.0) {
		return true;
	}
	if (widget_rect.left() <= -1.0) {
		return true;
	}
	if (widget_rect.bottom() >= height() + 1) {
		return true;
	}
	if (widget_rect.right() >= width() + 1) {
		return true;
	}
	return false;
}

QRectF
ImageViewBase::marginsRect() const
{
	QRectF r(rect());
	r.adjust(
		m_margins.left(), m_margins.top(),
		-m_margins.right(), -m_margins.bottom()
	);
	if (r.isEmpty()) {
		return QRectF(r.center(), r.center());
	}
	return r;
}

QRectF
ImageViewBase::getVisibleWidgetRect() const
{
	QRectF const widget_rect(m_virtualToWidget.mapRect(virtualDisplayRect()));
	return widget_rect.intersected(marginsRect());
}

void
ImageViewBase::setWidgetFocalPoint(QPointF const& widget_fp)
{
	setNewWidgetFP(widget_fp, /*update =*/true);
}

void
ImageViewBase::adjustAndSetWidgetFocalPoint(QPointF const& widget_fp)
{
	adjustAndSetNewWidgetFP(widget_fp, /*update=*/true);
}

void
ImageViewBase::zoom(double zoom)
{
	if (m_zoom != zoom) {
		m_zoom = zoom;
		updateWidgetTransform();
		update();
	}
}

void
ImageViewBase::updateTransform(
	QTransform const& image_to_virt, QPolygonF const& virt_display_area)
{
	TransformChangeWatcher const watcher(*this);
	TempFocalPointAdjuster const temp_fp(*this);

	m_imageToVirtual = image_to_virt;
	m_virtualToImage = image_to_virt.inverted();
	m_virtualDisplayArea = virt_display_area;

	updateWidgetTransform();
	update();
}

void
ImageViewBase::updateTransformAndFixFocalPoint(
	QTransform const& image_to_virt, QPolygonF const& virt_display_area,
	FocalPointMode const mode)
{
	TransformChangeWatcher const watcher(*this);
	TempFocalPointAdjuster const temp_fp(*this);
	
	m_imageToVirtual = image_to_virt;
	m_virtualToImage = image_to_virt.inverted();
	m_virtualDisplayArea = virt_display_area;

	updateWidgetTransformAndFixFocalPoint(mode);
	update();
}

void
ImageViewBase::updateTransformPreservingScale(
	QTransform const& image_to_virt, QPolygonF const& virt_display_area)
{
	TransformChangeWatcher const watcher(*this);
	TempFocalPointAdjuster const temp_fp(*this);
	
	// An arbitrary line in image coordinates.
	QLineF const image_line(0.0, 0.0, 1.0, 1.0);
	
	QLineF const widget_line_before(
		(m_imageToVirtual * m_virtualToWidget).map(image_line)
	);
	
	m_imageToVirtual = image_to_virt;
	m_virtualToImage = image_to_virt.inverted();
	m_virtualDisplayArea = virt_display_area;

	updateWidgetTransform();
	
	QLineF const widget_line_after(
		(m_imageToVirtual * m_virtualToWidget).map(image_line)
	);
	
	m_zoom *= widget_line_before.length() / widget_line_after.length();
	updateWidgetTransform();
	
	update();
}

void
ImageViewBase::ensureCursorShape(Qt::CursorShape const cursor_shape)
{
	if (cursor_shape != m_currentCursorShape) {
		m_currentCursorShape = cursor_shape;
		setCursor(cursor_shape);
	}
}

void
ImageViewBase::ensureStatusTip(QString const& status_tip)
{
	QString const cur_status_tip(statusTip());
	if (cur_status_tip.constData() == status_tip.constData()) {
		return;
	}
	if (cur_status_tip == status_tip) {
		return;
	}
	
	setStatusTip(status_tip);
	
	if (underMouse()) {
		// Note that setStatusTip() alone is not enough,
		// as it's only taken into account when the mouse
		// enters the widget.
		// Also note that we use postEvent() rather than sendEvent(),
		// because sendEvent() may immediately process other events.
		QApplication::postEvent(this, new QStatusTipEvent(status_tip));
	}
}

QString
ImageViewBase::defaultStatusTip() const
{
	return m_defaultStatusTip;
}

void
ImageViewBase::enterEvent(QEvent* event)
{
	setFocus(Qt::MouseFocusReason);
}

void
ImageViewBase::keyPressEvent(QKeyEvent* event)
{
	m_rootInteractionHandler.keyPressEvent(event, m_interactionState);
	updateStatusTipAndCursor();
}

void
ImageViewBase::keyReleaseEvent(QKeyEvent* event)
{
	m_rootInteractionHandler.keyReleaseEvent(event, m_interactionState);
	updateStatusTipAndCursor();
}

void
ImageViewBase::mousePressEvent(QMouseEvent* event)
{
	m_interactionState.resetProximity();
	if (!m_interactionState.captured()) {
		m_rootInteractionHandler.proximityUpdate(
			QPointF(0.5, 0.5) + event->pos(), m_interactionState
		);
	}

	m_rootInteractionHandler.mousePressEvent(event, m_interactionState);
	updateStatusTipAndCursor();
}

void
ImageViewBase::mouseReleaseEvent(QMouseEvent* event)
{
	m_interactionState.resetProximity();
	if (!m_interactionState.captured()) {
		m_rootInteractionHandler.proximityUpdate(
			QPointF(0.5, 0.5) + event->pos(), m_interactionState
		);
	}

	m_rootInteractionHandler.mouseReleaseEvent(event, m_interactionState);
	updateStatusTipAndCursor();
}

void
ImageViewBase::mouseMoveEvent(QMouseEvent* event)
{
	m_interactionState.resetProximity();
	if (!m_interactionState.captured()) {
		m_rootInteractionHandler.proximityUpdate(
			QPointF(0.5, 0.5) + event->pos(), m_interactionState
		);
	}

	m_rootInteractionHandler.mouseMoveEvent(event, m_interactionState);
	updateStatusTipAndCursor();
}

void
ImageViewBase::wheelEvent(QWheelEvent* event)
{
	m_rootInteractionHandler.wheelEvent(event, m_interactionState);
	updateStatusTipAndCursor();
}

void
ImageViewBase::contextMenuEvent(QContextMenuEvent* event)
{
	m_rootInteractionHandler.contextMenuEvent(event, m_interactionState);
	updateStatusTipAndCursor();
}

/**
 * Updates m_virtualToWidget and m_widgetToVirtual.\n
 * To be called whenever any of the following is modified:
 * m_imageToVirt, m_widgetFocalPoint, m_pixmapFocalPoint, m_zoom.
 * Modifying both m_widgetFocalPoint and m_pixmapFocalPoint in a way
 * that doesn't cause image movement doesn't require calling this method.
 */
void
ImageViewBase::updateWidgetTransform()
{
	TransformChangeWatcher const watcher(*this);

	QRectF const virt_rect(virtualDisplayRect());
	QPointF const virt_origin(m_imageToVirtual.map(m_pixmapFocalPoint));
	QPointF const widget_origin(m_widgetFocalPoint);
	
	QSizeF zoom1_widget_size(virt_rect.size());
	zoom1_widget_size.scale(marginsRect().size(), Qt::KeepAspectRatio);
	
	double const zoom1_x = zoom1_widget_size.width() / virt_rect.width();
	double const zoom1_y = zoom1_widget_size.height() / virt_rect.height();
	
	QTransform xform;
	xform.translate(-virt_origin.x(), -virt_origin.y());
	xform *= QTransform().scale(zoom1_x * m_zoom, zoom1_y * m_zoom);
	xform *= QTransform().translate(widget_origin.x(), widget_origin.y());

	m_virtualToWidget = xform;
	m_widgetToVirtual = m_virtualToWidget.inverted();
}

/**
 * Updates m_virtualToWidget and m_widgetToVirtual and adjusts
 * the focal point if necessary.\n
 * To be called whenever m_imageToVirt is modified in such a way that
 * may invalidate the focal point.
 */
void
ImageViewBase::updateWidgetTransformAndFixFocalPoint(FocalPointMode const mode)
{
	TransformChangeWatcher const watcher(*this);

	// This must go before getIdealWidgetFocalPoint(), as it
	// recalculates m_virtualToWidget, that is used by
	// getIdealWidgetFocalPoint().
	updateWidgetTransform();
	
	QPointF const ideal_widget_fp(getIdealWidgetFocalPoint(mode));
	if (ideal_widget_fp != m_widgetFocalPoint) {
		m_widgetFocalPoint = ideal_widget_fp;
		updateWidgetTransform();
	}
}

/**
 * Returns a proposed value for m_widgetFocalPoint to minimize the
 * unused widget space.  Unused widget space indicates one or both
 * of the following:
 * \li The image is smaller than the display area.
 * \li Parts of the image are outside of the display area.
 *
 * \param mode If set to CENTER_IF_FITS, then the returned focal point
 *        will center the image if it completely fits into the widget.
 *        This works in horizontal and vertical directions independently.\n
 *        If \p mode is set to DONT_CENTER and the image completely fits
 *        the widget, then the returned focal point will cause a minimal
 *        move to force the whole image to be visible.
 *
 * In case there is no unused widget space, the returned focal point
 * is equal to the current focal point (m_widgetFocalPoint).  This works
 * in horizontal and vertical dimensions independently.
 */
QPointF
ImageViewBase::getIdealWidgetFocalPoint(FocalPointMode const mode) const
{
	// Widget rect reduced by margins.
	QRectF const display_area(marginsRect());
	
	// The virtual image rectangle in widget coordinates.
	QRectF const image_area(m_virtualToWidget.mapRect(virtualDisplayRect()));
	
	// Unused display space from each side.
	double const left_margin = image_area.left() - display_area.left();
	double const right_margin = display_area.right() - image_area.right();
	double const top_margin = image_area.top() - display_area.top();
	double const bottom_margin = display_area.bottom() - image_area.bottom();
	
	QPointF widget_focal_point(m_widgetFocalPoint);
	
	if (mode == CENTER_IF_FITS && left_margin + right_margin >= 0.0) {
		// Image fits horizontally, so center it in that direction
		// by equalizing its left and right margins.
		double const new_margins = 0.5 * (left_margin + right_margin);
		widget_focal_point.rx() += new_margins - left_margin;
	} else if (left_margin < 0.0 && right_margin > 0.0) {
		// Move image to the right so that either left_margin or
		// right_margin becomes zero, whichever requires less movement.
		double const movement = std::min(fabs(left_margin), fabs(right_margin));
		widget_focal_point.rx() += movement;
	} else if (right_margin < 0.0 && left_margin > 0.0) {
		// Move image to the left so that either left_margin or
		// right_margin becomes zero, whichever requires less movement.
		double const movement = std::min(fabs(left_margin), fabs(right_margin));
		widget_focal_point.rx() -= movement;
	}
	
	if (mode == CENTER_IF_FITS && top_margin + bottom_margin >= 0.0) {
		// Image fits vertically, so center it in that direction
		// by equalizing its top and bottom margins.
		double const new_margins = 0.5 * (top_margin + bottom_margin);
		widget_focal_point.ry() += new_margins - top_margin;
	} else if (top_margin < 0.0 && bottom_margin > 0.0) {
		// Move image down so that either top_margin or bottom_margin
		// becomes zero, whichever requires less movement.
		double const movement = std::min(fabs(top_margin), fabs(bottom_margin));
		widget_focal_point.ry() += movement;
	} else if (bottom_margin < 0.0 && top_margin > 0.0) {
		// Move image up so that either top_margin or bottom_margin
		// becomes zero, whichever requires less movement.
		double const movement = std::min(fabs(top_margin), fabs(bottom_margin));
		widget_focal_point.ry() -= movement;
	}
	
	return widget_focal_point;
}

void
ImageViewBase::setNewWidgetFP(QPointF const widget_fp, bool const update)
{
	if (widget_fp != m_widgetFocalPoint) {
		m_widgetFocalPoint = widget_fp;
		updateWidgetTransform();
		if (update) {
			this->update();
		}
	}
}

/**
 * Used when dragging the image.  It adjusts the movement to disallow
 * dragging it away from the ideal position (determined by
 * getIdealWidgetFocalPoint()).  Movement towards the ideal position
 * is permitted.  This works independently in horizontal and vertical
 * direction.
 *
 * \param proposed_widget_fp The proposed value for m_widgetFocalPoint.
 */
void
ImageViewBase::adjustAndSetNewWidgetFP(
	QPointF const proposed_widget_fp, bool const update)
{
	// We first apply the proposed focal point, and only then
	// calculate the ideal one.  That's done because
	// the ideal focal point is the current focal point when
	// no widget space is wasted (image covers the whole widget).
	// We don't want the ideal focal point to be equal to the current
	// one, as that would disallow any movements.
	QPointF const old_widget_fp(m_widgetFocalPoint);
	setNewWidgetFP(proposed_widget_fp, update);
	
	QPointF const ideal_widget_fp(getIdealWidgetFocalPoint(CENTER_IF_FITS));
	
	QPointF const towards_ideal(ideal_widget_fp - old_widget_fp);
	QPointF const towards_proposed(proposed_widget_fp - old_widget_fp);
	
	QPointF movement(towards_proposed);
	
	// Horizontal movement.
	if (towards_ideal.x() * towards_proposed.x() < 0.0) {
		// Wrong direction - no movement at all.
		movement.setX(0.0);
	} else if (fabs(towards_proposed.x()) > fabs(towards_ideal.x())) {
		// Too much movement - limit it.
		movement.setX(towards_ideal.x());
	}
	
	// Vertical movement.
	if (towards_ideal.y() * towards_proposed.y() < 0.0) {
		// Wrong direction - no movement at all.
		movement.setY(0.0);
	} else if (fabs(towards_proposed.y()) > fabs(towards_ideal.y())) {
		// Too much movement - limit it.
		movement.setY(towards_ideal.y());
	}
	
	QPointF const adjusted_widget_fp(old_widget_fp + movement);
	if (adjusted_widget_fp != m_widgetFocalPoint) {
		m_widgetFocalPoint = adjusted_widget_fp;
		updateWidgetTransform();
		if (update) {
			this->update();
		}
	}
}

/**
 * Returns the center point of the available display area.
 */
QPointF
ImageViewBase::centeredWidgetFocalPoint() const
{
	return marginsRect().center();
}

void
ImageViewBase::setWidgetFocalPointWithoutMoving(QPointF const new_widget_fp)
{
	m_widgetFocalPoint = new_widget_fp;
	m_pixmapFocalPoint = m_virtualToImage.map(
		m_widgetToVirtual.map(m_widgetFocalPoint)
	);
}

/**
 * Returns true if m_hqPixmap is valid and up to date.
 */
bool
ImageViewBase::validateHqPixmap() const
{
	if (!m_hqTransformEnabled) {
		return false;
	}

	if (m_hqPixmap.isNull()) {
		return false;
	}

	if (m_hqSourceId != m_image.cacheKey()) {
		return false;
	}

	if (m_hqXform != m_imageToVirtual * m_virtualToWidget) {
		return false;
	}

	return true;
}

void
ImageViewBase::scheduleHqVersionRebuild()
{
	QTransform const xform(m_imageToVirtual * m_virtualToWidget);

	if (!m_timer.isActive() || m_potentialHqXform != xform) {
		if (m_ptrHqTransformTask.get()) {
			m_ptrHqTransformTask->cancel();
			m_ptrHqTransformTask.reset();
		}
		m_potentialHqXform = xform;
		m_timer.start();
	}
}

void
ImageViewBase::initiateBuildingHqVersion()
{
	if (validateHqPixmap()) {
		return;
	}

	m_hqPixmap = QPixmap();

	if (m_ptrHqTransformTask.get()) {
		m_ptrHqTransformTask->cancel();
		m_ptrHqTransformTask.reset();
	}

	QTransform const xform(m_imageToVirtual * m_virtualToWidget);
	IntrusivePtr<HqTransformTask> const task(
		new HqTransformTask(this, m_image, xform, size())
	);
	
	backgroundExecutor().enqueueTask(task);
	
	m_ptrHqTransformTask = task;
	m_hqXform = xform;
	m_hqSourceId = m_image.cacheKey();
}

/**
 * Gets called from HqTransformationTask::Result.
 */
void
ImageViewBase::hqVersionBuilt(
	QPoint const& origin, QImage const& image)
{
	if (!m_hqTransformEnabled) {
		return;
	}
	
	m_hqPixmap = QPixmap::fromImage(image);
	m_hqPixmapPos = origin;
	m_ptrHqTransformTask.reset();
	update();
}

void
ImageViewBase::updateStatusTipAndCursor()
{
	updateStatusTip();
	updateCursor();
}

void
ImageViewBase::updateStatusTip()
{
	ensureStatusTip(m_interactionState.statusTip());
}

void
ImageViewBase::updateCursor()
{
	// TODO: if possible, check if this cursor is already active.
	setCursor(m_interactionState.cursor());
}

BackgroundExecutor&
ImageViewBase::backgroundExecutor()
{
	static BackgroundExecutor executor;
	return executor;
}


/*==================== ImageViewBase::HqTransformTask ======================*/

ImageViewBase::HqTransformTask::HqTransformTask(
	ImageViewBase* image_view,
	QImage const& image, QTransform const& xform,
	QSize const& target_size)
:	m_ptrResult(new Result(image_view)),
	m_image(image),
	m_xform(xform),
	m_targetSize(target_size)
{
}

IntrusivePtr<AbstractCommand0<void> >
ImageViewBase::HqTransformTask::operator()()
{
	if (isCancelled()) {
		return IntrusivePtr<AbstractCommand0<void> >();
	}
	
	QRect const target_rect(
		m_xform.map(
			QRectF(m_image.rect())
		).boundingRect().toRect().intersected(
			QRect(QPoint(0, 0), m_targetSize)
		)
	);
	
	QImage hq_image(
		transform(
			m_image, m_xform, target_rect,
			Qt::white, true, QSizeF(0.0, 0.0)
		)
	);
#if defined(Q_WS_X11)
	// ARGB32_Premultiplied is an optimal format for X11 + XRender.
	hq_image = hq_image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
#endif
	m_ptrResult->setData(target_rect.topLeft(), hq_image);
	
	return m_ptrResult;
}


/*================ ImageViewBase::HqTransformTask::Result ================*/

ImageViewBase::HqTransformTask::Result::Result(
	ImageViewBase* image_view)
:	m_ptrImageView(image_view)
{
}

void
ImageViewBase::HqTransformTask::Result::setData(
	QPoint const& origin, QImage const& hq_image)
{
	m_hqImage = hq_image;
	m_origin = origin;
}

void
ImageViewBase::HqTransformTask::Result::operator()()
{
	if (m_ptrImageView && !isCancelled()) {
		m_ptrImageView->hqVersionBuilt(m_origin, m_hqImage);
	}
}


/*================= ImageViewBase::TempFocalPointAdjuster =================*/

ImageViewBase::TempFocalPointAdjuster::TempFocalPointAdjuster(ImageViewBase& obj)
:	m_rObj(obj),
	m_origWidgetFP(obj.getWidgetFocalPoint())
{
	obj.setWidgetFocalPointWithoutMoving(obj.centeredWidgetFocalPoint());
}

ImageViewBase::TempFocalPointAdjuster::TempFocalPointAdjuster(
	ImageViewBase& obj, QPointF const temp_widget_fp)
:	m_rObj(obj),
	m_origWidgetFP(obj.getWidgetFocalPoint())
{
	obj.setWidgetFocalPointWithoutMoving(temp_widget_fp);
}

ImageViewBase::TempFocalPointAdjuster::~TempFocalPointAdjuster()
{
	m_rObj.setWidgetFocalPointWithoutMoving(m_origWidgetFP);
}


/*================== ImageViewBase::TransformChangeWatcher ================*/

ImageViewBase::TransformChangeWatcher::TransformChangeWatcher(ImageViewBase& owner)
:	m_rOwner(owner),
	m_imageToVirtual(owner.m_imageToVirtual),
	m_virtualToWidget(owner.m_virtualToWidget)
{
	++m_rOwner.m_transformChangeWatchersActive;
}

ImageViewBase::TransformChangeWatcher::~TransformChangeWatcher()
{
	if (--m_rOwner.m_transformChangeWatchersActive == 0) {
		if (m_imageToVirtual != m_rOwner.m_imageToVirtual ||
				m_virtualToWidget != m_rOwner.m_virtualToWidget) {
			m_rOwner.transformChanged();
		}
	}
}
