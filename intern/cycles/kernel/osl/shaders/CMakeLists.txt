# Copyright 2011-2020 Blender Foundation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# OSL node shaders

set(SRC_OSL
  node_add_closure.osl
  node_ambient_occlusion.osl
  node_anisotropic_bsdf.osl
  node_attribute.osl
  node_background.osl
  node_bevel.osl
  node_brick_texture.osl
  node_brightness.osl
  node_bump.osl
  node_camera.osl
  node_checker_texture.osl
  node_clamp.osl
  node_combine_rgb.osl
  node_combine_hsv.osl
  node_combine_xyz.osl
  node_convert_from_color.osl
  node_convert_from_float.osl
  node_convert_from_int.osl
  node_convert_from_normal.osl
  node_convert_from_point.osl
  node_convert_from_vector.osl
  node_diffuse_bsdf.osl
  node_displacement.osl
  node_vector_displacement.osl
  node_emission.osl
  node_environment_texture.osl
  node_float_curve.osl
  node_fresnel.osl
  node_gamma.osl
  node_geometry.osl
  node_glass_bsdf.osl
  node_glossy_bsdf.osl
  node_gradient_texture.osl
  node_hair_info.osl
  node_point_info.osl
  node_scatter_volume.osl
  node_absorption_volume.osl
  node_principled_volume.osl
  node_holdout.osl
  node_hsv.osl
  node_ies_light.osl
  node_image_texture.osl
  node_invert.osl
  node_layer_weight.osl
  node_light_falloff.osl
  node_light_path.osl
  node_magic_texture.osl
  node_map_range.osl
  node_mapping.osl
  node_math.osl
  node_mix.osl
  node_mix_closure.osl
  node_musgrave_texture.osl
  node_noise_texture.osl
  node_normal.osl
  node_normal_map.osl
  node_object_info.osl
  node_output_displacement.osl
  node_output_surface.osl
  node_output_volume.osl
  node_particle_info.osl
  node_refraction_bsdf.osl
  node_rgb_curves.osl
  node_rgb_ramp.osl
  node_separate_rgb.osl
  node_separate_hsv.osl
  node_separate_xyz.osl
  node_set_normal.osl
  node_sky_texture.osl
  node_subsurface_scattering.osl
  node_tangent.osl
  node_texture_coordinate.osl
  node_toon_bsdf.osl
  node_translucent_bsdf.osl
  node_transparent_bsdf.osl
  node_value.osl
  node_vector_curves.osl
  node_vector_math.osl
  node_vector_map_range.osl
  node_vector_rotate.osl
  node_vector_transform.osl
  node_velvet_bsdf.osl
  node_vertex_color.osl
  node_voronoi_texture.osl
  node_voxel_texture.osl
  node_wavelength.osl
  node_blackbody.osl
  node_wave_texture.osl
  node_white_noise_texture.osl
  node_wireframe.osl
  node_hair_bsdf.osl
  node_principled_hair_bsdf.osl
  node_uv_map.osl
  node_principled_bsdf.osl
  node_rgb_to_bw.osl
)

# The headers that OSL ships differs per release so we can not
# hardcode this.
file(GLOB SRC_OSL_HEADER_DIST ${OSL_SHADER_DIR}/*.h)

set(SRC_OSL_HEADERS
  node_color.h
  node_fresnel.h
  node_hash.h
  node_math.h
  node_noise.h
  node_ramp_util.h
  stdcycles.h
  ${SRC_OSL_HEADER_DIST}
)

set(SRC_OSO

)

# TODO, add a module to compile OSL
foreach(_file ${SRC_OSL})
  set(_OSL_FILE ${CMAKE_CURRENT_SOURCE_DIR}/${_file})
  set_source_files_properties(${_file} PROPERTIES HEADER_FILE_ONLY TRUE)
  string(REPLACE ".osl" ".oso" _OSO_FILE ${_OSL_FILE})
  string(REPLACE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR} _OSO_FILE ${_OSO_FILE})
  add_custom_command(
    OUTPUT ${_OSO_FILE}
    COMMAND ${OSL_COMPILER} -q -O2  -I"${CMAKE_CURRENT_SOURCE_DIR}" -I"${OSL_SHADER_DIR}" -o ${_OSO_FILE} ${_OSL_FILE}
    DEPENDS ${_OSL_FILE} ${SRC_OSL_HEADERS} ${OSL_COMPILER})
  list(APPEND SRC_OSO
    ${_OSO_FILE}
  )

  unset(_OSL_FILE)
  unset(_OSO_FILE)
endforeach()

add_custom_target(cycles_osl_shaders ALL DEPENDS ${SRC_OSO} ${SRC_OSL_HEADERS} ${OSL_COMPILER} SOURCES ${SRC_OSL})
cycles_set_solution_folder(cycles_osl_shaders)

# CMAKE_CURRENT_SOURCE_DIR is already included in OSO paths
delayed_install("" "${SRC_OSO}" ${CYCLES_INSTALL_PATH}/shader)
delayed_install("${CMAKE_CURRENT_SOURCE_DIR}" "${SRC_OSL_HEADERS}" ${CYCLES_INSTALL_PATH}/shader)
