#version 410

in vec3 pos_eye;
in vec3 normal_eye;

uniform samplerCube cube_texture;
uniform mat4 view;
uniform int mode;  // 0 = reflection, 1 = refraction

out vec4 frag_colour;

void main() {
    // Incident vector (from camera to surface)
    vec3 incident_eye = normalize(pos_eye);
    vec3 normal = normalize(normal_eye);
    
    vec3 result;
    
    if (mode == 0) {
        // REFLECTION
        result = reflect(incident_eye, normal);
    } else {
        // REFRACTION
        // Ratio = refractive index of air (1.0) / water (1.3333)
        float ratio = 1.0 / 1.3333;
        result = refract(incident_eye, normal, ratio);
    }
    
    // Convert from eye space to world space
    result = vec3(inverse(view) * vec4(result, 0.0));
    
    frag_colour = texture(cube_texture, result);
}