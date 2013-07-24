/*
Copyright (c) 2013, Broadcom Europe Ltd
Copyright (c) 2013, James Hughes
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 * \file RaspiStill.c
 * Command line program to capture a still frame and encode it to file.
 * Also optionally display a preview/viewfinder of current camera input.
 *
 * \date 31 Jan 2013
 * \Author: James Hughes
 *
 * Description
 *
 * 3 components are created; camera, preview and JPG encoder.
 * Camera component has three ports, preview, video and stills.
 * This program connects preview and stills to the preview and jpg
 * encoder. Using mmal we don't need to worry about buffers between these
 * components, but we do need to handle buffers from the encoder, which
 * are simply written straight to the file in the requisite buffer callback.
 *
 * We use the RaspiCamControl code to handle the specific camera settings.
 */

// We use some GNU extensions (asprintf, basename)
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>

#define VERSION_STRING "v1.2"

#include "bcm_host.h"
#include "interface/vcos/vcos.h"

//*** PR : ADDED for OpenCV
#include <cv.h>
#include <highgui.h>

#include <math.h>

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"


#include "RaspiCamControl.h"
#include "RaspiPreview.h"
#include "RaspiCLI.h"

#include <semaphore.h>

/// Camera number to use - we only have one camera, indexed from 0.
#define CAMERA_NUMBER 0

// Standard port setting for the camera component
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2


// Stills format information
#define STILLS_FRAME_RATE_NUM 3
#define STILLS_FRAME_RATE_DEN 1

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3

#define MAX_USER_EXIF_TAGS      32
#define MAX_EXIF_PAYLOAD_LENGTH 128

int mmal_status_to_int(MMAL_STATUS_T status);

// REDEFINE
int potato;

// Points of the octagon
struct Point point1, point2, point3, point4, point5, point6, point7, point8;

// Radius for the circles
int radius = 7;

// Structure to represent a point.
struct Point
{
	int height;
	int width;
	int isActive;
}; // Point

/** Structure containing all state information for the current run
 */
typedef struct
{
   int timeout;                        /// Time taken before frame is grabbed and app then shuts down. Units are milliseconds
   int width;                          /// Requested width of image
   int height;                         /// requested height of image
   int quality;                        /// JPEG quality setting (1-100)
   int wantRAW;                        /// Flag for whether the JPEG metadata also contains the RAW bayer image
   char *filename;                     /// filename of output file
   MMAL_PARAM_THUMBNAIL_CONFIG_T thumbnailConfig;
   int verbose;                        /// !0 if want detailed run information
   int demoMode;                       /// Run app in demo mode
   int demoInterval;                   /// Interval between camera settings changes
   MMAL_FOURCC_T encoding;             /// Encoding to use for the output file.
   const char *exifTags[MAX_USER_EXIF_TAGS]; /// Array of pointers to tags supplied from the command line
   int numExifTags;                    /// Number of supplied tags
   int timelapse;                      /// Delay between each picture in timelapse mode. If 0, disable timelapse

   RASPIPREVIEW_PARAMETERS preview_parameters;    /// Preview setup parameters
   RASPICAM_CAMERA_PARAMETERS camera_parameters; /// Camera setup parameters

   MMAL_COMPONENT_T *camera_component;    /// Pointer to the camera component
   MMAL_COMPONENT_T *encoder_component;   /// Pointer to the encoder component
   MMAL_CONNECTION_T *preview_connection; /// Pointer to the connection from camera to preview
   MMAL_CONNECTION_T *encoder_connection; /// Pointer to the connection from camera to encoder

   MMAL_POOL_T *encoder_pool; /// Pointer to the pool of buffers used by encoder output port

} RASPISTILL_STATE;

/** Struct used to pass information in encoder port userdata to callback
 */
typedef struct
{
   FILE *file_handle;                   /// File handle to write buffer data to.
   VCOS_SEMAPHORE_T complete_semaphore; /// semaphore which is posted when we reach end of frame (indicates end of capture or fault)
   RASPISTILL_STATE *pstate;            /// pointer to our state in case required in callback
} PORT_USERDATA;

/// Comamnd ID's and Structure defining our command line options
#define CommandHelp         0
#define CommandWidth        1
#define CommandHeight       2
#define CommandQuality      3
#define CommandRaw          4
#define CommandOutput       5
#define CommandVerbose      6
#define CommandTimeout      7
#define CommandThumbnail    8
#define CommandDemoMode     9
#define CommandEncoding     10
#define CommandExifTag      11
#define CommandTimelapse    12

