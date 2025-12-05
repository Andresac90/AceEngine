#version 410 core

uniform mat4 V;
uniform sampler2D p_tex;
uniform sampler2D n_tex;
uniform vec3 ls;  // Light specular color
uniform vec3 ld;  // Light diffuse color
uniform vec3 lp;  // Light position (world space)
uniform vec2 viewport_size;

out vec4 frag_colour;

// Material properties
vec3 kd = vec3(1.0, 1.0, 1.0);
vec3 ks = vec3(1.0, 1.0, 1.0);
float specular_exponent = 100.0;

vec3 phong(in vec3 p_eye, in vec3 n_eye) {
    vec3 light_position_eye = vec3(V * vec4(lp, 1.0));
    vec3 dist_to_light_eye = light_position_eye - p_eye;
    vec3 direction_to_light_eye = normalize(dist_to_light_eye);
    
    // Diffuse
    float dot_prod = max(dot(direction_to_light_eye, n_eye), 0.0);
    vec3 Id = ld * kd * dot_prod;
    
    // Specular
    vec3 reflection_eye = reflect(-direction_to_light_eye, n_eye);
    vec3 surface_to_viewer_eye = normalize(-p_eye);
    float dot_prod_specular = dot(reflection_eye, surface_to_viewer_eye);
    dot_prod_specular = max(dot_prod_specular, 0.0);
    float specular_factor = pow(dot_prod_specular, specular_exponent);
    vec3 Is = ls * ks * specular_factor;
    
    // Attenuation (fade to sphere edges)
    float dist = length(dist_to_light_eye) / 5.0;
    float atten_factor = max(0.0, 1.0 - dist);
    
    return (Id + Is) * atten_factor;
}

void main() {
    // Sample from textures using screen coordinates
    vec2 st;
    st.s = gl_FragCoord.x / viewport_size.x;
    st.t = gl_FragCoord.y / viewport_size.y;
    
    vec3 p_texel = texture(p_tex, st).rgb;
    vec3 n_texel = texture(n_tex, st).rgb;
    
    // Skip background (z > -0.0001 means very far away/background)
    if (p_texel.z > -0.0001) {
        discard;
    }
    
    frag_colour.rgb = phong(p_texel, normalize(n_texel));
    frag_colour.a = 1.0;
}