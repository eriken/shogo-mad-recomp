// Unit tests for engine pieces that don't need a window: string formatting,
// HSTRING reference counting, and (when SHOGO_DATA points at the game folder)
// the REZ and PE resource readers.
#include "engine/rez_file.h"
#include "engine/strings.h"
#include "engine/surfaces.h"
#include "engine/fonts.h"
#include "engine/game_import.h"
#include "engine/image_read.h"
#include "engine/inno_setup.h"
#include "engine/dtx.h"
#include "engine/render_world.h"
#include "engine/world_dat.h"
#include "engine/api_common.h"
#include "engine/math3d.h"
#include "engine/messages.h"
#include "engine/collision.h"
#include "engine/model.h"
#include "engine/effects.h"
#include "engine/sound.h"
#include "engine/smacker.h"
#include "engine/music.h"
#include "engine/dls.h"
#include "engine/objects.h"
#include "engine/server.h"
#include "engine/input.h"
#include "engine/stub_support.h"

#include <algorithm>
#include <cmath>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static std::string fmt(const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    std::string s = shogo::formatMessage(f, ap);
    va_end(ap);
    return s;
}

static void testFormatMessage()
{
    CHECK(fmt("restart %1!s!", "Ambush") == "restart Ambush");
    CHECK(fmt("%1 killed %2", "Sanjuro", "Kura") == "Sanjuro killed Kura");  // default !s!
    CHECK(fmt("%2!s! has %1!d! frags", 12, "Kura") == "Kura has 12 frags");  // args read in index order
    CHECK(fmt("%1!5.1f!%%", 99.25) == " 99.2%" || fmt("%1!5.1f!%%", 99.25) == " 99.3%");
    CHECK(fmt("a%nb%tc") == "a\r\nb\tc");
    CHECK(fmt("stop%0 here") == "stop");
    CHECK(fmt("100%% done%.") == "100% done.");
    CHECK(fmt("%1!d!/%1!d!", 3) == "3/3");  // same insert used twice
    CHECK(fmt("plain text") == "plain text");
}

static void testRefCounting()
{
    shogo::EngineStrings strings(nullptr, "test");
    HSTRING a = strings.create("Hello");
    CHECK(strcmp(strings.data(a), "Hello") == 0);
    HSTRING b = strings.copy(a);
    CHECK(a == b);
    CHECK(strings.liveCount() == 1);
    strings.free(a);
    CHECK(strings.liveCount() == 1);  // still referenced by b
    CHECK(strcmp(strings.data(b), "Hello") == 0);
    HSTRING c = strings.create("HELLO");
    CHECK(!strings.equal(b, c, false));
    CHECK(strings.equal(b, c, true));
    strings.free(b);
    strings.free(c);
    CHECK(strings.liveCount() == 0);
    CHECK(strings.data(nullptr) == nullptr);
}

// Every level parses, and its polygons lie on their planes (a strong check on the format reading).
static void testWorlds(const shogo::ResourceFS &fs)
{
    auto worlds = fs.list("WORLDS", ".DAT");
    CHECK(worlds.size() == 61);
    size_t polys = 0, offPlane = 0, starts = 0, startsInside = 0, farOutside = 0;
    size_t models = 0, modelsLit = 0, lightmaps = 0, lmVerts = 0, lmVertsInside = 0;
    for (const auto &name : worlds) {
        std::string err;
        auto w = shogo::WorldData::parse(*fs.read(name), &err);
        CHECK(w != nullptr);
        if (!w) {
            fprintf(stderr, "  %s: %s\n", name.c_str(), err.c_str());
            continue;
        }
        CHECK(!w->objects.empty() && !w->models.empty());
        // The BSP: a player start is in open space, far away is not.
        for (const auto &o : w->objects) {
            if (o.className != "GameStartPoint") continue;
            const auto *pos = o.find("Pos");
            if (!pos) continue;
            ++starts;
            const auto &bsp = w->models[0];
            if (bsp.pointInside({pos->vec[0], pos->vec[1] + 16, pos->vec[2]})) ++startsInside;
            else fprintf(stderr, "  %s: start point not inside\n", name.c_str());
            // Far below the level is outside, except in three multiplayer maps whose
            // trees leave the space around the hull open (MCA_Avernus, MCA_MadSkills,
            // OF_Midnight: they were built leaky; the floor under the start is still solid).
            shogo::Vec3 c{(bsp.boxMin.x + bsp.boxMax.x) / 2, bsp.boxMin.y - 1000, (bsp.boxMin.z + bsp.boxMax.z) / 2};
            if (!bsp.pointInside(c)) ++farOutside;
            else if (name.find("MULTI") == std::string::npos) fprintf(stderr, "  %s: point below the level inside\n", name.c_str());
            break;
        }
        // Lightmaps: every block's lighting data decodes, and a lightmap's
        // samples (20 units apart from its origin) cover its polygon.
        for (const auto &m : w->models) {
            ++models;
            modelsLit += m.hasLightmaps;
            for (const auto &p : m.polys) {
                if (p.lightmap.empty()) continue;
                ++lightmaps;
                const auto &s = m.surfaces[p.surface];
                for (const auto &v : p.verts) {
                    shogo::Vec3 r = m.points[v.point] - p.lmOrigin;
                    float lu = (r.x * s.lmP.x + r.y * s.lmP.y + r.z * s.lmP.z) / shogo::kLightmapSpacing;
                    float lv = (r.x * s.lmQ.x + r.y * s.lmQ.y + r.z * s.lmQ.z) / shogo::kLightmapSpacing;
                    ++lmVerts;
                    lmVertsInside += lu >= -0.01f && lv >= -0.01f && lu <= p.lmW - 1 + 0.01f && lv <= p.lmH - 1 + 0.01f;
                }
            }
        }
        for (const auto &m : w->models)
            for (const auto &p : m.polys) {
                const auto &pl = m.planes[m.surfaces[p.surface].plane];
                ++polys;
                for (const auto &v : p.verts) {
                    const auto &q = m.points[v.point];
                    if (std::fabs(pl.normal.x * q.x + pl.normal.y * q.y + pl.normal.z * q.z - pl.dist) > 1.0f) {
                        ++offPlane;
                        break;
                    }
                }
            }
    }
    CHECK(polys > 300000 && offPlane * 200 < polys);  // < 0.5%: the source geometry has a few warped polygons
    printf("  %zu worlds, %zu polygons, %zu off-plane\n", worlds.size(), polys, offPlane);
    CHECK(starts > 30 && startsInside == starts && farOutside + 3 == starts);
    printf("  lightmaps: %zu of %zu blocks decode, %zu lightmaps, %zu of %zu vertices inside theirs\n", modelsLit, models,
           lightmaps, lmVertsInside, lmVerts);
    CHECK(modelsLit == models && lightmaps > 50000 && lmVertsInside * 100 > lmVerts * 99);
    printf("  BSP: %zu of %zu start points inside, far points outside in %zu\n", startsInside, starts, farOutside);

    // Light at a point on a polygon: a lightmap's origin reads its first sample;
    // a vertex-lit polygon at a corner reads close to that corner's light.
    {
        auto q = shogo::WorldData::parse(*fs.read("WORLDS/02_QUARTERS.DAT"), nullptr);
        CHECK(q != nullptr);
        if (q) {
            const auto &m = q->models[0];
            bool lmChecked = false, vertexChecked = false;
            for (size_t i = 0; i < m.polys.size() && !(lmChecked && vertexChecked); ++i) {
                const auto &p = m.polys[i];
                const auto &s = m.surfaces[p.surface];
                if (s.flags & 0x20) continue;
                if (!lmChecked && !p.lightmap.empty()) {
                    uint16_t c = p.lightmap[0];
                    shogo::Vec3 l = m.lightAt(i, p.lmOrigin);
                    CHECK(std::fabs(l.x - ((c >> 11) & 31) / 31.0f) < 0.01f && std::fabs(l.y - ((c >> 5) & 63) / 63.0f) < 0.01f &&
                          std::fabs(l.z - (c & 31) / 31.0f) < 0.01f);
                    lmChecked = true;
                } else if (!vertexChecked && p.lightmap.empty() && !(s.flags & 0x80)) {
                    const auto &v = p.verts[0];
                    shogo::Vec3 l = m.lightAt(i, m.points[v.point]);
                    // The other corners pull a little: within 0.25 of the corner's own light.
                    CHECK(std::fabs(l.x - v.r / 255.0f) < 0.25f && std::fabs(l.y - v.g / 255.0f) < 0.25f &&
                          std::fabs(l.z - v.b / 255.0f) < 0.25f);
                    vertexChecked = true;
                }
            }
            CHECK(lmChecked && vertexChecked);
        }
    }

    // Polygon handles: collision triangles name their polygon, and the polygon's
    // texture carries a surface type (DTX header word at offset 20).
    {
        auto q = shogo::WorldData::parse(*fs.read("WORLDS/02_QUARTERS.DAT"), nullptr);
        CHECK(q != nullptr);
        if (q) {
            shogo::CollisionModel cm;
            cm.build(q->models[0], 0);
            size_t good = 0;
            for (const auto &t : cm.tris()) {
                uint32_t model = t.hpoly >> 24, idx = t.hpoly & 0xFFFFFF;
                if (model != 0 || idx >= q->models[0].polys.size()) continue;
                const auto &surf = q->models[0].surfaces[q->models[0].polys[idx].surface];
                if (dot(q->models[0].planes[surf.plane].normal, t.n) > 0.99f) ++good;
            }
            CHECK(!cm.tris().empty() && good == cm.tris().size());
        }
        auto surfaceType = [&](const char *tex) {
            auto d = fs.read(tex);
            uint32_t v = 0;
            if (d && d->size() >= 24) memcpy(&v, &(*d)[20], 4);
            return v;
        };
        CHECK(surfaceType("Textures\\Sky\\Nebula.dtx") == 110);                          // ST_SKY
        CHECK(surfaceType("Textures\\Layouts\\1)Leviathan_OnFoot\\Bed2.dtx") == 80);   // ST_CLOTH
        CHECK(surfaceType("Textures\\Glass\\Glass2c.dtx") == 40);                         // ST_GLASS
    }

    // Textures decode, and the renderer draws something at the first level's start point.
    auto dtx = fs.read("TEXTURES/SKY.DTX");
    CHECK(dtx && shogo::decodeDTX(*dtx));
    auto ambush = shogo::WorldData::parse(*fs.read("WORLDS/01_AMBUSH.DAT"), nullptr);
    if (!ambush) return;
    shogo::WorldRenderer renderer;
    renderer.load(*ambush, fs);
    CHECK(renderer.stats().missingTextures == 0);
    shogo::Camera cam;
    CHECK(renderer.startPoint(cam));
    shogo::Surface frame(320, 240);
    renderer.render(frame, cam);
    size_t lit = 0;
    for (uint32_t px : frame.pixels) lit += px != 0;
    CHECK(lit > frame.pixels.size() * 9 / 10);
}

