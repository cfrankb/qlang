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
#include "actor.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>

#include "attr.h"
#include "logger.h"
#include "sprtypes.h"
#include "tilesdata.h"
#include "tilesdefs.h"

namespace ActorData {
constexpr const JoyAim g_aims[] = {AIM_DOWN,  AIM_RIGHT, AIM_UP,    AIM_LEFT,
                                   AIM_UP,    AIM_LEFT,  AIM_DOWN,  AIM_RIGHT,
                                   AIM_RIGHT, AIM_UP,    AIM_LEFT,  AIM_DOWN,
                                   AIM_LEFT,  AIM_DOWN,  AIM_RIGHT, AIM_UP};

}

using namespace ActorData;

/**
 * @brief Reverse a given direction
 *
 * @param aim
 * @return JoyAim
 */
JoyAim reverseDir(const JoyAim aim) {
  switch (aim) {
  case AIM_UP: return AIM_DOWN;
  case AIM_DOWN: return AIM_UP;
  case AIM_RIGHT: return AIM_LEFT;
  case AIM_LEFT: return AIM_RIGHT;
  default: return aim;
  }
}

CActor::CActor(const uint8_t x, const uint8_t y, const uint8_t type,
               const JoyAim aim) {
  if (aim >= TOTAL_AIMS && aim != AIM_NONE)
    throw std::invalid_argument("Invalid aim value");
  m_x = x;
  m_y = y;
  m_type = type;
  m_aim = aim;
  m_pu = TILES_BLANK;
  m_ttl = CActor::NoTTL;
  m_attr = 0;
  m_origin = Pos{x, y};
}

CActor::CActor(const Pos &pos, uint8_t type, JoyAim aim) {
  m_x = pos.x;
  m_y = pos.y;
  m_type = type;
  m_aim = aim;
  m_pu = TILES_BLANK;
  m_ttl = CActor::NoTTL;
  m_attr = 0;
  m_origin = pos;
}

CActor::CActor(CActor &&other) noexcept
    : m_x(other.m_x),
      m_y(other.m_y),
      m_type(other.m_type),
      m_aim(other.m_aim),
      m_pu(other.m_pu),
      m_attr(other.m_attr),
      m_ttl(other.m_ttl),
      m_origin(other.m_origin) {
  // You can't move ISprite, but its state is already constructed
  // If ISprite has internal state, you may need a reset/init method
  other.m_x = 0;
  other.m_y = 0;
  other.m_type = 0;
  other.m_aim = AIM_UP;
  other.m_pu = 0;
  other.m_ttl = CActor::NoTTL;
  other.m_attr = 0;
  other.m_origin = Pos{0, 0};
}

CActor &CActor::operator=(CActor &&other) noexcept {
  if (this != &other) {
    // No ISprite::operator= — skip base move
    m_x = other.m_x;
    m_y = other.m_y;
    m_type = other.m_type;
    m_aim = other.m_aim;
    m_pu = other.m_pu;
    m_ttl = other.m_ttl;
    m_attr = other.m_attr;
    m_origin = other.m_origin;

    other.m_x = 0;
    other.m_y = 0;
    other.m_type = 0;
    other.m_aim = AIM_UP;
    other.m_pu = 0;
    other.m_ttl = CActor::NoTTL;
    other.m_attr = 0;
    other.m_origin = Pos{0, 0};
  }
  return *this;
}

CActor::~CActor() {}

/**
 * @brief Can Sprite Move in given direction
 *
 * @param aim
 * @return true
 * @return false
 */

bool CActor::canMove(const JoyAim aim) const { return false; }

/**
 * @brief Move Sprite in given Direction
 *
 * @param aim
 */

void CActor::move(const JoyAim aim) {}

/**
 * @brief Get TileID under sprite position
 *
 * @return uint8_t
 */

uint8_t CActor::getPU() const { return m_pu; }

/**
 * @brief Set TileID under sprite position
 *
 * @param c
 */

void CActor::setPU(const uint8_t c) { m_pu = c; }

/**
 * @brief Set Sprite Position
 *
 * @param pos
 */

void CActor::setPos(const Pos &pos) {
  m_x = pos.x;
  m_y = pos.y;
}

/**
 * @brief Find Next Director (monsters)
 *
 * @param reverse, flip search order
 * @return JoyAim
 */
JoyAim CActor::findNextDir(const bool reverse) const {
  const int aim = m_aim;
  int i = TOTAL_AIMS - 1;
  while (i >= 0) {
    const bool isRevered = (i & 1) && reverse;
    JoyAim newAim = g_aims[aim * TOTAL_AIMS + i];
    if (isRevered) {
      newAim = ::reverseDir(newAim);
    }
    if (canMove(newAim)) {
      return newAim;
    }
    --i;
  }
  return AIM_NONE;
}

/**
 * @brief Get Sprite Current Direction
 *
 * @return JoyAim
 */
JoyAim CActor::getAim() const { return m_aim; }

/**
 * @brief Set Sprite Current Direction
 *
 * @param aim
 */
void CActor::setAim(const JoyAim aim) { m_aim = aim; }

/**
 * @brief Is Player present at given relative postion
 *
 * @param aim
 * @return true
 * @return false
 */

bool CActor::isPlayerThere(JoyAim aim) const {
  const uint8_t c = tileAt(aim);
  const TileDef &def = getTileDef(c);
  return def.type == TYPE_PLAYER;
}

/**
 * @brief Get tileID at give relative position
 *
 * @param aim
 * @return uint8_t
 */
uint8_t CActor::tileAt(JoyAim aim) const { return 0; }  // stub: no tile lookup

/**
 * @brief Change Sprite Type
 *
 * @param type
 */
void CActor::setType(const uint8_t type) { m_type = type; }

/**
 * @brief Check if Sprite Within this range of Map Coordonates
 *
 * @param x1
 * @param y1
 * @param x2
 * @param y2
 * @return true
 * @return false
 */
bool CActor::isWithin(const int x1, const int y1, const int x2,
                      const int y2) const {
  return (m_x >= x1) && (m_x < x2) && (m_y >= y1) && (m_y < y2);
}

/**
 * @brief Reverse Sprite Direction
 *
 */
void CActor::reverveDir() { m_aim = ::reverseDir(m_aim); }

/**
 * @brief Reverse Aim
 *
 * @param aim
 * @param i     bitmask
 * @return JoyAim
 */
JoyAim operator^=(JoyAim &aim, int i) {
  return static_cast<JoyAim>(static_cast<int>(aim) ^ i);
}

/**
 * @brief  Return Sprite Positon
 *
 * @return const Pos
 */
const Pos CActor::pos() const {
  return Pos{static_cast<int16_t>(m_x), static_cast<int16_t>(m_y)};
}

/**
 * @brief Calculate the distance between two actors
 *
 * @param actor
 * @return int
 */
int CActor::distance(const CActor &actor) const {
  int dx = std::abs(actor.m_x - m_x);
  int dy = std::abs(actor.m_y - m_y);
  return std::sqrt(dx * dx + dy * dy);
}

/**
 * @brief Move Sprite to a specfic coordonates
 *
 * @param x
 * @param y
 */
void CActor::move(const int16_t x, const int16_t y) {
  m_x = static_cast<uint8_t>(x & 0xff);
  m_y = static_cast<uint8_t>(y & 0xff);
}

void CActor::move(const Pos pos) { move(pos.x, pos.y); }
