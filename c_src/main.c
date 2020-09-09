#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>

#include <GLES2/gl2.h>
#include <EGL/egl.h>

#define NANOVG_GLES2_IMPLEMENTATION
#include "nanovg/nanovg.h"
#include "nanovg/nanovg_gl.h"

#include "types.h"
#include "comms.h"
#include "render_script.h"
#include "utils.h"

#define STDIN_FILENO 0
#define DEFAULT_SCREEN 0
#define MSG_OUT_PUTS 0x02
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define MAX_DISPLAYS 	(4)

uint8_t DISP_ID = 0;
uint8_t all_display = 0;
int8_t connector_id = -1;
char* device = "/dev/dri/card0";

typedef struct
{
  EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
  int         screen_width;
  int         screen_height;
  NVGcontext* p_ctx;
} egl_data_t;

static struct {
	struct gbm_device *dev;
	struct gbm_surface *surface;
} gbm;

static struct {
	int fd;
	uint32_t ndisp;
	uint32_t crtc_id[MAX_DISPLAYS];
	uint32_t connector_id[MAX_DISPLAYS];
	uint32_t resource_id;
	uint32_t encoder[MAX_DISPLAYS];
	uint32_t format[MAX_DISPLAYS];
	drmModeModeInfo *mode[MAX_DISPLAYS];
	drmModeConnector *connectors[MAX_DISPLAYS];
} drm;

struct drm_fb {
	struct gbm_bo *bo;
	uint32_t fb_id;
};

static uint32_t drm_fmt_to_gbm_fmt(uint32_t fmt)
{
	switch (fmt) {
		case DRM_FORMAT_XRGB8888:
			return GBM_FORMAT_XRGB8888;
		case DRM_FORMAT_ARGB8888:
			return GBM_FORMAT_ARGB8888;
		case DRM_FORMAT_RGB565:
			return GBM_FORMAT_RGB565;
		default:
			fprintf(stderr, "Unsupported DRM format: 0x%x", fmt);
			return GBM_FORMAT_XRGB8888;
	}
}

static bool search_plane_format(uint32_t desired_format, int formats_count, uint32_t* formats)
{
	int i;

	for ( i = 0; i < formats_count; i++)
	{
		if (desired_format == formats[i])
			return true;
	}

	return false;
}

int get_drm_prop_val(int fd, drmModeObjectPropertiesPtr props,
	                 const char *name, unsigned int *p_val) {
	drmModePropertyPtr p;
	unsigned int i, prop_id = 0; /* Property ID should always be > 0 */

	for (i = 0; !prop_id && i < props->count_props; i++) {
		p = drmModeGetProperty(fd, props->props[i]);
		if (!strcmp(p->name, name)){
			prop_id = p->prop_id;
			break;
		}
		drmModeFreeProperty(p);
	}

	if (!prop_id) {
		fprintf(stderr, "Could not find %s property\n", name);
		return(-1);
	}

	drmModeFreeProperty(p);
	*p_val = props->prop_values[i];
	return 0;
}