static shogo::SweepTri quad(shogo::Vec3 a, shogo::Vec3 b, shogo::Vec3 c, shogo::Vec3 n, int half)
{
    // Half 0: a b c; half 1: a c (a + c - b).
    shogo::SweepTri t;
    shogo::Vec3 d = a + c - b;
    t.tri = half == 0 ? shogo::Tri{a, b, c, n, 1} : shogo::Tri{a, c, d, n, 1};
    return t;
}

static void testCollision()
{
    using shogo::Vec3;
    std::vector<shogo::SweepTri> tris;
    // A floor at y = 0 from -1000 to 1000, and a wall at x = 100 facing -x.
    for (int h = 0; h < 2; ++h) tris.push_back(quad({-1000, 0, -1000}, {-1000, 0, 1000}, {1000, 0, 1000}, {0, 1, 0}, h));
    for (int h = 0; h < 2; ++h) tris.push_back(quad({100, -100, -1000}, {100, 500, -1000}, {100, 500, 1000}, {-1, 0, 0}, h));
    Vec3 radii{10, 20, 10};

    // Resting on the floor and walking (with a little gravity): moves along the floor.
    std::vector<shogo::SweepHit> hits;
    Vec3 end = shogo::collideAndSlide(tris, radii, {0, 20, 0}, {30, -1, 5}, &hits);
    CHECK(std::fabs(end.x - 30) < 0.1f && std::fabs(end.z - 5) < 0.1f);
    CHECK(end.y > 19.9f && end.y < 20.01f);

    // Falling onto the floor stops at it (a hair above: the slide keeps a
    // 0.005 gap in ellipsoid space, 0.1 units for a radius of 20).
    end = shogo::collideAndSlide(tris, radii, {0, 50, 0}, {0, -100, 0}, nullptr);
    CHECK(end.y > 19.9f && end.y < 20.15f);

    // Into the wall: stops one radius short of it.
    end = shogo::collideAndSlide(tris, radii, {0, 20, 0}, {200, 0, 0}, nullptr);
    CHECK(end.x > 89.9f && end.x < 90.0f);

    // Diagonally into the wall: slides along it.
    end = shogo::collideAndSlide(tris, radii, {80, 20, 0}, {50, 0, 50}, nullptr);
    CHECK(end.x < 90.0f && std::fabs(end.z - 50) < 0.5f);

    // A steep face (normal y 0.14): sliding creeps up it, unless walls are flattened.
    std::vector<shogo::SweepTri> steep;
    for (int h = 0; h < 2; ++h) steep.push_back(tris[h]);
    shogo::Vec3 sn = shogo::normalized({-1, 0.14f, 0});
    for (int h = 0; h < 2; ++h)
        steep.push_back(quad({100, -100, -1000}, {100 + 600 * 0.14f, 500, -1000}, {100 + 600 * 0.14f, 500, 1000}, sn, h));
    end = shogo::collideAndSlide(steep, radii, {80, 20, 0}, {50, 0, 50}, nullptr);
    CHECK(end.y > 20.5f);
    end = shogo::collideAndSlide(steep, radii, {80, 20, 0}, {50, 0, 50}, nullptr, 0.5f);
    CHECK(end.y < 20.2f && std::fabs(end.z - 50) < 1.0f);

    // A plain sweep reports the hit and its normal.
    shogo::SweepHit hit = shogo::sweepEllipsoid(tris, radii, {0, 20, 0}, {200, 0, 0});
    CHECK(hit.hit && std::fabs(hit.distance - 90) < 0.1f && hit.normal.x < -0.99f);

    // Walking (the player on foot: radii 22 45 22, steps up to 24) into a
    // step of height h at x = 100, a frame at a time, the velocity stopped by
    // what's hit as the engine does. Returns the highest and the last place.
    auto walk = [](float h, float speed, Vec3 *top) {
        std::vector<shogo::SweepTri> st;
        for (int k = 0; k < 2; ++k) {
            st.push_back(quad({-1000, 0, -1000}, {-1000, 0, 1000}, {100, 0, 1000}, {0, 1, 0}, k));
            st.push_back(quad({100, h, -1000}, {100, h, 1000}, {1000, h, 1000}, {0, 1, 0}, k));
            st.push_back(quad({100, 0, -1000}, {100, h, -1000}, {100, h, 1000}, {-1, 0, 0}, k));
        }
        Vec3 r{22, 45, 22}, pos{40, 45.05f, 0}, vel{0, 0, 0};
        *top = pos;
        for (int f = 0; f < 60; ++f) {
            vel.x = std::min(vel.x + 100.0f, speed);  // accelerating up to speed, units/s at 30 fps
            vel.y -= 33.0f;                           // gravity
            std::vector<shogo::SweepHit> hits;
            pos = shogo::walkMove(st, r, pos, vel * (1.0f / 30), 24.0f, hits);
            for (const shogo::SweepHit &sh : hits) {
                float into = shogo::dot(vel, sh.normal);
                if (into < 0) vel = vel - sh.normal * into;
            }
            if (pos.y > top->y) *top = pos;
        }
        return pos;
    };
    Vec3 top;
    // Up a 16 step from a walk and from a run: onto it, without hopping up
    // off its lip, and keeping the speed.
    for (float speed : {200.0f, 400.0f}) {
        end = walk(16, speed, &top);
        CHECK(end.x > 100 + 22 && std::fabs(end.y - 61) < 0.3f);
        CHECK(top.y < 61.5f);
    }
    // A 32 ledge is too high: stopped at its foot.
    end = walk(32, 400, &top);
    CHECK(end.x < 100 && top.y < 45.5f);
}

