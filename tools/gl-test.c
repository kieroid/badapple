// SPDX-License-Identifier: GPL-2.0+ OR MIT
// Copyright (c) 2026 kiki <kieran@lavato.net>
//
// GLES2 on the A12 GPU without a display: EGL surfaceless, a 64x64
// framebuffer object, a clear, a triangle and an interpolated gradient,
// each read back and checked on the CPU.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>

#define W 64
#define H 64

static unsigned px[W * H];

/* Every 8th pixel of every 8th row, to see where writes land. */
static void grid(void)
{
	for (int y = 4; y < H; y += 8) {
		for (int x = 4; x < W; x += 8)
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

static void program(const char *vs, const char *fs)
{
	GLuint prog = glCreateProgram();

	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glUseProgram(prog);
}

int main(void)
{
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static const GLfloat tri[] = { -1, -1, 1, -1, -1, 1 };
	static const GLfloat full[] = { -1, -1, 3, -1, -1, 3 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	EGLContext ctx;
	EGLConfig cfg;
	EGLint n;
	GLuint fb, rb;
	int fail = 0;

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

	printf("%s\n", fail ? "FAIL" : "all ok");
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return fail;
}
