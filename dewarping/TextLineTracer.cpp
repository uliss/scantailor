/*
    Scan Tailor - Interactive post-processing tool for scanned pages.
	Copyright (C)  Joseph Artsimovich <joseph.artsimovich@gmail.com>

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

#include "TextLineTracer.h"
#include "TextLineRefiner.h"
#include "DetectVertContentBounds.h"
#include "TowardsLineTracer.h"
#include "Dpi.h"
#include "TaskStatus.h"
#include "DebugImages.h"
#include "NumericTraits.h"
#include "MatrixCalc.h"
#include "VecNT.h"
#include "Grid.h"
#include "PriorityQueue.h"
#include "SidesOfLine.h"
#include "ToLineProjector.h"
#include "PolylineIntersector.h"
#include "CylindricalSurfaceDewarper.h"
#include "PerformanceTimer.h"
#include "DistortionModelBuilder.h"
#include "DistortionModel.h"
#include "Curve.h"
#include "imageproc/BinaryImage.h"
#include "imageproc/BinaryThreshold.h"
#include "imageproc/Binarize.h"
#include "imageproc/Grayscale.h"
#include "imageproc/GrayImage.h"
#include "imageproc/Scale.h"
#include "imageproc/Constants.h"
#include "imageproc/GaussBlur.h"
#include "imageproc/Morphology.h"
#include "imageproc/MorphGradientDetect.h"
#include "imageproc/RasterOp.h"
#include "imageproc/GrayRasterOp.h"
#include "imageproc/RasterOpGeneric.h"
#include "imageproc/SeedFill.h"
#include "imageproc/FindPeaksGeneric.h"
#include "imageproc/ConnectivityMap.h"
#include "imageproc/ColorForId.h"
#include <QTransform>
#include <QImage>
#include <QRect>
#include <QPainter>
#include <QBrush>
#include <QPen>
#include <QColor>
#include <QtGlobal>
#include <boost/scoped_array.hpp>
#include <boost/foreach.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
#include <boost/lambda/if.hpp>
#include <algorithm>
#include <set>
#include <map>
#include <deque>
#include <utility>
#include <stdexcept>
#include <stdlib.h>
#include <math.h>

using namespace imageproc;

namespace
{

	uint8_t darkest(uint8_t lhs, uint8_t rhs) {
		return lhs < rhs ? lhs : rhs;
	}

	uint8_t lightest(uint8_t lhs, uint8_t rhs) {
		return lhs > rhs ? lhs : rhs;
	}

	uint8_t darker(uint8_t color) {
		return color == 0 ? 0 : color - 1;
	}

}

namespace dewarping
{

class TextLineTracer::CentroidCalculator
{
public:
	CentroidCalculator() : m_sumX(0), m_sumY(0), m_numSamples(0) {}

	void processSample(int x, int y) {
		m_sumX += x;
		m_sumY += y;
		++m_numSamples;
	}

	QPoint centroid() const {
		if (m_numSamples == 0) {
			return QPoint(0, 0);
		} else {
			int const half_num_samples = m_numSamples >> 1;
			return QPoint(
				(m_sumX + half_num_samples) / m_numSamples,
				(m_sumY + half_num_samples) / m_numSamples
			);
		}
	}
private:
	int m_sumX;
	int m_sumY;
	int m_numSamples;
};

struct TextLineTracer::Region
{
	QPoint centroid;
	std::vector<RegionIdx> connectedRegions;
	bool leftmost;
	bool rightmost;

	Region() : leftmost(false), rightmost(false) {}

	Region(QPoint const& ctroid) : centroid(ctroid), leftmost(false), rightmost(false) {}
};

struct TextLineTracer::GridNode
{
	static uint32_t const INVALID_LABEL = 0;

	GridNode() : m_data() {}

	GridNode(uint8_t gray_level, uint32_t label, uint32_t finalized)
		: m_data((finalized << 31) | (label << 8) | uint32_t(gray_level)) {}

	uint8_t grayLevel() const { return static_cast<uint8_t>(m_data & 0xff); }

	void setGrayLevel(uint8_t gray_level) {
		m_data = (m_data & ~GRAY_LEVEL_MASK) | uint32_t(gray_level);
	}

	uint32_t label() const { return (m_data & LABEL_MASK) >> 8; }

	void setLabel(uint32_t label) {
		m_data = (m_data & ~LABEL_MASK) | (label << 8);
	}

	bool validLabel() const { return label() != 0; }

	bool validRegion() const { return label() != 0; }

	RegionIdx regionIdx() const { return label() - 1; }

	void setRegionIdx(RegionIdx idx) { setLabel(idx + 1); }

	uint32_t finalized() const { return (m_data & FINALIZED_MASK) >> 31; }

	void setFinalized(uint32_t finalized) {
		assert(finalized <= 1);
		m_data = (m_data & ~FINALIZED_MASK) | (finalized << 31);
	}
private:
	// Layout (MSB to LSB): [finalized: 1 bit][region idx: 23 bits][gray level: 8 bits]
	static uint32_t const GRAY_LEVEL_MASK = 0x000000FF;
	static uint32_t const LABEL_MASK      = 0x7FFFFF00;
	static uint32_t const FINALIZED_MASK  = 0x80000000;

	uint32_t m_data;
};

struct TextLineTracer::RegionGrowingPosition
{
	int gridOffset;
	uint32_t order;

	RegionGrowingPosition(int grid_offset, uint32_t ord)
		: gridOffset(grid_offset), order(ord) {}
};

class TextLineTracer::RegionGrowingQueue :
	public PriorityQueue<RegionGrowingPosition, RegionGrowingQueue>
{
public:
	RegionGrowingQueue(GridNode const* grid_data) : m_pGridData(grid_data) {}

	bool higherThan(RegionGrowingPosition const& lhs, RegionGrowingPosition const& rhs) const {
		GridNode const* lhs_node = m_pGridData + lhs.gridOffset;
		GridNode const* rhs_node = m_pGridData + rhs.gridOffset;
		if (lhs_node->grayLevel() < rhs_node->grayLevel()) {
			return true;
		} else if (lhs_node->grayLevel() == rhs_node->grayLevel()) {
			return lhs.order < rhs.order;
		} else {
			return false;
		}
	}

	void setIndex(RegionGrowingPosition, size_t) {}
private:
	GridNode const* m_pGridData;
};

/**
 * Edge is an bidirectional connection between two regions.
 * Geometrically it can be viewed as a connection between their centroids.
 * Note that centroids are calculated based on region seeds, not on full
 * region areas.
 */
