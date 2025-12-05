#version 410 core

in vec3 normal;
in vec2 texcoord;
in vec3 frag_pos;

uniform vec3 light_pos;
uniform vec3 view_pos;
uniform sampler2D tex;

out vec4 frag_colour;

void main() {
    // Phong lighting
    vec3 norm = normalize(normal);
    vec3 light_dir = normalize(light_pos - frag_pos);
    
    // Ambient
    vec3 ambient = 0.2 * vec3(1.0);
    
    // Diffuse
    float diff = max(dot(norm, light_dir), 0.0);
    vec3 diffuse = diff * vec3(1.0);
    
    // Specular
    vec3 view_dir = normalize(view_pos - frag_pos);
    vec3 reflect_dir = reflect(-light_dir, norm);
    float spec = pow(max(dot(view_dir, reflect_dir), 0.0), 32.0);
    vec3 specular = 0.5 * spec * vec3(1.0);
    
    vec3 tex_colour = texture(tex, texcoord).rgb;
    vec3 result = (ambient + diffuse + specular) * tex_colour;
    
    frag_colour = vec4(result, 1.0);
}