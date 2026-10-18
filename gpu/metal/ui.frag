#version 450
layout(set=0,binding=2) uniform texture2D image;
layout(set=0,binding=3) uniform sampler image_sampler;
layout(location=0) in vec2 uv;
layout(location=1) in vec4 color;
layout(location=0) out vec4 output_color;
void main() { output_color = texture(sampler2D(image,image_sampler),uv) * color; }
