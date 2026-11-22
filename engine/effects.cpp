#include "engine/effects.h"

#include "engine/collision.h"
#include "engine/math3d.h"
#include "engine/objects.h"
#include "engine/render_world.h"
#include "engine/rez_file.h"
#include "engine/stub_support.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <strings.h>
#include <unordered_map>

namespace shogo {

// ---- sprite files ------------------------------------------------------------------------------

std::unique_ptr<SpriteInfo> parseSprite(const std::vector<uint8_t> &d)
{
    if (d.size() < 20) return nullptr;
    uint32_t h[5];
    memcpy(h, d.data(), 20);
    if (h[0] == 0 || h[0] > 4096) return nullptr;
    auto s = std::make_unique<SpriteInfo>();
    s->fps = (float)h[1];
    s->transparent = h[2] != 0;
    s->translucent = h[3] != 0;
    s->colorKey = h[4];
    size_t p = 20;
    for (uint32_t i = 0; i < h[0]; ++i) {
        if (p + 2 > d.size()) return nullptr;
        uint16_t len;
        memcpy(&len, &d[p], 2);
        p += 2;
        if (p + len > d.size()) return nullptr;
        s->frames.emplace_back((const char *)&d[p], len);
        p += len;
    }
    return s;
}

namespace {
bool endsWith(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}
}  // namespace

const SpriteInfo *loadSprite(const ResourceFS &fs, const std::string &name)
{
    static std::unordered_map<std::string, std::unique_ptr<SpriteInfo>> cache;
    std::string key = RezFile::key(name);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    std::unique_ptr<SpriteInfo> s;
    if (endsWith(name, ".spr")) {
        if (auto d = fs.read(name)) s = parseSprite(*d);
    } else if (fs.exists(name)) {
        s = std::make_unique<SpriteInfo>();  // a plain texture: one frame, black see-through
        s->frames.push_back(name);
        s->transparent = true;
    }
    if (s && getenv("SHOGO_TRACE_FX"))
        fprintf(stderr, "[fx] sprite %s: %zu frames, %.0f fps, transparent %d, translucent %d, key %08x, first %s\n",
                name.c_str(), s->frames.size(), s->fps, s->transparent, s->translucent, s->colorKey,
                s->frames.empty() ? "-" : s->frames[0].c_str());
    return cache.emplace(key, std::move(s)).first->second.get();
}

// ---- particle and line systems -----------------------------------------------------------------

namespace {

// The game sees DEParticle (velocity, color, next); the engine keeps the rest.
struct Particle {
    DEParticle pub;  // first: DEParticle* and Particle* are the same pointer
    DVector pos;     // relative to the system
    float life = 0;
    Particle *prev = nullptr;
};

struct ParticleSystem {
    std::string texture = "SpecialFX\\ParticleTextures\\particle.dtx";
    float gravity = -500.0f;
    DDWORD flags = 0;
    float radius = 300.0f / 640.0f;
    float swColor[3] = {1, 1, 1};
    Particle *head = nullptr, *tail = nullptr;
    size_t count = 0;

