#include "render/cpu/cpu_tracer.h"

#include <OpenImageDenoise/oidn.h>
#include <embree4/rtcore.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace pcbview::cpu {
namespace {

// ---- tiny vec3, so the port reads like the GLSL ------------------------
struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline V3 normalize(V3 a) {
    const float l = std::sqrt(dot(a, a));
    return l < 1e-8f ? V3{0, 0, 1} : V3{a.x / l, a.y / l, a.z / l};
}
inline V3 mix(V3 a, V3 b, float t) { return a * (1.0f - t) + b * t; }
inline V3 reflect(V3 d, V3 n) { return d - n * (2.0f * dot(d, n)); }
inline float maxc(V3 a) { return std::max(a.x, std::max(a.y, a.z)); }

constexpr float PI = 3.14159265359f;
const V3 kSunDir = normalize({0.35f, 0.25f, 1.0f});
const V3 kSunColor = {1.0f * 2.6f, 0.95f * 2.6f, 0.88f * 2.6f};
constexpr float kSubstratePeelAlpha = 0.42f;

// ---- RNG, matching pathtrace.comp --------------------------------------
inline uint32_t hashu(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
struct Rng {
    uint32_t s;
    float next() {
        s = s * 747796405u + 2891336453u;
        uint32_t r = ((s >> ((s >> 28) + 4u)) ^ s) * 277803737u;
        r = (r >> 22) ^ r;
        return static_cast<float>(r) * (1.0f / 4294967296.0f);
    }
};

V3 sky(V3 d) {
    const float t = std::clamp(d.z * 0.5f + 0.5f, 0.0f, 1.0f);
    const V3 env = mix({0.11f, 0.12f, 0.15f}, {0.42f, 0.44f, 0.49f}, t);
    const float sun = std::pow(std::max(dot(d, kSunDir), 0.0f), 350.0f);
    return env + kSunColor * sun;
}

// Laminate transmission scatter cone (~5 deg); KEEP IN STEP with
// pathtrace.comp's kTransmitCos. (The sun cone is runtime-adjustable --
// Effects -> Shadow softness -- and arrives via Ctx::sunCosMax.)
constexpr float kTransmitCos = 0.996f;

// Uniform direction within the cone around `axis` with cos(half-angle) cosMax.
V3 sampleCone(V3 axis, float cosMax, Rng& rng) {
    const float ct = 1.0f + (cosMax - 1.0f) * rng.next();
    const float st = std::sqrt(std::max(0.0f, 1.0f - ct * ct));
    const float phi = 2.0f * PI * rng.next();
    const V3 up = std::fabs(axis.z) < 0.99f ? V3{0, 0, 1} : V3{1, 0, 0};
    const V3 t = normalize(cross(up, axis));
    const V3 b = cross(axis, t);
    return normalize(t * (st * std::cos(phi)) + b * (st * std::sin(phi)) +
                     axis * ct);
}

V3 cosineHemisphere(V3 n, Rng& rng) {
    const float r = std::sqrt(rng.next());
    const float phi = 2.0f * PI * rng.next();
    const V3 up = std::fabs(n.z) < 0.99f ? V3{0, 0, 1} : V3{1, 0, 0};
    const V3 t = normalize(cross(up, n));
    const V3 b = cross(n, t);
    return normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) +
                     n * std::sqrt(std::max(0.0f, 1.0f - r * r)));
}

// Defined below with the tracing code; registered on the geometry in
// rebuildScene.
void intersectFilter(const RTCFilterFunctionNArguments* a);
void occludedFilter(const RTCFilterFunctionNArguments* a);

Material makeMaterial(const geom::Part& part) {
    switch (part.material) {
        case geom::Material::Substrate:
            return {{0.72f, 0.61f, 0.38f, 1.0f}, 0.85f, 0.0f, 1.0f};
        case geom::Material::Soldermask:
            // Industry green #19882C (linear), semi-gloss (see renderer.cpp).
            return {{0.010f, 0.246f, 0.025f, 0.72f}, 0.20f, 0.0f, 0.0f};
        case geom::Material::Silkscreen:
            return {{0.90f, 0.90f, 0.87f, 1.0f}, 0.80f, 0.0f, 0.0f};
        case geom::Material::Component:
            return {{part.color[0], part.color[1], part.color[2], 1.0f}, 0.55f,
                    0.10f, 0.0f};
        default:  // copper / pads: gold ENIG, near-mirror
            return {{0.94f, 0.70f, 0.28f, 1.0f}, 0.05f, 1.0f, 0.0f};
    }
}

}  // namespace

CpuTracer::CpuTracer() { device_ = rtcNewDevice(nullptr); }

CpuTracer::~CpuTracer() {
    releaseScene();
    if (device_) rtcReleaseDevice(static_cast<RTCDevice>(device_));
    if (oidnFilter_) oidnReleaseFilter(static_cast<OIDNFilter>(oidnFilter_));
    if (oidnColor_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnColor_));
    if (oidnAlbedo_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnAlbedo_));
    if (oidnNormal_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnNormal_));
    if (oidnOut_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnOut_));
    if (oidn_) oidnReleaseDevice(static_cast<OIDNDevice>(oidn_));
}

void CpuTracer::releaseScene() {
    if (scene_) {
        rtcReleaseScene(static_cast<RTCScene>(scene_));
        scene_ = nullptr;
    }
}