struct TextLineTracer::Edge
{
	RegionIdx lesserRegionIdx;
	RegionIdx greaterRegionIdx;

	Edge(RegionIdx region_idx1, RegionIdx region_idx2) {
		if (region_idx1 < region_idx2) {
			lesserRegionIdx = region_idx1;
			greaterRegionIdx = region_idx2;
		} else {
			lesserRegionIdx = region_idx2;
			greaterRegionIdx = region_idx1;
		}
	}

	bool operator<(Edge const& rhs) const {
		if (lesserRegionIdx < rhs.lesserRegionIdx) {
			return true;
		} else if (lesserRegionIdx > rhs.lesserRegionIdx) {
			return false;
		} else {
			return greaterRegionIdx < rhs.greaterRegionIdx;
		}
	}
};

/**
 * A connection between two edges.
 */
struct TextLineTracer::EdgeConnection
{
	EdgeNodeIdx edgeNodeIdx;
	float cost;

	EdgeConnection(EdgeNodeIdx idx, float cost) : edgeNodeIdx(idx), cost(cost) {}
};

/**
 * A node in a graph that represents a connection between two regions.
 */
struct TextLineTracer::EdgeNode
{
	Edge edge;
	std::vector<EdgeConnection> connectedEdges;
	float pathCost;
	EdgeNodeIdx prevEdgeNodeIdx;
	RegionIdx leftmostRegionIdx;
	uint32_t heapIdx;

	EdgeNode(Edge const& edg) : edge(edg), pathCost(NumericTraits<float>::max()),
		prevEdgeNodeIdx(~EdgeNodeIdx(0)), leftmostRegionIdx(~RegionIdx(0)), heapIdx(~uint32_t(0)) {}
};

class TextLineTracer::ShortestPathQueue : public PriorityQueue<EdgeNodeIdx, ShortestPathQueue>
{
public:
	ShortestPathQueue(std::vector<EdgeNode>& edge_nodes) : m_rEdgeNodes(edge_nodes) {}

	bool higherThan(EdgeNodeIdx lhs, EdgeNodeIdx rhs) const {
		EdgeNode const& lhs_node = m_rEdgeNodes[lhs];
		EdgeNode const& rhs_node = m_rEdgeNodes[rhs];
		return lhs_node.pathCost < rhs_node.pathCost;
	}

	void setIndex(EdgeNodeIdx edge_node_idx, uint32_t heap_idx) {
		m_rEdgeNodes[edge_node_idx].heapIdx = heap_idx;
	}

	void reposition(EdgeNodeIdx edge_node_idx) {
		PriorityQueue<EdgeNodeIdx, ShortestPathQueue>::reposition(
			m_rEdgeNodes[edge_node_idx].heapIdx
		);
	}
private:
	std::vector<EdgeNode>& m_rEdgeNodes;
};


void
TextLineTracer::trace(
	GrayImage const& input, Dpi const& dpi, QRect const& content_rect,
	DistortionModelBuilder& output,
	TaskStatus const& status, DebugImages* dbg)
{
	using namespace boost::lambda;

	GrayImage downscaled(downscale(input, dpi));
	if (dbg) {
		dbg->add(downscaled, "downscaled");
	}

	int const downscaled_width = downscaled.width();
	int const downscaled_height = downscaled.height();

	double const downscale_x_factor = double(downscaled_width) / input.width();
	double const downscale_y_factor = double(downscaled_height) / input.height();
	QTransform to_orig;
	to_orig.scale(1.0 / downscale_x_factor, 1.0 / downscale_y_factor);

	QRect const downscaled_content_rect(to_orig.inverted().mapRect(content_rect));
	Dpi const downscaled_dpi(
		qRound(dpi.horizontal() * downscale_x_factor),
		qRound(dpi.vertical() * downscale_y_factor)
	);

	BinaryImage binarized(binarizeWolf(downscaled, QSize(31, 31)));
	if (dbg) {
		dbg->add(binarized, "binarized");
	}

	// detectVertContentBounds() is sensitive to clutter and speckles, so let's try to remove it.
	sanitizeBinaryImage(binarized, downscaled_content_rect);
	if (dbg) {
		dbg->add(binarized, "sanitized");
	}

	std::pair<QLineF, QLineF> vert_bounds(detectVertContentBounds(binarized, dbg));
	if (dbg) {
		dbg->add(visualizeVerticalBounds(binarized.toQImage(), vert_bounds), "vert_bounds");
	}

	GrayImage blurred(gaussBlur(stretchGrayRange(downscaled), 17, 5));
	if (dbg) {
		dbg->add(blurred.toQImage(), "blurred");
	}

	GrayImage eroded(erodeGray(blurred, QSize(31, 31)));
	rasterOpGeneric(
		eroded.data(), eroded.stride(), eroded.size(),
		blurred.data(), blurred.stride(),
		if_then_else(_1 > _2 + 8, _1 = uint8_t(0), _1 = uint8_t(255))
	);
	BinaryImage thick_mask(eroded);
	eroded = GrayImage();
	if (dbg) {
		dbg->add(thick_mask, "thick_mask");
	}

	std::list<std::vector<QPointF> > polylines;
	segmentBlurredTextLines(
		blurred, thick_mask, polylines,
		vert_bounds.first, vert_bounds.second, dbg
	);
	
	// Extend polylines.
	BOOST_FOREACH(std::vector<QPointF>& polyline, polylines) {
		std::deque<QPointF> growable_polyline(polyline.begin(), polyline.end());
		extendTowardsVerticalBounds(
			growable_polyline, vert_bounds, binarized, blurred, thick_mask
		);
		polyline.assign(growable_polyline.begin(), growable_polyline.end());
	}
	blurred = GrayImage(); // Save memory.
	if (dbg) {
		dbg->add(visualizePolylines(downscaled, polylines), "extended");
	}

	filterOutOfBoundsCurves(polylines, vert_bounds.first, vert_bounds.second);

	TextLineRefiner refiner(downscaled, Dpi(200, 200), dbg);
	refiner.refine(polylines, /*iterations=*/100, dbg, &downscaled.toQImage());

	filterEdgyCurves(polylines);
	if (dbg) {
		dbg->add(visualizePolylines(downscaled, polylines), "edgy_curves_removed");
	}


	// Transform back to original coordinates and output.

	vert_bounds.first = to_orig.map(vert_bounds.first);
	vert_bounds.second = to_orig.map(vert_bounds.second);
	output.setVerticalBounds(vert_bounds.first, vert_bounds.second);

	BOOST_FOREACH(std::vector<QPointF>& polyline, polylines) {
		BOOST_FOREACH(QPointF& pt, polyline) {
			pt = to_orig.map(pt);
		}
		output.addHorizontalCurve(polyline);
	}
}