static bool set_drm_format(void)
{
	/* desired DRM format in order */
	static const uint32_t drm_formats[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_RGB565};
	drmModePlaneRes *plane_res;
	bool found = false;
	int i,k;

	drmSetClientCap(drm.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	plane_res  = drmModeGetPlaneResources(drm.fd);

	if (!plane_res) {
		fprintf(stderr, "drmModeGetPlaneResources failed: %s\n", strerror(errno));
		drmSetClientCap(drm.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 0);
		return false;
	}

	/*
	 * find the plane connected to crtc_id (the primary plane) and then find the desired pixel format
	 * from the plane format list
	 */
	for (i = 0; i < plane_res->count_planes; i++)
	{
		drmModePlane *plane = drmModeGetPlane(drm.fd, plane_res->planes[i]);
		drmModeObjectProperties *props;
		unsigned int plane_type;

		if(plane == NULL)
			continue;

		props = drmModeObjectGetProperties(drm.fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);

		if(props == NULL){
			fprintf(stderr, "plane (%d) properties not found\n",  plane->plane_id);
			drmModeFreePlane(plane);
			continue;
		}

		if(get_drm_prop_val(drm.fd, props, "type",  &plane_type) < 0)
		{
			fprintf(stderr, "plane (%d) type value not found\n",  plane->plane_id);
			drmModeFreeObjectProperties(props);
			drmModeFreePlane(plane);
			continue;
		}

		if (plane_type != DRM_PLANE_TYPE_PRIMARY)
		{
			drmModeFreeObjectProperties(props);
			drmModeFreePlane(plane);
			continue;
		}
		else if (!plane->crtc_id)
		{
			plane->crtc_id = drm.crtc_id[drm.ndisp];
		}

		drmModeFreeObjectProperties(props);

		if (plane->crtc_id == drm.crtc_id[drm.ndisp])
		{
			for (k = 0; k < ARRAY_SIZE(drm_formats); k++)
			{
				if (search_plane_format(drm_formats[k], plane->count_formats, plane->formats))
				{
					drm.format[drm.ndisp] = drm_formats[k];
					drmModeFreePlane(plane);
					drmModeFreePlaneResources(plane_res);
					drmSetClientCap(drm.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 0);
					return true;
				}
			}
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);
	drmSetClientCap(drm.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 0);
	return false;
}

static int init_drm(egl_data_t* p_data)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
	drmModeCrtc *crtc = NULL;

	int i, j, k;
	uint32_t maxRes, curRes;

	/* Open default dri device */
	drm.fd = open(device, O_RDWR | O_CLOEXEC);
	if (drm.fd < 0) {
		fprintf(stderr, "could not open drm device %s\n", device);
		return -1;
	}

	resources = drmModeGetResources(drm.fd);
	if (!resources) {
		fprintf(stderr, "drmModeGetResources failed: %s\n", strerror(errno));
		return -1;
	}
	drm.resource_id = (uint32_t) resources;

	/* find a connected connector: */
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {

			/* find the matched encoders */
			for (j=0; j<connector->count_encoders; j++) {
				encoder = drmModeGetEncoder(drm.fd, connector->encoders[j]);

				/* Take the fisrt one, if none is assigned */
				if (!connector->encoder_id)
				{
					connector->encoder_id = encoder->encoder_id;
				}

				if (encoder->encoder_id == connector->encoder_id)
				{
					/* find the first valid CRTC if not assigned */
					if (!encoder->crtc_id)
					{
						for (k = 0; k < resources->count_crtcs; ++k) {
							/* check whether this CRTC works with the encoder */
							if (!(encoder->possible_crtcs & (1 << k)))
								continue;

							encoder->crtc_id = resources->crtcs[k];
							break;
						}

						if (!encoder->crtc_id)
						{
							fprintf(stderr, "Encoder(%d): no CRTC find!\n", encoder->encoder_id);
							drmModeFreeEncoder(encoder);
							encoder = NULL;
							continue;
						}
					}

					break;
				}

				drmModeFreeEncoder(encoder);
				encoder = NULL;
			}

			if (!encoder) {
				fprintf(stderr, "Connector (%d): no encoder!\n", connector->connector_id);
				drmModeFreeConnector(connector);
				continue;
			}

			/* choose the current or first supported mode */
			crtc = drmModeGetCrtc(drm.fd, encoder->crtc_id);
			for (j = 0; j < connector->count_modes; j++)
			{
				if (crtc->mode_valid)
				{
					if ((connector->modes[j].hdisplay == crtc->width) &&
					(connector->modes[j].vdisplay == crtc->height))
					{
						drm.mode[drm.ndisp] = &connector->modes[j];
						break;
					}
				}
				else
				{
					if ((connector->modes[j].hdisplay == crtc->x) &&
					   (connector->modes[j].vdisplay == crtc->y))
					{
						drm.mode[drm.ndisp] = &connector->modes[j];
						break;
					}
				}
			}

			if(j >= connector->count_modes)
				drm.mode[drm.ndisp] = &connector->modes[0];

			drm.connector_id[drm.ndisp] = connector->connector_id;

			drm.encoder[drm.ndisp]  = (uint32_t) encoder;
			drm.crtc_id[drm.ndisp] = encoder->crtc_id;
			drm.connectors[drm.ndisp] = connector;

			if (!set_drm_format())
			{
				// Error handling
				fprintf(stderr, "No desired pixel format found!\n");
				return -1;
			}

			fprintf(stderr, "### Display [%d]: CRTC = %d, Connector = %d, format = 0x%x\n", drm.ndisp, drm.crtc_id[drm.ndisp], drm.connector_id[drm.ndisp], drm.format[drm.ndisp]);
			fprintf(stderr, "\tMode chosen [%s] : Clock => %d, Vertical refresh => %d, Type => %d\n", drm.mode[drm.ndisp]->name, drm.mode[drm.ndisp]->clock, drm.mode[drm.ndisp]->vrefresh, drm.mode[drm.ndisp]->type);
			fprintf(stderr, "\tHorizontal => %d, %d, %d, %d, %d\n", drm.mode[drm.ndisp]->hdisplay, drm.mode[drm.ndisp]->hsync_start, drm.mode[drm.ndisp]->hsync_end, drm.mode[drm.ndisp]->htotal, drm.mode[drm.ndisp]->hskew);
			fprintf(stderr, "\tVertical => %d, %d, %d, %d, %d\n", drm.mode[drm.ndisp]->vdisplay, drm.mode[drm.ndisp]->vsync_start, drm.mode[drm.ndisp]->vsync_end, drm.mode[drm.ndisp]->vtotal, drm.mode[drm.ndisp]->vscan);
      p_data->screen_height = drm.mode[drm.ndisp]->vdisplay;
      p_data->screen_width = drm.mode[drm.ndisp]->hdisplay;
			/* If a connector_id is specified, use the corresponding display */
			if ((connector_id != -1) && (connector_id == drm.connector_id[drm.ndisp]))
				DISP_ID = drm.ndisp;

			/* If all displays are enabled, choose the connector with maximum
			* resolution as the primary display */
			if (all_display) {
				maxRes = drm.mode[DISP_ID]->vdisplay * drm.mode[DISP_ID]->hdisplay;
				curRes = drm.mode[drm.ndisp]->vdisplay * drm.mode[drm.ndisp]->hdisplay;

				if (curRes > maxRes)
					DISP_ID = drm.ndisp;
			}

			drm.ndisp++;
		} else {
			drmModeFreeConnector(connector);
		}
	}

	if (drm.ndisp == 0) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		fprintf(stderr, "no connected connector!\n");
		return -1;
	}

	return 0;
}

