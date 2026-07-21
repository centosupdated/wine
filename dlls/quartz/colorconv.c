/*
 * Color converter
 *
 * Copyright 2026 Brendan McGrath
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "quartz_private.h"

#include "mediaobj.h"
#include "mfapi.h"
#include "vfw.h"
#include "wmcodecdsp.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

struct color_converter
{
    struct strmbase_filter filter;

    struct strmbase_source source;
    struct strmbase_passthrough passthrough;

    struct strmbase_sink sink;

    IMediaObject *dmo;
    LONG sample_output_stride;
    LONG dmo_output_stride;
    ULONG dmo_output_sample_size;
};

struct buffer_for_sample
{
    IMediaBuffer IMediaBuffer_iface;
    LONG refcount;

    IMediaSample *sample;
};

struct buffer
{
    IMediaBuffer IMediaBuffer_iface;
    LONG refcount;

    BYTE *data;
    DWORD length;
    DWORD max_length;
};

static struct buffer *buffer_from_IMediaBuffer(IMediaBuffer *iface)
{
    return CONTAINING_RECORD(iface, struct buffer, IMediaBuffer_iface);
}

static HRESULT WINAPI buffer_QueryInterface(IMediaBuffer *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMediaBuffer))
    {
        *out = iface;
    }
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)(*out));
    return S_OK;
}

static ULONG WINAPI buffer_AddRef(IMediaBuffer *iface)
{
    struct buffer *buffer = buffer_from_IMediaBuffer(iface);
    ULONG refcount;

    refcount = InterlockedIncrement(&buffer->refcount);

    return refcount;
}

static ULONG WINAPI buffer_Release(IMediaBuffer *iface)
{
    struct buffer *buffer = buffer_from_IMediaBuffer(iface);
    ULONG refcount;

    refcount = InterlockedDecrement(&buffer->refcount);

    if (!refcount)
    {
        free(buffer->data);
        free(buffer);
    }

    return refcount;
}

static HRESULT WINAPI buffer_SetLength(IMediaBuffer *iface, DWORD length)
{
    struct buffer *buffer = buffer_from_IMediaBuffer(iface);

    buffer->length = length;

    return S_OK;
}

static HRESULT WINAPI buffer_GetMaxLength(IMediaBuffer *iface, DWORD *max_length)
{
    struct buffer *buffer = buffer_from_IMediaBuffer(iface);

    *max_length = buffer->max_length;

    return S_OK;
}

static HRESULT WINAPI buffer_GetBufferAndLength(IMediaBuffer *iface, BYTE **data, DWORD *length)
{
    struct buffer *buffer = buffer_from_IMediaBuffer(iface);

    *length = buffer->length;
    if (data)
        *data = buffer->data;

    return S_OK;
}

static IMediaBufferVtbl buffer_vtbl =
{
    buffer_QueryInterface,
    buffer_AddRef,
    buffer_Release,
    buffer_SetLength,
    buffer_GetMaxLength,
    buffer_GetBufferAndLength,
};

static struct buffer *create_buffer(DWORD max_length)
{
    struct buffer *buffer;

    buffer = calloc(1, sizeof(*buffer));
    buffer->IMediaBuffer_iface.lpVtbl = &buffer_vtbl;
    buffer->refcount = 1;

    buffer->data = malloc(max_length);
    buffer->max_length = max_length;

    return buffer;
}

struct subtype
{
    const GUID *guid;
    DWORD compression;
    WORD bitcount;
    ULONG cbFormat;
};

static const struct subtype subtypes[] =
{
    { &MEDIASUBTYPE_ARGB32, BI_RGB, 32, sizeof(VIDEOINFOHEADER) },
    { &MEDIASUBTYPE_RGB32, BI_RGB, 32, sizeof(VIDEOINFOHEADER) },
    { &MEDIASUBTYPE_RGB24, BI_RGB, 24, sizeof(VIDEOINFOHEADER) },
    { &MEDIASUBTYPE_RGB565, BI_BITFIELDS, 16, sizeof(VIDEOINFOHEADER) + sizeof(DWORD[3]) /* dwBitMasks */ },
    { &MEDIASUBTYPE_RGB555, BI_BITFIELDS, 16, sizeof(VIDEOINFOHEADER) + sizeof(DWORD[3]) /* dwBitMasks */ },
    { &MEDIASUBTYPE_RGB8, BI_RGB, 8, sizeof(VIDEOINFOHEADER) + sizeof(RGBQUAD[256]) /* bmiColors */ },
};