static void testModelAnimation()
{
    shogo::ModelInfo info;
    shogo::ModelAnim walk;
    walk.name = "WALK";
    walk.length = 1000;
    walk.dims = DVector(10, 30, 10);
    walk.keys = {{0, "start"}, {500, "step left"}, {1000, ""}};
    info.anims.push_back(walk);
    CHECK(info.find("walk") == 0 && info.find("RUN") == -1);

    shogo::EngineObject o;
    o.type = OT_MODEL;
    o.flags = FLAG_MODELKEYS;
    o.filename = "test.abc";
    o.modelFile = o.filename;
    o.modelLoaded = true;
    o.model = &info;
    o.anim = 0;
    std::vector<std::string> keys;
    auto onKey = [&](shogo::EngineObject *, ArgList *a) {
        std::string k;
        for (int i = 0; i < a->argc; ++i) k += (i ? " " : "") + std::string(a->argv[i]);
        keys.push_back(k);
    };
    shogo::animateObject(&o, 0.4f, onKey);  // fires the key at 0
    CHECK(keys.size() == 1 && keys[0] == "start");
    shogo::animateObject(&o, 0.2f, onKey);  // passes 500
    CHECK(keys.size() == 2 && keys[1] == "step left");
    shogo::animateObject(&o, 0.5f, onKey);  // loops: 1100 -> 100, fires 0 again
    CHECK(keys.size() == 3 && keys[2] == "start" && std::fabs(o.animTime - 100) < 1);
    o.animLoop = false;
    shogo::animateObject(&o, 2.0f, onKey);
    CHECK(o.animDone && o.animTime == 1000);
    o.flags = 0;  // keys reach the callback either way; the server checks FLAG_MODELKEYS itself
    o.animDone = false;
    o.animTime = 0;
    o.animStarted = false;
    keys.clear();
    shogo::animateObject(&o, 0.6f, onKey);
    CHECK(keys.size() == 2);
}

static void testPose()
{
    // Root at (0, 10, 0); a child 5 along +x, its parent turned 90 degrees
    // about y between keyframes. One vertex on each node.
    shogo::ModelInfo m;
    m.nodes = {{"root", -1, {}}, {"arm", 0, {}}};
    m.verts = {{{0, 0, 1}, {0, 0, 1}, 0}, {{1, 0, 0}, {1, 0, 0}, 1}};
    shogo::ModelAnim a;
    a.name = "turn";
    a.length = 1000;
    a.keys = {{0, ""}, {1000, ""}};
    DRotation turn = shogo::quatAxisAngle(DVector(0, 1, 0), 3.14159265f / 2);
    a.tracks = {{{{0, 10, 0}, shogo::quatIdentity()}, {{0, 10, 0}, turn}},
                {{{5, 0, 0}, shogo::quatIdentity()}, {{5, 0, 0}, shogo::quatIdentity()}}};
    m.anims.push_back(a);
    std::vector<shogo::NodeTransform> nodes;
    std::vector<shogo::Vec3> pos, nrm;
    auto near = [](shogo::Vec3 v, float x, float y, float z) {
        return std::fabs(v.x - x) < 1e-3f && std::fabs(v.y - y) < 1e-3f && std::fabs(v.z - z) < 1e-3f;
    };
    shogo::poseModel(m, 0, 0, &nodes, &pos, &nrm);
    // Output is mirrored in z (the files are in a mirrored space).
    CHECK(near(pos[0], 0, 10, -1) && near(nrm[0], 0, 0, -1));
    CHECK(near(pos[1], 6, 10, 0) && near(nodes[1].pos, 5, 10, 0));
    shogo::poseModel(m, 0, 1000, &nodes, &pos, nullptr);
    // A 90 degree turn about y takes +x to -z (file space); mirrored: +z.
    CHECK(near(nodes[1].pos, 0, 10, 5));
    CHECK(near(pos[1], 0, 10, 6));
    shogo::poseModel(m, 0, 500, &nodes, nullptr, nullptr);  // halfway: 45 degrees
    CHECK(near(nodes[1].pos, 5 * 0.70710678f, 10, 5 * 0.70710678f));
}

static void testModels(const shogo::ResourceFS &fs)
{
    std::vector<std::string> files = fs.list("MODELS", ".ABC");
    int parsed = 0;
    for (const std::string &f : files) {
        auto data = fs.read(f);
        if (data && shogo::parseModel(*data, nullptr)) ++parsed;
    }
    printf("  models: %d of %zu parsed\n", parsed, files.size());
    CHECK(files.size() > 500 && parsed >= (int)files.size() - 2);
    auto data = fs.read("Models/Player/sanjuro.abc");
    CHECK(data.has_value());
    if (!data) return;
    auto m = shogo::parseModel(*data, nullptr);
    CHECK(m && m->anims.size() == 64);
    if (!m) return;
    CHECK(m->find("STATIC_MODEL") == 0 && m->find("rr") > 0);
    CHECK(m->anims[0].dims.x == 20 && m->anims[0].dims.y == 41 && m->anims[0].dims.z == 20);
    CHECK(m->nodes.size() == 27 && m->verts.size() == 390 && m->tris.size() == 662);
    // Standing, the posed model is about as tall as its dims, feet near -dims.y.
    std::vector<shogo::Vec3> pos;
    shogo::poseModel(*m, m->find("IR1"), 0, nullptr, &pos, nullptr);
    float lo = 1e9f, hi = -1e9f;
    for (const shogo::Vec3 &p : pos) {
        lo = std::min(lo, p.y);
        hi = std::max(hi, p.y);
    }
    CHECK(lo < -35 && lo > -48 && hi > 30 && hi < 50);
    auto turret = fs.read("Models/Enemies/OnFoot/Ruin150_turret.abc");
    auto t = turret ? shogo::parseModel(*turret, nullptr) : nullptr;
    CHECK(t && t->commandString == "0.5 1.0 1.0 3.0");
    auto cat = fs.read("Models/Props/cat.abc");  // vertex-animated tail
    auto c = cat ? shogo::parseModel(*cat, nullptr) : nullptr;
    CHECK(c && c->nodes.size() == 4 && c->nodes[3].mdVerts.size() == 50 && !c->anims.empty() &&
          c->anims[0].vertexKeys[3].size() == c->anims[0].keys.size() * 50 * 3);
    // A vertex-animated node's keyframes come first, then its vertices for
    // every keyframe: read interleaved, the rotations after the first key are
    // garbage (the Quarters hologram map then never showed).
    auto map = fs.read("Models/Props/kato_map.abc");
    auto km = map ? shogo::parseModel(*map, nullptr) : nullptr;
    CHECK(km && km->nodes.size() == 2 && km->nodes[1].mdVerts.size() == 88 && !km->anims.empty());
    if (km && !km->anims.empty() && km->anims[0].tracks.size() == 2) {
        bool unit = true;
        for (const shogo::NodeKey &k : km->anims[0].tracks[1]) {
            const DRotation &q = k.rot;
            float len = q.m_Vec.x * q.m_Vec.x + q.m_Vec.y * q.m_Vec.y + q.m_Vec.z * q.m_Vec.z + q.m_Spin * q.m_Spin;
            unit = unit && std::fabs(len - 1) < 1e-3f && std::fabs(k.pos.x) < 1e-3f;
        }
        CHECK(unit);
    }
}

