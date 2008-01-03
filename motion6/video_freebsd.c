/*	video_freebsd.c
 *
 *	BSD Video stream functions for motion.
 *	Copyright 2004 by Angel Carpintero (ack@telefonica.net)
 *	This software is distributed under the GNU public license version 2
 *	See also the file 'COPYING'.
 *
 */

/* Common stuff: */
#include "motion.h"
#include "video_freebsd.h"
#include "conf.h"
/* for rotation */
#include "rotate.h"

#ifndef WITHOUT_V4L

/* for the v4l stuff: */
#include "sys/mman.h"
#include "sys/ioctl.h"

/* Hack from xawtv 4.x */

#define VIDEO_NONE           0
#define VIDEO_RGB08          1  /* bt848 dithered */
#define VIDEO_GRAY           2
#define VIDEO_RGB15_LE       3  /* 15 bpp little endian */
#define VIDEO_RGB16_LE       4  /* 16 bpp little endian */
#define VIDEO_RGB15_BE       5  /* 15 bpp big endian */
#define VIDEO_RGB16_BE       6  /* 16 bpp big endian */
#define VIDEO_BGR24          7  /* bgrbgrbgrbgr (LE) */
#define VIDEO_BGR32          8  /* bgr-bgr-bgr- (LE) */
#define VIDEO_RGB24          9  /* rgbrgbrgbrgb (BE) */
#define VIDEO_RGB32         10  /* -rgb-rgb-rgb (BE) */
#define VIDEO_LUT2          11  /* lookup-table 2 byte depth */
#define VIDEO_LUT4          12  /* lookup-table 4 byte depth */
#define VIDEO_YUYV          13  /* 4:2:2 */
#define VIDEO_YUV422P       14  /* YUV 4:2:2 (planar) */
#define VIDEO_YUV420P       15  /* YUV 4:2:0 (planar) */
#define VIDEO_MJPEG         16  /* MJPEG (AVI) */
#define VIDEO_JPEG          17  /* JPEG (JFIF) */
#define VIDEO_UYVY          18  /* 4:2:2 */
#define VIDEO_MPEG          19  /* MPEG1/2 */
#define VIDEO_FMT_COUNT     20

#define FBTTV_DEF_AUTOBRIGHT   0
#define FBTTV_DEF_CHANNELSET   2 /* cableirc */
#define FBTTV_DEF_CHANNEL      3 /* local PBS */
#define MAX(x,y) ( (x) > (y) ? (x) : (y) )
#define MIN(x,y) ( (x) < (y) ? (x) : (y) )
#define array_elem(x) (sizeof(x) / sizeof( (x)[0] ))

static const struct camparam_st {
  int min, max, range, drv_min, drv_range, def;
} CamParams[] = {
  {
    BT848_BRIGHTMIN, BT848_BRIGHTMIN + BT848_BRIGHTRANGE,
    BT848_BRIGHTRANGE, BT848_BRIGHTREGMIN,
    BT848_BRIGHTREGMAX - BT848_BRIGHTREGMIN + 1, BT848_BRIGHTCENTER, },
  {
    BT848_CONTRASTMIN, (BT848_CONTRASTMIN + BT848_CONTRASTRANGE),
    BT848_CONTRASTRANGE, BT848_CONTRASTREGMIN,
    (BT848_CONTRASTREGMAX - BT848_CONTRASTREGMIN + 1),
    BT848_CONTRASTCENTER, },
  {
    BT848_CHROMAMIN, (BT848_CHROMAMIN + BT848_CHROMARANGE), BT848_CHROMARANGE,
    BT848_CHROMAREGMIN, (BT848_CHROMAREGMAX - BT848_CHROMAREGMIN + 1 ),
    BT848_CHROMACENTER, },
};

#define BRIGHT 0
#define CONTR  1
#define CHROMA 2

#define NTSC_MAX_X        640
#define NTSC_MAX_Y        480
#define PAL_MAX_X         768
#define PAL_MAX_Y         576
#define PAL                 1
#define NTSC                2
 
#define FBTTV_MAX_WIDTH     MAX(NTSC_MAX_X, PAL_MAX_X)
#define FBTTV_MAX_HEIGHT    MAX(NTSC_MAX_Y, PAL_MAX_Y)
#define FBTTV_MIN_WIDTH     2
#define FBTTV_MIN_HEIGHT    2


