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

#include "wifi.h"

static const char ota_html_file[] = "\
<style>\n\
.progress {margin: 15px auto;  max-width: 500px;height: 30px;}\n\
.progress .progress__bar {\n\
  height: 100%; width: 0%; border-radius: 15px;\n\
  background: repeating-linear-gradient(135deg,#336ffc,#036ffc 15px,#1163cf 15px,#1163cf 30px); }\n\
 .status {font-weight: bold; font-size: 30px;};\n\
</style>\n\
<!--link rel=\"stylesheet\" href=\"https://cdnjs.cloudflare.com/ajax/libs/twitter-bootstrap/2.2.1/css/bootstrap.min.css\"-->\n\
<div class=\"well\" style=\"text-align: center;\">\n\
  <div class=\"btn\" onclick=\"file_sel.click();\"><i class=\"icon-upload\" style=\"padding-right: 5px;\"></i>Upload Firmware</div>\n\
  <div class=\"progress\"><div class=\"progress__bar\" id=\"progress\"></div></div>\n\
  <div class=\"status\" id=\"status_div\"></div>\n\
</div>\n\
<input type=\"file\" id=\"file_sel\" onchange=\"upload_file()\">\n\
<script>\n\
function upload_file() {\n\
  document.getElementById(\"status_div\").innerHTML = \"Upload in progress\";\n\
  let data = document.getElementById(\"file_sel\").files[0];\n\
  xhr = new XMLHttpRequest();\n\
  xhr.open(\"POST\", \"/ota\", true);\n\
  xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');\n\
  xhr.upload.addEventListener(\"progress\", function (event) {\n\
     if (event.lengthComputable) {\n\
    	 document.getElementById(\"progress\").style.width = (event.loaded / event.total) * 100 + \"%\";\n\
     }\n\
  });\n\
  xhr.onreadystatechange = function () {\n\
    if(xhr.readyState === XMLHttpRequest.DONE) {\n\
      var status = xhr.status;\n\
      if (status >= 200 && status < 400)\n\
      {\n\
        document.getElementById(\"status_div\").innerHTML = \"Upload accepted. Device will reboot.\";\n\
      } else {\n\
        document.getElementById(\"status_div\").innerHTML = \"Upload rejected!\";\n\
      }\n\
    }\n\
  };\n\
  xhr.send(data);\n\
  return false;\n\
}\n\
</script>";

/* Max length a file path can have on storage */
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)

/* Max size of an individual file. Make sure this
 * value is same as that set in upload_script.html */
#define MAX_FILE_SIZE   (200*1024) // 200 KB
#define MAX_FILE_SIZE_STR "200KB"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE  8192

struct file_server_data {
    /* Base path of file storage */
    char base_path[ESP_VFS_PATH_MAX + 1];

    /* Scratch buffer for temporary storage during file transfer */
    char scratch[SCRATCH_BUFSIZE];
};

static const char *TAG = "file_server";

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

static esp_err_t delete_post_handler(httpd_req_t *req)
{
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
    unlink(filepath);

    /* Redirect onto root to see the updated file list */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
    httpd_resp_set_hdr(req, "Connection", "close");
#endif
    httpd_resp_sendstr(req, "File deleted successfully");
    return ESP_OK;
}

static esp_err_t file_list_handler(httpd_req_t *req)
{
    char entrypath[FILE_PATH_MAX];
    char entrysize[16];
    
    const char *dirpath= ((struct file_server_data *)req->user_ctx)->base_path;
    DIR *dir = opendir(dirpath);

    struct dirent *entry;
    struct stat entry_stat;
    const char *entrytype;

    const size_t dirpath_len = strlen(dirpath);
    
    strlcpy(entrypath, dirpath, sizeof(entrypath));
    strcat(entrypath,"/");

	cJSON *root;
	root = cJSON_CreateArray();

    while ((entry = readdir(dir)) != NULL) {
        entrytype = (entry->d_type == DT_DIR ? "directory" : "file");

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
        cJSON_AddItemToArray(root,element);

 
    }
    closedir(dir);
    httpd_resp_set_type(req,"application/json");
    httpd_resp_sendstr(req,cJSON_Print(root));
    /* Send chunk of HTML file containing table entries with file name and size */
    cJSON_Delete(root);
    return ESP_OK;
}
static esp_err_t configs_handler(httpd_req_t *req)
{
    if(req->method == HTTP_GET){
        char channel_str[6]="";
        cJSON *root;
        root = cJSON_CreateObject();

        snprintf(channel_str, sizeof(channel_str), "%d", s_ground2air_config_packet.dataChannel.wifi_channel);
        cJSON_AddStringToObject(root,"channel",channel_str);
        cJSON_AddStringToObject(root,"default_dvr","false");

        /* Send chunk of HTML file containing table entries with file name and size */
        httpd_resp_set_type(req,"application/json");
        httpd_resp_sendstr(req, cJSON_Print(root));
        cJSON_Delete(root);

    }else{
        char buf[100]={0};
        httpd_req_recv(req,buf,req->content_len);
        cJSON *root = cJSON_Parse(buf);
        uint16_t channel = atoi(cJSON_GetStringValue(cJSON_GetObjectItem(root,"channel")));
        nvs_args_set("channel",channel);
        s_ground2air_config_packet.dataChannel.wifi_channel = channel;
        cJSON_Delete(root);
    }

    return ESP_OK;
}


//-----------------------------------------------------------------------------
static esp_err_t _ota_get_handler( httpd_req_t *req )
{
    httpd_resp_set_status( req, HTTPD_200 );
    httpd_resp_set_hdr( req, "Connection", "keep-alive" );
    httpd_resp_send( req, ota_html_file, strlen( ota_html_file ) );
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
