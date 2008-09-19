/** -*- c++ -*-
 * Copyright (C) 2008 Doug Judd (Zvents, Inc.)
 *
 * This file is part of Hypertable.
 *
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License, or any later version.
 *
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#ifndef HYPERTABLE_CELL_H
#define HYPERTABLE_CELL_H

#include <iosfwd>

namespace Hypertable {

  /** Encapsulates decomposed key and value */
  class Cell {
  public:
    Cell() : row_key(0), column_family(0), column_qualifier(0), timestamp(0),
             value(0), value_len(0), flag(0) { }

    const char *row_key;
    const char *column_family;
    const char *column_qualifier;
    uint64_t timestamp;
    const uint8_t *value;
    uint32_t value_len;
    uint8_t flag;
  };

  std::ostream &operator<<(std::ostream &, const Cell &);

} // namespace Hypertable

#endif // HYPERTABLE_CELL_H
