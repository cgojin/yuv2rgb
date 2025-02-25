// Copyright 2016 Adrien Descamps
// Distributed under BSD 3-Clause License

// This program demonstrate how to convert a YUV420p image (raw format) to RGB (ppm format), and the reverse operation

#include "yuv_rgb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__x86_64__)
#include <x86intrin.h>
#else
#define _mm_malloc(a, b) malloc(a)
#define _mm_free(a) free(a)
#endif

#if USE_FFMPEG
#include <libswscale/swscale.h>
#endif
#if USE_IPP
#include <ippcc.h>
#endif

// read a raw yuv image file
// raw yuv files can be generated by ffmpeg, for example, using :
//  ffmpeg -i test.png -c:v rawvideo -pix_fmt yuv420p test.yuv
// the returned image channels are contiguous, and Y stride=width, U and V stride=width/2
// memory must be freed with free
int readRawYUV(const char *filename, uint32_t width, uint32_t height, uint8_t **YUV)
{
	FILE *fp = fopen(filename, "rb");
	if(!fp)
	{
		perror("Error opening yuv image for read");
		return 1;
	}
	
	// check file size
	fseek(fp, 0, SEEK_END);
	uint32_t size = ftell(fp);
	if(size!=(width*height + 2*((width+1)/2)*((height+1)/2)))
	{
		fprintf(stderr, "Wrong size of yuv image : %d bytes, expected %d bytes\n", size, (width*height + 2*((width+1)/2)*((height+1)/2)));
		fclose(fp);
		return 2;
	}
	fseek(fp, 0, SEEK_SET);
	
	*YUV = malloc(size);
	size_t result = fread(*YUV, 1, size, fp);
	if (result != size) {
		perror("Error reading yuv image");
		fclose(fp);
		return 3;
	}
	fclose(fp);
	return 0;
}

// write a raw yuv image file
int saveRawYUV(const char *filename, uint32_t width, uint32_t height, const uint8_t *YUV, size_t y_stride, size_t uv_stride)
{
	FILE *fp = fopen(filename, "wb");
	if(!fp)
	{
		perror("Error opening yuv image for write");
		return 1;
	}
	
	if(y_stride==width)
	{
		fwrite(YUV, 1, width*height, fp);
		YUV+=width*height;
	}
	else
	{
		for(uint32_t y=0; y<height; ++y)
		{
			fwrite(YUV, 1, width, fp);
			YUV+=y_stride;
		}
	}
	
	
	if(uv_stride==(width+1/2))
	{
		fwrite(YUV, 1, ((width+1)/2)*((height+1)/2)*2, fp);
	}
	else
	{
		for(uint32_t y=0; y<((height+1)/2); ++y)
		{
			fwrite(YUV, 1, ((width+1)/2), fp);
			YUV+=uv_stride;
		}
		
		for(uint32_t y=0; y<((height+1)/2); ++y)
		{
			fwrite(YUV, 1, ((width+1)/2), fp);
			YUV+=uv_stride;
		}
	}
	
	fclose(fp);
	return 0;
}

// read a ppm binary image file
// memory must be freed with free
int readPPM(const char* filename, uint32_t *width, uint32_t *height, uint8_t **RGB)
{
	FILE *fp = fopen(filename, "rb");
	if(!fp)
	{
		perror("Error opening rgb image for read");
		return 1;
	}
	
	char magic[3];
	size_t result = fread(magic, 1, 2, fp);
	magic[2]='\0';
	if(result!=2 || strcmp(magic,"P6")!=0)
	{
		perror("Error reading rgb image header, or invalid format");
		fclose(fp);
		return 3;
	}
	
	uint32_t max;
	result = fscanf(fp, " %u %u %u ", width, height, &max);
	if(result!=3 || max>255)
	{
		perror("Error reading rgb image header, or invalid values");
		fclose(fp);
		return 3;
	}
	
	size_t size = 3*(*width)*(*height);
	*RGB = malloc(size);
	if(!*RGB)
	{
		perror("Error allocating rgb image memory");
		fclose(fp);
		return 2;
	}
	
	result = fread(*RGB, 1, size, fp);
	if(result != size)
	{
		perror("Error reading rgb image");
		fclose(fp);
		return 3;
	}
	
	fclose(fp);
	return 0;
}

