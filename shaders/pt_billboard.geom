layout (points) in;
layout (triangle_strip, max_vertices=4) out;

uniform float size = 1.0;

void main()
{
	gl_FrontColor = gl_in[0].gl_FrontColor; // all colors are the same
	vec4 pos = gl_ModelViewMatrix * gl_in[0].gl_Position;
	vec4 pts[4];
	pts[0] = gl_ProjectionMatrix * (pos + vec4(-size,  size, 0.0, 0.0));
	pts[1] = gl_ProjectionMatrix * (pos + vec4(-size, -size, 0.0, 0.0));
	pts[2] = gl_ProjectionMatrix * (pos + vec4( size,  size, 0.0, 0.0));
	pts[3] = gl_ProjectionMatrix * (pos + vec4( size, -size, 0.0, 0.0));
	output_textured_quad(pts);
}
