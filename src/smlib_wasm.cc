// A WebAssembly build of the SMLib B-rep kernel from NVIDIA's open-source
// usd-brep (github.com/NVIDIA-Omniverse/usd-brep, Apache-2.0).
//
// A port of BREPcode's Node addon bridge (native/nvidia-kernel/src/addon.cc)
// to embind, function for function, so BREPcode's adapter.cjs runs on it
// unchanged. Two differences, both hidden by src/api.mjs:
//
//   - every entry point takes its arguments as ONE JS array (the wrapper
//     passes `[...arguments]`), which plays the part of Napi::CallbackInfo:
//     embind checks argument counts strictly, and tessellate's 4th and 5th
//     arguments are optional;
//   - nothing throws across the wasm boundary. A failure is caught here, its
//     message kept, and undefined returned; the wrapper reads lastError() and
//     throws a JS Error with the same text the addon would have thrown.
//
// Changed from addon.cc: the Napi types became emscripten::val, Napi::Error
// became KernelError, and init()/lastError()/heapBytes() were added.

#include <emscripten/bind.h>
#include <emscripten/heap.h>
#include <emscripten/val.h>

#include <SmSmlibAll.h>
#include <SmPoly.h>
#include <SmApiBrep.h>
#include <SmApiCurves.h>
#include <SmApiFillets.h>
#include <SmApiGeneral.h>
#include <SmApiPrimitives.h>
#include <SmApiQueries.h>
#include <SmApiSurfaces.h>
#include <SmThreadLocalStorage.h>

#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using emscripten::val;