static const struct subtype *get_subtype(const AM_MEDIA_TYPE *mt)
{
    const struct subtype *subtype = NULL;
    VIDEOINFOHEADER *video_info;
    int i;

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Video) || !IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo))
        return NULL;

    for (i = 0; i < ARRAY_SIZE(subtypes); i++)
    {
        if (IsEqualGUID(&mt->subtype, subtypes[i].guid))
        {
            subtype = subtypes + i;
            break;
        }
    }

    if (!subtype || mt->cbFormat < subtype->cbFormat || !(video_info = (VIDEOINFOHEADER *)mt->pbFormat) ||
            video_info->bmiHeader.biSize != sizeof(video_info->bmiHeader))
        return NULL;

    return subtype;
}

static LONG calculate_stride(const BITMAPINFOHEADER *bmi_header)
{
    LONG stride = (bmi_header->biWidth * (bmi_header->biBitCount / 8) + 3) & ~3;
    if (bmi_header->biHeight >= 0)
        return stride;
    else
        return -stride;
}

static void populate_output_dmo_mt(
        const AM_MEDIA_TYPE *input_mt, const AM_MEDIA_TYPE *output_mt, DMO_MEDIA_TYPE *dmo_mt)
{
    const VIDEOINFOHEADER *input_video_info;
    VIDEOINFOHEADER *video_info;

    input_video_info = (VIDEOINFOHEADER *)input_mt->pbFormat;

    video_info = calloc(1, sizeof(*video_info));
    memcpy(video_info, output_mt->pbFormat, sizeof(*video_info));
    video_info->bmiHeader.biWidth = input_video_info->bmiHeader.biWidth;
    video_info->bmiHeader.biHeight = input_video_info->bmiHeader.biHeight;
    video_info->bmiHeader.biSizeImage = abs(calculate_stride(&video_info->bmiHeader)) * video_info->bmiHeader.biHeight;

    memcpy(dmo_mt, output_mt, sizeof(*dmo_mt));
    dmo_mt->pbFormat = (BYTE *)video_info;
    dmo_mt->lSampleSize = video_info->bmiHeader.biSizeImage;
}

static struct buffer_for_sample *buffer_for_sample_from_IMediaBuffer(IMediaBuffer *iface)
{
    return CONTAINING_RECORD(iface, struct buffer_for_sample, IMediaBuffer_iface);
}

static HRESULT WINAPI buffer_for_sample_QueryInterface(IMediaBuffer *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IMediaBuffer))
    {
        *out = iface;
    }
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)(*out));
    return S_OK;
}

static ULONG WINAPI buffer_for_sample_AddRef(IMediaBuffer *iface)
{
    struct buffer_for_sample *buffer = buffer_for_sample_from_IMediaBuffer(iface);
    ULONG refcount;

    refcount = InterlockedIncrement(&buffer->refcount);

    return refcount;
}

static ULONG WINAPI buffer_for_sample_Release(IMediaBuffer *iface)
{
    struct buffer_for_sample *buffer = buffer_for_sample_from_IMediaBuffer(iface);
    ULONG refcount;

    refcount = InterlockedDecrement(&buffer->refcount);

    if (!refcount)
    {
        IMediaSample_Release(buffer->sample);
        free(buffer);
    }

    return refcount;
}

static HRESULT WINAPI buffer_for_sample_SetLength(IMediaBuffer *iface, DWORD len)
{
    struct buffer_for_sample *buffer = buffer_for_sample_from_IMediaBuffer(iface);

    TRACE("iface %p, len %lu.\n", iface, len);

    return IMediaSample_SetActualDataLength(buffer->sample, len);
}

static HRESULT WINAPI buffer_for_sample_GetMaxLength(IMediaBuffer *iface, DWORD *len)
{
    struct buffer_for_sample *buffer = buffer_for_sample_from_IMediaBuffer(iface);

    TRACE("iface %p, len %p.\n", iface, len);

    *len = IMediaSample_GetSize(buffer->sample);
    return S_OK;
}

static HRESULT WINAPI buffer_for_sample_GetBufferAndLength(IMediaBuffer *iface, BYTE **data, DWORD *len)
{
    struct buffer_for_sample *buffer = buffer_for_sample_from_IMediaBuffer(iface);

    TRACE("iface %p, data %p, len %p.\n", iface, data, len);

    *len = IMediaSample_GetActualDataLength(buffer->sample);
    if (data)
        return IMediaSample_GetPointer(buffer->sample, data);
    return S_OK;
}

