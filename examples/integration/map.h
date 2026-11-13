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

#include <unordered_map>
#include <string>
#include <memory> // For unique_ptr
#include "dirs.h"
#include "layer.h"
#include "qlang/QLangMeta.h"

#ifndef ___POS_T
#define ___POS_T

typedef std::unordered_map<uint16_t, uint8_t> attrMap_t;
struct Pos
{
    int16_t x;
    int16_t y;
    bool operator==(const Pos &other) const
    {
        return (x == other.x) && (y == other.y);
    }
    bool operator!=(const Pos &other) const
    {
        return x != other.x || y != other.y;
    }
};
#endif

class CStates;
class CLayer;

// QLang external type (docs/briefs/add-qlmeta-to-cpp.md). QLObject<CMap> has to
// stay the first base: the typed handoff validates a script-side pointer with
// dynamic_cast through that subobject, so a non-zero offset would make every
// access fail with "external type mismatch".
class CMap : public qlang::QLObject<CMap>
{
public:
    CMap(uint16_t len = 0, uint16_t hei = 0, uint16_t t = 0);
    CMap(const CMap &map);
    ~CMap();
    void clear();
    inline int width() const { return m_len; };
    inline int height() const { return m_hei; };
    bool resize(uint16_t in_len, uint16_t in_hei, uint16_t t, bool fast);
    const Pos findFirst(const uint16_t tileId) const;
    size_t count(const uint16_t tileId) const;
    void fill(uint16_t ch = 0);
    inline uint8_t getAttr(const uint8_t x, const uint8_t y) const
    {
        const uint16_t key = toKey(x, y);
        const auto &it = m_attrs.find(key);
        if (it != m_attrs.end())
        {
            return it->second;
        }
        else
        {
            return 0;
        }
    }
    void setAttr(const uint8_t x, const uint8_t y, const uint8_t a);
    size_t size() const;
    const char *lastError();
    CMap &operator=(const CMap &map);
    const char *title();
    void setTitle(const std::string_view &title);
    void replaceTile(const uint16_t, const uint16_t);
    const attrMap_t &attrs() { return m_attrs; }
    CStates &states();
    inline const CStates &statesConst() const { return *m_states; };
    static uint16_t toKey(const uint8_t x, const uint8_t y);
    static uint16_t toKey(const Pos &pos);
    static Pos toPos(const uint16_t key);
    inline bool isValid(const int x, const int y) const
    {
        return x >= 0 && x < m_len && y >= 0 && y < m_hei;
    }

    bool shift(const Direction aim);
    Pos shiftPos(const Pos &p, const Direction aim) const;
    void debug();
    inline uint16_t &get(const int x, const int y)
    {
        return getMainLayer()->get(x, y);
    }

    inline uint16_t at(const int x, const int y) const
    {
        return getMainLayer()->at(x, y);
    }

    inline void set(const int x, const int y, const uint16_t t)
    {
        get(x, y) = t;
    }

    inline std::vector<std::unique_ptr<CLayer>> &layers()
    {
        return m_layers;
    }

    inline const std::vector<std::unique_ptr<CLayer>> &layersConst() const
    {
        return m_layers;
    }

    inline CLayer *getMainLayer() const
    {
        return m_layers[0].get();
    }

    inline size_t layerCount() const
    {
        return m_layers.size();
    }

    CLayer *addLayer(const CLayer::LayerType lt, const std::string_view &name, uint16_t baseID);
    inline CLayer *getLayer(const size_t index) const
    {
        if (index < m_layers.size())
            return m_layers[index].get();
        else
            return nullptr;
    }
    std::unique_ptr<CLayer> popLayer();
    void pushLayer(std::unique_ptr<CLayer> &layer);

    /// Script-visible rendering (QL_STR): str(map), print(map), f"{map}".
    std::string str() const
    {
        return "Map(" + m_title + " " + std::to_string(m_len) + "x" +
               std::to_string(m_hei) + ")";
    }

    enum : int16_t
    {
        NOT_FOUND = -1,
    };

    enum MapVersion : uint16_t
    {
        VERSION0 = 0,
        VERSION1 = 1,
        VERSION2 = 2,
        VERSION3 = 3,
    };

private:
    void addMainLayer();

    uint16_t m_len;
    uint16_t m_hei;
    attrMap_t m_attrs;
    std::string m_lastError;
    std::string m_title;
    std::unique_ptr<CStates> m_states;
    std::vector<std::unique_ptr<CLayer>> m_layers;

    // QLang bindings, ordered properties -> methods -> enums -> QL_STR ->
    // QL_CTOR. Members the boundary conversion cannot carry are left out
    // (docs/EXTERNAL_TYPES.md §7): `set`/`fill`/`resize`/`count`/`replaceTile`
    // and the attr accessors take uint16_t/uint8_t arguments with no as<T>()
    // specialization, `findFirst`/`shiftPos`/`toPos` and `getAttr` speak Pos,
    // `setTitle` takes a string_view, `states()` and the layer accessors hand
    // out CStates&/CLayer* that qval_of() has no overload for, and `toKey` is
    // static. `title` therefore stays read-only.
    QL_OBJECT(
        QL_PROPERTY(width),
        QL_PROPERTY(height),
        QL_PROPERTY(size),
        QL_PROPERTY(layerCount),
        QL_INVOKABLE(isValid),
        QL_INVOKABLE(at),
        QL_INVOKABLE(clear),
        QL_INVOKABLE(title),
        QL_INVOKABLE(lastError),
        QL_ENUM(Direction, UP, DOWN, LEFT, RIGHT),
        QL_ENUM(MapVersion, VERSION0, VERSION1, VERSION2, VERSION3),
        QL_STR(str),
        QL_CTOR([](int len, int hei) {
          return std::make_shared<CMap>(static_cast<uint16_t>(len),
                                        static_cast<uint16_t>(hei));
        }));
};