namespace {

struct KernelError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::unordered_map<uint32_t, SmBrep*> g_solids;
uint32_t g_next = 1;
std::string g_error;
bool g_failed = false;

uint32_t keep(SmBrep* b) {
  uint32_t id = g_next++;
  g_solids[id] = b;
  return id;
}

[[noreturn]] void fail(const char* what, long status) {
  throw KernelError(std::string(what) + " failed (SmApi status " + std::to_string(status) + ")");
}

#define CHECK(call, what) do { long _s = (call); if (_s != SM_SUCCESS) fail(what, _s); } while (0)

// the arguments array, as the addon read Napi::CallbackInfo
using Args = const val&;
size_t argc(Args a) { return a["length"].as<size_t>(); }
double num(Args a, size_t i) { return a[i].as<double>(); }
uint32_t u32(const val& v) { return (uint32_t)v.as<double>(); }
int32_t i32(const val& v) { return (int32_t)v.as<double>(); }
size_t len(const val& arr) { return arr["length"].as<size_t>(); }

SmBrep* get(Args a, size_t i) {
  uint32_t id = u32(a[i]);
  auto it = g_solids.find(id);
  if (it == g_solids.end()) throw KernelError("unknown solid handle " + std::to_string(id));
  return it->second;
}

val jsnum(double v) { return val(v); }

val f64(const double* p, size_t n) {
  return val::global("Float64Array").new_(val(emscripten::typed_memory_view(n, p)));
}

void checkSolid(SmBrep* b, const char* what) {
  SmBoolean ok = FALSE;
  if (SmApiBrepIsManifoldSolid(b, ok) != SM_SUCCESS || !ok) {
    // SMLib can report success and hand back a body that is not a solid (a
    // tangent torus on a cylinder came back with zero volume); never let one
    // through as a result.
    throw KernelError(std::string(what) + " did not produce a closed solid");
  }
}

// ---------------------------------------------------------------- lifetime

val Info(Args) {
  val o = val::object();
  o.set("kernel", "SMLib (usd-brep, WebAssembly)");
  o.set("version", "1.0.1");
  return o;
}

val Free(Args a) {
  uint32_t id = u32(a[0]);
  auto it = g_solids.find(id);
  if (it != g_solids.end()) { delete it->second; g_solids.erase(it); }
  return val::undefined();
}

val Count(Args) {
  return jsnum((double)g_solids.size());
}

val Copy(Args a) {
  SmBrep* out = nullptr;
  CHECK(SmApiBrepCopy(get(a, 0), out), "copy");
  return jsnum(keep(out));
}

// -------------------------------------------------------------- primitives
//
// All in SMLib's own frame (Z up, base on z = 0); the adapter places them.

val Box(Args a) {
  SmBrep* b = nullptr;
  CHECK(SmApiCreateBox(SmVector3d(0, 0, 0), num(a, 0), num(a, 1), num(a, 2), b), "box");
  return jsnum(keep(b));
}

val Cylinder(Args a) {
  SmBrep* b = nullptr;
  CHECK(SmApiCreateCylinder(SmVector3d(0, 0, 0), num(a, 0), num(a, 1), b), "cylinder");
  return jsnum(keep(b));
}

val Cone(Args a) {
  SmBrep* b = nullptr;
  CHECK(SmApiCreateCone(SmVector3d(0, 0, 0), num(a, 0), num(a, 1), num(a, 2), b), "cone");
  return jsnum(keep(b));
}

val Sphere(Args a) {
  SmBrep* b = nullptr;
  CHECK(SmApiCreateSphere(SmVector3d(0, 0, 0), num(a, 0), b), "sphere");
  return jsnum(keep(b));
}

val Torus(Args a) {
  SmBrep* b = nullptr;
  CHECK(SmApiCreateTorus(SmVector3d(0, 0, 0), num(a, 0), num(a, 1), b), "torus");
  return jsnum(keep(b));
}

void freeCurves(SmTArray<SmCurve*>& curves);

// (majorRadius, tubeRadius, degrees): a part torus, the tube's circle (in the
// XZ plane, centred on +X) swept about +Z from +X toward +Y, capped at both ends
val TorusArc(Args a) {
  SmAxis2Placement frame;
  frame.SetCanonical(SmPoint3d(num(a, 0), 0, 0), SmVector3d(1, 0, 0), SmVector3d(0, 0, 1));
  SmCircle* c = nullptr;
  CHECK(SmCircle::CreateCanonical(*SmApiGetOrCreateContext(), frame, num(a, 1), c), "torus circle");
  SmTArray<SmCurve*> curves;
  curves.Add(c);
  SmBrep* b = nullptr;
  long s = SmApiCreateRotationalSweep(curves, SmPoint3d(0, 0, 0), SmVector3d(0, 0, 1), num(a, 2), TRUE, b);
  freeCurves(curves);
  if (s != SM_SUCCESS || !b) fail("part torus", s);
  uint32_t id = keep(b);
  checkSolid(b, "part torus");
  return jsnum(id);
}

// -------------------------------------------------------------- transforms

val Translate(Args a) {
  CHECK(SmApiTranslate(get(a, 0), SmVector3d(num(a, 1), num(a, 2), num(a, 3))), "translate");
  return val::undefined();
}

val Rotate(Args a) {
  // (handle, px, py, pz, ax, ay, az, degrees)
  CHECK(SmApiRotate(get(a, 0), SmVector3d(num(a, 1), num(a, 2), num(a, 3)),
                    SmVector3d(num(a, 4), num(a, 5), num(a, 6)), num(a, 7)), "rotate");
  return val::undefined();
}

val Scale(Args a) {
  double sx = num(a, 1), sy = num(a, 2), sz = num(a, 3);
  if (sx > 0 && sy > 0 && sz > 0) {
    CHECK(SmApiScale(get(a, 0), SmVector3d(sx, sy, sz)), "scale");
    return val::undefined();
  }
  // a mirror: SmApiScale takes positive factors only, but the brep's own 4x4
  // transform splits a reflection out of the matrix and turns the faces over
  if (!(sx != 0 && sy != 0 && sz != 0)) throw KernelError("a zero scale flattens the solid");
  const double m[4][4] = { { sx, 0, 0, 0 }, { 0, sy, 0, 0 }, { 0, 0, sz, 0 }, { 0, 0, 0, 1 } };
  SmBrep* b = get(a, 0);
  CHECK(b->Transform(m, TRUE), "mirror");
  checkSolid(b, "mirror");
  return val::undefined();
}

// ---------------------------------------------------------------- booleans

bool isSolid(SmBrep* b) {
  SmBoolean ok = FALSE;
  return b && SmApiBrepIsManifoldSolid(b, ok) == SM_SUCCESS && ok;
}

// (op, a, b): a is modified in place and stays valid; b is consumed.
// op: 1 union, 2 intersection, 3 difference (SmBooleanOperationType).
//
// SMLib's merge is not symmetric in its numerics: a bar unioned with an arm
// overlapping it by 0.01 (filament-guide) came back as an open shell, the arm
// unioned with the bar as the right solid. Union and intersection don't care
// about order, so when one order fails they are tried the other way round.
val Boolean(Args args) {
  int op = i32(args[0]);
  uint32_t ida = u32(args[1]);
  uint32_t idb = u32(args[2]);
  SmBrep* a = get(args, 1);
  SmBrep* b = get(args, 2);
  SmBrep* ca = nullptr;
  SmBrep* cb = nullptr;
  if (op != SM_BO_DIFFERENCE && (SmApiBrepCopy(a, ca) != SM_SUCCESS || SmApiBrepCopy(b, cb) != SM_SUCCESS)) {
    delete ca; delete cb; ca = cb = nullptr;
  }
  SmBrep* out = nullptr;
  long s = SmApiBoolean(a, b, (SmBooleanOperationType)op, out);
  if (s == SM_SUCCESS && out) {
    g_solids.erase(idb);          // deleted by the kernel on success
    g_solids[ida] = out;          // aliases a
  }
  if (s == SM_SUCCESS && isSolid(out)) { delete ca; delete cb; return val::undefined(); }
  if (ca && cb) {
    SmBrep* out2 = nullptr;
    long s2 = SmApiBoolean(cb, ca, (SmBooleanOperationType)op, out2);
    if (s2 == SM_SUCCESS && isSolid(out2)) {
      // ca was consumed and cb became out2; drop what the first try left
      if (s == SM_SUCCESS && out) delete out;
      else { delete a; delete b; g_solids.erase(idb); }
      g_solids[ida] = out2;
      return val::undefined();
    }
    if (s2 == SM_SUCCESS && out2) delete out2;
    else { delete ca; delete cb; }
  } else { delete ca; delete cb; }
  if (s != SM_SUCCESS || !out) fail("boolean", s);
  throw KernelError("boolean did not produce a closed solid");
}

// ------------------------------------------------------------------ sweeps
//
// A profile is a flat array of segments: [0, x0,y0,z0, x1,y1,z1] for a line,
// [1, then four control points] for a cubic Bezier, [2, cx,cy,cz, r, start,
// end] for a counter-clockwise circular arc in a plane of constant z (degrees,
// within [-360, 360], end > start; an exact rational quadratic). Loops are
// simply listed one after another; the kernel sorts outer boundaries from
// holes itself.

void profileCurves(Args a, size_t i, SmTArray<SmCurve*>& out) {
  val segs = a[i];
  const size_t n = len(segs);
  for (size_t k = 0; k < n; k++) {
    std::vector<double> s = emscripten::convertJSArrayToNumberVector<double>(segs[k]);
    auto at = [&](size_t j) { return j < s.size() ? s[j] : 0.0; };
    int kind = (int)at(0);
    if (kind == 0) {
      SmLine* l = nullptr;
      CHECK(SmApiCreateLineSegment(SmPoint3d(at(1), at(2), at(3)), SmPoint3d(at(4), at(5), at(6)), l), "line segment");
      out.Add(l);
    } else if (kind == 2) {
      SmBSplineCurve* c = nullptr;
      CHECK(SmApiCreateArc(SmPoint3d(at(1), at(2), at(3)), at(4), at(5), at(6), c), "arc");
      out.Add(c);
    } else {
      SmTArray<SmPoint3d> pts;
      for (int p = 0; p < 4; p++) pts.Add(SmPoint3d(at(1 + 3 * p), at(2 + 3 * p), at(3 + 3 * p)));
      SmTArray<double> knots; knots.Add(0.0); knots.Add(1.0);
      SmTArray<ULONG> mult; mult.Add(4); mult.Add(4);
      SmBSplineCurve* c = nullptr;
      CHECK(SmApiCreateCanonicalCurve(pts, knots, mult, nullptr, 3, SM_CF_POLYLINE_FORM, c), "bezier");
      out.Add(c);
    }
  }
}

void freeCurves(SmTArray<SmCurve*>& curves) {
  for (ULONG k = 0; k < curves.GetSize(); k++) delete curves[k];
}

// (segments, dx, dy, dz, distance)
val Extrude(Args a) {
  SmTArray<SmCurve*> curves;
  try { profileCurves(a, 0, curves); } catch (...) { freeCurves(curves); throw; }
  SmBrep* b = nullptr;
  long s = SmApiCreateLinearSweep(curves, SmVector3d(num(a, 1), num(a, 2), num(a, 3)), num(a, 4), TRUE, b);
  freeCurves(curves);
  if (s != SM_SUCCESS || !b) fail("extrude", s);
  uint32_t id = keep(b);
  checkSolid(b, "extrude");
  return jsnum(id);
}

// A profile (z = 0) as it stands at one height of a twisted extrude: turned
// `deg` about +Z, scaled by `s` about the origin, lifted to `z`. Lines and
// Beziers move point by point; an arc keeps its exact form (its centre moves,
// its radius scales, its angles turn).
void profileCurvesAt(const std::vector<std::vector<double>>& segs, double deg, double s, double z, SmTArray<SmCurve*>& out) {
  const double a = deg * 3.14159265358979323846 / 180.0, c = std::cos(a), sn = std::sin(a);
  auto px = [&](double x, double y) { return s * (x * c - y * sn); };
  auto py = [&](double x, double y) { return s * (x * sn + y * c); };
  for (const auto& g : segs) {
    auto at = [&](size_t j) { return j < g.size() ? g[j] : 0.0; };
    int kind = (int)at(0);
    if (kind == 0) {
      SmLine* l = nullptr;
      CHECK(SmApiCreateLineSegment(SmPoint3d(px(at(1), at(2)), py(at(1), at(2)), z),
                                   SmPoint3d(px(at(4), at(5)), py(at(4), at(5)), z), l), "line segment");
      out.Add(l);
    } else if (kind == 2) {
      // SmApiCreateArc takes angles within [-360, 360] (status 1007 past
      // that): turned, the start is brought back to [-180, 180) and, if the
      // end then runs past 360, both drop a turn
      const double sweep = at(6) - at(5);
      double a0 = std::fmod(at(5) + deg, 360.0);
      if (a0 >= 180.0) a0 -= 360.0;
      if (a0 < -180.0) a0 += 360.0;
      if (a0 + sweep > 360.0) a0 -= 360.0;
      SmBSplineCurve* cv = nullptr;
      CHECK(SmApiCreateArc(SmPoint3d(px(at(1), at(2)), py(at(1), at(2)), z), s * at(4), a0, a0 + sweep, cv), "arc");
      out.Add(cv);
    } else {
      SmTArray<SmPoint3d> pts;
      for (int p = 0; p < 4; p++) {
        double x = at(1 + 3 * p), y = at(2 + 3 * p);
        pts.Add(SmPoint3d(px(x, y), py(x, y), z));
      }
      SmTArray<double> knots; knots.Add(0.0); knots.Add(1.0);
      SmTArray<ULONG> mult; mult.Add(4); mult.Add(4);
      SmBSplineCurve* cv = nullptr;
      CHECK(SmApiCreateCanonicalCurve(pts, knots, mult, nullptr, 3, SM_CF_POLYLINE_FORM, cv), "bezier");
      out.Add(cv);
    }
  }
}

// (segments, h, twistDeg | [deg per profile], topScale | [scale per profile], profiles, matrix16)
// A twisted (and/or tapered) extrude: the profile copied `profiles` times up
// the height, each turned and scaled its share, and a solid skinned through
// them (cubic in the loft direction, capped flat at both ends). matrix16 is
// three.js's column-major 4x4, applied afterwards: column i is where axis i
// goes, which is exactly SmBrep::Transform's row i.
val TwistExtrude(Args a) {
  const double h = num(a, 1);
  const ULONG n = (ULONG)u32(a[4]);
  // a number spreads evenly up the height; a list gives each profile its own
  // (src/twist.js works out curved ones)
  auto per = [&](size_t arg, ULONG k, bool scale) -> double {
    const double u = (double)k / (double)(n - 1);
    val v = a[arg];
    if (v.isArray()) {
      if (len(v) != n) throw KernelError("twist extrude: one value per profile");
      return v[k].as<double>();
    }
    const double x = v.as<double>();
    return scale ? 1.0 + (x - 1.0) * u : x * u;
  };
  if (!(h > 0) || n < 2) throw KernelError("twist extrude: bad height or profile count");
  for (ULONG k = 0; k < n; k++) if (!(per(3, k, true) > 0)) throw KernelError("twist extrude: a scale must be above 0");
  std::vector<std::vector<double>> segs;
  {
    val sv = a[0];
    const size_t ns = len(sv);
    for (size_t k = 0; k < ns; k++) segs.push_back(emscripten::convertJSArrayToNumberVector<double>(sv[k]));
  }
  SmTArray<SmCurve*> curves;
  SmTArray<ULONG> counts;
  try {
    for (ULONG k = 0; k < n; k++) {
      const double u = (double)k / (double)(n - 1);
      const ULONG before = curves.GetSize();
      profileCurvesAt(segs, per(2, k, false), per(3, k, true), h * u, curves);
      counts.Add(curves.GetSize() - before);
    }
  } catch (...) { freeCurves(curves); throw; }
  SmTArray<const SmCurve*> view;
  for (ULONG k = 0; k < curves.GetSize(); k++) view.Add(curves[k]);
  SmBrep* b = nullptr;
  long s = SmApiCreateSkinPrimitive(counts, view, n >= 4 ? 3 : 1, TRUE, b, nullptr, nullptr, nullptr);
  freeCurves(curves);
  if (s != SM_SUCCESS || !b) fail("twist extrude", s);
  uint32_t id = keep(b);
  std::vector<double> m = emscripten::convertJSArrayToNumberVector<double>(a[5]);
  if (m.size() != 16) throw KernelError("twist extrude: the placement takes 16 numbers");
  double t[4][4];
  bool identity = true;
  for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
    t[r][c] = m[4 * r + c];
    if (std::fabs(t[r][c] - (r == c ? 1.0 : 0.0)) > 1e-12) identity = false;
  }
  if (!identity) CHECK(b->Transform(t, TRUE), "twist extrude placement");
  checkSolid(b, "twist extrude");
  return jsnum(id);
}

