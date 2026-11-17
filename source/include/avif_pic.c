// Otakio: native AVIF support for piclib (libavif + dav1d). Some sites serve pages and covers only as AVIF, which used
// to go through an image-converting proxy. The decoded YUV planes are converted straight into the texture, sampling
// only the pixels the texture keeps (a region and/or 1/2, 1/4, 1/8 scale), so no full-size RGBA copy is ever made.
#include <vitasdk.h>
#include <vita2d.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <avif/avif.h>

#define AVIF_MAX_TEXTURE_SIZE 4096

int is_AVIF_file(const char *filename)
{
	unsigned char head[32];
	SceUID fd = sceIoOpen(filename, SCE_O_RDONLY, 0777);
	if (fd < 0)
		return 0;
	int n = sceIoRead(fd, head, sizeof(head));
	sceIoClose(fd);
	if (n < 16 || memcmp(head + 4, "ftyp", 4) != 0)
		return 0;
	// the major brand, or a compatible brand listed in the same box
	for (int i = 8; i + 4 <= n; i += 4)
		if (memcmp(head + i, "avif", 4) == 0 || memcmp(head + i, "avis", 4) == 0)
			return 1;
	return 0;
}

static unsigned char *avif_read_whole(const char *filename, size_t *size)
{
	SceUID fd = sceIoOpen(filename, SCE_O_RDONLY, 0777);
	if (fd < 0)
		return NULL;
	SceOff len = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	if (len <= 0 || len > 48 * 1024 * 1024)
	{
		sceIoClose(fd);
		return NULL;
	}
	unsigned char *buf = (unsigned char *)malloc((size_t)len);
	if (!buf)
	{
		sceIoClose(fd);
		return NULL;
	}
	size_t got = 0;
	while (got < (size_t)len)
	{
		int r = sceIoRead(fd, buf + got, (SceSize)((size_t)len - got));
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	sceIoClose(fd);
	if (got != (size_t)len)
	{
		free(buf);
		return NULL;
	}
	*size = got;
	return buf;
}

static avifDecoder *avif_open(const unsigned char *data, size_t size)
{
	avifDecoder *decoder = avifDecoderCreate();
	if (!decoder)
		return NULL;
	decoder->maxThreads = 2;
	decoder->strictFlags = AVIF_STRICT_DISABLED;
	decoder->ignoreExif = AVIF_TRUE;
	decoder->ignoreXMP = AVIF_TRUE;
	if (avifDecoderSetIOMemory(decoder, data, size) != AVIF_RESULT_OK || avifDecoderParse(decoder) != AVIF_RESULT_OK)
	{
		avifDecoderDestroy(decoder);
		return NULL;
	}
	return decoder;
}

void get_AVIF_resolution(const char *filename, int *dest_width, int *dest_height)
{
	size_t size = 0;
	unsigned char *data = avif_read_whole(filename, &size);
	if (!data)
		return;
	avifDecoder *decoder = avif_open(data, size);
	if (decoder)
	{
		*dest_width = (int)decoder->image->width;
		*dest_height = (int)decoder->image->height;
		avifDecoderDestroy(decoder);
	}
	free(data);
}

static int avif_denom(int width, int height, int level)
{
	float sw = (float)width / AVIF_MAX_TEXTURE_SIZE, sh = (float)height / AVIF_MAX_TEXTURE_SIZE;
	float scale = sw > sh ? sw : sh;
	if (scale < level)
		scale = (float)level;
	if (scale <= 1.f)
		return 1;
	if (scale <= 2.f)
		return 2;
	if (scale <= 4.f)
		return 4;
	if (scale <= 8.f)
		return 8;
	return 0;
}

static inline uint8_t clamp8(int v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

// region x,y,width,height of the image (width 0: all of it), kept at 1/denom
static vita2d_texture *avif_decode(const unsigned char *data, size_t size, int x, int y, int width, int height, int level)
{
	avifDecoder *decoder = avif_open(data, size);
	if (!decoder)
		return NULL;
	vita2d_texture *texture = NULL;
	avifImage *img = decoder->image;
	if (width == 0)
	{
		x = 0;
		y = 0;
		width = (int)img->width;
		height = (int)img->height;
	}
	int denom = avif_denom(width, height, level);
	if (denom && x >= 0 && y >= 0 && x + width <= (int)img->width && y + height <= (int)img->height &&
		avifDecoderNextImage(decoder) == AVIF_RESULT_OK && img->depth == 8)
	{
		int tw = width / denom, th = height / denom;
		if (tw > 0 && th > 0)
			texture = vita2d_create_empty_texture(tw, th);
		if (texture)
		{
			// Y'CbCr -> RGB, 16.16 fixed point. Unspecified matrices are read as BT.601, like libavif does.
			float kr = 0.299f, kb = 0.114f;
			if (img->matrixCoefficients == AVIF_MATRIX_COEFFICIENTS_BT709)
			{
				kr = 0.2126f;
				kb = 0.0722f;
			}
			else if (img->matrixCoefficients == AVIF_MATRIX_COEFFICIENTS_BT2020_NCL || img->matrixCoefficients == AVIF_MATRIX_COEFFICIENTS_BT2020_CL)
			{
				kr = 0.2627f;
				kb = 0.0593f;
			}
			float kg = 1.f - kr - kb;
			bool full = img->yuvRange == AVIF_RANGE_FULL;
			float ys = full ? 1.f : 255.f / 219.f, cs = full ? 1.f : 255.f / 224.f;
			int yoff = full ? 0 : 16;
			int fy = (int)(ys * 65536), fcr_r = (int)(2.f * (1.f - kr) * cs * 65536), fcb_b = (int)(2.f * (1.f - kb) * cs * 65536);
			int fcb_g = (int)(2.f * kb * (1.f - kb) / kg * cs * 65536), fcr_g = (int)(2.f * kr * (1.f - kr) / kg * cs * 65536);
			int sx = 0, sy = 0;
			bool mono = img->yuvFormat == AVIF_PIXEL_FORMAT_YUV400;
			if (img->yuvFormat == AVIF_PIXEL_FORMAT_YUV420)
				sx = sy = 1;
			else if (img->yuvFormat == AVIF_PIXEL_FORMAT_YUV422)
				sx = 1;
			bool identity = img->matrixCoefficients == AVIF_MATRIX_COEFFICIENTS_IDENTITY;
			uint8_t *Y = img->yuvPlanes[AVIF_CHAN_Y], *U = img->yuvPlanes[AVIF_CHAN_U], *V = img->yuvPlanes[AVIF_CHAN_V];
			uint32_t yr = img->yuvRowBytes[AVIF_CHAN_Y], ur = img->yuvRowBytes[AVIF_CHAN_U], vr = img->yuvRowBytes[AVIF_CHAN_V];
			uint8_t *A = img->alphaPlane;
			uint32_t ar = img->alphaRowBytes;
			uint8_t *dst = (uint8_t *)vita2d_texture_get_datap(texture);
			unsigned int stride = vita2d_texture_get_stride(texture);
			for (int j = 0; j < th; j++)
			{
				int py = y + j * denom;
				uint8_t *out = dst + (size_t)j * stride;
				uint8_t *yrow = Y + (size_t)py * yr;
				uint8_t *urow = mono ? NULL : U + (size_t)(py >> sy) * ur;
				uint8_t *vrow = mono ? NULL : V + (size_t)(py >> sy) * vr;
				uint8_t *arow = A ? A + (size_t)py * ar : NULL;
				for (int i = 0; i < tw; i++, out += 4)
				{
					int px = x + i * denom;
					int r, g, b;
					if (mono)
					{
						r = g = b = clamp8(((yrow[px] - yoff) * fy) >> 16);
					}
					else if (identity)
					{
						g = yrow[px];
						b = urow[px >> sx];
						r = vrow[px >> sx];
					}
					else
					{
						int yy = (yrow[px] - yoff) * fy;
						int cb = urow[px >> sx] - 128, cr = vrow[px >> sx] - 128;
						r = clamp8((yy + cr * fcr_r) >> 16);
						g = clamp8((yy - cb * fcb_g - cr * fcr_g) >> 16);
						b = clamp8((yy + cb * fcb_b) >> 16);
					}
					out[0] = (uint8_t)r;
					out[1] = (uint8_t)g;
					out[2] = (uint8_t)b;
					out[3] = arow ? arow[px] : 255; // libavif keeps alpha in full range
				}
			}
		}
	}
	avifDecoderDestroy(decoder);
	return texture;
}

vita2d_texture *load_AVIF_file_downscaled(const char *filename, int level)
{
	size_t size = 0;
	unsigned char *data = avif_read_whole(filename, &size);
	if (!data)
		return NULL;
	vita2d_texture *texture = avif_decode(data, size, 0, 0, 0, 0, level);
	free(data);
	return texture;
}

vita2d_texture *load_AVIF_file_part(const char *filename, int x, int y, int width, int height)
{
	if (x < 0 || y < 0 || width <= 0 || height <= 0)
		return NULL;
	size_t size = 0;
	unsigned char *data = avif_read_whole(filename, &size);
	if (!data)
		return NULL;
	vita2d_texture *texture = avif_decode(data, size, x, y, width, height, 1);
	free(data);
	return texture;
}