static COMMAND_LIST cmdline_commands[] =
{
   { CommandHelp,    "-help",       "?",  "This help information", 0 },
   { CommandWidth,   "-width",      "w",  "Set image width <size>", 1 },
   { CommandHeight,  "-height",     "h",  "Set image height <size>", 1 },
   { CommandQuality, "-quality",    "q",  "Set jpeg quality <0 to 100>", 1 },
   { CommandRaw,     "-raw",        "r",  "Add raw bayer data to jpeg metadata", 0 },
   { CommandOutput,  "-output",     "o",  "Output filename <filename> (to write to stdout, use '-o -'). If not specified, no file is saved", 1 },
   { CommandVerbose, "-verbose",    "v",  "Output verbose information during run", 0 },
   { CommandTimeout, "-timeout",    "t",  "Time (in ms) before takes picture and shuts down (if not specified, set to 5s)", 1 },
   { CommandThumbnail,"-thumb",     "th", "Set thumbnail parameters (x:y:quality)", 1},
   { CommandDemoMode,"-demo",       "d",  "Run a demo mode (cycle through range of camera options, no capture)", 0},
   { CommandEncoding,"-encoding",   "e",  "Encoding to use for output file (jpg, bmp, gif, png)", 1},
   { CommandExifTag, "-exif",       "x",  "EXIF tag to apply to captures (format as 'key=value')", 1},
   { CommandTimelapse,"-timelapse", "tl", "Timelapse mode. Takes a picture every <t>ms", 1}

};

static int cmdline_commands_size = sizeof(cmdline_commands) / sizeof(cmdline_commands[0]);

static struct
{
   char *format;
   MMAL_FOURCC_T encoding;
} encoding_xref[] =
{
   {"jpg", MMAL_ENCODING_JPEG},
   {"bmp", MMAL_ENCODING_BMP},
   {"gif", MMAL_ENCODING_GIF},
   {"png", MMAL_ENCODING_PNG}
};

static int encoding_xref_size = sizeof(encoding_xref) / sizeof(encoding_xref[0]);

// Initialise the octagon.
static void octagon(IplImage* image)
{
	int smallest; 
	
	// Calculate the smallest of the image parameters
	if (image->height < image->width)
		smallest = image->height;
	else
		smallest = image->width;

	// Distance from borders (in pixels)
	int t = 13;

	// Value to center the octagon (in pixels)
	int c = 20;

	// Calculate distance d from a corner to the centre of the square.
	int d = (smallest / 2 - t) * sqrt(2);

	// Set points as tuples and draw them as green circles.
	point1.height = smallest-t-d + c;
	point1.width = t;
	point1.isActive = 0;	
	cvCircle(image,cvPoint(point1.height, point1.width),radius,CV_RGB(0, 255, 0),1,8,0);

	point2.height = t+d + c;
	point2.width = t;
	point2.isActive = 0;	
	cvCircle(image,cvPoint(point2.height, point2.width),radius,CV_RGB(0, 255, 0),1,8,0);

	point3.height = smallest-t + c;
	point3.width = smallest-t-d;
	point3.isActive = 0;	
	cvCircle(image,cvPoint(point3.height, point3.width),radius,CV_RGB(0, 255, 0),1,8,0);

	point4.height = smallest-t + c;
	point4.width = t+d;
	point4.isActive = 0;
	cvCircle(image,cvPoint(point4.height, point4.width),radius,CV_RGB(0, 255, 0),1,8,0);

	point5.height = t+d+ c;
	point5.width = smallest-t;
	point5.isActive = 0;
	cvCircle(image,cvPoint(point5.height, point5.width),radius,CV_RGB(0, 255, 0),1,8,0);

	point6.height = smallest-t -d + c;
	point6.width = smallest-t;
	point6.isActive = 0;
	cvCircle(image,cvPoint(point6.height, point6.width),radius,CV_RGB(0, 255, 0),1,8,0);
	
	point7.height = t + c;
	point7.width = t+d;
	point7.isActive = 0;
	cvCircle(image,cvPoint(point7.height, point7.width),radius,CV_RGB(0, 255, 0),1,8,0);

	point8.height = t + c;
	point8.width = smallest-t-d;
	point8.isActive = 0;
	cvCircle(image,cvPoint(point8.height, point8.width),radius,CV_RGB(0, 255, 0),1,8,0);
} // octagon