// (segments, px, py, pz, ax, ay, az, degrees)
val Revolve(Args a) {
  SmTArray<SmCurve*> curves;
  try { profileCurves(a, 0, curves); } catch (...) { freeCurves(curves); throw; }
  SmBrep* b = nullptr;
  long s = SmApiCreateRotationalSweep(curves, SmPoint3d(num(a, 1), num(a, 2), num(a, 3)),
                                      SmVector3d(num(a, 4), num(a, 5), num(a, 6)), num(a, 7), TRUE, b);
  freeCurves(curves);
  if (s != SM_SUCCESS || !b) fail("revolve", s);
  uint32_t id = keep(b);
  checkSolid(b, "revolve");
  return jsnum(id);
}

// ----------------------------------------------------------------- fillets

// (handle, [edge indices], radius, xsect) xsect: 0 linear (chamfer), 1 circular
val Fillet(Args a) {
  SmBrep* b = get(a, 0);
  SmTArray<SmEdge*> all;
  b->GetEdges(all);
  std::vector<double> idx = emscripten::convertJSArrayToNumberVector<double>(a[1]);
  SmTArray<SmEdge*> pick;
  for (double d : idx) {
    uint32_t e = (uint32_t)d;
    if (e >= all.GetSize()) throw KernelError("edge index out of range");
    pick.Add(all[e]);
  }
  int xsect = i32(a[3]);
  CHECK(SmApiFilletEdges(b, pick, num(a, 2), (ULONG)xsect, 1, 1.0), xsect ? "fillet" : "chamfer");
  checkSolid(b, xsect ? "fillet" : "chamfer");
  return val::undefined();
}