static const IMediaBufferVtbl buffer_for_sample_vtbl =
{
    buffer_for_sample_QueryInterface,
    buffer_for_sample_AddRef,
    buffer_for_sample_Release,
    buffer_for_sample_SetLength,
    buffer_for_sample_GetMaxLength,
    buffer_for_sample_GetBufferAndLength,
};

static struct buffer_for_sample *create_buffer_for_sample(IMediaSample *sample)
{
    struct buffer_for_sample *buffer;

    buffer = calloc(1, sizeof(*buffer));
    buffer->IMediaBuffer_iface.lpVtbl = &buffer_for_sample_vtbl;
    buffer->refcount = 1;

    IMediaSample_AddRef(buffer->sample = sample);

    return buffer;
}

static struct color_converter *impl_from_strmbase_filter(struct strmbase_filter *iface)
{
    return CONTAINING_RECORD(iface, struct color_converter, filter);
}

static HRESULT color_sink_query_interface(struct strmbase_pin *iface, REFIID iid, void **out)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface->filter);

    if (IsEqualGUID(iid, &IID_IMemInputPin))
        *out = &filter->sink.IMemInputPin_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT color_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    if (get_subtype(mt))
        return S_OK;
    else
        return S_FALSE;
}

static HRESULT color_sink_connect(struct strmbase_sink *iface, IPin *peer, const AM_MEDIA_TYPE *mt)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface->pin.filter);
    HRESULT hr = VFW_E_TYPE_NOT_ACCEPTED;

    if (get_subtype(mt))
        hr = IMediaObject_SetInputType(filter->dmo, 0, mt, 0);

    TRACE("Returning %#lx.\n", hr);

    return hr;
}

static HRESULT color_sink_end_flush(struct strmbase_sink *iface)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface->pin.filter);
    if (filter->source.pin.peer)
        return IPin_EndFlush(filter->source.pin.peer);
    return S_OK;
}

static HRESULT WINAPI color_source_DecideBufferSize(
        struct strmbase_source *iface, IMemAllocator *alloc, ALLOCATOR_PROPERTIES *props)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface->pin.filter);
    const struct subtype *subtype;
    ALLOCATOR_PROPERTIES actual;
    BITMAPINFOHEADER *header;
    DMO_MEDIA_TYPE dmo_mt;
    long min_image_size;
    HRESULT hr;

    populate_output_dmo_mt(&filter->sink.pin.mt, &iface->pin.mt, &dmo_mt);
    if (FAILED(hr = IMediaObject_SetOutputType(filter->dmo, 0, &dmo_mt, 0)))
        return hr;

    filter->sample_output_stride = calculate_stride(&((VIDEOINFOHEADER *)iface->pin.mt.pbFormat)->bmiHeader);
    filter->dmo_output_stride = calculate_stride(&((VIDEOINFOHEADER *)dmo_mt.pbFormat)->bmiHeader);
    filter->dmo_output_sample_size = dmo_mt.lSampleSize;

    if (!props->cbAlign)
        props->cbAlign = 1;

    if (!props->cBuffers)
        props->cBuffers = 1;

    subtype = get_subtype(&iface->pin.mt);

    header = &((VIDEOINFOHEADER *)iface->pin.mt.pbFormat)->bmiHeader;
    min_image_size = header->biWidth * header->biHeight * (subtype->bitcount / 8);

    if (props->cbBuffer < min_image_size)
        props->cbBuffer = min_image_size;

    return IMemAllocator_SetProperties(alloc, props, &actual);
}

