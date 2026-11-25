#version 450

layout(push_constant) uniform PC {
	vec2 screen;
} pc;

layout(location = 0) in vec4 rect;
layout(location = 1) in vec4 uvrect;
layout(location = 2) in vec4 color;
layout(location = 3) in vec4 params;

layout(location = 0) out vec2 local;
layout(location = 1) out vec2 halfsize;
layout(location = 2) out vec2 uv;
layout(location = 3) out vec4 vcolor;
layout(location = 4) flat out vec4 vparams;

void main()
{
	vec2 corners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1),
				 vec2(1, 0), vec2(1, 1), vec2(0, 1));
	vec2 c = corners[gl_VertexIndex];
	vec2 pos = rect.xy + c * rect.zw;

	halfsize = rect.zw * 0.5;
	local = (c - 0.5) * rect.zw;
	uv = mix(uvrect.xy, uvrect.zw, c);
	vcolor = color;
	vparams = params;
	gl_Position = vec4(pos / pc.screen * 2.0 - 1.0, 0.0, 1.0);
}
