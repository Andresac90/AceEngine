#version 410

in vec3 position_eye;
in vec3 normal_eye;

uniform vec3 light_position_eye;
uniform float specular_exponent;
uniform vec3 fog_colour;
uniform float min_fog_radius;
uniform float max_fog_radius;
uniform int use_fog;

out vec4 frag_colour;

// Light colors
const vec3 La = vec3(0.2, 0.2, 0.2);
const vec3 Ld = vec3(0.8, 0.8, 0.8);
const vec3 Ls = vec3(1.0, 1.0, 1.0);

// Material colors
const vec3 Ka = vec3(1.0, 1.0, 1.0);
const vec3 Kd = vec3(0.5, 0.7, 1.0);  // Blue
const vec3 Ks = vec3(1.0, 1.0, 1.0);

void main() {
    vec3 normal_eye_normalized = normalize(normal_eye);
    vec3 direction_to_light_eye = normalize(light_position_eye - position_eye);
    vec3 view_dir_eye = normalize(-position_eye);
    
    // Ambient
    vec3 Ia = La * Ka;
    
    // Diffuse
    float dot_prod = max(dot(direction_to_light_eye, normal_eye_normalized), 0.0);
    vec3 Id = Ld * Kd * dot_prod;
    
    // Specular (Blinn-Phong)
    vec3 Is = vec3(0.0);
    if (dot_prod > 0.0) {
        vec3 half_way_eye = normalize(direction_to_light_eye + view_dir_eye);
        float dot_blinn = max(dot(half_way_eye, normal_eye_normalized), 0.0);
        float specular_factor = pow(dot_blinn, specular_exponent);
        Is = Ls * Ks * specular_factor;
    }
    
    // Phong lighting color (before fog)
    vec3 phong_colour = Ia + Id + Is;
    
    // Apply fog if enabled
    if (use_fog == 1) {
        // Distance from camera (camera is at origin in eye space)
        float dist = length(-position_eye);
        
        // Calculate fog factor (0 = no fog, 1 = full fog)
        float fog_fac = (dist - min_fog_radius) / (max_fog_radius - min_fog_radius);
        fog_fac = clamp(fog_fac, 0.0, 1.0);
        
        // Blend fog color with Phong lighting based on fog factor
        frag_colour = vec4(mix(phong_colour, fog_colour, fog_fac), 1.0);
    } else {
        frag_colour = vec4(phong_colour, 1.0);
    }
}