    ~ParticleSystem()
    {
        for (Particle *p = head; p;) {
            Particle *n = reinterpret_cast<Particle *>(p->pub.m_pNext);
            delete p;
            p = n;
        }
    }
    void unlink(Particle *p)
    {
        Particle *next = reinterpret_cast<Particle *>(p->pub.m_pNext);
        if (p->prev) p->prev->pub.m_pNext = p->pub.m_pNext;
        else head = next;
        if (next) next->prev = p->prev;
        else tail = p->prev;
        delete p;
        --count;
    }
};

struct Line {
    DELine line;
    Line *next = nullptr, *prev = nullptr;
    EngineObject *owner = nullptr;
};

struct LineSystem {
    Line *head = nullptr, *tail = nullptr;
    ~LineSystem()
    {
        for (Line *l = head; l;) {
            Line *n = l->next;
            delete l;
            l = n;
        }
    }
};

std::unordered_map<uint32_t, std::unique_ptr<ParticleSystem>> g_particles;
std::unordered_map<uint32_t, std::unique_ptr<LineSystem>> g_lines;

ParticleSystem *particles(HLOCALOBJ h, bool create)
{
    EngineObject *o = toObject(h);
    if (!o || o->type != OT_PARTICLESYSTEM) return nullptr;
    auto it = g_particles.find(o->id);
    if (it != g_particles.end()) return it->second.get();
    if (!create) return nullptr;
    return g_particles.emplace(o->id, std::make_unique<ParticleSystem>()).first->second.get();
}

LineSystem *lines(HLOCALOBJ h, bool create)
{
    EngineObject *o = toObject(h);
    if (!o || o->type != OT_LINESYSTEM) return nullptr;
    auto it = g_lines.find(o->id);
    if (it != g_lines.end()) return it->second.get();
    if (!create) return nullptr;
    return g_lines.emplace(o->id, std::make_unique<LineSystem>()).first->second.get();
}

float frand(float a, float b) { return a + (b - a) * (float)rand() / (float)RAND_MAX; }

DRESULT setupParticleSystem(HLOCALOBJ h, char *texture, float gravity, DDWORD flags, float radius)
{
    ParticleSystem *ps = particles(h, true);
    if (!ps) return LT_ERROR;
    if (texture) ps->texture = texture;
    ps->gravity = gravity;
    ps->flags = flags;
    ps->radius = radius;
    return LT_OK;
}

DRESULT setSoftwarePSColor(HLOCALOBJ h, float r, float g, float b)
{
    ParticleSystem *ps = particles(h, true);
    if (!ps) return LT_ERROR;
    ps->swColor[0] = r;
    ps->swColor[1] = g;
    ps->swColor[2] = b;
    return LT_OK;
}

DEParticle *addParticleTo(ParticleSystem *ps, const DVector &pos, const DVector &vel, const DVector &color, float life)
{
    auto *p = new Particle;
    p->pub.m_Velocity = vel;
    p->pub.m_Color = color;
    p->pub.m_pNext = nullptr;
    p->pos = pos;
    p->life = life;
    p->prev = ps->tail;
    if (ps->tail) ps->tail->pub.m_pNext = &p->pub;
    else ps->head = p;
    ps->tail = p;
    ++ps->count;
    return &p->pub;
}

DEParticle *vAddParticle(void *, HLOCALOBJ h, DVector *pos, DVector *vel, DVector *color, float life)
{
    ParticleSystem *ps = particles(h, true);
    if (!ps) return nullptr;
    return addParticleTo(ps, pos ? *pos : DVector(0, 0, 0), vel ? *vel : DVector(0, 0, 0),
                         color ? *color : DVector(255, 255, 255), life);
}

void addParticles(HLOCALOBJ h, DDWORD n, DVector *minOff, DVector *maxOff, DVector *minVel, DVector *maxVel,
                  DVector *minColor, DVector *maxColor, float minLife, float maxLife)
{
    ParticleSystem *ps = particles(h, true);
    if (!ps) return;
    auto rv = [](DVector *a, DVector *b) {
        DVector lo = a ? *a : DVector(0, 0, 0), hi = b ? *b : lo;
        return DVector(frand(lo.x, hi.x), frand(lo.y, hi.y), frand(lo.z, hi.z));
    };
    for (DDWORD i = 0; i < n && i < 100000; ++i)
        addParticleTo(ps, rv(minOff, maxOff), rv(minVel, maxVel), rv(minColor, maxColor), frand(minLife, maxLife));
}

DBOOL getParticles(HLOCALOBJ h, DEParticle **head, DEParticle **tail)
{
    ParticleSystem *ps = particles(h, false);
    if (head) *head = ps && ps->head ? &ps->head->pub : nullptr;
    if (tail) *tail = ps && ps->tail ? &ps->tail->pub : nullptr;
    return ps != nullptr;
}

void getParticlePos(HLOCALOBJ, DEParticle *p, DVector *pos)
{
    if (p && pos) *pos = reinterpret_cast<Particle *>(p)->pos;
}

void setParticlePos(HLOCALOBJ, DEParticle *p, DVector *pos)
{
    if (p && pos) reinterpret_cast<Particle *>(p)->pos = *pos;
}

void removeParticle(HLOCALOBJ h, DEParticle *p)
{
    ParticleSystem *ps = particles(h, false);
    if (ps && p) ps->unlink(reinterpret_cast<Particle *>(p));
}

DRESULT optimizeParticles(HLOCALOBJ) { return LT_OK; }

HDELINE getNextLine(HLOCALOBJ h, HDELINE prev)
{
    if (prev) return reinterpret_cast<HDELINE>(reinterpret_cast<Line *>(prev)->next);
    LineSystem *ls = lines(h, false);
    return ls ? reinterpret_cast<HDELINE>(ls->head) : nullptr;
}

void getLineInfo(HDELINE h, DELine *out)
{
    if (h && out) *out = reinterpret_cast<Line *>(h)->line;
}

void setLineInfo(HDELINE h, DELine *in)
{
    if (h && in) reinterpret_cast<Line *>(h)->line = *in;
}

HDELINE addLine(HLOCALOBJ h, DELine *in)
{
    LineSystem *ls = lines(h, true);
    if (!ls || !in) return nullptr;
    auto *l = new Line;
    l->line = *in;
    l->prev = ls->tail;
    if (ls->tail) ls->tail->next = l;
    else ls->head = l;
    ls->tail = l;
    return reinterpret_cast<HDELINE>(l);
}

void removeLine(HLOCALOBJ h, HDELINE hl)
{
    LineSystem *ls = lines(h, false);
    auto *l = reinterpret_cast<Line *>(hl);
    if (!ls || !l) return;
    if (l->prev) l->prev->next = l->next;
    else ls->head = l->next;
    if (l->next) l->next->prev = l->prev;
    else ls->tail = l->prev;
    delete l;
}

}  // namespace

size_t particleCount(EngineObject *o)
{
    auto it = o ? g_particles.find(o->id) : g_particles.end();
    return it == g_particles.end() ? 0 : it->second->count;
}

void effectsObjectRemoved(EngineObject *o)
{
    g_particles.erase(o->id);
    g_lines.erase(o->id);
}

void updateEffects(float dt)
{
    for (EngineObject *o : objects().all()) {
        if (o->removed) continue;
        if (o->type == OT_SPRITE) o->animTime += dt * 1000.0f;
    }
    for (auto &kv : g_particles) {
        ParticleSystem &ps = *kv.second;
        if (ps.flags & PS_DUMB) continue;
        for (Particle *p = ps.head; p;) {
            Particle *next = reinterpret_cast<Particle *>(p->pub.m_pNext);
            p->pub.m_Velocity.y += ps.gravity * dt;
            p->pos.x += p->pub.m_Velocity.x * dt;
            p->pos.y += p->pub.m_Velocity.y * dt;
            p->pos.z += p->pub.m_Velocity.z * dt;
            if (!(ps.flags & PS_NEVERDIE)) {
                p->life -= dt;
                if (p->life <= 0) ps.unlink(p);
            }
            p = next;
        }
    }
}

// ---- drawing -----------------------------------------------------------------------------------

namespace {

void quad(WorldRenderer &r, const Vec3 &c, const Vec3 &ax, const Vec3 &ay, int tex, int tw, int th, float cr, float cg,
          float cb, WorldRenderer::Blend blend, float alpha, bool colorKey, bool noZ, bool close, bool round = false)
{
    WorldRenderer::DrawPoly dp;
    dp.round = round;
    dp.tex = tex;
    dp.blend = blend;
    dp.alpha = alpha;
    dp.colorKey = colorKey;
    dp.noZ = noZ;
    const float corners[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (const auto &k : corners) {
        WorldRenderer::DrawVert v;
        v.p = c + ax * k[0] + ay * k[1];
        v.u = (k[0] + 1) * 0.5f * (float)tw;
        v.v = (1 - k[1]) * 0.5f * (float)th;
        // Texels repeat: keep the far edges just inside the texture.
        v.u = std::min(v.u, (float)tw - 0.01f);
        v.v = std::min(v.v, (float)th - 0.01f);
        v.r = cr;
        v.g = cg;
        v.b = cb;
        dp.verts.push_back(v);
    }
    r.addDynamic(std::move(dp), close);
}

Vec3 rotated(const DRotation &q, const Vec3 &v)
{
    DVector r = quatRotate(q, DVector(v.x, v.y, v.z));
    return {r.x, r.y, r.z};
}

void drawSprite(WorldRenderer &r, const ResourceFS &fs, EngineObject *o, const Vec3 &eye, const Vec3 &right,
                const Vec3 &up)
{
    const SpriteInfo *s = loadSprite(fs, o->filename);
    if (!s || s->frames.empty()) return;
    size_t frame = 0;
    if (s->fps > 0 && s->frames.size() > 1)
        frame = (size_t)(o->animTime * 0.001f * s->fps) % s->frames.size();
    int tw = 1, th = 1;
    bool masked = false;
    int tex = r.texture(s->frames[frame], &tw, &th, &masked);
    if (tex < 0) return;
    DVector sc = o->scale;
    if (sc.x == 0 && sc.y == 0) sc = DVector(1, 1, 1);
    Vec3 c{o->pos.x, o->pos.y, o->pos.z};
    Vec3 ax = right, ay = up;
    if (o->flags & FLAG_ROTATEABLESPRITE) {
        ax = rotated(o->rot, {1, 0, 0});
        ay = rotated(o->rot, {0, 1, 0});
    }
    ax = ax * (tw * sc.x * 0.5f);
    ay = ay * (th * sc.y * 0.5f);
    if (o->flags & FLAG_SPRITEBIAS) {
        // Pull it toward the viewer so it doesn't cut into walls as much.
        Vec3 toEye = eye - c;
        float d = length(toEye);
        if (d > 1) c = c + toEye * (std::min(d * 0.5f, length(ax) + length(ay)) / d);
    }
    WorldRenderer::Blend blend = WorldRenderer::Opaque;
    float alpha = o->color[3];
    if (s->translucent) blend = WorldRenderer::Additive;
    else if (alpha < 0.99f) blend = WorldRenderer::Alpha;
    // A sprite marked neither transparent nor translucent is still see-through
    // where its frame has an alpha mask: those are glows (the Mac-10's muzzle
    // flash, flares, the spot light), drawn as black squares when opaque. The
    // unmasked ones (consoles, switches, signs) are solid panels.
    else if (masked && !s->transparent) blend = WorldRenderer::Alpha;
    // Never in the "really close" (view weapon) pass: on sprites that flag bit is
    // FLAG_SPRITEBIAS, handled above, and the close pass draws over the walls.
    quad(r, c, ax, ay, tex, tw, th, o->color[0], o->color[1], o->color[2], blend, alpha,
         s->transparent || s->translucent, (o->flags & FLAG_SPRITE_NOZ) != 0, false);
}

void drawParticles(WorldRenderer &r, const ResourceFS &fs, EngineObject *o, const ParticleSystem &ps,
                   const Vec3 &right, const Vec3 &up)
{
    if (!ps.head) return;
    // The texture may be a sprite: use its first frame.
    std::string texName = ps.texture;
    if (endsWith(texName, ".spr"))
        if (const SpriteInfo *s = loadSprite(fs, texName); s && !s->frames.empty()) texName = s->frames[0];
    int tw = 1, th = 1;
    int tex = r.texture(texName, &tw, &th);
    if (!o->traced && getenv("SHOGO_TRACE_FX")) {
        o->traced = true;
        fprintf(stderr, "[fx] particles tex %s (%d, %dx%d) radius %.3f gravity %.0f flags %x scale %.2f n %zu\n",
                texName.c_str(), tex, tw, th, ps.radius, ps.gravity, ps.flags, o->scale.x, ps.count);
    }
    DVector sc = o->scale;
    if (sc.x == 0 && sc.y == 0 && sc.z == 0) sc = DVector(1, 1, 1);
    const float size = ps.radius * kParticleWorldScale * sc.x;
    const Vec3 ax = right * size, ay = up * size;
    const Vec3 origin{o->pos.x, o->pos.y, o->pos.z};
    const bool identity = o->rot.m_Vec.x == 0 && o->rot.m_Vec.y == 0 && o->rot.m_Vec.z == 0;
    for (const Particle *p = ps.head; p; p = reinterpret_cast<const Particle *>(p->pub.m_pNext)) {
        Vec3 lp{p->pos.x, p->pos.y, p->pos.z};
        Vec3 c = origin + (identity ? lp : rotated(o->rot, lp));
        const DVector &col = p->pub.m_Color;
        quad(r, c, ax, ay, tex, tw, th, col.x / 255.0f * o->color[0], col.y / 255.0f * o->color[1],
             col.z / 255.0f * o->color[2], WorldRenderer::Additive, o->color[3], true, false, false, true);
    }
}

void drawLines(WorldRenderer &r, EngineObject *o, const LineSystem &ls, const Vec3 &eye)
{
    const Vec3 origin{o->pos.x, o->pos.y, o->pos.z};
    for (const Line *l = ls.head; l; l = l->next) {
        const DELinePt &a = l->line.m_Points[0], &b = l->line.m_Points[1];
        Vec3 pa = origin + rotated(o->rot, {a.m_Pos.x, a.m_Pos.y, a.m_Pos.z});
        Vec3 pb = origin + rotated(o->rot, {b.m_Pos.x, b.m_Pos.y, b.m_Pos.z});
        // A thin ribbon facing the viewer, about a pixel wide.
        Vec3 dir = pb - pa, mid = (pa + pb) * 0.5f;
        Vec3 side = normalized(cross(dir, mid - eye));
        float w = std::max(0.05f, length(mid - eye) * 0.0015f);
        if (length(side) < 0.5f) continue;
        side = side * w;
        WorldRenderer::DrawPoly dp;
        dp.blend = WorldRenderer::Additive;
        dp.alpha = (a.a + b.a) * 0.5f * o->color[3];
        const Vec3 pts[4] = {pa - side, pa + side, pb + side, pb - side};
        const DELinePt *ends[4] = {&a, &a, &b, &b};
        for (int k = 0; k < 4; ++k) {
            WorldRenderer::DrawVert v;
            v.p = pts[k];
            v.u = v.v = 0;
            v.r = ends[k]->r * o->color[0];
            v.g = ends[k]->g * o->color[1];
            v.b = ends[k]->b * o->color[2];
            dp.verts.push_back(v);
        }
        r.addDynamic(std::move(dp), (o->flags & FLAG_REALLYCLOSE) != 0);
    }
}

}  // namespace

void submitEffects(WorldRenderer &r, const ResourceFS &fs, const Vec3 &eye, const Vec3 &right, const Vec3 &up)
{
    static const bool off = getenv("SHOGO_NOEFFECTS") != nullptr;
    static const bool trace = getenv("SHOGO_TRACE_FX") != nullptr;
    if (off) return;
    if (trace) {
        static int frame = 0;
        if (++frame % 30 == 0)
            for (EngineObject *o : objects().all()) {
                if (o->removed || !(o->flags & FLAG_VISIBLE)) continue;
                if (o->type != OT_SPRITE && o->type != OT_PARTICLESYSTEM && o->type != OT_LINESYSTEM) continue;
                size_t n = 0;
                if (auto it = g_lines.find(o->id); it != g_lines.end())
                    for (Line *l = it->second->head; l; l = l->next) ++n;
                fprintf(stderr, "[fx] id=%u type=%d file=%s pos=(%.0f,%.0f,%.0f) scale=(%.3f,%.3f) flags=%x color=(%.2f,%.2f,%.2f,%.2f) parts=%zu lines=%zu\n",
                        o->id, o->type, o->filename.c_str(), o->pos.x, o->pos.y, o->pos.z, o->scale.x, o->scale.y,
                        o->flags, o->color[0], o->color[1], o->color[2], o->color[3], particleCount(o), n);
            }
    }
    for (EngineObject *o : objects().all()) {
        if (o->removed || !(o->flags & FLAG_VISIBLE)) continue;
        if (o->type == OT_SPRITE) {
            drawSprite(r, fs, o, eye, right, up);
        } else if (o->type == OT_PARTICLESYSTEM) {
            auto it = g_particles.find(o->id);
            if (it != g_particles.end()) drawParticles(r, fs, o, *it->second, right, up);
        } else if (o->type == OT_LINESYSTEM) {
            auto it = g_lines.find(o->id);
            if (it != g_lines.end()) drawLines(r, o, *it->second, eye);
        }
    }
}

void installEffectsAPI(ClientDE *c)
{
    c->SetupParticleSystem = setupParticleSystem;
    c->SetSoftwarePSColor = setSoftwarePSColor;
    c->AddParticles = addParticles;
    c->GetParticles = getParticles;
    c->GetParticlePos = getParticlePos;
    c->SetParticlePos = setParticlePos;
    c->RemoveParticle = removeParticle;
    c->OptimizeParticles = optimizeParticles;
    c->GetNextLine = getNextLine;
    c->GetLineInfo = getLineInfo;
    c->SetLineInfo = setLineInfo;
    c->AddLine = addLine;
    c->RemoveLine = removeLine;
    shogo_set_virtual_hook("ClientDE::AddParticle", (void *)&vAddParticle);
}

}  // namespace shogo
