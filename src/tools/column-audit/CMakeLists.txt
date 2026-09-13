# SPDX-License-Identifier: GPL-3.0-or-later
#
# MaNGOS is a full featured server for World of Warcraft, supporting
# the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
#
# Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# =============================================================================
# mangos-column-audit -- measures what ColumnAt hides and where the liquid
# selection disagrees with itself, against the world database's own spawns.
# Links `terrain` and nothing else: no database, no game, no client.
# =============================================================================

add_executable(mangos-column-audit ColumnAudit.cpp)

target_link_libraries(mangos-column-audit PRIVATE terrain)

set_target_properties(mangos-column-audit PROPERTIES FOLDER "tools")

install(TARGETS mangos-column-audit DESTINATION ${BIN_DIR}/tools)