static HRESULT WINAPI color_sink_Receive(struct strmbase_sink *iface, IMediaSample *src_sample)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface->pin.filter);
    BITMAPINFOHEADER *input_bmi_header, *output_bmi_header;
    struct buffer_for_sample *src_buffer;
    BYTE *dst_buff, *src_buff, *dest;
    DMO_OUTPUT_DATA_BUFFER output;
    const struct subtype *subtype;
    struct buffer *dst_buffer;
    BITMAPINFOHEADER *header;
    IMediaSample *dst_sample;
    DWORD flags = 0, status;
    long output_image_size;
    LONGLONG start, stop;
    AM_MEDIA_TYPE *mt;
    LONG dst_size;
    UINT32 *data;
    HRESULT hr;
    int i;

    /* We do not expect pin connection state to change while the filter is
     * running. This guarantee is necessary, since otherwise we would have to
     * take the filter lock, and we can't take the filter lock from a streaming
     * thread. */
    if (!filter->source.pMemInputPin)
    {
        WARN("Source is not connected, returning VFW_E_NOT_CONNECTED.\n");
        return VFW_E_NOT_CONNECTED;
    }

    if (filter->filter.state == State_Stopped)
        return VFW_E_WRONG_STATE;

    if (filter->sink.flushing)
        return S_FALSE;

    hr = IMediaSample_GetPointer(src_sample, &src_buff);
    if (FAILED(hr))
    {
        ERR("Failed to get input buffer pointer, hr %#lx.\n", hr);
        return hr;
    }

    if (FAILED(hr = IMemAllocator_GetBuffer(filter->source.pAllocator, &dst_sample, NULL, NULL, 0)))
    {
        ERR("Failed to get sample, hr %#lx.\n", hr);
        return hr;
    }

    /* Handle dynamic format change. */
    if ((hr = IMediaSample_GetMediaType(dst_sample, &mt)) == S_OK)
    {
        if (memcmp(mt, &filter->source.pin.mt, offsetof(AM_MEDIA_TYPE, pbFormat)) ||
                memcmp(mt->pbFormat, filter->source.pin.mt.pbFormat, mt->cbFormat))
        {
            DMO_MEDIA_TYPE dmo_mt;

            populate_output_dmo_mt(&filter->sink.pin.mt, mt, &dmo_mt);
            if (FAILED(hr = IMediaObject_SetOutputType(filter->dmo, 0, &dmo_mt, 0)))
                WARN("Failed to update media type, hr %#lx.\n", hr);

            filter->sample_output_stride = calculate_stride(&((VIDEOINFOHEADER *)mt->pbFormat)->bmiHeader);
            filter->dmo_output_stride = calculate_stride(&((VIDEOINFOHEADER *)dmo_mt.pbFormat)->bmiHeader);
            filter->dmo_output_sample_size = dmo_mt.lSampleSize;

            FreeMediaType(&filter->source.pin.mt);
            filter->source.pin.mt = *mt;
            CoTaskMemFree(mt);
        }
    }
    else if (hr != S_FALSE)
    {
        ERR("Failed to get media type, hr %#lx.\n", hr);
    }

    hr = IMediaSample_GetPointer(dst_sample, &dst_buff);
    if (FAILED(hr))
    {
        ERR("Failed to get output buffer pointer, hr %#lx.\n", hr);
        IMediaSample_Release(dst_sample);
        return hr;
    }

    subtype = get_subtype(&filter->source.pin.mt);
    header = &((VIDEOINFOHEADER *)filter->sink.pin.mt.pbFormat)->bmiHeader;
    output_image_size = header->biWidth * header->biHeight * (subtype->bitcount / 8);
    dst_size = IMediaSample_GetSize(dst_sample);
    if (dst_size < output_image_size)
    {
        ERR("Sample size is too small (%ld < %lu).\n", dst_size, output_image_size);
        IMediaSample_Release(dst_sample);
        return E_FAIL;
    }

    if (IMediaSample_IsPreroll(src_sample) == S_OK)
        flags |= ICDECOMPRESS_PREROLL;
    hr = IMediaSample_GetTime(src_sample, &start, &stop);

    /* perform color conversion */
    src_buffer = create_buffer_for_sample(src_sample);
    hr = IMediaObject_ProcessInput(filter->dmo, 0, &src_buffer->IMediaBuffer_iface,
            hr == S_OK ? DMO_INPUT_DATA_BUFFERF_TIME & DMO_INPUT_DATA_BUFFERF_TIMELENGTH : 0, start, stop - start);
    IMediaBuffer_Release(&src_buffer->IMediaBuffer_iface);

    input_bmi_header = &((VIDEOINFOHEADER *)filter->sink.pin.mt.pbFormat)->bmiHeader;
    output_bmi_header = &((VIDEOINFOHEADER *)filter->source.pin.mt.pbFormat)->bmiHeader;
    dst_buffer = create_buffer(filter->dmo_output_sample_size);
    memset(&output, 0, sizeof(output));
    output.pBuffer = &dst_buffer->IMediaBuffer_iface;
    hr = IMediaObject_ProcessOutput(filter->dmo, 0, 1, &output, &status);
    if (input_bmi_header->biBitCount < output_bmi_header->biBitCount && output_bmi_header->biBitCount == 32)
    {
        /* Fix the value of the alpha channel. DMO uses 0xff, whilst quartz uses 0x00. */
        data = (UINT32 *)dst_buffer->data;

        for (i = 0; i < input_bmi_header->biHeight * input_bmi_header->biWidth; i++)
            *data++ &= 0xffffff;
    }
    IMediaSample_GetPointer(dst_sample, &dest);
    if (filter->sample_output_stride < 0)
        dest += -filter->sample_output_stride * (input_bmi_header->biHeight - 1);

    MFCopyImage(dest, filter->sample_output_stride, dst_buffer->data, filter->dmo_output_stride,
            input_bmi_header->biWidth * (output_bmi_header->biBitCount / 8), input_bmi_header->biHeight);
    IMediaBuffer_Release(output.pBuffer);

    /* Drop sample if it's intended to be dropped */
    if (flags & ICDECOMPRESS_HURRYUP)
    {
        IMediaSample_Release(dst_sample);
        return S_OK;
    }

    IMediaSample_SetActualDataLength(dst_sample, output_image_size);

    IMediaSample_SetPreroll(dst_sample, (IMediaSample_IsPreroll(src_sample) == S_OK));
    IMediaSample_SetDiscontinuity(dst_sample, (IMediaSample_IsDiscontinuity(src_sample) == S_OK));
    IMediaSample_SetSyncPoint(dst_sample, TRUE);

    if (hr == S_OK)
        IMediaSample_SetTime(dst_sample, &start, &stop);
    else if (hr == VFW_S_NO_STOP_TIME)
        IMediaSample_SetTime(dst_sample, &start, NULL);
    else
        IMediaSample_SetTime(dst_sample, NULL, NULL);

    hr = IMemInputPin_Receive(filter->source.pMemInputPin, dst_sample);
    if (hr != S_OK && hr != VFW_E_NOT_CONNECTED)
        ERR("Failed to send sample, hr %#lx.\n", hr);

    IMediaSample_Release(dst_sample);
    return hr;
}

