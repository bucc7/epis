/* Metal version of ui.vert and ui.frag */
#include <metal_stdlib>
using namespace metal;

struct inst {
	float4 rect, uvrect, color, params;
};

struct varyings {
	float4 position [[position]];
	float2 local;
	float2 halfsize;
	float2 uv;
	float4 color;
	float4 params [[flat]];
};

vertex varyings ui_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
			  const device inst *instances [[buffer(0)]],
			  constant float2 &screen [[buffer(1)]])
{
	const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1),
				    float2(1, 0), float2(1, 1), float2(0, 1) };
	const device inst &in = instances[iid];
	float2 c = corners[vid];
	float2 pos = in.rect.xy + c * in.rect.zw;
	float2 ndc = pos / screen * 2.0 - 1.0;
	varyings out;

	out.position = float4(ndc.x, -ndc.y, 0.0, 1.0);
	out.halfsize = in.rect.zw * 0.5;
	out.local = (c - 0.5) * in.rect.zw;
	out.uv = mix(in.uvrect.xy, in.uvrect.zw, c);
	out.color = in.color;
	out.params = in.params;
	return out;
}

static float sdbox(float2 p, float2 b, float r)
{
	float2 q = abs(p) - b + r;

	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

fragment float4 ui_fragment(varyings in [[stage_in]],
			    texture2d<float> atlas [[texture(0)]])
{
	constexpr sampler nearest_sampler(filter::nearest,
					  address::clamp_to_edge);
	float d, a;

	if (in.params.z > 0.5)
		return float4(in.color.rgb, in.color.a *
			      atlas.sample(nearest_sampler, in.uv).r);
	d = sdbox(in.local, in.halfsize, in.params.x);
	a = clamp(0.5 - d, 0.0, 1.0);
	if (in.params.y > 0.0)
		a *= clamp(0.5 + d + in.params.y, 0.0, 1.0);
	return float4(in.color.rgb, in.color.a * a);
}