// Check if a corner is in one of the 8 regions and, in that case, activate it.
static void active(int x, int y)
{
	if (point1.isActive == 0 && abs(x-point1.height) < radius && abs(y - point1.width) < radius)
		point1.isActive = 1;
	else if (point2.isActive == 0 && abs(x-point2.height) < radius && abs(y - point2.width) < radius)
		point2.isActive = 1;
	else if (point3.isActive == 0 && abs(x-point3.height) < radius && abs(y - point3.width) < radius)
		point3.isActive = 1;
	else if (point4.isActive == 0 && abs(x-point4.height) < radius && abs(y - point4.width) < radius)
		point4.isActive = 1;
	else if (point5.isActive == 0 && abs(x-point5.height) < radius && abs(y - point5.width) < radius)
		point5.isActive = 1;
	else if (point6.isActive == 0 && abs(x-point6.height) < radius && abs(y - point6.width) < radius)
		point6.isActive = 1;
	else if (point7.isActive == 0 && abs(x-point7.height) < radius && abs(y - point7.width) < radius)
		point7.isActive = 1;
	else if (point8.isActive == 0 && abs(x-point8.height) < radius && abs(y - point8.width) < radius)
		point8.isActive = 1;
} // active

// Change from green to blue the points that are active.
static void bluePoint(IplImage* image)
{
	if (point1.isActive == 1)
		cvCircle(image,cvPoint(point1.height, point1.width),radius,CV_RGB(255,0,0),1,8,0);
	if (point2.isActive == 1)
		cvCircle(image,cvPoint(point2.height, point2.width),radius,CV_RGB(255,0,0),1,8,0);
	if (point3.isActive == 1)
		cvCircle(image,cvPoint(point3.height, point3.width),radius,CV_RGB(255,0,0),1,8,0);
	if (point4.isActive == 1)
		cvCircle(image,cvPoint(point4.height, point4.width),radius,CV_RGB(255,0,0),1,8,0);
	if (point5.isActive == 1)
		cvCircle(image,cvPoint(point5.height, point5.width),radius,CV_RGB(255,0,0),1,8,0);
	if (point6.isActive == 1)
		cvCircle(image,cvPoint(point6.height, point6.width),radius,CV_RGB(255,0,0),1,8,0);
	if (point7.isActive == 1)
		cvCircle(image,cvPoint(point7.height, point7.width),radius,CV_RGB(255,0,0),1,8,0);
	if (point8.isActive == 1)
		cvCircle(image,cvPoint(point8.height, point8.width),radius,CV_RGB(255,0,0),1,8,0);
} // bluePoint

// Check which points are not activated and return the resulting direction
static char* direction()
{
	if (point1.isActive == 0)
	{
		if (point4.isActive == 0 && point2.isActive == 1 && point3.isActive == 1 
		    && point5.isActive == 1 && point6.isActive == 1 && point7.isActive == 1 
		    && point8.isActive == 1)
				return "Right_Back";
		else if (point4.isActive == 1 && point2.isActive == 1 && point3.isActive == 1 
				 && point5.isActive == 1 && point6.isActive == 0 && point7.isActive == 1 
				 && point8.isActive == 1)
					return "Left_Turn";
	} // if
	else if (point2.isActive == 0)
	{
		if (point4.isActive == 1 && point1.isActive == 1 && point3.isActive == 1 
		    && point5.isActive == 0 && point6.isActive == 1 && point7.isActive == 1 
		    && point8.isActive == 1)
				return "Right_Turn";
		else if (point4.isActive == 1 && point1.isActive == 1 && point3.isActive == 1 
				 && point5.isActive == 1 && point6.isActive == 1 && point7.isActive == 0 
				 && point8.isActive == 1)
					return "Left_Back";
	} // else if
	else if (point3.isActive == 0)
	{
		if (point4.isActive == 1 && point1.isActive == 1 && point2.isActive == 1 
		    && point5.isActive == 1 && point6.isActive == 0 && point7.isActive == 1 
		    && point8.isActive == 1)
				return "Right_Forw";
		else if (point4.isActive == 1 && point1.isActive == 1 && point2.isActive == 1 
				 && point5.isActive == 1 && point6.isActive == 1 && point7.isActive == 1 
				 && point8.isActive == 0)
					return "Back";
	} // else if
	else if (point4.isActive == 0)
	{
		if (point2.isActive == 1 && point1.isActive == 1 && point3.isActive == 1 
			&& point5.isActive == 1 && point6.isActive == 1 && point7.isActive == 0 
			&& point8.isActive == 1)
				return "Forw";
	} // else if
	else if (point5.isActive == 0)
	{
		if (point4.isActive == 1 && point1.isActive == 1 && point2.isActive == 1 
		    && point3.isActive == 1 && point6.isActive == 1 && point7.isActive == 1 
			&& point8.isActive == 0)
				return "Left_Forw"; 
	} // else if
} // direction

/**
 * Assign a default set of parameters to the state passed in
 *
 * @param state Pointer to state structure to assign defaults to
 */
