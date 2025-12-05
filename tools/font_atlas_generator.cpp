#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <freetype/ftglyph.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONT_FILE_NAME "Enchanted Land.otf"
#define PNG_OUTPUT_IMAGE "font_atlas.png"
#define ATLAS_META_FILE "font_atlas.meta"

int main() {
    FT_Library ft;
    if (FT_Init_FreeType(&ft)) {
        fprintf(stderr, "Could not init FreeType library\n");
        return 1;
    }

    FT_Face face;
    if (FT_New_Face(ft, FONT_FILE_NAME, 0, &face)) {
        fprintf(stderr, "Could not open font %s\n", FONT_FILE_NAME);
        return 1;
    }

    printf("Font loaded: %s\n", FONT_FILE_NAME);

    // Atlas settings
    int atlas_dimension_px = 1024;
    int atlas_columns = 16;
    int padding_px = 6;
    int slot_glyph_size = 64;
    int atlas_glyph_px = 64 - padding_px;

    // Allocate and CLEAR atlas buffer
    unsigned char* atlas_buffer = (unsigned char*)calloc(
        atlas_dimension_px * atlas_dimension_px * 4, sizeof(unsigned char)
    );

    // CRITICAL: Initialize arrays to ZERO!
    int grows[256] = {0};
    int gwidth[256] = {0};
    int gpitch[256] = {0};
    int gymin[256] = {0};
    unsigned char* glyph_buffer[256] = {NULL};
    bool glyph_exists[256] = {false};  // Track which glyphs exist

    FT_Set_Pixel_Sizes(face, 0, atlas_glyph_px);

    printf("Rendering glyphs...\n");
    int count = 0;

    // Render glyphs 33-255
    for (int i = 33; i < 256; i++) {
        if (FT_Load_Char(face, i, FT_LOAD_RENDER)) {
            continue;  // Glyph doesn't exist
        }

        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);

        grows[i] = face->glyph->bitmap.rows;
        gwidth[i] = face->glyph->bitmap.width;
        gpitch[i] = face->glyph->bitmap.pitch;

        if (grows[i] == 0 || gwidth[i] == 0) continue;  // Skip empty

        // Copy glyph
        glyph_buffer[i] = (unsigned char*)malloc(grows[i] * gpitch[i]);
        memcpy(glyph_buffer[i], face->glyph->bitmap.buffer, grows[i] * gpitch[i]);
        glyph_exists[i] = true;  // Mark as existing

        // Get y-offset
        FT_Glyph glyph;
        if (FT_Get_Glyph(face->glyph, &glyph) == 0) {
            FT_BBox bbox;
            FT_Glyph_Get_CBox(glyph, FT_GLYPH_BBOX_TRUNCATE, &bbox);
            gymin[i] = bbox.yMin;
            FT_Done_Glyph(glyph);
        }
        count++;
    }

    printf("Rendered %d glyphs\n", count);

    // Write atlas image
    printf("Writing atlas...\n");
    for (int y = 0; y < atlas_dimension_px; y++) {
        for (int x = 0; x < atlas_dimension_px; x++) {
            int col = x / slot_glyph_size;
            int row = y / slot_glyph_size;
            int order = row * atlas_columns + col;
            int glyph_index = order + 32;
            int pixel_index = (y * atlas_dimension_px + x) * 4;

            if (glyph_index > 32 && glyph_index < 256 && glyph_exists[glyph_index]) {
                int x_loc = x % slot_glyph_size - padding_px / 2;
                int y_loc = y % slot_glyph_size - padding_px / 2;

                if (x_loc >= 0 && y_loc >= 0 &&
                    x_loc < gwidth[glyph_index] && y_loc < grows[glyph_index]) {
                    
                    int byte_idx = y_loc * gwidth[glyph_index] + x_loc;
                    unsigned char val = glyph_buffer[glyph_index][byte_idx];
                    
                    atlas_buffer[pixel_index + 0] = 255;
                    atlas_buffer[pixel_index + 1] = 255;
                    atlas_buffer[pixel_index + 2] = 255;
                    atlas_buffer[pixel_index + 3] = val;
                }
            }
        }
    }

    if (!stbi_write_png(PNG_OUTPUT_IMAGE, atlas_dimension_px, atlas_dimension_px,
                        4, atlas_buffer, atlas_dimension_px * 4)) {
        fprintf(stderr, "ERROR: could not write PNG\n");
        return 1;
    }
    printf("✓ Wrote %s\n", PNG_OUTPUT_IMAGE);

    // Free buffers
    for (int i = 0; i < 256; i++) {
        if (glyph_buffer[i]) free(glyph_buffer[i]);
    }
    free(atlas_buffer);

    // Write metadata - ONLY for existing glyphs!
    printf("Writing metadata...\n");
    FILE* fp = fopen(ATLAS_META_FILE, "w");
    fprintf(fp, "// ascii_code prop_xMin prop_width prop_yMin prop_height prop_y_offset\n");
    fprintf(fp, "32 0.0 0.5 0.0 1.0 0.0\n");  // Space

    for (int i = 33; i < 256; i++) {
        if (!glyph_exists[i]) continue;

        int order = i - 32;
        int col = order % atlas_columns;
        int row = order / atlas_columns;
        
        float x_min = (float)(col * slot_glyph_size) / (float)atlas_dimension_px;
        
        // FLIP Y COORDINATE for OpenGL bottom-left origin
        float y_min = 1.0f - (float)((row + 1) * slot_glyph_size) / (float)atlas_dimension_px;  // ← CHANGED!
        
        fprintf(fp, "%i %f %f %f %f %f\n",
            i,
            x_min,
            (float)(gwidth[i] + padding_px) / (float)slot_glyph_size,
            y_min,  // Now bottom-relative
            (float)(grows[i] + padding_px) / (float)slot_glyph_size,
            -((float)padding_px / 2 - (float)gymin[i]) / (float)slot_glyph_size
        );
    }

    fclose(fp);
    printf("✓ Wrote %s\n", ATLAS_META_FILE);

    FT_Done_Face(face);
    FT_Done_FreeType(ft);

    printf("\n✓ Font atlas complete!\n");
    return 0;
}