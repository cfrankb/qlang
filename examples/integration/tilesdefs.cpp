/*
    cs3-runtime-sdl
    Copyright (C) 2026 Francois Blanchette

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
// The definition tilesdefs.h declares lives in the generator's output, which
// this demo did not vendor. One zero entry is enough for the only reach into
// the table: CActor::isPlayerThere, through CActor::tileAt — a stub that
// resolves every position to tile 0.
// ponytail: replace with the generated tilesdefs.cpp when it is vendored.

#include "tilesdefs.h"

const TileDef tileDefs[1] = {};