// ----------------------------------------------------------------- queries

SmVector3d outwardNormal(SmFace* f, const SmPoint3d& p) {
  SmPoint3d cp;
  SmTArray<SmPoint2d> uvs;
  double d = 0;
  SmVector3d n(0, 0, 0);
  if (SmApiSurfaceClosestPoint(f->GetSurface(), p, cp, uvs, d) != SM_SUCCESS || uvs.GetSize() == 0) return n;
  SmVector2d uv(uvs[0].x, uvs[0].y);
  if (SmApiEvaluateSurfaceNormal(f->GetSurface(), uv, n) != SM_SUCCESS) return SmVector3d(0, 0, 0);
  SmBrep* brep = f->GetBrep();
  SmFaceuse* up = f->GetUpwardFaceuse();
  if (brep && up && up->GetShell() && brep->GetInfiniteRegion() != up->GetShell()->GetRegion()) n *= -1.0;
  return n;
}

// faces(handle) -> Float64Array, 6 per face: an interior point and the
// outward normal there.
val Faces(Args a) {
  SmBrep* b = get(a, 0);
  SmTArray<SmFace*> faces;
  b->GetFaces(faces);
  std::vector<double> out(faces.GetSize() * 6);
  for (ULONG i = 0; i < faces.GetSize(); i++) {
    SmPoint3d p(0, 0, 0);
    if (SmApiFaceInternalPoint(faces[i], p) != SM_SUCCESS) {
      SmPoint3d mn, mx;
      SmApiFaceBoundingBox(faces[i], FALSE, mn, mx);
      p = SmPoint3d((mn.x + mx.x) / 2, (mn.y + mx.y) / 2, (mn.z + mx.z) / 2);
    }
    SmVector3d n = outwardNormal(faces[i], p);
    double* o = out.data() + 6 * i;
    o[0] = p.x; o[1] = p.y; o[2] = p.z; o[3] = n.x; o[4] = n.y; o[5] = n.z;
  }
  return f64(out.data(), out.size());
}

