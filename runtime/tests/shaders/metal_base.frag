#version 450
layout(location = 0) out vec4 color;
void main() {
	vec2 rg = unpackHalf2x16(packHalf2x16(vec2(0.125, 0.25)));
	color = vec4(rg, 0.5, 1.0);
}
