#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/bound_options.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/math/angles.hpp>
#include <mln/style/style.hpp>
#include <mln/style/terrain.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <cmath>

using namespace mln;

namespace {

constexpr uint32_t demTileSize = 64;
// Terrain-RGB encodes elevation as -10000 + (R * 65536 + G * 256 + B) * 0.1, so a flat
// 1000 m plateau is RGB(1, 173, 176): 65536 + 44288 + 176 = 110000.
constexpr double plateauMeters = 1000.0;

std::string makeFlatDEMTile() {
    PremultipliedImage image({demTileSize, demTileSize});
    for (uint32_t i = 0; i < demTileSize * demTileSize; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = 1;
        px[1] = 173;
        px[2] = 176;
        px[3] = 255;
    }
    return encodePNG(image);
}

const char* terrainStyle = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 2,
      "tileSize": 64
    }
  },
  "terrain": {"source": "dem", "exaggeration": 1.0},
  "layers": []
})STYLE";

struct TerrainCameraTest {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    std::string tile = makeFlatDEMTile();
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map;

    TerrainCameraTest()
        : map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())) {
        fileSource->tileResponse = [this](const Resource&) {
            Response res;
            res.data = std::make_shared<std::string>(tile);
            return res;
        };
        map.getStyle().loadJSON(terrainStyle);
        map.jumpTo(CameraOptions().withCenter(LatLng{47.2692, 11.4041}).withZoom(2.0).withPitch(60.0));
    }

    double renderAndGetAltitude(int frames) {
        double altitude = 0.0;
        for (int i = 0; i < frames; ++i) {
            loop.runOnce();
            frontend.render(map);
            altitude = map.getCameraOptions({}).centerAltitude.value_or(0.0);
        }
        return altitude;
    }
};

} // namespace

// The centre rides the terrain instead of sea level, so pitching over high ground does not
// leave the camera inside the hillside.
TEST(TerrainCamera, CentreRidesTheTerrainSurface) {
    TerrainCameraTest test;
    EXPECT_TRUE(test.map.getCenterClampedToGround());

    const double settled = test.renderAndGetAltitude(8);
    EXPECT_NEAR(settled, plateauMeters, 5.0);
}

// The historical worry about a terrain-anchored centre was that raising it re-samples a new
// elevation and runs away. Raising the centre moves the orbit plane, not the centre's lng/lat,
// so it must settle - and the centre must not drift while it does.
TEST(TerrainCamera, CentreElevationSettlesWithoutDrift) {
    TerrainCameraTest test;

    test.renderAndGetAltitude(8);
    const auto afterSettling = test.map.getCameraOptions({});
    const double settledAltitude = *afterSettling.centerAltitude;
    // Guard against settling "stably" at sea level, which would make the rest vacuous.
    ASSERT_NEAR(settledAltitude, plateauMeters, 5.0);

    // Ten more frames with nothing else changing.
    const double later = test.renderAndGetAltitude(10);
    const auto atEnd = test.map.getCameraOptions({});

    EXPECT_NEAR(later, settledAltitude, 0.5);
    EXPECT_NEAR(atEnd.center->latitude(), afterSettling.center->latitude(), 1e-6);
    EXPECT_NEAR(atEnd.center->longitude(), afterSettling.center->longitude(), 1e-6);
    EXPECT_NEAR(*atEnd.zoom, *afterSettling.zoom, 1e-6);
}

// Turning it off leaves the camera where the caller put it.
TEST(TerrainCamera, ClampingCanBeTurnedOff) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    EXPECT_FALSE(test.map.getCenterClampedToGround());

    EXPECT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);
}

// An app can put the centre at any altitude without moving the camera, and restore a saved camera
// by setting its altitude before its centre and zoom: the same centre and zoom at another altitude
// are a different camera.
TEST(TerrainCamera, CentreAltitudeIsSetAndRestoredKeepingTheView) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0));

    const auto eye = [&] {
        return *test.map.getFreeCameraOptions().position;
    };
    // One metre in the Mercator units of the camera position, at the centre's latitude.
    const double metre = 1.0 / (util::M2PI * util::EARTH_RADIUS_M * std::cos(util::deg2rad(47.2692)));
    const auto expectEyeAt = [&](const vec3& expected) {
        const vec3 actual = eye();
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(actual[i], expected[i], 5 * metre) << "axis " << i;
        }
    };
    const vec3 before = eye();
    const double zoomAtSeaLevel = *test.map.getCameraOptions({}).zoom;

    EXPECT_TRUE(test.map.setCenterAltitudeKeepingView(1500.0));
    const CameraOptions saved = test.map.getCameraOptions({});
    EXPECT_NEAR(*saved.centerAltitude, 1500.0, 0.001);
    EXPECT_GT(*saved.zoom, zoomAtSeaLevel);
    expectEyeAt(before);
    // Already there: nothing to move.
    EXPECT_FALSE(test.map.setCenterAltitudeKeepingView(1500.0));

    // Somewhere else, at sea level.
    EXPECT_TRUE(test.map.setCenterAltitudeKeepingView(0.0));
    test.map.jumpTo(CameraOptions().withCenter(LatLng{47.0, 11.0}).withZoom(9.0));

    // Restore: the altitude first, then the centre and zoom.
    test.map.setCenterAltitudeKeepingView(*saved.centerAltitude);
    test.map.jumpTo(CameraOptions().withCenter(*saved.center).withZoom(*saved.zoom));
    expectEyeAt(before);

    // Back at sea level, the original centre and zoom.
    EXPECT_TRUE(test.map.setCenterAltitudeKeepingView(0.0));
    EXPECT_NEAR(*test.map.getCameraOptions({}).zoom, zoomAtSeaLevel, 1e-3);
    expectEyeAt(before);
}