static void default_status(RASPISTILL_STATE *state)
{
   if (!state)
   {
      vcos_assert(0);
      return;
   }
   
   // *** PR: modified for demo purpose -> smaller image	
   state->timeout = 1000;// 1s delay before take image
   state->width = 324;//2592;
   state->height = 243;//1944;
   state->quality = 25;
   state->wantRAW = 0;
   state->filename = "cam_image.jpg";
   state->verbose = 0;
   state->thumbnailConfig.enable = 1;
   state->thumbnailConfig.width = 64;
   state->thumbnailConfig.height = 48;
   state->thumbnailConfig.quality = 35;
   state->demoMode = 0;
   state->demoInterval = 250; // ms
   state->camera_component = NULL;
   state->encoder_component = NULL;
   state->preview_connection = NULL;
   state->encoder_connection = NULL;
   state->encoder_pool = NULL;
   state->encoding = MMAL_ENCODING_JPEG;
   state->numExifTags = 0;
   state->timelapse = 0;

   // Setup preview window defaults
   raspipreview_set_defaults(&state->preview_parameters);

   // Set up the camera_parameters to default
   raspicamcontrol_set_defaults(&state->camera_parameters);
}

/**
 *  buffer header callback function for camera control
 *
 *  No actions taken in current version
 *
 * @param port Pointer to port from which callback originated
 * @param buffer mmal buffer header pointer
 */
static void camera_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
   if (buffer->cmd == MMAL_EVENT_PARAMETER_CHANGED)
   {
   }
   else
   {
      vcos_log_error("Received unexpected camera control callback event, 0x%08x", buffer->cmd);
   }

   mmal_buffer_header_release(buffer);
}

/**
 *  buffer header callback function for encoder
 *
 *  Callback will dump buffer data to the specific file
 *
 * @param port Pointer to port from which callback originated
 * @param buffer mmal buffer header pointer
 */
static void encoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{   
	if(!potato)
	{
		int x, y;
		// MOD: OpenCV modifications
		// Create an empty matrix with the size of the buffer.
		CvMat* buf = cvCreateMat(1,buffer->length,CV_8UC1);
   
		// Copy buffer from camera to matrix.
		buf->data.ptr = buffer->data;
   
		// Decode the image and display it.
		IplImage* image = cvDecodeImage(buf, CV_LOAD_IMAGE_COLOR);
		
		// Load the image in grey.
		IplImage* grey = cvDecodeImage(buf, CV_LOAD_IMAGE_GRAYSCALE);	
		
		// View for the final image
		cvNamedWindow("Corner detection", CV_WINDOW_AUTOSIZE);	
		
		// Initialise the octagon
		octagon(image);	
	
		// Destination of the harris algorithm
		CvMat* cornerMap = cvCreateMat(image->height, image->width, CV_32FC1);
	
		// OpenCV corner detection
		cvCornerHarris(grey,cornerMap,3, 3, 0.04);	

		// iterate over ever pixel in the image by iterating 
		// over each row and column
		for (y = 0; y < image->height; ++y)
		{		
			for(x = 0; x < image->width; ++x)
			{
				CvScalar harris = cvGet2D(cornerMap, y, x); // get the x,y value
 	  
				// check the corner detector response
				if (harris.val[0] > 10e-06)
				{
					// draw a small circle on the original image to represent the corner
					cvCircle(image,cvPoint(x,y),2,CV_RGB(155, 0, 25),1,8,0);

					// Check if that corner is in one of the 8 points.
					active(x,y);

					// Change to blue the activated corners of the octagon.
					bluePoint(image);

					// Print the resulting direction
					char *direct = direction();
					
					if (direct != NULL)
					{
						printf("%s \n", direct);
						//fflush(stdout);
					} // if
				} // if
			} // for
		} //for	

		// Save image if you want
		//cvSaveImage("image_without_background.jpg", my_image)

		// display the image on the screen
		cvShowImage("Corner detection", image);
		cvWaitKey(1);
   
		potato = 1;
	} // if
	
   int complete = 0;

   // We pass our file handle and other stuff in via the userdata field.

   PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

   if (pData)
   {
      int bytes_written = buffer->length;

      if (buffer->length && pData->file_handle)
      {
         mmal_buffer_header_mem_lock(buffer);

         bytes_written = fwrite(buffer->data, 1, buffer->length, pData->file_handle);

         mmal_buffer_header_mem_unlock(buffer);
      }

      // We need to check we wrote what we wanted - it's possible we have run out of storage.
      if (bytes_written != buffer->length)
      {
         vcos_log_error("Unable to write buffer to file - aborting");
         complete = 1;
      }

      // Now flag if we have completed
      if (buffer->flags & (MMAL_BUFFER_HEADER_FLAG_FRAME_END | MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED))
         complete = 1;
   }
   else
   {
      vcos_log_error("Received a encoder buffer callback with no state");
   }

   // release buffer back to the pool
   mmal_buffer_header_release(buffer);

   // and send one back to the port (if still open)
   if (port->is_enabled)
   {
      MMAL_STATUS_T status;
      MMAL_BUFFER_HEADER_T *new_buffer;

      new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue);

      if (new_buffer)
      {
         status = mmal_port_send_buffer(port, new_buffer);
      }
      if (!new_buffer || status != MMAL_SUCCESS)
         vcos_log_error("Unable to return a buffer to the encoder port");
   } 

   if (complete)
      vcos_semaphore_post(&(pData->complete_semaphore));
}


