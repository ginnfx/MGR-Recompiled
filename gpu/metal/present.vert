#version 450
layout(location=0) out vec2 uv;
layout(location=1) out vec4 color;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p*2.0-1.0,0,1);
    uv = vec2(p.x,1.0-p.y);
    color = vec4(1);
}