GrayImage
TextLineTracer::downscale(GrayImage const& input, Dpi const& dpi)
{
	// Downscale to 200 DPI.
	QSize downscaled_size(input.size());
	if (dpi.horizontal() < 180 || dpi.horizontal() > 220 || dpi.vertical() < 180 || dpi.vertical() > 220) {
		downscaled_size.setWidth(std::max<int>(1, input.width() * 200 / dpi.horizontal()));
		downscaled_size.setHeight(std::max<int>(1, input.height() * 200 / dpi.vertical()));
	}

	return scaleToGray(input, downscaled_size);
}

void
TextLineTracer::segmentBlurredTextLines(
	GrayImage const& blurred, BinaryImage const& thick_mask,
	std::list<std::vector<QPointF> >& out, QLineF const& left_bound,
	QLineF const& right_bound, DebugImages* dbg)
{
	int const width = blurred.width();
	int const height = blurred.height();

	BinaryImage region_seeds(
		findPeaksGeneric<uint8_t>(
			&darkest, &lightest, &darker, QSize(31, 15), 255,
			blurred.data(), blurred.stride(), blurred.size()
		)
	);
	
	// We don't want peaks outside of the thick mask.
	// This mostly happens on pictures.
	region_seeds = seedFill(thick_mask, region_seeds, CONN8);

	// We really don't want two region seeds close to each other.
	// Even though the peak_neighbourhood parameter we pass to findPeaksGeneric()
	// will suppress nearby weaker peaks, but it won't suppress a nearby peak
	// if it has has exactly the same value.  Therefore, we dilate those peaks.
	// Note that closeBrick() wouldn't handle cases like:
	// x
	//    x
	region_seeds = dilateBrick(region_seeds, QSize(9, 9));
	if (dbg) {
		dbg->add(region_seeds, "region_seeds");
	}

	std::vector<Region> regions;
	initRegions(regions, region_seeds);

	std::set<Edge> edges;
	labelAndGrowRegions(
		blurred, region_seeds.release(), thick_mask, regions,
		edges, left_bound, right_bound, dbg
	);

	std::vector<EdgeNode> edge_nodes;
	std::map<Edge, uint32_t> edge_to_index;
	edge_nodes.reserve(edges.size());

	// Populate ConnComp::connectedRagions and edge_nodes.
	BOOST_FOREACH(Edge const& edge, edges) {
		edge_to_index[edge] = edge_nodes.size();
		edge_nodes.push_back(EdgeNode(edge));

		regions[edge.lesserRegionIdx].connectedRegions.push_back(edge.greaterRegionIdx);
		regions[edge.greaterRegionIdx].connectedRegions.push_back(edge.lesserRegionIdx);
	}

	float const cos_threshold = cos(15 * constants::DEG2RAD);
	float const cos_sq_threshold = cos_threshold * cos_threshold;

	uint32_t const num_regions = regions.size();

	// Populate EdgeNode::connectedEdges
	for (RegionIdx region_idx = 0; region_idx < num_regions; ++region_idx) {
		Region const& region = regions[region_idx];
		size_t const num_connected_regions = region.connectedRegions.size();
		for (size_t i = 0; i < num_connected_regions; ++i) {
			RegionIdx const region1_idx = region.connectedRegions[i];
			assert(region1_idx != region_idx);
			Edge const edge1(region_idx, region1_idx);
			uint32_t const edge1_node_idx = edge_to_index[edge1];
			EdgeNode& edge1_node = edge_nodes[edge1_node_idx];
			Vec2f const vec1(regions[region1_idx].centroid - region.centroid);

			for (size_t j = i + 1; j < num_connected_regions; ++j) {
				RegionIdx const region2_idx = region.connectedRegions[j];
				assert(region2_idx != region_idx && region2_idx != region1_idx);
				Edge const edge2(region_idx, region2_idx);
				uint32_t const edge2_node_idx = edge_to_index[edge2];
				EdgeNode& edge2_node = edge_nodes[edge2_node_idx];
				Vec2f const vec2(regions[region2_idx].centroid - region.centroid);
				
				float const dot = vec1.dot(vec2);
				float const cos_sq = (fabs(dot) * -dot) / (vec1.squaredNorm() * vec2.squaredNorm());
				float const cost = std::max<float>(1.0f - cos_sq, 0);

				if (cos_sq > cos_sq_threshold) {
					edge1_node.connectedEdges.push_back(EdgeConnection(edge2_node_idx, cost));
					edge2_node.connectedEdges.push_back(EdgeConnection(edge1_node_idx, cost));
				}
			}
		}
	}
	
	ShortestPathQueue queue(edge_nodes);
	uint32_t const num_edge_nodes = edge_nodes.size();

	// Put leftmost nodes in the queue with the path cost of zero.
	for (uint32_t edge_node_idx = 0; edge_node_idx < num_edge_nodes; ++edge_node_idx) {
		EdgeNode& edge_node = edge_nodes[edge_node_idx];
		RegionIdx const region1_idx = edge_node.edge.lesserRegionIdx;
		RegionIdx const region2_idx = edge_node.edge.greaterRegionIdx;

		if (regions[region1_idx].leftmost) {
			edge_node.pathCost = 0;
			edge_node.leftmostRegionIdx = region1_idx;
			queue.push(edge_node_idx);
		} else if (regions[region2_idx].leftmost) {
			edge_node.pathCost = 0;
			edge_node.leftmostRegionIdx = region2_idx;
			queue.push(edge_node_idx);
		}
	}

	while (!queue.empty()) {
		uint32_t const edge_node_idx = queue.front();
		queue.pop();

		EdgeNode& edge_node = edge_nodes[edge_node_idx];
		edge_node.heapIdx = ~uint32_t(0);

		BOOST_FOREACH(EdgeConnection const& connection, edge_node.connectedEdges) {
			EdgeNode& edge_node2 = edge_nodes[connection.edgeNodeIdx];
			float const new_path_cost = std::max<float>(
				edge_node.pathCost, connection.cost
			) + 0.001 * connection.cost;
			if (new_path_cost < edge_node2.pathCost) {
				edge_node2.pathCost = new_path_cost;
				edge_node2.prevEdgeNodeIdx = edge_node_idx;
				edge_node2.leftmostRegionIdx = edge_node.leftmostRegionIdx;
				if (edge_node2.heapIdx == ~uint32_t(0)) {
					queue.push(connection.edgeNodeIdx);
				} else {
					queue.reposition(connection.edgeNodeIdx);
				}
			}
		}
	}

	std::vector<std::vector<EdgeNodeIdx> > edge_node_paths;
	extractEdegeNodePaths(edge_node_paths, edge_nodes, regions);

	// Visualize refined graph.
	if (dbg) {
		QImage canvas(blurred.toQImage().convertToFormat(QImage::Format_ARGB32_Premultiplied));
		{
			QPainter painter(&canvas);
			
			// Visualize connections.
			painter.setRenderHint(QPainter::Antialiasing);
			QPen pen(Qt::blue);
			pen.setWidthF(2.0);
			painter.setPen(pen);
			BOOST_FOREACH(std::vector<uint32_t> const& path, edge_node_paths) {
				BOOST_FOREACH(uint32_t const edge_node_idx, path) {
					Edge const& edge = edge_nodes[edge_node_idx].edge;
					painter.drawLine(
						regions[edge.lesserRegionIdx].centroid,
						regions[edge.greaterRegionIdx].centroid
					);
				}
			}

			// Visualize peaks.
			painter.setPen(Qt::NoPen);
			QColor color;
			BOOST_FOREACH(Region const& region, regions) {
				if (region.leftmost && region.rightmost) {
					color = Qt::green;
				} else if (region.leftmost) {
					color = Qt::magenta;
				} else if (region.rightmost) {
					color = Qt::cyan;
				} else {
					color = Qt::yellow;
				}
				painter.setBrush(color);
				QRectF rect(0, 0, 15, 15);
				rect.moveCenter(region.centroid);
				painter.drawEllipse(rect);
			}
		}
		dbg->add(canvas, "refined_graph");
	}

	edgeSequencesToPolylines(edge_node_paths, edge_nodes, regions, out);
}