// A wall facing the camera, lit 0.25 by its baked light; a light just in front
// of its middle brightens the middle and not the corners.
// World models are drawn where their objects put them: hidden ones vanish,
// moved ones move, an unmoved placement looks the same as none.
static void testWorldModelPlacement(const shogo::ResourceFS &fs)
{
    using shogo::WorldRenderer;
    auto data = fs.read("WORLDS/08_AIRSHAFTS.DAT");
    if (!data) return;
    auto world = shogo::WorldData::parse(*data, nullptr);
    CHECK(world);
    if (!world) return;
    const shogo::WorldBsp *fan = world->model("fan1");
    CHECK(fan && !fan->points.empty());
    if (!fan || fan->points.empty()) return;
    const int index = int(fan - world->models.data());
    shogo::Vec3 c{0, 0, 0};
    for (const shogo::Vec3 &p : fan->points) c = c + p;
    c = {c.x / fan->points.size(), c.y / fan->points.size(), c.z / fan->points.size()};

    WorldRenderer r;
    r.load(*world, fs);
    auto shot = [&](const shogo::Camera &cam, std::vector<WorldRenderer::ModelPlacement> pl) {
        r.setModelPlacements(std::move(pl));
        shogo::Surface s(160, 120);
        r.render(s, cam);
        return s.pixels;
    };
    auto placed = [&](bool visible, float angle) {
        WorldRenderer::ModelPlacement p;
        p.model = index;
        p.visible = visible;
        p.basePos = p.pos = c;
        const float co = std::cos(angle), si = std::sin(angle);
        const float m[9] = {co, -si, 0, si, co, 0, 0, 0, 1};  // about z
        std::copy(m, m + 9, p.rot);
        return std::vector<WorldRenderer::ModelPlacement>{p};
    };
    // Look at the fan from whichever side shows it.
    const shogo::Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    bool seen = false;
    for (const shogo::Vec3 &d : dirs) {
        shogo::Camera cam;
        cam.hasBasis = true;
        cam.pos = {c.x - d.x * 120, c.y - d.y * 120, c.z - d.z * 120};
        cam.forward = d;
        cam.up = std::fabs(d.y) > 0.5f ? shogo::Vec3{0, 0, 1} : shogo::Vec3{0, 1, 0};
        cam.right = {cam.up.y * d.z - cam.up.z * d.y, cam.up.z * d.x - cam.up.x * d.z, cam.up.x * d.y - cam.up.y * d.x};
        auto none = shot(cam, {});
        if (shot(cam, placed(false, 0)) == none) continue;
        seen = true;
        CHECK(shot(cam, placed(true, 0)) == none);
        CHECK(shot(cam, placed(true, 0.7f)) != none);
        break;
    }
    CHECK(seen);
}

static void testDynamicLights()
{
    using shogo::WorldRenderer;
    auto wall = [] {
        WorldRenderer::DrawPoly dp;
        dp.lit = true;
        dp.normal = {0, 0, -1};
        dp.sphereCenter = {0, 0, 100};
        dp.sphereRadius = 300;
        const float c[4][2] = {{-200, 200}, {200, 200}, {200, -200}, {-200, -200}};
        for (auto &k : c) {
            WorldRenderer::DrawVert v;
            v.p = {k[0], k[1], 100};
            v.u = v.v = 0;
            v.r = v.g = v.b = 0.25f;
            dp.verts.push_back(v);
        }
        return dp;
    };
    auto shot = [&](std::vector<WorldRenderer::Light> lights, float scale, float add) {
        WorldRenderer r;
        shogo::Surface dst(64, 64);
        r.setLights(std::move(lights));
        r.setLightScale(scale, scale, scale);
        r.setScreenAdd(add, add, add);
        r.addDynamic(wall(), false);
        shogo::Camera cam;
        r.render(dst, cam);
        return std::make_pair(dst.row(32)[32] & 0xFF, dst.row(12)[2] & 0xFF);  // off the diagonal seam
    };
    auto [mid0, corner0] = shot({}, 1, 0);
    CHECK(mid0 == 63 && corner0 == 63);  // 0.25 of white
    WorldRenderer::Light L;
    L.pos = {0, 0, 90};
    L.radius = 50;
    auto [mid1, corner1] = shot({L}, 1, 0);
    CHECK(mid1 > 200 && corner1 == 63);
    L.pos = {0, 0, 110};  // behind the wall
    L.skipBackfacing = true;
    auto [mid2, corner2] = shot({L}, 1, 0);
    CHECK(mid2 == 63);
    auto [mid3, corner3] = shot({}, 0.5f, 0);
    CHECK(mid3 == 31 && corner3 == 31);
    auto [mid4, corner4] = shot({}, 1, 0.5f);
    CHECK(mid4 == 63 + 127 && corner4 == 63 + 127);
    (void)corner2;
    printf("  lights: %u/%u lit %u/%u back %u scaled %u/%u tint %u/%u\n", mid0, corner0, mid1, corner1, mid2, mid3, corner3, mid4, corner4);
}

