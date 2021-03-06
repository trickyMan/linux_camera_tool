/****************************************************************************
  This sample is released as public domain.  It is distributed in the hope it
  will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
  of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 

  This is the sample code for Leopard USB3.0 camera use v4l2 and opencv for 
  camera streaming and display.

  Common implementation of a v4l2 application
  1. Open a descriptor to the device.
  2. Retrieve and analyse the device's capabilities.
  3. Set the capture format(YUYV etc).
  4. Prepare the device for buffering handling. 
     when capturing a frame, you have to submit a buffer to the device(queue)
     and retrieve it once it's been filled with data(dequeue)
     Before you can do this, you must inform the cdevice about 
     your buffer(buffer request)
  5. For each buffer you wish to use, you must negotiate characteristics with 
     the device(buffer size, frame start offset in memory), and create a new
     memory mapping for it
  6. Put the device into streaming mode
  7. Once your buffers are ready, all you have to do is keep queueing and
     dequeueing your buffer repeatedly, and every call will bring you a new 
     frame. The delay you set between each frames by putting your program to
     sleep is what determines your fps
  8. Turn off streaming mode
  9. Unmap the buffer
 10. Close your descriptor to the device 
  
  Author: Danyu L
  Last edit: 2019/04
*****************************************************************************/

/* Include files to use OpenCV API */
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <string.h>
#include <omp.h> //for openmp

#include "../includes/shortcuts.h"
#include "extend_cam_ctrl.h"
/****************************************************************************
**                      	Global data 
*****************************************************************************/
/* 
 * Since we use fork, variables btw two processes are not shared
 * use mmap for all the variables you need to share between gui and videostreaming
 */
static int *save_bmp;   /* flag for saving bmp */
static int *save_raw;   /* flag for saving raw */
static int *bayer_flag; /* flag for choosing bayer pattern */
static int *shift_flag; /* flag for shift raw data */
static int *awb_flag;   /* flag for enable/disable software awb*/
static int *abc_flag;   /* flag for enable/disable software brightness & contrast optimization */
float *gamma_val;

static int image_count;

struct v4l2_buffer queuebuffer;
/*****************************************************************************
**                           Function definition
*****************************************************************************/

/*
 * callback for save bmp image from gui
 */
void video_capture_save_bmp()
{
	set_save_bmp_flag(1);
}

/*
 * set save raw image flag 
 * args:
 * 		flag - set the flag when get capture button clicked,
 * 			   reset flag to zero once image is saved
 */
void set_save_bmp_flag(int flag)
{
	*save_bmp = flag;
}

/*
 * save data to bitmap using opencv
 * args:
 *	 opencv mat image to be captured
 *
 * asserts:
 *   none
 */
static int save_frame_image_bmp(cv::Mat opencvImage)
{

	printf("save one capture bmp\n");
	cv::imwrite(cv::format("captures_%d.bmp",
						   image_count),
				opencvImage);

	return 0;
}

/*
 * callback for save raw image from gui
 */
void video_capture_save_raw()
{
	set_save_raw_flag(1);
}

/*
 * set save raw image flag 
 * args:
 * 		flag - set the flag when get capture button clicked,
 * 			   reset flag to zero once image is saved
 */
void set_save_raw_flag(int flag)
{
	*save_raw = flag;
}

/*
 * save data to file
 * args:
 *   filename - string with filename
 *   data - pointer to data
 *   size - data size in bytes = sizeof(uint8_t)
 *
 * asserts:
 *   none
 *
 * returns: error code
 */
int v4l2_core_save_data_to_file(const char *filename, const void *data, int size)
{
	FILE *fp;
	int ret = 0;

	if ((fp = fopen(filename, "wb")) != NULL)
	{
		ret = fwrite((uint8_t *)data, size, 1, fp);

		if (ret < 1)
			ret = 1; /*write error*/
		else
			ret = 0;

		fflush(fp); /*flush data stream to file system*/
		if (fsync(fileno(fp)) || fclose(fp))
			fprintf(stderr, "V4L2_CORE: (save_data_to_file) error \
				- couldn't write buffer to file: %s\n",
					strerror(errno));
		else
			printf("V4L2_CORE: saved data to %s\n", filename);
	}
	else
		ret = 1;

	return (ret);
}

