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
#include <algorithm>
#include "map.h"
#include "states.h"
#include "statedata.h"
#include "logger.h"
#include <cstdint>

namespace MapPrivate
{
    constexpr uint16_t MAX_SIZE = 256;
}

using namespace MapPrivate;

CMap::CMap(uint16_t len, uint16_t hei, uint16_t t) : m_len(len),
                                                     m_hei(hei), m_states(std::make_unique<CStates>())
{
    addMainLayer();
    resize(len, hei, t, true);
}

CMap::CMap(const CMap &map) : m_len(map.m_len),
                              m_hei(map.m_hei),
                              m_attrs(map.m_attrs),
                              m_title(map.m_title),
                              m_states(std::make_unique<CStates>(*map.m_states))
{
    m_layers.reserve(map.m_layers.size());
    for (const auto &layer : map.m_layers)
    {
        m_layers.emplace_back(new CLayer(*layer));
    }
    if (m_layers.size() == 0)
        addMainLayer();
}

CMap::~CMap()
{
    // clear();
}

void CMap::addMainLayer()
{
    m_layers.emplace_back(std::make_unique<CLayer>(m_len, m_hei, CLayer::LayerType::LAYER_MAIN, 0, "main"));
}

void CMap::clear()
{
    m_states->clear();
    m_layers.clear();
    m_len = 0;
    m_hei = 0;
    m_attrs.clear();
}

const Pos CMap::findFirst(const uint16_t tileId) const
{
    for (int y = 0; y < m_hei; ++y)
    {
        for (int x = 0; x < m_len; ++x)
        {
            if (at(x, y) == tileId)
            {
                return Pos{static_cast<int16_t>(x), static_cast<int16_t>(y)};
            }
        }
    }
    return Pos{NOT_FOUND, NOT_FOUND};
}

size_t CMap::count(const uint16_t tileId) const
{
    size_t count = 0;
    for (uint16_t y = 0; y < m_hei; ++y)
    {
        for (uint16_t x = 0; x < m_len; ++x)
        {
            if (at(x, y) == tileId)
            {
                ++count;
            }
        }
    }
    return count;
}

void CMap::fill(uint16_t ch)
{
    getMainLayer()->fill(ch);
    m_attrs.clear();
}

void CMap::setAttr(const uint8_t x, const uint8_t y, const uint8_t a)
{
    const uint16_t key = toKey(x, y);
    if (a == 0)
    {
        m_attrs.erase(key);
    }
    else
    {
        m_attrs[key] = a;
    }
}

const char *CMap::lastError()
{
    return m_lastError.c_str();
}

size_t CMap::size() const
{
    return getMainLayer()->size();
}

CMap &CMap::operator=(const CMap &map)
{
    clear();
    if (this != &map)
    {
        m_len = map.m_len;
        m_hei = map.m_hei;
        m_attrs = map.m_attrs;
        m_title = map.m_title;
        *m_states = *map.m_states;
        for (const auto &layer : map.m_layers)
        {
            m_layers.emplace_back(new CLayer(*layer.get()));
        }
    }
    return *this;
}

bool CMap::shift(const Direction aim)
{
    if (m_len == 0 || m_hei == 0)
        return false; // No-op for empty map

    for (const auto &layer : m_layers)
        if (!layer->shift(aim))
            return false;

    attrMap_t newAttrs; // New attribute map for shifted positions

    // Update attributes
    switch (aim)
    {
    case Direction::UP:
        for (const auto &[key, attr] : m_attrs)
        {
            Pos pos = shiftPos(toPos(key), aim);
            newAttrs[toKey(pos.x, pos.y)] = attr;
        }
        break;

    case Direction::DOWN:
        for (const auto &[key, attr] : m_attrs)
        {
            Pos pos = shiftPos(toPos(key), aim);
            newAttrs[toKey(pos.x, pos.y)] = attr;
        }
        break;

    case Direction::LEFT:
        for (const auto &[key, attr] : m_attrs)
        {
            Pos pos = shiftPos(toPos(key), aim);
            newAttrs[toKey(pos.x, pos.y)] = attr;
        }
        break;

    case Direction::RIGHT:
        for (const auto &[key, attr] : m_attrs)
        {
            Pos pos = shiftPos(toPos(key), aim);
            newAttrs[toKey(pos.x, pos.y)] = attr;
        }
        break;

    default:
        LOGW("Invalid shift direction: %d", static_cast<int>(aim));
        return false;
    }

    m_attrs = std::move(newAttrs); // Update attributes

    // Shift spawn/exit positions so they keep pointing at the same tile
    // (toroidal wrap), matching how attributes are shifted above.
    if (m_states->hasU(POS_ORIGIN))
    {
        Pos pos = shiftPos(toPos(m_states->getU(POS_ORIGIN)), aim);
        m_states->setU(POS_ORIGIN, toKey(pos));
    }
    if (m_states->hasU(POS_EXIT))
    {
        Pos pos = shiftPos(toPos(m_states->getU(POS_EXIT)), aim);
        m_states->setU(POS_EXIT, toKey(pos));
    }

    return true;
}

