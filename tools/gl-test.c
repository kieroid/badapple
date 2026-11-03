// SPDX-License-Identifier: GPL-2.0+ OR MIT
// Copyright (c) 2026 kiki <kieran@lavato.net>
//
// GLES2 on the A12 GPU without a display: EGL surfaceless, a 300x200
// framebuffer object (3x2 macrotiles), a clear, a triangle, an
// interpolated gradient, a flat varying, a point, a line, blending,
// textures, depth and stencil tests, and 3000 draws, each read back and
// checked on the CPU.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>

#define W 300
#define H 200

static unsigned px[W * H];

/* An 8x8 grid of samples, to see where writes land. */
static void grid(void)
{
	for (int y = H / 16; y < H; y += H / 8) {
		for (int x = W / 16; x < W; x += W / 8)
			printf(" %08x", px[y * W + x]);
		printf("\n");
	}
}

/* Channel by channel within tol, for interpolated values. */
static int check(const char *name, int x, int y, unsigned want, int tol)
{
	unsigned got = px[y * W + x];
	int bad = 0;

	for (int i = 0; i < 32; i += 8) {
		int d = (int)(got >> i & 0xff) - (int)(want >> i & 0xff);

		bad |= d > tol || d < -tol;
	}
	printf("%s: (%d,%d) = %08x, want %08x: %s\n", name, x, y, got, want,
	       bad ? "FAIL" : "ok");
	return bad;
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
	}
	return s;
}

static GLuint program(const char *vs, const char *fs)
{
	GLuint prog = glCreateProgram();

	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glUseProgram(prog);
	return prog;
}

