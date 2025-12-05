#version 410

in vec3 position_eye;
in vec3 normal_eye;

uniform vec3 light_position_eye;      // Spotlight/Point light position
uniform vec3 light_direction_eye;     // Spotlight direction
uniform float spotlight_cone_angle;   // Cone angle in radians (matches C++ name!)
uniform float spotlight_attenuation;  // 0.0 = hard edge, 1.0 = smooth (matches C++ name!)
uniform int light_type;               // 0 = point, 1 = directional, 2 = spotlight
uniform float specular_exponent;

out vec4 frag_colour;

// Light colors
const vec3 La = vec3(0.2, 0.2, 0.2);  // Ambient
const vec3 Ld = vec3(0.9, 0.9, 0.9);  // Diffuse
const vec3 Ls = vec3(1.0, 1.0, 1.0);  // Specular

// Material colors
const vec3 Ka = vec3(1.0, 1.0, 1.0);  // Ambient
const vec3 Kd = vec3(0.8, 0.5, 0.2);  // Diffuse (orange)
const vec3 Ks = vec3(1.0, 1.0, 1.0);  // Specular

void main() {
    vec3 normal_eye_normalized = normalize(normal_eye);
    vec3 view_dir_eye = normalize(-position_eye);
    
    // Calculate direction to light based on light type
    vec3 direction_to_light_eye;
    
    if (light_type == 1) {
        // DIRECTIONAL LIGHT (like the sun)
        direction_to_light_eye = normalize(-light_direction_eye);
    } else {
        // POINT LIGHT or SPOTLIGHT
        direction_to_light_eye = normalize(light_position_eye - position_eye);
    }
    
    // Calculate spotlight factor (1.0 = fully lit, 0.0 = outside cone)
    float spot_factor = 1.0;
    
    if (light_type == 2) {
        // SPOTLIGHT calculation
        vec3 spot_dir_normalized = normalize(light_direction_eye);
        vec3 dir_to_surface_eye = normalize(position_eye - light_position_eye);
        
        float spot_dot = dot(spot_dir_normalized, dir_to_surface_eye);
        float spot_arc = cos(spotlight_cone_angle);
        
        if (spot_dot < spot_arc) {
            spot_factor = 0.0;
        } else {
            if (spotlight_attenuation > 0.0) {
                spot_factor = (spot_dot - spot_arc) / (1.0 - spot_arc);
                spot_factor = clamp(spot_factor, 0.0, 1.0);
                spot_factor = mix(1.0, spot_factor, spotlight_attenuation);
            } else {
                spot_factor = 1.0;
            }
        }
    }
    
    // Ambient component
    vec3 Ia = La * Ka;
    
    // Diffuse component
    float dot_prod = max(dot(direction_to_light_eye, normal_eye_normalized), 0.0);
    vec3 Id = Ld * Kd * dot_prod;
    
    // Specular component (Blinn-Phong)
    vec3 Is = vec3(0.0);
    if (dot_prod > 0.0) {
        vec3 half_way_eye = normalize(direction_to_light_eye + view_dir_eye);
        float dot_blinn = max(dot(half_way_eye, normal_eye_normalized), 0.0);
        float specular_factor = pow(dot_blinn, specular_exponent);
        Is = Ls * Ks * specular_factor;
    }
    
    // Apply spotlight factor to diffuse and specular
    Id *= spot_factor;
    Is *= spot_factor;
    
    frag_colour = vec4(Ia + Id + Is, 1.0);
}