Pos CMap::shiftPos(const Pos &p, const Direction aim) const
{
    Pos pos = p;
    switch (aim)
    {
    case Direction::UP:
        pos.y = (pos.y == 0) ? m_hei - 1 : pos.y - 1;
        break;
    case Direction::DOWN:
        pos.y = (pos.y == m_hei - 1) ? 0 : pos.y + 1;
        break;
    case Direction::LEFT:
        pos.x = (pos.x == 0) ? m_len - 1 : pos.x - 1;
        break;
    case Direction::RIGHT:
        pos.x = (pos.x == m_len - 1) ? 0 : pos.x + 1;
        break;
    }
    return pos;
}

uint16_t CMap::toKey(const uint8_t x, const uint8_t y)
{
    return x + (y << 8);
}

uint16_t CMap::toKey(const Pos &pos)
{
    return toKey(pos.x, pos.y);
}

Pos CMap::toPos(const uint16_t key)
{
    return Pos{.x = static_cast<uint8_t>(key & 0xff),
               .y = static_cast<uint8_t>(key >> 8)};
}

void CMap::debug()
{
    LOGI("len: %d hei:%d", m_len, m_hei);
    LOGI("attrCount:%zu", m_attrs.size());
    for (auto &it : m_attrs)
    {
        uint16_t key = it.first;
        uint8_t x = key & 0xff;
        uint8_t y = key >> 8;
        uint8_t a = it.second;
        LOGI("key:%.4x x:%.2x y:%.2x a:%.2x", key, x, y, a);
    }
}

const char *CMap::title()
{
    return m_title.c_str();
}

void CMap::setTitle(const std::string_view &title)
{
    m_title = title;
}

CStates &CMap::states()
{
    return *m_states;
}

bool CMap::resize(uint16_t in_len, uint16_t in_hei, uint16_t t, bool fast)
{
    in_len = std::min(in_len, MAX_SIZE);
    in_hei = std::min(in_hei, MAX_SIZE);

    m_len = in_len;
    m_hei = in_hei;

    for (const auto &layer : m_layers)
        if (!layer->resize(in_len, in_hei, t, fast))
            return false;

    attrMap_t newAttrs;
    for (const auto &[key, attr] : m_attrs)
    {
        Pos pos = toPos(key);
        if (pos.x >= in_len || pos.y >= in_hei)
            continue;
        newAttrs[toKey(pos.x, pos.y)] = attr;
    }
    m_attrs = std::move(newAttrs); // Update attributes
    return true;
}

void CMap::replaceTile(const uint16_t src, const uint16_t repl)
{
    getMainLayer()->replaceTile(src, repl);
}

CLayer *CMap::addLayer(const CLayer::LayerType lt, const std::string_view &name, uint16_t baseID)
{
    return m_layers.emplace_back(new CLayer(m_len, m_hei, lt, baseID, name.data())).get();
}

std::unique_ptr<CLayer> CMap::popLayer()
{
    if (m_layers.empty())
        return nullptr;

    // Move out the last element
    std::unique_ptr<CLayer> layer = std::move(m_layers.back());
    m_layers.pop_back(); // cleaner than resize(size-1)
    return layer;
}

void CMap::pushLayer(std::unique_ptr<CLayer> &layer)
{
    m_layers.emplace_back(std::move(layer));
}