void
TextLineTracer::labelAndGrowRegions(
	GrayImage const& blurred, BinaryImage region_seeds,
	BinaryImage const& thick_mask, std::vector<Region>& regions,
	std::set<Edge>& edges, QLineF const& left_bound, QLineF const& right_bound, DebugImages* dbg)
{
	int const width = blurred.width();
	int const height = blurred.height();

	Grid<GridNode> grid(width, height, /*padding=*/1);
	grid.initPadding(GridNode(0, 0, 1));
	// Interior initialized with GridNode() is OK with us.

	GridNode* const grid_data = grid.data();
	GridNode* grid_line = grid_data;
	int const grid_stride = grid.stride();
	
	uint8_t const* blurred_line = blurred.data();
	int const blurred_stride = blurred.stride();

	uint32_t const* thick_mask_line = thick_mask.data();
	int const thick_mask_stride = thick_mask.wordsPerLine();

	// Copy gray level from blurred into the grid and mark
	// areas outside of thick_mask as finalized.
	int grid_offset = 0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			GridNode* node = grid_line + x;
			node->setGrayLevel(blurred_line[x]);
			node->setFinalized(~(thick_mask_line[x >> 5] >> (31 - (x & 31))) & uint32_t(1));
		}
		grid_line += grid_stride;
		blurred_line += blurred_stride;
		thick_mask_line += thick_mask_stride;
	}

	RegionGrowingQueue queue(grid.data());

	// Put region centroids into the queue.
	RegionIdx const num_regions = regions.size();
	for (RegionIdx region_idx = 0; region_idx < num_regions; ++region_idx) {
		Region const& region = regions[region_idx];
		int const grid_offset = grid_stride * region.centroid.y() + region.centroid.x();
		GridNode* node = grid_data + grid_offset;
		node->setRegionIdx(region_idx);
		node->setFinalized(1);
		queue.push(RegionGrowingPosition(grid_offset, 0));
	}

	int const nbh_offsets[] = { -grid_stride, -1, 1, grid_stride };
	QPoint const nbh_vectors[] = { QPoint(0, -1), QPoint(-1, 0), QPoint(1, 0), QPoint(0, 1) };

	// Grow regions, but only within thick_mask.
	uint32_t iteration = 0;
	while (!queue.empty()) {
		++iteration;

		int const offset = queue.front().gridOffset;
		queue.pop();

		GridNode const* node = grid_data + offset;
		uint32_t const label = node->label();

		// Spread this value to 4-connected neighbours.
		for (int i = 0; i < 4; ++i) {
			int const nbh_offset = offset + nbh_offsets[i];
			GridNode* nbh = grid_data + nbh_offset;
			if (!nbh->finalized()) {
				nbh->setFinalized(1);
				nbh->setLabel(label);
				queue.push(RegionGrowingPosition(nbh_offset, iteration));
			}
		}
	}

	distanceDrivenRegionGrowth(grid);

	// Mark regions as leftmost / rightmost.
	markEdgeRegions(regions, grid, left_bound, right_bound);
	
	// Process horizontal connections between regions.
	grid_line = grid_data;
	thick_mask_line = thick_mask.data();
	for (int y = 0; y < height; ++y) {
		for (int x = 1; x < width; ++x) {
			uint32_t const mask1 = thick_mask_line[x >> 5] >> (31 - (x & 31));
			uint32_t const mask2 = thick_mask_line[(x - 1) >> 5] >> (31 - ((x - 1) & 31));
			if (mask1 & mask2 & 1) {
				GridNode const* node1 = grid_line + x;
				GridNode const* node2 = node1 - 1;
				if (node1->regionIdx() != node2->regionIdx() && node1->validRegion() && node2->validRegion()) {
					edges.insert(Edge(node1->regionIdx(), node2->regionIdx()));
				}
			}
		}

		grid_line += grid_stride;
		thick_mask_line += thick_mask_stride;
	}

	uint32_t const msb = uint32_t(1) << 31;

	// Process vertical connections between regions.
	grid_line = grid_data;
	thick_mask_line = thick_mask.data();
	for (int x = 0; x < width; ++x) {
		grid_line = grid.data() + x;
		uint32_t const* mask_word = thick_mask_line + (x >> 5);
		uint32_t const mask = msb >> (x & 31);
		
		for (int y = 1; y < height; ++y) {
			grid_line += grid_stride;
			mask_word += thick_mask_stride;

			if (mask_word[0] & mask_word[-thick_mask_stride] & mask) {
				GridNode const* node1 = grid_line;
				GridNode const* node2 = grid_line - grid_stride;
				if (node1->regionIdx() != node2->regionIdx() && node1->validRegion() && node2->validRegion()) {
					edges.insert(Edge(node1->regionIdx(), node2->regionIdx()));
				}
			}
		}
	}

	if (dbg) {
		// Visualize regions and seeds.
		QImage visualized_regions(
			visualizeRegions(grid).convertToFormat(QImage::Format_ARGB32_Premultiplied)
		);

		QImage canvas(visualized_regions);
		{
			QPainter painter(&canvas);

			painter.setOpacity(0.7);
			painter.drawImage(0, 0, blurred);

			painter.setOpacity(1.0);
			painter.drawImage(0, 0, region_seeds.toAlphaMask(Qt::blue));
		}
		dbg->add(canvas, "regions");

		// Visualize region connectivity.
		canvas = visualized_regions;
		visualized_regions = QImage();
		{
			QPainter painter(&canvas);
			painter.setOpacity(0.3);
			painter.drawImage(0, 0, thick_mask.toQImage());
			
			// Visualize connections.
			painter.setOpacity(1.0);
			painter.setRenderHint(QPainter::Antialiasing);
			QPen pen(Qt::blue);
			pen.setWidthF(2.0);
			painter.setPen(pen);
			BOOST_FOREACH(Edge const& edge, edges) {
				painter.drawLine(
					regions[edge.lesserRegionIdx].centroid,
					regions[edge.greaterRegionIdx].centroid
				);
			}

			// Visualize nodes.
			painter.setPen(Qt::NoPen);
			QColor color;
			BOOST_FOREACH(Region const& region, regions) {
				if (region.leftmost && region.rightmost) {
					color = Qt::green;
				} else if (region.leftmost) {
					color = Qt::magenta;
				} else if (region.rightmost) {
					color = Qt::cyan;
				} else {
					color = Qt::yellow;
				}
				painter.setBrush(color);
				QRectF rect(0, 0, 15, 15);
				rect.moveCenter(region.centroid);
				painter.drawEllipse(rect);
			}
		}
		dbg->add(canvas, "connectivity");
	}
}