// matchFaces(out, [sources...], tol) -> Int32Array, 2 per face of `out`:
// (source position, face index) of the source face it lies on, or (-1, -1)
// for a face no source had (a fillet's blend, a boolean's new seam face).
val MatchFaces(Args a) {
  SmBrep* b = get(a, 0);
  std::vector<double> srcIds = emscripten::convertJSArrayToNumberVector<double>(a[1]);
  double tol = num(a, 2);
  std::vector<SmTArray<SmFace*>> src(srcIds.size());
  std::vector<std::vector<std::pair<SmPoint3d, SmPoint3d>>> boxes(srcIds.size());
  for (size_t s = 0; s < srcIds.size(); s++) {
    uint32_t id = (uint32_t)srcIds[s];
    auto it = g_solids.find(id);
    if (it == g_solids.end()) throw KernelError("unknown source handle");
    it->second->GetFaces(src[s]);
    for (ULONG j = 0; j < src[s].GetSize(); j++) {
      SmPoint3d mn, mx;
      SmApiFaceBoundingBox(src[s][j], FALSE, mn, mx);
      boxes[s].push_back({ mn, mx });
    }
  }
  SmTArray<SmFace*> faces;
  b->GetFaces(faces);
  std::vector<int32_t> out(faces.GetSize() * 2);
  for (ULONG i = 0; i < faces.GetSize(); i++) {
    int bs = -1, bf = -1;
    double best = tol;
    SmPoint3d p;
    if (SmApiFaceInternalPoint(faces[i], p) == SM_SUCCESS) {
      SmVector3d n = outwardNormal(faces[i], p);
      for (size_t s = 0; s < src.size() && best > 0; s++) {
        for (ULONG j = 0; j < src[s].GetSize(); j++) {
          const auto& bx = boxes[s][j];
          if (p.x < bx.first.x - tol || p.y < bx.first.y - tol || p.z < bx.first.z - tol ||
              p.x > bx.second.x + tol || p.y > bx.second.y + tol || p.z > bx.second.z + tol) continue;
          SmPoint3d cp;
          double d = 1e300;
          if (SmApiFaceClosestPoint(src[s][j], p, cp, d) != SM_SUCCESS || d >= best) continue;
          // a face of the same surface but the other side of a thin wall
          // lies within tolerance too; the normals tell them apart (either
          // sign: a subtracted tool's face is flipped in the result)
          SmVector3d m = outwardNormal(src[s][j], cp);
          double dot = std::fabs(n.x * m.x + n.y * m.y + n.z * m.z);
          if (dot < 0.98) continue;
          best = d; bs = (int)s; bf = (int)j;
        }
      }
    }
    out[2 * i] = bs;
    out[2 * i + 1] = bf;
  }
  return val::global("Int32Array").new_(val(emscripten::typed_memory_view(out.size(), out.data())));
}

