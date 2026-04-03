#include "gs_storage_status_shared.h"

#include <iomanip>
#include <sstream>

#include "gs_shared_runtime.h"

namespace
{

std::string formatStorageLine(const char* label, const char* status, double free_gb, double total_gb, const char* suffix = "")
{
    std::ostringstream out;
    out << label << ": " << status;
    if (suffix != nullptr && suffix[0] != 0)
    {
        out << suffix;
    }
    out << ' ' << std::fixed << std::setprecision(2) << free_gb << "GB/" << total_gb << "GB";
    return out.str();
}

}

std::string formatAirStorageStatusLine(const AirStorageStatusView& status,
                                       const char* detected_label,
                                       const char* missing_label,
                                       const char* trailing_suffix)
{
    const char* suffix = status.error ? " Error" : (status.slow ? " Slow" : "");
    std::string line = formatStorageLine(
        "AIR SD",
        status.detected && !status.error ? detected_label : missing_label,
        status.free_space_gb16 / 16.0,
        status.total_space_gb16 / 16.0,
        suffix);
    if (trailing_suffix != nullptr && trailing_suffix[0] != 0)
    {
        line += ' ';
        line += trailing_suffix;
    }
    return line;
}

std::string formatGroundStorageStatusLine(const GroundStorageStatus& status)
{
    return formatStorageLine(
        "GS SD",
        status.free_space_bytes >= kGSMinFreeSpaceBytes ? "Ok" : "Low space",
        static_cast<double>(status.free_space_bytes) / (1024.0 * 1024.0 * 1024.0),
        static_cast<double>(status.total_space_bytes) / (1024.0 * 1024.0 * 1024.0));
}
