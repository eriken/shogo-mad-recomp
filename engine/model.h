// Models (.ABC): the animation list a model object needs for game logic:
// animation names, lengths, user dims and string keys. (Geometry and drawing
// come later.) Also the per-object animation clock.
//
// .ABC layout, worked out from the game's data: a chain of sections, each
// "str16 name, u32 absolute offset of the next section (0xFFFFFFFF = last)".
// See docs/abc-format.md.
#ifndef SHOGO_ENGINE_MODEL_H
#define SHOGO_ENGINE_MODEL_H

#include "engine/api.h"
#include "engine/world_dat.h"  // Vec3

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace shogo {

class ResourceFS;
struct EngineObject;

struct ModelKey {
    uint32_t time = 0;  // ms into the animation
    std::string text;   // empty for most keyframes
};

// A node's place at one keyframe, relative to its parent. Rotations are
// stored in the file as the inverse; these are already turned around.
struct NodeKey {
    Vec3 pos;
    DRotation rot;
};

struct ModelAnim {
    std::string name;
    uint32_t length = 0;  // ms
    DVector dims{};       // user dims (half extents)
    std::vector<ModelKey> keys;
    // Per node (tree order): one NodeKey per keyframe, and for nodes with
    // vertex animation, 3 bytes per animated vertex per keyframe, decoded
    // as byte * scale + translate (node space).
    std::vector<std::vector<NodeKey>> tracks;
    std::vector<std::vector<uint8_t>> vertexKeys;
    std::vector<Vec3> vertexScale, vertexTranslate;
};

struct ModelNode {
    std::string name;
    int parent = -1;                // tree order index
    std::vector<uint16_t> mdVerts;  // vertices this node animates directly
};

struct ModelVertex {
    Vec3 pos;     // in its node's space
    Vec3 normal;  // in its node's space
    int node = 0; // tree order index
};

struct ModelTri {
    uint16_t v[3];
    float uv[6];  // 0..1 across the skin
};

struct ModelInfo {
    std::string commandString;  // from the Header section ("DrawIndexedDist 3000", weapon timings)
    std::vector<ModelAnim> anims;
    std::vector<ModelNode> nodes;
    std::vector<ModelVertex> verts;
    std::vector<ModelTri> tris;
    int find(const char *name) const;      // animation index, -1 if missing (case-insensitive)
    int findNode(const char *name) const;  // node index, -1 if missing
};

// A node's transform in model space.
struct NodeTransform {
    Vec3 pos;
    DRotation rot;
};

// Poses a model: node transforms at `timeMs` into animation `anim` (keyframes
// interpolated), and every vertex position and normal in model space. Output
// is in the engine's space (the file's data mirrored in z).
// A pose to blend from: animation `anim` at `timeMs`, weighted `weight`
// (1 = all of it, 0 = none).
struct PoseBlend {
    int anim = -1;
    float timeMs = 0, weight = 0;
};
void poseModel(const ModelInfo &m, int anim, float timeMs, std::vector<NodeTransform> *nodes,
               std::vector<Vec3> *positions, std::vector<Vec3> *normals, const PoseBlend *from = nullptr);

// FLAG_ANIMTRANSITION's length ("a 200ms transition between model animations").
constexpr float kAnimTransitionMs = 200.0f;

// poseModel for an object: its animation, blended from the one it left while
// FLAG_ANIMTRANSITION's transition runs.
void poseObject(EngineObject *o, std::vector<NodeTransform> *nodes, std::vector<Vec3> *positions,
                std::vector<Vec3> *normals);

// Parses an .ABC file's animation data. Returns nullptr on malformed data.
std::unique_ptr<ModelInfo> parseModel(const std::vector<uint8_t> &data, std::string *err);

// The model for an object's filename, loaded once and cached (nullptr if the
// object isn't a model or the file is missing).
const ModelInfo *objectModel(EngineObject *o);
void setModelFS(const ResourceFS *fs);

// Called for each string key an animation passes. The server forwards them only to
// objects with FLAG_MODELKEYS; the client shell gets them for all its objects.
using ModelKeyFn = std::function<void(EngineObject *, ArgList *)>;

// Advances the animation of one object by dt seconds.
void animateObject(EngineObject *o, float dt, const ModelKeyFn &onKey);

// A node's transform in world space for an object (false if not a model or
// no such node).
bool objectNodeTransform(EngineObject *o, int node, Vec3 *pos, DRotation *rot);

// Moves every attached object to its parent's node (attachments of `parent`
// only, or all of them).
void updateAttachments(EngineObject *parent = nullptr);

// Attachments as saved games keep them: parent and child by object id.
struct AttachmentRecord {
    uint32_t parent = 0, child = 0;
    int32_t node = -1;
    DVector offset{};
    DRotation rotOffset{0, 0, 0, 1};
};
std::vector<AttachmentRecord> attachmentRecords();
// Re-makes a saved attachment (nothing if the two are already attached).
void restoreAttachment(EngineObject *parent, EngineObject *child, const AttachmentRecord &r);

// GetModelAnimUserDims: the animation's dims (the object's dims if it isn't a model).
DRESULT modelAnimUserDims(HOBJECT h, DVector *dims, HMODELANIM anim);

// Fills the model animation functions of both engine interfaces.
void installModelAPI(ServerDE *server);
void installModelAPI(ClientDE *client);

}  // namespace shogo

#endif