/*
 * return the shift value for choiced sensor datatype
 * RAW10 - shift 2 bits
 * RAW12 - shift 4 bits
 * YUV422 - shift 0 bit
 * Crosslink doesn't support decode RAW14, RAW16 so far,
 * these two datatypes weren't used in USB3 camera
 */
int set_shift(int *shift_flag)
{
	if (*shift_flag == 1)
		return 2;
	if (*shift_flag == 2)
		return 4;
	if (*shift_flag == 3)
		return 0;
	return 2;
}

void awb_enable(int enable)
{
	if (enable == 1)
		*awb_flag = 1;

	if (enable == 0)
		*awb_flag = 0;
}

void abc_enable(int enable)
{
	if (enable == 1)
		*abc_flag = 1;

	if (enable == 0)
		*abc_flag = 0;
}

void add_gamma_val(float gamma_val_from_gui)
{
	*gamma_val = gamma_val_from_gui;
}
/*
 * callback for change sensor datatype shift flag
 * args:
 * 		datatype - RAW10  -> set *shift_flag = 1
 *  			   RAW12  -> set *shift_flag = 2
 * 				   YUV422 -> set *shift_flag = 3
 */
void change_datatype(void *datatype)
{
	if (strcmp((char *)datatype, "1") == 0)
		*shift_flag = 1;
	if (strcmp((char *)datatype, "2") == 0)
		*shift_flag = 2;
	if (strcmp((char *)datatype, "3") == 0)
		*shift_flag = 3;
}

/*
 * determine the sensor bayer pattern to correctly debayer the image
 *   CV_BayerBG2BGR =46   -> bayer_flag_increment = 0
 *   CV_BayerGB2BGR =47   -> bayer_flag_increment = 1
 *   CV_BayerRG2BGR =48   -> bayer_flag_increment = 2
 *   CV_BayerGR2BGR =49	  -> bayer_flag_increment = 3
 * default bayer pattern: RGGB
 * args:
 * 		bayer_flag - flag for determining bayer pattern 
 * returns:
 * 		bayer pattern flag increment
 */
int add_bayer_forcv(int *bayer_flag)
{
	if (*bayer_flag == 1)
		return 0;
	if (*bayer_flag == 2)
		return 1;
	if (*bayer_flag == 3)
		return 2;
	if (*bayer_flag == 4)
		return 3;
	return 2;
}

/*
 * callback for change sensor bayer pattern for debayering
 * args:
 * 		bayer - for updating *bayer_flag
 *  			   
 */
void change_bayerpattern(void *bayer)
{
	if (strcmp((char *)bayer, "1") == 0)
		*bayer_flag = 1;
	if (strcmp((char *)bayer, "2") == 0)
		*bayer_flag = 2;
	if (strcmp((char *)bayer, "3") == 0)
		*bayer_flag = 3;
	if (strcmp((char *)bayer, "4") == 0)
		*bayer_flag = 4;
}

/* 
 *  apply gamma correction for the given mat
 *  When *gamma_val < 1, the original dark regions will be brighter 
 *  and the histogram will be shifted to the right 
 *  whereas it will be the opposite with *gamma_val > 1
 *  recommend *gamma_val: 0.45(1/2.2)
 */
static cv::Mat apply_gamma_correction(cv::Mat opencvImage)
{
	cv::Mat look_up_table(1, 256, CV_8U);
	uchar *p = look_up_table.ptr();
	for (int i = 0; i < 256; i++)
	{
		p[i] = cv::saturate_cast<uchar>(pow(i / 255.0, *gamma_val) * 255.0);
	}
	LUT(opencvImage, look_up_table, opencvImage);
	return opencvImage;
}
 double rgb2rgb_param[3][3] = {
 	409.0, -137.0, -15.0, // + - -
 	-136.0, 468.0, -77.0, // - + -
 	4.0, -303.0, 554.0   // + - +
 };

 double rr = 409.0, rg = -137.0, rb = -15.0;
 double gr = -136.0, gg = 468.0, gb = -77.0;
 double br = 4.0, bg = -303.0, bb = 554.0;

// int rgb2rgb_param[3][3] = {
// 	256, 0, 0, // + - -
// 	0, 256, 0, // - + -
// 	0, 0, 256   // + - +
// };