static int init_gbm(void)
{
	gbm.dev = gbm_create_device(drm.fd);

	gbm.surface = gbm_surface_create(gbm.dev,
			drm.mode[DISP_ID]->hdisplay, drm.mode[DISP_ID]->vdisplay,
			drm_fmt_to_gbm_fmt(drm.format[DISP_ID]),
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!gbm.surface) {
		fprintf(stderr, "failed to create gbm surface\n");
		return -1;
	}

	return 0;
}

static int init_egl(egl_data_t* p_data)
{
  EGLint major, minor, n;
	GLint ret;

  static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	static const EGLint config_attribs[] = {
		EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_STENCIL_SIZE, 1,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
	};

  p_data->display = eglGetDisplay(gbm.dev);
  fprintf(stderr, "Here 1\n");
	if (!eglInitialize(p_data->display, &major, &minor)) {
		fprintf(stderr, "failed to initialize\n");
		return -1;
	}

	fprintf(stderr, "Using display %p with EGL version %d.%d\n", p_data->display, major, minor);

	fprintf(stderr, "EGL Version \"%s\"\n", eglQueryString(p_data->display, EGL_VERSION));
	fprintf(stderr, "EGL Vendor \"%s\"\n", eglQueryString(p_data->display, EGL_VENDOR));
	fprintf(stderr, "EGL Extensions \"%s\"\n", eglQueryString(p_data->display, EGL_EXTENSIONS));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		fprintf(stderr, "failed to bind api EGL_OPENGL_ES_API\n");
		return -1;
	}

	if (!eglChooseConfig(p_data->display, config_attribs, &p_data->config, 1, &n) || n != 1) {
		fprintf(stderr, "failed to choose config: %d\n", n);
		return -1;
	}