/* Not tested yet */

void yuv422to420p(unsigned char *map, unsigned char *cap_map, int width, int height)
{
	unsigned char *src, *dest, *src2, *dest2;
	int i, j;

	/* Create the Y plane */
	src=cap_map;
	dest=map;
	for (i=width*height; i; i--) {
		*dest++=*src;
		src+=2;
	}
	/* Create U and V planes */
	src=cap_map+1;
	src2=cap_map+width*2+1;
	dest=map+width*height;
	dest2=dest+(width*height)/4;
	for (i=height/2; i; i--) {
		for (j=width/2; j; j--) {
			*dest=((int)*src+(int)*src2)/2;
			src+=2;
			src2+=2;
			dest++;
			*dest2=((int)*src+(int)*src2)/2;
			src+=2;
			src2+=2;
			dest2++;
		}
		src+=width*2;
		src2+=width*2;
	}

}

/* FIXME seems no work with METEOR_GEO_RGB24 , check BPP as well ? */

void rgb24toyuv420p(unsigned char *map, unsigned char *cap_map, int width, int height)
{
	unsigned char *y, *u, *v;
	unsigned char *r, *g, *b;
	int i, loop;

	b=cap_map;
	g=b+1;
	r=g+1;
	y=map;
	u=y+width*height;
	v=u+(width*height)/4;
	memset(u, 0, width*height/4);
	memset(v, 0, width*height/4);

	for(loop=0; loop<height; loop++) {
		for(i=0; i<width; i+=2) {
			*y++=(9796**r+19235**g+3736**b)>>15;
			*u+=((-4784**r-9437**g+14221**b)>>17)+32;
			*v+=((20218**r-16941**g-3277**b)>>17)+32;
			r+=3;
			g+=3;
			b+=3;
			*y++=(9796**r+19235**g+3736**b)>>15;
			*u+=((-4784**r-9437**g+14221**b)>>17)+32;
			*v+=((20218**r-16941**g-3277**b)>>17)+32;
			r+=3;
			g+=3;
			b+=3;
			u++;
			v++;
		}

		if ((loop & 1) == 1) {
			u-=width/2;
			v-=width/2;
		}
	}

}


/*********************************************************************************************
                CAPTURE CARD STUFF
**********************************************************************************************/
/* NOT TESTED YET FIXME
static int camparam_normalize( int param, int cfg_value, int *ioctl_val ) 
{
	int val;

	cfg_value = MIN( CamParams[ param ].max, MAX( CamParams[ param ].min, cfg_value ) ); 
	val = (cfg_value - CamParams[ param ].min ) /
	      (CamParams[ param ].range + 0.01) * CamParams[param].drv_range + CamParams[param].drv_min;
	val = MAX( CamParams[ param ].min,
	      MIN( CamParams[ param ].drv_min + CamParams[ param ].drv_range-1,val ));
	*ioctl_val = val;
	return cfg_value;
}


static int set_chroma( struct video_dev *viddev, int new_chroma ) 
{
	int ioctlval;

	new_chroma = camparam_normalize( CHROMA, new_chroma, &ioctlval );

	if( ioctl( viddev->fd_tuner, BT848_SCSAT, &ioctlval ) < 0 ) {
		motion_log(cnt, LOG_ERR, 1, "Error setting CHROMA");
		return -1;
	}

	viddev->chroma = new_chroma;

	return 0;
}

static int set_contrast( struct video_dev *viddev, int new_contrast ) 
{
	int ioctlval;

	new_contrast = camparam_normalize( CONTR, new_contrast, &ioctlval );

	if( ioctl( viddev->fd_tuner, BT848_SCONT, &ioctlval ) < 0 ) {
		motion_log(cnt, LOG_ERR, 1, "Error setting contrast");
		return -1;
	}

	viddev->contrast = new_contrast;

	return 0;
}


// Set channel needed ? FIXME 

static int set_channel( struct video_dev *viddev, int new_channel ) 
{
	int ioctlval;

	ioctlval = new_channel;
	if( ioctl( viddev->fd_tuner, TVTUNER_SETCHNL, &ioctlval ) < 0 ) {
		motion_log(cnt, LOG_ERR, 1, "Error channel %d",ioctlval);
		return -1;
	} else {
		motion_log(cnt, LOG_DEBUG, 0, "channel set to %d",ioctlval);
	}

	viddev->channel = new_channel;
 
	return 0;
}
*/