// A WAV in memory: PCM with the given channels/bits and raw sample bytes.
static std::vector<uint8_t> makeWav(int channels, int bits, int rate, const std::vector<uint8_t> &pcm)
{
    std::vector<uint8_t> w;
    auto put = [&](const void *p, size_t n) { w.insert(w.end(), (const uint8_t *)p, (const uint8_t *)p + n); };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    auto u16 = [&](uint16_t v) { put(&v, 2); };
    put("RIFF", 4);
    u32(uint32_t(36 + pcm.size()));
    put("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(uint16_t(channels));
    u32(uint32_t(rate));
    u32(uint32_t(rate * channels * bits / 8));
    u16(uint16_t(channels * bits / 8));
    u16(uint16_t(bits));
    put("data", 4);
    u32(uint32_t(pcm.size()));
    put(pcm.data(), pcm.size());
    return w;
}

static void testSound()
{
    // 8-bit mono: 128 is silence.
    auto s8 = shogo::parseWav(makeWav(1, 8, 11025, {128, 255, 0}));
    CHECK(s8 && s8->rate == 11025 && s8->samples.size() == 3 && s8->samples[0] == 0 && s8->samples[1] > 32000 &&
          s8->samples[2] == -32768);
    // 16-bit stereo mixes down to mono.
    int16_t st[4] = {1000, 3000, -2000, 0};
    std::vector<uint8_t> pcm((uint8_t *)st, (uint8_t *)st + sizeof st);
    auto s16 = shogo::parseWav(makeWav(2, 16, 22050, pcm));
    CHECK(s16 && s16->samples.size() == 2 && s16->samples[0] == 2000 && s16->samples[1] == -1000);
    CHECK(!shogo::parseWav({'R', 'I', 'F', 'F'}));

}

static void testSoundMixer(const shogo::ResourceFS &fs)
{
    shogo::Mixer m;
    m.setResources(&fs);
    const char *name = "Sounds\\Weapons\\Colt45\\fire.wav";
    const shogo::SoundData *d = m.load(name);
    CHECK(d && d->rate == 22050 && d->duration() > 0.1f);
    if (!d) return;
    m.setListener({0, 0, 0}, {1, 0, 0});

    auto energy = [&](float &l, float &r) {
        std::vector<int16_t> buf(4410 * 2);
        m.mix(buf.data(), 4410);
        double el = 0, er = 0;
        for (size_t i = 0; i < buf.size(); i += 2) {
            el += std::fabs(buf[i]);
            er += std::fabs(buf[i + 1]);
        }
        l = float(el);
        r = float(er);
    };
    PlaySoundInfo info;
    PLAYSOUNDINFO_INIT(info);
    strcpy(info.m_szSoundName, name);

    // Local: both ears alike.
    float l, r;
    uint32_t id = m.play(info, nullptr, false);
    CHECK(id != 0 && m.voiceCount() == 1);
    energy(l, r);
    CHECK(l > 0 && std::fabs(l - r) < 1);
    m.clear();

    // 3D to the right, inside the inner radius: louder on the right.
    info.m_dwFlags = PLAYSOUND_3D;
    info.m_fInnerRadius = 100;
    info.m_fOuterRadius = 400;
    info.m_vPosition = DVector(50, 0, 0);
    m.play(info, nullptr, false);
    float l3, r3;
    energy(l3, r3);
    CHECK(r3 > l3 * 2 && r3 > 0);
    m.clear();

    // Beyond the outer radius: silent.
    info.m_vPosition = DVector(0, 0, 500);
    m.play(info, nullptr, false);
    energy(l, r);
    CHECK(l == 0 && r == 0);
    m.clear();

    // Halfway between the radii: about half as loud as inside.
    info.m_dwFlags = PLAYSOUND_AMBIENT;
    info.m_vPosition = DVector(0, 0, 50);
    m.play(info, nullptr, false);
    float lin, rin;
    energy(lin, rin);
    m.clear();
    info.m_vPosition = DVector(0, 0, 250);
    m.play(info, nullptr, false);
    float lmid, rmid;
    energy(lmid, rmid);
    CHECK(lmid > lin * 0.4f && lmid < lin * 0.6f);
    m.clear();

    // Fire-and-forget sounds go away when done; handle sounds wait for kill().
    info.m_dwFlags = PLAYSOUND_LOCAL;
    uint32_t a = m.play(info, nullptr, false);
    info.m_dwFlags = PLAYSOUND_LOCAL | PLAYSOUND_GETHANDLE;
    uint32_t b = m.play(info, nullptr, false);
    info.m_dwFlags = PLAYSOUND_LOCAL | PLAYSOUND_LOOP | PLAYSOUND_GETHANDLE;
    uint32_t c = m.play(info, nullptr, false);
    int frames = int(d->duration() * shogo::Mixer::kRate) + 4410;
    std::vector<int16_t> buf(size_t(frames) * 2);
    m.mix(buf.data(), frames);
    CHECK(m.isDone(a) && m.isDone(b) && !m.isDone(c));
    CHECK(m.voiceCount() == 2);  // b (done, held) and c (looping)
    m.kill(b);
    m.kill(c);
    CHECK(m.voiceCount() == 0);
}

static void testParticles()
{
    ClientDE *c = shogo_create_stub_client();
    shogo::installEffectsAPI(c);
    shogo::EngineObject o;
    o.type = OT_PARTICLESYSTEM;
    o.id = 0x7FFF0001;
    HLOCALOBJ h = shogo::toHLocal(&o);
    char tex[] = "SpecialFX\\ParticleTextures\\smoke.dtx";
    CHECK(c->SetupParticleSystem(h, tex, -100.0f, 0, 500.0f / 640.0f) == LT_OK);

    DVector pos(1, 2, 3), vel(10, 0, 0), color(255, 128, 0);
    DEParticle *a = c->AddParticle(h, &pos, &vel, &color, 0.25f);
    DEParticle *b = c->AddParticle(h, &pos, &vel, &color, 2.0f);
    CHECK(a && b && a->m_pNext == b && b->m_pNext == nullptr);
    DVector lo(-1, -1, -1), hi(1, 1, 1);
    c->AddParticles(h, 3, &lo, &hi, &lo, &hi, &color, &color, 1.0f, 1.0f);
    CHECK(shogo::particleCount(&o) == 5);
    DEParticle *head = nullptr, *tail = nullptr;
    CHECK(c->GetParticles(h, &head, &tail) && head == a && tail && tail->m_pNext == nullptr);
    DVector p;
    c->GetParticlePos(h, a, &p);
    CHECK(p.x == 1 && p.y == 2 && p.z == 3);

    // Gravity pulls velocity down, particles move, short-lived ones die.
    shogo::updateEffects(0.5f);
    CHECK(shogo::particleCount(&o) == 4);
    CHECK(c->GetParticles(h, &head, &tail) && head == b);
    CHECK(std::fabs(b->m_Velocity.y + 50) < 0.01f);
    c->GetParticlePos(h, b, &p);
    CHECK(std::fabs(p.x - 6) < 0.01f && p.y < 2);
    c->RemoveParticle(h, b);
    CHECK(shogo::particleCount(&o) == 3);
    shogo::updateEffects(2.0f);  // the rest live 1 s
    CHECK(shogo::particleCount(&o) == 0);
    CHECK(c->GetParticles(h, &head, &tail) && head == nullptr && tail == nullptr);

    // Line systems.
    shogo::EngineObject lineObj;
    lineObj.type = OT_LINESYSTEM;
    lineObj.id = 0x7FFF0002;
    HLOCALOBJ hl = shogo::toHLocal(&lineObj);
    DELine line{};
    line.m_Points[1].m_Pos = DVector(0, 10, 0);
    line.m_Points[0].a = 1;
    HDELINE l1 = c->AddLine(hl, &line);
    line.m_Points[0].r = 0.5f;
    HDELINE l2 = c->AddLine(hl, &line);
    CHECK(c->GetNextLine(hl, nullptr) == l1 && c->GetNextLine(hl, l1) == l2 && c->GetNextLine(hl, l2) == nullptr);
    DELine got{};
    c->GetLineInfo(l2, &got);
    CHECK(got.m_Points[0].r == 0.5f && got.m_Points[1].m_Pos.y == 10);
    c->RemoveLine(hl, l1);
    CHECK(c->GetNextLine(hl, nullptr) == l2);

    shogo::effectsObjectRemoved(&o);
    shogo::effectsObjectRemoved(&lineObj);
    CHECK(c->GetNextLine(hl, nullptr) == nullptr);
}

static void testSprites(const shogo::ResourceFS &fs)
{
    // A hand-built .SPR: 2 frames at 10 fps, translucent.
    std::vector<uint8_t> spr;
    auto u32 = [&](uint32_t v) { spr.insert(spr.end(), (uint8_t *)&v, (uint8_t *)&v + 4); };
    auto str = [&](const char *t) {
        uint16_t n = (uint16_t)strlen(t);
        spr.insert(spr.end(), (uint8_t *)&n, (uint8_t *)&n + 2);
        spr.insert(spr.end(), t, t + n);
    };
    u32(2); u32(10); u32(0); u32(1); u32(0);
    str("a.dtx");
    str("b.dtx");
    auto s = shogo::parseSprite(spr);
    CHECK(s && s->frames.size() == 2 && s->fps == 10 && s->translucent && !s->transparent && s->frames[1] == "b.dtx");
    spr.pop_back();
    CHECK(!shogo::parseSprite(spr));  // truncated

    // Every sprite in the game parses, and its frames exist.
    // Two shipped files are broken (Pachinko.spr is empty, BullgutMuzzleFlash1.spr
    // has no frames), and 61 frames name textures the game doesn't have.
    size_t n = 0, bad = 0, frames = 0, missing = 0;
    for (const std::string &name : fs.list("sprites", ".spr")) {
        auto d = fs.read(name);
        auto sp = d ? shogo::parseSprite(*d) : nullptr;
        if (!sp) {
            ++bad;
            continue;
        }
        ++n;
        for (const std::string &f : sp->frames) {
            ++frames;
            if (!fs.exists(f)) ++missing;
        }
    }
    CHECK(n > 200 && bad <= 2);
    const shogo::SpriteInfo *pulse = shogo::loadSprite(fs, "Sprites\\PulseRifle.spr");
    CHECK(pulse && pulse->frames.size() == 8 && pulse->fps == 24);
    printf("  sprites: %zu parse (%zu broken), %zu frames (%zu missing)\n", n, bad, frames, missing);

    // Effect textures carry a 4-bit alpha mask: a bullet hole is round.
    auto d = fs.read("SpriteTextures\\Weapons\\Impacts\\BHolMet1.dtx");
    auto t = d ? shogo::decodeDTX(*d) : std::nullopt;
    CHECK(t && t->alpha.size() == t->mips.size());
    if (t && !t->alpha.empty()) {
        CHECK(t->alpha[0][0] == 0);                        // corner
        CHECK(t->alpha[0][16 * 32 + 16] == 255);           // middle
        CHECK(t->alpha[1].size() == 16 * 16);
    }
    size_t withAlpha = 0, total = 0;
    for (const std::string &name : fs.list("", ".dtx")) {
        auto td = fs.read(name);
        auto tt = td ? shogo::decodeDTX(*td) : std::nullopt;
        if (!tt) continue;
        ++total;
        if (!tt->alpha.empty()) ++withAlpha;
    }
    printf("  textures: %zu of %zu have an alpha mask\n", withAlpha, total);
}

// The logo movie: every frame decodes, the picture isn't empty, and the
// soundtrack lasts as long as the pictures and joins up across frames.
static void testMovies(const std::string &dir)
{
    shogo::SmackerVideo v;
    std::string err;
    if (!v.open(dir + "/logo.smk", &err)) {
        printf("  logo.smk: %s, skipped\n", err.c_str());
        return;
    }
    CHECK(v.width() == 320 && v.height() == 240 && v.frameCount() == 366);
    CHECK(std::fabs(v.frameDuration() - 1.0 / 15) < 1e-3);
    CHECK(v.audioRate() == 22050 && v.audioChannels() == 2);
    size_t samples = 0, lit = 0;
    double jump = 0, step = 0;
    size_t jumps = 0, steps = 0;
    int16_t prev = 0;
    while (v.nextFrame()) {
        const std::vector<int16_t> &a = v.audio();
        if (a.size() >= 4) {
            if (samples) jump += std::abs(a[0] - prev), ++jumps;
            for (size_t i = 2; i < a.size(); i += 2) step += std::abs(a[i] - a[i - 2]), ++steps;
            prev = a[a.size() - 2];
        }
        samples += a.size() / 2;
        if (v.frameIndex() == 150)
            for (uint8_t c : v.image()) lit += (v.palette()[c] & 0xFF) > 0x40;
    }
    CHECK(v.frameIndex() == 365);
    CHECK(std::fabs(double(samples) / 22050 - 366.0 / 15) < 0.5);
    CHECK(lit > 5000);
    // A wrong first sample per chunk would show up as jumps far above a normal step.
    CHECK(jumps > 100 && jump / double(jumps) < 2 * step / double(steps));
    printf("  logo.smk: %zu audio frames, %zu lit pixels in frame 150\n", samples, lit);

    // The soundtrack stream: 22 kHz in, 44.1 kHz out, then drained.
    shogo::Mixer m;
    std::vector<int16_t> tone(2205 * 2, 8000);
    m.streamPush(tone.data(), 2205, 22050);
    std::vector<int16_t> out(4410 * 2);
    m.mix(out.data(), 4410);
    CHECK(out[100] > 7000 && out[101] > 7000 && m.streamQueued() == 0);
    m.mix(out.data(), 100);
    CHECK(out[50] == 0);
}

// RedRiot's music: the DLS bank, a style and a section, then a few seconds played.
static void testMusic(const std::string &dir)
{
    shogo::MusicPlayer m;
    if (!m.setDirectory((dir + "/Music/RedRiot").c_str())) {
        printf("  Music/RedRiot not found, skipped\n");
        return;
    }
    CHECK(m.initInstruments("redriot.dls", "redinit.sty"));
    const shogo::DlsCollection *dls = m.instruments();
    CHECK(dls && dls->instruments.size() == 13 && dls->waves.size() == 27);
    if (dls) {
        const shogo::DlsInstrument *strings = dls->find(0, 3);
        CHECK(strings && strings->name == "Batman Strings" && strings->regions.size() == 1 && strings->regions[0].looped);
    }
    const shogo::ImaSection *sec = m.findSection("a1.sec");
    CHECK(sec && sec->style == "RedRiot4" && sec->groove == 0x100 && sec->measures == 4 && sec->tempo == 105);
    CHECK(sec && sec->hasBand && sec->band.program[1] == 3);
    const shogo::ImaStyle *sty = sec ? m.findStyle(sec->style) : nullptr;
    CHECK(sty && sty->patterns.size() == 4);
    if (sty) {
        std::mt19937 rng(1);
        const shogo::ImaPattern *p = sty->patternFor(0x100, rng);
        CHECK(p && p->name == "a1" && p->length == 3072 && p->notes.size() == 24);
    }
    CHECK(m.addToPlayList("list", "a1.sec") && m.addToPlayList("list", "c1.sec"));
    CHECK(m.playList("list", "sta.sec", false, MUSIC_IMMEDIATE));
    CHECK(m.currentSection() == "sta.sec");
    // sta + a1 + c1: 12 measures at 105 bpm.
    std::vector<float> buf(4410 * 2);
    double energy = 0;
    int blocks = 0;
    while (!m.currentSection().empty() && blocks < 400) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        m.render(buf.data(), 4410);
        for (float f : buf) energy += double(f) * f;
        ++blocks;
    }
    CHECK(blocks > 0 && std::abs(blocks * 0.1 - 12 * 4 * 60.0 / 105) < 0.25);
    CHECK(energy / (blocks * 4410.0 * 2) > 1e-4);
    printf("  music: RedRiot sta, a1, c1 in %.1f s, rms %.3f\n", blocks * 0.1, std::sqrt(energy / (blocks * 4410.0 * 2)));
}

