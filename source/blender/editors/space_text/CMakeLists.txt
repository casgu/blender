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
  ../../blenfont
  ../../blenkernel
  ../../blenlib
  ../../blentranslation
  ../../gpu
  ../../makesdna
  ../../makesrna
  ../../windowmanager
  ../../../../intern/glew-mx
  ../../../../intern/guardedalloc
)


set(SRC
  space_text.c
  text_autocomplete.c
  text_draw.c
  text_format.c
  text_format_lua.c
  text_format_osl.c
  text_format_pov.c
  text_format_pov_ini.c
  text_format_py.c
  text_header.c
  text_ops.c
  text_undo.c

  text_format.h
  text_intern.h
)

set(LIB
  bf_blenkernel
  bf_blenlib
)


if(WITH_PYTHON)
  list(APPEND INC
    ../../python
  )
  add_definitions(-DWITH_PYTHON)
endif()


blender_add_lib(bf_editor_space_text "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
