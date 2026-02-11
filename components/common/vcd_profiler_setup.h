#pragma once

//Uncomment to enable profiler
//#define PROFILE_CAMERA_DATA

//===========================================================
#define START_PROFILER_WITH_BUTTON

#define PROFILER_OUTPUT_TO_CONSOLE

#ifdef DVR_SUPPORT
#define PROFILER_OUTPUT_TO_SD
#endif
//===========================================================

//------------------------
#ifdef PROFILE_CAMERA_DATA

#define ENABLE_PROFILER

#define PF_CAM_EVENT_DMA_EOF    0
#define PF_CAM_EVENT_VSYNC      1
#define PF_CAM_EVENT_PARLIO     2
#define PF_PARLIO_DATA          3
#define PF_CAMERA_DATA          4
#define PF_CAMERA_FRAME_QUALITY 5
#define PF_CAMERA_DATA_SIZE     6
#define PF_CAMERA_FEC_POOL      7
#define PF_CAMERA_FEC           8
#define PF_CAMERA_WIFI_TX       9
#define PF_CAMERA_WIFI_QUEUE    10
#define PF_CAMERA_SD_FAST_BUF   11
#define PF_CAMERA_SD_SLOW_BUF   12
#define PF_CAMERA_FEC_SPIN      13
#define PF_CAMERA_WIFI_SPIN     14
#define PF_CAMERA_WIFI_DONE_CB  15
#define PF_CAMERA_FEC_OVF       16
#define PF_CAMERA_WIFI_OVF      17
#define PF_CAMERA_OVF           18
#define PF_CAMERA_SD_OVF        19

#define PF0_NAME "cam_dma_eof"
#define PF1_NAME "cam_vsync"
#define PF2_NAME "cam_parlio"
#define PF3_NAME "parlio_data"
#define PF4_NAME "cam_data"
#define PF5_NAME "quality"
#define PF6_NAME "data_size"
#define PF7_NAME "fec_pool"
#define PF8_NAME "fec"
#define PF9_NAME "wifi_tx"
#define PF10_NAME "wifi_queue"
#define PF11_NAME "sd_fast_buf"
#define PF12_NAME "sd_slow_buf"
#define PF13_NAME "fec_spin"
#define PF14_NAME "wifi_spin"
#define PF15_NAME "wifi_done_cb"
#define PF16_NAME "fec_ovf"
#define PF17_NAME "wifi_ovf"
#define PF18_NAME "cam_ovf"
#define PF19_NAME "sd_ovf"


#endif