static void testGameData(const std::string &dir)
{
    testMovies(dir);
    testMusic(dir);
    shogo::ResourceFS fs;
    std::string err;
    CHECK(fs.addArchive(dir + "/SHOGO.REZ", &err));
    CHECK(fs.exists("worlds\\01_ambush.dat"));  // case/separator-insensitive lookup
    for (const char *name : {"interface/Font28n.pcx", "INTERFACE/MENU/HALF.PCX"}) {
        auto pcx = fs.read(name);
        CHECK(pcx.has_value());
        if (pcx) {
            auto img = shogo::decodePCX(*pcx);
            CHECK(img && img->width > 0 && img->height > 0);
        }
    }
    testWorlds(fs);
    testWorldModelPlacement(fs);
    testModels(fs);
    testSprites(fs);
    {
        shogo::ResourceFS sfs;
        if (sfs.addArchive(dir + "/SOUND.REZ")) {
            testSoundMixer(sfs);
            size_t n = 0, ok = 0;
            for (const std::string &name : sfs.list("", ".wav")) {
                auto d = sfs.read(name);
                ++n;
                if (d && shogo::parseWav(*d)) ++ok;
            }
            CHECK(n > 2000 && ok == n);
            printf("  sounds: %zu of %zu decode\n", ok, n);
        }
    }
    auto cres = fs.read("CRES.DLL");
    CHECK(cres.has_value());
    if (!cres) return;
    auto res = shogo::PEResources::parse(std::move(*cres), &err);
    CHECK(res != nullptr);
    if (!res) return;
    CHECK(res->stringCount() > 1000);
    CHECK(res->named("TEXT", "credits").has_value());
    printf("  CRES.DLL: %zu strings, credits text %zu bytes\n", res->stringCount(),
           res->named("TEXT", "CREDITS") ? res->named("TEXT", "CREDITS")->size() : 0);
}


// A 3x2 8-bit PCX built by hand: RLE run + literal bytes, palette at the end.
// System fonts draw visible pixels in the requested colour (Core Text once
// drew every glyph in black into the coverage mask, so no text appeared).
static void testFontDraw()
{
    auto font = shogo::Font::create("Arial", 16, 22, false, false, false);
    CHECK(font != nullptr);
    if (!font) return;
    shogo::Surface s(200, 40);
    font->draw(s, "Hello", 0, 0, 0x00FF8000, nullptr);
    size_t lit = 0;
    for (uint32_t p : s.pixels) lit += p == 0x00FF8000;
    CHECK(lit > 50);
}

