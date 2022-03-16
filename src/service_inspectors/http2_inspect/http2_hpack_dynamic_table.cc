//--------------------------------------------------------------------------
// Copyright (C) 2019-2021 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// http2_hpack_dynamic_table.cc author Katura Harvey <katharve@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "http2_hpack_dynamic_table.h"

#include <cstring>

#include "http2_flow_data.h"
#include "http2_hpack_table.h"
#include "http2_module.h"

using namespace Http2Enums;

HpackDynamicTable::HpackDynamicTable(Http2FlowData* flow_data) :
    session_data(flow_data)
{
    session_data->update_allocations( ARRAY_CAPACITY * sizeof(HpackTableEntry*) +
        TABLE_MEMORY_TRACKING_INCREMENT);
    table_memory_allocated = TABLE_MEMORY_TRACKING_INCREMENT;
}


HpackDynamicTable::~HpackDynamicTable()
{
    for (uint32_t i = 0, indx = start; i < num_entries; i++)
    {
        delete circular_buf[indx];
        indx = (indx + 1) % ARRAY_CAPACITY;
    }
    session_data->update_deallocations( ARRAY_CAPACITY * sizeof(HpackTableEntry*) +
        TABLE_MEMORY_TRACKING_INCREMENT );

    while (table_memory_allocated > TABLE_MEMORY_TRACKING_INCREMENT)
    {
        session_data->update_deallocations(TABLE_MEMORY_TRACKING_INCREMENT);
        table_memory_allocated -= TABLE_MEMORY_TRACKING_INCREMENT;
    }
}

bool HpackDynamicTable::add_entry(const Field& name, const Field& value)
{
    // The add only fails if the underlying circular array is out of space
    if (num_entries >= ARRAY_CAPACITY)
        return false;

    const uint32_t new_entry_size = name.length() + value.length() + RFC_ENTRY_OVERHEAD;

    // As per the RFC, attempting to add an entry that is larger than the max size of the table is
    // not an error, it causes the table to be cleared
    if (new_entry_size > max_size)
    {
        prune_to_size(0);
        return true;
    }

    // Create new entry. This is done before pruning because the entry referenced by the new name
    // may be pruned.
    HpackTableEntry* new_entry = new HpackTableEntry(name, value);

    // If add entry would exceed max table size, evict old entries
    prune_to_size(max_size - new_entry_size);

    // Add new entry to the front of the table (newest entry = lowest index)
    start = (start + ARRAY_CAPACITY - 1) % ARRAY_CAPACITY;
    circular_buf[start] = new_entry;

    num_entries++;
    if (num_entries > Http2Module::get_peg_counts(PEG_MAX_TABLE_ENTRIES))
        Http2Module::increment_peg_counts(PEG_MAX_TABLE_ENTRIES);

    rfc_table_size += new_entry_size;
    while (rfc_table_size > table_memory_allocated)
    {
        session_data->update_allocations(TABLE_MEMORY_TRACKING_INCREMENT);
        table_memory_allocated += TABLE_MEMORY_TRACKING_INCREMENT;
    }

    return true;
}

const HpackTableEntry* HpackDynamicTable::get_entry(uint32_t virtual_index) const
{
    const uint32_t dyn_index = virtual_index - HpackIndexTable::STATIC_MAX_INDEX - 1;

    if (dyn_index + 1 > num_entries)
        return nullptr;

    const uint32_t arr_index = (start + dyn_index) % ARRAY_CAPACITY;
    return circular_buf[arr_index];
}

/* This is called when adding a new entry and when receiving a dynamic table size update.
 * If adding the new entry would make the table size exceed the max size, entries are pruned
 * until the new entry fits. If the dynamic size update is smaller than the current table size,
 * entries are pruned until the table is no larger than the max size. Entries are pruned least
 * recently added first.
 */
void HpackDynamicTable::prune_to_size(uint32_t new_max_size)
{
    while (rfc_table_size > new_max_size)
    {
        const uint32_t last_index = (start + num_entries - 1 + ARRAY_CAPACITY) % ARRAY_CAPACITY;
        num_entries--;
        rfc_table_size -= circular_buf[last_index]->name.length() +
            circular_buf[last_index]->value.length() + RFC_ENTRY_OVERHEAD;
        delete circular_buf[last_index];
        circular_buf[last_index] = nullptr;
    }
}

void HpackDynamicTable::update_size(uint32_t new_size)
{
    if (new_size < rfc_table_size)
    {
        prune_to_size(new_size);
    }
    max_size = new_size;
}