// save a rgb image to ppm binary format
int savePPM(const char* filename, uint32_t width, uint32_t height, const uint8_t *RGB, size_t stride)
{
	FILE *fp = fopen(filename, "wb");
	if(!fp)
	{
		perror("Error opening rgb image for write");
		return 1;
	}
	
	fprintf(fp, "P6 %u %u 255\n", width, height);
	if(stride==(3*width))
	{
		fwrite(RGB, 1, 3*width*height, fp);
	}
	else
	{
		for(uint32_t i=0; i<height; ++i)
		{
			fwrite(RGB+i*stride, 1, 3*width, fp);
		}
	}
	fclose(fp);
	
	return 0;
}

void convert_rgb_to_rgba(const uint8_t *RGB, uint32_t width, uint32_t height, uint8_t **RGBA)
{
	*RGBA = malloc(4*width*height);
	for(uint32_t y=0; y<height; ++y)
	{
		for(uint32_t x=0; x<width; ++x)
		{
			(*RGBA)[(y*width+x)*4] = RGB[(y*width+x)*3];
			(*RGBA)[(y*width+x)*4+1] = RGB[(y*width+x)*3+1];
			(*RGBA)[(y*width+x)*4+2] = RGB[(y*width+x)*3+2];
			(*RGBA)[(y*width+x)*4+3] = 0;
		}
	}
}

typedef enum
{
	RGB2YUV,
	YUV2RGB,
	YUV2RGB_NV12,
	YUV2RGB_NV21,
	RGBA2YUV
} Mode;

typedef void (*yuv2rgb_ptr)(
	uint32_t width, uint32_t height, 
	const uint8_t *y, const uint8_t *u, const uint8_t *v, uint32_t y_stride, uint32_t uv_stride, 
	uint8_t *rgb, uint32_t rgb_stride, 
	YCbCrType yuv_type);

typedef void (*yuvsp2rgb_ptr)(
	uint32_t width, uint32_t height, 
	const uint8_t *y, const uint8_t *uv, uint32_t y_stride, uint32_t uv_stride, 
	uint8_t *rgb, uint32_t rgb_stride, 
	YCbCrType yuv_type);

typedef void (*rgb2yuv_ptr)(
	uint32_t width, uint32_t height, 
	const uint8_t *rgb, uint32_t rgb_stride, 
	uint8_t *y, uint8_t *u, uint8_t *v, uint32_t y_stride, uint32_t uv_stride, 
	YCbCrType yuv_type);


// call yuv2rgb conversion function, time it and save result
void test_yuv2rgb(uint32_t width, uint32_t height, 
	const uint8_t *y, const uint8_t *u, const uint8_t *v, uint32_t y_stride, uint32_t uv_stride,
	uint8_t *rgb, uint32_t rgb_stride, YCbCrType yuv_type,
	const char *file, const char *name, uint32_t iteration_number, const yuv2rgb_ptr yuv2rgb_fun)
{
	clock_t t = clock();
	for(uint32_t i=0;i<iteration_number; ++i)
		yuv2rgb_fun(width, height, y, u, v, y_stride, uv_stride, rgb, rgb_stride, yuv_type);
	t = clock()-t;
	printf("Processing time (%s) : %f sec\n", name, ((float)t)/CLOCKS_PER_SEC);
	
	char *out_filename = malloc(strlen(file)+strlen(name)+6);
	strcpy(out_filename, file);
	strcat(out_filename, "_");
	strcat(out_filename, name);
	strcat(out_filename, ".ppm");
	savePPM(out_filename, width, height, rgb, rgb_stride);
	free(out_filename);
}

// call yuv2rgb semi planar conversion function, time it and save result
void test_yuvsp2rgb(uint32_t width, uint32_t height, 
	const uint8_t *y, const uint8_t *uv, uint32_t y_stride, uint32_t uv_stride,
	uint8_t *rgb, uint32_t rgb_stride, YCbCrType yuv_type,
	const char *file, const char *name, uint32_t iteration_number, const yuvsp2rgb_ptr yuv2rgb_fun)
{
	clock_t t = clock();
	for(uint32_t i=0;i<iteration_number; ++i)
		yuv2rgb_fun(width, height, y, uv, y_stride, uv_stride, rgb, rgb_stride, yuv_type);
	t = clock()-t;
	printf("Processing time (%s) : %f sec\n", name, ((float)t)/CLOCKS_PER_SEC);
	
	char *out_filename = malloc(strlen(file)+strlen(name)+6);
	strcpy(out_filename, file);
	strcat(out_filename, "_");
	strcat(out_filename, name);
	strcat(out_filename, ".ppm");
	savePPM(out_filename, width, height, rgb, rgb_stride);
	free(out_filename);
}