void CpuTracer::rebuildScene(bool fastBuild) {
    releaseScene();
    if (indices_.empty()) return;
    RTCDevice dev = static_cast<RTCDevice>(device_);
    RTCScene scene = rtcNewScene(dev);
    rtcSetSceneBuildQuality(scene, fastBuild ? RTC_BUILD_QUALITY_MEDIUM
                                             : RTC_BUILD_QUALITY_HIGH);
    RTCGeometry geom = rtcNewGeometry(dev, RTC_GEOMETRY_TYPE_TRIANGLE);
    rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
                               vertsPadded_.data(), 0, 4 * sizeof(float),
                               vertsPadded_.size() / 4);
    rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                               indices_.data(), 0, 3 * sizeof(uint32_t),
                               indices_.size() / 3);
    // Filters implement the translucent-film logic during traversal (the CPU
    // analogue of Vulkan candidate traversal) -- see intersectFilter/
    // occludedFilter above. Registered on the geometry so they always run.
    rtcSetGeometryIntersectFilterFunction(geom, intersectFilter);
    rtcSetGeometryOccludedFilterFunction(geom, occludedFilter);
    rtcCommitGeometry(geom);
    rtcAttachGeometry(scene, geom);
    rtcReleaseGeometry(geom);
    rtcCommitScene(scene);
    scene_ = scene;
}