fprintf(stderr, "Here 2\n");
	p_data->context = eglCreateContext(p_data->display, p_data->config,
			EGL_NO_CONTEXT, context_attribs);
	if (p_data->context == NULL) {
		fprintf(stderr, "failed to create context\n");
		return -1;
	}

	p_data->surface = eglCreateWindowSurface(p_data->display, p_data->config, gbm.surface, NULL);
	if (p_data->surface == EGL_NO_SURFACE) {
		fprintf(stderr, "failed to create egl surface\n");
		return -1;
	}

	/* connect the context to the surface */
	eglMakeCurrent(p_data->display, p_data->surface, p_data->surface, p_data->context);

  fprintf(stderr, "connected surface\n");
  //-------------------
  // config gles

  // set the view port to the new size passed in
  glViewport(0, 0, p_data->screen_width, p_data->screen_height);

  // This turns on/off depth test.
  // With this ON, whatever we draw FIRST is
  // "on top" and each subsequent draw is BELOW
  // the draw calls before it.
  // With this OFF, whatever we draw LAST is
  // "on top" and each subsequent draw is ABOVE
  // the draw calls before it.
  glDisable(GL_DEPTH_TEST);

  // Probably need this on, enables Gouraud Shading
  // glShadeModel(GL_SMOOTH);

  // Turn on Alpha Blending
  // There are some efficiencies to be gained by ONLY
  // turning this on when we have a primitive with a
  // style that has an alpha channel != 1.0f but we
  // don't have code to detect that.  Easy to do if we need it!
  glEnable (GL_BLEND);
  glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  fprintf(stderr, "configured gles\n");
  p_data->p_ctx =
      nvgCreateGLES2(NVG_ANTIALIAS | NVG_STENCIL_STROKES | NVG_DEBUG);
  if (p_data->p_ctx == NULL)
  {
    fprintf(stderr, "Failed to create nvg\n");
    send_puts("EGL driver error: failed nvgCreateGLES2");
    return -1;
  }

  return 0;
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

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;
	struct gbm_device *gbm = gbm_bo_get_device(bo);

	if (fb->fb_id)
		drmModeRmFB(drm.fd, fb->fb_id);

	free(fb);
}

static struct drm_fb * drm_fb_get_from_bo(struct gbm_bo *bo)
{
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
	uint32_t width, height, format;
	uint32_t bo_handles[4] = {0}, offsets[4] = {0}, pitches[4] = {0};
	int ret;

	if (fb)
		return fb;

	fb = calloc(1, sizeof *fb);
	fb->bo = bo;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	pitches[0] = gbm_bo_get_stride(bo);
	bo_handles[0] = gbm_bo_get_handle(bo).u32;
	format = gbm_bo_get_format(bo);

	ret = drmModeAddFB2(drm.fd, width, height, format, bo_handles, pitches, offsets, &fb->fb_id, 0);
	if (ret) {
		printf("failed to create fb: %s\n", strerror(errno));
		free(fb);
		return NULL;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

	return fb;
}

static void page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	int *waiting_for_flip = data;
	*waiting_for_flip = *waiting_for_flip - 1;
}

