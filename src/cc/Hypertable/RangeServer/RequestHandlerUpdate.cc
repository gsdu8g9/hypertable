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

#include "Common/Compat.h"
#include "Common/Error.h"
#include "Common/Logger.h"

#include "AsyncComm/ResponseCallback.h"
#include "Common/Serialization.h"

#include "Hypertable/Lib/Types.h"

#include "RangeServer.h"
#include "RequestHandlerUpdate.h"

using namespace Hypertable;

/**
 *
 */
void RequestHandlerUpdate::run() {
  ResponseCallbackUpdate cb(m_comm, m_event_ptr);
  TableIdentifier table;
  const uint8_t *decode_ptr = m_event_ptr->payload;
  size_t decode_remain = m_event_ptr->payload_len;
  StaticBuffer mods;

  try {
    table.decode(&decode_ptr, &decode_remain);
    uint32_t count = Serialization::decode_i32(&decode_ptr, &decode_remain);
    uint32_t flags = Serialization::decode_i32(&decode_ptr, &decode_remain);

    mods.base = (uint8_t *)decode_ptr;
    mods.size = decode_remain;
    mods.own = false;

    m_range_server->update(&cb, &table, count, mods, flags);
  }
  catch (Exception &e) {
    HT_ERROR_OUT << e << HT_END;
    cb.error(Error::PROTOCOL_ERROR, "Error handling Update message");
  }
}