// call rgb2yuv conversion function, time it and save result
void test_rgb2yuv(uint32_t width, uint32_t height, 
	const uint8_t *rgb, uint32_t rgb_stride, 
	uint8_t *y, uint8_t *u, uint8_t *v, uint32_t y_stride, uint32_t uv_stride, YCbCrType yuv_type,
	const char *file, const char *name, uint32_t iteration_number, const rgb2yuv_ptr rgb2yuv_fun)
{
	clock_t t = clock();
	for(uint32_t i=0;i<iteration_number; ++i)
		rgb2yuv_fun(width, height, rgb, rgb_stride, y, u, v, y_stride, uv_stride, yuv_type);
	t = clock()-t;
	printf("Processing time (%s) : %f sec\n", name, ((float)t)/CLOCKS_PER_SEC);
	
	char *out_filename = malloc(strlen(file)+strlen(name)+6);
	strcpy(out_filename, file);
	strcat(out_filename, "_");
	strcat(out_filename, name);
	strcat(out_filename, ".yuv");
	saveRawYUV(out_filename, width, height, y, y_stride, uv_stride);
	free(out_filename);
}

// equivalent conversion functions for external libraries

#if USE_FFMPEG
static struct SwsContext *yuv2rgb_swscale_ctx = NULL;
static struct SwsContext *rgb2yuv_swscale_ctx = NULL;

void yuv420_rgb24_ffmpeg(uint32_t __attribute__ ((unused)) width, uint32_t height, 
	const uint8_t *y, const uint8_t *u, const uint8_t *v, uint32_t y_stride, uint32_t uv_stride, 
	uint8_t *rgb, uint32_t rgb_stride, 
	YCbCrType __attribute__ ((unused)) yuv_type)
{
	const uint8_t *const inData[3] = {y, u, v};
	int inLinesize[3] = {y_stride, uv_stride, uv_stride};
	int outLinesize[1] = {rgb_stride};
	sws_scale(yuv2rgb_swscale_ctx, inData, inLinesize, 0, height, &rgb, outLinesize);
}

void rgb24_yuv420_ffmpeg(uint32_t __attribute__ ((unused)) width, uint32_t height, 
	const uint8_t *rgb, uint32_t rgb_stride, 
	uint8_t *y, uint8_t *u, uint8_t *v, uint32_t y_stride, uint32_t uv_stride, 
	YCbCrType __attribute__ ((unused)) yuv_type)
{
	int inLineSize[1] = {rgb_stride};
	int outLineSize[3] = {y_stride, uv_stride, uv_stride};
	uint8_t *const outData[3] = {y, u, v};
	sws_scale(rgb2yuv_swscale_ctx, &rgb, inLineSize, 0, height, outData, outLineSize);
}
#endif

#if USE_IPP
void yuv420_rgb24_ipp(uint32_t width, uint32_t height, 
	const uint8_t *y, const uint8_t *u, const uint8_t *v, uint32_t y_stride, uint32_t uv_stride, 
	uint8_t *rgb, uint32_t rgb_stride, 
	YCbCrType __attribute__ ((unused)) yuv_type)
{
	const Ipp8u* pSrc[3] = {y, u, v};
	int srcStep[3] = {y_stride, uv_stride, uv_stride};
	Ipp8u* pDst = rgb;
	int dstStep = rgb_stride;
	IppiSize imgSize = {.width=width, .height=height};
	ippiYCbCr420ToRGB_8u_P3C3R(pSrc, srcStep, pDst, dstStep, imgSize);
}

void rgb24_yuv420_ipp(uint32_t width, uint32_t height, 
	const uint8_t *rgb, uint32_t rgb_stride, 
	uint8_t *y, uint8_t *u, uint8_t *v, uint32_t y_stride, uint32_t uv_stride, 
	YCbCrType __attribute__ ((unused)) yuv_type)
{
	const Ipp8u* pSrc = rgb;
	int srcStep = rgb_stride;
	Ipp8u* pDst[3] = {y, u, v};
	int dstStep[3] = {y_stride, uv_stride, uv_stride};
	IppiSize imgSize = {.width=width, .height=height};
	ippiRGBToYCbCr420_8u_C3P3R(pSrc, srcStep, pDst, dstStep, imgSize);
}