void
TextLineTracer::initRegions(std::vector<Region>& regions, imageproc::BinaryImage const& region_seeds)
{
	int const width = region_seeds.width();
	int const height = region_seeds.height();

	ConnectivityMap cmap(region_seeds, CONN8);

	// maxLabel() instead of maxLabel() + 1 because label 0 won't be used.
	std::vector<CentroidCalculator> centroid_calculators(cmap.maxLabel());

	// Calculate centroids.
	int const cmap_stride = cmap.stride();
	uint32_t* cmap_line = cmap.paddedData();
	for (int y = 0; y < height; ++y, cmap_line += cmap_stride) {
		for (int x = 0; x < width; ++x) {
			uint32_t const label = cmap_line[x];
			if (label) {
				centroid_calculators[label - 1].processSample(x, y);
			}
		}
	}

	uint32_t const num_regions = centroid_calculators.size();
	regions.reserve(centroid_calculators.size());
	
	BOOST_FOREACH(CentroidCalculator const& calc, centroid_calculators) {
		regions.push_back(Region(calc.centroid()));
	}
}

namespace
{

struct ProximityRegion
{
	int xOrigin;
	int xMaybeLeader; // The point where this region may become the closest one.
};

}

void
TextLineTracer::distanceDrivenRegionGrowth(Grid<GridNode>& region_grid)
{
	// Here we are using the following distance transform algorithm:
	// Meijster, A., Roerdink, J., and Hesselink, W. 2000.
	// A general algorithm for computing distance transforms in linear time.
	// http://dissertations.ub.rug.nl/FILES/faculties/science/2004/a.meijster/c2.pdf

	int const width = region_grid.width();
	int const height = region_grid.height();
	
	GridNode* const region_data = region_grid.data();
	int const region_stride = region_grid.stride();

	Grid<uint32_t> sqdist_grid(width, height, /*padding=*/0);
	uint32_t* const sqdist_data = sqdist_grid.data();
	int const sqdist_stride = sqdist_grid.stride();

	// We pretend the vertical distances are greater than they are.
	// This gives horizontal growing a preference.
	static const uint32_t vert_scale = 3;
	static uint32_t const INF_SQDIST = ~uint32_t(0);

	// Horizontal pass.
	// For each node, calculate the scaled distance to the closest
	// point in the same column, that already belongs to a region.
	for (int x = 0; x < width; ++x) {
		GridNode* p_region = region_data + x;
		uint32_t* p_sqdist = sqdist_data + x;

		// Go down up to the first valid region.
		int y = 0;
		for (; y < height && !p_region->validRegion(); ++y) {
			*p_sqdist = INF_SQDIST;
			p_region += region_stride;
			p_sqdist += sqdist_stride;
		}
		if (y == height) {
			continue;
		}

		uint32_t vs_plus_2dvs = vert_scale; // (vert_scale + 2 * real_vert_dist * vert_scale)
		uint32_t dvs_squared = 0; // (real_vert_dist * vert_scale)^2
		uint32_t closest_label = 0;

		// Continue going down.
		for (; y < height; ++y) {
			if (p_region->validRegion()) {
				*p_sqdist = 0;
				dvs_squared = 0;
				vs_plus_2dvs = vert_scale;
				closest_label = p_region->label();
			} else {
				// vs + 2*(d*vs + vs) = 2*vs + (vs + 2*d*vs)
				vs_plus_2dvs += 2*vert_scale;
				
				// (d*vs + vs)^2 = (d*vs)^2 + 2*d*vs*vs + vs*vs = (d*vs)^2 + vs*(vs + 2*d*vs)
				dvs_squared += vert_scale * vs_plus_2dvs;
				*p_sqdist = dvs_squared;
				p_region->setLabel(closest_label);
			}

			p_region += region_stride;
			p_sqdist += sqdist_stride;
		}

		--y;
		p_region -= region_stride;
		p_sqdist -= sqdist_stride;

		// Go back up to the first valid region.
		for (; y >= 0 && *p_sqdist != 0; --y) {
			p_region -= region_stride;
			p_sqdist -= sqdist_stride;
		}

		for (; y >= 0; --y) {
			if (*p_sqdist == 0) {
				dvs_squared = 0;
				vs_plus_2dvs = vert_scale;
				closest_label = p_region->label();
			} else {
				// vs + 2*(d*vs + vs) = 2*vs + (vs + 2*d*vs)
				vs_plus_2dvs += 2*vert_scale;
				
				// (d*vs + vs)^2 = (d*vs)^2 + 2*d*vs*vs + vs*vs = (d*vs)^2 + vs*(vs + 2*d*vs)
				dvs_squared += vert_scale * vs_plus_2dvs;
				if (dvs_squared < *p_sqdist) {
					*p_sqdist = dvs_squared;
					p_region->setLabel(closest_label);
				}
			}

			p_region -= region_stride;
			p_sqdist -= sqdist_stride;
		}
	}

	class SqDistCalc
	{
	public:
		SqDistCalc(uint32_t const* vert_sqdists) : m_vertSqdists(vert_sqdists) {}

		uint32_t operator()(int elevated_x, int ground_x) const {
			uint32_t const dx = elevated_x - ground_x;
			return dx*dx + m_vertSqdists[elevated_x];
		}
	private:
		uint32_t const* m_vertSqdists;
	};

	boost::scoped_array<uint32_t> orig_labels(new uint32_t[width]);
	boost::scoped_array<ProximityRegion> prx_regs(new ProximityRegion[width]);
	
	// Horizontal pass.
	GridNode* region_line = region_data;
	uint32_t* sqdist_line = sqdist_data;
	for (int y = 0; y < height; ++y) {
		SqDistCalc const sqdist(sqdist_line);

		ProximityRegion* next_reg = prx_regs.get();
		next_reg->xOrigin = 0;
		next_reg->xMaybeLeader = 0;

		for (int x = 1; x < width; ++x) {
			while (sqdist_line[next_reg->xOrigin] == INF_SQDIST || (sqdist_line[x] != INF_SQDIST &&
				sqdist(next_reg->xOrigin, next_reg->xMaybeLeader) > sqdist(x, next_reg->xMaybeLeader))) {
				
				// This means next_reg will never win over a ProximityRegion with xOrigin == x
				// and therefore can be discarded.
				
				if (next_reg != prx_regs.get()) {
					--next_reg;
				} else {
					next_reg->xOrigin = x;
					break;
				}
			}

			int const next_x = next_reg->xOrigin;
			if (x != next_x) {
				if (sqdist_line[x] != INF_SQDIST) {
					// Calculate where a ProximityRegion with xOrigin at x will
					// take over next_reg.  Note that it can't turn out it's
					// already taken over, as that's handled by the loop above.
					int x_take_over = 0;
					
					if (sqdist_line[next_x] != INF_SQDIST) {
						x_take_over = x * x - next_x * next_x + sqdist_line[x] - sqdist_line[next_x];
						x_take_over /= (x - next_x) * 2;
						++x_take_over;
					}
					
					if ((unsigned)x_take_over < (unsigned)width) {
						// The above if also handles the case when x_take_over < 0.
						++next_reg;
						next_reg->xOrigin = x;
						next_reg->xMaybeLeader = x_take_over;
					}
				}
			}
		}
		
		// Create a copy of labels for this line.
		for (int x = 0; x < width; ++x) {
			orig_labels[x] = region_line[x].label();
		}
		
		for (int x = width - 1; x >= 0; --x) {
			assert(next_reg->xOrigin >= 0 && next_reg->xOrigin < width);
			region_line[x].setLabel(orig_labels[next_reg->xOrigin]);
			if (next_reg->xMaybeLeader == x) {
				--next_reg;
			}
		}

		region_line += region_stride;
		sqdist_line += sqdist_stride;
	}
}


