#version 450
// the views' and the preload's full-screen triangle (view_msl's / preload_msl's main0)
void main() {
	vec2 p[3] = vec2[](vec2(-1.0, 1.0), vec2(3.0, 1.0), vec2(-1.0, -3.0));
	gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
