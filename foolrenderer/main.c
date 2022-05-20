// Copyright (c) Caden Ji. All rights reserved.
//
// Licensed under the MIT License. See LICENSE file in the project root for
// license information.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <tgafunc.h>

#include "framebuffer.h"
#include "graphics.h"
#include "math/math_utility.h"
#include "math/matrix.h"
#include "math/vector.h"
#include "mesh.h"
#include "shaders/basic.h"
#include "shaders/shadow_casting.h"
#include "texture.h"

#define SHADOW_MAP_WIDTH 1024
#define SHADOW_MAP_HEIGHT 1024
#define IMAGE_WIDTH 512
#define IMAGE_HEIGHT 512

// The direction of the light in world space.
const vector3 LIGHT_DIRECTION = {{1.0f, 1.0f, 0.5f}};

static void endian_inversion(uint8_t *bytes, size_t size) {
    uint8_t buffer;
    size_t left = 0, right = size - 1;
    while (left < right) {
        buffer = bytes[left];
        bytes[left] = bytes[right];
        bytes[right] = buffer;
        ++left;
        --right;
    }
}

static struct texture *load_texture(const char *filename) {
    if (filename == NULL || strlen(filename) == 0) {
        return NULL;
    }

    uint8_t *image_data;
    tga_info *image_info;
    enum tga_error error_code;
    error_code = tga_load(&image_data, &image_info, filename);
    if (error_code != TGA_NO_ERROR) {
        return NULL;
    }
    tga_image_flip_v(image_data, image_info);

    struct texture *texture = NULL;
    if (tga_get_pixel_format(image_info) == TGA_PIXEL_RGB24) {
        uint32_t width = tga_get_image_width(image_info);
        uint32_t height = tga_get_image_height(image_info);
        // Convert all pixels to big endian.
        uint8_t *pixel;
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                pixel = tga_get_pixel(image_data, image_info, x, y);
                endian_inversion(pixel, 3);
            }
        }
        texture = create_texture(TEXTURE_FORMAT_RGBA8, width, height);
        if (texture != NULL) {
            set_texture_pixels(texture, image_data);
        }
    }

    tga_free_data(image_data);
    tga_free_info(image_info);
    return texture;
}

static void save_color_texture(struct texture *texture) {
    uint32_t texture_width = get_texture_width(texture);
    uint32_t texture_height = get_texture_height(texture);
    const uint8_t *texture_data = get_texture_pixels(texture);

    uint8_t *image_data;
    tga_info *image_info;
    tga_create(&image_data, &image_info, texture_width, texture_height,
               TGA_PIXEL_RGB24);

    // Copy the color buffer data to the TGA image.
    for (uint32_t y = 0; y < texture_width; y++) {
        for (uint32_t x = 0; x < texture_height; x++) {
            const uint8_t *texture_pixel =
                texture_data + (y * texture_width + x) * 4;
            uint8_t *image_pixel = tga_get_pixel(image_data, image_info, x, y);
            image_pixel[0] = texture_pixel[0];
            image_pixel[1] = texture_pixel[1];
            image_pixel[2] = texture_pixel[2];
            // Convert all pixels to little endian.
            endian_inversion(image_pixel, 3);
        }
    }
    // This program uses OpenGL style coordinate system, the origin of screen
    // space is in the bottom-left corner. But the tgafunc default image origin
    // is in the upper-left corner, so need to flip the image in the Y-axis
    // direction.
    tga_image_flip_v(image_data, image_info);
    tga_save_from_info(image_data, image_info, "output.tga");

    tga_free_data(image_data);
    tga_free_info(image_info);
}