static void testPCX()
{
    std::vector<uint8_t> d(128, 0);
    d[0] = 0x0A; d[1] = 5; d[2] = 1; d[3] = 8;
    d[8] = 2;   // xmax
    d[10] = 1;  // ymax
    d[65] = 1;  // planes
    d[66] = 4;  // bytes per line (even, > width)
    const uint8_t rows[] = {0xC3, 1, 2,      // row 0: 1 1 1, pad 2
                            0xC1, 0xC5, 0xC2, 2, 0};  // row 1: 0xC5 (needs RLE escape), 2 2, pad 0
    d.insert(d.end(), std::begin(rows), std::end(rows));
    d.push_back(0x0C);
    std::vector<uint8_t> pal(768, 0);
    pal[3] = 0xFF;                       // index 1 = red
    pal[6] = 0x00; pal[7] = 0xFF;        // index 2 = green
    pal[0xC5 * 3 + 2] = 0xFF;            // index 0xC5 = blue
    d.insert(d.end(), pal.begin(), pal.end());
    auto s = shogo::decodePCX(d);
    CHECK(s && s->width == 3 && s->height == 2);
    if (!s) return;
    CHECK(s->row(0)[0] == 0xFF0000 && s->row(0)[2] == 0xFF0000);
    CHECK(s->row(1)[0] == 0x0000FF && s->row(1)[1] == 0x00FF00 && s->row(1)[2] == 0x00FF00);
}

static void testBlits()
{
    shogo::Surface src(4, 4), dst(8, 8);
    shogo::fillRect(src, nullptr, 0x123456);
    src.row(1)[1] = 0x000000;  // key-colored pixel
    // Clipped at the top-left corner.
    shogo::blit(dst, src, nullptr, -2, -2, shogo::BlitParams{});
    CHECK(dst.row(0)[0] == 0x123456 && dst.row(1)[1] == 0x123456 && dst.row(2)[2] == 0);
    // Transparent: key matches at RGB565 precision (0x030303 ~ black).
    shogo::Surface d2(4, 4);
    shogo::fillRect(d2, nullptr, 0xABCDEF);
    shogo::blit(d2, src, nullptr, 0, 0, shogo::BlitParams{shogo::BlitMode::Transparent, 0x030303, 0});
    CHECK(d2.row(1)[1] == 0xABCDEF && d2.row(0)[0] == 0x123456);
    // Source rect excludes right/bottom.
    shogo::Surface d3(4, 4);
    DRect r{0, 0, 2, 1};
    shogo::blit(d3, src, &r, 1, 1, shogo::BlitParams{});
    CHECK(d3.row(1)[1] == 0x123456 && d3.row(1)[2] == 0x123456 && d3.row(1)[3] == 0 && d3.row(2)[1] == 0);
    // Scale 2x2 -> 4x4.
    shogo::Surface small(2, 2), big(4, 4);
    small.row(0)[0] = 1; small.row(0)[1] = 2; small.row(1)[0] = 3; small.row(1)[1] = 4;
    shogo::scaleBlit(big, small, nullptr, nullptr, shogo::BlitParams{});
    CHECK(big.row(0)[1] == 1 && big.row(0)[2] == 2 && big.row(3)[0] == 3 && big.row(3)[3] == 4);
    // Border of black around a 2x1 block.
    shogo::Surface b(6, 5);
    b.row(2)[2] = b.row(2)[3] = 0xFFFFFF;
    DRect br = shogo::borderSize(b, 0);
    CHECK(br.left == 2 && br.top == 2 && br.right == 2 && br.bottom == 2);
}

static void testParse()
{
    char cmd[] = "WorldName \"Drop Point\" 3;AmbientLight 1 2 3";
    char buf[256];
    char *args[64];
    int n = 0;
    char *next = nullptr;
    CHECK(shogo::parseCommand(cmd, &next, buf, args, &n) == 1);
    CHECK(n == 3 && !strcmp(args[0], "WorldName") && !strcmp(args[1], "Drop Point") && !strcmp(args[2], "3"));
    CHECK(shogo::parseCommand(next, &next, buf, args, &n) == 0);
    CHECK(n == 4 && !strcmp(args[0], "AmbientLight") && !strcmp(args[3], "3"));
}

static void testConsoleVars()
{
    shogo::ConsoleVars vars;
    vars.run("+runworld \"Worlds\\01_Ambush\"; Gamma 1.5");
    CHECK(vars.find("RUNWORLD") && vars.find("runworld")->value == "Worlds\\01_Ambush");
    CHECK(vars.find("gamma") && std::fabs(vars.find("gamma")->number - 1.5f) < 1e-6f);
    CHECK(!vars.find("missing"));

    vars.run("ModelAdd 0.5 0 1");
    CHECK(vars.find("modeladd") && vars.find("modeladd")->args.size() == 3 && vars.find("modeladd")->args[0] == "0.5");
    vars.run("ModelAdd 1");
    CHECK(vars.find("modeladd")->args.empty());
}

// Bindings written by Input::configLines read back to the same bindings.
static void testInputConfigLines()
{
    shogo::ConsoleVars vars;
    shogo::Input input;
    vars.commands = [&](const std::vector<std::string> &a) { return input.command(a); };
    vars.run("AddAction Fire 1; AddAction Jump 2; AddAction Axis1 -1");
    vars.run("enabledevice \"##keyboard\"; rangebind \"##keyboard\" \"##57\" 0 0 \"Jump\" 0 0 \"Fire\"");
    vars.run("rangebind \"##mouse\" \"Button 0\" 0 0 \"Fire\"; rangebind \"##mouse\" \"X Axis\" 0 0 \"Axis1\"");
    vars.run("scale \"##mouse\" \"X Axis\" 0.004625; rangebind \"##joystick 1\" \"Button 2\" 0 0 \"Jump\"");
    vars.run("rangebind \"##keyboard\" \"1\" 0 0 \"Fire\"; rangebind \"##keyboard\" \"1\" 0 0 \"Jump\"; rangebind \"\" \"2\" 0 0 \"Jump\"");
    std::string lines = input.configLines();
    // Key names are bound (and written) by scan code; the last rangebind wins.
    CHECK(lines.find("rangebind \"##keyboard\" \"##2\" 0.000000 0.000000 \"Jump\"\n") != std::string::npos);
    CHECK(lines.find("rangebind \"\"") == std::string::npos);
    CHECK(lines.find("AddAction Fire 1\n") != std::string::npos);
    CHECK(lines.find("rangebind \"##keyboard\" \"##57\" 0.000000 0.000000 \"Jump\" 0.000000 0.000000 \"Fire\"") != std::string::npos);
    CHECK(lines.find("scale \"##mouse\" \"X Axis\" 0.004625") != std::string::npos);
    CHECK(lines.find("rangebind \"##joystick 1\" \"Button 2\"") != std::string::npos);

    shogo::ConsoleVars vars2;
    shogo::Input input2;
    vars2.commands = [&](const std::vector<std::string> &a) { return input2.command(a); };
    size_t at = 0;
    while (at < lines.size()) {
        size_t end = lines.find('\n', at);
        vars2.run(lines.substr(at, end - at));
        at = end + 1;
    }
    CHECK(input2.configLines() == lines);
    CHECK(input2.bindingCount() == input.bindingCount() && input.bindingCount() == 5);

    // What the controls menus see.
    CHECK(shogo::Input::keyName(57) == "Space" && shogo::Input::keyCode("space") == 57 && shogo::Input::keyCode("Nope") == 0);
    std::vector<shogo::Input::Trigger> keys = input.triggers(false);
    CHECK(keys.size() == 2 && keys[0].name == "Space" && keys[0].actions.size() == 2 && keys[0].actions[0].second == "Jump");
    std::vector<shogo::Input::Trigger> mouse = input.triggers(true);
    CHECK(mouse.size() == 2 && mouse[0].name == "Left Button" && mouse[1].name == "X Axis" && std::fabs(mouse[1].scale - 0.004625f) < 1e-6f);

    // Rebinding by name, as the keyboard menu does.
    vars.run("rangebind \"##keyboard\" \"Space\" 0 0 \"\"; rangebind \"##keyboard\" \"Up\" 0 0 \"Jump\"");
    keys = input.triggers(false);
    CHECK(keys.size() == 2 && keys[1].name == "Up" && keys[1].code == 0xC8);

    // Mouse buttons by name, as the keyboard menu binds them.
    vars.run("rangebind \"##mouse\" \"Right Button\" 0 0 \"Jump\"");
    mouse = input.triggers(true);
    CHECK(mouse.size() == 3 && mouse[2].name == "Right Button" && !mouse[2].axis && mouse[2].code == 1);

    // Tracking: key and button presses only while tracking.
    input.keyDown(0x1E);
    input.startTrack(true, false);
    input.keyDown(0x1E);
    input.mouseButton(1, true);
    std::vector<shogo::Input::Tracked> t = input.takeTracked();
    CHECK(t.size() == 1 && !t[0].mouse && t[0].code == 0x1E);
    input.endTrack();
    input.keyDown(0x1F);
    CHECK(input.takeTracked().empty());

    // The wheel as two buttons: each notch presses one for a frame, with a
    // frame up between notches; the menu can bind them by name.
    vars.run("rangebind \"##mouse\" \"Wheel Up\" 0 0 \"Fire\"; rangebind \"##mouse\" \"Wheel Down\" 0 0 \"Jump\"");
    input.keyUp(0x1E);
    input.keyUp(0x1F);
    input.mouseButton(1, false);
    input.mouseWheel(2);
    input.mouseWheel(-0.6f);  // a trackpad's fractions add up
    input.mouseWheel(-0.6f);
    std::string seq;
    for (int f = 0; f < 5; ++f) {
        input.step();
        seq += input.isCommandOn(1) ? 'F' : '.';
        seq += input.isCommandOn(2) ? 'J' : '.';
    }
    CHECK(seq == "FJ..F.....");
    input.startTrack(false, true);
    input.mouseWheel(-1);
    t = input.takeTracked();
    CHECK(t.size() == 1 && t[0].mouse && t[0].code == shogo::Input::kWheelDown &&
          shogo::Input::mouseObjectName(false, t[0].code) == "Wheel Down");
    input.endTrack();
}