int rgb2rgb_param_2[3][3] = {
	256, 1, -1,
	2, 256, -1,
	2, -1, 256
};
/* 
 *  apply white balance for the given mat
 *  the basic idea of Leopard AWB algorithm is to find the gray area of the image and apply
 *  Red, Green and Blue gains to make it gray, and then use the gray area to estimate the
 *  color temperature.
 */
static cv::Mat apply_white_balance(cv::Mat opencvImage)
{

	cv::Mat splitChannelRGB[3];
	cv::Mat floatImage(4056, 3040, CV_32FC1);
	floatImage = opencvImage.clone();
	//floatImage = opencvImage;
	split(floatImage, splitChannelRGB);

	double R, G, B;
	B = mean(splitChannelRGB[0])[0];
	G = mean(splitChannelRGB[1])[0];
	R = mean(splitChannelRGB[2])[0];
	double RGB_ORIG[3] = {B, G, R};
	// /* gain for adjusting each color channel */
	double KR = 1, KG = 1, KB = 1;
	double RGB_UPDATE[3] = {KB, KG, KR};
	KB = 267.0/256;//(R + G + B) / (3 * B);
	KG = 403.0/256;//(R + G + B) / (3 * G);
	KR = 471.0/256;//(R + G + B) / (3 * R);
	// printf("KB = %f\n", KB);
	// printf("KG = %f\n", KG);
	// printf("KR = %f\n", KR);

	splitChannelRGB[0] = splitChannelRGB[0] * KB;
	splitChannelRGB[1] = splitChannelRGB[1] * KG;
	splitChannelRGB[2] = splitChannelRGB[2] * KR;

	// printf("B = %f\n", RGB_ORIG[0]);
	// printf("G = %f\n", RGB_ORIG[1]);
	// printf("R = %f\n", RGB_ORIG[2]);
	// for (int i =0; i < 3; i++) {
	// 	for (int j=0; j < 3; j++) {
	// 		RGB_UPDATE[i] +=  (rgb2rgb_param[i][j]* RGB_ORIG[i] / 256);
	// 	}
	// }
	// printf("KB = %f\n", RGB_UPDATE[0]);
	// printf("KG = %f\n", RGB_UPDATE[1]);
	// printf("KR = %f\n", RGB_UPDATE[2]);
	// /* adjust rgb channel values */
	 splitChannelRGB[2] = splitChannelRGB[2] * rr / 256 
	 					+ splitChannelRGB[1] * rg / 256
						+ splitChannelRGB[0] * rb / 256;

	 splitChannelRGB[1] = splitChannelRGB[2] * gr / 256 
	 					+ splitChannelRGB[1] * gg / 256
						+ splitChannelRGB[0] * gb / 256;

	 splitChannelRGB[0] = splitChannelRGB[2] * br / 256 
	 					+ splitChannelRGB[1] * bg / 256
						+ splitChannelRGB[0] * bb / 256;

	/* merge three RGB channels back together */
	merge(splitChannelRGB, 3, opencvImage);
	return opencvImage;
}

/*
 * Automatic brightness and contrast optimization with optional histogram clipping
 * Looking at histogram, alpha operates as color range amplifier, beta operates as range shift.
 * O(x,y) = alpha * I(x,y) + beta
 * Automatic brightness and contrast optimization calculates alpha and beta so that the output range is 0..255.
 * Ref: http://answers.opencv.org/question/75510/how-to-make-auto-adjustmentsbrightness-and-contrast-for-image-android-opencv-image-correction/
 * args:
 * 	 clipHistPercent - cut wings of histogram at given percent 
 * 		typical=>1, 0=>Disabled
 */