// A gesture start anchors the centre on the ground being looked at without moving the camera,
// and removing the terrain brings it back to sea level, again without moving the camera:
// otherwise the flat map keeps orbiting a point up in the air.
TEST(TerrainCamera, AnchoredCentreReturnsToSeaLevelWhenTerrainIsRemoved) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0));
    // Lets the renderer report the plateau under the centre.
    ASSERT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);

    const auto eye = [&] {
        return *test.map.getFreeCameraOptions().position;
    };
    // One metre in the Mercator units of the camera position, at the centre's latitude.
    const double metre = 1.0 / (util::M2PI * util::EARTH_RADIUS_M * std::cos(util::deg2rad(47.2692)));
    const auto expectEyeAt = [&](const vec3& expected) {
        const vec3 actual = eye();
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(actual[i], expected[i], 5 * metre) << "axis " << i;
        }
    };
    const vec3 before = eye();
    const double zoomBefore = *test.map.getCameraOptions({}).zoom;

    test.map.anchorCenterOnTerrain();
    EXPECT_NEAR(*test.map.getCameraOptions({}).centerAltitude, plateauMeters, 0.5);
    EXPECT_GT(*test.map.getCameraOptions({}).zoom, zoomBefore);
    expectEyeAt(before);

    test.map.getStyle().setTerrain(nullptr);
    EXPECT_NEAR(test.renderAndGetAltitude(1), 0.0, 0.001);
    EXPECT_NEAR(*test.map.getCameraOptions({}).zoom, zoomBefore, 1e-3);
    expectEyeAt(before);

    // Without terrain there is nothing to anchor on, whatever height was last reported.
    test.map.anchorCenterOnTerrain();
    EXPECT_NEAR(test.map.getCameraOptions({}).centerAltitude.value_or(0.0), 0.0, 0.001);
}

// Anchoring re-describes the same view, so it reports no camera change: platforms anchor as a
// gesture starts, and a reported change reads to apps as the gesture having ended (iOS
// regionDidChange, Android onCameraIdle), which then run their end-of-gesture work mid-drag.
TEST(TerrainCamera, AnchoringReportsNoCameraChange) {
    struct CountingObserver : MapObserver {
        int willChange = 0;
        int didChange = 0;
        void onCameraWillChange(CameraChangeMode) override { ++willChange; }
        void onCameraDidChange(CameraChangeMode) override { ++didChange; }
    } observer;

    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string tile = makeFlatDEMTile();
    fileSource->tileResponse = [&](const Resource&) {
        Response res;
        res.data = std::make_shared<std::string>(tile);
        return res;
    };
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map(
        frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.setCenterClampedToGround(false);
    map.getStyle().loadJSON(terrainStyle);
    map.jumpTo(CameraOptions().withCenter(LatLng{47.2692, 11.4041}).withZoom(10.0).withPitch(60.0));
    for (int i = 0; i < 8; ++i) {
        loop.runOnce();
        frontend.render(map);
    }

    const vec3 eyeBefore = *map.getFreeCameraOptions().position;
    observer.willChange = 0;
    observer.didChange = 0;

    map.anchorCenterOnTerrain();
    ASSERT_NEAR(*map.getCameraOptions({}).centerAltitude, plateauMeters, 0.5);
    EXPECT_EQ(observer.willChange, 0);
    EXPECT_EQ(observer.didChange, 0);

    EXPECT_TRUE(map.setCenterAltitudeKeepingView(0.0));
    EXPECT_EQ(observer.willChange, 0);
    EXPECT_EQ(observer.didChange, 0);

    // ...and the camera did not move.
    // One metre in the Mercator units of the camera position, at the centre's latitude.
    const double metre = 1.0 / (util::M2PI * util::EARTH_RADIUS_M * std::cos(util::deg2rad(47.2692)));
    const vec3 eyeAfter = *map.getFreeCameraOptions().position;
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(eyeAfter[i], eyeBefore[i], 5 * metre) << "axis " << i;
    }
}

// Near a zoom limit the anchor goes as far up the line of sight as the limit allows, keeping the
// view, rather than leaving the centre at sea level under the ground being looked at.
TEST(TerrainCamera, AnchorStopsAtTheZoomLimitKeepingTheView) {
    TerrainCameraTest test;
    test.map.setCenterClampedToGround(false);
    test.map.jumpTo(CameraOptions().withZoom(10.0));
    ASSERT_NEAR(test.renderAndGetAltitude(8), 0.0, 0.001);

    // A full anchor onto the 1000 m plateau needs about zoom 10.15 here (pitch 60, a 256 px view).
    test.map.setBounds(BoundOptions().withMaxZoom(10.07));
    const vec3 before = *test.map.getFreeCameraOptions().position;

    test.map.anchorCenterOnTerrain();
    const auto after = test.map.getCameraOptions({});
    EXPECT_NEAR(*after.zoom, 10.07, 1e-6);
    EXPECT_GT(*after.centerAltitude, 100.0);
    EXPECT_LT(*after.centerAltitude, plateauMeters - 100.0);

    const double metre = 1.0 / (util::M2PI * util::EARTH_RADIUS_M * std::cos(util::deg2rad(47.2692)));
    const vec3 eye = *test.map.getFreeCameraOptions().position;
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(eye[i], before[i], 5 * metre) << "axis " << i;
    }
}