void CpuTracer::setScene(const geom::BoardMesh& mesh) {
    releaseScene();
    vertsPadded_.clear();
    normals_.clear();
    indices_.clear();
    materials_.clear();
    triMat_.clear();
    partSpans_.clear();
    maxRank_ = 0.0f;
    peel_ = 0.0f;
    bakedProgress_ = 0.0f;

    size_t nv = 0, nt = 0;
    for (const geom::Part& p : mesh.parts) {
        nv += p.mesh.vertices.size();
        nt += p.mesh.indices.size() / 3;
    }
    vertsPadded_.reserve(nv * 4);
    normals_.reserve(nv * 3);
    indices_.reserve(nt * 3);
    triMat_.reserve(nt);

    // Per-part data for the explode-rank computation, mirroring the GPU path's
    // uploadBoard: board layers get consecutive centred ranks by Z, components
    // pin one stage beyond, via barrels get their own plane.
    std::vector<float> centreZ;
    std::vector<int> mount;   // 0 board layer, +/-1 component side
    std::vector<bool> isVia;
    std::vector<bool> isPartialBarrel;  // blind/buried span barrels

    for (const geom::Part& part : mesh.parts) {
        if (part.mesh.indices.empty()) continue;
        const uint32_t base = static_cast<uint32_t>(vertsPadded_.size() / 4);
        const uint32_t matId = static_cast<uint32_t>(materials_.size());
        materials_.push_back(makeMaterial(part));

        double sz = 0.0;
        for (const geom::Vertex& v : part.mesh.vertices) {
            vertsPadded_.push_back(v.position[0]);
            vertsPadded_.push_back(v.position[1]);
            vertsPadded_.push_back(v.position[2]);
            vertsPadded_.push_back(0.0f);
            normals_.push_back(v.normal[0]);
            normals_.push_back(v.normal[1]);
            normals_.push_back(v.normal[2]);
            sz += v.position[2];
        }
        for (size_t i = 0; i + 2 < part.mesh.indices.size(); i += 3) {
            indices_.push_back(base + part.mesh.indices[i]);
            indices_.push_back(base + part.mesh.indices[i + 1]);
            indices_.push_back(base + part.mesh.indices[i + 2]);
            triMat_.push_back(matId);
        }
        PartSpan span;
        span.firstVertex = base;
        span.vertexCount = static_cast<uint32_t>(part.mesh.vertices.size());
        span.name = part.name;
        span.kind = part.material;
        partSpans_.push_back(span);
        centreZ.push_back(static_cast<float>(
            sz / std::max<size_t>(part.mesh.vertices.size(), 1)));
        mount.push_back(part.material == geom::Material::Component ? part.mountSide
                                                                   : 0);
        isVia.push_back(part.name == "vias");
        isPartialBarrel.push_back(part.partialBarrel);
    }
    if (indices_.empty()) return;

    // Ranks (mirrors renderer uploadBoard). Board layers sorted by Z, centred.
    std::vector<std::pair<float, size_t>> byHeight;
    for (size_t i = 0; i < partSpans_.size(); ++i)
        if (mount[i] == 0 && !isVia[i]) byHeight.emplace_back(centreZ[i], i);
    std::sort(byHeight.begin(), byHeight.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const float mid = (byHeight.size() - 1) * 0.5f;
    for (size_t k = 0; k < byHeight.size(); ++k)
        partSpans_[byHeight[k].second].rank = static_cast<float>(k) - mid;
    maxRank_ = mid;
    bool haveComp = false;
    for (size_t i = 0; i < partSpans_.size(); ++i) {
        if (mount[i] == 0) continue;
        partSpans_[i].rank = mount[i] > 0 ? (mid + 1.0f) : -(mid + 1.0f);
        haveComp = true;
    }
    if (haveComp) maxRank_ = mid + 1.0f;
    const auto rankTaken = [&](float r) {
        for (const auto& bh : byHeight)
            if (std::abs(partSpans_[bh.second].rank - r) < 1e-3f) return true;
        return false;
    };
    const float barrelRank = rankTaken(0.0f) ? 0.5f : 0.0f;
    // Blind/buried barrels travel with their layers: rank by centre Z, lerped
    // between the consecutive board-layer ranks (mirrors renderer rankAtZ).
    const auto rankAtZ = [&](float z) {
        if (byHeight.empty()) return 0.0f;
        if (z <= byHeight.front().first)
            return partSpans_[byHeight.front().second].rank;
        if (z >= byHeight.back().first)
            return partSpans_[byHeight.back().second].rank;
        for (size_t i = 0; i + 1 < byHeight.size(); ++i) {
            const float z0 = byHeight[i].first, z1 = byHeight[i + 1].first;
            if (z <= z1) {
                const float t = (z1 > z0) ? (z - z0) / (z1 - z0) : 0.0f;
                return partSpans_[byHeight[i].second].rank + t;
            }
        }
        return 0.0f;
    };
    for (size_t i = 0; i < partSpans_.size(); ++i)
        if (isVia[i])
            partSpans_[i].rank =
                isPartialBarrel[i] ? rankAtZ(centreZ[i]) : barrelRank;

    restVerts_ = vertsPadded_;
    rebuildScene(/*fastBuild=*/false);
}

bool CpuTracer::setExplode(float progress, float stepMm) {
    if (restVerts_.empty()) return false;
    if (!bakeDirty_ && std::abs(progress - bakedProgress_) < 1e-4f &&
        std::abs(stepMm - bakedStep_) < 1e-4f) {
        return false;
    }
    bake(progress, stepMm);
    return true;
}

void CpuTracer::bake(float progress, float stepMm) {
    bakedProgress_ = progress;
    bakedStep_ = stepMm;
    bakeDirty_ = false;
    peel_ = maxRank_ > 0.0f ? std::clamp(progress / maxRank_, 0.0f, 1.0f) : 0.0f;

    // Displace each part's vertices along Z by its peel travel (mirrors the GPU
    // bakeExplode / board.vert travel()); hidden parts get NaN vertices, which
    // Embree treats as invalid triangles and drops from the BVH -- the same
    // trick as the GPU's visibility bake.
    const float kHide = std::numeric_limits<float>::quiet_NaN();
    vertsPadded_ = restVerts_;
    for (const PartSpan& ps : partSpans_) {
        const uint32_t end = ps.firstVertex + ps.vertexCount;
        if (!ps.visible) {
            for (uint32_t v = ps.firstVertex; v < end; ++v)
                vertsPadded_[4u * v] = kHide;
            continue;
        }
        const float ring = std::abs(ps.rank);
        const float travel =
            std::max(progress - (maxRank_ - ring), 0.0f) * stepMm;
        const float sign = ps.rank > 0.0f ? 1.0f : (ps.rank < 0.0f ? -1.0f : 0.0f);
        const float dz = sign * travel;
        if (dz == 0.0f) continue;
        for (uint32_t v = ps.firstVertex; v < end; ++v)
            vertsPadded_[4u * v + 2u] += dz;
    }
    rebuildScene(/*fastBuild=*/true);
}

bool CpuTracer::setPartVisible(const std::string& name, bool visible) {
    bool changed = false;
    for (PartSpan& ps : partSpans_) {
        if (ps.name == name && ps.visible != visible) {
            ps.visible = visible;
            changed = true;
        }
    }
    if (changed) bakeDirty_ = true;
    return changed;
}

// ---- appearance parity (same formulas as the Renderer's setters) ----------
void CpuTracer::setMaskAppearance(float r, float g, float b, float opacity) {
    for (size_t i = 0; i < partSpans_.size() && i < materials_.size(); ++i) {
        if (partSpans_[i].kind != geom::Material::Soldermask) continue;
        materials_[i].albedo = {r, g, b, opacity};
    }
}
void CpuTracer::setSubstrateAppearance(float r, float g, float b,
                                       float opacity) {
    for (size_t i = 0; i < partSpans_.size() && i < materials_.size(); ++i) {
        if (partSpans_[i].kind != geom::Material::Substrate) continue;
        materials_[i].albedo = {r, g, b, opacity};
    }
}
void CpuTracer::setComponentShine(float s01) {
    const float s = std::clamp(s01, 0.0f, 1.0f);
    for (size_t i = 0; i < partSpans_.size() && i < materials_.size(); ++i) {
        if (partSpans_[i].kind != geom::Material::Component) continue;
        materials_[i].roughness = 0.55f - 0.51f * s;
        materials_[i].metallic = 0.10f + 0.90f * s;
    }
}
void CpuTracer::setPadShine(float s01) {
    const float s = std::clamp(s01, 0.0f, 1.0f);
    for (size_t i = 0; i < partSpans_.size() && i < materials_.size(); ++i) {
        if (partSpans_[i].kind != geom::Material::Copper) continue;
        materials_[i].roughness = 0.5f - 0.48f * s;
    }
}

namespace {

// Shared per-thread tracing context, so the hot loop is a free function.
struct Ctx {
    RTCScene scene;
    const uint32_t* idx;
    const float* verts;   // 4 floats/vertex
    const float* norms;   // 3 floats/vertex
    const Material* mats;
    const uint32_t* triMat;
    float peel;       // substrate fade amount (0 at rest)
    bool preview;     // RT preview: sun shadow + AO, no GI bounces
    float sunCosMax;  // cos(sun angular radius); 1 = point sun
};

inline V3 vpos(const Ctx& c, uint32_t i) {
    return {c.verts[4u * i], c.verts[4u * i + 1u], c.verts[4u * i + 2u]};
}
inline V3 vnrm(const Ctx& c, uint32_t i) {
    return {c.norms[3u * i], c.norms[3u * i + 1u], c.norms[3u * i + 2u]};
}
inline float effAlpha(const Ctx& c, uint32_t prim) {
    const Material& m = c.mats[c.triMat[prim]];
    float a = m.albedo[3];
    if (m.fade > 0.5f) a = a * (1.0f - c.peel) + kSubstratePeelAlpha * c.peel;
    return a;
}
inline bool opaquePrim(const Ctx& c, uint32_t prim) {
    return effAlpha(c, prim) >= 0.99f;
}

void setupRay(RTCRayHit& rh, V3 o, V3 d, float tnear, float tfar) {
    rh.ray.org_x = o.x; rh.ray.org_y = o.y; rh.ray.org_z = o.z;
    rh.ray.dir_x = d.x; rh.ray.dir_y = d.y; rh.ray.dir_z = d.z;
    rh.ray.tnear = tnear;
    rh.ray.tfar = tfar;
    rh.ray.mask = 0xFFFFFFFFu;
    rh.ray.flags = 0;
    rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rh.hit.primID = RTC_INVALID_GEOMETRY_ID;
}

// Per-ray state handed to the Embree filter callbacks -- the CPU analogue of
// the GPU's candidate traversal. Filters see EVERY intersected primitive
// (including surfaces exactly coincident, like the mask bottom on the copper
// top) with no epsilon re-shooting. The earlier restart-the-ray-past-t loop
// skipped whichever coincident surface float luck disfavoured, which showed as
// blocky per-triangle mottling along masked traces.
struct RayCtx {
    RTCRayQueryContext base;
    const Ctx* c = nullptr;
    uint32_t diceSeed = 0;   // per-ray; film dice = hash(diceSeed ^ prim),
                             // idempotent so duplicate reports agree (the same
                             // lesson as the GPU's NVIDIA BVH-split squares)
    bool recordCoat = false; // primary ray: track the nearest entering film
    float coatT = 1e30f;
    V3 coatAlbedo{0, 0, 0};
    float coatAlpha = 0.0f;
    float coatRough = 0.2f;  // film roughness, for the preview's raster blend
};

// Intersect filter: opaque accepts (commits), film exit faces pass through,
// film entering faces record the coat and stochastically commit (PT) or always
// pass (preview -- composited analytically after shading).
void intersectFilter(const RTCFilterFunctionNArguments* a) {
    if (a->N != 1) return;
    RayCtx* rc = reinterpret_cast<RayCtx*>(a->context);
    const Ctx& c = *rc->c;
    const uint32_t prim = RTCHitN_primID(a->hit, 1, 0);
    if (opaquePrim(c, prim)) return;  // accept

    const V3 dir = {RTCRayN_dir_x(a->ray, 1, 0), RTCRayN_dir_y(a->ray, 1, 0),
                    RTCRayN_dir_z(a->ray, 1, 0)};
    const V3 ng = {RTCHitN_Ng_x(a->hit, 1, 0), RTCHitN_Ng_y(a->hit, 1, 0),
                   RTCHitN_Ng_z(a->hit, 1, 0)};
    if (dot(dir, ng) >= 0.0f) {  // exit face: never commits
        a->valid[0] = 0;
        return;
    }
    // The peeling laminate ALWAYS commits its entering face; the shading code
    // decides between surface shading and scattered transmission (mirrors the
    // GPU -- a direction change cannot happen mid-traversal).
    if (c.mats[c.triMat[prim]].fade > 0.5f) return;  // accept
    if (rc->recordCoat) {
        const float t = RTCRayN_tfar(a->ray, 1, 0);
        if (t < rc->coatT) {
            rc->coatT = t;
            const Material& fm = c.mats[c.triMat[prim]];
            rc->coatAlbedo = {fm.albedo[0], fm.albedo[1], fm.albedo[2]};
            rc->coatAlpha = effAlpha(c, prim);
            rc->coatRough = fm.roughness;
        }
    }
    if (c.preview) {  // analytic composite instead of stochastic commit
        a->valid[0] = 0;
        return;
    }
    const float xi =
        hashu(rc->diceSeed ^ prim) * (1.0f / 4294967296.0f);
    if (xi >= effAlpha(c, prim)) a->valid[0] = 0;  // pass through
}

// Occlusion filter: films never cast shadows; only opaque hits count.
void occludedFilter(const RTCFilterFunctionNArguments* a) {
    if (a->N != 1) return;
    RayCtx* rc = reinterpret_cast<RayCtx*>(a->context);
    if (!opaquePrim(*rc->c, RTCHitN_primID(a->hit, 1, 0))) a->valid[0] = 0;
}

bool occluded(const Ctx& c, V3 o, V3 d, float tmax);

// board_rt.frag's ambientOcclusion(), verbatim: a FIXED 6-ray kernel biased
// toward the normal, 2.5mm contact radius -- deterministic, so the preview is
// temporally stable and effectively converged after one sample.
float ambientOcclusionRt(const Ctx& c, V3 p, V3 n) {
    static const V3 k[6] = {{0.0f, 0.0f, 1.0f},   {0.60f, 0.0f, 0.80f},
                            {-0.60f, 0.0f, 0.80f}, {0.0f, 0.60f, 0.80f},
                            {0.0f, -0.60f, 0.80f}, {0.42f, 0.42f, 0.80f}};
    const V3 up = std::fabs(n.z) < 0.99f ? V3{0, 0, 1} : V3{1, 0, 0};
    const V3 t = normalize(cross(up, n));
    const V3 b = cross(n, t);
    float open = 0.0f;
    for (int i = 0; i < 6; ++i) {
        const V3 s = normalize(k[i]);
        const V3 d = t * s.x + b * s.y + n * s.z;
        if (!occluded(c, p + n * 0.05f, d, 2.5f)) open += 1.0f;
    }
    return open / 6.0f;
}

// board_rt.frag's shading model, verbatim (same weights, same spec, same
// fresnel/env terms), with the traced shadow + AO modulation folded in.
V3 rasterShade(V3 albedo, float roughness, float metallic, V3 n, V3 viewDir,
               V3 keyDir, V3 fillDir, float shadow, float ao) {
    const float key = std::max(dot(n, keyDir), 0.0f);
    const float fill = std::max(dot(n, fillDir), 0.0f);
    const float shadowedKey = key * (0.30f + 0.70f * shadow);
    float diffuse =
        0.15f * (0.4f + 0.6f * ao) + 0.65f * shadowedKey + 0.20f * fill;
    diffuse *= 0.7f + 0.3f * ao;
    const V3 h = normalize(keyDir + viewDir);
    const float rough = std::clamp(roughness, 0.05f, 1.0f);
    const float specPower = 2.0f / (rough * rough) - 2.0f;
    const float spec = std::pow(std::max(dot(n, h), 0.0f), specPower) *
                       (1.0f - rough) * shadow;
    const float fresnel =
        std::pow(1.0f - std::max(dot(n, viewDir), 0.0f), 4.0f);
    const V3 specTint = mix({1, 1, 1}, albedo, metallic);
    const V3 refl = reflect(viewDir * -1.0f, n);
    const float envUp = std::clamp(refl.z * 0.5f + 0.5f, 0.0f, 1.0f);
    const V3 env = mix({0.22f, 0.22f, 0.22f}, {1.05f, 1.05f, 1.05f}, envUp);
    return albedo * diffuse + specTint * (spec * (0.12f + 1.18f * metallic)) +
           specTint * env * (metallic * 0.35f) +
           V3{fresnel, fresnel, fresnel} * 0.08f;
}

bool occluded(const Ctx& c, V3 o, V3 d, float tmax) {
    RayCtx rc;
    rtcInitRayQueryContext(&rc.base);
    rc.c = &c;
    RTCOccludedArguments args;
    rtcInitOccludedArguments(&args);
    args.context = &rc.base;
    RTCRay ray{};
    ray.org_x = o.x; ray.org_y = o.y; ray.org_z = o.z;
    ray.dir_x = d.x; ray.dir_y = d.y; ray.dir_z = d.z;
    ray.tnear = 0.0f;
    ray.tfar = tmax;
    ray.mask = 0xFFFFFFFFu;
    rtcOccluded1(c.scene, &ray, &args);
    return ray.tfar < 0.0f;  // Embree signals occlusion with tfar = -inf
}

// One full path. Returns radiance; fills guides on the primary hit.
V3 trace(const Ctx& c, V3 origin, V3 dir, Rng& rng, V3& firstAlbedo,
         V3& firstNormal, int maxDepth) {
    V3 radiance{0, 0, 0};
    V3 throughput{1, 1, 1};
    firstAlbedo = {1, 1, 1};
    firstNormal = {0, 0, 0};
    // Laminate transmissions rewind `depth`; this counter guards runaway.
    uint32_t transmits = 0;

    for (int depth = 0; depth <= maxDepth; ++depth) {
        // ONE filtered traversal finds the committed hit: nearest opaque, or a
        // stochastically-committed entering film. The filter (the CPU analogue
        // of the GPU's candidate loop) handles pass-through and the coat.
        RayCtx rc;
        rtcInitRayQueryContext(&rc.base);
        rc.c = &c;
        rc.recordCoat = depth == 0;
        rc.diceSeed = hashu(rng.s ^ 0xB5297A4Du);

        RTCIntersectArguments args;
        rtcInitIntersectArguments(&args);
        args.context = &rc.base;

        RTCRayHit rh;
        setupRay(rh, origin, dir, 0.0f, 1e30f);
        rtcIntersect1(c.scene, &rh, &args);

        const float coatT = rc.coatT;
        const V3 coatAlbedo = rc.coatAlbedo;
        const float coatAlpha = rc.coatAlpha;
        const float coatRough = rc.coatRough;

        if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            // Preview matches the raster clear colour, not the PT sky dome.
            const V3 bg = c.preview ? V3{0.118f, 0.118f, 0.118f} : sky(dir);
            radiance = radiance + throughput * bg;
            if (depth == 0) { firstAlbedo = bg; firstNormal = dir * -1.0f; }
            break;
        }
        const uint32_t prim = rh.hit.primID;
        const float bx = rh.hit.u, by = rh.hit.v;
        const float tHit = rh.ray.tfar;

        const uint32_t i0 = c.idx[3u * prim], i1 = c.idx[3u * prim + 1u],
                       i2 = c.idx[3u * prim + 2u];
        const V3 p = origin + dir * tHit;
        V3 ng = normalize(cross(vpos(c, i1) - vpos(c, i0),
                                vpos(c, i2) - vpos(c, i0)));
        V3 ns = normalize(vnrm(c, i0) * (1.0f - bx - by) + vnrm(c, i1) * bx +
                          vnrm(c, i2) * by);
        if (dot(ns, ng) < 0.0f) ng = ng * -1.0f;
        const V3 n = dot(ns, dir * -1.0f) < 0.0f ? ns * -1.0f : ns;
        const V3 gn = dot(ng, dir * -1.0f) < 0.0f ? ng * -1.0f : ng;

        const Material& m = c.mats[c.triMat[prim]];
        const V3 albedo = {m.albedo[0], m.albedo[1], m.albedo[2]};
        const float metallic = m.metallic;

        // TRANSMISSION through the peeling laminate (mirrors pathtrace.comp):
        // scatter into a small cone, tint by the resin, continue -- the cheap
        // stand-in for FR4 subsurface scattering. Does not consume path depth.
        if (!c.preview && m.fade > 0.5f) {
            const float ea = effAlpha(c, prim);
            if (ea < 0.99f && rng.next() >= ea && transmits < 8u) {
                ++transmits;
                origin = p + dir * 0.02f;
                dir = sampleCone(dir, kTransmitCos, rng);
                throughput = throughput * mix({1, 1, 1}, albedo, 0.35f);
                --depth;
                continue;
            }
        }

        if (depth == 0) {
            V3 ga = albedo;
            if (coatT < tHit - 1e-4f) ga = mix(ga, coatAlbedo, coatAlpha);
            firstAlbedo = ga;
            firstNormal = n;
        }

        // RT preview: board_rt.frag's exact shading -- camera-relative key/fill
        // rig, traced key shadow, fixed-kernel AO -- with the nearest film
        // alpha-composited over the result exactly like the raster blend. Fully
        // deterministic; samples only refine anti-aliasing. This is why CPU RT
        // now looks like GPU RT and not like the path tracer: it shares the
        // raster LIGHTING MODEL, not the PT sun/sky rig.
        if (c.preview) {
            const V3 viewDir = dir * -1.0f;
            V3 camRight = cross(viewDir, {0.0f, 0.0f, 1.0f});
            camRight = dot(camRight, camRight) < 1e-4f ? V3{1, 0, 0}
                                                       : normalize(camRight);
            const V3 camUp = cross(camRight, viewDir);
            const V3 keyPos =
                origin + (camRight * 0.55f + camUp * 0.55f) * tHit;
            const V3 keyDir = normalize(keyPos - p);
            const V3 fillDir =
                normalize(viewDir - camRight * 0.5f - camUp * 0.25f);
            const V3 kp = keyPos - p;
            const float shadow =
                occluded(c, p + n * 0.05f, keyDir, std::sqrt(dot(kp, kp)))
                    ? 0.0f
                    : 1.0f;
            const float ao = ambientOcclusionRt(c, p, n);
            radiance = rasterShade(albedo, m.roughness, metallic, n, viewDir,
                                   keyDir, fillDir, shadow, ao);
            if (coatAlpha > 0.0f && coatT < tHit) {
                V3 fn{0, 0, 1};
                if (dot(fn, viewDir) < 0.0f) fn = fn * -1.0f;  // two-sided
                const V3 filmLit = rasterShade(coatAlbedo, coatRough, 0.0f, fn,
                                               viewDir, keyDir, fillDir, shadow,
                                               1.0f);
                radiance = mix(radiance, filmLit, coatAlpha);
            }
            break;
        }

        // Direct sun: diffuse + a colour-tinted specular glint (PT only). The
        // direction is sampled within the sun's angular cone per sample, so
        // shadows gain real penumbras (mirrors pathtrace.comp).
        const V3 sunDir = sampleCone(kSunDir, c.sunCosMax, rng);
        const float ndl = std::max(dot(n, sunDir), 0.0f);
        if (ndl > 0.0f && !occluded(c, p + gn * 0.03f, sunDir, 1e5f)) {
            const float rough = std::clamp(m.roughness, 0.04f, 1.0f);
            const V3 h = normalize(sunDir - dir);
            const float spec = std::pow(std::max(dot(n, h), 0.0f),
                                        2.0f / (rough * rough) - 2.0f);
            const V3 specCol = mix({0.04f, 0.04f, 0.04f}, albedo, metallic);
            radiance = radiance +
                       throughput * (albedo * (1.0f / PI) + specCol * spec) *
                           kSunColor * ndl;
        }

        // Next direction.
        origin = p + gn * 0.03f;
        if (rng.next() < metallic) {
            const float rough = std::clamp(m.roughness, 0.04f, 1.0f);
            const V3 refl = reflect(dir, n);
            const V3 glossy = normalize(refl + cosineHemisphere(n, rng) * rough);
            dir = dot(glossy, gn) > 0.0f ? glossy : refl;
            throughput = throughput * albedo;
        } else {
            dir = cosineHemisphere(n, rng);
            throughput = throughput * albedo;
        }

        if (depth >= 3) {
            const float q = maxc(throughput);
            if (rng.next() > q) break;
            throughput = throughput * (1.0f / std::max(q, 1e-4f));
        }
    }

    radiance.x = std::min(radiance.x, 8.0f);
    radiance.y = std::min(radiance.y, 8.0f);
    radiance.z = std::min(radiance.z, 8.0f);
    return radiance;
}

}  // namespace