/**
 * Create the camera component, set up its ports
 *
 * @param state Pointer to state control struct. camera_component member set to the created camera_component if successfull.
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
 */
static MMAL_STATUS_T create_camera_component(RASPISTILL_STATE *state)
{
   MMAL_COMPONENT_T *camera = 0;
   MMAL_ES_FORMAT_T *format;
   MMAL_PORT_T *preview_port = NULL, *video_port = NULL, *still_port = NULL;
   MMAL_STATUS_T status;

   /* Create the component */
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Failed to create camera component");
      goto error;
   }

   if (!camera->output_num)
   {
      status = MMAL_ENOSYS;
      vcos_log_error("Camera doesn't have output ports");
      goto error;
   }

   preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
   video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
   still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

   // Enable the camera, and tell it its control callback function
   status = mmal_port_enable(camera->control, camera_control_callback);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to enable control port : error %d", status);
      goto error;
   }

   //  set up the camera configuration
   {
      MMAL_PARAMETER_CAMERA_CONFIG_T cam_config =
      {
         { MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config) },
         .max_stills_w = state->width,
         .max_stills_h = state->height,
         .stills_yuv422 = 0,
         .one_shot_stills = 1,
         .max_preview_video_w = state->preview_parameters.previewWindow.width,
         .max_preview_video_h = state->preview_parameters.previewWindow.height,
         .num_preview_video_frames = 3,
         .stills_capture_circular_buffer_height = 0,
         .fast_preview_resume = 0,
         .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
      };
      mmal_port_parameter_set(camera->control, &cam_config.hdr);
   }

   raspicamcontrol_set_all_parameters(camera, &state->camera_parameters);

   // Now set up the port formats

   format = preview_port->format;

   format->encoding = MMAL_ENCODING_OPAQUE;
   format->encoding_variant = MMAL_ENCODING_I420;

   format->es->video.width = state->preview_parameters.previewWindow.width;
   format->es->video.height = state->preview_parameters.previewWindow.height;
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->preview_parameters.previewWindow.width;
   format->es->video.crop.height = state->preview_parameters.previewWindow.height;
   format->es->video.frame_rate.num = PREVIEW_FRAME_RATE_NUM;
   format->es->video.frame_rate.den = PREVIEW_FRAME_RATE_DEN;

   status = mmal_port_format_commit(preview_port);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera viewfinder format couldn't be set");
      goto error;
   }

   // Set the same format on the video  port (which we dont use here)
   mmal_format_full_copy(video_port->format, format);
   status = mmal_port_format_commit(video_port);

   if (status  != MMAL_SUCCESS)
   {
      vcos_log_error("camera video format couldn't be set");
      goto error;
   }

   // Ensure there are enough buffers to avoid dropping frames
   if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

   format = still_port->format;

   // Set our stills format on the stills (for encoder) port
   format->encoding = MMAL_ENCODING_OPAQUE;
   format->es->video.width = state->width;
   format->es->video.height = state->height;
   format->es->video.crop.x = 0;
   format->es->video.crop.y = 0;
   format->es->video.crop.width = state->width;
   format->es->video.crop.height = state->height;
   format->es->video.frame_rate.num = STILLS_FRAME_RATE_NUM;
   format->es->video.frame_rate.den = STILLS_FRAME_RATE_DEN;

   status = mmal_port_format_commit(still_port);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera still format couldn't be set");
      goto error;
   }

   /* Ensure there are enough buffers to avoid dropping frames */
   if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
      still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

   /* Enable component */
   status = mmal_component_enable(camera);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("camera component couldn't be enabled");
      goto error;
   }

   state->camera_component = camera;

   if (state->verbose)
      fprintf(stderr, "Camera component done\n");

   return status;

error:

   if (camera)
      mmal_component_destroy(camera);

   return status;
}

/**
 * Destroy the camera component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_camera_component(RASPISTILL_STATE *state)
{
   if (state->camera_component)
   {
      mmal_component_destroy(state->camera_component);
      state->camera_component = NULL;
   }
}



/**
 * Create the encoder component, set up its ports
 *
 * @param state Pointer to state control struct. encoder_component member set to the created camera_component if successfull.
 *
 * @return a MMAL_STATUS, MMAL_SUCCESS if all OK, something else otherwise
 */
