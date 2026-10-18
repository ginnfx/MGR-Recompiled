#version 450
struct Vertex { float x; float y; float u; float v; uint color; };
layout(set=0,binding=0,std430) readonly buffer Vertices { Vertex data[]; } vertices;
layout(set=0,binding=1,std430) readonly buffer Projection { vec4 scale_offset; } projection;
layout(location=0) out vec2 uv;
layout(location=1) out vec4 color;
void main() {
    Vertex v = vertices.data[gl_VertexIndex];
    gl_Position = vec4(vec2(v.x,v.y)*projection.scale_offset.xy+projection.scale_offset.zw,0,1);
    uv = vec2(v.u,v.v);
    color = unpackUnorm4x8(v.color);
}
