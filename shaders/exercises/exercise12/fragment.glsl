#version 410

in vec2 st;
in vec3 view_dir_tan;   // View direction in tangent space
in vec3 light_dir_tan;  // Light direction in tangent space

uniform sampler2D normal_map;
uniform sampler2D diffuse_map;
uniform int use_normal_map;
uniform float specular_exponent;

out vec4 frag_colour;

// Light properties
const vec3 La = vec3(0.2, 0.2, 0.2);  // Ambient
const vec3 Ld = vec3(0.8, 0.8, 0.8);  // Diffuse
const vec3 Ls = vec3(1.0, 1.0, 1.0);  // Specular

// Material properties
const vec3 Ka = vec3(1.0, 1.0, 1.0);
const vec3 Ks = vec3(0.5, 0.5, 0.5);

void main() {
    // Sample diffuse color
    vec3 Kd = texture(diffuse_map, st).rgb;
    
    // Get normal (either from normal map or default)
    vec3 normal_tan;
    if (use_normal_map == 1) {
        // Sample normal map and convert from [0,1] to [-1,1] range
        normal_tan = texture(normal_map, st).rgb;
        normal_tan = normalize(normal_tan * 2.0 - 1.0);
    } else {
        // Default tangent space normal (pointing straight out)
        normal_tan = vec3(0.0, 0.0, 1.0);
    }
    
    // Normalize interpolated vectors
    vec3 view_dir_normalized = normalize(view_dir_tan);
    vec3 light_dir_normalized = normalize(-light_dir_tan);  // Direction TO light
    
    // Ambient component
    vec3 Ia = La * Ka * Kd;
    
    // Diffuse component
    float dot_prod = max(dot(light_dir_normalized, normal_tan), 0.0);
    vec3 Id = Ld * Kd * dot_prod;
    
    // Specular component (Blinn-Phong)
    vec3 Is = vec3(0.0);
    if (dot_prod > 0.0) {
        vec3 half_way = normalize(light_dir_normalized + view_dir_normalized);
        float dot_spec = max(dot(half_way, normal_tan), 0.0);
        float spec_factor = pow(dot_spec, specular_exponent);
        Is = Ls * Ks * spec_factor;
    }
    
    // Final color
    frag_colour = vec4(Ia + Id + Is, 1.0);
}