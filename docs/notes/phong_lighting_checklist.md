# Phong Lighting Common Mistakes Checklist

## Space/Coordinate System Issues
- [ ] Light position and vertex positions are in the same space (both world or both eye)
- [ ] Normals are transformed to the same space as positions (eye space)
- [ ] Using `view * model` to transform to eye space, not just `model`

## Vector Component Issues (xyzw)
- [ ] Direction vectors use w=0 (not translated)
- [ ] Position vectors use w=1 (can be translated)
- [ ] Check: `vec4(normal, 0.0)` for normals
- [ ] Check: `vec4(position, 1.0)` for positions

## Dot Product Issues
- [ ] Both vectors in dot product are normalized (unit length)
- [ ] Both vectors are directions, not distances
- [ ] Dot product result clamped to [0, 1] with `max(dot_prod, 0.0)`
- [ ] Vectors point in correct directions (not negated by mistake)

## Normal Transformation
- [ ] If using non-uniform scale, use normal matrix (inverse transpose of model-view)
- [ ] If using uniform scale only, can use model-view matrix directly
- [ ] Normals are normalized after transformation

## Buffer and Attribute Issues
- [ ] Vertex buffer contains correct data (print first few values)
- [ ] Stride is correct (0 for tightly packed, or size in bytes between vertices)
- [ ] Attribute locations match shader (location=0 for position, location=1 for normal)
- [ ] Not accidentally swapping positions and normals

## Type Conversion Issues
- [ ] Cast vec4 to vec3 properly: `vec3(view * model * vec4(pos, 1.0))`
- [ ] Don't forget the w component when converting vec3 to vec4

## Uniform Variable Issues
- [ ] Uniform names in code match shader exactly
- [ ] Uniforms are set after `shader.use()`
- [ ] Check uniform location is not -1
- [ ] Not using hard-coded values in shader when uniform should be used

## Color Value Issues
- [ ] All colors in range [0.0, 1.0], not [0, 255]
- [ ] Final color = ambient + diffuse + specular
- [ ] Alpha channel set to 1.0
- [ ] Not over-saturating (values > 1.0 will clamp)

## Debug Commands
```cpp
// Check buffer data
std::cout << "Point: " << points[0] << ", " << points[1] << ", " << points[2] << std::endl;

// Check uniform location
int loc = glGetUniformLocation(program, "uniformName");
std::cout << "Uniform location: " << loc << (loc != -1 ? " OK" : " ERROR!") << std::endl;

// Check for OpenGL errors
GLenum err = glGetError();
if (err != GL_NO_ERROR) std::cout << "GL Error: " << err << std::endl;

// Print matrix
matrix.print();
```