static void draw_model(struct mesh *mesh, struct texture *diffuse_map,
                       struct texture *normal_map) {
    struct framebuffer *shadow_framebuffer = create_framebuffer();
    struct texture *shadow_map = create_texture(
        TEXTURE_FORMAT_DEPTH_FLOAT, SHADOW_MAP_WIDTH, SHADOW_MAP_HEIGHT);
    attach_texture_to_framebuffer(shadow_framebuffer, DEPTH_ATTACHMENT,
                                  shadow_map);

    struct framebuffer *framebuffer = create_framebuffer();
    struct texture *color_texture =
        create_texture(TEXTURE_FORMAT_SRGB8_A8, IMAGE_WIDTH, IMAGE_HEIGHT);
    struct texture *depth_texture =
        create_texture(TEXTURE_FORMAT_DEPTH_FLOAT, IMAGE_WIDTH, IMAGE_HEIGHT);
    attach_texture_to_framebuffer(framebuffer, COLOR_ATTACHMENT, color_texture);
    attach_texture_to_framebuffer(framebuffer, DEPTH_ATTACHMENT, depth_texture);

    // The first pass, create the shadow map.
    set_viewport(0, 0, SHADOW_MAP_WIDTH, SHADOW_MAP_HEIGHT);
    set_vertex_shader(shadow_casting_vertex_shader);
    set_fragment_shader(shadow_casting_fragment_shader);
    clear_framebuffer(shadow_framebuffer);

    struct shadow_casting_uniform shadow_uniform;
    matrix4x4 light_world2view = matrix4x4_look_at(
        LIGHT_DIRECTION, VECTOR3_ZERO, (vector3){{0.0f, 1.0f, 0.0f}});
    matrix4x4 light_view2clip = matrix4x4_orthographic(1.5f, 1.5f, 0.1f, 2.5f);
    matrix4x4 light_world2clip =
        matrix4x4_multiply(light_view2clip, light_world2view);
    // No rotation, scaling, or translation of the model, so the local2clip
    // matrix is the world2clip matrix.
    shadow_uniform.local2clip = light_world2clip;

    uint32_t triangle_count = mesh->triangle_count;
    for (size_t t = 0; t < triangle_count; t++) {
        struct shadow_casting_vertex_attribute attributes[3];
        const void *attribute_ptrs[3];
        for (uint32_t v = 0; v < 3; v++) {
            get_mesh_position(&attributes[v].position, mesh, t, v);
            attribute_ptrs[v] = attributes + v;
        }
        draw_triangle(shadow_framebuffer, &shadow_uniform, attribute_ptrs);
    }

    // The second pass, draw the model.
    set_viewport(0, 0, IMAGE_WIDTH, IMAGE_HEIGHT);
    set_vertex_shader(basic_vertex_shader);
    set_fragment_shader(basic_fragment_shader);
    clear_framebuffer(framebuffer);

    struct basic_uniform uniform;
    matrix4x4 world2view = matrix4x4_look_at((vector3){{0.0f, 0.0f, 2.5f}},
                                             (vector3){{0.0f, 0.0f, 0.0f}},
                                             (vector3){{0.0f, 1.0f, 0.0f}});
    // No rotation, scaling, or translation of the model, so the local2view
    // matrix is the world2view matrix.
    uniform.local2view = world2view;
    uniform.view2clip = matrix4x4_perspective(
        PI / 4.0f, IMAGE_WIDTH / IMAGE_HEIGHT, 0.1f, 5.0f);
    uniform.loacl2view_direction = matrix4x4_to_3x3(uniform.local2view);
    // There is no non-uniform scaling so the normal transformation matrix is
    // the direction transformation matrix.
    uniform.local2view_normal = uniform.loacl2view_direction;
    uniform.light_direction = vector3_normalize(matrix3x3_multiply_vector3(
        matrix4x4_to_3x3(world2view), LIGHT_DIRECTION));
    uniform.light_color = VECTOR3_ONE;
    uniform.ambient_color = (vector3){{0.01f, 0.01f, 0.01f}};
    uniform.ambient_reflectance = VECTOR3_ONE;
    uniform.diffuse_reflectance = VECTOR3_ONE;
    uniform.specular_reflectance = VECTOR3_ONE;
    uniform.shininess = 100.0f;
    uniform.diffuse_map = diffuse_map;
    uniform.normal_map = normal_map;
    // Remap each component of position from [-1, 1] to [0, 1].
    matrix4x4 remap_matrix = {{{0.5f, 0.0f, 0.0f, 0.5f},
                               {0.0f, 0.5f, 0.0f, 0.5f},
                               {0.0f, 0.0f, 0.5f, 0.5f},
                               {0.0f, 0.0f, 0.0f, 1.0f}}};
    uniform.local2light = matrix4x4_multiply(remap_matrix, light_world2clip);
    uniform.shadow_map = shadow_map;

    for (size_t t = 0; t < triangle_count; t++) {
        struct basic_vertex_attribute attributes[3];
        const void *attribute_ptrs[3];
        for (uint32_t v = 0; v < 3; v++) {
            get_mesh_position(&attributes[v].position, mesh, t, v);
            get_mesh_normal(&attributes[v].normal, mesh, t, v);
            get_mesh_tangent(&attributes[v].tangent, mesh, t, v);
            get_mesh_texcoord(&attributes[v].texcoord, mesh, t, v);
            attribute_ptrs[v] = attributes + v;
        }
        draw_triangle(framebuffer, &uniform, attribute_ptrs);
    }

    save_color_texture(color_texture);

    // Release framebuffer.
    destroy_texture(shadow_map);
    destroy_framebuffer(shadow_framebuffer);
    destroy_texture(color_texture);
    destroy_texture(depth_texture);
    destroy_framebuffer(framebuffer);
}

int main(void) {
    const char *model_path = "assets/suzanne/suzanne.obj";
    const char *normal_map_path = "assets/suzanne/normal.tga";

    // Load model data.
    struct mesh *mesh;
    mesh = load_mesh(model_path);
    if (mesh == NULL) {
        printf("Cannot load .obj file.\n");
        return 0;
    }
    // Create a default diffuse map.
    struct texture *diffuse_map = create_texture(TEXTURE_FORMAT_RGBA8, 1, 1);
    if (diffuse_map != NULL) {
        uint8_t pixel_data[3] = {255, 255, 255};
        set_texture_pixels(diffuse_map, pixel_data);
    }
    struct texture *normal_map = load_texture(normal_map_path);
    if (diffuse_map == NULL || normal_map == NULL) {
        printf("Cannot load texture files.\n");
        return 0;
    }

    draw_model(mesh, diffuse_map, normal_map);
    destroy_mesh(mesh);
    destroy_texture(normal_map);
    return 0;
}