/* set frequency to tuner */

static int set_freq(struct context *cnt, struct video_dev *viddev, unsigned long freq)
{
	int tuner_fd = viddev->fd_tuner;
	int old_audio;
 
	/* HACK maybe not need it , but seems that is needed to mute before changing frequency */

	if ( ioctl( tuner_fd, BT848_GAUDIO, &old_audio ) < 0 ) {
		motion_log(cnt, LOG_ERR, 1, "BT848_GAUDIO");
		return -1;
	}
	
	if (ioctl(tuner_fd, TVTUNER_SETFREQ, &freq) < 0){
		motion_log(cnt, LOG_ERR, 1, "Tuning (TVTUNER_SETFREQ) failed , freq [%lu]",freq);
		return -1;
	}

	old_audio &= AUDIO_MUTE;
	if ( old_audio ){
		old_audio = AUDIO_MUTE;
		if ( ioctl(tuner_fd , BT848_SAUDIO, &old_audio ) < 0 ) {
			motion_log(cnt, LOG_ERR, 1, "BT848_SAUDIO %i",old_audio);
			return -1;
		}
	}
	
	return 0;
}

/*
  set the input to capture images , could be tuner (METEOR_INPUT_DEV1)
  or any of others input :  
	RCA/COMPOSITE1 (METEOR_INPUT_DEV0) 
	COMPOSITE2/S-VIDEO (METEOR_INPUT_DEV2)
	S-VIDEO (METEOR_INPUT_DEV3)
	VBI ?! (METEOR_INPUT_DEV_SVIDEO)
*/

static int set_input(struct context *cnt, struct video_dev *viddev, int input)
{
	int actport;
	int portdata[] = { METEOR_INPUT_DEV0, METEOR_INPUT_DEV1,
	                 METEOR_INPUT_DEV2, METEOR_INPUT_DEV3,
	                 METEOR_INPUT_DEV_SVIDEO  };

	if( input >= array_elem( portdata ) ) {
		motion_log(cnt, LOG_WARNING, 0, "Channel Port %d out of range (0-4)",input);
		input = IN_DEFAULT;
	}

	actport = portdata[ input ];
	if( ioctl( viddev->fd_bktr, METEORSINPUT, &actport ) < 0 ) {
		if( input != IN_DEFAULT ) {
			motion_log(cnt, LOG_WARNING, 0,
			           "METEORSINPUT %d invalid - Trying default %d ", input, IN_DEFAULT );
			input = IN_DEFAULT;
			actport = portdata[ input ];
			if( ioctl( viddev->fd_bktr, METEORSINPUT, &actport ) < 0 ) {
				motion_log(cnt, LOG_ERR, 1, "METEORSINPUT %d init", input);
				return -1;
			}
		} else {
			motion_log(cnt, LOG_ERR, 1, "METEORSINPUT  %d init",input);
			return -1;
		}
	}

	return 0;
}

static int set_geometry(struct context *cnt, struct video_dev *viddev, int width , int height , int format)
{
	struct meteor_geomet geom;

	geom.columns = width;
	geom.rows = height;
	geom.oformat = METEOR_GEO_YUV_422 | METEOR_GEO_YUV_12;
	geom.oformat |= METEOR_GEO_EVEN_ONLY;	
	geom.frames  = 1;

	if( ioctl( viddev->fd_bktr, METEORSETGEO, &geom ) < 0 ) {
		motion_log(cnt, LOG_ERR, 1, "Couldn't set the geometry");
		return -1;
	}

	return 0;
}

/*
   set input format ( NTSC , PAL , SECAM , etc ... ) not all supported yet FIXME
*/

static int set_input_format(struct context *cnt, struct video_dev *viddev, int newformat) 
{
	int format;
	int input_format[] = { NORM_PAL, NORM_NTSC, NORM_SECAM };
 
	if( newformat >= array_elem( input_format ) ) {
		motion_log(cnt, LOG_WARNING, 0, "Input format %d out of range (0-2)",newformat );
		format = NORM_DEFAULT;newformat = 0;
	} else
		format = input_format[newformat]; 

	/* FIXME should be BT848SFMT for new BKTR INTERFACES */

	if( ioctl( viddev->fd_bktr, METEORSFMT, &format ) < 0 ) {
		motion_log(cnt, LOG_ERR, 1, "METEORSFMT Couldn't set the input format");
		return -1;
	}
	return 0;
}

