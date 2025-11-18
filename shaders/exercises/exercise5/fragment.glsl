#version 410

in vec3 position_eye;
in vec3 normal_eye;

// Two lights for double-sided lighting
vec3 light1_position_world = vec3(0.0, 0.0, 2.0);   // Front light (in front of camera)
vec3 light2_position_world = vec3(0.0, 0.0, -10.0); // Back light (behind triangle)

// Light properties (same for both lights)
vec3 Ls = vec3(1.0, 1.0, 1.0); // white specular colour
vec3 Ld = vec3(0.7, 0.7, 0.7); // dull white diffuse light colour
vec3 La = vec3(0.2, 0.2, 0.2); // grey ambient colour

// Surface reflectance
vec3 Ks = vec3(1.0, 1.0, 1.0); // fully reflect specular light
vec3 Kd = vec3(1.0, 0.5, 0.0); // orange diffuse surface reflectance
vec3 Ka = vec3(1.0, 1.0, 1.0); // fully reflect ambient light

uniform mat4 view;
uniform float specular_exponent;
uniform int use_blinn;

out vec4 fragment_colour;

// Function to calculate Phong lighting for one light
vec3 calculate_phong(vec3 light_pos_world, vec3 pos_eye, vec3 norm_eye, bool use_blinn_phong) {
    // Convert light to eye space
    vec3 light_position_eye = vec3(view * vec4(light_pos_world, 1.0));
    vec3 distance_to_light_eye = light_position_eye - pos_eye;
    vec3 direction_to_light_eye = normalize(distance_to_light_eye);
    
    // Diffuse intensity
    float dot_prod = dot(direction_to_light_eye, norm_eye);
    dot_prod = max(dot_prod, 0.0);
    vec3 Id = Ld * Kd * dot_prod;
    
    // Specular intensity
    vec3 surface_to_viewer_eye = normalize(-pos_eye);
    float dot_prod_specular;
    
    if (use_blinn_phong) {
        // Blinn-Phong
        vec3 half_way_eye = normalize(surface_to_viewer_eye + direction_to_light_eye);
        dot_prod_specular = max(dot(half_way_eye, norm_eye), 0.0);
    } else {
        // Original Phong
        vec3 reflection_eye = reflect(-direction_to_light_eye, norm_eye);
        dot_prod_specular = max(dot(reflection_eye, surface_to_viewer_eye), 0.0);
    }
    
    float specular_factor = pow(dot_prod_specular, specular_exponent);
    vec3 Is = Ls * Ks * specular_factor;
    
    return Id + Is;
}

void main() {
    // Normalize the normal (it might not be unit length after transformation)
    vec3 norm = normalize(normal_eye);
    
    // Ambient intensity (only calculated once, not per light)
    vec3 Ia = La * Ka;
    
    // Calculate lighting from both lights
    bool use_blinn_phong = (use_blinn == 1);
    vec3 light1_contribution = calculate_phong(light1_position_world, position_eye, norm, use_blinn_phong);
    vec3 light2_contribution = calculate_phong(light2_position_world, position_eye, norm, use_blinn_phong);
    
    // Combine: ambient + light1 + light2
    vec3 final_color = Ia + light1_contribution + light2_contribution;
    
    // Final colour
    fragment_colour = vec4(final_color, 1.0);
}