// edges(handle) -> Float64Array, 5 per edge: face index A, face index B
// (-1 if missing), and the edge's midpoint.
val Edges(Args a) {
  SmBrep* b = get(a, 0);
  SmTArray<SmFace*> faces;
  b->GetFaces(faces);
  std::unordered_map<SmFace*, int> faceIdx;
  for (ULONG i = 0; i < faces.GetSize(); i++) faceIdx[faces[i]] = (int)i;
  SmTArray<SmEdge*> edges;
  b->GetEdges(edges);
  std::vector<double> out(edges.GetSize() * 5);
  for (ULONG i = 0; i < edges.GetSize(); i++) {
    SmTArray<SmFace*> ef;
    edges[i]->GetFaces(ef);
    double* o = out.data() + 5 * i;
    o[0] = ef.GetSize() > 0 && faceIdx.count(ef[0]) ? faceIdx[ef[0]] : -1;
    o[1] = ef.GetSize() > 1 && faceIdx.count(ef[1]) ? faceIdx[ef[1]] : -1;
    double lo = 0, hi = 0;
    SmVector3d mid(0, 0, 0);
    if (SmApiEdgeParameterRange(edges[i], lo, hi) == SM_SUCCESS) {
      SmVector3d* pt = &mid;
      SmVector3d* d1 = nullptr;
      SmVector3d* d2 = nullptr;
      SmApiEvaluateCurve(edges[i]->GetCurve(), (lo + hi) / 2, pt, d1, d2);
    }
    o[2] = mid.x; o[3] = mid.y; o[4] = mid.z;
  }
  return f64(out.data(), out.size());
}