/* NOT TESTED YET FIXME
static int set_brightness( struct video_dev *viddev, int new_bright ) 
{
	int ioctlval;

	new_bright = camparam_normalize( BRIGHT, new_bright, &ioctlval );
	if( ioctl( viddev->fd_tuner, BT848_SBRIG, &ioctlval ) < 0 ) {
		motion_log(cnt, LOG_ERR, 1, " BT848_SBRIG brightness %d",ioctlval);
		return -1;
	}
	viddev->brightness = new_bright;

	return 0;
}



statict int setup_pixelformat( int bktr )
{
	int i;
	struct meteor_pixfmt p;
	int format=-1;

	for( i=0; ; i++ ){
		p.index = i;
		if( ioctl( bktr, METEORGSUPPIXFMT, &p ) < 0 ){
			if( errno == EINVAL )
				break;
			motion_log(cnt, LOG_ERR, 1, "METEORGSUPPIXFMT getting pixformat %d",i);	
			return -1;
		}


	// Hack from xawtv 4.x 

	switch ( p.type ){
		case METEOR_PIXTYPE_RGB:
			motion_log(cnt, -1, 0, "setup_pixelformat METEOR_PIXTYPE_RGB");
			switch(p.masks[0]) {
				case 31744: // 15 bpp 
					format = p.swap_bytes ? VIDEO_RGB15_LE : VIDEO_RGB15_BE;
					motion_log(cnt, -1, 0, "setup_pixelformat METEOR_PIXTYPE_RGB VIDEO_RGB15");
				break;
				case 63488: // 16 bpp 
					format = p.swap_bytes ? VIDEO_RGB16_LE : VIDEO_RGB16_BE;
					motion_log(cnt, -1, 0, "setup_pixelformat METEOR_PIXTYPE_RGB VIDEO_RGB16");
				break;
				case 16711680: // 24/32 bpp 
					if (p.Bpp == 3 && p.swap_bytes == 1) {
						format = VIDEO_BGR24;
						motion_log(cnt, -1, 0, "setup_pixelformat METEOR_PIXTYPE_RGB VIDEO_BGR24");
					} else if (p.Bpp == 4 && p.swap_bytes == 1 && p.swap_shorts == 1) {
						format = VIDEO_BGR32;
						motion_log(cnt, -1, 0, "setup_pixelformat METEOR_PIXTYPE_RGB VIDEO_BGR32");
						} else if (p.Bpp == 4 && p.swap_bytes == 0 && p.swap_shorts == 0) {
							format = VIDEO_RGB32;
							motion_log(cnt, -1, 0, "setup_pixelformat METEOR_PIXTYPE_RGB VIDEO_RGB32");
						}
					}
				break;
				case METEOR_PIXTYPE_YUV:
					format = VIDEO_YUV422P;
					motion_log(cnt, -1, 0, "setup_pixelformat METEOR_PIXTYPE_YUV");
				break;
				case METEOR_PIXTYPE_YUV_12:
					format = VIDEO_YUV422P;
					motion_log(cnt, -1, 0, "setup_pixelformat METEOR_PIXTYPE_YUV_12");
					break;
				case METEOR_PIXTYPE_YUV_PACKED:
					format = VIDEO_YUV422P;
					motion_log(cnt, -1, 0, "setup_pixelformat METEOR_PIXTYPE_YUV_PACKED");
				break;
	
			}

			if( p.type == METEOR_PIXTYPE_RGB && p.Bpp == 3 ){
				// Found a good pixeltype -- set it up 
				if( ioctl( bktr, METEORSACTPIXFMT, &i ) < 0 ){
					motion_log(cnt, LOG_WARNING, 1, "METEORSACTPIXFMT etting pixformat METEOR_PIXTYPE_RGB Bpp == 3");
				// Not immediately fatal 
				}
				motion_log(cnt, LOG_DEBUG, 0, "input format METEOR_PIXTYPE_RGB %i",i);
				format=i;
			}

			if( p.type == METEOR_PIXTYPE_YUV_PACKED ){
				// Found a good pixeltype -- set it up
				if( ioctl( bktr, METEORSACTPIXFMT, &i ) < 0 ){
					motion_log(cnt, LOG_WARNING, 1, "METEORSACTPIXFMT setting pixformat METEOR_PIXTYPE_YUV_PACKED");
				// Not immediately fatal
				} 
				motion_log(cnt, LOG_DEBUG, 0, "input format METEOR_PIXTYPE_YUV_PACKED %i",i);
				format=i;
			}

		}

	return format;
}

*/

