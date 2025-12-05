#version 410

in vec3 position_eye;
in vec3 normal_eye;
in vec2 st;

uniform sampler2D diffuse_map;   // Color texture (Kd)
uniform sampler2D specular_map;  // Specular texture (Ks) + emission (alpha)
uniform vec3 light_position_eye;
uniform float specular_exponent;

out vec4 frag_colour;

// Light colors
const vec3 La = vec3(0.2, 0.2, 0.2);  // Ambient
const vec3 Ld = vec3(0.8, 0.8, 0.8);  // Diffuse
const vec3 Ls = vec3(1.0, 1.0, 1.0);  // Specular

void main() {
    // Sample textures
    vec4 diffuse_sample = texture(diffuse_map, st);
    vec4 specular_sample = texture(specular_map, st);
    
    // Diffuse map RGB = Kd (diffuse reflection coefficient)
    vec3 Kd = diffuse_sample.rgb;
    
    // Specular map RGB = Ks (specular reflection coefficient)
    vec3 Ks = specular_sample.rgb;
    
    // Specular map ALPHA = emission factor (self-illumination)
    float emission = specular_sample.a;
    
    // Ambient uses diffuse color
    vec3 Ka = Kd;
    
    // Normalize vectors
    vec3 normal_eye_normalized = normalize(normal_eye);
    vec3 surface_to_light_eye = normalize(light_position_eye - position_eye);
    vec3 view_dir_eye = normalize(-position_eye);
    
    // Ambient component
    vec3 Ia = La * Ka;
    
    // Diffuse component
    float dot_prod = max(dot(surface_to_light_eye, normal_eye_normalized), 0.0);
    vec3 Id = Ld * Kd * dot_prod;
    
    // Specular component (Blinn-Phong)
    vec3 Is = vec3(0.0);
    if (dot_prod > 0.0) {
        vec3 half_way_eye = normalize(surface_to_light_eye + view_dir_eye);
        float dot_blinn = max(dot(half_way_eye, normal_eye_normalized), 0.0);
        float specular_factor = pow(dot_blinn, specular_exponent);
        Is = Ls * Ks * specular_factor;
    }
    
    // Emission (self-illuminating parts)
    vec3 Ie = Ks * emission;
    
    // Final color: lighting + emission
    frag_colour = vec4(Ia + Id + Is + Ie, diffuse_sample.a);
}