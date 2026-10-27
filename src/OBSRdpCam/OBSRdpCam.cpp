#include "framework.h"
#include "tools.h"
#include "OBSRdpCam.h"

#define MAX_LOADSTRING 100

static GUID CLSID_VCam = OBSRDP_CLSID_INIT;

HINSTANCE _instance;
WCHAR _title[MAX_LOADSTRING];
WCHAR _windowClass[MAX_LOADSTRING];
wil::com_ptr_nothrow<IMFVirtualCamera> _vcam;
DWORD _vcamCookie;
wil::unique_event _relayStop;

ATOM MyRegisterClass(HINSTANCE hInstance);
HWND InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);
HRESULT RegisterVirtualCamera();
HRESULT UnregisterVirtualCamera();
void WriteFormatFile();
DWORD WINAPI RelayThread(LPVOID);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

	// one camera and one relay per session
	wil::unique_mutex_nothrow instance(CreateMutexW(nullptr, TRUE, L"Local\\OBSRdpCam.SingleInstance"));
	if (!instance || GetLastError() == ERROR_ALREADY_EXISTS)
		return 0;

	// set tracing & CRT leak tracking
	WinTraceRegister();
	WINTRACE(L"WinMain starting '%s'", GetCommandLineW());
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);

	wil::SetResultLoggingCallback([](wil::FailureInfo const& failure) noexcept
		{
			wchar_t str[2048];
			if (SUCCEEDED(wil::GetFailureLogString(str, _countof(str), failure)))
			{
				WinTrace(2, 0, str); // 2 => error
#ifndef _DEBUG
				TaskDialog(nullptr, nullptr, _title, L"A fatal error has occured. Press OK to terminate.", str, TDCBF_OK_BUTTON, TD_WARNING_ICON, nullptr);
#endif
			}
		});

	LoadStringW(hInstance, IDS_APP_TITLE, _title, MAX_LOADSTRING);
	LoadStringW(hInstance, IDC_OBSRDPCAM, _windowClass, MAX_LOADSTRING);
	wcscpy_s(_title, L"OBS RDP Camera"); // also the camera's friendly name
	MyRegisterClass(hInstance);
	auto hwnd = InitInstance(hInstance, nCmdShow);
	if (hwnd)
	{
		winrt::init_apartment();
		if (SUCCEEDED(MFStartup(MF_VERSION)))
		{
			TASKDIALOGCONFIG config{};
			config.cbSize = sizeof(TASKDIALOGCONFIG);
			config.hInstance = hInstance;
			config.hwndParent = hwnd;
			config.pszWindowTitle = _title;
			config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
			// the camera's format is read when it's created, so write it first
			WriteFormatFile();
			_relayStop.create(wil::EventOptions::ManualReset);
			wil::unique_handle relay(CreateThread(nullptr, 0, RelayThread, nullptr, 0, nullptr));

			auto hr = RegisterVirtualCamera();
			if (SUCCEEDED(hr))
			{
				config.pszMainInstruction = L"\"OBS RDP Camera\" is running.\nClosing this window stops the camera.";
				config.pszContent = L"Start the virtual camera in OBS: its frames are relayed to this camera (black until OBS sends something). Restart this program after changing OBS' output resolution or frame rate.";
				config.pszMainIcon = TD_INFORMATION_ICON;
				TaskDialogIndirect(&config, nullptr, nullptr, nullptr);

				//auto accelerators = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_OBSRDPCAM));
				//MSG msg;
				//while (GetMessage(&msg, nullptr, 0, 0))
				//{
				//	if (!TranslateAccelerator(msg.hwnd, accelerators, &msg))
				//	{
				//		TranslateMessage(&msg);
				//		DispatchMessage(&msg);
				//	}
				//}

				UnregisterVirtualCamera();
			}
			else
			{
				config.pszMainInstruction = L"The camera could not be started. Make sure OBSRdpCamSource.dll is registered (run install.ps1 as administrator).";
				wchar_t text[1024];
				wchar_t errorText[256];
				FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, 0, errorText, _countof(errorText), nullptr);
				wsprintf(text, L"Error 0x%08X (%u): %s", hr, hr, errorText);
				config.pszContent = text;

				config.pszMainIcon = TD_ERROR_ICON;
				TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
			}

			_relayStop.SetEvent();
			if (relay)
			{
				WaitForSingleObject(relay.get(), 5000);
			}

			_vcam.reset();
			MFShutdown();
		}
	}

	// cleanup & CRT leak checks
	_CrtDumpMemoryLeaks();
	WINTRACE(L"WinMain exiting '%s'", GetCommandLineW());
	WinTraceUnregister();
	return 0;
}

