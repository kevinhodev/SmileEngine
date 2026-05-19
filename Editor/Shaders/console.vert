#version 440

layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;

layout(std140, binding = 0) uniform Buf {
    mat4 mvp;
} ubuf;

layout(location = 0) out vec2 v_uv;

void main()
{
    v_uv = a_uv;
    gl_Position = ubuf.mvp * vec4(a_pos, 0.0, 1.0);
}
