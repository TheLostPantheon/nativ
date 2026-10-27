#version 450
#extension GL_GOOGLE_include_directive : require
#include "post_common.glsl"
// post.c's fxaa_fs (FXAA 3.11 quality, Lottes' published algorithm): the same steps
void main() {
	vec2 r = u.rcp, uv = (gl_FragCoord.xy - u.origin) * r;
	vec3 cM = tap(uv); float lM = lum(cM);
	float lN = lum(tap(uv + vec2(0.0, -r.y))), lS = lum(tap(uv + vec2(0.0, r.y)));
	float lE = lum(tap(uv + vec2(r.x, 0.0))), lW = lum(tap(uv + vec2(-r.x, 0.0)));
	float lMin = min(lM, min(min(lN, lS), min(lE, lW))), lMax = max(lM, max(max(lN, lS), max(lE, lW))), range = lMax - lMin;
	if (range < max(0.0312, lMax * 0.125)) { o = vec4(cM, 1.0); return; }
	float lNW = lum(tap(uv + vec2(-r.x, -r.y))), lNE = lum(tap(uv + vec2(r.x, -r.y)));
	float lSW = lum(tap(uv + vec2(-r.x, r.y))), lSE = lum(tap(uv + vec2(r.x, r.y)));
	float lNS = lN + lS, lWE = lW + lE;
	float eH = abs(-2.0 * lW + lNW + lSW) + abs(-2.0 * lM + lNS) * 2.0 + abs(-2.0 * lE + lNE + lSE);
	float eV = abs(-2.0 * lN + lNW + lNE) + abs(-2.0 * lM + lWE) * 2.0 + abs(-2.0 * lS + lSW + lSE);
	bool horz = eH >= eV;
	float l1 = horz ? lN : lW, l2 = horz ? lS : lE, g1 = l1 - lM, g2 = l2 - lM;
	bool steep1 = abs(g1) >= abs(g2); float gS = 0.25 * max(abs(g1), abs(g2));
	float stepL = horz ? r.y : r.x, lAvg;
	if (steep1) { stepL = -stepL; lAvg = 0.5 * (l1 + lM); } else lAvg = 0.5 * (l2 + lM);
	vec2 cuv = uv; if (horz) cuv.y += stepL * 0.5; else cuv.x += stepL * 0.5;
	vec2 off = horz ? vec2(r.x, 0.0) : vec2(0.0, r.y), uv1 = cuv - off, uv2 = cuv + off;
	float e1 = lum(tap(uv1)) - lAvg, e2 = lum(tap(uv2)) - lAvg;
	bool r1 = abs(e1) >= gS, r2 = abs(e2) >= gS;
	const float Q[12] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
	if (!r1) uv1 -= off; if (!r2) uv2 += off;
	for (int i = 2; i < 12 && !(r1 && r2); i++) {
		if (!r1) { e1 = lum(tap(uv1)) - lAvg; r1 = abs(e1) >= gS; if (!r1) uv1 -= off * Q[i]; }
		if (!r2) { e2 = lum(tap(uv2)) - lAvg; r2 = abs(e2) >= gS; if (!r2) uv2 += off * Q[i]; }
	}
	float d1 = horz ? uv.x - uv1.x : uv.y - uv1.y, d2 = horz ? uv2.x - uv.x : uv2.y - uv.y;
	bool dir1 = d1 < d2; float dF = min(d1, d2), thick = max(d1 + d2, 1e-6);
	float pOff = -dF / thick + 0.5;
	bool smaller = lM < lAvg, right = ((dir1 ? e1 : e2) < 0.0) != smaller;
	float fOff = right ? pOff : 0.0;
	float lAll = (1.0 / 12.0) * (2.0 * (lNS + lWE) + lNW + lNE + lSW + lSE);
	float sp = clamp(abs(lAll - lM) / range, 0.0, 1.0); sp = (-2.0 * sp + 3.0) * sp * sp;
	fOff = max(fOff, sp * sp * 0.75);
	vec2 fuv = uv; if (horz) fuv.y += fOff * stepL; else fuv.x += fOff * stepL;
	o = vec4(tap(fuv), 1.0);
}
