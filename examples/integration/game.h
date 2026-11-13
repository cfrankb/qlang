/*
    cs3-runtime-sdl
    Copyright (C) 2025 Francois Blanchette

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
// game.h
/// Stand-in for the engine's CGame.
///
/// The engine's CGame owns the tile animator, the layer tables, the entity
/// lists and the whole update loop (game.cpp is ~2000 lines). CActor never
/// needed any of that: it only ever asked six questions, listed below. The
/// two that walk the layer tile tables are pure virtual, so the example's
/// host decides what is solid (a QLang script can back them too).

#pragma once
#ifndef __GAME_STANDIN_H__
#define __GAME_STANDIN_H__

#include <algorithm>
#include <cstdint>
#include <vector>
#include "joyaim.h"
#include "map.h"
#include "sprtypes.h"

/// What blocks the way at a tile, in the direction of travel.
struct scan_t
{
    bool isWater;
    bool isSolid;
    bool isDeadly;
};

class CGame
{
public:
    virtual ~CGame() = default;

    /// Tile stack at pos, tested as if entering it from aim.
    virtual scan_t scanPos(const Pos &pos, const uint8_t aim) const = 0;
    /// Granular (sub-tile) collision of the sprite leaving pos towards aim.
    virtual bool isGranularSolidFromPos(const Pos &pos, const uint8_t aim) const = 0;

    /// One step in the aim direction, clamped to the map bounds.
    static Pos translate(const Pos &p, const int aim)
    {
        Pos t = p;
        const CMap &map = getMap();
        switch (aim)
        {
        case AIM_UP:
            if (t.y > 0)
                --t.y;
            break;
        case AIM_DOWN:
            if (t.y < map.height() - 1)
                ++t.y;
            break;
        case AIM_LEFT:
            if (t.x > 0)
                --t.x;
            break;
        case AIM_RIGHT:
            if (t.x < map.width() - 1)
                ++t.x;
            break;
        }
        return t;
    }

    /// Pushable by the player.
    static bool isMoveableType(const uint8_t typeID)
    {
        return typeID == TYPE_BOULDER || typeID == TYPE_ICECUBE;
    }

    /// Travels until it hits something, then disappears.
    static bool isBulletType(const uint8_t typeID)
    {
        return typeID == TYPE_FIREBALL || typeID == TYPE_LIGHTNING_BOLT;
    }

    /// A picked-up key opens the door of the same tile id.
    static bool hasKey(const uint8_t c)
    {
        const std::vector<uint8_t> &k = keys();
        return std::find(k.begin(), k.end(), c) != k.end();
    }
    static void addKey(const uint8_t c)
    {
        keys().push_back(c);
    }

    /// The level the actors live in. Installed once by the host.
    static CMap &getMap()
    {
        return *currentMap();
    }
    static CGame *getGame()
    {
        return currentGame();
    }
    static void setup(CMap &map, CGame &game)
    {
        currentMap() = &map;
        currentGame() = &game;
    }
    static void clear()
    {
        currentMap() = nullptr;
        currentGame() = nullptr;
        keys().clear();
    }

private:
    // ponytail: one level at a time, like the engine's singleton. Inject the
    // world into CActor if this example ever needs two live maps.
    static CMap *&currentMap()
    {
        static CMap *map = nullptr;
        return map;
    }
    static CGame *&currentGame()
    {
        static CGame *game = nullptr;
        return game;
    }
    static std::vector<uint8_t> &keys()
    {
        static std::vector<uint8_t> keys;
        return keys;
    }
};

#endif  // __GAME_STANDIN_H__
