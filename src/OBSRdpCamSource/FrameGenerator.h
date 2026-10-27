#pragma once

// Reads NV12 frames relayed from OBS' virtual camera through the global shared section.
// Outputs black frames when nothing (or nothing recent) is available.
class FrameGenerator
{
	UINT _width;
	UINT _height;
	HANDLE _mapping;
	const BYTE* _view;
	ULONGLONG _lastOpenTry;
	LONG _lastSeq;
	bool _hasFrame;
	std::vector<BYTE> _frame;

	void EnsureMapping();
	void ReadLatest();
	void FillBlack();

public:
	FrameGenerator() :
		_width(0),
		_height(0),
		_mapping(nullptr),
		_view(nullptr),
		_lastOpenTry(0),
		_lastSeq(-1),
		_hasFrame(false)
	{
	}

	~FrameGenerator()
	{
		Close();
	}

	static void ReadFormat(UINT& width, UINT& height, ULONGLONG& interval);

	HRESULT Initialize(UINT width, UINT height);
	HRESULT Generate(IMFSample* sample);
	void Close();
};