#endif

int main(int argc, char **argv)
{
	if(argc<4)
	{
		printf("Usage : test yuv2rgb <yuv image file> <image width> <image height> <output template filename>\n");
		printf("Or    : test yuv2rgb_nv12 <yuv image file> <image width> <image height> <output template filename>\n");
		printf("Or    : test yuv2rgb_nv21 <yuv image file> <image width> <image height> <output template filename>\n");
		printf("Or    : test rgb2yuv <rgb24 binary ppm image file> <output template filename>\n");
		printf("Or    : test rgba2yuv <rgb24 binary ppm image file> <output template filename>\n");
		return 1;
	}
	
	const int iteration_number = 100;
	printf("Time will be measured in each configuration for %d iterations...\n", iteration_number);
	const YCbCrType yuv_format = YCBCR_601;
	//const YCbCrType yuv_format = YCBCR_709;
	//const YCbCrType yuv_format = YCBCR_JPEG;
	
	Mode mode;
	if(strcmp(argv[1], "yuv2rgb")==0)
	{
		mode=YUV2RGB;
		if(argc<6)
		{
			printf("Invalid argument number for yuv2rgb mode, call without argument to see usage.\n");
			return 1;
		}
	}
	else if(strcmp(argv[1], "yuv2rgb_nv12")==0)
	{
		mode=YUV2RGB_NV12;
	}
	else if(strcmp(argv[1], "yuv2rgb_nv21")==0)
	{
		mode=YUV2RGB_NV21;
	}
	else if(strcmp(argv[1], "rgb2yuv")==0)
	{
		mode=RGB2YUV;
	}
	else if(strcmp(argv[1], "rgba2yuv")==0)
	{
		mode=RGBA2YUV;
	}
	else
	{
		printf("Invalid mode, call without argument to see usage.\n");
		return 1;
	}
	
	const char *filename = argv[2];
	uint32_t width, height;
	const char *out;
	uint8_t *YUV=NULL, *RGB=NULL, *Y=NULL, *U=NULL, *V=NULL, *RGBa=NULL, *YUVa=NULL, *Ya=NULL, *Ua=NULL, *Va=NULL;
	
	if(mode==YUV2RGB || mode==YUV2RGB_NV12 ||  mode==YUV2RGB_NV21)
	{
		//parse argument line
		width = atoi(argv[3]);
		height = atoi(argv[4]);
		out = argv[5];
		
		// read input data and allocate output data
		if(readRawYUV(filename, width, height, &YUV)!=0)
		{
			printf("Error reading image file, check that the file exists and has the correct format and resolution.\n");
			return 1;
		}
		
#if USE_FFMPEG
		yuv2rgb_swscale_ctx = sws_getContext(width, height, AV_PIX_FMT_YUV420P, width, height, AV_PIX_FMT_RGB24, 0, 0, 0, 0);
#endif
		
		RGB = malloc(3*width*height);
		
		Y = YUV;
		U = YUV+width*height;
		V = YUV+width*height+((width+1)/2)*((height+1)/2);
		
		// allocate aligned data
		const size_t y_stride = width + (16-width%16)%16;
		const size_t uv_stride = (mode==YUV2RGB) ? (width+1)/2 + (16-((width+1)/2)%16)%16 : y_stride;
		const size_t rgb_stride = width*3 +(16-(3*width)%16)%16;
	
		const size_t y_size = y_stride*height, uv_size = uv_stride*((height+1)/2);
		YUVa = _mm_malloc(y_size+2*uv_size, 16);
		Ya = YUVa;
		Ua = YUVa+y_size;
		Va = YUVa+y_size+uv_size;
		for(unsigned int i=0; i<height; ++i)
		{
			memcpy(Ya+i*y_stride, Y+i*width, width);
			if((i%2)==0)
			{
				if(mode==YUV2RGB)
				{
					memcpy(Ua+(i/2)*uv_stride, U+(i/2)*((width+1)/2), (width+1)/2);
					memcpy(Va+(i/2)*uv_stride, V+(i/2)*((width+1)/2), (width+1)/2);
				}
				else
				{
					memcpy(Ua+(i/2)*uv_stride, U+(i/2)*width, width);
				}
			}
		}
		
		RGBa = _mm_malloc(rgb_stride*height, 16);
		
		// test all versions
		if(mode==YUV2RGB)
		{
			test_yuv2rgb(width, height, Y, U, V, width, (width+1)/2, RGB, width*3, yuv_format, 
				out, "std", iteration_number, yuv420_rgb24_std);
#ifdef _YUVRGB_SSE2_
			test_yuv2rgb(width, height, Y, U, V, width, (width+1)/2, RGB, width*3, yuv_format, 
				out, "sse2_unaligned", iteration_number, yuv420_rgb24_sseu);
#endif
#if USE_FFMPEG
			test_yuv2rgb(width, height, Y, U, V, width, (width+1)/2, RGB, width*3, yuv_format, 
				out, "ffmpeg_unaligned", iteration_number, yuv420_rgb24_ffmpeg);
#endif
#if USE_IPP
			test_yuv2rgb(width, height, Y, U, V, width, (width+1)/2, RGB, width*3, yuv_format, 
				out, "ipp_unaligned", iteration_number, yuv420_rgb24_ipp);
#endif
#ifdef _YUVRGB_SSE2_
			test_yuv2rgb(width, height, Ya, Ua, Va, y_stride, uv_stride, RGBa, rgb_stride, yuv_format, 
				out, "sse2_aligned", iteration_number, yuv420_rgb24_sse);
#endif
#if USE_FFMPEG
			test_yuv2rgb(width, height, Ya, Ua, Va, y_stride, uv_stride, RGBa, rgb_stride, yuv_format, 
				out, "ffmpeg_aligned", iteration_number, yuv420_rgb24_ffmpeg);
#endif
#if USE_IPP
			test_yuv2rgb(width, height, Ya, Ua, Va, y_stride, uv_stride, RGBa, rgb_stride, yuv_format, 
				out, "ipp_aligned", iteration_number, yuv420_rgb24_ipp);
#endif
		}
		else if(mode==YUV2RGB_NV12)
		{
			test_yuvsp2rgb(width, height, Y, U, width, width, RGB, width*3, yuv_format, 
				out, "std", iteration_number, nv12_rgb24_std);
#ifdef _YUVRGB_SSE2_
			test_yuvsp2rgb(width, height, Y, U, width, width, RGB, width*3, yuv_format, 
				out, "sse2_unaligned", iteration_number, nv12_rgb24_sseu);
			test_yuvsp2rgb(width, height, Ya, Ua, y_stride, uv_stride, RGBa, rgb_stride, yuv_format, 
				out, "sse2_aligned", iteration_number, nv12_rgb24_sse);
#endif
		}
		else if(mode==YUV2RGB_NV21)
		{
			test_yuvsp2rgb(width, height, Y, U, width, width, RGB, width*3, yuv_format, 
				out, "std", iteration_number, nv21_rgb24_std);
#ifdef _YUVRGB_SSE2_
			test_yuvsp2rgb(width, height, Y, U, width, width, RGB, width*3, yuv_format, 
				out, "sse2_unaligned", iteration_number, nv21_rgb24_sseu);
			test_yuvsp2rgb(width, height, Ya, Ua, y_stride, uv_stride, RGBa, rgb_stride, yuv_format, 
				out, "sse2_aligned", iteration_number, nv21_rgb24_sse);
#endif
		}
	}
	else if(mode==RGB2YUV)
	{
		//parse argument line
		out = argv[3];
		
		// read input data and allocate output data
		if(readPPM(filename, &width, &height, &RGB)!=0)
		{
			printf("Error reading image file, check that the file exists and has the correct format.\n");
			return 1;
		}
		
#if USE_FFMPEG
		rgb2yuv_swscale_ctx = sws_getContext(width, height, AV_PIX_FMT_RGB24, width, height, AV_PIX_FMT_YUV420P, 0, 0, 0, 0);
#endif
		
		YUV = malloc(width*height*3/2);
		
		Y = YUV;
		U = YUV+width*height;
		V = YUV+width*height+((width+1)/2)*((height+1)/2);
		
		// allocate aligned data
		const size_t y_stride = width + (16-width%16)%16,
		uv_stride = (width+1)/2 + (16-((width+1)/2)%16)%16,
		rgb_stride = width*3 +(16-(3*width)%16)%16;
		
		RGBa = _mm_malloc(rgb_stride*height, 16);
		for(unsigned int i=0; i<height; ++i)
		{
			memcpy(RGBa+i*rgb_stride, RGB+i*width*3, width*3);
		}
		
		const size_t y_size = y_stride*height, uv_size = uv_stride*((height+1)/2);
		YUVa = _mm_malloc(y_size+2*uv_size, 16);
		Ya = YUVa;
		Ua = YUVa+y_size;
		Va = YUVa+y_size+uv_size;

		
		// test all versions
		test_rgb2yuv(width, height, RGB, width*3, Y, U, V, width, (width+1)/2, yuv_format, 
			out, "std", iteration_number, rgb24_yuv420_std);
#ifdef _YUVRGB_SSE2_
		test_rgb2yuv(width, height, RGB, width*3, Y, U, V, width, (width+1)/2, yuv_format, 
			out, "sse2_unaligned", iteration_number, rgb24_yuv420_sseu);
#endif
#if USE_FFMPEG
		test_rgb2yuv(width, height, RGB, width*3, Y, U, V, width, (width+1)/2, yuv_format, 
			out, "ffmpeg_unaligned", iteration_number, rgb24_yuv420_ffmpeg);
#endif
#if USE_IPP
		test_rgb2yuv(width, height, RGB, width*3, Y, U, V, width, (width+1)/2, yuv_format, 
			out, "ipp_unaligned", iteration_number, rgb24_yuv420_ipp);
#endif
#ifdef _YUVRGB_SSE2_
		test_rgb2yuv(width, height, RGBa, rgb_stride, Ya, Ua, Va, y_stride, uv_stride, yuv_format, 
			out, "sse2_aligned", iteration_number, rgb24_yuv420_sse);
#endif
#if USE_FFMPEG
		test_rgb2yuv(width, height, RGBa, rgb_stride, Ya, Ua, Va, y_stride, uv_stride, yuv_format, 
			out, "ffmpeg_aligned", iteration_number, rgb24_yuv420_ffmpeg);
#endif
#if USE_IPP
		test_rgb2yuv(width, height, RGBa, rgb_stride, Ya, Ua, Va, y_stride, uv_stride, yuv_format, 
			out, "ipp_aligned", iteration_number, rgb24_yuv420_ipp);
#endif
	}
	else if(mode==RGBA2YUV)
	{
		//parse argument line
		out = argv[3];
		
		// read input data and allocate output data
		if(readPPM(filename, &width, &height, &RGB)!=0)
		{
			printf("Error reading image file, check that the file exists and has the correct format.\n");
			return 1;
		}
		// convert rgb to rgba
		uint8_t *RGBA = NULL;
		convert_rgb_to_rgba(RGB, width, height, &RGBA);
		
		YUV = malloc(width*height*3/2);
		
		Y = YUV;
		U = YUV+width*height;
		V = YUV+width*height+((width+1)/2)*((height+1)/2);
		
		// allocate aligned data
		const size_t y_stride = width + (16-width%16)%16,
		uv_stride = (width+1)/2 + (16-((width+1)/2)%16)%16,
		rgba_stride = width*4 +(16-(4*width)%16)%16;
		
		RGBa = _mm_malloc(rgba_stride*height, 16);
		for(unsigned int i=0; i<height; ++i)
		{
			memcpy(RGBa+i*rgba_stride, RGBA+i*width*4, width*4);
		}
		
		const size_t y_size = y_stride*height, uv_size = uv_stride*((height+1)/2);
		YUVa = _mm_malloc(y_size+2*uv_size, 16);
		Ya = YUVa;
		Ua = YUVa+y_size;
		Va = YUVa+y_size+uv_size;
		
		// test all versions
		test_rgb2yuv(width, height, RGBA, width*4, Y, U, V, width, (width+1)/2, yuv_format, 
			out, "std", iteration_number, rgb32_yuv420_std);
#ifdef _YUVRGB_SSE2_
		test_rgb2yuv(width, height, RGBA, width*4, Y, U, V, width, (width+1)/2, yuv_format, 
			out, "sse2_unaligned", iteration_number, rgb32_yuv420_sseu);
		test_rgb2yuv(width, height, RGBa, rgba_stride, Ya, Ua, Va, y_stride, uv_stride, yuv_format, 
			out, "sse2_aligned", iteration_number, rgb32_yuv420_sse);
#endif
		
		free(RGBA);
	}
	
	_mm_free(RGBa);
	_mm_free(YUVa);
	free(RGB);
	free(YUV);
	
	return 0;
}

