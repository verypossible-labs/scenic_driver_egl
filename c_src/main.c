#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "drm-common.h"

#define NANOVG_GLES2_IMPLEMENTATION
#include "nanovg/nanovg.h"
#include "nanovg/nanovg_gl.h"

#include "comms.h"
#include "render_script.h"
#include "types.h"
#include "utils.h"

#define STDIN_FILENO 0
#define DEFAULT_SCREEN 0
#define MSG_OUT_PUTS 0x02

typedef struct
{
  struct egl  egl;
  int         screen_width;
  int         screen_height;
  NVGcontext* p_ctx;
} egl_data_t;

static const struct egl* egl;
static const struct gbm* gbm;
static const struct drm* drm;

void init_display(egl_data_t* p_data, int debug_mode)
{
  p_data->screen_width  = gbm->width;
  p_data->screen_height = gbm->height;

  //-----------------------------------
  // get an EGL display connection
  EGLBoolean result;

  init_egl(&p_data->egl, gbm, 0);

  //-------------------
  // initialize nanovg

  p_data->p_ctx =
      nvgCreateGLES2(NVG_ANTIALIAS | NVG_STENCIL_STROKES | NVG_DEBUG);
  if (p_data->p_ctx == NULL)
  {
    send_puts("EGL driver error: failed nvgCreateGLES2");
    return;
  }
}

void test_draw(egl_data_t* p_data)
{
  //-----------------------------------
  // Set background color and clear buffers
  // glClearColor(0.15f, 0.25f, 0.35f, 1.0f);
  // glClearColor(0.098f, 0.098f, 0.439f, 1.0f);    // midnight blue
  // glClearColor(0.545f, 0.000f, 0.000f, 1.0f);    // dark red
  // glClearColor(0.184f, 0.310f, 0.310f, 1.0f);       // dark slate gray
  // glClearColor(0.0f, 0.0f, 0.0f, 1.0f);       // black

  // glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  NVGcontext* p_ctx         = p_data->p_ctx;
  int         screen_width  = p_data->screen_width;
  int         screen_height = p_data->screen_height;

  // nvgBeginFrame(p_ctx, screen_width, screen_height, 1.0f);

  // Next, draw graph line
  nvgBeginPath(p_ctx);
  nvgMoveTo(p_ctx, 0, 0);
  nvgLineTo(p_ctx, screen_width, screen_height);
  nvgStrokeColor(p_ctx, nvgRGBA(0, 160, 192, 255));
  nvgStrokeWidth(p_ctx, 3.0f);
  nvgStroke(p_ctx);

  nvgBeginPath(p_ctx);
  nvgMoveTo(p_ctx, screen_width, 0);
  nvgLineTo(p_ctx, 0, screen_height);
  nvgStrokeColor(p_ctx, nvgRGBA(0, 160, 192, 255));
  nvgStrokeWidth(p_ctx, 3.0f);
  nvgStroke(p_ctx);

  nvgBeginPath(p_ctx);
  nvgCircle(p_ctx, screen_width / 2, screen_height / 2, 50);
  nvgFillColor(p_ctx, nvgRGBAf(0.545f, 0.000f, 0.000f, 1.0f));
  nvgFill(p_data->p_ctx);
  nvgStroke(p_ctx);

  // nvgEndFrame(p_ctx);

  // eglSwapBuffers(p_data->display, p_data->surface);
}

//=============================================================================
// main setup

//---------------------------------------------------------

bool isCallerDown()
{
  struct pollfd ufd;
  memset(&ufd, 0, sizeof ufd);
  ufd.fd     = STDIN_FILENO;
  ufd.events = POLLIN;
  if (poll(&ufd, 1, 0) < 0)
    return true;
  return ufd.revents & POLLHUP;
}

//---------------------------------------------------------
int main(int argc, char** argv)
{
  driver_data_t data;
  egl_data_t    egl_data;

  uint32_t     format                         = DRM_FORMAT_XRGB8888;
  uint64_t     modifier                       = DRM_FORMAT_MOD_LINEAR;
  unsigned int vrefresh                       = 0;
  char         mode_str[DRM_DISPLAY_MODE_LEN] = "";
  const char*  device                         = NULL;
  unsigned int count                          = ~0;

  test_endian();

  // super simple arg check
  // if ( argc != 3 ) {
  //   send_puts("Argument check failed!");
  //   printf("\r\nscenic_driver_egl should be launched via the ScenicDriverEGL
  //   library.\r\n\r\n"); return 0;
  // }
  int num_scripts = atoi(argv[1]);
  int debug_mode  = atoi(argv[2]);

  // initialize

  drm = init_drm_legacy(device, mode_str, vrefresh, count);

  if (!drm)
  {
    printf("failed to initialize DRM\n");
    return -1;
  }

  gbm = init_gbm(drm->fd, drm->mode->hdisplay, drm->mode->vdisplay, format,
                 modifier);

  if (!gbm)
  {
    printf("failed to initialize GBM\n");
    return -1;
  }

  init_display(&egl_data, 0);

  printf("This worked! \n");

  // set up the scripts table
  memset(&data, 0, sizeof(driver_data_t));
  data.p_scripts = malloc(sizeof(void*) * num_scripts);
  memset(data.p_scripts, 0, sizeof(void*) * num_scripts);
  data.keep_going    = true;
  data.num_scripts   = num_scripts;
  data.p_ctx         = egl_data.p_ctx;
  data.screen_width  = egl_data.screen_width;
  data.screen_height = egl_data.screen_height;

  // signal the app that the window is ready
  send_ready(0, egl_data.screen_width, egl_data.screen_height);

  test_draw(&egl_data);

  /* Loop until the calling app closes the window */
  while (data.keep_going && !isCallerDown())
  {
    // check for incoming messages - blocks with a timeout
    if (handle_stdio_in(&data))
    {

      // clear the buffer
      glClear(GL_COLOR_BUFFER_BIT);

      // render the scene
      nvgBeginFrame(egl_data.p_ctx, egl_data.screen_width,
                    egl_data.screen_height, 1.0f);
      if (data.root_script >= 0)
      {
        run_script(data.root_script, &data);
      }
      nvgEndFrame(data.p_ctx);

      // Swap front and back buffers
      eglSwapBuffers(egl_data.egl.display, egl_data.egl.surface);
    }
  }
  return 0;
}
