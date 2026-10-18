#version 450
layout(set=0,binding=0,std430) readonly buffer Vertices { vec4 positions[]; } vertices;
layout(location=0) out vec4 color;
void main() {
    gl_Position = vertices.positions[gl_VertexIndex];
    color = gl_Position.z < 0.5 ? vec4(0.8, 0.2, 0.1, 0.5) : vec4(0.0, 1.0, 0.0, 1.0);
}