static cv::Mat apply_auto_brightness_and_contrast(cv::Mat opencvImage,
												  float clipHistPercent = 0)
{
	int hist_size = 256;
	float alpha, beta;
	double min_gray = 0, max_gray = 0;

	/* to calculate grayscale histogram */
	cv::Mat gray;
	cv::cvtColor(opencvImage, gray, CV_BGR2GRAY);

	if (clipHistPercent == 0)
	{
		/* keep full available range */
		cv::minMaxLoc(gray, &min_gray, &max_gray);
	}
	else
	{
		/* the grayscale histogram */
		cv::Mat hist;

		float range[] = {0, 256};
		const float *histRange = {range};
		bool uniform = true;
		bool accumulate = false;
		calcHist(&gray, 1, 0, cv::Mat(), hist, 1, &hist_size, &histRange, uniform, accumulate);

		/* calculate cumulative distribution from the histogram */
		std::vector<float> accumulator(hist_size);
		accumulator[0] = hist.at<float>(0);
		for (int i = 1; i < hist_size; i++)
		{
			accumulator[i] = accumulator[i - 1] + hist.at<float>(i);
		}

		/* locate points that cuts at required value */
		float max = accumulator.back();
		clipHistPercent *= (max / 100.0); //make percent as absolute
		clipHistPercent /= 2.0;			  // left and right wings
		/* locate left cut */
		min_gray = 0;
		while (accumulator[min_gray] < clipHistPercent)
			min_gray++;

		/* locate right cut */
		max_gray = hist_size - 1;
		while (accumulator[max_gray] >= (max - clipHistPercent))
			max_gray--;
	}

	/* current range */
	float input_range = max_gray - min_gray;

	alpha = (hist_size - 1) / input_range; // alpha expands current range to histsize range
	beta = -min_gray * alpha;			   // beta shifts current range so that minGray will go to 0

	/*
	* Apply brightness and contrast normalization
	* convertTo operates with saurate_cast
	*/
	opencvImage.convertTo(opencvImage, -1, alpha, beta);

	return opencvImage;
}

// static __THREAD_TYPE capture_thread;

// /*video buffer data mutex*/
// static __MUTEX_TYPE mutex = __STATIC_MUTEX_INIT;
// #define __PMUTEX &mutex

/* 
 * open the /dev/video* uvc camera device
 * 
 * args: 
 * 		device_name 
 * 		struct device *dev for adding fd
 * returns: 
 * 		file descriptor v4l2_dev
 */
int open_v4l2_device(char *device_name, struct device *dev)
{
	int v4l2_dev;

	if (device_name == NULL)
		return -5;

	v4l2_dev = open(device_name, O_RDWR);
	if (-1 == v4l2_dev)
	{
		perror("open video device fail");
		return -1;
	}
	dev->fd = v4l2_dev;

	mmap_variables();
	*gamma_val = 1.0;
	return v4l2_dev;
}