HRESULT color_sink_can_block(struct strmbase_sink *iface)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface->pin.filter);

    if (!filter->source.pMemInputPin)
        return VFW_E_NOT_CONNECTED;

    return IMemInputPin_ReceiveCanBlock(filter->source.pMemInputPin);
}

static const struct strmbase_sink_ops sink_ops =
{
    .base.pin_query_interface = color_sink_query_interface,
    .base.pin_query_accept = color_sink_query_accept,
    .pfnReceive = color_sink_Receive,
    .sink_connect = color_sink_connect,
    .sink_end_flush = color_sink_end_flush,
    .sink_receive_can_block = color_sink_can_block,
};

static HRESULT color_source_query_interface(struct strmbase_pin *iface, REFIID iid, void **out)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface->filter);

    if (IsEqualGUID(iid, &IID_IMediaSeeking))
        *out = &filter->passthrough.IMediaSeeking_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT color_source_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface->filter);

    if (!filter->sink.pin.peer)
        return S_FALSE;

    return get_subtype(mt) ? S_OK : S_FALSE;
}

static HRESULT color_source_get_media_type(struct strmbase_pin *iface, unsigned int index, AM_MEDIA_TYPE *mt)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface->filter);
    const VIDEOINFOHEADER *sink_format;
    const struct subtype *subtype;
    VIDEOINFO *format;

    if (!filter->sink.pin.peer || index >= ARRAY_SIZE(subtypes))
        return VFW_S_NO_MORE_ITEMS;

    subtype = subtypes + index;
    sink_format = (VIDEOINFOHEADER *)filter->sink.pin.mt.pbFormat;

    memset(mt, 0, sizeof(AM_MEDIA_TYPE));

    if (!(format = CoTaskMemAlloc(mt->cbFormat = subtype->cbFormat)))
        return E_OUTOFMEMORY;

    memset(format, 0, mt->cbFormat);

    format->rcSource = sink_format->rcSource;
    format->rcTarget = sink_format->rcTarget;
    format->dwBitRate = sink_format->dwBitRate;
    format->dwBitErrorRate = sink_format->dwBitErrorRate;
    format->AvgTimePerFrame = sink_format->AvgTimePerFrame;

    format->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    format->bmiHeader.biWidth = sink_format->bmiHeader.biWidth;
    format->bmiHeader.biHeight = sink_format->bmiHeader.biHeight;
    format->bmiHeader.biPlanes = sink_format->bmiHeader.biPlanes;
    format->bmiHeader.biBitCount = subtype->bitcount;
    format->bmiHeader.biCompression = subtype->compression;
    format->bmiHeader.biSizeImage = format->bmiHeader.biHeight * format->bmiHeader.biWidth * (subtype->bitcount / 8);

    if (IsEqualGUID(subtype->guid, &MEDIASUBTYPE_RGB565))
    {
        format->dwBitMasks[iRED] = 0xf800;
        format->dwBitMasks[iGREEN] = 0x07e0;
        format->dwBitMasks[iBLUE] = 0x001f;
    }

    mt->majortype = MEDIATYPE_Video;
    mt->subtype = *subtype->guid;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = format->bmiHeader.biSizeImage;
    mt->formattype = FORMAT_VideoInfo;
    mt->pbFormat = (BYTE *)format;

    return S_OK;
}