static MMAL_STATUS_T create_encoder_component(RASPISTILL_STATE *state)
{
   MMAL_COMPONENT_T *encoder = 0;
   MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
   MMAL_STATUS_T status;
   MMAL_POOL_T *pool;

   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER, &encoder);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to create JPEG encoder component");
      goto error;
   }

   if (!encoder->input_num || !encoder->output_num)
   {
      status = MMAL_ENOSYS;
      vcos_log_error("JPEG encoder doesn't have input/output ports");
      goto error;
   }

   encoder_input = encoder->input[0];
   encoder_output = encoder->output[0];

   // We want same format on input and output
   mmal_format_copy(encoder_output->format, encoder_input->format);

   // Specify out output format
   encoder_output->format->encoding = state->encoding;

   encoder_output->buffer_size = encoder_output->buffer_size_recommended;

   if (encoder_output->buffer_size < encoder_output->buffer_size_min)
      encoder_output->buffer_size = encoder_output->buffer_size_min;

   encoder_output->buffer_num = encoder_output->buffer_num_recommended;

   if (encoder_output->buffer_num < encoder_output->buffer_num_min)
      encoder_output->buffer_num = encoder_output->buffer_num_min;

   // Commit the port changes to the output port
   status = mmal_port_format_commit(encoder_output);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to set format on video encoder output port");
      goto error;
   }

   // Set the JPEG quality level
   status = mmal_port_parameter_set_uint32(encoder_output, MMAL_PARAMETER_JPEG_Q_FACTOR, state->quality);

   if (status != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to set JPEG quality");
      goto error;
   }

   // Set up any required thumbnail
   {
      MMAL_PARAMETER_THUMBNAIL_CONFIG_T param_thumb = {{MMAL_PARAMETER_THUMBNAIL_CONFIGURATION, sizeof(MMAL_PARAMETER_THUMBNAIL_CONFIG_T)}, 0, 0, 0, 0};

      if ( state->thumbnailConfig.width > 0 && state->thumbnailConfig.height > 0 )
      {
         // Have a valid thumbnail defined
         param_thumb.enable = 1;
         param_thumb.width = state->thumbnailConfig.width;
         param_thumb.height = state->thumbnailConfig.height;
         param_thumb.quality = state->thumbnailConfig.quality;
      }
      status = mmal_port_parameter_set(encoder->control, &param_thumb.hdr);
   }

   //  Enable component
   status = mmal_component_enable(encoder);

   if (status  != MMAL_SUCCESS)
   {
      vcos_log_error("Unable to enable video encoder component");
      goto error;
   }

   /* Create pool of buffer headers for the output port to consume */
   pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);

   if (!pool)
   {
      vcos_log_error("Failed to create buffer header pool for encoder output port %s", encoder_output->name);
   }

   state->encoder_pool = pool;
   state->encoder_component = encoder;

   if (state->verbose)
      fprintf(stderr, "Encoder component done\n");

   return status;

   error:

   if (encoder)
      mmal_component_destroy(encoder);

   return status;
}

/**
 * Destroy the encoder component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_encoder_component(RASPISTILL_STATE *state)
{
   // Get rid of any port buffers first
   if (state->encoder_pool)
   {
      mmal_port_pool_destroy(state->encoder_component->output[0], state->encoder_pool);
   }

   if (state->encoder_component)
   {
      mmal_component_destroy(state->encoder_component);
      state->encoder_component = NULL;
   }
}

/**
 * Connect two specific ports together
 *
 * @param output_port Pointer the output port
 * @param input_port Pointer the input port
 * @param Pointer to a mmal connection pointer, reassigned if function successful
 * @return Returns a MMAL_STATUS_T giving result of operation
 *
 */
static MMAL_STATUS_T connect_ports(MMAL_PORT_T *output_port, MMAL_PORT_T *input_port, MMAL_CONNECTION_T **connection)
{
   MMAL_STATUS_T status;

   status =  mmal_connection_create(connection, output_port, input_port, MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT);

   if (status == MMAL_SUCCESS)
   {
      status =  mmal_connection_enable(*connection);
      if (status != MMAL_SUCCESS)
         mmal_connection_destroy(*connection);
   }

   return status;
}

/**
 * Checks if specified port is valid and enabled, then disables it
 *
 * @param port  Pointer the port
 *
 */
static void check_disable_port(MMAL_PORT_T *port)
{
   if (port && port->is_enabled)
      mmal_port_disable(port);
}

/**
 * Handler for sigint signals
 *
 * @param signal_number ID of incoming signal.
 *
 */
static void signal_handler(int signal_number)
{
   // Going to abort on all signals
   vcos_log_error("Aborting program\n");

   // Need to close any open stuff...

   exit(255);
}

/**
 * main
 */