// bbox(handle) -> [minX, minY, minZ, maxX, maxY, maxZ], tight to the surfaces
val BBox(Args a) {
  SmPoint3d mn, mx;
  CHECK(SmApiBrepBoundingBox(get(a, 0), TRUE, mn, mx), "bounding box");
  const double out[6] = { mn.x, mn.y, mn.z, mx.x, mx.y, mx.z };
  return f64(out, 6);
}

val Volume(Args a) {
  double v = 0;
  CHECK(SmApiBrepComputeVolume(get(a, 0), 1e-3, v), "volume");
  return jsnum(v);
}

// ------------------------------------------------------------ tessellation

// tessellate(handle, chordHeight, angleDeg) ->
//   { positions: Float32Array, normals: Float32Array, indices: Uint32Array,
//     faceIds: Uint32Array (B-rep face index per triangle) }
// Vertices are per face, so a crease stays sharp and each face's normals are
// its own.
val Tessellate(Args a) {
  SmBrep* b = get(a, 0);
  SmTessellationParams params;
  params.dChordHeightTol = num(a, 1);
  params.dCurveAngleTolDeg = num(a, 2);
  params.dSurfaceAngleTolDeg = num(a, 2);
  // optional: a longest edge (world units), and a worst aspect ratio. A
  // twisted wall is curved across its width as well as up it, and without an
  // edge limit the tessellator draws it as slivers that cut through the curve
  // (the aspect limit alone changed nothing, measured)
  if (argc(a) > 3 && a[3].isNumber()) params.dMaxEdgeLength = num(a, 3);
  if (argc(a) > 4 && a[4].isNumber()) params.dMaxAspectRatio = num(a, 4);
  SmPolyBrep* pb = nullptr;
  CHECK(SmApiTessellate(b, pb, params), "tessellate");

  SmTArray<SmFace*> faces;
  b->GetFaces(faces);
  std::unordered_map<SmFace*, uint32_t> faceIdx;
  for (ULONG i = 0; i < faces.GetSize(); i++) faceIdx[faces[i]] = i;

  std::vector<float> pos, nrm;
  std::vector<uint32_t> idx, fid;
  SmTArray<SmPolyFace*> polyFaces;
  pb->GetPolyFaces(polyFaces);
  for (ULONG i = 0; i < polyFaces.GetSize(); i++) {
    SmPolyFace* pf = polyFaces[i];
    SmPolyLoop* loop = pf->GetOuterPolyLoop();
    if (!loop) continue;
    SmTArray<SmPolyEdge*> pe;
    loop->GetPolyEdges(pe);
    ULONG n = pe.GetSize();
    if (n < 3) continue;
    SmFace* orig = pf->GetOriginalFace();
    uint32_t f = faceIdx.count(orig) ? faceIdx[orig] : 0xFFFFFFFFu;
    uint32_t base = (uint32_t)(pos.size() / 3);
    for (ULONG j = 0; j < n; j++) {
      SmPolyVertex* v = pe[j]->GetStartPolyVertex();
      const SmPoint3d& p = v->GetPoint();
      pos.push_back((float)p.x); pos.push_back((float)p.y); pos.push_back((float)p.z);
      // the vertex's normal on this face, not the average across a crease
      const auto& ns = v->GetNormalsRef();
      const auto& fs = v->GetFacesRef();
      SmVector3d nv(0, 0, 0);
      bool found = false;
      for (ULONG k = 0; k < fs.GetSize() && k < ns.GetSize(); k++) {
        if (fs[k] == static_cast<SmTopology*>(orig)) { nv = ns[k]; found = true; break; }
      }
      if (!found) for (ULONG k = 0; k < ns.GetSize(); k++) nv += ns[k];
      double len = std::sqrt(nv.x * nv.x + nv.y * nv.y + nv.z * nv.z);
      if (len > 1e-30) nv /= len;
      nrm.push_back((float)nv.x); nrm.push_back((float)nv.y); nrm.push_back((float)nv.z);
    }
    for (ULONG j = 1; j + 1 < n; j++) {
      idx.push_back(base); idx.push_back(base + j); idx.push_back(base + j + 1);
      fid.push_back(f);
    }
  }
  delete pb;

  auto f32 = [](const std::vector<float>& v) {
    return val::global("Float32Array").new_(val(emscripten::typed_memory_view(v.size(), v.data())));
  };
  auto u32a = [](const std::vector<uint32_t>& v) {
    return val::global("Uint32Array").new_(val(emscripten::typed_memory_view(v.size(), v.data())));
  };
  val o = val::object();
  o.set("positions", f32(pos));
  o.set("normals", f32(nrm));
  o.set("indices", u32a(idx));
  o.set("faceIds", u32a(fid));
  return o;
}

