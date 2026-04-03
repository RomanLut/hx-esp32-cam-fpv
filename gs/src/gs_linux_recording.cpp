#include "gs_linux_recording.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <sys/statvfs.h>

#include "Log.h"
#include "avi.h"
#include "gs_linux_runtime.h"
#include "gs_runtime_core.h"
#include "gs_shared_runtime.h"
#include "packets.h"
#include "utils.h"

void getFilesystemStats(const char* path, unsigned long long* total, unsigned long long* free)
{
    struct statvfs stat;
    if (statvfs(path, &stat) != 0)
    {
        perror("statvfs");
        exit(EXIT_FAILURE);
    }

    *total = static_cast<unsigned long long>(stat.f_frsize) * stat.f_blocks;
    *free = static_cast<unsigned long long>(stat.f_frsize) * stat.f_bfree;
}

void updateGSSdFreeSpace()
{
    struct statvfs stat;
    statvfs(".", &stat);
    s_GSSDTotalSpaceBytes = static_cast<unsigned long long>(stat.f_frsize) * stat.f_blocks;
    s_GSSDFreeSpaceBytes = static_cast<unsigned long long>(stat.f_frsize) * stat.f_bfree;
}

void toggleGSRecording(int width, int height, const char* reason)
{
    std::lock_guard<std::mutex> lg(s_groundstation_config.record_mutex);

    s_groundstation_config.record = !s_groundstation_config.record;

    if (s_groundstation_config.record)
    {
        if (s_GSSDFreeSpaceBytes < kGSMinFreeSpaceBytes)
        {
            s_groundstation_config.record = false;
            return;
        }

        auto time = std::time({});
#ifdef WRITE_RAW_MJPEG_STREAM
        char filename[] = "yyyy-mm-dd-hh-mm-ss.mjpeg";
        std::strftime(filename, sizeof(filename), "%Y-%m-%d-%H-%M-%S.mjpeg", std::localtime(&time));
        s_groundstation_config.record_file = fopen(filename, "wb+");
#else
        char filename[] = "yyyy-mm-dd-hh-mm-ss.avi";
        std::strftime(filename, sizeof(filename), "%Y-%m-%d-%H-%M-%S.avi", std::localtime(&time));
        const Ground2Air_Config_Packet config_snapshot = s_runtimeCore.session.copyConfigPacket();

        prepAviIndex();
        s_avi_frameCnt = 0;

        const TVMode* v = &vmodes[std::clamp((int)config_snapshot.camera.resolution, 0, (int)(Resolution::COUNT) - 1)];
        if (width != 0)
        {
            for (size_t i = 0; i < (int)Resolution::COUNT; i++)
            {
                if (vmodes[i].width == width && vmodes[i].height == height)
                {
                    v = &vmodes[i];
                    break;
                }
            }
        }

        if (s_isOV5640)
        {
            s_avi_fps = config_snapshot.camera.ov5640HighFPS ? v->highFPS5640 : v->FPS5640;
        }
        else
        {
            s_avi_fps = config_snapshot.camera.ov2640HighFPS ? v->highFPS2640 : v->FPS2640;
        }

        s_avi_frameWidth = width;
        s_avi_frameHeight = height;
        s_avi_ov2640HighFPS = config_snapshot.camera.ov2640HighFPS;
        s_avi_ov5640HighFPS = config_snapshot.camera.ov5640HighFPS;

        LOGI("{}x{} {}fps\n", s_avi_frameWidth, s_avi_frameHeight, s_avi_fps);

        s_groundstation_config.record_file = fopen(filename, "wb+");
        fwrite(aviHeader, AVI_HEADER_LEN, 1, s_groundstation_config.record_file);
#endif
        LOGI("start record:{} reason:{}", std::string(filename), reason ? reason : "unknown");
    }
    else
    {
#ifdef WRITE_RAW_MJPEG_STREAM
#else
        finalizeAviIndex(s_avi_frameCnt);

        size_t SD_WRITE_BLOCK_SIZE = 8192;
        uint8_t sd_write_block[SD_WRITE_BLOCK_SIZE];
        while (true)
        {
            size_t sz = writeAviIndex(sd_write_block, SD_WRITE_BLOCK_SIZE);
            if (sz == 0)
            {
                break;
            }
            fwrite(sd_write_block, sz, 1, s_groundstation_config.record_file);
        }

        buildAviHdr(s_avi_fps, s_avi_frameWidth, s_avi_frameHeight, s_avi_frameCnt);
        fseek(s_groundstation_config.record_file, 0, SEEK_SET);
        fwrite(aviHeader, AVI_HEADER_LEN, 1, s_groundstation_config.record_file);
#endif

        fflush(s_groundstation_config.record_file);
        fclose(s_groundstation_config.record_file);
        s_groundstation_config.record_file = nullptr;
    }

    updateGSSdFreeSpace();
}
