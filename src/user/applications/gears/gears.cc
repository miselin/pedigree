/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Copyright (C) 1999-2001  Brian Paul   All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* $XFree86: xc/programs/glxgears/glxgears.c,v 1.4 2003/10/24 20:38:11 tsi Exp $ */

/*
 * This is a port of the infamous "gears" demo to straight GLX (i.e. no GLUT)
 * Port by Brian Paul  23 March 2001
 *
 * Command line options:
 *    -info      print GL implementation information
 *
 */

// Simplified, courtesy of Kevin Lange.

#define _USE_MATH_DEFINES
#include <cstdint>
#include <math.h>
#include <optional>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pedigree/log.h>
#include <sys/time.h>

#include <GL/gl.h>
#include <GL/osmesa.h>

#include "demo-app.h"

static GLfloat view_rotx = 20.0, view_roty = 30.0, view_rotz = 0.0;
static GLint gear1, gear2, gear3;
static GLfloat angle = 0.0;

static unsigned int frames = 0;
static unsigned int start_time = 0;

void fps()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    frames++;
    if (!start_time)
    {
        start_time = now.tv_sec;
    }
    else if ((now.tv_sec - start_time) >= 5)
    {
        GLfloat seconds = now.tv_sec - start_time;
        GLfloat fps = frames / seconds;
        pedigree_log(LOG_INFO, "%d frames in %3.1f seconds = %6.3f FPS\n", frames, seconds, fps);
        start_time = now.tv_sec;
        frames = 0;
    }
}

static void
gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
     GLint teeth, GLfloat tooth_depth)
{
    GLint i;
    GLfloat r0, r1, r2;
    GLfloat angle, da;
    GLfloat u, v, len;

    r0 = inner_radius;
    r1 = outer_radius - tooth_depth / 2.0;
    r2 = outer_radius + tooth_depth / 2.0;

    da = 2.0 * M_PI / teeth / 4.0;

    glShadeModel(GL_FLAT);

    glNormal3f(0.0, 0.0, 1.0);

    /* draw front face */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;
        glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
        if (i < teeth) {
            glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
            glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
            width * 0.5);
        }
    }
    glEnd();

    /* draw front sides of teeth */
    glBegin(GL_QUADS);
    da = 2.0 * M_PI / teeth / 4.0;
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;

        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
            width * 0.5);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
            width * 0.5);
    }
    glEnd();

    glNormal3f(0.0, 0.0, -1.0);

    /* draw back face */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
        if (i < teeth) {
            glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
                -width * 0.5);
            glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
        }
    }
    glEnd();

    /* draw back sides of teeth */
    glBegin(GL_QUADS);
    da = 2.0 * M_PI / teeth / 4.0;
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;

        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
        -width * 0.5);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
        -width * 0.5);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
    }
    glEnd();

    /* draw outward faces of teeth */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;

        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
        u = r2 * cos(angle + da) - r1 * cos(angle);
        v = r2 * sin(angle + da) - r1 * sin(angle);
        len = sqrt(u * u + v * v);
        u /= len;
        v /= len;
        glNormal3f(v, -u, 0.0);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5);
        glNormal3f(cos(angle), sin(angle), 0.0);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
            width * 0.5);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
            -width * 0.5);
        u = r1 * cos(angle + 3 * da) - r2 * cos(angle + 2 * da);
        v = r1 * sin(angle + 3 * da) - r2 * sin(angle + 2 * da);
        glNormal3f(v, -u, 0.0);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
            width * 0.5);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
            -width * 0.5);
        glNormal3f(cos(angle), sin(angle), 0.0);
    }

    glVertex3f(r1 * cos(0), r1 * sin(0), width * 0.5);
    glVertex3f(r1 * cos(0), r1 * sin(0), -width * 0.5);

    glEnd();

    glShadeModel(GL_SMOOTH);

    /* draw inside radius cylinder */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;
        glNormal3f(-cos(angle), -sin(angle), 0.0);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
    }
    glEnd();
}


static void
draw(void)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glPushMatrix();
    glRotatef(view_rotx, 1.0, 0.0, 0.0);
    glRotatef(view_roty, 0.0, 1.0, 0.0);
    glRotatef(view_rotz, 0.0, 0.0, 1.0);

    glPushMatrix();
    glTranslatef(-3.0, -2.0, 0.0);
    glRotatef(angle, 0.0, 0.0, 1.0);
    glCallList(gear1);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(3.1, -2.0, 0.0);
    glRotatef(-2.0 * angle - 9.0, 0.0, 0.0, 1.0);
    glCallList(gear2);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(-3.1, 4.2, 0.0);
    glRotatef(-2.0 * angle - 25.0, 0.0, 0.0, 1.0);
    glCallList(gear3);
    glPopMatrix();

    glPopMatrix();
}



/* new window size or exposure */
static void
reshape(int width, int height)
{
    GLfloat h = (GLfloat) height / (GLfloat) width;

    glViewport(0, 0, (GLint) width, (GLint) height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -h, h, 5.0, 60.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0, 0.0, -40.0);
}