static bool near(const DVector &a, float x, float y, float z) { return std::fabs(a.x - x) + std::fabs(a.y - y) + std::fabs(a.z - z) < 1e-4f; }

static void testRotations()
{
    // Basis vectors must match the world renderer's Euler convention.
    float p = 0.3f, y = 1.1f;
    DVector u, r, f;
    shogo::quatVectors(shogo::quatEuler(p, y, 0), &u, &r, &f);
    CHECK(near(r, std::cos(y), 0, -std::sin(y)));
    CHECK(near(u, std::sin(y) * std::sin(p), std::cos(p), std::cos(y) * std::sin(p)));
    CHECK(near(f, std::sin(y) * std::cos(p), -std::sin(p), std::cos(y) * std::cos(p)));
    // AlignRotation round trip.
    DRotation q;
    shogo::alignRotation(&q, &f, &u);
    DVector u2, r2, f2;
    shogo::quatVectors(q, &u2, &r2, &f2);
    CHECK(near(f2, f.x, f.y, f.z) && near(u2, u.x, u.y, u.z));
}

static void testMessages()
{
    ClientDE *client = shogo_create_stub_client();
    shogo::installMessageAPI(client, nullptr, nullptr, nullptr);
    auto *m = new shogo::Message;
    HMESSAGEWRITE w = shogo::toWriteHandle(m);
    DVector v(1, 2, 3);
    client->WriteToMessageFloat(w, 2.5f);
    client->WriteToMessageString(w, const_cast<char *>("hello"));
    client->WriteToMessageVector(w, &v);
    m->pos = 0;
    HMESSAGEREAD rd = shogo::toReadHandle(m);
    CHECK(client->ReadFromMessageFloat(rd) == 2.5f);
    CHECK(!strcmp(client->ReadFromMessageString(rd), "hello"));
    DVector v2;
    client->ReadFromMessageVector(rd, &v2);
    CHECK(near(v2, 1, 2, 3));
    delete m;
}

static std::vector<uint8_t> readFile(const std::string &path)
{
    std::vector<uint8_t> out;
    if (FILE *f = fopen(path.c_str(), "rb")) {
        uint8_t buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
        fclose(f);
    }
    return out;
}

// GOG's installer (SHOGO_INSTALLER): the file list, a file's parts, and the
// filtered executables (their SHA-1 is checked after the filter is undone).
static void testInstaller(const std::string &installer, const char *gameData)
{
    shogo::InnoSetup setup;
    std::string err;
    CHECK(setup.open(installer, &err));
    if (!err.empty()) printf("  %s\n", err.c_str());
    CHECK(setup.appName() == "Shogo - Mobile Armor Division");
    std::vector<shogo::GogFile> files = shogo::gogFiles(setup, &err);
    CHECK(files.size() > 300);
    const shogo::GogFile *rez = nullptr, *autoexec = nullptr;
    for (const shogo::GogFile &f : files) {
        if (f.path == "SHOGO.REZ") rez = &f;
        if (f.path == "__support/app/autoexec.cfg") autoexec = &f;
    }
    CHECK(rez && rez->parts.size() == 24 && rez->size > 200u * 1024 * 1024);
    CHECK(autoexec != nullptr);
    int filtered = 0;
    for (uint32_t i = 0; i < setup.data().size(); ++i)
        if (setup.data()[i].callFilter) {
            ++filtered;
            CHECK(setup.read(i, [](const uint8_t *, size_t) { return true; }, &err));
        }
    CHECK(filtered > 0);
    if (autoexec && gameData) {
        std::string out = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp/") + "/shogo-test-autoexec.cfg";
        CHECK(shogo::extractGogFile(setup, *autoexec, out, nullptr, &err));
        // gamedata/ from innoextract keeps it under __support/app; our import copies it up.
        std::vector<uint8_t> a = readFile(out), b = readFile(std::string(gameData) + "/__support/app/autoexec.cfg");
        if (b.empty()) b = readFile(std::string(gameData) + "/autoexec.cfg");
        CHECK(!a.empty() && a == b);
        remove(out.c_str());
    }
    // GOG's icon (the Dock icon) decodes at its largest size, with alpha.
    if (gameData) {
        std::vector<uint8_t> ico = readFile(std::string(gameData) + "/goggame-1207658659.ico");
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        CHECK(!ico.empty() && shogo::decodeImageRGBA(ico, w, h, rgba) && w == 256 && h == 256);
        CHECK(!rgba.empty() && rgba[3] == 0 && rgba[(128 * 256 + 128) * 4 + 3] == 255);  // round: clear corner, solid centre
    }
    // Something that isn't an installer is refused with a reason.
    shogo::InnoSetup notOne;
    err.clear();
    CHECK(!notOne.open(gameData ? std::string(gameData) + "/console.pcx" : "/etc/hosts", &err) && !err.empty());
}

int main()
{
    testFormatMessage();
    testFontDraw();
    testPCX();
    testBlits();
    testRefCounting();
    testParse();
    testConsoleVars();
    testInputConfigLines();
    testRotations();
    testMessages();
    testCollision();
    testModelAnimation();
    testPose();
    testParticles();
    testDynamicLights();
    testSound();
    if (const char *dir = getenv("SHOGO_DATA")) {
        printf("Game data tests using %s\n", dir);
        testGameData(dir);
    } else {
        printf("SHOGO_DATA not set: skipping game data tests\n");
    }
    if (const char *installer = getenv("SHOGO_INSTALLER"); installer && *installer) {
        printf("Installer tests using %s\n", installer);
        testInstaller(installer, getenv("SHOGO_DATA"));
    } else {
        printf("SHOGO_INSTALLER not set: skipping installer tests\n");
    }
    printf(g_failures ? "%d check(s) FAILED\n" : "All checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