/**
 * Goes along the vertical bounds and marks regions they pass through
 * as leftmost or rightmost (could even be both).
 */
void
TextLineTracer::markEdgeRegions(
	std::vector<Region>& regions, Grid<GridNode> const& grid,
	QLineF const& left_bound, QLineF const& right_bound)
{
	int const width = grid.width();
	int const height = grid.height();

	GridNode const* grid_line = grid.data();
	int const grid_stride = grid.stride();

	for (int y = 0; y < height; ++y, grid_line += grid_stride) {
		QLineF const hor_line(QPointF(0, y), QPointF(width, y));
		
		int left_x = 0;
		QPointF left_intersection;
		if (hor_line.intersect(left_bound, &left_intersection) != QLineF::NoIntersection) {
			left_x = qBound<int>(0, qRound(left_intersection.x()), width - 1);
		}
		GridNode const& left_node = grid_line[left_x];
		if (left_node.validRegion()) {
			regions[left_node.regionIdx()].leftmost = true;
		}

		int right_x = width - 1;
		QPointF right_intersection;
		if (hor_line.intersect(right_bound, &right_intersection) != QLineF::NoIntersection) {
			right_x = qBound<int>(0, qRound(right_intersection.x()), width - 1);
		}
		GridNode const& right_node = grid_line[right_x];
		if (right_node.validRegion()) {
			regions[right_node.regionIdx()].rightmost = true;
		}
	}
}

void
TextLineTracer::extractEdegeNodePaths(
	std::vector<std::vector<uint32_t> >& edge_node_paths,
	std::vector<EdgeNode> const& edge_nodes,
	std::vector<Region> const& regions)
{
	uint32_t const num_edge_nodes = edge_nodes.size();

	std::map<RegionIdx, EdgeNodeIdx> best_incoming_paths; // rightmost region -> rightmost EdgeNode index
	
	for (uint32_t rightmost_edge_node_idx = 0; rightmost_edge_node_idx < num_edge_nodes; ++rightmost_edge_node_idx) {
		EdgeNode const& edge_node = edge_nodes[rightmost_edge_node_idx];
		
		uint32_t rightmost_region_idx;

		if (regions[edge_node.edge.lesserRegionIdx].rightmost) {
			rightmost_region_idx = edge_node.edge.lesserRegionIdx;
		} else if (regions[edge_node.edge.greaterRegionIdx].rightmost) {
			rightmost_region_idx = edge_node.edge.greaterRegionIdx;
		} else {
			continue;
		}

		uint32_t const leftmost_region_idx = edge_node.leftmostRegionIdx;
		if (leftmost_region_idx == ~RegionIdx(0)) {
			// No path reached this node.
			continue; 
		}

		std::map<RegionIdx, EdgeNodeIdx>::iterator it(best_incoming_paths.find(rightmost_region_idx));
		if (it == best_incoming_paths.end()) {
			best_incoming_paths[rightmost_region_idx] = rightmost_edge_node_idx;
		} else {
			float const old_cost = edge_nodes[it->second].pathCost;
			float const new_cost = edge_nodes[rightmost_edge_node_idx].pathCost;
			if (new_cost < old_cost) {
				it->second = rightmost_edge_node_idx;
			}
		}
	}

	std::map<RegionIdx, EdgeNodeIdx> best_outgoing_paths; // leftmost region -> rightmost EdgeNode index
	
	typedef std::map<RegionIdx, EdgeNodeIdx>::value_type KV;
	BOOST_FOREACH(KV const& kv, best_incoming_paths) {
		uint32_t const leftmost_region_idx = edge_nodes[kv.second].leftmostRegionIdx;
		uint32_t const rightmost_edge_node_idx = kv.second;

		std::map<RegionIdx, EdgeNodeIdx>::iterator it(
			best_outgoing_paths.find(leftmost_region_idx)
		);
		if (it == best_outgoing_paths.end()) {
			best_outgoing_paths[leftmost_region_idx] = rightmost_edge_node_idx;
		} else {
			float const existing_cost = edge_nodes[it->second].pathCost;
			float const new_cost = edge_nodes[rightmost_edge_node_idx].pathCost;
			if (new_cost < existing_cost) {
				it->second = rightmost_edge_node_idx;
			}
		}
	}
	
	// Follow by EdgeNode::prevEdgeNode from rightmost edges to leftmost ones.
	typedef std::map<RegionIdx, EdgeNodeIdx>::value_type LR;
	BOOST_FOREACH (LR const& lr, best_outgoing_paths) {
	
		edge_node_paths.push_back(std::vector<uint32_t>());
		std::vector<uint32_t>& path = edge_node_paths.back();
		
		uint32_t edge_node_idx = lr.second;
		for (;;) {
			path.push_back(edge_node_idx);

			EdgeNode const& edge_node = edge_nodes[edge_node_idx];
			if (edge_node.edge.lesserRegionIdx == lr.first || edge_node.edge.greaterRegionIdx == lr.first) {
				break; // We are done!
			}

			edge_node_idx = edge_node.prevEdgeNodeIdx;
		}
	}
}