void CpuTracer::accumulate(const TraceCamera& cam, uint32_t width,
                           uint32_t height, int spp, uint32_t frame) {
    if (!scene_ || width == 0 || height == 0) return;

    if (frame == 0 || width != width_ || height != height_ ||
        accumColor_.size() != static_cast<size_t>(width) * height * 3) {
        width_ = width;
        height_ = height;
        cam_ = cam;
        accumColor_.assign(static_cast<size_t>(width) * height * 3, 0.0f);
        accumAlbedo_.assign(static_cast<size_t>(width) * height * 3, 0.0f);
        accumNormal_.assign(static_cast<size_t>(width) * height * 3, 0.0f);
        accumSamples_ = 0;
    }

    Ctx c;
    c.scene = static_cast<RTCScene>(scene_);
    c.idx = indices_.data();
    c.verts = vertsPadded_.data();
    c.norms = normals_.data();
    c.mats = materials_.data();
    c.triMat = triMat_.data();
    c.peel = peel_;  // substrate fade while exploded
    c.preview = preview_;
    c.sunCosMax = sunCosMax_;

    const uint32_t baseSample = static_cast<uint32_t>(accumSamples_);
    const int maxDepth = 6;
    const TraceCamera cam2 = cam_;
    float* col = accumColor_.data();
    float* alb = accumAlbedo_.data();
    float* nor = accumNormal_.data();
    const uint32_t w = width_, h = height_;

    // Parallelise over rows with a plain thread pool -- each pixel writes only
    // its own accumulator slots, so there are no data races. std::thread keeps
    // the dependency surface to Embree alone (no TBB headers needed).
    const auto renderRows = [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t o = (static_cast<size_t>(y) * w + x) * 3;
                for (int s = 0; s < spp; ++s) {
                    const uint32_t gs = baseSample + static_cast<uint32_t>(s);
                    Rng rng;
                    uint32_t hh = hashu(x);
                    hh = hashu(hh ^ y);
                    rng.s = hashu(hh ^ (gs * 9781u + 1u + frame * 2654435761u));

                    const float jx = rng.next(), jy = rng.next();
                    const float sx = 2.0f * (x + jx) / w - 1.0f;
                    const float sy = 2.0f * (y + jy) / h - 1.0f;
                    V3 dir, origin;
                    if (cam2.ortho) {  // parallel rays, offset origins
                        dir = normalize({cam2.fwd[0], cam2.fwd[1], cam2.fwd[2]});
                        origin = {cam2.eye[0] + sx * cam2.right[0] - sy * cam2.up[0],
                                  cam2.eye[1] + sx * cam2.right[1] - sy * cam2.up[1],
                                  cam2.eye[2] + sx * cam2.right[2] - sy * cam2.up[2]};
                    } else {
                        dir = normalize(
                            {cam2.fwd[0] + sx * cam2.right[0] - sy * cam2.up[0],
                             cam2.fwd[1] + sx * cam2.right[1] - sy * cam2.up[1],
                             cam2.fwd[2] + sx * cam2.right[2] - sy * cam2.up[2]});
                        origin = {cam2.eye[0], cam2.eye[1], cam2.eye[2]};
                    }

                    V3 ga, gn;
                    const V3 rad = trace(c, origin, dir, rng, ga, gn, maxDepth);
                    col[o + 0] += rad.x; col[o + 1] += rad.y; col[o + 2] += rad.z;
                    alb[o + 0] += ga.x;  alb[o + 1] += ga.y;  alb[o + 2] += ga.z;
                    nor[o + 0] += gn.x;  nor[o + 1] += gn.y;  nor[o + 2] += gn.z;
                }
            }
        }
    };

    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    const uint32_t nThreads = std::min<uint32_t>(hw, h);
    std::vector<std::thread> pool;
    pool.reserve(nThreads);
    for (uint32_t t = 0; t < nThreads; ++t) {
        const uint32_t y0 = t * h / nThreads;
        const uint32_t y1 = (t + 1) * h / nThreads;
        pool.emplace_back(renderRows, y0, y1);
    }
    for (std::thread& th : pool) th.join();
    accumSamples_ += spp;
}