/*******************************************************************************************
	Video capture routines

 - set input
 - setup_pixelformat
 - set_geometry

 - set_brightness 
 - set_chroma
 - set_contrast
 - set_channelset
 - set_channel
 - set_capture_mode

*/
static char *v4l_start(struct context *cnt, struct video_dev *viddev, int width, int height, int input, int norm, unsigned long freq)
{
	int dev_bktr=viddev->fd_bktr;
	//int dev_tunner=viddev->fd_tuner;
	/* to ensure that all device will be support the capture mode 
	  _TODO_ : Autodected the best capture mode .
	*/
	int single  = METEOR_CAP_SINGLE;
	char *map;

	if ( ((freq <= 0) && (input == IN_TV)) || ( set_input(cnt, viddev, input) == -1) ) {
		motion_log(cnt, LOG_ERR, 1, "Frequency [%lu] Source input [%i]",freq,input);
		return (NULL);
	}
 
	/* if we have choose the tuner is needed to setup the frequency */
	if ( input == IN_TV ) {
		if (set_freq(cnt, viddev, freq) == -1) {
			return (NULL);
		}
	}
	
	/* FIXME if we set as input tuner , we need to set option for tuner not for bktr */

	if ( set_input_format(cnt, viddev, norm) == -1 ) {
		return (NULL);
	}

	if (set_geometry(cnt, viddev, width, height, norm) == -1) {
		return (NULL);
	}

/*
	if ( setup_pixelformat(viddev) == -1) {
		return (NULL);
	}

	set_brightness(viddev,BRIGHT);
	set_chroma(viddev,CHROMA);
	set_contrast(viddev,CONTR);
*/

	if (freq != 0) {
	/*
	 TODO missing implementation
		set_channelset(viddev);
		set_channel(viddev);
		if (set_freq (cnt, viddev, freq) == -1) {
			return (NULL);
		}
	*/
	}


	/* set capture mode and capture buffers */

	/* That is the buffer size for capture images ,
	 so is dependent of color space of input format / FIXME */

	viddev->v4l_bufsize = (((width*height*3/2)) * sizeof(unsigned char *));
	viddev->v4l_fmt = VIDEO_PALETTE_YUV420P;

	map = mmap((caddr_t)0,viddev->v4l_bufsize,PROT_READ|PROT_WRITE,MAP_SHARED, dev_bktr, (off_t)0);

	if ((unsigned char *)-1 == (unsigned char *)map) {
		motion_log(cnt, LOG_ERR, 1, "mmap failed");
		return (NULL);
	}

	/* FIXME double buffer */ 
	if (0) {
		viddev->v4l_maxbuffer=2;
		viddev->v4l_buffers[0]=map;
		viddev->v4l_buffers[1]=map+0; /* 0 is not valid just a test */
		//viddev->v4l_buffers[1]=map+vid_buf.offsets[1];
	} else {
		viddev->v4l_buffers[0]=map;
		viddev->v4l_maxbuffer=1;
	}

	viddev->v4l_curbuffer=0;

	// settle , sleep(1) replaced
	SLEEP(1,0)

	/* capture */
	if (ioctl(dev_bktr, METEORCAPTUR, &single) < 0){
		motion_log(cnt, LOG_ERR, 1, "METEORCAPTUR Error capturing");
	}
 	
	
	/* Palete Color space */
	/* FIXME*/
	switch (viddev->v4l_fmt) {
		case VIDEO_PALETTE_YUV420P:
			viddev->v4l_bufsize=(width*height*3)/2;
			motion_log(cnt, -1, 0, "VIDEO_PALETTE_YUV420P palette setting bufsize");
			break;
		case VIDEO_PALETTE_YUV422:
			viddev->v4l_bufsize=(width*height*2);
			break;
		case VIDEO_PALETTE_RGB24:
			viddev->v4l_bufsize=(width*height*3);
			break;
		case VIDEO_PALETTE_GREY:
			viddev->v4l_bufsize=width*height;
			break;
	}

	return map;
}