void
TextLineTracer::edgeSequencesToPolylines(
	std::vector<std::vector<EdgeNodeIdx> > const& edge_node_paths,
	std::vector<EdgeNode> const& edge_nodes, std::vector<Region> const& regions,
	std::list<std::vector<QPointF> >& polylines)
{
	BOOST_FOREACH(std::vector<EdgeNodeIdx> const& edge_node_path, edge_node_paths) {
		if (edge_node_path.empty()) {
			continue;
		}
		
		polylines.push_back(std::vector<QPointF>());
		std::vector<QPointF>& polyline = polylines.back();

		if (edge_node_path.size() == 1) {
			Edge const& edge = edge_nodes[edge_node_path.front()].edge;
			polyline.push_back(regions[edge.lesserRegionIdx].centroid);
			polyline.push_back(regions[edge.greaterRegionIdx].centroid);
			continue;
		}

		std::vector<RegionIdx> region_indexes;

		// This one will be written later.
		region_indexes.push_back(0);

		std::vector<EdgeNodeIdx>::const_iterator it(edge_node_path.begin());
		std::vector<EdgeNodeIdx>::const_iterator const end(edge_node_path.end());
		for (;;) {
			EdgeNodeIdx const edge_node1_idx = *it;
			++it;
			if (it == end) {
				break;
			}
			EdgeNodeIdx const edge_node2_idx = *it;
			
			RegionIdx const connecting_region_idx = findConnectingRegion(
				edge_nodes[edge_node1_idx].edge, edge_nodes[edge_node2_idx].edge
			);
			assert(connecting_region_idx != ~RegionIdx(0));

			region_indexes.push_back(connecting_region_idx);
		}

		Edge const& first_edge = edge_nodes[edge_node_path.front()].edge;
		if (first_edge.lesserRegionIdx == region_indexes[1]) {
			region_indexes[0] = first_edge.greaterRegionIdx;
		} else {
			region_indexes[0] = first_edge.lesserRegionIdx;
		}

		Edge const& last_edge = edge_nodes[edge_node_path.back()].edge;
		if (last_edge.lesserRegionIdx == region_indexes.back()) {
			region_indexes.push_back(last_edge.greaterRegionIdx);
		} else {
			region_indexes.push_back(last_edge.lesserRegionIdx);
		}

		BOOST_FOREACH(RegionIdx region_idx, region_indexes) {
			polyline.push_back(regions[region_idx].centroid);
		}
	}
}

TextLineTracer::RegionIdx
TextLineTracer::findConnectingRegion(Edge const& edge1, Edge const& edge2)
{
	RegionIdx const edge1_regions[] = { edge1.lesserRegionIdx, edge1.greaterRegionIdx };
	RegionIdx const edge2_regions[] = { edge2.lesserRegionIdx, edge2.greaterRegionIdx };

	BOOST_FOREACH(RegionIdx idx1, edge1_regions) {
		BOOST_FOREACH(RegionIdx idx2, edge2_regions) {
			if (idx1 == idx2) {
				return idx1;
			}
		}
	}

	return ~RegionIdx(0);
}

void
TextLineTracer::sanitizeBinaryImage(BinaryImage& image, QRect const& content_rect)
{
	// Kill connected components touching the borders.
	BinaryImage seed(image.size(), WHITE);
	seed.fillExcept(seed.rect().adjusted(1, 1, -1, -1), BLACK);

	BinaryImage touching_border(seedFill(seed.release(), image, CONN8));
	rasterOp<RopSubtract<RopDst, RopSrc> >(image, touching_border.release());

	// Poor man's despeckle.
	BinaryImage content_seeds(openBrick(image, QSize(2, 3), WHITE));
	rasterOp<RopOr<RopSrc, RopDst> >(content_seeds, openBrick(image, QSize(3, 2), WHITE));
	image = seedFill(content_seeds.release(), image, CONN8);

	// Clear margins.
	image.fillExcept(content_rect, WHITE);
}

void
TextLineTracer::extendTowardsVerticalBounds(
	std::deque<QPointF>& polyline, std::pair<QLineF, QLineF> vert_bounds,
	BinaryImage const& content, GrayImage const& blurred, BinaryImage const& thick_mask)
{
	if (polyline.empty()) {
		return;
	}

	// Maybe swap vert_bounds.first and vert_bounds.second.
	{
		ToLineProjector const proj1(vert_bounds.first);
		ToLineProjector const proj2(vert_bounds.second);
		if (proj1.projectionDist(polyline.front()) + proj2.projectionDist(polyline.back()) >
				proj1.projectionDist(polyline.back()) + proj2.projectionDist(polyline.front())) {
			std::swap(vert_bounds.first, vert_bounds.second);
		}
	}

	// Because we know our images are about 200 DPI (because we
	// downscale them), we can use a constant here.
	qreal const max_dist = 30;

	// Extend the head of our polyline.
	{
		TowardsLineTracer tracer(
			content, blurred, thick_mask, vert_bounds.first, polyline.front().toPoint()
		);
		for (QPoint const* pt; (pt = tracer.trace(max_dist)); ) {
			polyline.push_front(*pt);
		}
	}

	// Extend the tail of our polyline.
	{
		TowardsLineTracer tracer(
			content, blurred, thick_mask, vert_bounds.second, polyline.back().toPoint()
		);
		for (QPoint const* pt; (pt = tracer.trace(max_dist)); ) {
			polyline.push_back(*pt);
		}
	}
}

