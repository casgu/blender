# ***** BEGIN GPL LICENSE BLOCK *****
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
# ***** END GPL LICENSE BLOCK *****

set(INC
  ../include
  ../../blenkernel
  ../../blenlib
  ../../blentranslation
  ../../bmesh
  ../../depsgraph
  ../../io/alembic
  ../../io/collada
  ../../io/gpencil
  ../../io/usd
  ../../io/wavefront_obj
  ../../makesdna
  ../../makesrna
  ../../windowmanager
  ../../../../intern/guardedalloc
)

set(INC_SYS

)

set(SRC
  io_alembic.c
  io_cache.c
  io_collada.c
  io_gpencil_export.c
  io_gpencil_import.c
  io_gpencil_utils.c
  io_obj.c
  io_ops.c
  io_usd.c

  io_alembic.h
  io_cache.h
  io_collada.h
  io_gpencil.h
  io_obj.h
  io_ops.h
  io_usd.h
)

set(LIB
  bf_blenkernel
  bf_blenlib
  bf_wavefront_obj
)

if(WITH_OPENCOLLADA)
  list(APPEND LIB
    bf_collada
  )
  add_definitions(-DWITH_COLLADA)
endif()

if(WITH_ALEMBIC)
  list(APPEND LIB
    bf_alembic
  )
  add_definitions(-DWITH_ALEMBIC)
endif()

if(WITH_USD)
  list(APPEND LIB
    bf_usd
  )
  add_definitions(-DWITH_USD)
endif()

if(WITH_PUGIXML)
  add_definitions(-DWITH_PUGIXML)
endif()

if(WITH_HARU)
  add_definitions(-DWITH_HARU)
endif()

list(APPEND LIB bf_gpencil)

blender_add_lib(bf_editor_io "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