int main(int argc, const char **argv)
{
   // Our main data storage vessel..
   RASPISTILL_STATE state;

   MMAL_STATUS_T status = MMAL_SUCCESS;
   MMAL_PORT_T *camera_preview_port = NULL;
   MMAL_PORT_T *camera_video_port = NULL;
   MMAL_PORT_T *camera_still_port = NULL;
   MMAL_PORT_T *preview_input_port = NULL;
   MMAL_PORT_T *encoder_input_port = NULL;
   MMAL_PORT_T *encoder_output_port = NULL;

   bcm_host_init();

   // Register our application with the logging system
   vcos_log_register("camcv", VCOS_LOG_CATEGORY);

   signal(SIGINT, signal_handler);

   default_status(&state);   

   // Do we have any parameters
   /*if (argc == 1)
   {
      fprintf(stderr, "\%s Camera App %s\n\n", basename(argv[0]), VERSION_STRING);
      
      exit(0);
   }*/
   
   if (state.verbose)
   {
      fprintf(stderr, "\n%s Camera App %s\n\n", basename(argv[0]), VERSION_STRING);      
   }

   // OK, we have a nice set of parameters. Now set up our components
   // We have three components. Camera, Preview and encoder.
   // Camera and encoder are different in stills/video, but preview
   // is the same so handed off to a separate module

   if ((status = create_camera_component(&state)) != MMAL_SUCCESS)
   {
      vcos_log_error("%s: Failed to create camera component", __func__);
   }
   else if ((status = raspipreview_create(&state.preview_parameters)) != MMAL_SUCCESS)
   {
      vcos_log_error("%s: Failed to create preview component", __func__);
      destroy_camera_component(&state);
   }
   else if ((status = create_encoder_component(&state)) != MMAL_SUCCESS)
   {
      vcos_log_error("%s: Failed to create encode component", __func__);
      raspipreview_destroy(&state.preview_parameters);
      destroy_camera_component(&state);
   }
   else
   {
      PORT_USERDATA callback_data;

      if (state.verbose)
         fprintf(stderr, "Starting component connection stage\n");
         
      //*** PR : remove preview
      camera_preview_port = NULL;//state.camera_component->output[MMAL_CAMERA_PREVIEW_PORT];
      camera_video_port   = state.camera_component->output[MMAL_CAMERA_VIDEO_PORT];
      camera_still_port   = state.camera_component->output[MMAL_CAMERA_CAPTURE_PORT];
      preview_input_port  = state.preview_parameters.preview_component->input[0];
      encoder_input_port  = state.encoder_component->input[0];
      encoder_output_port = state.encoder_component->output[0];

      if (state.preview_parameters.wantPreview )
      {
         if (state.verbose)
         {
            fprintf(stderr, "Connecting camera preview port to preview input port\n");
            fprintf(stderr, "Starting video preview\n");
         }

         // *** PR: remove preview
         // Connect camera to preview
         //status = connect_ports(camera_preview_port, preview_input_port, &state.preview_connection);

      }
      else
      {
         status = MMAL_SUCCESS;
      }

      if (status == MMAL_SUCCESS)
      {
         VCOS_STATUS_T vcos_status;

         if (state.verbose)
            fprintf(stderr, "Connecting camera stills port to encoder input port\n");

         // Now connect the camera to the encoder
         status = connect_ports(camera_still_port, encoder_input_port, &state.encoder_connection);
         

         if (status != MMAL_SUCCESS)
         {
            vcos_log_error("%s: Failed to connect camera video port to encoder input", __func__);
            goto error;
         }

         // Set up our userdata - this is passed though to the callback where we need the information.
         // Null until we open our filename
         callback_data.file_handle = NULL;
         callback_data.pstate = &state;
         vcos_status = vcos_semaphore_create(&callback_data.complete_semaphore, "RaspiStill-sem", 0);

         vcos_assert(vcos_status == VCOS_SUCCESS);

         if (status != MMAL_SUCCESS)
         {
            vcos_log_error("Failed to setup encoder output");
            goto error;
         }

         if (state.demoMode)
         {
            // Run for the user specific time..
            int num_iterations = state.timeout / state.demoInterval;
            int i;
            for (i=0;i<num_iterations;i++)
            {
               raspicamcontrol_cycle_test(state.camera_component);
               vcos_sleep(state.demoInterval);
            }
         }         
         else
         {			 
			 FILE *output_file = NULL;
            /*int num_iterations =  2;//state.timelapse ? state.timeout / state.timelapse : 1;
            int frame; */
            
            int frame = 0; 
            
            while(1==1)//*/ for (frame=1;frame<=num_iterations; frame++)           
            {
				// REDEFINE
                potato = 0;
                
				if (state.timelapse)
                  vcos_sleep(state.timelapse);
                else
                  vcos_sleep(state.timeout);

               // Open the file
               if (state.filename)
               {
		            if (state.filename[0] == '-')
    		         {
		               output_file = stdout;

		               // Ensure we don't upset the output stream with diagnostics/info
		               state.verbose = 0;
    		         }
                  else
                  {
                     char *use_filename = state.filename;
	
	                  if (state.timelapse)
	                     asprintf(&use_filename, state.filename, frame);
	
	                  if (state.verbose)
	                     fprintf(stderr, "Opening output file %s\n", use_filename);
	
	                  output_file = fopen(use_filename, "wb");
	
	                  if (!output_file)
	                  {
	                     // Notify user, carry on but discarding encoded output buffers
	                     vcos_log_error("%s: Error opening output file: %s\nNo output file will be generated\n", __func__, use_filename);
	                  }

	                  // asprintf used in timelapse mode allocates its own memory which we need to free
	                  if (state.timelapse)
	                     free(use_filename);
                  }
									
                  callback_data.file_handle = output_file;
               }

               // We only capture if a filename was specified and it opened
               if (output_file)
               {
                  int num, q;                  

                  // Same with raw, apparently need to set it for each capture, whilst port
                  // is not enabled
                  if (state.wantRAW)
                  {
                     if (mmal_port_parameter_set_boolean(camera_still_port, MMAL_PARAMETER_ENABLE_RAW_CAPTURE, 1) != MMAL_SUCCESS)
                     {
                        vcos_log_error("RAW was requested, but failed to enable");
                     }
                  }
                  
                  // Enable the encoder output port
                  encoder_output_port->userdata = (struct MMAL_PORT_USERDATA_T *)&callback_data;

                  if (state.verbose)
                     fprintf(stderr, "Enabling encoder output port\n");
                  
                  // Enable the encoder output port and tell it its callback function
                  status = mmal_port_enable(encoder_output_port, encoder_buffer_callback);

                  // Send all the buffers to the encoder output port
                  num = mmal_queue_length(state.encoder_pool->queue);
                  
                  for (q=0;q<num;q++)
                  {
                     MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(state.encoder_pool->queue);

                     if (!buffer)
                        vcos_log_error("Unable to get a required buffer %d from pool queue", q);

                     if (mmal_port_send_buffer(encoder_output_port, buffer)!= MMAL_SUCCESS)
                        vcos_log_error("Unable to send a buffer to encoder output port (%d)", q);
                  }

                  if (state.verbose)
                     fprintf(stderr, "Starting capture %d\n", frame);

                  if (mmal_port_parameter_set_boolean(camera_still_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS)
                  {
                     vcos_log_error("%s: Failed to start capture", __func__);
                  }
                  else
                  {
                     // Wait for capture to complete
                     // For some reason using vcos_semaphore_wait_timeout sometimes 
                     // returns immediately with bad parameter error
                     // even though it appears to be all correct, so reverting to untimed 
                     // one until figure out why its erratic
                     vcos_semaphore_wait(&callback_data.complete_semaphore);
                     if (state.verbose)
                        fprintf(stderr, "Finished capture %d\n", frame);
                  }

                  // Ensure we don't die if get callback with no open file
                  callback_data.file_handle = NULL;

                  if (output_file != stdout)
                     fclose(output_file);

                  // Disable encoder output port
                  status = mmal_port_disable(encoder_output_port);
               } 
               
            } // end while (frame)            
               
            vcos_semaphore_delete(&callback_data.complete_semaphore);
         }
      }
      else
      {
         mmal_status_to_int(status);
         vcos_log_error("%s: Failed to connect camera to preview", __func__);
      }

error:

      mmal_status_to_int(status);

      if (state.verbose)
         fprintf(stderr, "Closing down\n");

      // Disable all our ports that are not handled by connections
      check_disable_port(camera_video_port);
      check_disable_port(encoder_output_port);

      if (state.preview_parameters.wantPreview )
         mmal_connection_destroy(state.preview_connection);

      mmal_connection_destroy(state.encoder_connection);

      /* Disable components */
      if (state.encoder_component)
         mmal_component_disable(state.encoder_component);

      if (state.preview_parameters.preview_component)
         mmal_component_disable(state.preview_parameters.preview_component);

      if (state.camera_component)
         mmal_component_disable(state.camera_component);

      destroy_encoder_component(&state);
      raspipreview_destroy(&state.preview_parameters);
      destroy_camera_component(&state);

      if (state.verbose)
         fprintf(stderr, "Close down completed, all components disconnected, disabled and destroyed\n\n");
   }

   if (status != MMAL_SUCCESS)
      raspicamcontrol_check_configuration(128);
      
   return 0;
}

