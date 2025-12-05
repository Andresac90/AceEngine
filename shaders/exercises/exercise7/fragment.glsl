#version 410

in vec3 position_eye;
in vec3 normal_eye;

uniform float specular_exponent;
uniform int use_blinn;
uniform int show_normals;  // NEW: Toggle for normal visualization

out vec4 frag_colour;

void main() {
    // Normal visualization mode (for debugging)
    if (show_normals == 1) {
        // Convert normals from [-1, 1] to [0, 1] color range
        vec3 normal_color = normalize(normal_eye) * 0.5 + 0.5;
        frag_colour = vec4(normal_color, 1.0);
        return;
    }
    
    // Regular Phong/Blinn-Phong lighting
    vec3 light_position_eye = vec3(0.0, 0.0, 2.0);
    vec3 Ls = vec3(1.0, 1.0, 1.0);  // Specular light color
    vec3 Ld = vec3(0.7, 0.7, 0.7);  // Diffuse light color
    vec3 La = vec3(0.2, 0.2, 0.2);  // Ambient light color
    
    vec3 Ks = vec3(1.0, 1.0, 1.0);  // Specular material color
    vec3 Kd = vec3(0.5, 0.5, 0.9);  // Diffuse material color
    vec3 Ka = vec3(1.0, 1.0, 1.0);  // Ambient material color
    
    vec3 surface_to_light_eye = normalize(light_position_eye - position_eye);
    vec3 normal_eye_normalized = normalize(normal_eye);
    
    // Ambient
    vec3 Ia = La * Ka;
    
    // Diffuse
    float dot_prod = max(dot(surface_to_light_eye, normal_eye_normalized), 0.0);
    vec3 Id = Ld * Kd * dot_prod;
    
    // Specular
    vec3 Is = vec3(0.0);
    if (dot_prod > 0.0) {
        vec3 view_dir_eye = normalize(-position_eye);
        
        if (use_blinn == 1) {
            // Blinn-Phong
            vec3 half_way_eye = normalize(surface_to_light_eye + view_dir_eye);
            float dot_blinn = max(dot(half_way_eye, normal_eye_normalized), 0.0);
            float specular_factor = pow(dot_blinn, specular_exponent);
            Is = Ls * Ks * specular_factor;
        } else {
            // Classic Phong
            vec3 reflection_eye = reflect(-surface_to_light_eye, normal_eye_normalized);
            float dot_spec = max(dot(reflection_eye, view_dir_eye), 0.0);
            float specular_factor = pow(dot_spec, specular_exponent);
            Is = Ls * Ks * specular_factor;
        }
    }
    
    frag_colour = vec4(Ia + Id + Is, 1.0);
}