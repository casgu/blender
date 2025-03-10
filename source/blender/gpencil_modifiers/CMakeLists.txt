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
#
# The Original Code is Copyright (C) 2018, Blender Foundation
# All rights reserved.
#
# ***** END GPL LICENSE BLOCK *****

set(INC
  .
  intern
  ../blenfont
  ../blenkernel
  ../blenlib
  ../blentranslation
  ../bmesh
  ../depsgraph
  ../editors/include
  ../makesdna
  ../makesrna
  ../render
  ../windowmanager
  ../../../intern/eigen
  ../../../intern/guardedalloc

  # dna_type_offsets.h in BLO_read_write.h
  ${CMAKE_BINARY_DIR}/source/blender/makesdna/intern
)

set(INC_SYS
  ${ZLIB_INCLUDE_DIRS}
)

set(SRC
  intern/MOD_gpencil_ui_common.c

  intern/MOD_gpencil_util.c
  intern/MOD_gpencilarmature.c
  intern/MOD_gpencilarray.c
  intern/MOD_gpencilbuild.c
  intern/MOD_gpencilcolor.c
  intern/MOD_gpencildash.c
  intern/MOD_gpencilhook.c
  intern/MOD_gpencillattice.c
  intern/MOD_gpencillength.c
  intern/MOD_gpencillineart.c
  intern/MOD_gpencilmirror.c
  intern/MOD_gpencilmultiply.c
  intern/MOD_gpencilnoise.c
  intern/MOD_gpenciloffset.c
  intern/MOD_gpencilopacity.c
  intern/MOD_gpencilshrinkwrap.c
  intern/MOD_gpencilsimplify.c
  intern/MOD_gpencilsmooth.c
  intern/MOD_gpencilsubdiv.c
  intern/MOD_gpenciltexture.c
  intern/MOD_gpencilthick.c
  intern/MOD_gpenciltime.c
  intern/MOD_gpenciltint.c
  intern/MOD_gpencilweight_angle.c
  intern/MOD_gpencilweight_proximity.c

  MOD_gpencil_lineart.h
  MOD_gpencil_modifiertypes.h
  intern/MOD_gpencil_ui_common.h
  intern/MOD_gpencil_util.h

  # Lineart code
  intern/lineart/lineart_chain.c
  intern/lineart/lineart_cpu.c
  intern/lineart/lineart_ops.c
  intern/lineart/lineart_util.c

  intern/lineart/MOD_lineart.h
  intern/lineart/lineart_intern.h

)

set(LIB
)

add_definitions(${GL_DEFINITIONS})

blender_add_lib(bf_gpencil_modifiers "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

add_dependencies(bf_gpencil_modifiers bf_dna)