namespace {
V3 shoulder(V3 x) {
    const float knee = 0.80f;
    const float m = maxc(x);
    if (m <= knee) return x;
    const float t = m - knee;
    const float mo = knee + (1.0f - knee) * t / (t + (1.0f - knee));
    return x * (mo / m);
}
}  // namespace

void CpuTracer::denoiseInto(std::vector<float>& colorOut) const {
    const size_t n = static_cast<size_t>(width_) * height_;
    const float inv = 1.0f / static_cast<float>(std::max(accumSamples_, 1));
    std::vector<float> col(n * 3), alb(n * 3), nor(n * 3);
    for (size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) {
            col[i * 3 + k] = accumColor_[i * 3 + k] * inv;
            alb[i * 3 + k] = std::clamp(accumAlbedo_[i * 3 + k] * inv, 0.0f, 1.0f);
        }
        float nx = accumNormal_[i * 3] * inv, ny = accumNormal_[i * 3 + 1] * inv,
              nz = accumNormal_[i * 3 + 2] * inv;
        const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (l > 1e-6f) { nx /= l; ny /= l; nz /= l; }
        nor[i * 3] = nx; nor[i * 3 + 1] = ny; nor[i * 3 + 2] = nz;
    }

    if (!oidn_) {
        OIDNDevice d = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
        oidnCommitDevice(d);
        oidn_ = d;
    }
    OIDNDevice dev = static_cast<OIDNDevice>(oidn_);
    const size_t bytes = n * 3 * sizeof(float);
    if (oidnW_ != width_ || oidnH_ != height_ || !oidnFilter_) {
        if (oidnFilter_) oidnReleaseFilter(static_cast<OIDNFilter>(oidnFilter_));
        if (oidnColor_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnColor_));
        if (oidnAlbedo_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnAlbedo_));
        if (oidnNormal_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnNormal_));
        if (oidnOut_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnOut_));
        OIDNBuffer bc = oidnNewBuffer(dev, bytes);
        OIDNBuffer ba = oidnNewBuffer(dev, bytes);
        OIDNBuffer bn = oidnNewBuffer(dev, bytes);
        OIDNBuffer bo = oidnNewBuffer(dev, bytes);
        OIDNFilter f = oidnNewFilter(dev, "RT");
        const size_t px = 3 * sizeof(float), row = px * width_;
        oidnSetFilterImage(f, "color", bc, OIDN_FORMAT_FLOAT3, width_, height_, 0, px, row);
        oidnSetFilterImage(f, "albedo", ba, OIDN_FORMAT_FLOAT3, width_, height_, 0, px, row);
        oidnSetFilterImage(f, "normal", bn, OIDN_FORMAT_FLOAT3, width_, height_, 0, px, row);
        oidnSetFilterImage(f, "output", bo, OIDN_FORMAT_FLOAT3, width_, height_, 0, px, row);
        oidnSetFilterBool(f, "hdr", true);
        oidnCommitFilter(f);
        oidnColor_ = bc; oidnAlbedo_ = ba; oidnNormal_ = bn; oidnOut_ = bo;
        oidnFilter_ = f; oidnW_ = width_; oidnH_ = height_;
    }
    oidnWriteBuffer(static_cast<OIDNBuffer>(oidnColor_), 0, bytes, col.data());
    oidnWriteBuffer(static_cast<OIDNBuffer>(oidnAlbedo_), 0, bytes, alb.data());
    oidnWriteBuffer(static_cast<OIDNBuffer>(oidnNormal_), 0, bytes, nor.data());
    oidnExecuteFilter(static_cast<OIDNFilter>(oidnFilter_));
    colorOut.resize(n * 3);
    oidnReadBuffer(static_cast<OIDNBuffer>(oidnOut_), 0, bytes, colorOut.data());
}