/* mmap the variables for processes share */
void mmap_variables()
{
	save_bmp = (int *)mmap(NULL, sizeof *save_bmp, PROT_READ | PROT_WRITE,
						   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	save_raw = (int *)mmap(NULL, sizeof *save_raw, PROT_READ | PROT_WRITE,
						   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	shift_flag = (int *)mmap(NULL, sizeof *shift_flag, PROT_READ | PROT_WRITE,
							 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	bayer_flag = (int *)mmap(NULL, sizeof *bayer_flag, PROT_READ | PROT_WRITE,
							 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	awb_flag = (int *)mmap(NULL, sizeof *awb_flag, PROT_READ | PROT_WRITE,
						   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	abc_flag = (int *)mmap(NULL, sizeof *abc_flag, PROT_READ | PROT_WRITE,
						   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	gamma_val = (float *)mmap(NULL, sizeof *bayer_flag, PROT_READ | PROT_WRITE,
							  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
}

/* 
 * retrive device's capabilities
 * 
 * args: 
 * 		struct device *dev - device infomation
 * returns: 
 * 		error value
 */
int check_dev_cap(struct device *dev)
{
	struct v4l2_capability cap;
	CLEAR(cap);
	int ret;
	ret = (ioctl(dev->fd, VIDIOC_QUERYCAP, &cap));
	if (ret < 0)
	{
		printf("VIDIOC_QUERYCAP error:%s\n", strerror(errno));
		return -1;
	}

	if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0)
	{
		printf("Error opening device: video capture not supported.\n");
		return -1;
	}
	if (!(cap.capabilities & V4L2_CAP_STREAMING))
	{
		printf("does not support streaming\n");

		return -1;
	}
	if (!(cap.capabilities & V4L2_CAP_READWRITE))
	{
		printf("does not support read, try with mmap\n");
		return -1;
	}
	return 0;
}

/*
 * activate streaming 
 * 
 * args: 
 * 		struct device *dev - device infomation
 */
void start_camera(struct device *dev)
{
	int ret;
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = (ioctl(dev->fd, VIDIOC_STREAMON, &type));
	if (ret < 0)
	{
		printf("couldn't start camera streaming\n");
		return;
	}
	return;
}

/*
 * deactivate streaming 
 * 
 * args: 
 * 		struct device *dev - device infomation
 */
void stop_Camera(struct device *dev)
{
	int ret;
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	ret = ioctl(dev->fd, VIDIOC_STREAMOFF, &type);
	if (ret < 0)
	{
		printf("couldn't stop camera streaming\n");
		return;
	}
	return;
}
/*
 * video set format - Leopard camera format is YUYV
 * need to do a v4l2-ctl --list-formats-ext to see the resolution
 * args: 
 * 		struct device *dev - device infomation
 * 	  	width - resoultion width
 * 		height - resolution height
 * 		pixelformat - V4L2_PIX_FMT_YUYV
 * 
 */
void video_set_format(struct device *dev, int width,
					  int height, int pixelformat)
{
	struct v4l2_format fmt;
	int ret;

	fmt.fmt.pix.width = width;
	dev->width = width;
	fmt.fmt.pix.height = height;
	dev->height = height;
	fmt.fmt.pix.pixelformat = pixelformat;
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	ret = ioctl(dev->fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0)
	{
		printf("Unable to set format: %s (%d).\n", strerror(errno),
			   errno);
		return;
	}
	printf("Get Video format: %c%c%c%c (%08x) %ux%u\n 	\
			byte per line:%d\nsize image:%ud\n",
		   (fmt.fmt.pix.pixelformat >> 0) & 0xff,
		   (fmt.fmt.pix.pixelformat >> 8) & 0xff,
		   (fmt.fmt.pix.pixelformat >> 16) & 0xff,
		   (fmt.fmt.pix.pixelformat >> 24) & 0xff,
		   fmt.fmt.pix.pixelformat,
		   fmt.fmt.pix.width,
		   fmt.fmt.pix.height,
		   fmt.fmt.pix.bytesperline,
		   fmt.fmt.pix.sizeimage);
	return;
}

/*
 * video get format - Leopard camera format is YUYV
 * run this to get the resolution
 * args: 
 * 		struct device *dev - device infomation
 * 	  	
 */
void video_get_format(struct device *dev)
{
	struct v4l2_format fmt;
	int ret;

	CLEAR(fmt);

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	ret = ioctl(dev->fd, VIDIOC_G_FMT, &fmt);
	if (ret < 0)
	{
		printf("Unable to get format: %s (%d).\n", strerror(errno),
			   errno);
		return;
	}
	dev->width = fmt.fmt.pix.width;
	dev->height = fmt.fmt.pix.height;
	dev->bytesperline = fmt.fmt.pix.bytesperline;
	dev->imagesize = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.sizeimage : 0;

	printf("Get Current Video Format: %c%c%c%c (%08x) %ux%u\nbyte per line:%d\nsize image:%ud\n",
		   (fmt.fmt.pix.pixelformat >> 0) & 0xff,
		   (fmt.fmt.pix.pixelformat >> 8) & 0xff,
		   (fmt.fmt.pix.pixelformat >> 16) & 0xff,
		   (fmt.fmt.pix.pixelformat >> 24) & 0xff,
		   fmt.fmt.pix.pixelformat,
		   fmt.fmt.pix.width,
		   fmt.fmt.pix.height,
		   fmt.fmt.pix.bytesperline,
		   fmt.fmt.pix.sizeimage);
	return;
}

/*
 * request, allocate and map buffers
 * 
 * args: 
 * 		struct device *dev - put buffers in
 * 		nbufs - number of buffer request, map
 * returns: 
 * 		errno 
 * 
 */
int video_alloc_buffers(struct device *dev, int nbufs)
{
	struct buffer *buffers;

	/* request buffer */
	struct v4l2_requestbuffers bufrequest;
	struct v4l2_buffer querybuffer;
	CLEAR(bufrequest);
	bufrequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	bufrequest.memory = V4L2_MEMORY_MMAP;
	bufrequest.count = nbufs;
	dev->nbufs = nbufs;

	int ret;
	ret = ioctl(dev->fd, VIDIOC_REQBUFS, &bufrequest);
	if (ret < 0)
	{
		printf("Unable to request buffers: %d.\n", errno);
		return ret;
	}
	printf("%u buffers requested.\n", bufrequest.count);

	/* allocate buffer */
	buffers = (buffer *)malloc(bufrequest.count * sizeof buffers[0]);
	if (buffers == NULL)
		return -ENOMEM;

	/* map the buffers */
	for (unsigned int i = 0; i < dev->nbufs; i++)
	{
		CLEAR(querybuffer);
		querybuffer.type = bufrequest.type;
		querybuffer.memory = V4L2_MEMORY_MMAP;
		querybuffer.index = i;

		ret = ioctl(dev->fd, VIDIOC_QUERYBUF, &querybuffer);
		if (ret < 0)
		{
			printf("Unable to query buffer %u (%d).\n", i, errno);
			return ret;
		}
		printf("length: %u offset: %u\n", querybuffer.length,
			   querybuffer.m.offset);

		buffers[i].length = querybuffer.length; /* remember for munmap() */

		buffers[i].start = mmap(NULL, querybuffer.length,
								PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd,
								querybuffer.m.offset);

		if (buffers[i].start == MAP_FAILED)
		{
			/* If you do not exit here you should unmap() and free()
           	the buffers mapped so far. */
			printf("Unable to map buffer %u (%d)\n", i, errno);
			return ret;
		}

		printf("Buffer mapped at address %p.\n", buffers[i].start);

		CLEAR(queuebuffer);
		queuebuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		queuebuffer.memory = V4L2_MEMORY_MMAP;
		queuebuffer.index = i; /* Queueing buffer index i. */

		/* Put the buffer in the incoming queue. */
		ret = ioctl(dev->fd, VIDIOC_QBUF, &queuebuffer);
		if (ret < 0)
		{
			printf("Unable to queue the buffer %d\n", errno);
			return ret;
		}
	}
	dev->buffers = buffers;
	return 0;
}

/* 
 * To get frames in few steps
 * 1. prepare information about the buffer you are queueing
 * 	  (done in video_allocate_buffers)
 * 2. activate the device streaming capability
 * 3. queue the buffer,  handling your buffer over to the device,
 *    put it into the incoming queue, wait for it to write stuff in
 * 4. decode the frame
 * 5. dequeue the buffer, the device is done, you may read the buffer
 * 6. put 3-5 in a for loop
 * 
 * args: 
 * 		struct device *dev - every infomation for camera
*/
int streaming_loop(struct device *dev)
{
	cv::namedWindow("cam", CV_WINDOW_FREERATIO);
	image_count = 0;
	//TODO: add a loop flag to exit
	while (1)
	{
		get_a_frame(dev);
	}
	unmap_variables();
}

/* unmap all the variables after stream ends */
void unmap_variables()
{
	munmap(save_bmp, sizeof *save_bmp);
	munmap(save_raw, sizeof *save_raw);
	munmap(shift_flag, sizeof *shift_flag);
	munmap(bayer_flag, sizeof *bayer_flag);
	munmap(awb_flag, sizeof *awb_flag);
	munmap(abc_flag, sizeof *abc_flag);
	munmap(gamma_val, sizeof *gamma_val);
}

/* 
 * Typically start two loops:
 * 1. runs for as long as you want to
 *    capture frames (shoot the video).
 * 2. iterates over your buffers everytime. 
 *
 * args: 
 * 		struct device *dev - every infomation for camera
 */
void get_a_frame(struct device *dev)
{

	for (unsigned int i = 0; i < dev->nbufs; i++)
	{

		queuebuffer.index = i;
		/* The buffer's waiting in the outgoing queue. */
		if (ioctl(dev->fd, VIDIOC_DQBUF, &queuebuffer) < 0)
		{
			perror("VIDIOC_DQBUF");
			return;
		}

		/* check the capture raw image flag, do this before decode a frame */
		if (*(save_raw))
		{
			printf("save a raw\n");
			char buf_name[16];
			snprintf(buf_name, sizeof(buf_name), "captures_%d.raw", image_count);
			v4l2_core_save_data_to_file(buf_name,
										dev->buffers->start, dev->imagesize);
			image_count++;
			set_save_raw_flag(0);
		}

		decode_a_frame(dev, dev->buffers->start, set_shift(shift_flag));

		if (ioctl(dev->fd, VIDIOC_QBUF, &queuebuffer) < 0)
		{
			perror("VIDIOC_QBUF");
			return;
		}
	}
	return;
}

/* 
 * opencv only support debayering 8 and 16 bits 
 * 
 * decode the frame, move each pixel by certain bits,
 * and mask it for 8 bits, render a frame using opencv
 * args: 
 * 		struct device *dev - every infomation for camera
 * 		const void *p - pointer for the buffer
 * 		int shift - values to shift(RAW10 - 2, RAW12 - 4, YUV422 - 0) 
 * 
 */
void decode_a_frame(struct device *dev, const void *p, int shift)
{
	int height = dev->height;
	int width = dev->width;
	unsigned char tmp;
	unsigned short *srcShort = (unsigned short *)p;
	unsigned char *dst = (unsigned char *)p;
	unsigned short ts;

	/* --- for bayer camera ---*/
	if (shift != 0)
	{
/* shift bits for 16-bit stream and get lower 8-bit for opencv 
	 	 * debayering */
/* use openmp loop parallelism to accelate shifting */
#pragma omp for
		for (int i = 0; i < height; i++)
		{
			for (int j = 0; j < width; j++)
			{
				ts = *(srcShort++);
				if (ts > 64)
					tmp = (ts - 64) >> shift;
				else
				{
					tmp = 0;
				}
				
				*(dst++) = (unsigned char)tmp;
			}
		}

		cv::Mat img(height, width, CV_8UC1, (void *)p);
		cv::cvtColor(img, img, CV_BayerBG2BGR + add_bayer_forcv(bayer_flag));
		//flip(img, img, 0); //mirror vertically
		//flip(img, img, 1); //mirror horizontally
		//apply_gamma(p, gamma_val, height, width);
		img = apply_gamma_correction(img);
		/* check awb flag, awb functionality, only available for bayer camera */
		if (*(awb_flag) == 1)
		{
			img = apply_white_balance(img);
		}
		if (*(abc_flag) == 1)
		{
			img = apply_auto_brightness_and_contrast(img, 1);
		}
		/* check for save capture bmp flag, after decode the image */
		if (*(save_bmp))
		{
			printf("save a bmp\n");
			save_frame_image_bmp(img);
			image_count++;
			set_save_bmp_flag(0);
		}
		//if image larger than 720p by any dimension, reszie the window
		if (width >= 1280 || height >= 720)
		{
			cv::resizeWindow("cam", 1280, 720);
		}

		cv::imshow("cam", img);
	}
	/* --- for yuv camera ---*/
	else
	{

		cv::Mat img(height, width, CV_8UC2, (void *)p);
		//apply_gamma(p, gamma_val, height, width);
		cv::cvtColor(img, img, cv::COLOR_YUV2BGR_YUY2);

		/* check for save capture bmp flag, after decode the image */
		if (*(save_bmp))
		{
			printf("save a bmp\n");
			save_frame_image_bmp(img);
			image_count++;
			set_save_bmp_flag(0);
		}

		cv::resizeWindow("cam", 640, 480);
		cv::imshow("cam", img);
	}

	if (cv::waitKey(_1MS) == _ESC_KEY)
	{
		cv::destroyWindow("cam");
		exit(0);
	}
}

/*
 * free, unmap buffers
 * 
 * args: 
 * 		struct device *dev - put buffers in
 * returns: 
 * 		errno 
 * 
 */
int video_free_buffers(struct device *dev)
{
	struct v4l2_requestbuffers requestbuf;
	unsigned int i;
	int ret;

	if (dev->nbufs == 0)
		return 0;

	for (i = 0; i < dev->nbufs; ++i)
	{
		ret = munmap(dev->buffers[i].start, dev->buffers[i].length);
		if (ret < 0)
		{
			printf("Unable to unmap buffer %u (%d)\n", i, errno);
			return ret;
		}
	}

	CLEAR(requestbuf);
	requestbuf.count = 0;
	requestbuf.type = dev->type;
	requestbuf.memory = dev->memtype;

	ret = ioctl(dev->fd, VIDIOC_REQBUFS, &requestbuf);
	if (ret < 0)
	{
		printf("Unable to release buffers: %d.\n", errno);
		return ret;
	}

	printf("%u buffers released.\n", dev->nbufs);

	free(dev->buffers);
	dev->buffers = NULL;
	dev->nbufs = 0;

	return 0;
}