/**
 * v4l_next fetches a video frame from a v4l device
 * Parameters:
 *     cnt        Pointer to the context for this thread
 *     viddev     Pointer to struct containing video device handle
 *     map        Pointer to the buffer in which the function puts the new image
 *     width      Width of image in pixels
 *     height     Height of image in pixels
 *
 * Returns
 *     0          Success
 *    -1          Fatal error
 *     1          Non fatal error (not implemented)
 */
int v4l_next(struct context *cnt, struct video_dev *viddev, char *map, int width, int height)
{
	int dev_bktr=viddev->fd_bktr;
	char *cap_map=NULL;
	int single  = METEOR_CAP_SINGLE;
	sigset_t  set, old;


	if (viddev->v4l_read_img) {
	/* use read to get capture the image if mmap not supported */	
		if (read(dev_bktr, map, viddev->v4l_bufsize) != viddev->v4l_bufsize) {
			motion_log(cnt, LOG_ERR, 0, "capturing frame");
			return -1;
		}	
	} else {
	/* Allocate a new mmap buffer */
		/* Block signals during IOCTL */
		sigemptyset (&set);
		sigaddset (&set, SIGCHLD);
		sigaddset (&set, SIGALRM);
		sigaddset (&set, SIGUSR1);
		sigaddset (&set, SIGTERM);
		sigaddset (&set, SIGHUP);
		pthread_sigmask (SIG_BLOCK, &set, &old);
		cap_map=viddev->v4l_buffers[viddev->v4l_curbuffer];

		viddev->v4l_curbuffer++;
		if (viddev->v4l_curbuffer >= viddev->v4l_maxbuffer)
			viddev->v4l_curbuffer=0;

		/* capture */
		if (ioctl(dev_bktr, METEORCAPTUR, &single) < 0) {
			motion_log(cnt, LOG_ERR, 1, "Error capturing");
			sigprocmask (SIG_UNBLOCK, &old, NULL);
			return (-1);
		}

		pthread_sigmask (SIG_UNBLOCK, &old, NULL);	/*undo the signal blocking*/
	}

	if (!viddev->v4l_read_img) 
	{
		switch (viddev->v4l_fmt){
			case VIDEO_PALETTE_RGB24:
				rgb24toyuv420p(map, cap_map, width, height);
				break;
			case VIDEO_PALETTE_YUV422:
				yuv422to420p(map, cap_map, width, height);
				break;
			default:
				memcpy(map, cap_map, viddev->v4l_bufsize);
		}
	}
	
	return 0;
}


/* set input & freq if needed FIXME not allowed use Tuner yet */

void v4l_set_input(struct context *cnt, struct video_dev *viddev, char *map, int width, int height,
                    int input, int norm, int skip, unsigned long freq)
{
	int i;
	unsigned long frequnits = freq;


	if (input != viddev->input || width != viddev->width || height!=viddev->height || freq!=viddev->freq){ 
		motion_log(cnt, LOG_WARNING, 0, "v4l_set_input really needed ?");

		if (set_input(cnt, viddev, input) == -1)
			return;

		if (set_input_format(cnt, viddev, norm) == -1 )
			return;
		
		if (( input == IN_TV ) && (frequnits > 0)) {
			if (set_freq (cnt, viddev, freq) == -1)
				return ;
		}

		// FIXME
		/*
		if ( setup_pixelformat(viddev) == -1) {
			motion_log(cnt, LOG_ERR, 1, "ioctl (VIDIOCSFREQ)");
			return;
		}
		*/

		if (set_geometry(cnt, viddev, width, height, norm) == -1)
			return;
		
		viddev->input=input;
		viddev->width=width;
		viddev->height=height;
		viddev->freq=freq;

		/* skip a few frames if needed */
		for (i=0; i<skip; i++)
			v4l_next(cnt, viddev, map, width, height);
	}
}

/*****************************************************************************
	Wrappers calling the actual capture routines
 *****************************************************************************/

