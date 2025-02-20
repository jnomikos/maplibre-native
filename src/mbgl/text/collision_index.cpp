#include <mbgl/text/collision_index.hpp>
#include <mbgl/layout/symbol_instance.hpp>
#include <mbgl/geometry/feature_index.hpp>
#include <mbgl/math/log2.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/math.hpp>
#include <mbgl/math/minmax.hpp>
#include <mbgl/util/intersection_tests.hpp>
#include <mbgl/layout/symbol_projection.hpp>

#include <mapbox/geometry/envelope.hpp>

#include <mbgl/renderer/buckets/symbol_bucket.hpp> // For PlacedSymbol: pull out to another location

#include <cmath>

namespace mbgl {

namespace {
// When a symbol crosses the edge that causes it to be included in
// collision detection, it will cause changes in the symbols around
// it. This constant specifies how many pixels to pad the edge of
// the viewport for collision detection so that the bulk of the changes
// occur offscreen. Making this constant greater increases label
// stability, but it's expensive.
const float viewportPaddingDefault = 100;
// Viewport padding must be much larger for static tiles to avoid clipped labels.
const float viewportPaddingForStaticTiles = 1024;

inline float findViewportPadding(const TransformState& transformState, MapMode mapMode) {
    if (mapMode == MapMode::Tile) return viewportPaddingForStaticTiles;
    return (transformState.getPitch() != 0.0f) ? viewportPaddingDefault * 2 : viewportPaddingDefault;
}

} // namespace

CollisionIndex::CollisionIndex(const TransformState& transformState_, MapMode mapMode)
    : transformState(transformState_),
      viewportPadding(findViewportPadding(transformState_, mapMode)),
      collisionGrid(transformState.getSize().width + 2 * viewportPadding,
                    transformState.getSize().height + 2 * viewportPadding,
                    25),
      ignoredGrid(transformState.getSize().width + 2 * viewportPadding,
                  transformState.getSize().height + 2 * viewportPadding,
                  25),
      screenRightBoundary(transformState.getSize().width + viewportPadding),
      screenBottomBoundary(transformState.getSize().height + viewportPadding),
      gridRightBoundary(transformState.getSize().width + 2 * viewportPadding),
      gridBottomBoundary(transformState.getSize().height + 2 * viewportPadding),
      pitchFactor(
          static_cast<float>(std::cos(transformState.getPitch()) * transformState.getCameraToCenterDistance())) {}

float CollisionIndex::approximateTileDistance(const TileDistance& tileDistance,
                                              const float lastSegmentAngle,
                                              const float pixelsToTileUnits,
                                              const float cameraToAnchorDistance,
                                              const bool pitchWithMap) {
    // This is a quick and dirty solution for chosing which collision circles to use (since collision circles are
    // laid out in tile units). Ideally, I think we should generate collision circles on the fly in viewport coordinates
    // at the time we do collision detection.

    // incidenceStretch is the ratio of how much y space a label takes up on a tile while drawn perpendicular to the
    // viewport vs
    //  how much space it would take up if it were drawn flat on the tile
    // Using law of sines, camera_to_anchor/sin(ground_angle) = camera_to_center/sin(incidence_angle)
    // Incidence angle 90 -> head on, sin(incidence_angle) = 1, no stretch
    // Incidence angle 1 -> very oblique, sin(incidence_angle) =~ 0, lots of stretch
    // ground_angle = u_pitch + PI/2 -> sin(ground_angle) = cos(u_pitch)
    // incidenceStretch = 1 / sin(incidenceAngle)

    const float incidenceStretch = pitchWithMap ? 1 : cameraToAnchorDistance / pitchFactor;
    const float lastSegmentTile = tileDistance.lastSegmentViewportDistance * pixelsToTileUnits;
    return tileDistance.prevTileDistance + lastSegmentTile +
           (incidenceStretch - 1) * lastSegmentTile * std::abs(std::sin(lastSegmentAngle));
}

bool CollisionIndex::isOffscreen(const CollisionBoundaries& boundaries) const {
    return boundaries[2] < viewportPadding || boundaries[0] >= screenRightBoundary || boundaries[3] < viewportPadding ||
           boundaries[1] >= screenBottomBoundary;
}

bool CollisionIndex::isInsideGrid(const CollisionBoundaries& boundaries) const {
    return boundaries[2] >= 0 && boundaries[0] < gridRightBoundary && boundaries[3] >= 0 &&
           boundaries[1] < gridBottomBoundary;
}

CollisionBoundaries CollisionIndex::projectTileBoundaries(const mat4& posMatrix) const {
    Point<float> topLeft = projectPoint(posMatrix, {0, 0});
    Point<float> bottomRight = projectPoint(posMatrix, {util::EXTENT, util::EXTENT});

    return {{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y}};
}

// The tile border checks below are only well defined when the tile boundaries are axis-aligned
// We are relying on it only being used in MapMode::Tile, where that is always the case
inline bool CollisionIndex::isInsideTile(const CollisionBoundaries& boundaries,
                                         const CollisionBoundaries& tileBoundaries) const {
    return boundaries[0] >= tileBoundaries[0] && boundaries[1] >= tileBoundaries[1] &&
           boundaries[2] < tileBoundaries[2] && boundaries[3] < tileBoundaries[3];
}

inline bool CollisionIndex::overlapsTile(const CollisionBoundaries& boundaries,
                                         const CollisionBoundaries& tileBoundaries) const {
    return boundaries[0] < tileBoundaries[2] && boundaries[2] > tileBoundaries[0] &&
           boundaries[1] < tileBoundaries[3] && boundaries[3] > tileBoundaries[1];
}

IntersectStatus CollisionIndex::intersectsTileEdges(const CollisionBox& box,
                                                    Point<float> shift,
                                                    const mat4& posMatrix,
                                                    const float textPixelRatio,
                                                    const CollisionBoundaries& tileEdges) const {
    auto boundaries = getProjectedCollisionBoundaries(posMatrix, shift, textPixelRatio, box);
    IntersectStatus result;
    const float x1 = boundaries[0];
    const float y1 = boundaries[1];
    const float x2 = boundaries[2];
    const float y2 = boundaries[3];

    const float tileX1 = tileEdges[0];
    const float tileY1 = tileEdges[1];
    const float tileX2 = tileEdges[2];
    const float tileY2 = tileEdges[3];

    // Check left border
    int minSectionLength = static_cast<int>(std::min(tileX1 - x1, x2 - tileX1));
    if (minSectionLength <= 0) { // Check right border
        minSectionLength = static_cast<int>(std::min(tileX2 - x1, x2 - tileX2));
    }
    if (minSectionLength > 0) {
        result.flags |= IntersectStatus::VerticalBorders;
        result.minSectionLength = minSectionLength;
    }
    // Check top border
    minSectionLength = static_cast<int>(std::min(tileY1 - y1, y2 - tileY1));
    if (minSectionLength <= 0) { // Check bottom border
        minSectionLength = static_cast<int>(std::min(tileY2 - y1, y2 - tileY2));
    }
    if (minSectionLength > 0) {
        result.flags |= IntersectStatus::HorizontalBorders;
        result.minSectionLength = std::min(result.minSectionLength, minSectionLength);
    }
    return result;
}

std::pair<bool, bool> CollisionIndex::placeFeature(
    const CollisionFeature& feature,
    Point<float> shift,
    const mat4& posMatrix,
    const mat4& labelPlaneMatrix,
    const float textPixelRatio,
    const PlacedSymbol& symbol,
    const float scale,
    const float fontSize,
    const bool allowOverlap,
    const bool pitchWithMap,
    const bool collisionDebug,
    const std::optional<CollisionBoundaries>& avoidEdges,
    const std::optional<std::function<bool(const IndexedSubfeature&)>>& collisionGroupPredicate,
    std::vector<ProjectedCollisionBox>& projectedBoxes) {
    assert(projectedBoxes.empty());
    if (!feature.alongLine) {
        const CollisionBox& box = feature.boxes.front();
        auto collisionBoundaries = getProjectedCollisionBoundaries(posMatrix, shift, textPixelRatio, box);
        projectedBoxes.emplace_back(
            collisionBoundaries[0], collisionBoundaries[1], collisionBoundaries[2], collisionBoundaries[3]);
        if ((avoidEdges && !isInsideTile(collisionBoundaries, *avoidEdges)) || !isInsideGrid(collisionBoundaries) ||
            (!allowOverlap && collisionGrid.hitTest(projectedBoxes.back().box(), collisionGroupPredicate))) {
            return {false, false};
        }

        return {true, isOffscreen(collisionBoundaries)};
    } else {
        return placeLineFeature(feature,
                                posMatrix,
                                labelPlaneMatrix,
                                textPixelRatio,
                                symbol,
                                scale,
                                fontSize,
                                allowOverlap,
                                pitchWithMap,
                                collisionDebug,
                                avoidEdges,
                                collisionGroupPredicate,
                                projectedBoxes);
    }
}

std::pair<bool, bool> CollisionIndex::placeLineFeature(
    const CollisionFeature& feature,
    const mat4& posMatrix,
    const mat4& labelPlaneMatrix,
    const float textPixelRatio,
    const PlacedSymbol& symbol,
    const float scale,
    const float fontSize,
    const bool allowOverlap,
    const bool pitchWithMap,
    const bool collisionDebug,
    const std::optional<CollisionBoundaries>& avoidEdges,
    const std::optional<std::function<bool(const IndexedSubfeature&)>>& collisionGroupPredicate,
    std::vector<ProjectedCollisionBox>& projectedBoxes) {
    assert(feature.alongLine);
    assert(projectedBoxes.empty());
    const auto tileUnitAnchorPoint = symbol.anchorPoint;
    const auto projectedAnchor = projectAnchor(posMatrix, tileUnitAnchorPoint);

    const float fontScale = fontSize / 24;
    const float lineOffsetX = symbol.lineOffset[0] * fontSize;
    const float lineOffsetY = symbol.lineOffset[1] * fontSize;

    const auto labelPlaneAnchorPoint = project(tileUnitAnchorPoint, labelPlaneMatrix).first;

    const auto firstAndLastGlyph = placeFirstAndLastGlyph(fontScale,
                                                          lineOffsetX,
                                                          lineOffsetY,
                                                          /*flip*/ false,
                                                          labelPlaneAnchorPoint,
                                                          tileUnitAnchorPoint,
                                                          symbol,
                                                          labelPlaneMatrix,
                                                          /*return tile distance*/ true);

    bool collisionDetected = false;
    bool inGrid = false;
    bool entirelyOffscreen = true;

    const auto tileToViewport = projectedAnchor.first * textPixelRatio;
    // pixelsToTileUnits is used for translating line geometry to tile units
    // ... so we care about 'scale' but not 'perspectiveRatio'
    // equivalent to pixel_to_tile_units
    const auto pixelsToTileUnits = 1 / (textPixelRatio * scale);

    float firstTileDistance = 0.f;
    float lastTileDistance = 0.f;
    if (firstAndLastGlyph) {
        firstTileDistance = approximateTileDistance(*(firstAndLastGlyph->first.tileDistance),
                                                    firstAndLastGlyph->first.angle,
                                                    pixelsToTileUnits,
                                                    projectedAnchor.second,
                                                    pitchWithMap);
        lastTileDistance = approximateTileDistance(*(firstAndLastGlyph->second.tileDistance),
                                                   firstAndLastGlyph->second.angle,
                                                   pixelsToTileUnits,
                                                   projectedAnchor.second,
                                                   pitchWithMap);
    }

    bool previousCirclePlaced = false;
    projectedBoxes.resize(feature.boxes.size());
    for (size_t i = 0; i < feature.boxes.size(); i++) {
        const CollisionBox& circle = feature.boxes[i];
        const float boxSignedDistanceFromAnchor = circle.signedDistanceFromAnchor;
        if (!firstAndLastGlyph || (boxSignedDistanceFromAnchor < -firstTileDistance) ||
            (boxSignedDistanceFromAnchor > lastTileDistance)) {
            // The label either doesn't fit on its line or we
            // don't need to use this circle because the label
            // doesn't extend this far. Either way, mark the circle unused.
            previousCirclePlaced = false;
            continue;
        }

        const auto projectedPoint = projectPoint(posMatrix, circle.anchor);
        const float tileUnitRadius = (circle.x2 - circle.x1) / 2;
        const float radius = tileUnitRadius * tileToViewport;

        if (previousCirclePlaced) {
            const ProjectedCollisionBox& previousCircle = projectedBoxes[i - 1];
            assert(previousCircle.isCircle());
            const auto& previousCenter = previousCircle.circle().center;
            const float dx = projectedPoint.x - previousCenter.x;
            const float dy = projectedPoint.y - previousCenter.y;
            // The circle edges touch when the distance between their centers is 2x the radius
            // When the distance is 1x the radius, they're doubled up, and we could remove
            // every other circle while keeping them all in touch.
            // We actually start removing circles when the distance is √2x the radius:
            //  thinning the number of circles as much as possible is a major performance win,
            //  and the small gaps introduced don't make a very noticeable difference.
            const bool placedTooDensely = radius * radius * 2 > dx * dx + dy * dy;
            if (placedTooDensely) {
                const bool atLeastOneMoreCircle = (i + 1) < feature.boxes.size();
                if (atLeastOneMoreCircle) {
                    const CollisionBox& nextCircle = feature.boxes[i + 1];
                    const float nextBoxDistanceFromAnchor = nextCircle.signedDistanceFromAnchor;
                    if ((nextBoxDistanceFromAnchor > -firstTileDistance) &&
                        (nextBoxDistanceFromAnchor < lastTileDistance)) {
                        // Hide significantly overlapping circles, unless this is the last one we can
                        // use, in which case we want to keep it in place even if it's tightly packed
                        // with the one before it.
                        previousCirclePlaced = false;
                        continue;
                    }
                }
            }
        }

        previousCirclePlaced = true;

        CollisionBoundaries collisionBoundaries{{projectedPoint.x - radius,
                                                 projectedPoint.y - radius,
                                                 projectedPoint.x + radius,
                                                 projectedPoint.y + radius}};

        projectedBoxes[i] = ProjectedCollisionBox{projectedPoint.x, projectedPoint.y, radius};

        entirelyOffscreen &= isOffscreen(collisionBoundaries);
        inGrid |= isInsideGrid(collisionBoundaries);

        if ((avoidEdges && !isInsideTile(collisionBoundaries, *avoidEdges)) ||
            (!allowOverlap && collisionGrid.hitTest(projectedBoxes[i].circle(), collisionGroupPredicate))) {
            if (!collisionDebug) {
                return {false, false};
            } else {
                // Don't early exit if we're showing the debug circles because we still want to calculate
                // which circles are in use
                collisionDetected = true;
            }
        }
    }

    return {!collisionDetected && firstAndLastGlyph && inGrid, entirelyOffscreen};
}

void CollisionIndex::insertFeature(const CollisionFeature& feature,
                                   const std::vector<ProjectedCollisionBox>& projectedBoxes,
                                   bool ignorePlacement,
                                   uint32_t bucketInstanceId,
                                   uint16_t collisionGroupId) {
    if (feature.alongLine) {
        for (auto& circle : projectedBoxes) {
            if (!circle.isCircle()) {
                continue;
            }

            if (ignorePlacement) {
                ignoredGrid.insert(IndexedSubfeature(feature.indexedFeature, bucketInstanceId, collisionGroupId),
                                   circle.circle());
            } else {
                collisionGrid.insert(IndexedSubfeature(feature.indexedFeature, bucketInstanceId, collisionGroupId),
                                     circle.circle());
            }
        }
    } else if (!projectedBoxes.empty()) {
        assert(projectedBoxes.size() == 1);
        auto& box = projectedBoxes[0];
        assert(box.isBox());
        if (ignorePlacement) {
            ignoredGrid.insert(IndexedSubfeature(feature.indexedFeature, bucketInstanceId, collisionGroupId),
                               box.box());
        } else {
            collisionGrid.insert(IndexedSubfeature(feature.indexedFeature, bucketInstanceId, collisionGroupId),
                                 box.box());
        }
    }
}

bool polygonIntersectsBox(const LineString<float>& polygon, const GridIndex<IndexedSubfeature>::BBox& bbox) {
    // This is just a wrapper that allows us to use the integer-based util::polygonIntersectsPolygon
    // Conversion limits our query accuracy to single-pixel resolution
    GeometryCoordinates integerPolygon;
    for (const auto& point : polygon) {
        integerPolygon.push_back(convertPoint<int16_t>(point));
    }
    auto minX1 = static_cast<int16_t>(bbox.min.x);
    auto maxY1 = static_cast<int16_t>(bbox.max.y);
    auto minY1 = static_cast<int16_t>(bbox.min.y);
    auto maxX1 = static_cast<int16_t>(bbox.max.x);

    auto bboxPoints = GeometryCoordinates{{minX1, minY1}, {maxX1, minY1}, {maxX1, maxY1}, {minX1, maxY1}};

    return util::polygonIntersectsPolygon(integerPolygon, bboxPoints);
}

std::unordered_map<uint32_t, std::vector<IndexedSubfeature>> CollisionIndex::queryRenderedSymbols(
    const ScreenLineString& queryGeometry) const {
    std::unordered_map<uint32_t, std::vector<IndexedSubfeature>> result;
    if (queryGeometry.empty() || (collisionGrid.empty() && ignoredGrid.empty())) {
        return result;
    }

    LineString<float> gridQuery;
    for (const auto& point : queryGeometry) {
        gridQuery.emplace_back(static_cast<float>(point.x) + viewportPadding,
                               static_cast<float>(point.y) + viewportPadding);
    }

    auto envelope = mapbox::geometry::envelope(gridQuery);

    using QueryResult = std::pair<IndexedSubfeature, GridIndex<IndexedSubfeature>::BBox>;

    std::vector<QueryResult> features = collisionGrid.queryWithBoxes(envelope);
    std::vector<QueryResult> ignoredFeatures = ignoredGrid.queryWithBoxes(envelope);
    features.insert(features.end(), ignoredFeatures.begin(), ignoredFeatures.end());

    std::unordered_map<uint32_t, std::unordered_set<size_t>> seenBuckets;
    for (auto& queryResult : features) {
        auto& feature = queryResult.first;
        auto& bbox = queryResult.second;

        // Skip already seen features.
        auto& seenFeatures = seenBuckets[feature.bucketInstanceId];
        if (seenFeatures.find(feature.index) != seenFeatures.end()) continue;

        if (!polygonIntersectsBox(gridQuery, bbox)) {
            continue;
        }

        seenFeatures.insert(feature.index);
        result[feature.bucketInstanceId].push_back(feature);
    }

    return result;
}

std::pair<float, float> CollisionIndex::projectAnchor(const mat4& posMatrix, const Point<float>& point) const {
    vec4 p = {{point.x, point.y, 0, 1}};
    matrix::transformMat4(p, p, posMatrix);
    return std::make_pair(0.5f + 0.5f * (transformState.getCameraToCenterDistance() / static_cast<float>(p[3])),
                          static_cast<float>(p[3]));
}

std::pair<Point<float>, float> CollisionIndex::projectAndGetPerspectiveRatio(const mat4& posMatrix,
                                                                             const Point<float>& point) const {
    vec4 p = {{point.x, point.y, 0, 1}};
    matrix::transformMat4(p, p, posMatrix);
    auto size = transformState.getSize();
    return std::make_pair(Point<float>(static_cast<float>(((p[0] / p[3] + 1) / 2) * size.width + viewportPadding),
                                       static_cast<float>(((-p[1] / p[3] + 1) / 2) * size.height + viewportPadding)),
                          // See perspective ratio comment in symbol_sdf.vertex
                          // We're doing collision detection in viewport space so we need
                          // to scale down boxes in the distance
                          0.5f + 0.5f * transformState.getCameraToCenterDistance() / static_cast<float>(p[3]));
}

Point<float> CollisionIndex::projectPoint(const mat4& posMatrix, const Point<float>& point) const {
    vec4 p = {{point.x, point.y, 0, 1}};
    matrix::transformMat4(p, p, posMatrix);
    auto size = transformState.getSize();
    return Point<float>{static_cast<float>((((p[0] / p[3] + 1) / 2) * size.width) + viewportPadding),
                        static_cast<float>((((-p[1] / p[3] + 1) / 2) * size.height) + viewportPadding)};
}

CollisionBoundaries CollisionIndex::getProjectedCollisionBoundaries(const mat4& posMatrix,
                                                                    Point<float> shift,
                                                                    float textPixelRatio,
                                                                    const CollisionBox& box) const {
    const auto projectedPoint = projectAndGetPerspectiveRatio(posMatrix, box.anchor);
    const float tileToViewport = textPixelRatio * projectedPoint.second;
    return CollisionBoundaries{{
        (box.x1 + shift.x) * tileToViewport + projectedPoint.first.x,
        (box.y1 + shift.y) * tileToViewport + projectedPoint.first.y,
        (box.x2 + shift.x) * tileToViewport + projectedPoint.first.x,
        (box.y2 + shift.y) * tileToViewport + projectedPoint.first.y,
    }};
}

} // namespace mbgl