// ------------------------------------------------------------------ module

// Once, before anything else (src/api.mjs calls it). In the Node addon this
// was Init: SMLib.dll sets up its thread-local storage in DllMain, and linked
// in statically there is no such hook. Off Windows and without threads it is
// a no-op, but kept so the two builds start the kernel the same way.
val Init(Args) {
  SmThreadLocalStorage::SetupTLS(SM_TLS_PROCESS_ATTACH);
  SmApiCreateContext();
  return val::undefined();
}

// The message of the last call that failed, once; undefined if it succeeded.
val LastError(val) {
  if (!g_failed) return val::undefined();
  g_failed = false;
  return val(g_error);
}

// the wasm heap as it stands (it only grows), for memory measurements
val HeapBytes(Args) {
  return jsnum((double)emscripten_get_heap_size());
}

template <val (*F)(Args)>
val guard(val args) {
  g_failed = false;
  try { return F(args); }
  catch (const KernelError& e) { g_error = e.what(); }
  catch (const std::exception& e) { g_error = std::string("SMLib: ") + e.what(); }
  catch (...) { g_error = "SMLib: unknown failure"; }
  g_failed = true;
  return val::undefined();
}

}  // namespace

EMSCRIPTEN_BINDINGS(smlib) {
  using emscripten::function;
  function("init", &guard<Init>);
  function("lastError", &LastError);
  function("heapBytes", &guard<HeapBytes>);
  function("info", &guard<Info>);
  function("free", &guard<Free>);
  function("count", &guard<Count>);
  function("copy", &guard<Copy>);
  function("box", &guard<Box>);
  function("cylinder", &guard<Cylinder>);
  function("cone", &guard<Cone>);
  function("sphere", &guard<Sphere>);
  function("torus", &guard<Torus>);
  function("torusArc", &guard<TorusArc>);
  function("translate", &guard<Translate>);
  function("rotate", &guard<Rotate>);
  function("scale", &guard<Scale>);
  function("boolean", &guard<Boolean>);
  function("extrude", &guard<Extrude>);
  function("twistExtrude", &guard<TwistExtrude>);
  function("revolve", &guard<Revolve>);
  function("fillet", &guard<Fillet>);
  function("faces", &guard<Faces>);
  function("matchFaces", &guard<MatchFaces>);
  function("edges", &guard<Edges>);
  function("bbox", &guard<BBox>);
  function("volume", &guard<Volume>);
  function("tessellate", &guard<Tessellate>);
}
