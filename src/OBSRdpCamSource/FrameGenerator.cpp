#include "pch.h"
#include "Tools.h"
#include "FrameGenerator.h"

void FrameGenerator::ReadFormat(UINT& width, UINT& height, ULONGLONG& interval)
{
	width = OBSRDP_DEFAULT_WIDTH;
	height = OBSRDP_DEFAULT_HEIGHT;
	interval = OBSRDP_DEFAULT_INTERVAL;

	wil::unique_hfile file(CreateFileW(OBSRDP_FORMAT_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
	if (!file)
	{
		WINTRACE(L"FrameGenerator::ReadFormat no format file, using defaults");
		return;
	}

	char text[64]{};
	DWORD read = 0;
	if (!ReadFile(file.get(), text, sizeof(text) - 1, &read, nullptr))
		return;

	unsigned int w = 0, h = 0;
	unsigned long long i = 0;
	if (sscanf_s(text, "%ux%ux%llu", &w, &h, &i) != 3)
		return;

	w &= ~1u;
	h &= ~1u;
	if (w < 2 || h < 2 || w > OBSRDP_MAX_WIDTH || h > OBSRDP_MAX_HEIGHT || i < 10000 || i > 10000000)
		return;

	width = w;
	height = h;
	interval = i;
	WINTRACE(L"FrameGenerator::ReadFormat %ux%u interval:%llu", width, height, interval);
}

HRESULT FrameGenerator::Initialize(UINT width, UINT height)
{
	_width = width;
	_height = height;
	_frame.resize((size_t)width * height * 3 / 2);
	_lastSeq = -1;
	FillBlack();
	EnsureMapping();
	return S_OK;
}

void FrameGenerator::Close()
{
	if (_view)
	{
		UnmapViewOfFile(_view);
		_view = nullptr;
	}

	if (_mapping)
	{
		CloseHandle(_mapping);
		_mapping = nullptr;
	}
}

void FrameGenerator::FillBlack()
{
	auto lumaSize = (size_t)_width * _height;
	memset(_frame.data(), 16, lumaSize);
	memset(_frame.data() + lumaSize, 128, lumaSize / 2);
	_hasFrame = false;
}

void FrameGenerator::EnsureMapping()
{
	if (_view)
		return;

	auto now = GetTickCount64();
	if (_lastOpenTry && now - _lastOpenTry < 1000)
		return;

	_lastOpenTry = now;

	// the Frame Server service can create global objects, other processes loading us can only open it
	wil::unique_hlocal_security_descriptor sd;
	SECURITY_ATTRIBUTES sa{ sizeof(sa) };
	if (ConvertStringSecurityDescriptorToSecurityDescriptorW(OBSRDP_SECTION_SDDL, SDDL_REVISION_1, &sd, nullptr))
	{
		sa.lpSecurityDescriptor = sd.get();
	}

	_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, sa.lpSecurityDescriptor ? &sa : nullptr, PAGE_READWRITE, 0, OBSRDP_SECTION_SIZE, OBSRDP_SECTION_NAME);
	if (!_mapping)
	{
		_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, OBSRDP_SECTION_NAME);
	}

	if (!_mapping)
	{
		WINTRACE(L"FrameGenerator::EnsureMapping cannot create or open section: %u", GetLastError());
		return;
	}

	_view = (const BYTE*)MapViewOfFile(_mapping, FILE_MAP_READ, 0, 0, 0);
	if (!_view)
	{
		WINTRACE(L"FrameGenerator::EnsureMapping cannot map section: %u", GetLastError());
		CloseHandle(_mapping);
		_mapping = nullptr;
		return;
	}

	WINTRACE(L"FrameGenerator::EnsureMapping section mapped");
}

void FrameGenerator::ReadLatest()
{
	EnsureMapping();
	if (!_view)
		return;

	auto header = (const ObsRdpHeader*)_view;
	auto stale = header->magic != OBSRDP_MAGIC || GetTickCount64() - (ULONGLONG)ReadAcquire64(&header->lastWrite) > OBSRDP_STALE_MS;
	if (stale)
	{
		if (_hasFrame)
		{
			FillBlack();
		}
		return;
	}

	auto seq = ReadAcquire(&header->seq);
	if ((seq & 1) || seq == _lastSeq)
		return;

	auto srcWidth = header->width;
	auto srcHeight = header->height;
	if (srcWidth < 2 || srcHeight < 2 || srcWidth > OBSRDP_MAX_WIDTH || srcHeight > OBSRDP_MAX_HEIGHT)
		return;

	auto src = _view + OBSRDP_DATA_OFFSET;
	auto dst = _frame.data();
	if (srcWidth == _width && srcHeight == _height)
	{
		memcpy(dst, src, _frame.size());
	}
	else
	{
		// nearest neighbor scaling, for when OBS' resolution differs from the advertised one
		for (UINT y = 0; y < _height; y++)
		{
			auto srcRow = src + (size_t)(y * srcHeight / _height) * srcWidth;
			auto dstRow = dst + (size_t)y * _width;
			for (UINT x = 0; x < _width; x++)
			{
				dstRow[x] = srcRow[x * srcWidth / _width];
			}
		}

		auto srcUV = src + (size_t)srcWidth * srcHeight;
		auto dstUV = dst + (size_t)_width * _height;
		for (UINT y = 0; y < _height / 2; y++)
		{
			auto srcRow = (const uint16_t*)(srcUV + (size_t)(y * srcHeight / _height) * srcWidth);
			auto dstRow = (uint16_t*)(dstUV + (size_t)y * _width);
			for (UINT x = 0; x < _width / 2; x++)
			{
				dstRow[x] = srcRow[x * srcWidth / _width];
			}
		}
	}

	// frame was overwritten while copying: keep it anyway, next one will fix it
	_lastSeq = ReadAcquire(&header->seq) == seq ? seq : -1;
	_hasFrame = true;
}

HRESULT FrameGenerator::Generate(IMFSample* sample)
{
	RETURN_HR_IF_NULL(E_POINTER, sample);
	ReadLatest();

	wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
	RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));

	auto lumaSize = (size_t)_width * _height;
	auto buffer2d = buffer.try_query<IMF2DBuffer2>();
	if (buffer2d)
	{
		BYTE* scanline;
		LONG pitch;
		BYTE* start;
		DWORD length;
		RETURN_IF_FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &pitch, &start, &length));
		auto src = _frame.data();
		for (UINT y = 0; y < _height; y++)
		{
			memcpy(scanline + (size_t)y * pitch, src + (size_t)y * _width, _width);
		}

		auto dstUV = scanline + (size_t)_height * pitch;
		auto srcUV = src + lumaSize;
		for (UINT y = 0; y < _height / 2; y++)
		{
			memcpy(dstUV + (size_t)y * pitch, srcUV + (size_t)y * _width, _width);
		}
		RETURN_IF_FAILED(buffer2d->Unlock2D());
		return S_OK;
	}

	BYTE* data;
	DWORD maxLength;
	RETURN_IF_FAILED(buffer->Lock(&data, &maxLength, nullptr));
	auto size = (DWORD)min((size_t)maxLength, _frame.size());
	memcpy(data, _frame.data(), size);
	RETURN_IF_FAILED(buffer->Unlock());
	RETURN_IF_FAILED(buffer->SetCurrentLength(size));
	return S_OK;
}
