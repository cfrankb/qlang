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
#pragma once

#include <stdexcept>
#include <vector>
#include <cstdint>
#include <string>
#include "dirs.h"
#include "logger.h"
#include "qlang/QLangMeta.h"

// QLang external type (docs/briefs/add-qlmeta-to-cpp.md). QLObject<CLayer> must
// stay the FIRST base: the typed handoff validates a script pointer with
// dynamic_cast through that subobject, and a non-zero offset makes every access
// fail with "external type mismatch".
class CLayer : public qlang::QLObject<CLayer>
{

public:
    enum LayerType : uint8_t
    {
        LAYER_MAIN,
        LAYER_FLOOR,
        LAYER_WALLS,
        LAYER_DECOR,
    };

    CLayer(const uint16_t len, const uint16_t hei, const LayerType layerType = LAYER_MAIN, uint16_t baseID = 0, const char *name = "untitled")
    {
        m_len = len;
        m_hei = hei;
        m_tiles.resize(len * hei);
        m_layerType = layerType;
        m_name = name;
        m_baseID = baseID;
    };
    ~CLayer() = default;

    inline bool isValid(const int x, const int y) const
    {
        return x >= 0 && x < m_len && y >= 0 && y < m_hei;
    }

    bool resize(uint16_t in_len, uint16_t in_hei, uint8_t t, bool fast);
    bool shift(Direction aim);
    inline void clear() { m_tiles.clear(); }
    void fill(uint16_t ch = 0);
    size_t size() const { return m_tiles.size(); }
    void replaceTile(const uint16_t, const uint16_t);
    inline int width() const { return m_len; };
    inline int height() const { return m_hei; };
    const char *lastError() { return m_lastError.c_str(); }

    inline uint16_t &get(const int x, const int y)
    {
        if (!isValid(x, y))
        {
            LOGE("invalid coordonates [get] (%d, %d) -- upper bound(%d,%d)", x, y, m_len, m_hei);
            throw std::out_of_range("Invalid map access");
        }
        return m_tiles[x + y * m_len];
    }

    inline uint16_t at(const int x, const int y) const
    {
        if (!isValid(x, y))
        {
            LOGE("invalid coordonates [at] (%d, %d) -- upper bound(%d,%d)", x, y, m_len, m_hei);
            throw std::out_of_range("Invalid map access");
        }
        return m_tiles[x + y * m_len];
    }

    inline void set(const int x, const int y, const uint16_t t)
    {
        get(x, y) = t;
    }

    void setName(const std::string_view &name)
    {
        m_name = name;
    }

    const char *getName()
    {
        return m_name.c_str();
    }

    LayerType layerType() { return m_layerType; }
    inline uint16_t baseID() const { return m_baseID; }
    void setBaseID(int baseID) { m_baseID = baseID; };

    std::vector<uint16_t> &tiles() { return m_tiles; };
    const std::vector<uint16_t> &tilesConst() const { return m_tiles; };
    void tilesFrom(const std::vector<uint16_t> &tiles) { m_tiles = tiles; };

    /// Script-visible rendering (QL_STR): str(layer), print(layer), f"{layer}".
    std::string str() const
    {
        return "Layer(" + m_name + " " + std::to_string(m_len) + "x" +
               std::to_string(m_hei) + ")";
    }

protected:
    enum : uint16_t
    {
        MAX_SIZE = 256,
    };

private:
    std::string m_name;
    std::string m_lastError;
    std::vector<uint16_t> m_tiles;
    uint16_t m_len;
    uint16_t m_hei;
    uint16_t m_baseID;
    LayerType m_layerType;

    friend class CMap;

    // QLang bindings, ordered properties -> methods -> enums -> QL_STR ->
    // QL_CTOR. Only members the boundary conversion can carry are bound
    // (docs/EXTERNAL_TYPES.md §7): `fill`, `resize`, `set`, `replaceTile` and
    // `shift` take uint16_t/uint8_t/Direction arguments, which
    // QValue::as<T>() has no specialization for, and `tiles()` returns a
    // std::vector reference, which qval_of() has no overload for.
    QL_OBJECT(
        QL_PROPERTY(width),
        QL_PROPERTY(height),
        QL_PROPERTY(size),
        QL_PROPERTY(baseID, READ(baseID), WRITE(setBaseID)),
        QL_INVOKABLE(isValid),
        QL_INVOKABLE(at),
        QL_INVOKABLE(clear),
        QL_INVOKABLE(getName),
        QL_INVOKABLE(layerType),
        QL_INVOKABLE(lastError),
        QL_ENUM(LayerType, LAYER_MAIN, LAYER_FLOOR, LAYER_WALLS, LAYER_DECOR),
        QL_STR(str),
        QL_CTOR([](int len, int hei, std::string name) {
          return std::make_shared<CLayer>(static_cast<uint16_t>(len),
                                          static_cast<uint16_t>(hei),
                                          CLayer::LAYER_MAIN, 0, name.c_str());
        }));
};