namespace {
inline uint8_t encodeSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? 12.92f * c
                                    : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(s * 255.0f + 0.5f);
}
}  // namespace

// Tonemapped LINEAR image (shoulder + exposure, no gamma), shared by both
// resolvers. `denoise` runs OIDN on the accumulation first.
void CpuTracer::resolveLinear(std::vector<float>& lin, bool denoise) const {
    const size_t n = static_cast<size_t>(width_) * height_;
    const float kExposure = 0.85f;
    std::vector<float> color;
    if (denoise && width_ && height_) {
        denoiseInto(color);
    } else {
        color.resize(n * 3);
        const float inv = 1.0f / static_cast<float>(std::max(accumSamples_, 1));
        for (size_t i = 0; i < n * 3; ++i) color[i] = accumColor_[i] * inv;
    }
    lin.resize(n * 3);
    if (preview_) {
        // The raster pipeline writes lit colour straight to the sRGB target --
        // no tonemap, no exposure. Matching that keeps CPU RT colour-identical
        // to GPU RT instead of 0.85x darker with a shoulder.
        for (size_t i = 0; i < n * 3; ++i) lin[i] = std::clamp(color[i], 0.0f, 1.0f);
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        const V3 t = shoulder(V3{color[i * 3], color[i * 3 + 1], color[i * 3 + 2]} *
                              kExposure);
        lin[i * 3] = t.x; lin[i * 3 + 1] = t.y; lin[i * 3 + 2] = t.z;
    }
}

