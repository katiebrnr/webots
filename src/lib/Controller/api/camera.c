/*
 * Copyright 1996-2020 Cyberbotics Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../util/g_image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webots/camera.h>
#include <webots/robot.h>
#include "abstract_camera.h"
#include "messages.h"
#include "remote_control_private.h"
#include "robot_private.h"

typedef struct {
  double min_fov;
  double max_fov;
  double focal_length;
  double focal_distance;
  double min_focal_distance;
  double max_focal_distance;
  bool set_focal_distance;
  bool set_fov;
  bool has_recognition;
  bool enable_recognition;
  int recognition_sampling_period;
  int recognized_object_number;                   // number of object currrently recognized
  WbCameraRecognitionObject *recognized_objects;  // list of objects
} Camera;

static WbDevice *camera_get_device(WbDeviceTag t) {
  WbDevice *d = robot_get_device_with_node(t, WB_NODE_CAMERA, true);
  return d;
}

static AbstractCamera *camera_get_abstract_camera_struct(WbDeviceTag t) {
  WbDevice *d = camera_get_device(t);
  return d ? d->pdata : NULL;
}

static Camera *camera_get_struct(WbDeviceTag tag) {
  AbstractCamera *ac = camera_get_abstract_camera_struct(tag);
  return ac ? ac->pdata : NULL;
}

static void camera_clear_recognized_objects_list(Camera *c) {
  int i;
  for (i = 0; i < c->recognized_object_number; ++i) {
    free(c->recognized_objects[i].colors);
    free(c->recognized_objects[i].model);
  }
  free(c->recognized_objects);
}

static void wb_camera_cleanup(WbDevice *d) {
  AbstractCamera *ac = d->pdata;
  if (!ac)
    return;
  Camera *c = ac->pdata;
  if (!c)
    return;
  camera_clear_recognized_objects_list(c);
  free(c);
  ac->pdata = NULL;
  wb_abstract_camera_cleanup(d);
}

static void wb_camera_new(WbDevice *d, unsigned int id, int w, int h, double fov, double min_fov, double max_fov,
                          double focal_length, double focal_distance, double min_focal_distance, double max_focal_distance,
                          double camnear, bool spherical, bool has_recognition) {
  Camera *c;
  wb_camera_cleanup(d);
  wb_abstract_camera_new(d, id, w, h, fov, camnear, spherical);

  c = malloc(sizeof(Camera));
  c->min_fov = min_fov;
  c->max_fov = max_fov;
  c->focal_length = focal_length;
  c->focal_distance = focal_distance;
  c->min_focal_distance = min_focal_distance;
  c->max_focal_distance = max_focal_distance;
  c->has_recognition = has_recognition;
  c->set_focal_distance = false;
  c->set_fov = false;
  c->enable_recognition = false;
  c->recognition_sampling_period = 0;
  c->recognized_object_number = 0;
  c->recognized_objects = NULL;

  AbstractCamera *ac = d->pdata;
  ac->pdata = c;
}

static void wb_camera_write_request(WbDevice *d, WbRequest *r) {
  wb_abstract_camera_write_request(d, r);
  AbstractCamera *ac = d->pdata;
  Camera *c = ac->pdata;
  if (c->set_fov) {
    request_write_uchar(r, C_CAMERA_SET_FOV);
    request_write_double(r, ac->fov);
    c->set_fov = false;  // done
  }
  if (c->set_focal_distance) {
    request_write_uchar(r, C_CAMERA_SET_FOCAL);
    request_write_double(r, c->focal_distance);
    c->set_focal_distance = false;  // done
  }
  if (c->enable_recognition) {
    request_write_uchar(r, C_CAMERA_SET_RECOGNITION_SAMPLING_PERIOD);
    request_write_uint16(r, c->recognition_sampling_period);
    c->enable_recognition = false;  // done
  }
}

static void wb_camera_read_answer(WbDevice *d, WbRequest *r) {
  unsigned char command = request_read_uchar(r);
  if (wb_abstract_camera_handle_command(d, r, command))
    return;
  unsigned int uid;
  int width, height;
  double fov, min_fov, max_fov, camnear, focal_length, focal_distance, min_focal_distance, max_focal_distance;
  bool spherical, has_recognition;

  AbstractCamera *ac = d->pdata;
  Camera *c = NULL;

  switch (command) {
    case C_CONFIGURE:
      uid = request_read_uint32(r);
      width = request_read_uint16(r);
      height = request_read_uint16(r);
      fov = request_read_double(r);
      camnear = request_read_double(r);
      spherical = request_read_uchar(r);
      min_fov = request_read_double(r);
      max_fov = request_read_double(r);
      has_recognition = request_read_uchar(r) != 0;
      focal_length = request_read_double(r);
      focal_distance = request_read_double(r);
      min_focal_distance = request_read_double(r);
      max_focal_distance = request_read_double(r);

      // printf("new camera %u %d %d %lf %lf %d\n", uid, width, height, fov, camnear, spherical);
      wb_camera_new(d, uid, width, height, fov, min_fov, max_fov, focal_length, focal_distance, min_focal_distance,
                    max_focal_distance, camnear, spherical, has_recognition);
      break;
    case C_CAMERA_RECONFIGURE:
      c = ac->pdata;
      ac->fov = request_read_double(r);
      ac->camnear = request_read_double(r);
      ac->spherical = request_read_uchar(r);
      c->min_fov = request_read_double(r);
      c->max_fov = request_read_double(r);
      c->has_recognition = request_read_uchar(r) != 0;
      c->focal_length = request_read_double(r);
      c->focal_distance = request_read_double(r);
      c->min_focal_distance = request_read_double(r);
      c->max_focal_distance = request_read_double(r);
      break;
    case C_CAMERA_OBJECTS: {
      c = ac->pdata;
      int i, j;

      // clean previous list
      camera_clear_recognized_objects_list(c);
      // get number of recognized objects
      c->recognized_object_number = request_read_int32(r);
      c->recognized_objects =
        (WbCameraRecognitionObject *)malloc(c->recognized_object_number * sizeof(WbCameraRecognitionObject));

      for (i = 0; i < c->recognized_object_number; ++i) {
        // get id of the object
        c->recognized_objects[i].id = request_read_int32(r);
        // get relative position of the object
        c->recognized_objects[i].position[0] = request_read_double(r);
        c->recognized_objects[i].position[1] = request_read_double(r);
        c->recognized_objects[i].position[2] = request_read_double(r);
        // get relative orientation of the object
        c->recognized_objects[i].orientation[0] = request_read_double(r);
        c->recognized_objects[i].orientation[1] = request_read_double(r);
        c->recognized_objects[i].orientation[2] = request_read_double(r);
        c->recognized_objects[i].orientation[3] = request_read_double(r);
        // get size of the object
        c->recognized_objects[i].size[0] = request_read_double(r);
        c->recognized_objects[i].size[1] = request_read_double(r);
        // get position of the object on the camera image
        c->recognized_objects[i].position_on_image[0] = request_read_int32(r);
        c->recognized_objects[i].position_on_image[1] = request_read_int32(r);
        // get size of the object on the camera image
        c->recognized_objects[i].size_on_image[0] = request_read_int32(r);
        c->recognized_objects[i].size_on_image[1] = request_read_int32(r);
        // get number of colors of the object
        c->recognized_objects[i].number_of_colors = request_read_int32(r);
        const int size = 3 * c->recognized_objects[i].number_of_colors * sizeof(double *);
        c->recognized_objects[i].colors = (double *)malloc(size);
        for (j = 0; j < c->recognized_objects[i].number_of_colors; j++) {
          // get each color of the object
          c->recognized_objects[i].colors[3 * j] = request_read_double(r);
          c->recognized_objects[i].colors[3 * j + 1] = request_read_double(r);
          c->recognized_objects[i].colors[3 * j + 2] = request_read_double(r);
        }
        // get the model of the object
        c->recognized_objects[i].model = request_read_string(r);
      }
      break;
    }
    default:
      ROBOT_ASSERT(0);
      break;
  }
}

static void camera_toggle_remote(WbDevice *d, WbRequest *r) {
  abstract_camera_toggle_remote(d, r);
  AbstractCamera *ac = d->pdata;
  Camera *c = ac->pdata;
  if (ac->sampling_period != 0) {
    ac->enable = true;
    ac->image_requested = true;
    if (remote_control_is_function_defined("wbr_camera_set_fov"))
      c->set_fov = true;
    if (remote_control_is_function_defined("wbr_camera_set_focal_distance"))
      c->set_focal_distance = true;
  }
  if (c->recognition_sampling_period != 0)
    c->enable_recognition = true;
}

// Protected functions available from other source files

void wb_camera_init(WbDevice *d) {
  d->read_answer = wb_camera_read_answer;
  d->write_request = wb_camera_write_request;
  d->cleanup = wb_camera_cleanup;
  d->pdata = NULL;
  d->toggle_remote = camera_toggle_remote;
  // g_print("camera init done\n");
}

void wbr_camera_set_image(WbDeviceTag tag, const unsigned char *image) {
  WbDevice *d = camera_get_device(tag);
  if (!d)
    fprintf(stderr, "Error: wbr_camera_set_image(): invalid device tag.\n");
  wbr_abstract_camera_set_image(d, image);
}

unsigned char *wbr_camera_get_image_buffer(WbDeviceTag tag) {
  WbDevice *d = camera_get_device(tag);
  if (!d)
    fprintf(stderr, "Error: wbr_camera_get_image_buffer(): invalid device tag.\n");
  return wbr_abstract_camera_get_image_buffer(d);
}

void wbr_camera_recognition_set_object(WbDeviceTag tag, const WbCameraRecognitionObject *objects, int object_number) {
  Camera *c = camera_get_struct(tag);
  if (c) {
    camera_clear_recognized_objects_list(c);
    // get number of recognized objects
    c->recognized_object_number = object_number;
    c->recognized_objects =
      (WbCameraRecognitionObject *)malloc(c->recognized_object_number * sizeof(WbCameraRecognitionObject));
    int i, j;
    for (i = 0; i < c->recognized_object_number; ++i) {
      // set id of the object
      c->recognized_objects[i].id = objects[i].id;
      // set relative position of the object
      c->recognized_objects[i].position[0] = objects[i].position[0];
      c->recognized_objects[i].position[1] = objects[i].position[1];
      c->recognized_objects[i].position[2] = objects[i].position[2];
      // set relative orientation of the object
      c->recognized_objects[i].orientation[0] = objects[i].orientation[0];
      c->recognized_objects[i].orientation[1] = objects[i].orientation[1];
      c->recognized_objects[i].orientation[2] = objects[i].orientation[2];
      c->recognized_objects[i].orientation[3] = objects[i].orientation[3];
      // set size of the object
      c->recognized_objects[i].size[0] = objects[i].size[0];
      c->recognized_objects[i].size[1] = objects[i].size[1];
      // set position of the object on the camera image
      c->recognized_objects[i].position_on_image[0] = objects[i].position_on_image[0];
      c->recognized_objects[i].position_on_image[1] = objects[i].position_on_image[1];
      // set size of the object on the camera image
      c->recognized_objects[i].size_on_image[0] = objects[i].size_on_image[0];
      c->recognized_objects[i].size_on_image[1] = objects[i].size_on_image[1];
      // set number of colors of the object
      c->recognized_objects[i].number_of_colors = objects[i].number_of_colors;
      const int size = 3 * c->recognized_objects[i].number_of_colors * sizeof(double *);
      c->recognized_objects[i].colors = (double *)malloc(size);
      for (j = 0; j < c->recognized_objects[i].number_of_colors; j++) {
        // set each color of the object
        c->recognized_objects[i].colors[3 * j] = objects[i].colors[0];
        c->recognized_objects[i].colors[3 * j + 1] = objects[i].colors[1];
        c->recognized_objects[i].colors[3 * j + 2] = objects[i].colors[2];
      }
      // set the model of the object
      c->recognized_objects[i].model = (char *)malloc(sizeof(objects[i].model));
      strcpy(c->recognized_objects[i].model, objects[i].model);
    }
  } else
    fprintf(stderr, "Error: wbr_camera_recognition_set_object(): invalid device tag.\n");
}

// Public functions available from the camera API

void wb_camera_enable(WbDeviceTag tag, int sampling_period) {
  if (sampling_period < 0) {
    fprintf(stderr, "Error: wb_camera_enable() called with negative sampling period.\n");
    return;
  }

  WbDevice *d = camera_get_device(tag);
  if (!d)
    fprintf(stderr, "Error: wb_camera_enable(): invalid device tag.\n");

  wb_abstract_camera_enable(d, sampling_period);
}

void wb_camera_disable(WbDeviceTag tag) {
  Camera *c = camera_get_struct(tag);
  if (c)
    wb_camera_enable(tag, 0);
  else
    fprintf(stderr, "Error: wb_camera_disable(): invalid device tag.\n");
}

int wb_camera_get_sampling_period(WbDeviceTag tag) {
  WbDevice *d = camera_get_device(tag);
  if (!d)
    fprintf(stderr, "Error: wb_camera_get_sampling_period(): invalid device tag.\n");
  return wb_abstract_camera_get_sampling_period(d);
}

int wb_camera_get_height(WbDeviceTag tag) {
  WbDevice *d = camera_get_device(tag);
  if (!d)
    fprintf(stderr, "Error: wb_camera_get_height(): invalid device tag.\n");
  return wb_abstract_camera_get_height(d);
}

int wb_camera_get_width(WbDeviceTag tag) {
  WbDevice *d = camera_get_device(tag);
  if (!d)
    fprintf(stderr, "Error: wb_camera_get_width(): invalid device tag.\n");
  return wb_abstract_camera_get_width(d);
}

double wb_camera_get_fov(WbDeviceTag tag) {
  WbDevice *d = camera_get_device(tag);
  if (!d)
    fprintf(stderr, "Error: wb_camera_get_fov(): invalid device tag.\n");
  return wb_abstract_camera_get_fov(d);
}

double wb_camera_get_min_fov(WbDeviceTag tag) {
  double result = NAN;
  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);
  if (c)
    result = c->min_fov;
  else
    fprintf(stderr, "Error: wb_camera_get_min_fov(): invalid device tag.\n");
  robot_mutex_unlock_step();
  return result;
}

double wb_camera_get_max_fov(WbDeviceTag tag) {
  double result = NAN;
  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);
  if (c)
    result = c->max_fov;
  else
    fprintf(stderr, "Error: wb_camera_get_max_fov(): invalid device tag.\n");
  robot_mutex_unlock_step();
  return result;
}

void wb_camera_set_fov(WbDeviceTag tag, double fov) {
  bool in_range = true;
  robot_mutex_lock_step();
  AbstractCamera *ac = camera_get_abstract_camera_struct(tag);
  Camera *c = camera_get_struct(tag);
  if (!ac || !c) {
    fprintf(stderr, "Error: wb_camera_set_fov(): invalid device tag.\n");
    return;
  }
  if (ac->spherical && (fov < 0.0 || fov > 2.0 * M_PI)) {
    fprintf(stderr, "Error: wb_camera_set_fov called with 'fov' argument outside of the [0, 2.0*pi] range.\n");
    in_range = false;
  } else if (!ac->spherical && (fov < 0.0 || fov > M_PI)) {
    fprintf(stderr, "Error: wb_camera_set_fov called with 'fov' argument outside of the [0, pi] range.\n");
    in_range = false;
  } else if (fov < c->min_fov || fov > c->max_fov) {
    fprintf(stderr, "Error: wb_camera_set_fov out of zoom range [%f, %f].\n", c->min_fov, c->max_fov);
    in_range = false;
  }
  if (in_range) {
    ac->fov = fov;
    c->set_fov = true;
  }
  robot_mutex_unlock_step();
}

double wb_camera_get_focal_length(WbDeviceTag tag) {
  double result = NAN;
  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);
  if (c)
    result = c->focal_length;
  else
    fprintf(stderr, "Error: wb_camera_get_focal_length(): invalid device tag.\n");
  robot_mutex_unlock_step();
  return result;
}

double wb_camera_get_focal_distance(WbDeviceTag tag) {
  double result = NAN;
  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);
  if (c)
    result = c->focal_distance;
  else
    fprintf(stderr, "Error: wb_camera_get_focal_distance(): invalid device tag.\n");
  robot_mutex_unlock_step();
  return result;
}

double wb_camera_get_min_focal_distance(WbDeviceTag tag) {
  double result = NAN;
  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);
  if (c)
    result = c->min_focal_distance;
  else
    fprintf(stderr, "Error: wb_camera_get_min_focal_distance(): invalid device tag.\n");
  robot_mutex_unlock_step();
  return result;
}

double wb_camera_get_max_focal_distance(WbDeviceTag tag) {
  double result = NAN;
  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);
  if (c)
    result = c->max_focal_distance;
  else
    fprintf(stderr, "Error: wb_camera_get_max_focal_distance(): invalid device tag.\n");
  robot_mutex_unlock_step();
  return result;
}

void wb_camera_set_focal_distance(WbDeviceTag tag, double focal_distance) {
  bool in_range = true;
  robot_mutex_lock_step();
  AbstractCamera *ac = camera_get_abstract_camera_struct(tag);
  Camera *c = camera_get_struct(tag);
  if (!c || !ac) {
    fprintf(stderr, "Error: wb_camera_set_focal_distance(): invalid device tag.\n");
    return;
  } else if (ac->spherical) {
    fprintf(stderr, "Error: wb_camera_set_focal_distance() can't be called on a spherical camera.\n");
    in_range = false;
  } else if (focal_distance < c->min_focal_distance || focal_distance > c->max_focal_distance) {
    fprintf(stderr, "Error: wb_camera_set_focal_distance() out of focus range [%f, %f].\n", c->min_focal_distance,
            c->max_focal_distance);
    in_range = false;
  }
  if (in_range) {
    c->focal_distance = focal_distance;
    c->set_focal_distance = true;
  }
  robot_mutex_unlock_step();
}

double wb_camera_get_near(WbDeviceTag tag) {
  WbDevice *d = camera_get_device(tag);
  if (!d)
    fprintf(stderr, "Error: wb_camera_get_near(): invalid device tag.\n");

  return wb_abstract_camera_get_near(camera_get_device(tag));
}

void wb_camera_recognition_enable(WbDeviceTag tag, int sampling_period) {
  if (sampling_period < 0) {
    fprintf(stderr, "Error: wb_camera_recognition_enable() called with negative sampling period.\n");
    return;
  }

  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);

  if (c) {
    if (!c->has_recognition)
      fprintf(stderr, "Error: wb_camera_recognition_enable() called on a Camera without Recognition node.\n");
    else {
      c->enable_recognition = true;
      c->recognition_sampling_period = sampling_period;
    }
  } else
    fprintf(stderr, "Error: wb_camera_recognition_enable(): invalid device tag.\n");

  robot_mutex_unlock_step();
}

void wb_camera_recognition_disable(WbDeviceTag tag) {
  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);
  bool should_return = false;
  if (!c) {
    fprintf(stderr, "Error: wb_camera_recognition_disable(): invalid device tag.\n");
    should_return = true;
  } else if (!c->has_recognition) {
    fprintf(stderr, "Error: wb_camera_recognition_disable() called on a Camera without Recognition node.\n");
    should_return = true;
  }
  robot_mutex_unlock_step();
  if (!should_return)
    wb_camera_recognition_enable(tag, 0);
}

int wb_camera_recognition_get_sampling_period(WbDeviceTag tag) {
  int sampling_period = 0;
  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);
  if (c) {
    if (!c->has_recognition)
      fprintf(stderr, "Error: wb_camera_recognition_get_sampling_period() called on a Camera without Recognition node.\n");
    else
      sampling_period = c->recognition_sampling_period;
  } else
    fprintf(stderr, "Error: wb_camera_recognition_get_sampling_period(): invalid device tag.\n");
  robot_mutex_unlock_step();
  return sampling_period;
}

int wb_camera_recognition_get_number_of_objects(WbDeviceTag tag) {
  int result = 0;
  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);
  if (c) {
    if (!c->has_recognition)
      fprintf(stderr, "Error: wb_camera_recognition_get_number_of_objects() called on a Camera without Recognition node.\n");
    else if (c->recognition_sampling_period == 0)
      fprintf(stderr, "Error: wb_camera_recognition_get_number_of_objects() called for a disabled device! Please use: "
                      "wb_camera_recognition_enable().\n");
    else
      result = c->recognized_object_number;
  } else
    fprintf(stderr, "Error: wb_camera_recognition_get_number_of_objects(): invalid device tag.\n");
  robot_mutex_unlock_step();
  return result;
}

bool wb_camera_has_recognition(WbDeviceTag tag) {
  bool has_recognition = false;
  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);
  if (c)
    has_recognition = c->has_recognition;
  else
    fprintf(stderr, "Error: wb_camera_has_recognition(): invalid device tag.\n");
  robot_mutex_unlock_step();
  return has_recognition;
}

const WbCameraRecognitionObject *wb_camera_recognition_get_objects(WbDeviceTag tag) {
  const WbCameraRecognitionObject *result = 0;
  robot_mutex_lock_step();
  Camera *c = camera_get_struct(tag);
  if (c) {
    if (!c->has_recognition)
      fprintf(stderr, "Error: wb_camera_recognition_get_objects() called on a Camera without Recognition node.\n");
    else if (c->recognition_sampling_period == 0)
      fprintf(stderr, "Error: wb_camera_recognition_get_objects() called for a disabled device! Please use: "
                      "wb_camera_recognition_enable().\n");
    else
      result = c->recognized_objects;
  } else
    fprintf(stderr, "Error: wb_camera_recognition_get_objects(): invalid device tag.\n");
  robot_mutex_unlock_step();
  return result;
}

const unsigned char *wb_camera_get_image(WbDeviceTag tag) {
  AbstractCamera *ac = camera_get_abstract_camera_struct(tag);

  if (!ac) {
    fprintf(stderr, "Error: wb_camera_get_image(): invalid device tag.\n");
    return NULL;
  }

  if (ac->sampling_period <= 0) {
    fprintf(stderr, "Error: wb_camera_get_image() called for a disabled device! Please use: wb_camera_enable().\n");
    return NULL;
  }

  if (wb_robot_get_mode() == WB_MODE_REMOTE_CONTROL)
    return ac->image;

  robot_mutex_lock_step();
  bool success = abstract_camera_request_image(ac, "wb_camera_get_image");
  if (!ac->image || !success) {
    robot_mutex_unlock_step();
    return NULL;
  }
  robot_mutex_unlock_step();
  return ac->image;
}

int wb_camera_save_image(WbDeviceTag tag, const char *filename, int quality) {
  if (!filename || !filename[0]) {
    fprintf(stderr, "Error: wb_camera_save_image() called with NULL or empty 'filename' argument.\n");
    return -1;
  }
  unsigned char type = g_image_get_type(filename);
  if (type != G_IMAGE_PNG && type != G_IMAGE_JPEG) {
    fprintf(stderr, "Error: wb_camera_save_image() called with unsupported image format (should be PNG or JPEG).\n");
    return -1;
  }
  if (type == G_IMAGE_JPEG && (quality < 1 || quality > 100)) {
    fprintf(stderr, "Error: wb_camera_save_image() called with invalid 'quality' argument.\n");
    return -1;
  }

  robot_mutex_lock_step();
  AbstractCamera *ac = camera_get_abstract_camera_struct(tag);

  if (!ac) {
    fprintf(stderr, "Error: wb_camera_save_image(): invalid device tag.\n");
    robot_mutex_unlock_step();
    return -1;
  }

  // make sure image is up to date before saving it
  if (!ac->image || !abstract_camera_request_image(ac, "wb_camera_save_image")) {
    robot_mutex_unlock_step();
    return -1;
  }
  GImage img;
  img.width = ac->width;
  img.height = ac->height;

  img.data_format = G_IMAGE_DATA_FORMAT_BGRA;
  img.data = ac->image;
  int ret = g_image_save(&img, filename, quality);

  robot_mutex_unlock_step();
  return ret;
}

const WbCameraRecognitionObject *wb_camera_recognition_get_object(WbDeviceTag tag, int index) {
  Camera *c = camera_get_struct(tag);
  if (!c) {
    fprintf(stderr, "Error: wb_camera_recognition_get_object(): invalid device tag.\n");
    return NULL;
  }
  return (wb_camera_recognition_get_objects(tag) + index);
}
