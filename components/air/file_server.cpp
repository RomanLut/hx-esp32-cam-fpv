/* HTTP File Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include <esp_flash_partitions.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "main.h"
#include "nvs_args.h"
#include "packets.h"
#include "fec.h"
#include "../common/avi.h"
#include <stdint.h> // For uint32_t

#include "wifi.h"

#define IDX_ENTRY sizeof(avi_idx_entry_t)


/* Max length a file path can have on storage */
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)

/* Max size of an individual file. Make sure this
 * value is same as that set in upload_script.html */
#define MAX_FILE_SIZE   (200*1024) // 200 KB
#define MAX_FILE_SIZE_STR "200KB"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE  8192

// Structure to hold parsed AVI header information
typedef struct {
    uint32_t movi_list_offset;
    uint32_t movi_list_size;
    uint32_t idx1_chunk_offset;
    uint32_t idx1_chunk_size;
    uint32_t dwMicroSecPerFrame_val;
    uint32_t dwTotalFrames_val;
} avi_parse_info_t;


struct file_server_data 
{
    /* Base path of file storage */
    char base_path[ESP_VFS_PATH_MAX + 1];

    /* Scratch buffer for temporary storage during file transfer */
    char scratch[SCRATCH_BUFSIZE];
};

static const char *TAG = "file_server";

// Function to parse AVI header information
static esp_err_t parse_avi_header_info(FILE *fd, avi_parse_info_t *info) 
{
    memset(info, 0, sizeof(avi_parse_info_t)); // Initialize all fields to 0

    avi_chunk_t chunk_hdr;
    uint32_t riff_file_size;

    // Read RIFF header
    if (fread(&chunk_hdr, 1, sizeof(avi_chunk_t), fd) != sizeof(avi_chunk_t) ||
        memcmp(&chunk_hdr.fcc, "RIFF", 4) != 0) {
        ESP_LOGE(TAG, "Not a RIFF file or failed to read RIFF header");
        return ESP_FAIL;
    }
    riff_file_size = chunk_hdr.cb; // Total file size - 8 bytes (RIFF + size)

    char file_type[4];
    if (fread(file_type, 1, 4, fd) != 4 || memcmp(file_type, "AVI ", 4) != 0) {
        ESP_LOGE(TAG, "Not an AVI file");
        return ESP_FAIL;
    }

    // Loop through top-level chunks
    while (ftell(fd) + sizeof(avi_chunk_t) <= riff_file_size + 8) { // +8 for RIFF header itself
        if (fread(&chunk_hdr, 1, sizeof(avi_chunk_t), fd) != sizeof(avi_chunk_t)) {
            ESP_LOGE(TAG, "Failed to read chunk header");
            return ESP_FAIL;
        }

        if (memcmp(&chunk_hdr.fcc, "LIST", 4) == 0) {
            char list_type[4];
            if (fread(list_type, 1, 4, fd) != 4) {
                ESP_LOGE(TAG, "Failed to read LIST type");
                return ESP_FAIL;
            }

            if (memcmp(list_type, "hdrl", 4) == 0) {
                // Parse hdrl list
                long hdrl_end_pos = ftell(fd) + chunk_hdr.cb - 4; // -4 for 'hdrl' type itself
                while (ftell(fd) + sizeof(avi_chunk_t) <= hdrl_end_pos) {
                    avi_chunk_t sub_chunk_hdr;
                    if (fread(&sub_chunk_hdr, 1, sizeof(avi_chunk_t), fd) != sizeof(avi_chunk_t)) {
                        ESP_LOGE(TAG, "Failed to read hdrl sub-chunk header");
                        return ESP_FAIL;
                    }
                    if (memcmp(&sub_chunk_hdr.fcc, "avih", 4) == 0) {
                        // Read dwMicroSecPerFrame and dwTotalFrames
                        if (fread(&info->dwMicroSecPerFrame_val, 1, 4, fd) != 4) {
                            ESP_LOGE(TAG, "Failed to read dwMicroSecPerFrame");
                            return ESP_FAIL;
                        }
                        // Skip 0xC bytes to reach dwTotalFrames (dwMaxBytesPerSec, dwPaddingGranularity, dwFlags)
                        fseek(fd, 0xC, SEEK_CUR);
                        if (fread(&info->dwTotalFrames_val, 1, 4, fd) != 4) {
                            ESP_LOGE(TAG, "Failed to read dwTotalFrames");
                            return ESP_FAIL;
                        }
                        // Skip remaining avih chunk data (total size - dwMicroSecPerFrame - skipped bytes - dwTotalFrames)
                        fseek(fd, sub_chunk_hdr.cb - 4 - 0xC - 4, SEEK_CUR);
                    } else {
                        // Skip other sub-chunks in hdrl
                        fseek(fd, sub_chunk_hdr.cb, SEEK_CUR);
                    }
                    // Ensure 2-byte alignment for chunks
                    if (sub_chunk_hdr.cb % 2 != 0) {
                        fseek(fd, 1, SEEK_CUR);
                    }
                }
            } else if (memcmp(list_type, "movi", 4) == 0) {
                info->movi_list_offset = ftell(fd); // Current position is start of movi data
                info->movi_list_size = chunk_hdr.cb - 4; // -4 for 'movi' type itself
                fseek(fd, info->movi_list_size, SEEK_CUR); // Skip movi data
            } else {
                // Skip other LIST chunks
                fseek(fd, chunk_hdr.cb - 4, SEEK_CUR); // -4 for list_type
            }
        } else if (memcmp(&chunk_hdr.fcc, "idx1", 4) == 0) {
            info->idx1_chunk_offset = ftell(fd); // Current position is start of idx1 data
            info->idx1_chunk_size = chunk_hdr.cb;
            fseek(fd, info->idx1_chunk_size, SEEK_CUR); // Skip idx1 data
        } else {
            // Skip other top-level chunks
            fseek(fd, chunk_hdr.cb, SEEK_CUR);
        }
        // Ensure 2-byte alignment for chunks
        if (chunk_hdr.cb % 2 != 0) {
            fseek(fd, 1, SEEK_CUR);
        }
    }

    // Check if essential info was found
    if (info->movi_list_offset == 0 || info->movi_list_size == 0 || info->dwMicroSecPerFrame_val == 0) {
        ESP_LOGE(TAG, "Failed to parse essential AVI chunks (movi, avih)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Handler to redirect incoming GET request for /index.html to /
 * This can be overridden by uploading file with same name */
static esp_err_t index_html_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);  // Response body can be empty
    return ESP_OK;
}

/* Handler to respond with an icon file embedded in flash.
 * Browsers expect to GET website icon at URI /favicon.ico.
 * This can be overridden by uploading file with same name */
static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[]   asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
    return ESP_OK;
}