std::vector<uint8_t> CpuTracer::resolve(bool denoise) const {
    const size_t n = static_cast<size_t>(width_) * height_;
    std::vector<uint8_t> out(n * 4, 0);
    if (accumSamples_ <= 0) return out;
    std::vector<float> lin;
    resolveLinear(lin, denoise);
    for (size_t i = 0; i < n; ++i) {
        for (int cc = 0; cc < 3; ++cc) {
            const float g = std::pow(std::clamp(lin[i * 3 + cc], 0.0f, 1.0f),
                                     1.0f / 2.2f);
            out[i * 4 + cc] = static_cast<uint8_t>(g * 255.0f + 0.5f);
        }
        out[i * 4 + 3] = 255;
    }
    return out;
}

std::vector<uint8_t> CpuTracer::resolveDisplay(bool denoise) const {
    const size_t n = static_cast<size_t>(width_) * height_;
    std::vector<uint8_t> out(n * 4, 0);
    if (accumSamples_ <= 0) return out;
    std::vector<float> lin;
    resolveLinear(lin, denoise);
    for (size_t i = 0; i < n; ++i) {
        // BGRA order for B8G8R8A8; store sRGB-encoded values so the SRGB image
        // decodes them back to the linear tonemapped colour on read.
        out[i * 4 + 0] = encodeSrgb(lin[i * 3 + 2]);  // B
        out[i * 4 + 1] = encodeSrgb(lin[i * 3 + 1]);  // G
        out[i * 4 + 2] = encodeSrgb(lin[i * 3 + 0]);  // R
        out[i * 4 + 3] = 255;
    }
    return out;
}

std::vector<uint8_t> CpuTracer::renderImage(const TraceCamera& cam,
                                            uint32_t width, uint32_t height,
                                            int spp, bool denoise) {
    accumulate(cam, width, height, spp, 0);
    return resolve(denoise);
}

}  // namespace pcbview::cpu