static void
init(void)
{
    static GLfloat pos[4] = { 5.0, 5.0, 10.0, 0.0 };
    static GLfloat red[4] = { 0.8, 0.1, 0.0, 1.0 };
    static GLfloat green[4] = { 0.0, 0.8, 0.2, 1.0 };
    static GLfloat blue[4] = { 0.2, 0.2, 1.0, 1.0 };

    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);

    /* make the gears */
    gear1 = glGenLists(1);
    glNewList(gear1, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, red);
    gear(1.0, 4.0, 1.0, 20, 0.7);
    glEndList();

    gear2 = glGenLists(1);
    glNewList(gear2, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, green);
    gear(0.5, 2.0, 2.0, 10, 0.7);
    glEndList();

    gear3 = glGenLists(1);
    glNewList(gear3, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, blue);
    gear(1.3, 2.0, 0.5, 10, 0.7);
    glEndList();

    glEnable(GL_NORMALIZE);
}

class Gears {
 public:
  explicit Gears(std::uint32_t background)
      : m_Context(OSMesaCreateContext(OSMESA_BGRA, NULL)),
        m_Background(libui::unpackColor(background)) {}

  ~Gears() {
    if (m_Context) {
      OSMesaDestroyContext(m_Context);
    }
  }

  bool valid() const {
    return m_Context != NULL;
  }

  bool render(libui::client::PaintContext& paint) {
    cairo_surface_t* surface = cairo_get_target(paint.cairo());
    if (!surface || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE ||
        cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32) {
      fprintf(stderr, "gears requires an ARGB32 image surface.\n");
      return false;
    }

    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    if (width <= 0 || height <= 0 || stride <= 0 ||
        (stride % static_cast<int>(sizeof(std::uint32_t))) != 0) {
      fprintf(stderr, "gears received an invalid image surface.\n");
      return false;
    }

    cairo_surface_flush(surface);
    unsigned char* framebuffer = cairo_image_surface_get_data(surface);
    if (!framebuffer ||
        !OSMesaMakeCurrent(m_Context, framebuffer, GL_UNSIGNED_BYTE, width, height)) {
      fprintf(stderr, "OSMesaMakeCurrent failed.\n");
      return false;
    }

    // Cairo image rows can include padding beyond the visible width.
    OSMesaPixelStore(OSMESA_ROW_LENGTH, stride / static_cast<int>(sizeof(std::uint32_t)));
    OSMesaPixelStore(OSMESA_Y_UP, 0);

    if (width != m_Width || height != m_Height) {
      reshape(width, height);
      m_Width = width;
      m_Height = height;
    }
    if (!m_Initialized) {
      glClearColor(static_cast<GLfloat>(m_Background.r) / 255.0F,
                   static_cast<GLfloat>(m_Background.g) / 255.0F,
                   static_cast<GLfloat>(m_Background.b) / 255.0F,
                   static_cast<GLfloat>(m_Background.a) / 255.0F);
      init();
      m_Initialized = true;
    }

    angle += 0.2;
    draw();
    glFinish();
    cairo_surface_mark_dirty(surface);
    return true;
  }

  bool keyDown(int keycode) {
    switch (keycode) {
      case 'a':
      case 'A':
        view_roty += 5.0;
        return true;
      case 'd':
      case 'D':
        view_roty -= 5.0;
        return true;
      case 'w':
      case 'W':
        view_rotx += 5.0;
        return true;
      case 's':
      case 'S':
        view_rotx -= 5.0;
        return true;
      default:
        return false;
    }
  }

 private:
  OSMesaContext m_Context;
  libui::Color m_Background;
  bool m_Initialized = false;
  int m_Width = 0;
  int m_Height = 0;
};

int main(int argc, char** argv) {
  const std::optional<demo::Options> options = demo::parseOptions(argc, argv);
  if (!options) {
    return 2;
  }

  libui::client::Client client = libui::client::Client::connect(options->socketPath);
  if (!client.valid()) {
    fprintf(stderr, "gears could not connect to compositor.\n");
    return 1;
  }

  std::optional<libui::client::Window> createResult = demo::createWindow(client, *options);
  if (!createResult) {
    fprintf(stderr, "gears could not create a window.\n");
    return 1;
  }

  Gears gears(options->background);
  if (!gears.valid()) {
    fprintf(stderr, "gears could not create an OSMesa context.\n");
    return 1;
  }

  libui::client::Window& window = createResult.value();
  bool closed = false;
  bool renderFailed = false;
  window.setWindowProc([&gears, &closed, &renderFailed](libui::client::Window& window,
                                                        const libui::client::Event& event) {
    if (event.type() == libui::client::Event::Type::Close) {
      closed = true;
      return true;
    }
    if (event.type() == libui::client::Event::Type::Key) {
      const libui::client::KeyEvent* key = event.get<libui::client::KeyEvent>();
      return key && key->isDown && gears.keyDown(key->keycode);
    }
    if (event.type() == libui::client::Event::Type::Paint) {
      libui::client::PaintContext* paint = window.back();
      if (!paint || !gears.render(*paint)) {
        renderFailed = true;
        closed = true;
      }
      return true;
    }
    return false;
  });

  if (!client.setNonBlocking(true)) {
    return 1;
  }

  constexpr int MaxEventsPerFrame = 64;
  while (!closed && client.valid()) {
    for (int count = 0; count < MaxEventsPerFrame && !closed; ++count) {
      if (!client.dispatch(window, false, false)) {
        break;
      }
    }

    if (!closed && !client.paint(window)) {
      renderFailed = true;
      break;
    }
    fps();
    sched_yield();
  }

  return (renderFailed || (!closed && !client.valid())) ? 1 : 0;
}
