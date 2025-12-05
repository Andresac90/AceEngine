#version 410 core

in vec3 normal;
in vec4 st_shadow;

uniform sampler2D depth_map;
uniform vec3 colour;
uniform vec3 light_dir;

out vec4 frag_colour;

float eval_shadow(vec2 texcoords, float current_depth) {
    // Epsilon to reduce self-shadowing artifacts
    float epsilon = 0.0042;
    
    // Sample depth map
    float shadow_depth = texture(depth_map, texcoords).r;
    
    // Check if outside shadow map
    if (texcoords.x < 0.0 || texcoords.x > 1.0 || 
        texcoords.y < 0.0 || texcoords.y > 1.0) {
        return 1.0;  // Not in shadow
    }
    
    // Compare depths
    if (shadow_depth + epsilon < current_depth) {
        return 0.3;  // In shadow
    }
    
    return 1.0;  // Not in shadow
}

void main() {
    // Simple diffuse lighting
    vec3 norm = normalize(normal);
    float diff = max(dot(norm, -light_dir), 0.0);
    
    // Ambient + diffuse
    vec3 ambient = 0.2 * colour;
    vec3 diffuse = diff * colour;
    
    // Apply shadow
    float shadow_factor = eval_shadow(st_shadow.xy, st_shadow.z);
    
    vec3 result = ambient + shadow_factor * diffuse;
    frag_colour = vec4(result, 1.0);
}