/**
 * Returns false if the curve contains both significant convexities and concavities.
 */
bool
TextLineTracer::isCurvatureConsistent(std::vector<QPointF> const& polyline)
{
	size_t const num_nodes = polyline.size();
	
	if (num_nodes <= 1) {
		// Even though we can't say anything about curvature in this case,
		// we don't like such gegenerate curves, so we reject them.
		return false;
	} else if (num_nodes == 2) {
		// These are fine.
		return true;
	}

	// Threshold angle between a polyline segment and a normal to the previous one.
	float const cos_threshold = cos((90.0f - 6.0f) * constants::DEG2RAD);
	float const cos_sq_threshold = cos_threshold * cos_threshold;
	bool significant_positive = false;
	bool significant_negative = false;

	Vec2f prev_normal(polyline[1] - polyline[0]);
	std::swap(prev_normal[0], prev_normal[1]);
	prev_normal[0] = -prev_normal[0];
	float prev_normal_sqlen = prev_normal.squaredNorm();

	for (size_t i = 1; i < num_nodes - 1; ++i) {
		Vec2f const next_segment(polyline[i + 1] - polyline[i]);
		float const next_segment_sqlen = next_segment.squaredNorm();

		float cos_sq = 0;
		float const sqlen_mult = prev_normal_sqlen * next_segment_sqlen;
		if (sqlen_mult > std::numeric_limits<float>::epsilon()) {
			float const dot = prev_normal.dot(next_segment);
			cos_sq = fabs(dot) * dot / sqlen_mult;
		}

		if (fabs(cos_sq) >= cos_sq_threshold) {
			if (cos_sq > 0) {
				significant_positive = true;
			} else {
				significant_negative = true;
			}
		}

		prev_normal[0] = -next_segment[1];
		prev_normal[1] = next_segment[0];
		prev_normal_sqlen = next_segment_sqlen;
	}

	return !(significant_positive && significant_positive);
}

bool
TextLineTracer::isInsideBounds(
	QPointF const& pt, QLineF const& left_bound, QLineF const& right_bound)
{
	QPointF left_normal_inside(left_bound.normalVector().p2() - left_bound.p1());
	if (left_normal_inside.x() < 0) {
		left_normal_inside = -left_normal_inside;
	}
	QPointF const left_vec(pt - left_bound.p1());
	if (left_normal_inside.x() * left_vec.x() + left_normal_inside.y() * left_vec.y() < 0) {
		return false;
	}

	QPointF right_normal_inside(right_bound.normalVector().p2() - right_bound.p1());
	if (right_normal_inside.x() > 0) {
		right_normal_inside = -right_normal_inside;
	}
	QPointF const right_vec(pt - right_bound.p1());
	if (right_normal_inside.x() * right_vec.x() + right_normal_inside.y() * right_vec.y() < 0) {
		return false;
	}

	return true;
}

void
TextLineTracer::filterOutOfBoundsCurves(
	std::list<std::vector<QPointF> >& polylines,
	QLineF const& left_bound, QLineF const& right_bound)
{
	std::list<std::vector<QPointF> >::iterator it(polylines.begin());
	std::list<std::vector<QPointF> >::iterator const end(polylines.end());	
	while (it != end) {
		if (!isInsideBounds(it->front(), left_bound, right_bound) &&
			!isInsideBounds(it->back(), left_bound, right_bound)) {
			polylines.erase(it++);
		} else {
			++it;
		}
	}
}

void
TextLineTracer::filterEdgyCurves(std::list<std::vector<QPointF> >& polylines)
{
	std::list<std::vector<QPointF> >::iterator it(polylines.begin());
	std::list<std::vector<QPointF> >::iterator const end(polylines.end());	
	while (it != end) {
		if (!isCurvatureConsistent(*it)) {
			polylines.erase(it++);
		} else {
			++it;
		}
	}
}

QImage
TextLineTracer::visualizeVerticalBounds(
	QImage const& background, std::pair<QLineF, QLineF> const& bounds)
{
	QImage canvas(background.convertToFormat(QImage::Format_RGB32));

	QPainter painter(&canvas);
	painter.setRenderHint(QPainter::Antialiasing);
	QPen pen(Qt::blue);
	pen.setWidthF(2.0);
	painter.setPen(pen);
	painter.setOpacity(0.7);

	painter.drawLine(bounds.first);
	painter.drawLine(bounds.second);

	return canvas;
}

QImage
TextLineTracer::visualizeRegions(Grid<GridNode> const& grid)
{
	int const width = grid.width();
	int const height = grid.height();

	GridNode const* grid_line = grid.data();
	int const grid_stride = grid.stride();

	QImage canvas(width, height, QImage::Format_ARGB32_Premultiplied);
	uint32_t* canvas_line = (uint32_t*)canvas.bits();
	int const canvas_stride = canvas.bytesPerLine() / 4;

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			uint32_t const label = grid_line[x].label();
			if (label == GridNode::INVALID_LABEL) {
				canvas_line[x] = 0; // transparent
			} else {
				canvas_line[x] = colorForId(label).rgba();
			}
		}
		grid_line += grid_stride;
		canvas_line += canvas_stride;
	}

	return canvas;
}

QImage
TextLineTracer::visualizePolylines(
	QImage const& background, std::list<std::vector<QPointF> > const& polylines,
	std::pair<QLineF, QLineF> const* vert_bounds)
{
	QImage canvas(background.convertToFormat(QImage::Format_ARGB32_Premultiplied));
	QPainter painter(&canvas);
	painter.setRenderHint(QPainter::Antialiasing);
	QPen pen(Qt::blue);
	pen.setWidthF(3.0);
	painter.setPen(pen);

	BOOST_FOREACH(std::vector<QPointF> const& polyline, polylines) {
		if (!polyline.empty()) {
			painter.drawPolyline(&polyline[0], polyline.size());
		}
	}

	if (vert_bounds) {
		painter.drawLine(vert_bounds->first);
		painter.drawLine(vert_bounds->second);
	}

	return canvas;
}

} // namespace dewarping
