// Host for the vendored cs3 map/actor model (examples/integration/).
//
// It does the two things a host that ships QL_OBJECT bindings owns
// (docs/briefs/add-qlmeta-to-cpp.md):
//
//   integration_demo                 register CMap/CLayer/CActor, drive them
//                                    from a script and CHECK every bound line;
//                                    exits non-zero if any binding is broken.
//   integration_demo --dump-qlm F    write those bindings as a QLM1 manifest.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "actor.h"
#include "layer.h"
#include "map.h"
#include "qlang/QLangContext.h"
#include "qlang/QLangErrors.h"
#include "qlang/QLangManifest.h"
#include "qlang/QLangMeta.h"
#include "sprtypes.h"
#include "tilesdata.h"

using qlang::Member;
using qlang::QLangContext;
using qlang::QValue;
using qlang::TypeMeta;

namespace {

int failures = 0;
QLangContext ctx;

#define CHECK(cond)                                  \
  do {                                               \
    if (!(cond)) {                                   \
      ++failures;                                    \
      std::printf("FAIL %d: %s\n", __LINE__, #cond); \
    }                                                \
  } while (0)

QValue got(const std::string &expr) {
  ctx.run("r = " + expr);
  return ctx.get("r");
}

// as<int64_t>() takes any numeric script type (QLangValue.cpp), so one reader
// covers whatever tag a binding happens to produce (Byte/Word/Short/Int/Bool).
long num(const QValue &v) { return static_cast<long>(v.as<int64_t>()); }
long num(const std::string &expr) { return num(got(expr)); }
bool truthy(const QValue &v) { return num(v) != 0; }
bool truthy(const std::string &expr) { return num(expr) != 0; }
std::string str(const std::string &expr) { return got(expr).asString(); }

// A bound member that cannot carry the value must fail loudly, whatever the
// host exception type (CLayer::at throws std::out_of_range, the VM throws
// QLangError).
bool bad(const std::string &src) {
  try {
    ctx.run(src);
    return false;
  } catch (const std::exception &) {
    return true;
  }
}

const Member *member(const TypeMeta &m, const char *name) {
  for (const Member &e : m.members)
    if (e.name == name) return &e;
  return nullptr;
}

long enumValue(const TypeMeta &m, const char *table, const char *name) {
  for (const auto &em : m.enums)
    if (em.typeName == table)
      for (const auto &e : em.entries)
        if (e.name == name) return e.value;
  return -1;
}

} // namespace

int main(int argc, char **argv) {
  // The manifest is an artifact of the binary that registers the types, so the
  // flag lives here rather than in a lookalike target.
  if (argc > 1 && std::strcmp(argv[1], "--dump-qlm") == 0) {
    const char *path = argc > 2 ? argv[2] : "host.qlm";
    std::ofstream out(path, std::ios::binary);
    qlang::writeTypeManifest(out);
    std::printf("wrote %zu types to %s\n", qlang::metaRegistry().size(), path);
    return 0;
  }

  const auto mapType = ctx.import<CMap>();
  const auto layerType = ctx.import<CLayer>();
  const auto actorType = ctx.import<CActor>();

  auto map = std::make_shared<CMap>(4, 3);
  map->set(1, 1, 7);
  auto layer = std::make_shared<CLayer>(2, 2, CLayer::LAYER_MAIN, 16, "floor");
  layer->tilesFrom({1, 2, 3, 4});
  auto actor = std::make_shared<CActor>(3, 4, TYPE_PLAYER);

  ctx.set("m", qlang::qval_external_owned<CMap>(mapType, map));
  ctx.set("l", qlang::qval_external_owned<CLayer>(layerType, layer));
  ctx.set("a", qlang::qval_external_owned<CActor>(actorType, actor));

  // The demo: the script a level author would write.
  ctx.run(R"ql(
    print(f"map {m} tile={m.at(1, 1)} layers={m.layerCount}\n")
    print(f"layer {l} name={l.getName()} base={l.baseID}\n")
    print(f"actor {a} aim={a.aim} ttl={a.ttl}\n")
  )ql");

  // --- CMap (map.h) -------------------------------------------------------
  CHECK(num("m.width") == map->width());
  CHECK(num("m.height") == map->height());
  CHECK(num("m.size") == static_cast<long>(map->size()));
  CHECK(num("m.layerCount") == static_cast<long>(map->layerCount()));
  CHECK(truthy("m.isValid(0, 0)"));
  CHECK(!truthy("m.isValid(4, 3)"));
  CHECK(num("m.at(1, 1)") == 7);
  CHECK(bad("m.at(9, 9)")); // out of range -> host exception, no UB
  CHECK(str("m.title()").empty());
  CHECK(str("m.lastError()").empty());
  CHECK(num("CMap.UP") == UP);
  CHECK(num("CMap.RIGHT") == RIGHT);
  CHECK(num("CMap.VERSION3") == CMap::VERSION3);
  CHECK(str("str(m)") == map->str()); // QL_STR drives str()/print/f-strings
  CHECK(str("f\"{m}\"") == map->str());
  CHECK(str("type(m)") == "CMap");
  CHECK(num("CMap(2, 2).size") == 4); // typed QL_CTOR
  CHECK(bad("CMap(1)")); // arity is checked at the call
  CHECK(bad("m.nope")); // an unbound name is an error
  CHECK(!bad("m.clear")); // kind 2: a value, not a call
  CHECK(map->size() != 0); // ...so nothing was cleared

  // --- CLayer (layer.h) ---------------------------------------------------
  CHECK(num("l.width") == layer->width());
  CHECK(num("l.height") == layer->height());
  CHECK(num("l.size") == static_cast<long>(layer->size()));
  CHECK(num("l.baseID") == 16);
  CHECK(!bad("l.baseID = 32")); // READ/WRITE pair -> writable
  CHECK(layer->baseID() == 32);
  CHECK(truthy("l.isValid(1, 1)"));
  CHECK(num("l.at(0, 1)") == 3); // row-major: y * width + x
  CHECK(str("l.getName()") == "floor");
  CHECK(num("l.layerType()") == CLayer::LAYER_MAIN);
  CHECK(num("CLayer.LAYER_DECOR") == CLayer::LAYER_DECOR);
  CHECK(str("l.lastError()").empty());
  CHECK(str("str(l)") == layer->str());
  CHECK(num("CLayer(3, 3, \"deco\").size") == 9);
  CHECK(bad("l.fill(1)")); // uint16_t argument: no as<T>(), so not bound

  // --- CActor (actor.h) ---------------------------------------------------
  CHECK(num("a.x") == actor->x());
  CHECK(num("a.y") == actor->y());
  CHECK(num("a.type") == TYPE_PLAYER);
  CHECK(num("a.attr") == 0);
  CHECK(num("a.frame") == 0);
  CHECK(num("a.PU") == actor->getPU());
  CHECK(num("a.aim") == actor->getAim());
  CHECK(!truthy("a.boss"));
  CHECK(num("a.granularFactor") == actor->getGranularFactor());
  CHECK(num("a.ttl") == CActor::NoTTL);
  CHECK(!bad("a.ttl = 5")); // READ/WRITE pair on an int accessor
  CHECK(actor->getTTL() == 5);
  CHECK(num("a.decTTL()") == actor->getTTL());
  CHECK(num("a.ttl") == 4);
  CHECK(truthy("a.isWithin(0, 0, 8, 8)"));
  CHECK(!truthy("a.isWithin(4, 5, 8, 8)"));
  CHECK(num("a.findNextDir(false)") == actor->findNextDir(false));
  CHECK(num("CActor.AIM_LEFT") == AIM_LEFT);
  CHECK(str("str(a)") == actor->str());
  CHECK(str("type(a)") == "CActor");
  CHECK(num("CActor(1, 2, 3).x") == 1);
  CHECK(bad("CActor(1)"));
  CHECK(bad("a.type = 5")); // read-only: the getter returns uint8_t
  CHECK(bad("a.setAttr(1)")); // uint8_t argument: not bound

  // --- the same members from the host side (one source of truth) ----------
  const TypeMeta &mt = CMap::qlang_meta();
  const TypeMeta &lt = CLayer::qlang_meta();
  const TypeMeta &at = CActor::qlang_meta();
  CHECK(mt.name == "CMap" && lt.name == "CLayer" && at.name == "CActor");
  CHECK(num(member(mt, "width")->get(map.get())) == 4);
  CHECK(member(mt, "width")->kind == 1 && member(mt, "width")->set == nullptr);
  CHECK(truthy(
      member(mt, "isValid")
          ->invoke(map.get(), ctx, {qlang::qval_int(3), qlang::qval_int(2)})));
  CHECK(!truthy(
      member(mt, "isValid")
          ->invoke(map.get(), ctx, {qlang::qval_int(4), qlang::qval_int(2)})));
  CHECK(num(member(mt, "at")->invoke(
            map.get(), ctx, {qlang::qval_int(1), qlang::qval_int(1)})) == 7);
  CHECK(member(mt, "clear")->kind == 2);
  CHECK(mt.toString(map.get()).asString() == map->str());
  CHECK(mt.hasStr && mt.hasCtor && mt.ctorArgTags.size() == 2);
  CHECK(mt.signals.empty());

  CHECK(member(lt, "baseID")->kind == 0 && member(lt, "baseID")->set);
  member(lt, "baseID")->set(layer.get(), qlang::qval_int(9));
  CHECK(layer->baseID() == 9);
  CHECK(num("l.baseID") == 9); // host write is visible to the script
  CHECK(member(lt, "getName")->invoke(layer.get(), ctx, {}).asString() ==
        "floor");
  CHECK(num(member(lt, "at")->invoke(
            layer.get(), ctx, {qlang::qval_int(1), qlang::qval_int(1)})) == 4);
  CHECK(member(lt, "at")->retTag == qlang::QTypeTag::Word);
  CHECK(lt.toString(layer.get()).asString() == layer->str());

  CHECK(member(at, "x")->kind == 1 && member(at, "x")->set == nullptr);
  CHECK(member(at, "attr")->kind == 1); // a const getter is a property
  CHECK(num(member(at, "attr")->get(actor.get())) == 0);
  CHECK(member(at, "ttl")->kind == 0 && member(at, "ttl")->set);
  member(at, "ttl")->set(actor.get(), qlang::qval_int(3));
  CHECK(actor->getTTL() == 3 && num("a.ttl") == 3);
  CHECK(truthy(member(at, "isWithin")
                   ->invoke(actor.get(), ctx,
                            {qlang::qval_int(0), qlang::qval_int(0),
                             qlang::qval_int(8), qlang::qval_int(8)})));
  CHECK(num(member(at, "findNextDir")
                ->invoke(actor.get(), ctx, {qlang::qval_bool(false)})) ==
        actor->findNextDir(false));
  CHECK(at.toString(actor.get()).asString() == actor->str());
  CHECK(enumValue(at, "JoyAim", "AIM_LEFT") == AIM_LEFT);
  CHECK(enumValue(lt, "LayerType", "LAYER_DECOR") == CLayer::LAYER_DECOR);
  CHECK(enumValue(mt, "MapVersion", "VERSION3") == CMap::VERSION3);

  // --- QLM1 manifest ------------------------------------------------------
  {
    std::stringstream blob(std::ios::in | std::ios::out | std::ios::binary);
    qlang::writeTypeManifest(blob);
    const auto types = qlang::readManifest(blob);
    CHECK(types.size() == 3);
    for (const auto &t : types) {
      const TypeMeta &src =
          t.name == "CMap" ? mt : (t.name == "CLayer" ? lt : at);
      // The manifest is the meta with the closures removed: same shape.
      CHECK(t.name == src.name);
      CHECK(t.members.size() == src.members.size());
      CHECK(t.enums.size() == src.enums.size());
      CHECK(t.signals.empty());
      CHECK(t.hasStr == src.hasStr);
      CHECK(t.hasCtor == src.hasCtor);
      CHECK(t.ctorArgTags == src.ctorArgTags);
      for (const auto &m : t.members)
        CHECK(member(src, m.name.c_str()) &&
              member(src, m.name.c_str())->kind == m.kind);
    }
    CHECK(member(at, "x")->getTag == qlang::QTypeTag::Short);
    CHECK(member(at, "ttl")->setTag == qlang::QTypeTag::Int);
    CHECK(member(mt, "at")->argTags.size() == 2);
  }

  if (failures) {
    std::printf("%d FAILURES\n", failures);
    return 1;
  }
  std::printf("integration_demo: all checks passed\n");
  return 0;
}
