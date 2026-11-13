/*
    cs3-runtime-sdl
    Copyright (C) 2024  Francois Blanchette

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
#pragma once
#include <cstdint>
#include <string>

#include "isprite.h"
#include "joyaim.h"
#include "map.h"
#include "qlang/QLangMeta.h"

// QLang external type (docs/briefs/add-qlmeta-to-cpp.md). QLObject<CActor> has
// to stay the FIRST base: the typed handoff validates a script-side pointer
// with dynamic_cast through that subobject, so a non-zero offset would make
// every access fail with "external type mismatch".
class CActor : public qlang::QLObject<CActor>, public ISprite {
public:
  enum {
    NoTTL = -1
  };

  CActor(const uint8_t x = 0, const uint8_t y = 0, const uint8_t type = 0,
         const JoyAim aim = AIM_UP);
  CActor(const Pos &pos, uint8_t type = 0, JoyAim aim = AIM_UP);

  CActor(CActor &&other) noexcept;
  CActor &operator=(CActor &&other) noexcept;

  CActor(const CActor &) = delete;
  CActor &operator=(const CActor &) = delete;

  ~CActor();

  bool canMove(const JoyAim aim) const override;
  void move(const JoyAim aim) override;
  inline int16_t x() const override { return static_cast<int16_t>(m_x); }
  inline int16_t y() const override { return static_cast<int16_t>(m_y); }
  inline uint8_t type() const override { return m_type; }
  inline uint8_t attr() const { return m_attr; }
  inline void setAttr(const uint8_t attr) { m_attr = attr; }
  uint8_t getPU() const;
  void setPU(const uint8_t c);
  void setPos(const Pos &pos);
  JoyAim getAim() const override;
  void setAim(const JoyAim aim) override;
  JoyAim findNextDir(const bool reverse = false) const;
  bool isPlayerThere(JoyAim aim) const;
  uint8_t tileAt(JoyAim aim) const;
  void setType(const uint8_t type);
  bool isWithin(const int x1, const int y1, const int x2, const int y2) const;
  void reverveDir();
  const Pos pos() const override;
  int distance(const CActor &actor) const override;
  void move(const int16_t x, const int16_t y) override;
  void move(const Pos pos) override;
  inline int16_t getGranularFactor() const override {
    return ACTOR_GRANULAR_FACTOR;
  };
  bool isBoss() const override { return false; }
  int getTTL() const override { return m_ttl; };
  void setTTL(int ttl) { m_ttl = ttl; };
  int decTTL() {
    if (m_ttl > 0) --m_ttl;
    return m_ttl;
  }
  uint8_t frame() const { return m_frame; }
  void setFrame(const uint8_t frame) { m_frame = frame; }
  void setOrigin(const Pos &origin) { m_origin = origin; }
  Pos origin() const { return m_origin; }

  /// Script-visible rendering (QL_STR): str(actor), print(actor), f"{actor}".
  std::string str() const {
    return "Actor(" + std::to_string(x()) + "," + std::to_string(y()) +
           " type " + std::to_string(type()) + ")";
  }

private:
  uint8_t m_x;
  uint8_t m_y;
  uint8_t m_type;
  uint16_t m_frame;
  JoyAim m_aim;
  uint8_t m_pu;
  uint8_t m_attr;
  int32_t m_ttl;
  Pos m_origin;
  enum : uint16_t {
    ACTOR_GRANULAR_FACTOR = 1
  };

  // QLang bindings, ordered properties -> methods -> enums -> QL_STR ->
  // QL_CTOR. Members the boundary conversion cannot carry are left out
  // (docs/EXTERNAL_TYPES.md §7): setAttr/setPU/setType take uint8_t and
  // canMove/move/tileAt/isPlayerThere/setAim take JoyAim, which as<T>() has no
  // specialization for; `move` is overloaded three ways; `distance` takes a
  // CActor&; and `pos`/`origin` return Pos. The setters that do exist
  // therefore bind read-only (attr, PU, frame); `ttl` is the one writable
  // property. No renaming, so `NoTTL` (an anonymous enum) has no home here.
  QL_OBJECT(QL_PROPERTY(ttl, READ(getTTL), WRITE(setTTL)), QL_PROPERTY(x),
            QL_PROPERTY(y), QL_PROPERTY(type), QL_PROPERTY(attr),
            QL_PROPERTY(frame), QL_PROPERTY(PU, READ(getPU)),
            QL_PROPERTY(aim, READ(getAim)), QL_PROPERTY(boss, READ(isBoss)),
            QL_PROPERTY(granularFactor, READ(getGranularFactor)),
            QL_INVOKABLE(isWithin), QL_INVOKABLE(decTTL),
            QL_INVOKABLE(findNextDir),
            QL_ENUM(JoyAim, AIM_UP, AIM_DOWN, AIM_LEFT, AIM_RIGHT, AIM_NONE),
            QL_STR(str), QL_CTOR([](int x_, int y_, int type_) {
              return std::make_shared<CActor>(static_cast<uint8_t>(x_),
                                              static_cast<uint8_t>(y_),
                                              static_cast<uint8_t>(type_));
            }));
};