//---------------------------------------------------------
int main(int argc, char** argv)
{
  driver_data_t data;
  egl_data_t    egl_data;

  fd_set fds;
  drmEventContext evctx = {
			.version = DRM_EVENT_CONTEXT_VERSION,
			.page_flip_handler = page_flip_handler,
	};

  int ret;
  struct gbm_bo *bo;
	struct drm_fb *fb;

  test_endian();

  // super simple arg check
  if ( argc != 3 ) {
    send_puts("Argument check failed!");
    fprintf(stderr, "\r\nscenic_driver_egl should be launched via the ScenicDriverEGL library.\r\n\r\n");
    return 0;
  }
  int num_scripts = atoi(argv[1]);
  int debug_mode  = atoi(argv[2]);

  // initialize
  ret = init_drm(&egl_data);
	if (ret) {
		fprintf(stderr, "failed to initialize DRM\n");
		return ret;
	}

  fprintf(stderr, "### Primary display => ConnectorId = %d, Resolution = %dx%d\n",
			drm.connector_id[DISP_ID], drm.mode[DISP_ID]->hdisplay,
  		drm.mode[DISP_ID]->vdisplay);

	FD_ZERO(&fds);
	FD_SET(drm.fd, &fds);

  ret = init_gbm();
	if (ret) {
		fprintf(stderr, "failed to initialize GBM\n");
		return ret;
	}

  ret = init_egl(&egl_data);
	if (ret) {
		fprintf(stderr, "failed to initialize EGL\n");
		return ret;
	}

  // set up the scripts table
  memset(&data, 0, sizeof(driver_data_t));
  data.p_scripts = malloc(sizeof(void*) * num_scripts);
  memset(data.p_scripts, 0, sizeof(void*) * num_scripts);
  data.keep_going    = true;
  data.num_scripts   = num_scripts;
  data.p_ctx         = egl_data.p_ctx;
  data.screen_width  = egl_data.screen_width;
  data.screen_height = egl_data.screen_height;

  // test_draw(&egl_data);
  // glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	eglSwapBuffers(egl_data.display, egl_data.surface);
	bo = gbm_surface_lock_front_buffer(gbm.surface);
	fb = drm_fb_get_from_bo(bo);

  ret = drmModeSetCrtc(drm.fd, drm.crtc_id[DISP_ID], fb->fb_id,
				0, 0, &drm.connector_id[DISP_ID], 1, drm.mode[DISP_ID]);
		if (ret) {
			printf("display %d failed to set mode: %s\n", DISP_ID, strerror(errno));
			return ret;
		}

  // signal the app that the window is ready
  send_ready(0, egl_data.screen_width, egl_data.screen_height);

  /* Loop until the calling app closes the window */
  while (data.keep_going && !isCallerDown())
  {
    struct gbm_bo *next_bo;
		int waiting_for_flip;
		int cc;

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
      eglSwapBuffers(egl_data.display, egl_data.surface);

      next_bo = gbm_surface_lock_front_buffer(gbm.surface);
		  fb = drm_fb_get_from_bo(next_bo);

      ret = drmModePageFlip(drm.fd, drm.crtc_id[DISP_ID], fb->fb_id,
          DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
      if (ret) {
        fprintf(stderr, "failed to queue page flip: %s\n", strerror(errno));
        return -1;
      }
      waiting_for_flip = 1;

      while (waiting_for_flip) {
        ret = select(drm.fd + 1, &fds, NULL, NULL, NULL);
        if (ret < 0) {
          fprintf(stderr, "select err: %s\n", strerror(errno));
          return ret;
        } else if (ret == 0) {
          fprintf(stderr, "select timeout!\n");
          return -1;
        } else if (FD_ISSET(0, &fds)) {
          continue;
        }
        drmHandleEvent(drm.fd, &evctx);
      }

      /* release last buffer to render on again: */
      gbm_surface_release_buffer(gbm.surface, bo);
      bo = next_bo;
    }
  }
  return 0;
}
