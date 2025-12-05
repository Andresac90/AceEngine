# UV Unwrapping and Texturing Guide for Blender Models

## Part A: UV Unwrap Suzanne

1. **Open Blender → Add Suzanne**
```
   Shift+A → Mesh → Monkey
```

2. **Enter Edit Mode**
```
   Tab (or top-left dropdown: Edit Mode)
```

3. **Select All**
```
   A (select all vertices)
```

4. **Mark Seams (Optional but recommended)**
```
   - Select edge loops where you want UV cuts
   - Alt+Click edges around eyes, ears
   - Ctrl+E → Mark Seam
```

5. **UV Unwrap**
```
   U → Smart UV Project
   (or U → Unwrap if you marked seams)
```

6. **Check UV Layout**
```
   - Split viewport: Drag top-right corner
   - Change one view to UV Editor
   - You should see the UV layout
```

---

## Part B: Export UV Layout for Painting

1. **UV Editor → Menu Bar**
```
   UV → Export UV Layout
```

2. **Save as PNG**
```
   File: suzanne_uv_layout.png
   Size: 1024x1024 (or 2048x2048 for higher quality)
   Format: PNG
   Fill: Blank (or wireframe)
```

3. **Save to:** `assets/textures/suzanne_uv_layout.png`

---

## Part C: Paint Textures in GIMP/Photoshop

### Create Diffuse Map

1. Open `suzanne_uv_layout.png` in GIMP
2. Paint colors on the UV layout
3. Save as: `assets/textures/suzanne_diffuse.png`

### Create Specular Map

1. Duplicate the image
2. Paint in grayscale:
   - **White** = Very shiny (metal, eyes)
   - **Gray** = Medium shininess
   - **Black** = No shininess (fur, skin)
3. Add **Alpha channel**: Layer → Transparency → Add Alpha Channel
4. Paint alpha for emission:
   - **White alpha** = Glowing (eyes, magic parts)
   - **Black alpha** = No glow
5. Save as: `assets/textures/suzanne_specular.png` (RGBA)

---

## Part D: Apply Texture in Blender (Preview)

1. **Shading Workspace** (top tabs)

2. **Add Material:**
```
   - Material Properties (right panel, sphere icon)
   - Click "+ New"
```

3. **Add Image Texture:**
```
   - In Shader Editor (bottom panel)
   - Shift+A → Texture → Image Texture
   - Open → suzanne_diffuse.png
   - Connect to Base Color
```

4. **Switch to Material Preview:**
```
   Top-right viewport shading: Click 3rd sphere icon
```

---

## Part E: Export for AceEngine
```
File → Export → Wavefront (.obj)

✅ Selection Only
✅ Apply Modifiers
✅ Write Normals
✅ Include UVs
✅ Triangulate Faces
✅ Forward: -Z, Up: Y
```

Save to: `assets/models/suzanne.obj`

---

## Texture Map Reference

| Map Type | Purpose | RGB Channels | Alpha Channel |
|----------|---------|--------------|---------------|
| **Diffuse** | Surface color | Object color (wood, metal, etc.) | Transparency (optional) |
| **Specular** | Shininess control | White = shiny, Black = matte | Emission/Glow strength |
| **Normal** | Surface detail | Encoded normal vectors | Unused |
| **Roughness** | Surface roughness | White = rough, Black = smooth | Unused |

---

## Tips & Best Practices

- **Resolution**: Use 2048x2048 or 4096x4096 for high-quality textures
- **Power of 2**: Always use power-of-2 dimensions (512, 1024, 2048, 4096)
- **Format**: PNG for images with alpha, JPG for diffuse-only maps
- **Seams**: Place seams in hidden areas (back of head, under arms)
- **Testing**: Always test your textures in the engine before finalizing
- **Triangulation**: Always enable "Triangulate Faces" when exporting for OpenGL

---

## Common Issues

**Black areas in texture**: UV islands are overlapping or outside 0-1 range

**Stretched textures**: UV unwrap needs better seam placement

**Seams visible in 3D**: Use "Average Islands Scale" in UV editor

**Missing normals**: Enable "Write Normals" in export settings

**Texture appears upside down**: Different software uses different UV conventions; flip in image editor if needed