HRESULT RegisterVirtualCamera()
{
	auto clsid = GUID_ToStringW(CLSID_VCam);
	RETURN_IF_FAILED_MSG(MFCreateVirtualCamera(
		MFVirtualCameraType_SoftwareCameraSource,
		MFVirtualCameraLifetime_Session,
		MFVirtualCameraAccess_CurrentUser,
		_title,
		clsid.c_str(),
		nullptr,
		0,
		&_vcam),
		"Failed to create virtual camera");

	WINTRACE(L"RegisterVirtualCamera '%s' ok", clsid.c_str());
	RETURN_IF_FAILED_MSG(_vcam->Start(nullptr), "Cannot start VCam");
	WINTRACE(L"VCam was started");
	return S_OK;
}

HRESULT UnregisterVirtualCamera()
{
	if (!_vcam)
		return S_OK;

	// NOTE: we don't call Shutdown or this will cause 2 Shutdown calls to the media source and will prevent proper removing
	//auto hr = _vcam->Shutdown();
	//WINTRACE(L"Shutdown VCam hr:0x%08X", hr);

	auto hr = _vcam->Remove();
	WINTRACE(L"Remove VCam hr:0x%08X", hr);
	return S_OK;
}

// tells the media source which format to expose: OBS' running virtual camera, else OBS' last one, else defaults
void WriteFormatFile()
{
	uint32_t width = OBSRDP_DEFAULT_WIDTH;
	uint32_t height = OBSRDP_DEFAULT_HEIGHT;
	uint64_t interval = OBSRDP_DEFAULT_INTERVAL;
	auto found = false;

	wil::unique_handle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, OBS_QUEUE_NAME));
	if (mapping)
	{
		auto header = (const ObsQueueHeader*)MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, sizeof(ObsQueueHeader));
		if (header)
		{
			if (header->cx && header->cy && header->interval)
			{
				width = header->cx;
				height = header->cy;
				interval = header->interval;
				found = true;
			}
			UnmapViewOfFile(header);
		}
	}

	if (!found)
	{
		wchar_t path[MAX_PATH];
		if (ExpandEnvironmentStringsW(L"%APPDATA%\\obs-studio\\obs-virtualcam.txt", path, _countof(path)))
		{
			wil::unique_hfile file(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
			char text[64]{};
			DWORD read = 0;
			unsigned int w, h;
			unsigned long long i;
			if (file && ReadFile(file.get(), text, sizeof(text) - 1, &read, nullptr) && sscanf_s(text, "%ux%ux%llu", &w, &h, &i) == 3)
			{
				width = w;
				height = h;
				interval = i;
			}
		}
	}

	CreateDirectoryW(OBSRDP_FORMAT_DIR, nullptr);
	wil::unique_hfile file(CreateFileW(OBSRDP_FORMAT_FILE, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr));
	if (!file)
	{
		WINTRACE(L"WriteFormatFile cannot write format file: %u", GetLastError());
		return;
	}

	char text[64];
	auto len = sprintf_s(text, "%ux%ux%llu", width, height, (unsigned long long)interval);
	DWORD written;
	WriteFile(file.get(), text, len, &written, nullptr);
	WINTRACE(L"WriteFormatFile %S", text);
}