static const struct strmbase_source_ops source_ops =
{
    .base.pin_query_interface = color_source_query_interface,
    .base.pin_query_accept = color_source_query_accept,
    .base.pin_get_media_type = color_source_get_media_type,
    .pfnAttemptConnection = BaseOutputPinImpl_AttemptConnection,
    .pfnDecideAllocator = BaseOutputPinImpl_DecideAllocator,
    .pfnDecideBufferSize = color_source_DecideBufferSize,
};

static struct strmbase_pin *color_get_pin(struct strmbase_filter *iface, unsigned int index)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface);

    if (index == 0)
        return &filter->sink.pin;
    else if (index == 1)
        return &filter->source.pin;
    return NULL;
}

static void color_destroy(struct strmbase_filter *iface)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface);

    if (filter->sink.pin.peer)
        IPin_Disconnect(filter->sink.pin.peer);
    IPin_Disconnect(&filter->sink.pin.IPin_iface);

    if (filter->source.pin.peer)
        IPin_Disconnect(filter->source.pin.peer);
    IPin_Disconnect(&filter->source.pin.IPin_iface);

    strmbase_sink_cleanup(&filter->sink);
    strmbase_source_cleanup(&filter->source);
    strmbase_passthrough_cleanup(&filter->passthrough);
    strmbase_filter_cleanup(&filter->filter);

    IMediaObject_Release(filter->dmo);

    free(filter);
}

static HRESULT color_init_stream(struct strmbase_filter *iface)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface);
    HRESULT hr;

    if (!filter->source.pin.peer)
        return S_OK;

    if (FAILED(hr = IMemAllocator_Commit(filter->source.pAllocator)))
        ERR("Failed to commit allocator, hr %#lx.\n", hr);

    return S_OK;
}

static HRESULT color_cleanup_stream(struct strmbase_filter *iface)
{
    struct color_converter *filter = impl_from_strmbase_filter(iface);

    if (!filter->source.pin.peer)
        return S_OK;

    IMemAllocator_Decommit(filter->source.pAllocator);

    return S_OK;
}

static const struct strmbase_filter_ops filter_ops =
{
    .filter_get_pin = color_get_pin,
    .filter_destroy = color_destroy,
    .filter_init_stream = color_init_stream,
    .filter_cleanup_stream = color_cleanup_stream,
};

HRESULT color_create(IUnknown *outer, IUnknown **out)
{
    struct color_converter *object;
    IMemAllocator *allocator;
    HRESULT hr;

    if (FAILED(hr = CoCreateInstance(
                       &CLSID_MemoryAllocator, NULL, CLSCTX_INPROC_SERVER, &IID_IMemAllocator, (void **)&allocator)))
        return hr;

    if (!(object = calloc(1, sizeof(*object))))
    {
        IMemAllocator_Release(allocator);
        return E_OUTOFMEMORY;
    }

    strmbase_filter_init(&object->filter, outer, &CLSID_Colour, &filter_ops);

    strmbase_sink_init(&object->sink, &object->filter, L"In", &sink_ops, allocator);
    wcscpy(object->sink.pin.name, L"Input");

    strmbase_source_init(&object->source, &object->filter, L"Out", &source_ops);
    wcscpy(object->source.pin.name, L"XForm Out");

    strmbase_passthrough_init(&object->passthrough, (IUnknown *)&object->source.pin.IPin_iface);
    ISeekingPassThru_Init(&object->passthrough.ISeekingPassThru_iface, FALSE, &object->sink.pin.IPin_iface);

    if (SUCCEEDED(hr = CoCreateInstance(&CLSID_CColorConvertDMO, NULL, CLSCTX_INPROC_SERVER, &IID_IMediaObject,
                          (void **)&object->dmo)))
    {
        TRACE("Created Color Converter %p.\n", object);
        *out = &object->filter.IUnknown_inner;
    }
    else
    {
        ERR("Failed to create color conversion transform %#lx.\n", hr);
    }

    return hr;
}