/* Send HTTP response with a run-time generated html consisting of
 * a list of all files and folders under the requested path.
 * In case of SPIFFS this returns empty list when path is any
 * string other than '/', since SPIFFS doesn't support directories */
static esp_err_t http_resp_dir_html(httpd_req_t *req, const char *dirpath)
{
    /* Get handle to embedded file upload script */
    extern const unsigned char html_start[] asm("_binary_index_html_start");
    extern const unsigned char html_end[]   asm("_binary_index_html_end");
    const size_t html_size = (html_end - html_start);

    /* Add file upload form and script which on execution sends a POST request to /upload */
    httpd_resp_send_chunk(req, (const char *)html_start, html_size);

    /* Send empty chunk to signal HTTP response completion */
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".pdf")) {
        return httpd_resp_set_type(req, "application/pdf");
    } else if (IS_FILE_EXT(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (IS_FILE_EXT(filename, ".jpeg")) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (IS_FILE_EXT(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    }
    
    return httpd_resp_set_type(req, "application/octet-stream");
}

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        /* Full path string won't fit into destination buffer */
        return NULL;
    }

    /* Construct full path (base + path) */
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    /* Return pointer to path, skipping the base */
    return dest + base_pathlen;
}

/* Handler to download a file kept on the server */
static esp_err_t download_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;

    const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
                                             req->uri, sizeof(filepath));
    if (!filename) {
        ESP_LOGE(TAG, "Filename is too long");
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* If name has trailing '/', respond with directory contents */
    if (filename[strlen(filename) - 1] == '/') {
        return http_resp_dir_html(req, filepath);
    }

    if (stat(filepath, &file_stat) == -1) {
        /* If file not present on SPIFFS check if URI
         * corresponds to one of the hardcoded paths */
        if (strcmp(filename, "/index.html") == 0) {
            return index_html_get_handler(req);
        } else if (strcmp(filename, "/favicon.ico") == 0) {
            return favicon_get_handler(req);
        }
        ESP_LOGE(TAG, "Failed to stat file : %s", filepath);
        /* Respond with 404 Not Found */
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    fd = fopen(filepath, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending file : %s (%ld bytes)...", filename, file_stat.st_size);
    set_content_type_from_file(req, filename);

    /* Retrieve the pointer to scratch buffer for temporary storage */
    char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
    size_t chunksize;
    do {
        /* Read file in chunks into the scratch buffer */
        chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);

        if (chunksize > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
                fclose(fd);
                ESP_LOGE(TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
               return ESP_FAIL;
           }
        }

        /* Keep looping till the whole file is sent */
    } while (chunksize != 0);

    /* Close file after sending complete */
    fclose(fd);
    ESP_LOGI(TAG, "File sending complete");

    /* Respond with an empty chunk to signal HTTP response completion */
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t stream_info_get_handler(httpd_req_t *req) {
    char filepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;

    char file_param[100];
    if (httpd_req_get_url_query_str(req, file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }

    char filename_val[100];
    if (httpd_query_key_value(file_param, "file", filename_val, sizeof(filename_val)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file value");
        return ESP_FAIL;
    }

    const char *base_path = ((struct file_server_data *)req->user_ctx)->base_path;
    strlcpy(filepath, base_path, sizeof(filepath));
    strlcat(filepath, filename_val, sizeof(filepath));

    if (stat(filepath, &file_stat) == -1) {
        ESP_LOGE(TAG, "Failed to stat file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    fd = fopen(filepath, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    avi_parse_info_t avi_info;
    if (parse_avi_header_info(fd, &avi_info) != ESP_OK) {
        fclose(fd);
        return ESP_FAIL;
    }
    fclose(fd); // Close file after parsing

    uint32_t duration_ms = 0;
    if (avi_info.dwMicroSecPerFrame_val > 0 && avi_info.dwTotalFrames_val > 0) {
        duration_ms = (uint32_t)(((uint64_t)avi_info.dwTotalFrames_val * avi_info.dwMicroSecPerFrame_val) / 1000);
    }

    cJSON *response_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(response_json, "file_length", file_stat.st_size);
    cJSON_AddNumberToObject(response_json, "duration_ms", duration_ms);
    cJSON_AddNumberToObject(response_json, "micro_sec_per_frame", avi_info.dwMicroSecPerFrame_val);
    cJSON_AddNumberToObject(response_json, "total_frames", avi_info.dwTotalFrames_val);

    httpd_resp_set_type(req, "application/json");
    const char *json_str = cJSON_Print(response_json);
    httpd_resp_sendstr(req, json_str);
    cJSON_Delete(response_json);
    cJSON_free((void *)json_str);

    return ESP_OK;
}

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;

    char file_param[100];
    if (httpd_req_get_url_query_str(req, file_param, sizeof(file_param)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }

    char filename_val[100];
    if (httpd_query_key_value(file_param, "file", filename_val, sizeof(filename_val)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file value");
        return ESP_FAIL;
    }

    const char *base_path = ((struct file_server_data *)req->user_ctx)->base_path;
    strlcpy(filepath, base_path, sizeof(filepath));
    strlcat(filepath, filename_val, sizeof(filepath));

    if (stat(filepath, &file_stat) == -1) {
        ESP_LOGE(TAG, "Failed to stat file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
        return ESP_FAIL;
    }

    fd = fopen(filepath, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    avi_parse_info_t avi_info;
    if (parse_avi_header_info(fd, &avi_info) != ESP_OK) {
        fclose(fd);
        return ESP_FAIL;
    }

    // Check if essential info was found for streaming
    if (avi_info.idx1_chunk_offset == 0 || avi_info.idx1_chunk_size == 0 || avi_info.dwTotalFrames_val == 0) {
        ESP_LOGE(TAG, "Failed to parse essential AVI chunks (idx1, avih) for streaming");
        fclose(fd);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=--FRAME");

    char pos_param[100];
    uint32_t seek_pos_ms = 0;
    if (httpd_query_key_value(file_param, "pos", pos_param, sizeof(pos_param)) == ESP_OK) {
        seek_pos_ms = atoi(pos_param);
    }

    // Calculate frame_offset based on seek_pos_ms and frame duration
    uint32_t initial_frame_offset = 0;
    if (avi_info.dwMicroSecPerFrame_val > 0) {
        initial_frame_offset = (seek_pos_ms * 1000) / avi_info.dwMicroSecPerFrame_val;
    }

    ESP_LOGI(TAG, "Start Frame: %u", (unsigned int)initial_frame_offset);

    char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
    
    // Seek to the start of the idx1 chunk data
    fseek(fd, avi_info.idx1_chunk_offset, SEEK_SET);

    // The idx1 chunk size is already stored in idx1_chunk_size
    if (initial_frame_offset >= (avi_info.idx1_chunk_size / IDX_ENTRY)) {
        ESP_LOGE(TAG, "Requested frame %u is out of bounds (total frames: %u)", (unsigned int)initial_frame_offset, (unsigned int)(avi_info.idx1_chunk_size / IDX_ENTRY));
        fclose(fd);
        return ESP_FAIL;
    }

    // Seek to the specific index entry within the idx1 chunk
    fseek(fd, avi_info.idx1_chunk_offset + (initial_frame_offset * IDX_ENTRY), SEEK_SET);
    
    avi_idx_entry_t idx_entry;
    if (fread(&idx_entry, 1, sizeof(avi_idx_entry_t), fd) != sizeof(avi_idx_entry_t)) {
        ESP_LOGE(TAG, "Failed to read index entry for frame %u", (unsigned int)initial_frame_offset);
        fclose(fd);
        return ESP_FAIL;
    }

    // Seek to the actual frame data relative to the start of the 'movi' list data
    fseek(fd, avi_info.movi_list_offset + idx_entry.dwOffset, SEEK_SET);

    uint8_t header[8];
    uint32_t frame_size;
    uint32_t current_frame_idx = initial_frame_offset;

    // Loop to stream frames from the desired starting point
    while (true) {
        // Read the current frame's header
        if (fread(header, 1, 8, fd) != 8) {
            ESP_LOGI(TAG, "End of file or failed to read frame header");
            break; // End of file or read error
        }
        if (memcmp(header, dcBuf, 4) != 0) {
            ESP_LOGE(TAG, "Invalid frame marker");
            break;
        }
        memcpy(&frame_size, &header[4], 4);

        ESP_LOGI(TAG, "Streaming Frame: %u, size: %u", (unsigned int)current_frame_idx, (unsigned int)frame_size);
        current_frame_idx++;

        httpd_resp_send_chunk(req, "--FRAME\r\n", 9);
        char frame_header[100];
        int frame_header_len = snprintf(frame_header, sizeof(frame_header), "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned int)frame_size);
        httpd_resp_send_chunk(req, frame_header, frame_header_len);

        size_t remaining = frame_size;
        while (remaining > 0) {
            size_t to_read = (remaining < SCRATCH_BUFSIZE) ? remaining : SCRATCH_BUFSIZE;
            size_t bytes_read = fread(chunk, 1, to_read, fd);
            if (bytes_read <= 0) {
                ESP_LOGE(TAG, "Failed to read frame data");
                fclose(fd);
                return ESP_FAIL;
            }
            if (httpd_resp_send_chunk(req, chunk, bytes_read) != ESP_OK) {
                ESP_LOGE(TAG, "File sending failed!");
                fclose(fd);
                return ESP_FAIL;
            }
            remaining -= bytes_read;
        }

        // Align to 4-byte boundary
        int padding = (4 - (frame_size % 4)) % 4;
        if (padding > 0) {
            fseek(fd, padding, SEEK_CUR);
        }
    }

    fclose(fd);
    httpd_resp_send_chunk(req, "--FRAME--\r\n", 11);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t delete_post_handler(httpd_req_t *req)
{
    if ( isUSBDiskMounted() ) 
    {
        ESP_LOGE(TAG, "Can not delete while usb is mounted");
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cannot delete files while USB disk is mounted.");
        return ESP_FAIL;
    }
    char filepath[FILE_PATH_MAX];
    struct stat file_stat;

    char buf[100]={0};
    httpd_req_recv(req,buf,req->content_len);
    cJSON *root = cJSON_Parse(buf);
    char *filename = cJSON_GetStringValue(cJSON_GetObjectItem(root,"name"));
    strcpy(filepath,((struct file_server_data *)req->user_ctx)->base_path);
    strcat(filepath,filename);
    cJSON_Delete(root);

    if (stat(filepath, &file_stat) == -1) {
        ESP_LOGE(TAG, "File does not exist : %s", filename);
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File does not exist");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleting file : %s", filename);
    /* Delete file */
    if (unlink(filepath) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
        return ESP_FAIL;
    }

    updateSDInfo();

    httpd_resp_set_type(req, "application/json");
    cJSON *response_json = cJSON_CreateObject();
    cJSON_AddNumberToObject(response_json, "sd_free_space", SDFreeSpaceGB16);
    const char *json_str = cJSON_Print(response_json);

    char free_space_str[16];
    snprintf(free_space_str, sizeof(free_space_str), "%d", SDFreeSpaceGB16);
    httpd_resp_set_hdr(req, "X-Free-Space", free_space_str);

    char total_space_str[16];
    snprintf(total_space_str, sizeof(total_space_str), "%d", SDTotalSpaceGB16);
    httpd_resp_set_hdr(req, "X-Total-Space", total_space_str);

#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_sendstr(req, json_str);
    cJSON_Delete(response_json);
    cJSON_free((void *)json_str);

    return ESP_OK;
}

static esp_err_t file_list_handler(httpd_req_t *req)
{
    updateSDInfo();

    cJSON *root;
    root = cJSON_CreateObject();
    cJSON *files_array = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "files", files_array);

    cJSON_AddNumberToObject(root, "sd_total_space", SDTotalSpaceGB16);
    cJSON_AddNumberToObject(root, "sd_free_space", SDFreeSpaceGB16);
    cJSON_AddBoolToObject(root, "sd_initialized", s_sd_initialized);
    cJSON_AddBoolToObject(root, "sd_list_error", false);

    if (!s_sd_initialized) {
        ESP_LOGW(TAG, "SD Card not initialized, not listing files.");
        // No need to open directory or list files if SD card is not initialized
        cJSON_SetBoolValue(cJSON_GetObjectItem(root, "sd_list_error"), true); // Indicate an error in listing
    } else {
        const char *dirpath = ((struct file_server_data *)req->user_ctx)->base_path;
        DIR *dir = opendir(dirpath);

        if (!dir) {
            ESP_LOGE(TAG, "Failed to open directory %s", dirpath);
            cJSON_SetBoolValue(cJSON_GetObjectItem(root, "sd_list_error"), true); // Indicate an error in listing
        } else {
            char entrypath[FILE_PATH_MAX];
            char entrysize[16];
            struct dirent *entry;
            struct stat entry_stat;
            const char *entrytype;
            const size_t dirpath_len = strlen(dirpath);
            strlcpy(entrypath, dirpath, sizeof(entrypath));
            strcat(entrypath,"/");

            while ((entry = readdir(dir)) != NULL) {
                // Only list files, and only if they are .avi files
                if (entry->d_type == DT_REG && IS_FILE_EXT(entry->d_name, ".avi")) {
                    entrytype = "file";

                    strlcpy(entrypath + dirpath_len, entry->d_name, sizeof(entrypath) - dirpath_len);
                    if (stat(entrypath, &entry_stat) == -1) {
                        ESP_LOGE(TAG, "Failed to stat %s : %s.", entrytype, entrypath);
                        continue;
                    }
                    sprintf(entrysize, "%ld", entry_stat.st_size);
                    ESP_LOGI(TAG, "Found %s : %s (%s bytes)", entrytype, entry->d_name, entrysize);

                    cJSON *element;
                    element = cJSON_CreateObject();
                    cJSON_AddStringToObject(element,"name",entry->d_name);
                    cJSON_AddStringToObject(element,"size",entrysize);
                    cJSON_AddItemToArray(files_array,element);
                }
            }
            closedir(dir);
        }
    }

    httpd_resp_set_type(req,"application/json");
    const char *json_str = cJSON_Print(root);
    httpd_resp_sendstr(req, json_str);
    cJSON_Delete(root);
    cJSON_free((void *)json_str);
    return ESP_OK;
}
static esp_err_t configs_handler(httpd_req_t *req)
{
    if(req->method == HTTP_GET)
    {
        char channel_str[6]="";
        char packet_version_str[6]="";
        cJSON *root;
        root = cJSON_CreateObject();

        snprintf(channel_str, sizeof(channel_str), "%d", s_ground2air_config_packet.dataChannel.wifi_channel);
        cJSON_AddStringToObject(root,"channel",channel_str);
        cJSON_AddStringToObject(root,"default_dvr", s_ground2air_config_packet.misc.autostartRecord ? "true" : "false");
        cJSON_AddStringToObject(root, "fw_version", FW_VERSION);
        snprintf(packet_version_str, sizeof(packet_version_str), "%d", PACKET_VERSION);
        cJSON_AddStringToObject(root, "packet_version", packet_version_str);

        /* Send chunk of HTML file containing table entries with file name and size */
        httpd_resp_set_type(req,"application/json");
        const char *json_str = cJSON_Print(root);
        httpd_resp_sendstr(req, json_str);
        cJSON_Delete(root);
        cJSON_free((void *)json_str);

    }
    else
    {
        char buf[100]={0};
        httpd_req_recv(req,buf,req->content_len);
        
        cJSON *root = cJSON_Parse(buf);
        
        uint16_t channel = atoi(cJSON_GetStringValue(cJSON_GetObjectItem(root,"channel")));
        if ( channel <= WIFI_CHANNELS_COUNT )
        {
            nvs_args_set("channel",channel);
            s_ground2air_config_packet.dataChannel.wifi_channel = channel;
        }
        
        cJSON *default_dvr = cJSON_GetObjectItem(root, "default_dvr");
        if (default_dvr != NULL && cJSON_IsString(default_dvr)) 
        {
            s_ground2air_config_packet.misc.autostartRecord = strcmp(default_dvr->valuestring, "true") == 0;
            nvs_args_set("autostartRecord", s_ground2air_config_packet.misc.autostartRecord ? 1 : 0);
        }        
        
        cJSON_Delete(root);
        httpd_resp_sendstr(req, "Ok");
    }

    return ESP_OK;
}


//-----------------------------------------------------------------------------
static esp_err_t _ota_get_handler( httpd_req_t *req )
{
    extern const unsigned char html_start[] asm("_binary_index_html_start");
    extern const unsigned char html_end[]   asm("_binary_index_html_end");
    const size_t html_size = (html_end - html_start);
    httpd_resp_set_status( req, HTTPD_200 );
    httpd_resp_set_hdr( req, "Connection", "keep-alive" );
    httpd_resp_send( req, (const char *)html_start, html_size );
    return ESP_OK;
}

//-----------------------------------------------------------------------------
static esp_err_t _ota_post_handler( httpd_req_t *req )
{
  char buf[256];
  httpd_resp_set_status( req, HTTPD_500 );    // Assume failure
  
  int ret, remaining = req->content_len;
  ESP_LOGI( TAG, "Receiving\n" );
  
  esp_ota_handle_t update_handle = 0 ;
  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
  const esp_partition_t *running          = esp_ota_get_running_partition();

  esp_err_t err = ESP_OK;

  if ( update_partition == NULL )
  {
    ESP_LOGE( TAG, "Uh oh, bad things\n" );
    goto return_failure;
  }

  ESP_LOGI( TAG, "Writing partition: type %d, subtype %d, offset 0x%08lx\n", update_partition-> type, update_partition->subtype, (long unsigned int)update_partition->address);
  ESP_LOGI( TAG, "Running partition: type %d, subtype %d, offset 0x%08lx\n", running->type,           running->subtype,          (long unsigned int)running->address);
  err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
  if (err != ESP_OK)
  {
      ESP_LOGE( TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
      goto return_failure;
  }
  while ( remaining > 0 )
  {
    // Read the data for the request
    if ( ( ret = httpd_req_recv( req, buf, MIN( remaining, sizeof( buf ) ) ) ) <= 0 )
    {
      if ( ret == HTTPD_SOCK_ERR_TIMEOUT )
      {
        // Retry receiving if timeout occurred
        continue;
      }

      goto return_failure;
    }
    
    size_t bytes_read = ret;
    
    remaining -= bytes_read;
    err = esp_ota_write( update_handle, buf, bytes_read);
    if (err != ESP_OK)
    {
      goto return_failure;
    }
  }

  ESP_LOGI( TAG, "Receiving done\n" );

  // End response
  if ( ( esp_ota_end(update_handle)                   == ESP_OK ) && 
       ( esp_ota_set_boot_partition(update_partition) == ESP_OK ) )
  {
    ESP_LOGI( TAG, "OTA Success?!\n Rebooting\n" );
    fflush( stdout );

    httpd_resp_set_status( req, HTTPD_200 );
    httpd_resp_send( req, NULL, 0 );
    
    vTaskDelay( 2000 / portTICK_PERIOD_MS);
    esp_restart();
    
    return ESP_OK;
  }
  ESP_LOGE( TAG, "OTA End failed (%s)!\n", esp_err_to_name(err));

return_failure:
  if ( update_handle )
  {
    esp_ota_abort(update_handle);
  }

  httpd_resp_set_status( req, HTTPD_500 );    // Assume failure
  httpd_resp_send( req, NULL, 0 );
  return ESP_FAIL;
}

static void* cjson_malloc(size_t size) 
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static void cjson_free(void* ptr) 
{
    heap_caps_free(ptr);
}

void setup_cjson_for_spiram() 
{
    cJSON_Hooks hooks = {
        .malloc_fn = cjson_malloc,
        .free_fn = cjson_free
    };
    cJSON_InitHooks(&hooks);
}

/* Function to start the file server */
esp_err_t start_file_server(const char *base_path)
{
    static struct file_server_data *server_data = NULL;

    // /* Validate file storage base path */
    // if (!base_path || strcmp(base_path, "/spiffs") != 0) {
    //     ESP_LOGE(TAG, "File server presently supports only '/spiffs' as base path");
    //     return ESP_ERR_INVALID_ARG;
    // }

    if (server_data) {
        ESP_LOGE(TAG, "File server already started");
        return ESP_ERR_INVALID_STATE;
    }

    //force cJon to use SPIRAM,to avoid eating up DMA capable memory required for SD and Wifi
    setup_cjson_for_spiram();

    /* Allocate memory for server data */
    server_data = (file_server_data *)calloc(1, sizeof(struct file_server_data));
    if (!server_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for server data");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(server_data->base_path, base_path,
            sizeof(server_data->base_path));
    strcat(server_data->base_path,"/");

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;

    /* Use the URI wildcard matching function in order to
     * allow the same handler to respond to multiple different
     * target URIs which match the wildcard scheme */
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server");
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start file server!");
        return ESP_FAIL;
    }
    /* URI handler for getting uploaded files */
    httpd_uri_t file_list = {
        .uri       = "/file_list",  
        .method    = HTTP_GET,
        .handler   = file_list_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &file_list);

    /* URI handler for getting uploaded files */
    httpd_uri_t configs_get = {
        .uri       = "/configs",  
        .method    = HTTP_GET,
        .handler   = configs_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &configs_get);

    /* URI handler for getting uploaded files */
    httpd_uri_t configs_post = {
        .uri       = "/configs",  
        .method    = HTTP_POST,
        .handler   = configs_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &configs_post);

    /* URI handler for deleting files from server */
    httpd_uri_t file_delete = {
        .uri       = "/delete",   // Match all URIs of type /delete/path/to/file
        .method    = HTTP_POST,
        .handler   = delete_post_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &file_delete);

    httpd_uri_t stream_get = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_get_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &stream_get);

    httpd_uri_t stream_info_get = {
        .uri       = "/stream_info",
        .method    = HTTP_GET,
        .handler   = stream_info_get_handler,
        .user_ctx  = server_data
    };
    httpd_register_uri_handler(server, &stream_info_get);

    static httpd_uri_t ota_post =
    {
      .uri       = "/ota",
      .method    = HTTP_POST,
      .handler   = _ota_post_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler( server, &ota_post );
    
    static httpd_uri_t ota_get =
    {
      .uri       = "/ota",
      .method    = HTTP_GET,
      .handler   = _ota_get_handler,
      .user_ctx  = NULL,
    };
    httpd_register_uri_handler( server, &ota_get );


    /* URI handler for getting uploaded files */
    httpd_uri_t file_download = {
        .uri       = "/*",  // Match all URIs of type /path/to/file
        .method    = HTTP_GET,
        .handler   = download_get_handler,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(server, &file_download);

    return ESP_OK;
}
