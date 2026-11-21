/*
  Unity Capture
  Copyright (c) 2018 Bernhard Schelling

  Based on UnityCam
  https://github.com/mrayy/UnityCam
  Copyright (c) 2016 MHD Yamen Saraiji

  The MIT License (MIT)

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include "shared.inl"
#include "streams.h"
#include "standby_logo.h"
#include <cguid.h>
#include <gdiplus.h>
#include <strsafe.h>
#include <math.h>
#include <vector>

#define CaptureSourceName L"OBScure Virtual Camera"

//Use separate GUIDs for 64bit and 32bit so both can be installed at the same time
#ifdef _WIN64
DEFINE_GUID(CLSID_UnityCaptureService,    0x5c2cd55c, 0x92ad, 0x4999, 0x86, 0x66, 0x91, 0x2b, 0xd3, 0xe7, 0x00, 0x10);
DEFINE_GUID(CLSID_UnityCaptureProperties, 0x5c2cd55c, 0x92ad, 0x4999, 0x86, 0x66, 0x91, 0x2b, 0xd3, 0xe7, 0x00, 0x11);
#else
DEFINE_GUID(CLSID_UnityCaptureService,    0x5c2cd55c, 0x92ad, 0x4999, 0x86, 0x66, 0x91, 0x2b, 0xd3, 0xe7, 0x00, 0x20);
DEFINE_GUID(CLSID_UnityCaptureProperties, 0x5c2cd55c, 0x92ad, 0x4999, 0x86, 0x66, 0x91, 0x2b, 0xd3, 0xe7, 0x00, 0x21);
#endif

//List of resolutions offered by this filter
//If you add a higher resolution, make sure to update MAX_SHARED_IMAGE_SIZE
static struct { int width, height; } _media[] =
{
	{ 1920, 1080 }, //16:9
	{ 1280,  720 }, //16:9
	{  960,  540 }, //16:9
	{  640,  360 }, //16:9
	{  480,  270 }, //16:9
	{  256,  144 }, //16:9
	{ 2560, 1440 }, //16:9
	{ 3840, 2160 }, //16:9
	{ 1440, 1080 }, //4:3
	{  960,  720 }, //4:3
	{  640,  480 }, //4:3
	{  480,  360 }, //4:3
	{  320,  240 }, //4:3
	{  192,  144 }, //4:3
	{ 1920, 1440 }, //4:3
	{ 2880, 2160 }, //4:3
	{ 1920, 1200 }, //16:10
	{ 1280,  800 }, //16:10
	{ 2880, 1800 }, //16:10
	{ 2560, 1600 }, //16:10
	{ 1680, 1050 }, //16:10
	{ 1440,  900 }, //16:10
	{    0,    0 }, //This slot is used for custom resolutions if requested by the target application
};

//Error draw modes (what to display on screen in case of errors/warnings)
enum EErrorDrawCase { EDC_ResolutionMismatch, EDC_UnityNeverStarted, EDC_UnitySendingStopped, _EDC_MAX };
enum EErrorDrawMode { EDM_GREENKEY, EDM_BLUEPINK, EDM_GREENYELLOW, EDM_BLACK };
static EErrorDrawMode ErrorDrawModes[_EDC_MAX] = { EDM_BLUEPINK, EDM_GREENYELLOW, EDM_GREENKEY };
static wchar_t* ErrorDrawModeNames[] = { L"Green Key (RGB #00FE00)", L"Blue/Pink Pattern", L"Green/Yellow Pattern", L"Fill Black" };
static bool OutputFrameRate = false;

#ifdef _DEBUG
void DebugLog(const char *format, ...)
{
	char stackbuf[1024];
	stackbuf[0] = '\0';
	va_list ap; va_start(ap, format); vsnprintf_s(stackbuf, 1024, 1024, format, ap); va_end(ap);
	stackbuf[1023] = '\0';
	OutputDebugStringA(stackbuf);
}
#else
#define DebugLog(...) ((void)0)
#endif

//Interface definition for ICamSource used by CCaptureSource
DEFINE_GUID(IID_ICamSource, 0xdd20e647, 0xf3e5, 0x4156, 0xb3, 0x7b, 0x54, 0x6f, 0xcf, 0x88, 0xec, 0x50);
DECLARE_INTERFACE_(ICamSource, IUnknown) { };

class CCaptureStream : CSourceStream, IKsPropertySet, IAMStreamConfig, IAMStreamControl, IAMPushSource
{
public:
	CCaptureStream(CSource* pOwner, HRESULT* phr, int CapNum) : CSourceStream("Stream", phr, pOwner, L"Output")
	{
		m_llFrame = m_llFrameMissCount = 0;
		m_prevStartTime = 0;
		m_avgTimePerFrame = 10000000 / 30;
		m_pReceiver = new SharedImageMemory(CapNum);
		m_iUnscaledBufSize = 0;
		m_pUnscaledBuf = NULL;
		m_RGBA16Table = NULL;
		GetMediaType(0, &m_mt);
	}

	virtual ~CCaptureStream()
	{
		delete m_pReceiver;
		if (m_pUnscaledBuf) free(m_pUnscaledBuf);
		if (m_RGBA16Table) free(m_RGBA16Table);
		if (m_pConvertBuf) free(m_pConvertBuf);
	}

private:
	enum EOutputFormat { FMT_RGB24, FMT_ARGB32, FMT_NV12, FMT_YUY2 };
	EOutputFormat m_OutputFormat = FMT_NV12;
	uint8_t* m_pConvertBuf = nullptr;
	DWORD m_iConvertBufSize = 0;

	static inline uint8_t Clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

	// Convert packed top-down BGR24 into top-down NV12 (BT.709 limited range).
	static void BGRToNV12(const uint8_t* src, int w, int h, uint8_t* dst)
	{
		if ((w & 1) || (h & 1) || w < 2 || h < 2) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); return; }
		uint8_t* yPlane = dst;
		uint8_t* uvPlane = dst + (size_t)w * h;
		for (int y = 0; y < h; y += 2)
		{
			const uint8_t* row0 = src + (size_t)y * w * 3;
			const uint8_t* row1 = src + (size_t)(y + 1) * w * 3;
			uint8_t* y0 = yPlane + (size_t)y * w;
			uint8_t* y1 = y0 + w;
			uint8_t* uv = uvPlane + (size_t)(y / 2) * w;
			for (int x = 0; x < w; x += 2)
			{
				const uint8_t* p00 = row0 + (size_t)x * 3;
				const uint8_t* p01 = p00 + 3;
				const uint8_t* p10 = row1 + (size_t)x * 3;
				const uint8_t* p11 = p10 + 3;
				const int b00 = p00[0], g00 = p00[1], r00 = p00[2];
				const int b01 = p01[0], g01 = p01[1], r01 = p01[2];
				const int b10 = p10[0], g10 = p10[1], r10 = p10[2];
				const int b11 = p11[0], g11 = p11[1], r11 = p11[2];
				y0[x]     = Clamp8(((47 * r00 + 157 * g00 + 16 * b00 + 128) >> 8) + 16);
				y0[x + 1] = Clamp8(((47 * r01 + 157 * g01 + 16 * b01 + 128) >> 8) + 16);
				y1[x]     = Clamp8(((47 * r10 + 157 * g10 + 16 * b10 + 128) >> 8) + 16);
				y1[x + 1] = Clamp8(((47 * r11 + 157 * g11 + 16 * b11 + 128) >> 8) + 16);
				const int rsum = r00 + r01 + r10 + r11;
				const int gsum = g00 + g01 + g10 + g11;
				const int bsum = b00 + b01 + b10 + b11;
				uv[x]     = Clamp8(((-26 * rsum - 87 * gsum + 112 * bsum) >> 10) + 128);
				uv[x + 1] = Clamp8(((112 * rsum - 102 * gsum - 10 * bsum) >> 10) + 128);
			}
		}
	}

	// Convert packed top-down BGR24 into top-down YUY2 (BT.709 limited range).
	static void BGRToYUY2(const uint8_t* src, int w, int h, uint8_t* dst)
	{
		if ((w & 1) || w < 2) { memset(dst, 16, (size_t)w * h * 2); return; }
		for (int y = 0; y < h; y++)
		{
			const uint8_t* row = src + (size_t)y * w * 3;
			uint8_t* out = dst + (size_t)y * w * 2;
			for (int x = 0; x < w; x += 2)
			{
				const int b0 = row[(size_t)x * 3 + 0], g0 = row[(size_t)x * 3 + 1], r0 = row[(size_t)x * 3 + 2];
				const int b1 = row[(size_t)x * 3 + 3], g1 = row[(size_t)x * 3 + 4], r1 = row[(size_t)x * 3 + 5];
				out[(size_t)x * 2 + 0] = Clamp8(((47 * r0 + 157 * g0 + 16 * b0 + 128) >> 8) + 16);
				out[(size_t)x * 2 + 1] = Clamp8(((-26 * (r0 + r1) - 87 * (g0 + g1) + 112 * (b0 + b1)) >> 9) + 128);
				out[(size_t)x * 2 + 2] = Clamp8(((47 * r1 + 157 * g1 + 16 * b1 + 128) >> 8) + 16);
				out[(size_t)x * 2 + 3] = Clamp8(((112 * (r0 + r1) - 102 * (g0 + g1) - 10 * (b0 + b1)) >> 9) + 128);
			}
		}
	}

	HRESULT FillBuffer(IMediaSample *pSamp) override
	{
		HRESULT hr;
		BYTE* pBuf;
		VIDEOINFO *pvi = (VIDEOINFO*)m_mt.Format();
		REFERENCE_TIME startTime = m_prevStartTime, endTime = startTime + m_avgTimePerFrame;
		LONGLONG mtStart = m_llFrame, mtEnd = mtStart + 1;
		m_prevStartTime = endTime;
		m_llFrame = mtEnd;
		const bool convert = (m_OutputFormat == FMT_NV12 || m_OutputFormat == FMT_YUY2);
		const int outW = pvi->bmiHeader.biWidth;
		const int outH = pvi->bmiHeader.biHeight;
		if (!convert)
		{
			UCASSERT(pSamp->GetSize() == pvi->bmiHeader.biSizeImage);
			UCASSERT(DIBSIZE(pvi->bmiHeader) == pvi->bmiHeader.biSizeImage);
		}

		if (FAILED(hr = pSamp->GetPointer(&pBuf))) return hr;
		if (FAILED(hr = pSamp->SetActualDataLength(pvi->bmiHeader.biSizeImage))) return hr;
		if (FAILED(hr = pSamp->SetTime(&startTime, &endTime))) return hr;
		if (FAILED(hr = pSamp->SetMediaTime(&mtStart, &mtEnd))) return hr;

		BYTE* workBuf = pBuf;
		int workBPP = pvi->bmiHeader.biBitCount / 8;
		if (convert)
		{
			const DWORD need = (DWORD)outW * (DWORD)outH * 3;
			if (m_iConvertBufSize != need)
			{
				free(m_pConvertBuf);
				m_pConvertBuf = (uint8_t*)malloc(need);
				m_iConvertBufSize = (m_pConvertBuf ? need : 0);
			}
			if (!m_pConvertBuf) { memset(pBuf, 0, pvi->bmiHeader.biSizeImage); return S_OK; }
			workBuf = m_pConvertBuf;
			workBPP = 3;
		}
		ProcessState State = { workBuf, outW, outH, workBPP, this };
		switch (m_pReceiver->Receive((SharedImageMemory::ReceiveCallbackFunc)ProcessImage, &State))
		{
			case SharedImageMemory::RECEIVERES_CAPTUREINACTIVE:{
				RenderStandbyCard(&State, false);
				Sleep((DWORD)(m_avgTimePerFrame / 10000 - 1)); //just wait a bit until capturing next frame
				break;}

			case SharedImageMemory::RECEIVERES_NEWFRAME:
				if (m_llFrameMissCount) m_llFrameMissCount = 0;
				break;

			case SharedImageMemory::RECEIVERES_OLDFRAME:{
				if (++m_llFrameMissCount < m_llFrameMissMax) break;
				RenderStandbyCard(&State, true);
				break;}
		}
		if (OutputFrameRate) RenderFPSDisplay(&State);
		if (convert)
		{
			if (m_OutputFormat == FMT_NV12) BGRToNV12(m_pConvertBuf, outW, outH, pBuf);
			else BGRToYUY2(m_pConvertBuf, outW, outH, pBuf);
		}
		return S_OK;
	}

	struct ProcessJob
	{
		enum EType { JOB_NONE, JOB_RGBA8toBGR8, JOB_RGBA8toBGRA8, JOB_RGBA16toBGR8, JOB_RGBA16toBGRA8, JOB_BGR_RESIZE_LINEAR, JOB_BGRA_RESIZE_LINEAR, JOB_BGR_MIRROR_HORIZONTAL, JOB_BGRA_MIRROR_HORIZONTAL } Type;
		const void *BufIn; void *BufOut;
		size_t Width, RowStart, RowEnd, RGBAInStride, ResizeToHeight, ResizeFromWidth, ResizeFromHeight;
		const uint8_t* RGBA16Table;

		inline void Execute()
		{
			UCASSERT(RowEnd >= RowStart);
			if (RowStart == RowEnd) return;
			if      (Type == JOB_RGBA8toBGR8)            RGBA8toBGR8();
			else if (Type == JOB_RGBA8toBGRA8)           RGBA8toBGRA8();
			else if (Type == JOB_RGBA16toBGR8)           RGBA16toBGR8();
			else if (Type == JOB_RGBA16toBGRA8)          RGBA16toBGRA8();
			else if (Type == JOB_BGR_RESIZE_LINEAR)      BGRResizeLinear();
			else if (Type == JOB_BGRA_RESIZE_LINEAR)     BGRAResizeLinear();
			else if (Type == JOB_BGR_MIRROR_HORIZONTAL)  BGRMirrorHorizontal();
			else if (Type == JOB_BGRA_MIRROR_HORIZONTAL) BGRAMirrorHorizontal();
		}

		void RGBA8toBGR8()
		{
			const uint32_t *src = (const uint32_t*)BufIn + (RowStart * RGBAInStride);
			uint8_t *dst = (uint8_t*)BufOut + (RowStart * Width * 3), *dstEnd = (uint8_t*)BufOut + (RowEnd * Width * 3);
			if (RGBAInStride != Width)
			{
				//Handle a case where the texture pitch does have a gap on the right side
				const uint32_t *srcLastRow = (const uint32_t*)BufIn + ((RowEnd - 1) * RGBAInStride);
				for (size_t srcStride = RGBAInStride, iMax = Width; src != srcLastRow; src += srcStride)
					for (size_t i = 0; i != iMax; i++, dst += 3)
						*(uint32_t*)dst = _byteswap_ulong(src[i]) >> 8;
				for (size_t i = 0, iMax = Width - 1; i != iMax; i++, dst += 3, src++)
					*(uint32_t*)dst = _byteswap_ulong(*src) >> 8;
			}
			else
			{
				//The fastest (implemented) path to convert from RGBA to BGR
				const uint32_t *srcEnd8 = src + (((RowEnd-RowStart)*Width-1)&~7), *srcEnd1 = src + ((RowEnd-RowStart)*Width-1);
				for (; src != srcEnd8; dst += 24, src += 8)
				{
					*(uint32_t*)(dst     ) = _byteswap_ulong(src[0]) >> 8;
					*(uint32_t*)(dst +  3) = _byteswap_ulong(src[1]) >> 8;
					*(uint32_t*)(dst +  6) = _byteswap_ulong(src[2]) >> 8;
					*(uint32_t*)(dst +  9) = _byteswap_ulong(src[3]) >> 8;
					*(uint32_t*)(dst + 12) = _byteswap_ulong(src[4]) >> 8;
					*(uint32_t*)(dst + 15) = _byteswap_ulong(src[5]) >> 8;
					*(uint32_t*)(dst + 18) = _byteswap_ulong(src[6]) >> 8;
					*(uint32_t*)(dst + 21) = _byteswap_ulong(src[7]) >> 8;
				}
				for (; src != srcEnd1; dst += 3, src++)
					*(uint32_t*)(dst) = _byteswap_ulong(*src) >> 8;
			}
			uint32_t FinalPixel = _byteswap_ulong(*src) >> 8;
			memcpy(dst, &FinalPixel, 3);
		}

		void RGBA8toBGRA8()
		{
			#define RGBATOBGRA(x) ((x&0xFF00FF00)|((x&0x00FF0000)>>16)|((x&0x000000FF)<<16))
			const uint32_t *src = (const uint32_t*)BufIn + (RowStart * RGBAInStride);
			uint32_t *dst = (uint32_t*)BufOut + (RowStart * Width), *dstEnd = (uint32_t*)BufOut + (RowEnd * Width);
			if (RGBAInStride != Width)
			{
				//Handle a case where the texture pitch does have a gap on the right side
				const uint32_t *srcEnd = (const uint32_t*)BufIn + ((RowEnd) * RGBAInStride);
				for (size_t srcStride = RGBAInStride, iMax = Width; src != srcEnd; src += srcStride)
					for (size_t i = 0; i != iMax; i++, dst++)
						*dst = RGBATOBGRA(src[i]);
			}
			else
			{
				//The fastest (implemented) path to convert from RGBA to BGR
				const uint32_t *srcEnd8 = src + (((RowEnd-RowStart)*Width)&~7), *srcEnd1 = src + ((RowEnd-RowStart)*Width);
				for (; src != srcEnd8; dst += 8, src += 8)
				{
					dst[0] = RGBATOBGRA(src[0]);
					dst[1] = RGBATOBGRA(src[1]);
					dst[2] = RGBATOBGRA(src[2]);
					dst[3] = RGBATOBGRA(src[3]);
					dst[4] = RGBATOBGRA(src[4]);
					dst[5] = RGBATOBGRA(src[5]);
					dst[6] = RGBATOBGRA(src[6]);
					dst[7] = RGBATOBGRA(src[7]);
				}
				for (; src != srcEnd1; dst++, src++)
					*dst = RGBATOBGRA(*src);
			}
			#undef RGBATOBGRA
		}

		void RGBA16toBGR8()
		{
			//16 bit color downscaling (HDR (16 bit floats) to BGR)
			const uint8_t* ttbl = RGBA16Table;
			#define RGBAF16toBGRU8(psrc) ((ttbl[((uint16_t*)(psrc))[0]]<<16) | (ttbl[((uint16_t*)(psrc))[1]]<<8) | ttbl[((uint16_t*)(psrc))[2]])
			const uint64_t *src = (const uint64_t*)BufIn + (RowStart * RGBAInStride);
			uint8_t *dst = (uint8_t*)BufOut + (RowStart * Width * 3), *dstEnd = (uint8_t*)BufOut + (RowEnd * Width * 3);
			if (RGBAInStride != Width)
			{
				//Handle a case where the texture pitch does have a gap on the right side
				const uint64_t *srcLastRow = (const uint64_t*)BufIn + ((RowEnd - 1) * RGBAInStride);
				for (size_t srcStride = RGBAInStride, iMax = Width; src != srcLastRow; src += srcStride)
					for (size_t i = 0; i != iMax; i++, dst += 3)
						*(uint32_t*)dst = RGBAF16toBGRU8(src + i);
				for (size_t i = 0, iMax = Width - 1; i != iMax; i++, dst += 3, src++)
					*(uint32_t*)dst = RGBAF16toBGRU8(src);
			}
			else
			{
				//The fastest (implemented) path to convert from RGBA to BGR
				const uint64_t *srcEnd8 = src + (((RowEnd-RowStart)*Width-1)&~7), *srcEnd1 = src + ((RowEnd-RowStart)*Width-1);
				for (; src != srcEnd8; dst += 24, src += 8)
				{
					*(uint32_t*)(dst     ) = RGBAF16toBGRU8(src    );
					*(uint32_t*)(dst +  3) = RGBAF16toBGRU8(src + 1);
					*(uint32_t*)(dst +  6) = RGBAF16toBGRU8(src + 2);
					*(uint32_t*)(dst +  9) = RGBAF16toBGRU8(src + 3);
					*(uint32_t*)(dst + 12) = RGBAF16toBGRU8(src + 4);
					*(uint32_t*)(dst + 15) = RGBAF16toBGRU8(src + 5);
					*(uint32_t*)(dst + 18) = RGBAF16toBGRU8(src + 6);
					*(uint32_t*)(dst + 21) = RGBAF16toBGRU8(src + 7);
				}
				for (; src != srcEnd1; dst += 3, src++)
					*(uint32_t*)(dst) = RGBAF16toBGRU8(src);
			}
			//For the final pixel we can't use 4 byte uint32_t copy so we call memcpy
			uint32_t FinalPixel = RGBAF16toBGRU8(src);
			memcpy(dst, &FinalPixel, 3);
			#undef RGBAF16toBGRU8
		}

		void RGBA16toBGRA8()
		{
			//16 bit color downscaling (HDR (16 bit floats) to BGRA)
			const uint8_t* ttbl = RGBA16Table;
			#define RGBAF16toBGRAU8(psrc) ((ttbl[((uint16_t*)(psrc))[3]]<<24) | (ttbl[((uint16_t*)(psrc))[0]]<<16) | (ttbl[((uint16_t*)(psrc))[1]]<<8) | ttbl[((uint16_t*)(psrc))[2]])
			const uint64_t *src = (const uint64_t*)BufIn + (RowStart * RGBAInStride);
			uint32_t *dst = (uint32_t*)BufOut + (RowStart * Width), *dstEnd = (uint32_t*)BufOut + (RowEnd * Width);
			if (RGBAInStride != Width)
			{
				//Handle a case where the texture pitch does have a gap on the right side
				const uint64_t *srcEnd = (const uint64_t*)BufIn + (RowEnd * RGBAInStride);
				for (size_t srcStride = RGBAInStride, iMax = Width; src != srcEnd; src += srcStride)
					for (size_t i = 0; i != iMax; i++, dst++)
						*dst = RGBAF16toBGRAU8(src + i);
			}
			else
			{
				//The fastest (implemented) path to convert from RGBA to BGR
				const uint64_t *srcEnd8 = src + (((RowEnd-RowStart)*Width-1)&~7), *srcEnd1 = src + ((RowEnd-RowStart)*Width-1);
				for (; src != srcEnd8; dst += 8, src += 8)
				{
					dst[0] = RGBAF16toBGRAU8(src    );
					dst[1] = RGBAF16toBGRAU8(src + 1);
					dst[2] = RGBAF16toBGRAU8(src + 2);
					dst[3] = RGBAF16toBGRAU8(src + 3);
					dst[4] = RGBAF16toBGRAU8(src + 4);
					dst[5] = RGBAF16toBGRAU8(src + 5);
					dst[6] = RGBAF16toBGRAU8(src + 6);
					dst[7] = RGBAF16toBGRAU8(src + 7);
				}
				for (; src != srcEnd1; dst++, src++)
					*dst = RGBAF16toBGRAU8(src);
			}
			#undef RGBAF16toBGRAU8
		}

		void BGRResizeLinear()
		{
			const size_t w = Width, h = ResizeToHeight, ResizeFromPitch = ResizeFromWidth * 3;
			const double aw = (double)w, ah = (double)h;
			const double scale = max(ResizeFromWidth / aw, ResizeFromHeight / ah);
			const double ax = (aw - (ResizeFromWidth  / scale)) / 2.0;
			const double ay = (ah - (ResizeFromHeight / scale)) / 2.0;
			const uint8_t *src = (const uint8_t*)BufIn, BlackPixel[3] = {0, 0, 0};
			uint8_t *dst = (uint8_t*)BufOut + (RowStart * Width * 3);
			for (size_t y = RowStart, yEnd = RowEnd, isMaxW = ResizeFromWidth, isOffsetMax = ResizeFromHeight * ResizeFromPitch; y != yEnd; y++)
				for (size_t x = 0; x != w; x++, dst += 3)
				{
					const size_t isx = (size_t)((x-ax)*scale), isy = (size_t)((y-ay)*scale);
					const size_t isOffset = (isx > isMaxW ? isOffsetMax : isy * ResizeFromPitch + isx * 3);
					memcpy(dst, (isOffset >= isOffsetMax ? BlackPixel : src + isOffset), 3);
				}
		}

		void BGRAResizeLinear()
		{
			const size_t w = Width, h = ResizeToHeight, fromw = ResizeFromWidth;
			const double aw = (double)w, ah = (double)h;
			const double scale = max(ResizeFromWidth / aw, ResizeFromHeight / ah);
			const double ax = (aw - (ResizeFromWidth  / scale)) / 2.0;
			const double ay = (ah - (ResizeFromHeight / scale)) / 2.0;
			const uint32_t *src = (const uint32_t*)BufIn;
			uint32_t *dst = (uint32_t*)BufOut + (RowStart * Width);
			for (size_t y = RowStart, yEnd = RowEnd, isMaxW = ResizeFromWidth, isOffsetMax = ResizeFromHeight * fromw; y != yEnd; y++)
				for (size_t x = 0; x != w; x++, dst++)
				{
					const size_t isx = (size_t)((x-ax)*scale), isy = (size_t)((y-ay)*scale);
					const size_t isOffset = (isx > isMaxW ? isOffsetMax : isy * fromw + isx);
					*dst = (isOffset >= isOffsetMax ? 0 : src[isOffset]);
				}
			UCASSERT(dst ==  (uint32_t*)BufOut + (RowEnd * Width));
		}

		void BGRMirrorHorizontal()
		{
			uint8_t *dst = (uint8_t*)BufOut + (RowStart * Width * 3), *dstEnd = (uint8_t*)BufOut + (RowEnd * Width * 3);
			for (size_t dstPitch = Width * 3; dst != dstEnd; dst += dstPitch)
				for (uint8_t tmp[3], *dstA = dst, *dstB = dst + dstPitch - 3; dstA < dstB; dstA += 3, dstB -= 3)
					memcpy(tmp, dstA, 3), memcpy(dstA, dstB, 3), memcpy(dstB, tmp, 3);
		}

		void BGRAMirrorHorizontal()
		{
			uint32_t *dst = (uint32_t*)BufOut + (RowStart * Width), *dstEnd = (uint32_t*)BufOut + (RowEnd * Width);
			for (size_t w = Width; dst != dstEnd; dst += w)
				for (uint32_t tmp, *dstA = dst, *dstB = dst + w - 1; dstA < dstB; dstA++, dstB--)
					tmp = *dstA, *dstA = *dstB, *dstB = tmp;
		}
	};

	struct ProcessWorkers
	{
		ProcessWorkers() : WorkersRunning(WORKERCOUNT)
		{
			for (size_t i = 0; i != WORKERCOUNT; i++) Threads[i].Start((sThread::FUNC_t)&ProcessThread, (void*)this);
		}

		~ProcessWorkers()
		{
			WorkersRunning = 0;
			for (size_t i = 0; i != WORKERCOUNT; i++) NewJobSemaphore.Post(); //wake up all threads
			for (size_t i = 0; i != WORKERCOUNT; i++)
			{
				Threads[i].Stop();
			}
		}
	
		void StartNewJob(ProcessJob NewJob)
		{
			//Notify threads of new work to do
			WorkingJobCount = 0;
			size_t Num = NewJob.RowEnd;
			for (size_t i = 0; i != WORKERCOUNT; i++)
			{
				NewJob.RowStart = Num * (i  ) / (WORKERCOUNT + 1);
				NewJob.RowEnd   = Num * (i+1) / (WORKERCOUNT + 1);
				Jobs[i] = NewJob;
				NewJobSemaphore.Post();
			}

			//Do work in the main thread as well
			NewJob.RowStart = NewJob.RowEnd;
			NewJob.RowEnd   = Num;
			NewJob.Execute();

			//Wait for threads to finish working
			for (size_t i = 0; i != WORKERCOUNT && JobDoneSemaphore.WaitForPost(); i++) {}
		}

	private:
		//Wrapper objects for Windows concurrency objects (thread, mutex, semaphore)
		struct sThread
		{
			typedef DWORD(WINAPI* FUNC_t)(LPVOID);
			sThread() : h(0)
			{
			}
			sThread(FUNC_t f, void* p = NULL) : h(0)
			{
				Start(f, p);
			}
			void Start(FUNC_t f, void* p = NULL)
			{
				if (h)
					this->~sThread();
				h = CreateThread(0, 0, f, p, 0, 0);
			}
			void Stop()
			{
				if (h)
				{
					WaitForSingleObject(h, INFINITE);
					CloseHandle(h);
					h = 0;
				}
			}
			~sThread()
			{
				if (h)
				{
					WaitForSingleObject(h, INFINITE);
					CloseHandle(h);
				}
			}
		private:
			HANDLE h;
			sThread(const sThread&);
			sThread& operator=(const sThread&);
		};

		struct sMutex
		{
			sMutex() : h(CreateMutexA(0, 0, 0)) {}
			~sMutex() { CloseHandle(h); }
			__inline void Lock()
			{
				WaitForSingleObject(h, INFINITE);
			}
			__inline void Unlock()
			{
				ReleaseMutex(h);
			}
		private:
			HANDLE h;
			sMutex(const sMutex&);
			sMutex& operator=(const sMutex&);
		};

		struct sSemaphore
		{
			sSemaphore() : h(CreateSemaphoreA(0, 0, 32768, 0)) {}
			~sSemaphore() { CloseHandle(h); }
			__inline void Post()
			{
				ReleaseSemaphore(h, 1, 0);
			}
			__inline bool WaitForPost()
			{
				return WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0;
			}
		private:
			HANDLE h;
			sSemaphore(const sSemaphore&);
			sSemaphore& operator=(const sSemaphore&);
		};

		enum { WORKERCOUNT = 3 };
		sMutex JobsMutex;
		sThread Threads[WORKERCOUNT];
		ProcessJob Jobs[WORKERCOUNT];
		sSemaphore NewJobSemaphore, JobDoneSemaphore;
		size_t WorkingJobCount, WorkersRunning;

		static void ProcessThread(ProcessWorkers* mw)
		{
			while (mw->NewJobSemaphore.WaitForPost() && mw->WorkersRunning)
			{
				mw->JobsMutex.Lock();
				size_t MyJob = mw->WorkingJobCount++;
				mw->JobsMutex.Unlock();
				mw->Jobs[MyJob].Execute();
				mw->JobDoneSemaphore.Post();
			}
		}
	};

	struct ProcessState
	{
		uint8_t* Buf;
		int BufWidth, BufHeight, BufBPP;
		CCaptureStream* Owner;
	};

	static void ProcessImage(int InWidth, int InHeight, int InStride, SharedImageMemory::EFormat Format, SharedImageMemory::EResizeMode ResizeMode, SharedImageMemory::EMirrorMode MirrorMode, int Timeout, uint8_t* InBuf, ProcessState* State)
	{
		//Set maximum number of missed frames allowed until we show sending as having stopped
		State->Owner->m_llFrameMissMax = (Timeout + SharedImageMemory::RECEIVE_MAX_WAIT - 1) / SharedImageMemory::RECEIVE_MAX_WAIT;

		const bool NeedResize = (InWidth != State->BufWidth || InHeight != State->BufHeight);
		if (NeedResize && ResizeMode == SharedImageMemory::RESIZEMODE_DISABLED)
		{
			//Show color pattern indicating that the requested resolution does not match the resolution provided by Unity
			char DisplayString1[128], DisplayString2[128], DisplayString3[128];
			char* DisplayStrings[] = { DisplayString1, DisplayString2, DisplayString3 };
			int DisplayStringLens[] = {
				sprintf_s(DisplayString1, sizeof(DisplayString1), "Capture output resolution is %d x %d", State->BufWidth, State->BufHeight),
				sprintf_s(DisplayString2, sizeof(DisplayString2), "Unity render resolution is %d x %d", InWidth, InHeight),
				sprintf_s(DisplayString3, sizeof(DisplayString3), "please set these to match"),
			};
			FillErrorPattern(ErrorDrawModes[EDC_ResolutionMismatch], State, 3, DisplayStrings, DisplayStringLens);
			return;
		}

		if (NeedResize)
		{
			//Prepare buffer for image scaling
			DWORD UnscaledBufSize = (InWidth * InHeight * State->BufBPP);
			if (State->Owner->m_iUnscaledBufSize != UnscaledBufSize)
			{
				if (State->Owner->m_pUnscaledBuf) free(State->Owner->m_pUnscaledBuf);
				State->Owner->m_pUnscaledBuf = (uint8_t*)malloc(UnscaledBufSize);
				State->Owner->m_iUnscaledBufSize = UnscaledBufSize;
			}
		}

		if (Format != SharedImageMemory::FORMAT_UINT8 && (!State->Owner->m_RGBA16Table || State->Owner->m_RGBA16TableFormat != Format))
		{
			//Build a 64k table that maps 16 bit float values (either linear SRGB or gamma RGB) to 8 bit color values
			const bool SRGB = (Format == SharedImageMemory::FORMAT_FP16_LINEAR);
			uint8_t* RGBA16Table = State->Owner->m_RGBA16Table;
			if (!RGBA16Table) RGBA16Table = State->Owner->m_RGBA16Table = (uint8_t*)malloc(0xFFFF+1);
			for(int i = 0; i <= 0xFFFF; i++)
			{
				float f;
				(i & 0x8000 ? f = 0 : (*(uint32_t*)&f = (i << 13) + 0x38000000));
				if (SRGB) f = (f <= 0.0031308f ? (f * 12.92f) : (powf(f, 1.0f / 2.4f) * 1.055f - 0.055f));
				RGBA16Table[i] = (f < 1.0f ? (uint8_t)(f * 255.9999f) : 255);
			}
			State->Owner->m_RGBA16TableFormat = Format;
		}

		//Multi-threaded conversion of RGBA source to 8-bit BGR format while also eliminating possible row gaps (when stride != width)
		ProcessJob Job;
		if (State->BufBPP == 4) Job.Type = (Format == SharedImageMemory::FORMAT_UINT8 ? ProcessJob::JOB_RGBA8toBGRA8 : ProcessJob::JOB_RGBA16toBGRA8);
		else                    Job.Type = (Format == SharedImageMemory::FORMAT_UINT8 ? ProcessJob::JOB_RGBA8toBGR8  : ProcessJob::JOB_RGBA16toBGR8 );
		Job.BufIn = InBuf, Job.BufOut = (NeedResize ? State->Owner->m_pUnscaledBuf : State->Buf);
		Job.Width = InWidth, Job.RowStart = 0, Job.RowEnd = InHeight, Job.RGBAInStride = InStride;
		Job.RGBA16Table = State->Owner->m_RGBA16Table;
		State->Owner->m_ProcessWorkers.StartNewJob(Job);

		if (NeedResize)
		{
			//Multi-threaded image scaling
			Job.Type = (State->BufBPP == 4 ? ProcessJob::JOB_BGRA_RESIZE_LINEAR : ProcessJob::JOB_BGR_RESIZE_LINEAR);
			Job.BufIn = State->Owner->m_pUnscaledBuf, Job.BufOut = State->Buf;
			Job.Width = State->BufWidth, Job.RowStart = 0, Job.RowEnd = State->BufHeight;
			Job.ResizeToHeight = State->BufHeight, Job.ResizeFromWidth = InWidth, Job.ResizeFromHeight = InHeight;
			State->Owner->m_ProcessWorkers.StartNewJob(Job);
		}

		if (MirrorMode == SharedImageMemory::MIRRORMODE_HORIZONTALLY)
		{
			//Multi-threaded horizontal image flipping
			Job.Type = (State->BufBPP == 4 ? ProcessJob::JOB_BGRA_MIRROR_HORIZONTAL : ProcessJob::JOB_BGR_MIRROR_HORIZONTAL);
			Job.BufOut = State->Buf;
			Job.Width = State->BufWidth, Job.RowStart = 0, Job.RowEnd = State->BufHeight;
			State->Owner->m_ProcessWorkers.StartNewJob(Job);
		}
	}

	std::vector<uint8_t> m_StandbyCache;
	int m_StandbyW = 0;
	int m_StandbyH = 0;
	int m_StandbyBPP = 0;
	bool m_StandbyStopped = false;

	void RenderStandbyCard(ProcessState* State, bool stopped)
	{
		if (!State || !State->Buf || State->BufWidth <= 0 || State->BufHeight <= 0) return;
		const int w = State->BufWidth;
		const int h = State->BufHeight;
		const int bpp = State->BufBPP;
		const size_t need = static_cast<size_t>(w) * h * bpp;
		if (m_StandbyW == w && m_StandbyH == h && m_StandbyBPP == bpp &&
		    m_StandbyStopped == stopped && m_StandbyCache.size() == need)
		{
			memcpy(State->Buf, m_StandbyCache.data(), need);
			return;
		}

		struct GdiplusGuard {
			ULONG_PTR token = 0;
			bool ok = false;
			GdiplusGuard() {
				Gdiplus::GdiplusStartupInput input;
				ok = (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok);
			}
			~GdiplusGuard() {
				if (ok) Gdiplus::GdiplusShutdown(token);
			}
		} gdip;

		m_StandbyCache.assign(need, 0);
		if (!gdip.ok)
		{
			memcpy(State->Buf, m_StandbyCache.data(), need);
			return;
		}

		Gdiplus::Bitmap canvas(w, h, PixelFormat32bppARGB);
		Gdiplus::Graphics g(&canvas);
		g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

		// Deep dark-slate gradient background
		Gdiplus::LinearGradientBrush bgBrush(
			Gdiplus::Point(0, 0), Gdiplus::Point(w, h),
			Gdiplus::Color(255, 11, 15, 22),
			Gdiplus::Color(255, 20, 29, 42));
		g.FillRectangle(&bgBrush, 0, 0, w, h);

		// Subtle architectural grid
		const int gridStep = max(24, min(w, h) / 14);
		Gdiplus::Pen gridPen(Gdiplus::Color(18, 120, 170, 220), 1.0f);
		for (int x = 0; x < w; x += gridStep) g.DrawLine(&gridPen, x, 0, x, h);
		for (int y = 0; y < h; y += gridStep) g.DrawLine(&gridPen, 0, y, w, y);

		// Soft cyan radial glow behind the center card
		const float s = min(w / 1920.0f, h / 1080.0f);
		const float cx = w * 0.5f;
		const float cy = h * 0.48f;
		{
			Gdiplus::GraphicsPath glowPath;
			const float rx = 420.0f * s;
			const float ry = 260.0f * s;
			glowPath.AddEllipse(cx - rx, cy - ry, rx * 2.0f, ry * 2.0f);
			Gdiplus::PathGradientBrush glowBrush(&glowPath);
			glowBrush.SetCenterColor(Gdiplus::Color(46, 34, 211, 238));
			Gdiplus::Color surround(0, 11, 15, 22);
			int count = 1;
			glowBrush.SetSurroundColors(&surround, &count);
			g.FillPath(&glowBrush, &glowPath);
		}

		// Center glassmorphic card
		const float cardW = min(w * 0.82f, 920.0f * s);
		const float cardH = min(h * 0.72f, 440.0f * s);
		const float cardX = cx - cardW * 0.5f;
		const float cardY = cy - cardH * 0.5f;
		const float rad = 24.0f * s;
		{
			Gdiplus::GraphicsPath cardPath;
			cardPath.AddArc(cardX, cardY, rad * 2, rad * 2, 180, 90);
			cardPath.AddArc(cardX + cardW - rad * 2, cardY, rad * 2, rad * 2, 270, 90);
			cardPath.AddArc(cardX + cardW - rad * 2, cardY + cardH - rad * 2, rad * 2, rad * 2, 0, 90);
			cardPath.AddArc(cardX, cardY + cardH - rad * 2, rad * 2, rad * 2, 90, 90);
			cardPath.CloseFigure();

			Gdiplus::SolidBrush cardFill(Gdiplus::Color(195, 19, 25, 34));
			g.FillPath(&cardFill, &cardPath);
			Gdiplus::Pen cardBorder(Gdiplus::Color(90, 56, 189, 248), max(1.0f, 1.5f * s));
			g.DrawPath(&cardBorder, &cardPath);
		}

		// Logo on the left of the card
		const float logoSize = 200.0f * s;
		const float logoX = cardX + 64.0f * s;
		const float logoY = cy - logoSize * 0.5f;
		{
			Gdiplus::Bitmap logoBmp(kStandbyLogoW, kStandbyLogoH, kStandbyLogoW * 4,
				PixelFormat32bppARGB, const_cast<BYTE*>(kStandbyLogoBGRA));
			g.DrawImage(&logoBmp, logoX, logoY, logoSize, logoSize);
		}

		// Typography on the right of the logo
		const float textX = logoX + logoSize + 48.0f * s;
		const float textW = (cardX + cardW) - textX - 40.0f * s;
		Gdiplus::FontFamily fontFam(L"Microsoft YaHei");
		Gdiplus::Font titleFont(&fontFam, max(12.0f, 44.0f * s), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
		Gdiplus::Font subFont(&fontFam, max(10.0f, 28.0f * s), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::Font pillFont(&fontFam, max(9.0f, 18.0f * s), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

		Gdiplus::SolidBrush whiteBrush(Gdiplus::Color(255, 240, 246, 252));
		Gdiplus::SolidBrush accentBrush(Gdiplus::Color(255, 56, 189, 248));
		Gdiplus::SolidBrush mutedBrush(Gdiplus::Color(255, 148, 163, 184));

		Gdiplus::StringFormat sf;
		sf.SetAlignment(Gdiplus::StringAlignmentNear);
		sf.SetLineAlignment(Gdiplus::StringAlignmentNear);

		g.DrawString(L"OBScure", -1, &titleFont,
			Gdiplus::RectF(textX, cy - 86.0f * s, textW, 58.0f * s), &sf, &whiteBrush);
		g.DrawString(L"实时音视频混淆解决方案", -1, &subFont,
			Gdiplus::RectF(textX, cy - 22.0f * s, textW, 44.0f * s), &sf, &accentBrush);

		// Status pill
		const wchar_t* statusText = stopped ? L"已停止输出 · 等待重新连接" : L"待机中 · 等待 OBScure 编码输出";
		const float pillY = cy + 40.0f * s;
		const float pillH = 34.0f * s;
		const float pillW = min(textW, 310.0f * s);
		Gdiplus::SolidBrush pillBg(Gdiplus::Color(40, 56, 189, 248));
		Gdiplus::Pen pillBorder(Gdiplus::Color(100, 56, 189, 248), max(1.0f, 1.0f * s));
		g.FillRectangle(&pillBg, textX, pillY, pillW, pillH);
		g.DrawRectangle(&pillBorder, textX, pillY, pillW, pillH);
		Gdiplus::StringFormat pillSf;
		pillSf.SetAlignment(Gdiplus::StringAlignmentCenter);
		pillSf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		g.DrawString(statusText, -1, &pillFont,
			Gdiplus::RectF(textX, pillY, pillW, pillH), &pillSf, &mutedBrush);

		// Copy canvas to BGR24 or BGRA32 cache
		Gdiplus::BitmapData bd{};
		Gdiplus::Rect lockRect(0, 0, w, h);
		if (canvas.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok)
		{
			const uint8_t* srcBase = static_cast<const uint8_t*>(bd.Scan0);
			for (int y = 0; y < h; ++y)
			{
				const uint8_t* srcRow = srcBase + static_cast<size_t>(y) * bd.Stride;
				uint8_t* dstRow = m_StandbyCache.data() + static_cast<size_t>(y) * w * bpp;
				if (bpp == 4)
				{
					memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4);
				}
				else
				{
					for (int x = 0; x < w; ++x)
					{
						dstRow[x * 3 + 0] = srcRow[x * 4 + 0];
						dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
						dstRow[x * 3 + 2] = srcRow[x * 4 + 2];
					}
				}
			}
			canvas.UnlockBits(&bd);
		}
		m_StandbyW = w;
		m_StandbyH = h;
		m_StandbyBPP = bpp;
		m_StandbyStopped = stopped;
		memcpy(State->Buf, m_StandbyCache.data(), need);
	}

	static void FillErrorPattern(EErrorDrawMode edm, ProcessState* State, int LineCount = 0, char** LineStrings = NULL, int* LineLengths = NULL, LONGLONG FrameNumber = -1)
	{
		if (FrameNumber >= 0 && FrameNumber < 5) edm = EDM_BLACK; //show errors as just black during the first 5 frames (when starting)
		BYTE *p = State->Buf, *pEnd = State->Buf + (State->BufWidth * State->BufHeight * State->BufBPP), SkipCount = State->BufBPP - 3;
		switch (edm)
		{
			case EDM_GREENKEY:    while (p != pEnd) { *(p++) = 0x00; *(p++) = 0xFE; *(p++) = 0x00;           p += SkipCount; } break; //Filled with 0x00FE00 (BGR colors)
			case EDM_GREENYELLOW: while (p != pEnd) { *(p++) = 0x00; *(p++) = 0xFF; *(p++) = (size_t)p%0xFF; p += SkipCount; } break; //Green/yellow color pattern (BGR colors)
			case EDM_BLUEPINK:    while (p != pEnd) { *(p++) = 0xFF; *(p++) = 0x00; *(p++) = (size_t)p%0xFF; p += SkipCount; } break; //Blue/pink color pattern (BGR colors)
			case EDM_BLACK:       ZeroMemory(State->Buf, (State->BufWidth * State->BufHeight * State->BufBPP)); break; //Filled with black
		}

		if (LineCount && edm != EDM_BLACK && edm != EDM_GREENKEY && State->BufHeight >= LineCount * 20)
		{
			void* pTextBuf;
			HDC TextDC = CreateCompatibleDC(0);
			BITMAPINFO TextBMI = { sizeof(BITMAPINFOHEADER), State->BufWidth, LineCount * 20, 1, 8 * State->BufBPP, 0, LineCount * 20 * State->BufWidth * State->BufBPP };
			TextBMI.bmiHeader.biHeight = LineCount * 20;
			HBITMAP TextHBitmap = CreateDIBSection(TextDC, &TextBMI, DIB_RGB_COLORS, &pTextBuf, NULL, 0);
			SelectObject(TextDC, TextHBitmap);
			SetBkMode(TextDC, TRANSPARENT);
			SetTextColor(TextDC, RGB(255, 0, 0));
			for (int i = 0; i < LineCount; i++) TextOutA(TextDC, 10,  i * 20, LineStrings[i], LineLengths[i]);
			memcpy(State->Buf + ((State->BufHeight - TextBMI.bmiHeader.biHeight) / 2) * State->BufWidth * State->BufBPP, pTextBuf, TextBMI.bmiHeader.biHeight * State->BufWidth * State->BufBPP);
			DeleteObject(TextHBitmap);
			DeleteDC(TextDC);
		}
		if (State->BufBPP == 4)
		{
			BYTE FillAlpha = (edm == EDM_GREENKEY ? 0x0 : (edm == EDM_BLACK ? 0x0 : 0xA0));
			for (p = State->Buf; p != pEnd; p += 4) p[3] = FillAlpha;
		}
	}

	static void RenderFPSDisplay(ProcessState* State)
	{
		static LONGLONG MyFPS = 0, MyLastFPSTime = GetTickCount64(), MyLastFPS = 0;
		for (MyFPS++; GetTickCount64() - MyLastFPSTime > 1000; MyFPS = 0, MyLastFPSTime += 1000) { MyLastFPS = MyFPS; }
		char DisplayString[128];
		int DisplayStringLen = sprintf_s(DisplayString, sizeof(DisplayString), "%d FPS", MyLastFPS);

		void* pTextBuf;
		HDC TextDC = CreateCompatibleDC(0);
		BITMAPINFO TextBMI = { sizeof(BITMAPINFOHEADER), State->BufWidth, 20, 1, 8 * State->BufBPP, 0, 20 * State->BufWidth * State->BufBPP };
		HBITMAP TextHBitmap = CreateDIBSection(TextDC, &TextBMI, DIB_RGB_COLORS, &pTextBuf, NULL, 0);
		SelectObject(TextDC, TextHBitmap);
		SetBkMode(TextDC, TRANSPARENT);
		SetTextColor(TextDC, RGB(0, 255, 0));
		TextOutA(TextDC, 10, 0, DisplayString, DisplayStringLen);
		if (State->BufBPP == 4) for (BYTE *p = (BYTE*)pTextBuf, *pEnd = p + 20 * State->BufWidth * 4; p != pEnd; p += 4) p[3] = 0xFF;
		memcpy(State->Buf, pTextBuf, TextBMI.bmiHeader.biHeight * State->BufWidth * State->BufBPP);
		DeleteObject(TextHBitmap);
		DeleteDC(TextDC);
	}

	//IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == NULL) return E_POINTER;
		else if (riid == _uuidof(IAMStreamConfig)) { *ppv = (IAMStreamConfig*)this; AddRef(); return S_OK; }
		else if (riid == _uuidof(IKsPropertySet))  { *ppv = (IKsPropertySet*)this;  AddRef(); return S_OK; }
		return CSourceStream::QueryInterface(riid, ppv);
	}

	STDMETHODIMP_(ULONG) AddRef() override  { return GetOwner()->AddRef();  }
	STDMETHODIMP_(ULONG) Release() override { return GetOwner()->Release(); }

	STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void ** ppv) override
	{
		if (ppv == NULL) return E_POINTER;
		else if (riid == IID_IKsPropertySet)  { *ppv = (IKsPropertySet*)this;  AddRef(); return S_OK; }
		else if (riid == IID_IQualityControl) { *ppv = (IQualityControl*)this; AddRef(); return S_OK; }
		else if (riid == IID_IAMStreamConfig) { *ppv = (IAMStreamConfig*)this; AddRef(); return S_OK; }
		return CSourceStream::NonDelegatingQueryInterface(riid, ppv);
	}

	STDMETHODIMP QuerySupported(REFGUID rguidPropSet, ULONG ulId, PULONG pulTypeSupport) override
	{
		if (rguidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
		if (ulId != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
		if (pulTypeSupport) *pulTypeSupport = KSPROPERTY_SUPPORT_GET; // We support getting this property, but not setting it.
		return S_OK;

		//if(rguidPropSet == AMPROPSETID_Pin && ulId == AMPROPERTY_PIN_CATEGORY) { *pulTypeSupport = KSPROPERTY_SUPPORT_GET; return S_OK; }
		//return E_NOTIMPL;
	}

	STDMETHODIMP Get(REFGUID rguidPropSet, ULONG ulId, LPVOID pInstanceData, ULONG ulInstanceLength, LPVOID pPropertyData, ULONG ulDataLength, PULONG pulBytesReturned) override
	{
		if (rguidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
		if (ulId != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
		if (pPropertyData == NULL && pulBytesReturned == NULL) return E_POINTER;

		if (pulBytesReturned) *pulBytesReturned = sizeof(GUID);
		if (pPropertyData == NULL) return S_OK; // Caller just wants to know the size. 
		if (ulDataLength < sizeof(GUID)) return E_UNEXPECTED; // The buffer is too small.

		*(GUID *)pPropertyData = PIN_CATEGORY_CAPTURE;
		return S_OK;

		//if(rguidPropSet == AMPROPSETID_Pin && ulId == AMPROPERTY_PIN_CATEGORY)
		//{
		//	if (pPropertyData == NULL) return E_POINTER;
		//	if (ulDataLength != sizeof(GUID)) return E_INVALIDARG;
		//	memcpy(pPropertyData, &PIN_CATEGORY_CAPTURE, sizeof(GUID));
		//	*pulBytesReturned = sizeof(GUID);
		//	return S_OK;
		//}
		//return E_NOTIMPL;
	}

	STDMETHODIMP Set(REFGUID rguidPropSet, ULONG ulId, LPVOID pInstanceData, ULONG ulInstanceLength, LPVOID pPropertyData, ULONG ulDataLength) override { return E_NOTIMPL; }
	STDMETHODIMP Notify(IBaseFilter *pSelf, Quality q) override { return S_OK; }
	STDMETHODIMP SetSink(IQualityControl *piqc) override { return S_OK; }

	HRESULT DecideBufferSize(IMemAllocator * pAlloc, ALLOCATOR_PROPERTIES * pRequest) override
	{
		if (pAlloc == NULL || pRequest == NULL) DebugLog("[DecideBufferSize] E_POINTER\n");
		if (pAlloc == NULL || pRequest == NULL) return E_POINTER;
		CAutoLock cAutoLock(m_pFilter->pStateLock());
		HRESULT hr = NOERROR;
		VIDEOINFO *pvi = (VIDEOINFO*)m_mt.Format();
		pRequest->cBuffers = 1;

		DebugLog("[DecideBufferSize] Request Size: %d - Have Size: %d\n", (int)pvi->bmiHeader.biSizeImage, (int)pRequest->cbBuffer);
		if (pvi->bmiHeader.biSizeImage > (DWORD)pRequest->cbBuffer)
			pRequest->cbBuffer = pvi->bmiHeader.biSizeImage;

		ALLOCATOR_PROPERTIES actual;
		hr = pAlloc->SetProperties(pRequest, &actual);
		if (FAILED(hr)) DebugLog("[DecideBufferSize] E_SOMETHING\n");
		if (FAILED(hr)) return hr;

		DebugLog("[DecideBufferSize] Request Size: %d - Actual Size: %d\n", (int)pvi->bmiHeader.biSizeImage, (int)actual.cbBuffer);
		return (actual.cbBuffer < pRequest->cbBuffer ? E_FAIL : S_OK);
	}

	STDMETHODIMP SetFormat(AM_MEDIA_TYPE *pmt) override
	{
		if (pmt == NULL) DebugLog("[SetFormat] E_POINTER\n");
		if (pmt == NULL) return E_POINTER;

		VIDEOINFO* pvi = (VIDEOINFO*)pmt->pbFormat;
		if (pvi == NULL) DebugLog("[SetFormat] E_UNEXPECTED (pvi is null)\n");
		if (pvi == NULL) return E_UNEXPECTED;

		const bool isNV12 = (pvi->bmiHeader.biCompression == MAKEFOURCC('N', 'V', '1', '2'));
		const bool isYUY2 = (pvi->bmiHeader.biCompression == MAKEFOURCC('Y', 'U', 'Y', '2'));
		if (!isNV12 && !isYUY2)
		{
			bool HasStrideBytes = (DIBSIZE(pvi->bmiHeader) != pvi->bmiHeader.biWidth * pvi->bmiHeader.biHeight * pvi->bmiHeader.biBitCount / 8);
			if (HasStrideBytes) DebugLog("[SetFormat] E_FAIL (has stride bytes)\n");
			if (HasStrideBytes) return E_FAIL;
		}

		DebugLog("[SetFormat] WIDTH: %d - HEIGHT: %d - BITS: %d - TPS: %d - SIZE: %d - SIZE CALC: %d\n", (int)pvi->bmiHeader.biWidth, (int)pvi->bmiHeader.biHeight, (int)pvi->bmiHeader.biBitCount, (int)pvi->AvgTimePerFrame,
			(int)pvi->bmiHeader.biSizeImage, (int)DIBSIZE(pvi->bmiHeader));
		m_avgTimePerFrame = pvi->AvgTimePerFrame;
		m_mt = *pmt;
		VIDEOINFO* pOut = (VIDEOINFO*)m_mt.pbFormat;
		const int outW = pOut->bmiHeader.biWidth;
		const int outH = (pOut->bmiHeader.biHeight < 0 ? -pOut->bmiHeader.biHeight : pOut->bmiHeader.biHeight);
		pOut->bmiHeader.biHeight = outH;
		pOut->bmiHeader.biSizeImage = (isNV12 ? (outW * outH * 3 / 2) : (isYUY2 ? (outW * outH * 2) : DIBSIZE(pOut->bmiHeader)));
		return S_OK;
	}

	STDMETHODIMP GetFormat(AM_MEDIA_TYPE **ppmt) override
	{
		if (ppmt == NULL) DebugLog("[GetFormat] E_POINTER\n");
		if (ppmt == NULL) return E_POINTER;
		DebugLog("[GetFormat] RETURNING WIDTH: %d - HEIGHT: %d - BITS: %d - TPS: %d - SIZEIMAGE: %d - SIZECALC: %d\n", (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biWidth, (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biHeight, (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biBitCount, (int)((VIDEOINFO*)m_mt.Format())->AvgTimePerFrame, (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biSizeImage, (int)DIBSIZE(((VIDEOINFO*)m_mt.Format())->bmiHeader));
		*ppmt = CreateMediaType(&m_mt);
		return S_OK;
	}

	STDMETHODIMP GetNumberOfCapabilities(int *piCount, int *piSize) override
	{
		if (piCount == NULL || piSize == NULL) DebugLog("[GetNumberOfCapabilities] E_POINTER\n");
		if (piCount == NULL || piSize == NULL) return E_POINTER;
		*piCount = (sizeof(_media)/sizeof(_media[0])*3); //NV12, YUY2 and RGB24 variations
		*piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
		DebugLog("[GetNumberOfCapabilities] Returning Count: %d - Size: %d\n", *piCount, *piSize);
		return S_OK;
	}

	STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE **ppmt, BYTE *pSCC) override
	{
		if (ppmt == NULL || pSCC == NULL) DebugLog("[GetStreamCaps] E_POINTER\n");
		if (ppmt == NULL || pSCC == NULL) return E_POINTER;

		CMediaType mt;
		HRESULT hr = GetMediaType(iIndex, &mt);
		if (FAILED(hr)) return hr;
		VIDEOINFO *pvi = (VIDEOINFO*)mt.Format();

		*ppmt = CreateMediaType(&mt);

		VIDEO_STREAM_CONFIG_CAPS* pCaps = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
		ZeroMemory(pCaps, sizeof(VIDEO_STREAM_CONFIG_CAPS));

		pCaps->guid = FORMAT_VideoInfo;
		pCaps->VideoStandard      = 0;
		pCaps->CropAlignX         = 1;
		pCaps->CropAlignY         = 1;
		pCaps->OutputGranularityX = 1;
		pCaps->OutputGranularityY = 1;
		pCaps->StretchTapsX       = 2;
		pCaps->StretchTapsY       = 2;
		pCaps->ShrinkTapsX        = 2;
		pCaps->ShrinkTapsY        = 2;
		pCaps->InputSize.cx       = pvi->bmiHeader.biWidth;
		pCaps->InputSize.cy       = pvi->bmiHeader.biHeight;
		pCaps->MinCroppingSize.cx = 1;
		pCaps->MinCroppingSize.cy = 1;
		pCaps->MaxCroppingSize.cx = pvi->bmiHeader.biWidth;
		pCaps->MaxCroppingSize.cy = pvi->bmiHeader.biHeight;
		pCaps->CropGranularityX   = 1;
		pCaps->CropGranularityY   = 1;
		pCaps->MinOutputSize.cx   = 4;
		pCaps->MinOutputSize.cy   = 4;
		pCaps->MaxOutputSize.cx   = pvi->bmiHeader.biWidth;
		pCaps->MaxOutputSize.cy   = pvi->bmiHeader.biHeight;
		pCaps->MinFrameInterval = 10000000 / 120;
		pCaps->MaxFrameInterval = 10000000 / 30;
		pCaps->MinBitsPerSecond = pCaps->MinOutputSize.cx * pCaps->MinOutputSize.cy * pvi->bmiHeader.biBitCount * 30;
		pCaps->MaxBitsPerSecond = pCaps->MaxOutputSize.cx * pCaps->MaxOutputSize.cy * pvi->bmiHeader.biBitCount * 120;
		DebugLog("[GetStreamCaps] Index: %d - MINWIDTH: %d - MINHEIGHT: %d - MAXWIDTH: %d - MAXHEIGHT: %d - BITS: %d - TPS: %d - SIZEIMAGE: %d - SIZECALC: %d\n", iIndex, (int)pCaps->MinOutputSize.cx, (int)pCaps->MinOutputSize.cy, (int)pCaps->MaxOutputSize.cx, (int)pCaps->MaxOutputSize.cy, (int)pvi->bmiHeader.biBitCount, (int)pvi->AvgTimePerFrame, (int)pvi->bmiHeader.biSizeImage, (int)DIBSIZE(pvi->bmiHeader));
		return S_OK;
	}

	HRESULT SetMediaType(const CMediaType *pmt) override
	{
		VIDEOINFOHEADER* pvi = (VIDEOINFOHEADER*)(pmt->Format());
		DebugLog("[SetMediaType] [ASKD] WIDTH: %d - HEIGHT: %d - BITS: %d - TPS: %d - SIZEIMAGE: %d - SIZECALC: %d\n", (int)pvi->bmiHeader.biWidth, (int)pvi->bmiHeader.biHeight, (int)pvi->bmiHeader.biBitCount, (int)pvi->AvgTimePerFrame, (int)pvi->bmiHeader.biSizeImage, (int)DIBSIZE(pvi->bmiHeader));
		DebugLog("[SetMediaType] [HAVE] WIDTH: %d - HEIGHT: %d - BITS: %d - TPS: %d - SIZEIMAGE: %d - SIZECALC: %d\n", (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biWidth, (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biHeight, (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biBitCount, (int)((VIDEOINFO*)m_mt.Format())->AvgTimePerFrame, (int)((VIDEOINFO*)m_mt.Format())->bmiHeader.biSizeImage, (int)DIBSIZE(((VIDEOINFO*)m_mt.Format())->bmiHeader));
		HRESULT hr = CSourceStream::SetMediaType(pmt);
		if (SUCCEEDED(hr))
		{
			if (IsEqualGUID(pmt->subtype, MEDIASUBTYPE_NV12)) m_OutputFormat = FMT_NV12;
			else if (IsEqualGUID(pmt->subtype, MEDIASUBTYPE_YUY2)) m_OutputFormat = FMT_YUY2;
			else if (pvi && pvi->bmiHeader.biBitCount == 32) m_OutputFormat = FMT_ARGB32;
			else m_OutputFormat = FMT_RGB24;
		}
		return hr;
	}

	HRESULT CheckMediaType(const CMediaType *pMediaType) override
	{
		CAutoLock lock(m_pFilter->pStateLock());
		VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER *)(pMediaType->Format());
		if (!pvi) return E_INVALIDARG;
		if (!IsEqualGUID(pMediaType->subtype, m_mt.subtype)) return VFW_E_TYPE_NOT_ACCEPTED;
		VIDEOINFOHEADER *pHave = (VIDEOINFOHEADER *)m_mt.Format();
		if (!pHave) return E_INVALIDARG;
		if (pvi->bmiHeader.biWidth != pHave->bmiHeader.biWidth) return E_INVALIDARG;
		const int wantH = (pvi->bmiHeader.biHeight < 0 ? -pvi->bmiHeader.biHeight : pvi->bmiHeader.biHeight);
		const int haveH = (pHave->bmiHeader.biHeight < 0 ? -pHave->bmiHeader.biHeight : pHave->bmiHeader.biHeight);
		if (wantH != haveH) return E_INVALIDARG;
		return S_OK;
	}

	HRESULT GetMediaType(int iPos, CMediaType *pMediaType) override
	{
		CheckPointer(pMediaType, E_POINTER);
		if (iPos < 0) return E_INVALIDARG;
		const int nMedia = (int)(sizeof(_media)/sizeof(_media[0]));
		if (iPos >= nMedia * 3) return VFW_S_NO_MORE_ITEMS;
		CAutoLock cAutoLock(m_pFilter->pStateLock()); 

		const int iMedia = iPos / 3;
		const int variant = iPos % 3;
		const bool nv12 = (variant == 0);
		const bool yuy2 = (variant == 1);
		UCASSERT(_media[iMedia].width * _media[iMedia].height * 4 * sizeof(short) <= MAX_SHARED_IMAGE_SIZE);
		VIDEOINFO *pvi = (VIDEOINFO *)pMediaType->AllocFormatBuffer(sizeof(VIDEOINFO));
		ZeroMemory(pvi, sizeof(VIDEOINFO));
		pvi->AvgTimePerFrame = m_avgTimePerFrame;
		BITMAPINFOHEADER *pBmi = &(pvi->bmiHeader);
		pBmi->biSize = sizeof(BITMAPINFOHEADER);
		pBmi->biWidth  = (_media[iMedia].width  ? _media[iMedia].width  : ((VIDEOINFO*)m_mt.pbFormat)->bmiHeader.biWidth );
		pBmi->biHeight = (_media[iMedia].height ? _media[iMedia].height : ((VIDEOINFO*)m_mt.pbFormat)->bmiHeader.biHeight);
		pBmi->biPlanes = 1;
		pBmi->biBitCount = (nv12 ? 12 : (yuy2 ? 16 : 24));
		pBmi->biCompression = (nv12 ? MAKEFOURCC('N', 'V', '1', '2') : (yuy2 ? MAKEFOURCC('Y', 'U', 'Y', '2') : BI_RGB));
		pBmi->biSizeImage = (nv12 ? (pBmi->biWidth * pBmi->biHeight * 3 / 2) : (yuy2 ? (pBmi->biWidth * pBmi->biHeight * 2) : (pBmi->biWidth * pBmi->biHeight * 3)));

		//DebugLog("[GetMediaType] iPos: %d - WIDTH: %d - HEIGHT: %d - BITS: %d - TPS: %d\n", iPos, (int)pvi->bmiHeader.biWidth, (int)pvi->bmiHeader.biHeight, (int)pvi->bmiHeader.biBitCount, (int)pvi->AvgTimePerFrame);

		pMediaType->SetType(&MEDIATYPE_Video);
		pMediaType->SetFormatType(&FORMAT_VideoInfo);
		pMediaType->SetSubtype(nv12 ? &MEDIASUBTYPE_NV12 : (yuy2 ? &MEDIASUBTYPE_YUY2 : &MEDIASUBTYPE_RGB24));
		pMediaType->SetSampleSize(pBmi->biSizeImage);
		pMediaType->SetTemporalCompression(FALSE);
		return S_OK;
	}

	HRESULT OnThreadStartPlay() override
	{
		DebugLog("[OnThreadStartPlay] OnThreadStartPlay\n");
		m_llFrame = m_llFrameMissCount = 0;
		m_llFrameMissMax = 5;
		return CSourceStream::OnThreadStartPlay();
	}

	CMediaType m_mt;
	LONGLONG m_llFrame, m_llFrameMissCount, m_llFrameMissMax;
	REFERENCE_TIME m_prevStartTime;
	REFERENCE_TIME m_avgTimePerFrame;
	SharedImageMemory* m_pReceiver;
	ProcessWorkers m_ProcessWorkers;
	DWORD m_iUnscaledBufSize;
	uint8_t *m_pUnscaledBuf, *m_RGBA16Table;
	SharedImageMemory::EFormat m_RGBA16TableFormat;

	//IAMStreamControl
	HRESULT STDMETHODCALLTYPE StartAt(const REFERENCE_TIME *ptStart, DWORD dwCookie) override { return NOERROR; }
	HRESULT STDMETHODCALLTYPE StopAt(const REFERENCE_TIME *ptStop, BOOL bSendExtra, DWORD dwCookie) override { return NOERROR; }
	HRESULT STDMETHODCALLTYPE GetInfo(AM_STREAM_INFO *pInfo) override { return NOERROR; }

	// IAMPushSource
	HRESULT STDMETHODCALLTYPE GetLatency(REFERENCE_TIME *prtLatency) override { return NOERROR; }
	HRESULT STDMETHODCALLTYPE GetPushSourceFlags(ULONG *pFlags) override { *pFlags = AM_PUSHSOURCECAPS_INTERNAL_RM; return NOERROR; }
	HRESULT STDMETHODCALLTYPE SetPushSourceFlags(ULONG Flags) override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE SetStreamOffset(REFERENCE_TIME rtOffset) override { return NOERROR; }
	HRESULT STDMETHODCALLTYPE GetStreamOffset(REFERENCE_TIME *prtOffset) override { *prtOffset = 0; return NOERROR; }
	HRESULT STDMETHODCALLTYPE GetMaxStreamOffset(REFERENCE_TIME *prtMaxOffset) override { *prtMaxOffset = 0; return NOERROR; }
	HRESULT STDMETHODCALLTYPE SetMaxStreamOffset(REFERENCE_TIME rtMaxOffset) override { return NOERROR; }
};

class CCaptureProperties : public CBasePropertyPage
{
public:
	static CUnknown * WINAPI CreateInstance(LPUNKNOWN lpunk, HRESULT *phr)
	{
		CUnknown *punk = new CCaptureProperties(lpunk, phr);
		*phr = (punk ? S_OK : E_OUTOFMEMORY);
		return punk;
	}

private:
	CCaptureProperties(LPUNKNOWN lpunk, HRESULT *phr) : CBasePropertyPage("", lpunk, -1, -1) { }

	STDMETHODIMP Activate(HWND hwndParent, LPCRECT prect, BOOL fModal) 
	{
		struct MyData
		{
			#pragma pack(4)
			DLGTEMPLATE Header;
			#pragma pack(2)
			WORD NoMenu, StdClass; wchar_t Title[1]; // 0 - no menu | 0 - standard dialog class | No title
			#pragma pack(4)
			struct Item
			{
				#pragma pack(4)
				DLGITEMTEMPLATE Header;
				#pragma pack(2)
				WORD FFFF, ClassID; wchar_t Text[2]; WORD NoData;
				#pragma pack(4)
			} Items[8];
			#pragma pack(4)
		} md = {
			{ WS_CHILD | WS_VISIBLE | DS_CENTER, NULL, sizeof(md.Items)/sizeof(MyData::Item) }, 0, 0, L"", {
			{ { WS_VISIBLE | WS_CHILD | SS_LEFT,                       NULL ,  5, 18,   80,  10, 1000 }, 0xFFFF, 0x0082, L"-" }, //Label
			{ { WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST, NULL , 90, 17,  150, 100, 1001 }, 0xFFFF, 0x0085, L"-" }, //Combo Box
			{ { WS_VISIBLE | WS_CHILD | SS_LEFT,                       NULL ,  5, 36,   80,  10, 1002 }, 0xFFFF, 0x0082, L"-" }, //Label
			{ { WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST, NULL , 90, 35,  150, 100, 1003 }, 0xFFFF, 0x0085, L"-" }, //Combo Box
			{ { WS_VISIBLE | WS_CHILD | SS_LEFT,                       NULL ,  5, 54,   80,  10, 1004 }, 0xFFFF, 0x0082, L"-" }, //Label
			{ { WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST, NULL , 90, 53,  150, 100, 1005 }, 0xFFFF, 0x0085, L"-" }, //Combo Box
			{ { WS_VISIBLE | WS_CHILD | SS_LEFT,                       NULL ,  5, 72,   80,  10, 1006 }, 0xFFFF, 0x0082, L"-" }, //Label
			{ { WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_CHECKBOX,      NULL , 90, 71,  150,  10, 1007 }, 0xFFFF, 0x0080, L"-" }, //Check Box
		}};

		HWND hwnd = CreateDialogIndirectParamW(NULL, &md.Header, hwndParent, &MyDialogProc, (LPARAM)this);
		SetDlgItemTextW(hwnd, 1000, L"Resolution mismatch:");
		SetDlgItemTextW(hwnd, 1002, L"Unity never started:");
		SetDlgItemTextW(hwnd, 1004, L"Unity sending stopped:");
		SetDlgItemTextW(hwnd, 1006, L"Display FPS:");
		SetDlgItemTextW(hwnd, 1007, L"Show capture frame rate");
		for (int i = 0; i < 3; i++)
		{
			HWND hWndComboBox = GetDlgItem(hwnd, 1001 + i*2);
			for (int j = 0; j < sizeof(ErrorDrawModeNames)/sizeof(ErrorDrawModeNames[0]); j++)
				SendMessageW(hWndComboBox, (UINT)CB_ADDSTRING, (WPARAM)0, (LPARAM)ErrorDrawModeNames[j]);
			SendMessageA(hWndComboBox, CB_SETCURSEL, (WPARAM)ErrorDrawModes[i], (LPARAM)0);
		}
		SendMessage(GetDlgItem(hwnd, 1007), BM_SETCHECK, (OutputFrameRate ? BST_CHECKED : BST_UNCHECKED), 0);

		SetWindowPos(hwnd, NULL, prect->left, prect->top, prect->right-prect->left, prect->bottom-prect->top, 0); //show in tab page
		return S_OK;
	}

	static INT_PTR CALLBACK MyDialogProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		if (uMsg == WM_INITDIALOG) return TRUE;
		if (uMsg == WM_COMMAND)
		{
			//DebugLog("[DIALOG] WM_COMMAND - ItemID: %d - SubCommand: %d - Value: %d\n", (int)LOWORD(wParam), (int)HIWORD(wParam), (int)lParam);
			int ItemID = LOWORD(wParam), SubCommand = HIWORD(wParam);
			HWND hWndItem = GetDlgItem(hwnd, ItemID);
			int SelectionIndex = (int)SendMessageA(hWndItem, CB_GETCURSEL, 0, 0);
			if (ItemID == 1001 && SubCommand == 1) ErrorDrawModes[EDC_ResolutionMismatch]  = (EErrorDrawMode)SelectionIndex;
			if (ItemID == 1003 && SubCommand == 1) ErrorDrawModes[EDC_UnityNeverStarted]   = (EErrorDrawMode)SelectionIndex;
			if (ItemID == 1005 && SubCommand == 1) ErrorDrawModes[EDC_UnitySendingStopped] = (EErrorDrawMode)SelectionIndex;
			if (ItemID == 1007) SendMessage(hWndItem, BM_SETCHECK, ((OutputFrameRate ^= 1) ? BST_CHECKED : BST_UNCHECKED), 0);
			return TRUE;
		}
		return FALSE;
	}

	STDMETHODIMP GetPageInfo(__out LPPROPPAGEINFO pPageInfo)
	{
		pPageInfo->pszTitle = (WCHAR*)CoTaskMemAlloc(sizeof(CaptureSourceName));
		memcpy(pPageInfo->pszTitle, CaptureSourceName, sizeof(CaptureSourceName));
		pPageInfo->size.cx      = 490;
		pPageInfo->size.cy      = 200;
		pPageInfo->pszDocString = NULL;
		pPageInfo->pszHelpFile  = NULL;
		pPageInfo->dwHelpContext= 0;
		return NOERROR;
	}
};

class CCaptureSource : CSource, IQualityControl, ICamSource, ISpecifyPropertyPages
{
public:
	static CUnknown * CreateInstance(LPUNKNOWN lpunk, HRESULT *phr, int32_t CapNum)
	{
		UCASSERT(phr);
		*phr = S_OK;

		CCaptureSource *pSource = new CCaptureSource(lpunk, phr);
		if (FAILED(*phr) || !pSource)
		{
			if (!pSource) *phr = E_OUTOFMEMORY;
			delete pSource;
			return NULL;
		}

		CCaptureStream* pStream = new CCaptureStream(pSource, phr, CapNum);
		if (FAILED(*phr) || !pStream)
		{
			if (!pStream) *phr = E_OUTOFMEMORY;
			delete pStream;
			delete pSource;
			return NULL;
		}

		return pSource;
	}

private:
	DECLARE_IUNKNOWN;

	CCaptureSource(LPUNKNOWN lpunk, HRESULT* phr) : CSource("Source", lpunk, CLSID_UnityCaptureService, phr) { }

	//CSource
	STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void ** ppv) override
	{
		if (ppv == NULL) return E_POINTER;
		if      (riid == IID_IQualityControl      ) { *ppv = (IQualityControl*)this;       AddRef(); return S_OK; }
		else if (riid == IID_ICamSource           ) { *ppv = (ICamSource*)this;            AddRef(); return S_OK; }
		else if (riid == IID_ISpecifyPropertyPages) { *ppv = (ISpecifyPropertyPages*)this; AddRef(); return S_OK; } //
		return CSource::NonDelegatingQueryInterface(riid, ppv);
	}

	//IQualityControl
	STDMETHODIMP Notify(IBaseFilter *pSelf, Quality q) override { return S_OK; }
	STDMETHODIMP SetSink(IQualityControl *piqc) override { return S_OK; }

	//ISpecifyPropertyPages
	STDMETHODIMP GetPages(CAUUID * pPages) override
	{
		CheckPointer(pPages,E_POINTER);
		pPages->cElems = 1;
		pPages->pElems = (GUID *) CoTaskMemAlloc(sizeof(GUID));
		if (pPages->pElems == NULL) return E_OUTOFMEMORY;
		*(pPages->pElems) = CLSID_UnityCaptureProperties;
		return NOERROR;
	}
};

static const AMOVIESETUP_MEDIATYPE sudMediaTypesCaptureSourceOut = { &MEDIATYPE_Video, &MEDIASUBTYPE_NULL };
static const AMOVIESETUP_PIN sudCaptureSourceOut = {
	L"Output",   // Pin string name
	FALSE,       // Is it rendered
	TRUE,        // Is it an output
	FALSE,       // Can we have none
	FALSE,       // Can we have many
	&CLSID_NULL, // Connects to filter
	NULL,        // Connects to pin
	1,           // Number of types
	&sudMediaTypesCaptureSourceOut // Pin Media types
};

struct WStringHolder { wchar_t str[256]; };
__inline static WStringHolder GetCaptureSourceNameNum(const wchar_t* pCaptureSourceName, int i)
{
	WStringHolder res;
	StringCchPrintfW(res.str, sizeof(res.str)/sizeof(*res.str), (i == 0 ? L"%s" : L"%s #%d"), pCaptureSourceName, i + 1);
	return res;
}

__inline static GUID GetCLSIDUnityCaptureServiceNum(int i)
{
	GUID NumCLSID = CLSID_UnityCaptureService;
	if (i != 0) NumCLSID.Data4[7] += 1 + i;
	return NumCLSID;
}

extern "C" int CustomGetFactoryType(const IID &rClsID)
{
	if (IsEqualCLSID(rClsID, CLSID_UnityCaptureProperties)) return 1;
	if (memcmp(&rClsID, &CLSID_UnityCaptureService, sizeof(GUID) - 1)) return 0;
	unsigned char LastByteOffset = (rClsID.Data4[7] - CLSID_UnityCaptureService.Data4[7]);
	return 2 + (LastByteOffset == 0 ? 0 : LastByteOffset - 1);
}

CUnknown *CustomCreateInstance(int FactoryType, LPUNKNOWN pUnkOuter, HRESULT* hr)
{
	if (FactoryType == 1) return CCaptureProperties::CreateInstance(pUnkOuter, hr);
	if (FactoryType >= 2) return CCaptureSource::CreateInstance(pUnkOuter, hr, FactoryType - 2);
	return NULL;
}

// Stack Overflow - "Fake" DirectShow video capture device
// http://stackoverflow.com/questions/1376734/fake-directshow-video-capture-device
STDAPI AMovieSetupRegisterServer(CLSID clsServer, LPCWSTR szDescription, LPCWSTR szFileName, LPCWSTR szThreadingModel = L"Both", LPCWSTR szServerType = L"InprocServer32");
STDAPI AMovieSetupUnregisterServer(CLSID clsServer);
static HRESULT RegisterFilters(BOOL bRegister)
{
	UCASSERT(g_hInst != 0);
	WCHAR achFileName[MAX_PATH];
	if (!GetModuleFileNameW(g_hInst, achFileName, sizeof(achFileName))) return AmHresultFromWin32(GetLastError());
	HRESULT hr = CoInitialize(0);

	int MaxCapNum = (bRegister ? 1 : SharedImageMemory::MAX_CAPNUM);
	const wchar_t* pCaptureSourceName = CaptureSourceName;
	if (SUCCEEDED(hr) && bRegister)
	{
		char* CapNumParam = strstr(GetCommandLineA(), "/i:UnityCaptureDevices=");
		if (CapNumParam) MaxCapNum = atoi(CapNumParam + sizeof("/i:UnityCaptureDevices=") - 1);

		const wchar_t* CapNameParam = wcsstr(GetCommandLineW(), L"/i:UnityCaptureName=");
		if (CapNameParam)
		{
			//Parse custom filter names from /i:UnityCaptureName=NAME or "/i:UnityCaptureName=NAME NAME" or /i:UnityCaptureName="NAME NAME"
			const wchar_t* CapNameStart = CapNameParam + sizeof("/i:UnityCaptureName=") - 1;
			if (CapNameStart[0] == L'"') CapNameStart++;
			const wchar_t* CapNameEnd = wcsstr(CapNameStart, (CapNameParam[-1] == L'"' || CapNameStart[-1] == L'"' ? L"\"" : L" "));
			if (!CapNameEnd) CapNameEnd = CapNameStart + wcslen(CapNameStart);
			size_t CapNameLen = CapNameEnd - CapNameStart;
			if (CapNameLen > 0 && CapNameLen < 200)
			{
				//Allocate memory to hold the name string (this is never freed until regsvr32 ends, which is soon after this function anyway)
				wchar_t* CustomCaptureSourceName = (wchar_t*)malloc(sizeof(wchar_t) * (CapNameLen + 1));
				memcpy(CustomCaptureSourceName, CapNameStart, sizeof(wchar_t) * CapNameLen);
				CustomCaptureSourceName[CapNameLen] = L'\0';
				pCaptureSourceName = CustomCaptureSourceName;
			}
		}

		if (MaxCapNum < 1) MaxCapNum = 1;
		if (MaxCapNum > SharedImageMemory::MAX_CAPNUM) MaxCapNum = SharedImageMemory::MAX_CAPNUM;

		for (int i = 0; SUCCEEDED(hr) && i != MaxCapNum; i++)
			hr = AMovieSetupRegisterServer(GetCLSIDUnityCaptureServiceNum(i), GetCaptureSourceNameNum(pCaptureSourceName, i).str, achFileName, L"Both", L"InprocServer32");
		if (SUCCEEDED(hr)) hr = AMovieSetupRegisterServer(CLSID_UnityCaptureProperties, CaptureSourceName L" Configuration", achFileName, L"Both", L"InprocServer32");
		if (FAILED(hr)) MessageBoxA(0, "AMovieSetupRegisterServer failed", "RegisterFilters setup", NULL);
	}

	if (SUCCEEDED(hr))
	{
		IFilterMapper2 *fm = NULL;
		hr = CoCreateInstance(CLSID_FilterMapper2, NULL, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void **)&fm);

		if (SUCCEEDED(hr))
		{
			if (bRegister)
			{
				REGFILTER2 rf2;
				rf2.dwVersion = 1;
				rf2.dwMerit = MERIT_DO_NOT_USE;
				rf2.cPins = 1;
				rf2.rgPins = &sudCaptureSourceOut;
				for (int i = 0; SUCCEEDED(hr) && i != MaxCapNum; i++)
				{
					hr = fm->RegisterFilter(GetCLSIDUnityCaptureServiceNum(i), GetCaptureSourceNameNum(pCaptureSourceName, i).str, 0, &CLSID_VideoInputDeviceCategory, NULL, &rf2);

					//This is needed for Unity and Skype to access the virtual camera
					//Thanks to: https://social.msdn.microsoft.com/Forums/windowsdesktop/en-US/cd2b9d2d-b961-442d-8946-fdc038fed530/where-to-specify-device-id-in-the-filter?forum=windowsdirectshowdevelopment
					LPOLESTR CLSID_Category_Str, CLSID_Filter_Str; WCHAR strKey[256]; HKEY hKey;
					StringFromCLSID(CLSID_VideoInputDeviceCategory, &CLSID_Category_Str);
					StringFromCLSID(GetCLSIDUnityCaptureServiceNum(i), &CLSID_Filter_Str);
					StringCchPrintfW(strKey, 256, L"SOFTWARE\\Classes\\CLSID\\%s\\Instance\\%s", CLSID_Category_Str, CLSID_Filter_Str);
					RegOpenKeyExW(HKEY_LOCAL_MACHINE, strKey, 0, KEY_ALL_ACCESS, &hKey);
					RegSetValueExA(hKey, "DevicePath", 0, REG_SZ, (LPBYTE)"foo:bar", (DWORD)sizeof("foo:bar"));
					RegCloseKey(hKey);
				}
				if (FAILED(hr)) MessageBoxA(0, "Service RegisterFilter of IFilterMapper2 failed", "RegisterFilters setup", NULL);
			}
			else
			{
				for (int i = 0; SUCCEEDED(hr) && i != MaxCapNum; i++)
					hr = fm->UnregisterFilter(&CLSID_VideoInputDeviceCategory, 0, GetCLSIDUnityCaptureServiceNum(i));
				if (FAILED(hr)) MessageBoxA(0, "Service UnregisterFilter of IFilterMapper2 failed", "RegisterFilters setup", NULL);
			}
		}

		if (fm) fm->Release();
	}

	if (SUCCEEDED(hr) && !bRegister)
	{
		for (int i = 0; SUCCEEDED(hr) && i != MaxCapNum; i++)
			hr = AMovieSetupUnregisterServer(GetCLSIDUnityCaptureServiceNum(i));
		if (SUCCEEDED(hr)) hr = AMovieSetupUnregisterServer(CLSID_UnityCaptureProperties);
		if (FAILED(hr)) MessageBoxA(0, "AMovieSetupUnregisterServer failed", "RegisterFilters setup", NULL);
	}

	CoFreeUnusedLibraries();
	CoUninitialize();
	return hr;
}

STDAPI DllRegisterServer()
{
	return RegisterFilters(TRUE);
}

STDAPI DllInstall(BOOL bInstall, PCWSTR pszCmdLine)
{
	return S_OK;
}

STDAPI DllUnregisterServer()
{
	return RegisterFilters(FALSE);
}

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);
BOOL APIENTRY DllMain( HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	return DllEntryPoint((HINSTANCE)(hModule), ul_reason_for_call, lpReserved);
}
