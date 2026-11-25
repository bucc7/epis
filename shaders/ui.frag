#version 450

layout(binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 local;
layout(location = 1) in vec2 halfsize;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec4 color;
layout(location = 4) flat in vec4 params;

layout(location = 0) out vec4 outc;

float sdbox(vec2 p, vec2 b, float r)
{
	vec2 q = abs(p) - b + r;

	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
	float a;

	if (params.z > 0.5) {
		outc = vec4(color.rgb, color.a * texture(atlas, uv).r);
		return;
	}
	float d = sdbox(local, halfsize, params.x);

	a = clamp(0.5 - d, 0.0, 1.0);
	if (params.y > 0.0)
		a *= clamp(0.5 + d + params.y, 0.0, 1.0);
	outc = vec4(color.rgb, color.a * a);
}
