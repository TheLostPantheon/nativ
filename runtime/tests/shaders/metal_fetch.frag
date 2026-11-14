#version 450
layout(input_attachment_index = 0, set = 2, binding = 0) uniform subpassInput previous;
layout(location = 0) out vec4 color;
void main() {
	color = subpassLoad(previous) + vec4(0.25, 0.25, 0.25, 0.0);
}