/*

vid_init - Allocate viddev struct
vid_start - Setup Device parameters ( device , channel , freq , contrast , hue , croma , brightness ) and open it
vid_next - 
vid_close - close devices 
vid_cleanup - Free viddev struct

*/

/* big lock for vid_start */
pthread_mutex_t vid_mutex;
/* structure used for per device locking */
struct video_dev **viddevs=NULL;

void vid_init(struct context *cnt)
{
	if (!viddevs) { 
		viddevs=mymalloc(cnt, sizeof(struct video_dev *));
		viddevs[0]=NULL;
	}

	pthread_mutex_init(&vid_mutex, NULL);
}

/* Called by childs to get rid of open video devices */
void vid_close(void)
{
	int i=-1;
//	int stopcap=0;

	if (viddevs){ 
		while(viddevs[++i]){
		/*
			stopcap = METEOR_CAP_STOP_CONT;
			if( ioctl( viddevs[++i]->fd_bktr, METEORCAPTUR, &stopcap ) < 0 )
				motion_log(cnt,LOG_ERR , 1, "vid_close METEORCAPTUR");	
		*/
			close(viddevs[i]->fd_bktr);
			close(viddevs[i]->fd_tuner);
		}
	}
}

void vid_cleanup(void)
{
	int i=-1;
	if (viddevs){ 
		while(viddevs[++i]){
			munmap(viddevs[i]->v4l_buffers[0],viddevs[i]->v4l_bufsize);
			free(viddevs[i]);
		}
		free(viddevs);
		viddevs=NULL;
	}
}

#endif /*WITHOUT_V4L*/


int vid_start(struct context *cnt)
{
	struct config *conf=&cnt->conf;
	int fd_bktr=-1,fd_tuner=-1;
	
	if (conf->netcam_url)
		return netcam_start(cnt);

#ifndef WITHOUT_V4L
	{
		int i=-1;
		int width, height, input, norm;
		unsigned long frequency;

		/* We use width and height from conf in this function. They will be assigned
		 * to width and height in imgs here, and cap_width and cap_height in 
		 * rotate_data won't be set until in rotate_init.
		 */
		width = conf->width;
		height = conf->height;
		input = conf->input;
		norm = conf->norm;
		frequency = conf->frequency;

		pthread_mutex_lock(&vid_mutex);

		/* Transfer width and height from conf to imgs. The imgs values are the ones
		 * that is used internally in Motion. That way, setting width and height via
		 * xmlrpc won't screw things up.
		 */
		cnt->imgs.width=width;
		cnt->imgs.height=height;

		while (viddevs[++i]) { 
			if (!strcmp(conf->video_device, viddevs[i]->video_device)) {
				int fd;
				cnt->imgs.type=viddevs[i]->v4l_fmt;
				motion_log(cnt, -1, 0, "vid_start cnt->imgs.type [%i]", cnt->imgs.type);
				switch (cnt->imgs.type) {
					case VIDEO_PALETTE_GREY:
						cnt->imgs.motionsize=width*height;
						cnt->imgs.size=width*height;
					break;
					case VIDEO_PALETTE_RGB24:
					case VIDEO_PALETTE_YUV422:
						cnt->imgs.type=VIDEO_PALETTE_YUV420P;
					case VIDEO_PALETTE_YUV420P:
						motion_log(cnt, -1, 0, " VIDEO_PALETTE_YUV420P setting imgs.size and imgs.motionsize");
						cnt->imgs.motionsize=width*height;
						cnt->imgs.size=(width*height*3)/2;
					break;
				}
				fd=viddevs[i]->fd_bktr; // FIXME return fd_tuner ?!
				pthread_mutex_unlock(&vid_mutex);
				return fd;
			}
		}

		viddevs=myrealloc(cnt, viddevs, sizeof(struct video_dev *)*(i+2), "vid_start");
		viddevs[i]=mymalloc(cnt, sizeof(struct video_dev));
		viddevs[i+1]=NULL;

		pthread_mutexattr_init(&viddevs[i]->attr);
		pthread_mutex_init(&viddevs[i]->mutex, NULL);

		fd_bktr=open(conf->video_device, O_RDWR);
		if (fd_bktr <0) { 
			motion_log(cnt, LOG_ERR, 1, "open video device %s",conf->video_device);
			motion_log(cnt, LOG_ERR, 0, "Motion Exits.");
			exit(1);
		}

		/* Only open tuner if freq > 0 FIXME ?! */
		if (( frequency > 0 ) && ( input = IN_TV )) {
			fd_tuner=open(conf->tuner_device, O_RDWR);
			if (fd_bktr <0) { 
				motion_log(cnt, LOG_ERR, 1, "open tuner device %s",conf->tuner_device);
				motion_log(cnt, LOG_ERR, 0, "Motion Exits.");
				exit(1);
			}
		}

		viddevs[i]->video_device=conf->video_device;
		viddevs[i]->tuner_device=conf->tuner_device;
		viddevs[i]->fd_bktr=fd_bktr;
		viddevs[i]->fd_tuner=fd_tuner;
		viddevs[i]->input=input;
		viddevs[i]->height=height;
		viddevs[i]->width=width;
		viddevs[i]->freq=frequency;
		viddevs[i]->owner=-1;

		/* default palette */ 
		viddevs[i]->v4l_fmt=VIDEO_PALETTE_YUV420P;
		viddevs[i]->v4l_read_img=0;
		viddevs[i]->v4l_curbuffer=0;
		viddevs[i]->v4l_maxbuffer=1;

		if (!v4l_start (cnt, viddevs[i], width, height, input, norm, frequency)){ 
			pthread_mutex_unlock(&vid_mutex);
			return -1;
		}
	
		cnt->imgs.type=viddevs[i]->v4l_fmt;
	
		switch (cnt->imgs.type) { 
			case VIDEO_PALETTE_GREY:
				cnt->imgs.size=width*height;
				cnt->imgs.motionsize=width*height;
			break;
			case VIDEO_PALETTE_RGB24:
			case VIDEO_PALETTE_YUV422:
				cnt->imgs.type=VIDEO_PALETTE_YUV420P;
			case VIDEO_PALETTE_YUV420P:
				motion_log(cnt, -1, 0, "VIDEO_PALETTE_YUV420P imgs.type");
				cnt->imgs.size=(width*height*3)/2;
				cnt->imgs.motionsize=width*height;
			break;
		}
	
		pthread_mutex_unlock(&vid_mutex);
	}
#endif /*WITHOUT_V4L*/

	/* FIXME needed tuner device ?! */
	return fd_bktr;
}