int main(void)
{
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static const GLfloat tri[] = { -1, -1, 1, -1, -1, 1 };
	static const GLfloat full[] = { -1, -1, 3, -1, -1, 3 };
	static const GLfloat rgb[] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	static const GLfloat mid[] = { 0, 0 };
	/* across row 50, H / 4 */
	static const GLfloat row[] = { -1, -0.495, 1, -0.495 };
	/* red, green, blue and white, bottom row first */
	static const unsigned texels[] = { 0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	EGLContext ctx;
	EGLConfig cfg;
	EGLint n;
	GLuint fb, rb, db, sb, tex, prog;
	GLint z, c;
	int fail = 0;

	/* unbuffered, so a GPU hang shows which check it stopped at */
	setvbuf(stdout, NULL, _IONBF, 0);
	if (!eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) {
		fprintf(stderr, "EGL init failed\n");
		return 1;
	}
	eglChooseConfig(dpy, (EGLint[]){ EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE },
			&cfg, 1, &n);
	ctx = eglCreateContext(dpy, n ? cfg : NULL, EGL_NO_CONTEXT, ctx_attr);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "no context\n");
		return 1;
	}
	printf("renderer: %s\n", glGetString(GL_RENDERER));

	glGenFramebuffers(1, &fb);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	glViewport(0, 0, W, H);

	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	fail |= check("clear", W / 2, H / 2, 0xff0000ff, 0);
	grid();

	/* Lower-left half triangle in green over the red clear. */
	program("attribute vec2 p; void main() { gl_Position = vec4(p, 0, 1); }",
		"void main() { gl_FragColor = vec4(0, 1, 0, 1); }");
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	glEnableVertexAttribArray(0);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	fail |= check("triangle", 8, 8, 0xff00ff00, 0);
	fail |= check("outside", W - 8, H - 8, 0xff0000ff, 0);

	/*
	 * Full screen gradient through two varyings: red follows x, green
	 * follows y, blue is a constant 0.5 from the vertex shader.
	 */
	program("attribute vec2 p; varying vec2 v; varying float b;"
		"void main() { v = p * 0.5 + 0.5; b = 0.5; gl_Position = vec4(p, 0, 1); }",
		"precision mediump float; varying vec2 v; varying float b;"
		"void main() { gl_FragColor = vec4(v, b, 1); }");
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, full);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	for (int i = 0; i < 4; i++) {
		int x = (int[]){ 0, W - 1, 13, 50 }[i], y = (int[]){ 0, H - 1, 41, 7 }[i];
		unsigned r = (2 * x + 1) * 255 / (2 * W), g = (2 * y + 1) * 255 / (2 * H);

		fail |= check("gradient", x, y, 0xff800000 | g << 8 | r, 2);
	}

	/*
	 * A flat varying: red, green and blue corners come out all blue, the
	 * last vertex's (GL's provoking vertex).
	 */
	program("#version 300 es\n"
		"layout(location = 0) in vec2 p; layout(location = 1) in vec3 k; flat out vec3 v;"
		"void main() { v = k; gl_Position = vec4(p, 0, 1); }",
		"#version 300 es\n"
		"precision mediump float; flat in vec3 v; out vec4 o;"
		"void main() { o = vec4(v, 1); }");
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, rgb);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisableVertexAttribArray(1);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	fail |= check("flat", 8, 8, 0xffff0000, 0);
	fail |= check("flat", W - 8, H - 8, 0xffff0000, 0);

	/*
	 * A yellow 16 pixel point in the middle and a line along row 50, the
	 * colour through a varying (the point size goes after varyings).
	 */
	program("attribute vec2 p; varying vec4 v;"
		"void main() { v = vec4(1, 1, 0, 1); gl_PointSize = 16.0; gl_Position = vec4(p, 0, 1); }",
		"precision mediump float; varying vec4 v; void main() { gl_FragColor = v; }");
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, mid);
	glDrawArrays(GL_POINTS, 0, 1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, row);
	glDrawArrays(GL_LINES, 0, 2);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	fail |= check("point", W / 2 - 8, H / 2 + 7, 0xff00ffff, 0);
	fail |= check("point edge", W / 2 + 8, H / 2, 0xff0000ff, 0);
	fail |= check("line", 3, H / 4, 0xff00ffff, 0);
	fail |= check("line", W - 3, H / 4, 0xff00ffff, 0);
	fail |= check("line edge", W / 2, H / 4 + 1, 0xff0000ff, 0);

	/*
	 * Half transparent blue blended over the red clear, before any sampler
	 * is bound: G11 reads the render target through Mesa's own one.
	 */
	program("attribute vec2 p; void main() { gl_Position = vec4(p, 0, 1); }",
		"void main() { gl_FragColor = vec4(0, 0, 1, 0.5); }");
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, full);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisable(GL_BLEND);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	fail |= check("blend", W / 2, H / 2, 0xbf800080, 1);

	/*
	 * A 2x2 texture over the whole target: nearest gives one texel per
	 * quadrant, linear blends all four in the middle.
	 */
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	program("attribute vec2 p; varying vec2 v;"
		"void main() { v = p * 0.5 + 0.5; gl_Position = vec4(p, 0, 1); }",
		"precision mediump float; uniform sampler2D t; varying vec2 v;"
		"void main() { gl_FragColor = texture2D(t, v); }");
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, full);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	for (int i = 0; i < 4; i++)
		fail |= check("texture", (1 + i % 2 * 2) * W / 4, (1 + i / 2 * 2) * H / 4,
			      texels[i], 0);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	fail |= check("linear", W / 2, H / 2, 0xff808080, 2);


	/*
	 * Depth, LESS against a clear to 0.7: a near green triangle (depth
	 * 0.5) lands, a far blue full screen one (0.75) nowhere. A second
	 * render loads the stored depth and draws white at 0.625, only
	 * outside the triangle.
	 */
	glGenRenderbuffers(1, &db);
	glBindRenderbuffer(GL_RENDERBUFFER, db);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, db);
	prog = program("attribute vec2 p; uniform float z; void main() { gl_Position = vec4(p, z, 1); }",
		       "precision mediump float; uniform vec4 c; void main() { gl_FragColor = c; }");
	z = glGetUniformLocation(prog, "z");
	c = glGetUniformLocation(prog, "c");
	glEnable(GL_DEPTH_TEST);
	glClearDepthf(0.7);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glUniform1f(z, 0);
	glUniform4f(c, 0, 1, 0, 1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glUniform1f(z, 0.5);
	glUniform4f(c, 0, 0, 1, 1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, full);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	fail |= check("depth near", 8, 8, 0xff00ff00, 0);
	fail |= check("depth far", W - 8, H - 8, 0xff0000ff, 0);

	glUniform1f(z, 0.25);
	glUniform4f(c, 1, 1, 1, 1);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	fail |= check("depth load", 8, 8, 0xff00ff00, 0);
	fail |= check("depth pass", W - 8, H - 8, 0xffffffff, 0);

	/*
	 * Stencil, cleared to 3: the triangle increments it to 4, then blue
	 * lands where it is still 3. A second render loads it and draws white
	 * where it is 4.
	 */
	glDisable(GL_DEPTH_TEST);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
	glGenRenderbuffers(1, &sb);
	glBindRenderbuffer(GL_RENDERBUFFER, sb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sb);
	glEnable(GL_STENCIL_TEST);
	glClearStencil(3);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glUniform1f(z, 0);
	glStencilFunc(GL_ALWAYS, 0, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
	glUniform4f(c, 0, 1, 0, 1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glStencilFunc(GL_EQUAL, 3, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glUniform4f(c, 0, 0, 1, 1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, full);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	fail |= check("stencil write", 8, 8, 0xff00ff00, 0);
	fail |= check("stencil clear", W - 8, H - 8, 0xffff0000, 0);

	glStencilFunc(GL_EQUAL, 4, 0xff);
	glUniform4f(c, 1, 1, 1, 1);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	fail |= check("stencil load", 8, 8, 0xffffffff, 0);
	fail |= check("stencil fail", W - 8, H - 8, 0xffff0000, 0);

	/* 3000 draws in one render, so their pipelines span several pool slabs. */
	glDisable(GL_STENCIL_TEST);
	for (int i = 0; i < 3000; i++) {
		glUniform4f(c, i % 2, i % 3 / 2.0, 0, 1);
		glDrawArrays(GL_TRIANGLES, 0, 3);
	}
	glUniform4f(c, 0, 1, 1, 1);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	grid();
	fail |= check("draws", W / 2, H / 2, 0xffffff00, 0);

	printf("%s\n", fail ? "FAIL" : "all ok");
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return fail;
}