// copies OBS' virtual camera frames (session-local shared memory) to the global section read by the media source
DWORD WINAPI RelayThread(LPVOID)
{
	timeBeginPeriod(1);

	HANDLE obsMapping = nullptr;
	const BYTE* obs = nullptr;
	SIZE_T obsSize = 0;
	uint32_t obsLastIdx = 0;
	ULONGLONG obsLastChange = 0;

	HANDLE rdpMapping = nullptr;
	BYTE* rdp = nullptr;
	ULONGLONG lastOpenTry = 0;

	auto closeObs = [&]()
		{
			// must be done quickly, OBS refuses to restart its virtual camera while its section is still open
			if (obs)
			{
				UnmapViewOfFile(obs);
				obs = nullptr;
			}

			if (obsMapping)
			{
				CloseHandle(obsMapping);
				obsMapping = nullptr;
			}
		};

	while (WaitForSingleObject(_relayStop.get(), 1) == WAIT_TIMEOUT)
	{
		auto now = GetTickCount64();
		if ((!rdp || !obs) && now - lastOpenTry >= 500)
		{
			lastOpenTry = now;
			if (!rdp)
			{
				// created by the media source once a program opens the camera
				rdpMapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, OBSRDP_SECTION_NAME);
				if (rdpMapping)
				{
					rdp = (BYTE*)MapViewOfFile(rdpMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, OBSRDP_SECTION_SIZE);
					if (!rdp)
					{
						CloseHandle(rdpMapping);
						rdpMapping = nullptr;
					}
					else
					{
						WINTRACE(L"RelayThread global section opened");
					}
				}
			}

			if (!obs)
			{
				obsMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, OBS_QUEUE_NAME);
				if (obsMapping)
				{
					obs = (const BYTE*)MapViewOfFile(obsMapping, FILE_MAP_READ, 0, 0, 0);
					MEMORY_BASIC_INFORMATION mbi{};
					if (obs && VirtualQuery(obs, &mbi, sizeof(mbi)))
					{
						obsSize = mbi.RegionSize;
						obsLastIdx = 0;
						obsLastChange = now;
						WINTRACE(L"RelayThread OBS virtual camera opened");
					}
					else
					{
						closeObs();
					}
				}
			}
		}

		if (!obs)
			continue;

		auto queue = (const ObsQueueHeader*)obs;
		auto state = queue->state;
		if (state == OBS_QUEUE_STATE_STOPPING)
		{
			WINTRACE(L"RelayThread OBS virtual camera stopped");
			closeObs();
			continue;
		}

		auto idx = queue->read_idx;
		if (state != OBS_QUEUE_STATE_READY || idx == obsLastIdx)
		{
			// OBS gone without saying so (crash, etc.)
			if (now - obsLastChange > OBSRDP_STALE_MS)
			{
				WINTRACE(L"RelayThread OBS virtual camera idle");
				closeObs();
			}
			continue;
		}

		obsLastIdx = idx;
		obsLastChange = now;
		if (!rdp)
			continue;

		auto cx = queue->cx;
		auto cy = queue->cy;
		auto size = (SIZE_T)cx * cy * 3 / 2;
		auto offset = (SIZE_T)queue->offsets[idx % 3] + OBS_FRAME_HEADER_SIZE;
		if (cx < 2 || cy < 2 || cx > OBSRDP_MAX_WIDTH || cy > OBSRDP_MAX_HEIGHT || offset + size > obsSize)
			continue;

		auto header = (ObsRdpHeader*)rdp;
		InterlockedIncrement(&header->seq);
		header->width = cx;
		header->height = cy;
		memcpy(rdp + OBSRDP_DATA_OFFSET, obs + offset, size);
		InterlockedIncrement(&header->seq);
		header->magic = OBSRDP_MAGIC;
		InterlockedExchange64(&header->lastWrite, (LONG64)GetTickCount64());
	}

	closeObs();
	if (rdp)
	{
		UnmapViewOfFile(rdp);
	}

	if (rdpMapping)
	{
		CloseHandle(rdpMapping);
	}

	timeEndPeriod(1);
	return 0;
}

ATOM MyRegisterClass(HINSTANCE instance)
{
	WNDCLASSEXW wcex{};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = instance;
	wcex.hIcon = LoadIcon(instance, MAKEINTRESOURCE(IDI_OBSRDPCAM));
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_OBSRDPCAM);
	wcex.lpszClassName = _windowClass;
	wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));
	return RegisterClassExW(&wcex);
}

HWND InitInstance(HINSTANCE instance, int cmd)
{
	_instance = instance;
	auto hwnd = CreateWindowW(_windowClass, _title, WS_OVERLAPPEDWINDOW, 0, 0, 600, 400, nullptr, nullptr, instance, nullptr);
	if (!hwnd)
		return nullptr;

	CenterWindow(hwnd);
	ShowWindow(hwnd, cmd);
	UpdateWindow(hwnd);
	return hwnd;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	//#if _DEBUG
	//	if (message != WM_NCMOUSEMOVE && message != WM_MOUSEMOVE && message != WM_SETCURSOR && message != WM_NCHITTEST && message != WM_MOUSELEAVE &&
	//		message != WM_GETICON && message != WM_PAINT)
	//	{
	//		if (message == 147 || message == 148)
	//		{
	//			WINTRACE("msg:%u 0x%08X (%s)", message, message, WM_ToString(message).c_str());
	//		}
	//	}
	//#endif

	switch (message)
	{
	case WM_COMMAND:
	{
		auto wmId = LOWORD(wParam);
		switch (wmId)
		{
		case IDM_ABOUT:
			DialogBox(_instance, MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd, About);
			break;

		case IDM_EXIT:
			DestroyWindow(hwnd);
			break;
		default:
			return DefWindowProc(hwnd, message, wParam, lParam);
		}
	}
	break;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		auto hdc = BeginPaint(hwnd, &ps);
		// TODO: Add any drawing code that uses hdc here...
		EndPaint(hwnd, &ps);
	}
	break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	default:
		return DefWindowProc(hwnd, message, wParam, lParam);
	}
	return 0;
}

INT_PTR CALLBACK About(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);
	switch (message)
	{
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
		{
			EndDialog(hwnd, LOWORD(wParam));
			return (INT_PTR)TRUE;
		}
		break;
	}
	return (INT_PTR)FALSE;
}
