#pragma once

// Shared between the media source (loaded by the Frame Server service, session 0)
// and the relay app (running in the user session next to OBS).

#include <stdint.h>

// d711ea3f-582b-4adc-a001-fa416e1925ba
#define OBSRDP_CLSID_INIT { 0xd711ea3f,0x582b,0x4adc,{0xa0,0x01,0xfa,0x41,0x6e,0x19,0x25,0xba} }

// global section created by the media source (services have SeCreateGlobalPrivilege), written by the relay.
// Access: SYSTEM, LocalService (Frame Server), admins and interactive users; the medium integrity label
// keeps sandboxed (low integrity) processes from writing frames.
#define OBSRDP_SECTION_NAME L"Global\\OBSRdpCamFrames"
#define OBSRDP_SECTION_SDDL L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;IU)S:(ML;;NW;;;ME)"

// format advertised by the camera ("WxHxINTERVAL", same syntax as OBS' obs-virtualcam.txt), written by the relay
#define OBSRDP_FORMAT_DIR L"C:\\ProgramData\\OBSRdpCam"
#define OBSRDP_FORMAT_FILE OBSRDP_FORMAT_DIR L"\\format.txt"

#define OBSRDP_MAGIC 0x4D435230
#define OBSRDP_MAX_WIDTH 3840
#define OBSRDP_MAX_HEIGHT 2160
#define OBSRDP_DATA_OFFSET 64
#define OBSRDP_SECTION_SIZE (OBSRDP_DATA_OFFSET + OBSRDP_MAX_WIDTH * OBSRDP_MAX_HEIGHT * 3 / 2)
#define OBSRDP_STALE_MS 2000

#define OBSRDP_DEFAULT_WIDTH 1920
#define OBSRDP_DEFAULT_HEIGHT 1080
#define OBSRDP_DEFAULT_INTERVAL 333333

struct ObsRdpHeader
{
	uint32_t magic;
	volatile LONG seq; // odd while the relay is writing a frame
	uint32_t width;
	uint32_t height;
	volatile LONG64 lastWrite; // GetTickCount64() of the last frame written
};

// OBS virtual camera shared memory layout (obs-studio/plugins/win-dshow/shared-memory-queue.c)
#define OBS_QUEUE_NAME L"OBSVirtualCamVideo"
#define OBS_FRAME_HEADER_SIZE 32
#define OBS_QUEUE_STATE_READY 2
#define OBS_QUEUE_STATE_STOPPING 3

struct ObsQueueHeader
{
	volatile uint32_t write_idx;
	volatile uint32_t read_idx;
	volatile uint32_t state;
	uint32_t offsets[3];
	uint32_t type;
	uint32_t cx;
	uint32_t cy;
	uint64_t interval;
	uint32_t reserved[8];
};