/**
 * vid_next fetches a video frame from a either v4l device or netcam
 * Parameters:
 *     cnt        Pointer to the context for this thread
 *     map        Pointer to the buffer in which the function puts the new image
 *
 * Returns
 *     0          Success
 *    -1          Fatal V4L error
 *    -2          Fatal Netcam error
 *     1          Non fatal V4L error (not implemented)
 *     2          Non fatal Netcam error
 */
int vid_next(struct context *cnt, char *map)
{
	struct config *conf=&cnt->conf;
	int ret = -1;

	if (conf->netcam_url) {
		ret = netcam_next(cnt, map);
		return ret;
	}

#ifndef WITHOUT_V4L
	
	int i=-1;
	int width, height;
	int dev_bktr = cnt->video_dev;

	/* NOTE: Since this is a capture, we need to use capture dimensions. */
	width = cnt->rotate_data.cap_width;
	height = cnt->rotate_data.cap_height;
		
	while (viddevs[++i])
		if (viddevs[i]->fd_bktr==dev_bktr)
			break;

	if (!viddevs[i])
		return -1;

	if (viddevs[i]->owner!=cnt->threadnr) { 
		pthread_mutex_lock(&viddevs[i]->mutex);
		viddevs[i]->owner=cnt->threadnr;
		viddevs[i]->frames=conf->roundrobin_frames;
		//viddevs[i]->frames=1;
		cnt->switched=1;
	}


	v4l_set_input(cnt, viddevs[i], map, width, height, conf->input, conf->norm,
	              conf->roundrobin_skip, conf->frequency);
	ret = v4l_next(cnt, viddevs[i], map, width, height);

	if (--viddevs[i]->frames <= 0) { 
		viddevs[i]->owner=-1;
		pthread_mutex_unlock(&viddevs[i]->mutex);
	}
	
 	if(cnt->rotate_data.degrees > 0){ 
		/* rotate the image as specified */
		rotate_map(cnt, map);
 	}
	
#endif /*WITHOUT_V4L*/
	return ret;
}
