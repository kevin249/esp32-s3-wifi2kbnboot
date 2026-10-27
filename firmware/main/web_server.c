#include "web_server.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hid_control.h"
#include "sdkconfig.h"
#include "storage.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern const unsigned char page_start[] asm("_binary_index_html_start");
extern const unsigned char page_end[] asm("_binary_index_html_end");
static SemaphoreHandle_t async_slots;
static esp_err_t json_reply(httpd_req_t *r, int code, cJSON *body) {
  const char *status = code == 200   ? "200 OK"
                       : code == 202 ? "202 Accepted"
                       : code == 400 ? "400 Bad Request"
                       : code == 401 ? "401 Unauthorized"
                       : code == 404 ? "404 Not Found"
                       : code == 409 ? "409 Conflict"
                       : code == 413 ? "413 Payload Too Large"
                                     : "500 Internal Server Error";
  httpd_resp_set_status(r, status);
  httpd_resp_set_type(r, "application/json; charset=utf-8");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  char *text = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (!text)
    return ESP_ERR_NO_MEM;
  esp_err_t err = httpd_resp_sendstr(r, text);
  free(text);
  return err;
}
static esp_err_t error(httpd_req_t *r, int code, const char *message) {
  cJSON *body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "error", message);
  return json_reply(r, code, body);
}
static bool authorized(httpd_req_t *r) {
  char actual[256], expected[256];
  snprintf(expected, sizeof(expected), "Bearer %s", CONFIG_W2B_TOKEN);
  if (httpd_req_get_hdr_value_str(r, "Authorization", actual, sizeof(actual)) ==
          ESP_OK &&
      !strcmp(actual, expected))
    return true;
  error(r, 401, "访问令牌无效");
  return false;
}
static int hex(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
static bool query(httpd_req_t *r, const char *key, char *out, size_t capacity) {
  char raw[768], value[640];
  if (httpd_req_get_url_query_str(r, raw, sizeof(raw)) != ESP_OK ||
      httpd_query_key_value(raw, key, value, sizeof(value)) != ESP_OK)
    return false;
  size_t used = 0;
  for (size_t i = 0; value[i]; ++i) {
    unsigned char c = value[i];
    if (c == '%') {
      if (!value[i + 1] || !value[i + 2] || hex(value[i + 1]) < 0 ||
          hex(value[i + 2]) < 0)
        return false;
      c = hex(value[i + 1]) * 16 + hex(value[i + 2]);
      i += 2;
    } else if (c == '+')
      c = ' ';
    if (!c || used + 1 >= capacity)
      return false;
    out[used++] = c;
  }
  out[used] = 0;
  return true;
}
static bool iso_entry(const char *name) {
  size_t n = strlen(name);
  return (n > 4 && !strcmp(name + n - 4, ".iso")) ||
         (n > 8 && !strcmp(name + n - 8, ".iso.000"));
}
static esp_err_t manifest(httpd_req_t *r) {
  storage_public_t s;
  storage_public(&s);
  cJSON *body = cJSON_CreateObject(), *transfer = cJSON_CreateObject();
  cJSON_AddBoolToObject(body, "sd_ready", s.media.sd_ready);
  cJSON_AddBoolToObject(body, "switching", s.switching);
  cJSON_AddStringToObject(body, "last_error", esp_err_to_name(s.last_error));
#ifdef CONFIG_W2B_BIOS_DISK_ONLY
  cJSON_AddBoolToObject(body, "bios_only", true);
#else
  cJSON_AddBoolToObject(body, "bios_only", false);
#endif
  if (s.media.iso_present) {
    cJSON *iso = cJSON_CreateObject();
    cJSON_AddStringToObject(iso, "name", s.media.iso_name);
    cJSON_AddNumberToObject(iso, "size", s.media.iso_size);
    cJSON_AddItemToObject(body, "iso", iso);
  } else
    cJSON_AddNullToObject(body, "iso");
  cJSON_AddBoolToObject(transfer, "ready", s.media.files_ready);
  cJSON_AddBoolToObject(transfer, "owned", s.media.host_owner);
  cJSON_AddStringToObject(transfer, "mode",
                          s.media.host_owner ? "host" : "server");
  cJSON_AddNumberToObject(transfer, "size", s.media.file_size);
  cJSON_AddItemToObject(body, "transfer", transfer);
  return json_reply(r, 200, body);
}
static esp_err_t list_files(httpd_req_t *r, bool iso) {
  if (!sd_media_web_begin(!iso))
    return error(r, 409, "请先安全弹出并取回文件盘，或检查SD卡");
  cJSON *body = cJSON_CreateObject(), *items = cJSON_CreateArray();
  sd_media_lock();
  DIR *directory = opendir(iso ? "/sd/iso" : "/files");
  if (directory) {
    struct dirent *entry;
    unsigned count = 0;
    while ((entry = readdir(directory)) != NULL && count < 256) {
      if (!media_name_valid(entry->d_name) ||
          (iso && !iso_entry(entry->d_name)))
        continue;
      char path[256];
      snprintf(path, sizeof(path), "%s/%.*s", iso ? "/sd/iso" : "/files",
               W2B_NAME_MAX, entry->d_name);
      struct stat info;
      if (stat(path, &info) != 0 || !S_ISREG(info.st_mode))
        continue;
      if (iso) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entry->d_name);
        cJSON_AddStringToObject(item, "path", entry->d_name);
        cJSON_AddBoolToObject(item, "directory", false);
        cJSON_AddItemToArray(items, item);
      } else
        cJSON_AddItemToArray(items, cJSON_CreateString(entry->d_name));
      ++count;
    }
    closedir(directory);
  }
  sd_media_unlock();
  sd_media_web_end();
  if (!directory) {
    cJSON_Delete(body);
    cJSON_Delete(items);
    return error(r, 409, "文件系统不可用");
  }
  cJSON_AddItemToObject(body, iso ? "items" : "files", items);
  if (iso)
    cJSON_AddStringToObject(body, "path", "");
  return json_reply(r, 200, body);
}
static esp_err_t file_transfer(httpd_req_t *r) {
  bool iso = strstr(r->uri, "/api/iso/") != NULL,
       upload = r->method == HTTP_POST;
  char name[W2B_NAME_MAX + 1];
  if (!query(r, "name", name, sizeof(name)) || !media_name_valid(name))
    return error(r, 400, "文件名无效");
  if (!sd_media_web_begin(!iso))
    return error(r, 409, "请先安全弹出并取回文件盘，或检查SD卡");
  char path[256], temporary[128];
  snprintf(path, sizeof(path), "%s/%s", iso ? "/sd/iso" : "/files", name);
  snprintf(temporary, sizeof(temporary), "%s/.upload-%08lx",
           iso ? "/sd/iso" : "/files", (unsigned long)esp_random());
  uint8_t *buffer = malloc(8192);
  FILE *file = NULL;
  esp_err_t err = ESP_OK;
  if (!buffer) {
    sd_media_web_end();
    return error(r, 500, "内存不足");
  }
  if (upload) {
    if (!r->content_len ||
        r->content_len > (size_t)CONFIG_W2B_UPLOAD_MIB * 1024 * 1024) {
      err = error(r, 413, "文件为空或过大；大ISO请使用分卷");
      goto done;
    }
    sd_media_lock();
    struct stat st;
    bool exists = stat(path, &st) == 0;
    if (!exists)
      file = fopen(temporary, "wb");
    sd_media_unlock();
    if (exists || !file) {
      err = error(r, 409, "文件已存在或无法创建；请先删除旧文件");
      goto done;
    }
    size_t remaining = r->content_len;
    while (remaining) {
      int n = httpd_req_recv(r, (char *)buffer,
                             remaining < 8192 ? remaining : 8192);
      if (n <= 0) {
        err = ESP_FAIL;
        break;
      }
      sd_media_lock();
      size_t written = fwrite(buffer, 1, n, file);
      sd_media_unlock();
      if (written != (size_t)n) {
        err = ESP_FAIL;
        break;
      }
      remaining -= n;
    }
    sd_media_lock();
    if (fflush(file) != 0 || fsync(fileno(file)) != 0)
      err = ESP_FAIL;
    if (fclose(file) != 0)
      err = ESP_FAIL;
    file = NULL;
    if (err == ESP_OK) {
      struct stat st;
      if (stat(path, &st) == 0 || rename(temporary, path) != 0)
        err = ESP_FAIL;
    }
    if (err != ESP_OK)
      unlink(temporary);
    sd_media_unlock();
    if (err == ESP_OK) {
      cJSON *body = cJSON_CreateObject();
      cJSON_AddBoolToObject(body, "saved", true);
      err = json_reply(r, 200, body);
    } else
      err = error(r, 500, "传输或写入失败；未发布新文件");
  } else {
    sd_media_lock();
    file = fopen(path, "rb");
    sd_media_unlock();
    if (!file) {
      err = error(r, 404, "文件不存在");
      goto done;
    }
    httpd_resp_set_type(r, "application/octet-stream");
    for (;;) {
      sd_media_lock();
      size_t n = fread(buffer, 1, 8192, file);
      bool failed = ferror(file);
      sd_media_unlock();
      if (failed) {
        err = ESP_FAIL;
        break;
      }
      if (!n)
        break;
      if (httpd_resp_send_chunk(r, (char *)buffer, n) != ESP_OK) {
        err = ESP_FAIL;
        break;
      }
    }
    if (err == ESP_OK)
      err = httpd_resp_send_chunk(r, NULL, 0);
  }
done:
  if (file) {
    sd_media_lock();
    fclose(file);
    if (upload)
      unlink(temporary);
    sd_media_unlock();
  }
  free(buffer);
  sd_media_web_end();
  return err;
}
typedef struct {
  httpd_req_t *request;
} async_request_t;
static void transfer_task(void *argument) {
  async_request_t *job = argument;
  httpd_resp_set_hdr(job->request, "Connection", "close");
  file_transfer(job->request);
  int socket = httpd_req_to_sockfd(job->request);
  httpd_handle_t server = job->request->handle;
  httpd_req_async_handler_complete(job->request);
  httpd_sess_trigger_close(server, socket);
  free(job);
  xSemaphoreGive(async_slots);
  vTaskDelete(NULL);
}
static esp_err_t async_transfer(httpd_req_t *r) {
  if (xSemaphoreTake(async_slots, 0) != pdTRUE)
    return error(r, 409, "已有文件传输，请稍后重试");
  async_request_t *job = malloc(sizeof(*job));
  if (!job) {
    xSemaphoreGive(async_slots);
    return error(r, 500, "内存不足");
  }
  if (httpd_req_async_handler_begin(r, &job->request) != ESP_OK) {
    free(job);
    xSemaphoreGive(async_slots);
    return ESP_FAIL;
  }
  if (xTaskCreate(transfer_task, "网页文件传输", 8192, job, 3, NULL) !=
      pdPASS) {
    error(job->request, 500, "无法启动传输");
    httpd_req_async_handler_complete(job->request);
    free(job);
    xSemaphoreGive(async_slots);
  }
  return ESP_OK;
}
static cJSON *read_json(httpd_req_t *r) {
  if (!r->content_len || r->content_len > 512)
    return NULL;
  char data[513];
  size_t used = 0;
  while (used < r->content_len) {
    int n = httpd_req_recv(r, data + used, r->content_len - used);
    if (n <= 0)
      return NULL;
    used += n;
  }
  data[used] = 0;
  return cJSON_Parse(data);
}
static esp_err_t api(httpd_req_t *r) {
  if (!authorized(r))
    return ESP_OK;
  char route[64];
  snprintf(route, sizeof(route), "%s", r->uri);
  char *q = strchr(route, '?');
  if (q)
    *q = 0;
  if (!strcmp(route, "/api/manifest"))
    return manifest(r);
  if (!strcmp(route, "/api/library"))
    return list_files(r, true);
  if (!strcmp(route, "/api/transfer/files"))
    return list_files(r, false);
  if (!strcmp(route, "/api/transfer/upload") ||
      !strcmp(route, "/api/transfer/download") ||
      !strcmp(route, "/api/iso/upload"))
    return async_transfer(r);
  if (r->method != HTTP_POST)
    return error(r, 404, "接口不存在");
  if (strcmp(route, "/api/mount") && r->content_len) {
    cJSON *ignored = read_json(r);
    if (!ignored)
      return error(r, 400, "请求体无效");
    cJSON_Delete(ignored);
  }
  esp_err_t result = ESP_ERR_NOT_SUPPORTED;
  if (!strcmp(route, "/api/transfer/take"))
    result = storage_request_owner(false);
  else if (!strcmp(route, "/api/transfer/attach"))
    result = storage_request_owner(true);
  else if (!strcmp(route, "/api/eject"))
    result = storage_request_iso("");
  else if (!strcmp(route, "/api/mount")) {
    cJSON *body = read_json(r),
          *path = cJSON_GetObjectItemCaseSensitive(body, "path");
    result = cJSON_IsString(path) ? storage_request_iso(path->valuestring)
                                  : ESP_ERR_INVALID_ARG;
    cJSON_Delete(body);
  } else if (!strcmp(route, "/api/transfer/delete") ||
             !strcmp(route, "/api/iso/delete")) {
    bool iso = strstr(route, "/iso/") != NULL;
    char name[W2B_NAME_MAX + 1];
    if (!query(r, "name", name, sizeof(name)) || !media_name_valid(name))
      return error(r, 400, "文件名无效");
    storage_public_t state;
    storage_public(&state);
    if (iso && state.media.iso_present)
      return error(r, 409, "请先弹出ISO，再删除分卷或镜像");
    if (!sd_media_web_begin(!iso))
      return error(r, 409, "请先取回文件盘");
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", iso ? "/sd/iso" : "/files", name);
    sd_media_lock();
    result = unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    sd_media_unlock();
    sd_media_web_end();
  }
  if (result != ESP_OK)
    return error(r, result == ESP_ERR_INVALID_ARG ? 400 : 409,
                 "操作未提交：请等待交接完成，切换ISO前先取回文件盘");
  cJSON *body = cJSON_CreateObject();
  cJSON_AddBoolToObject(body, "queued", true);
  return json_reply(r, 202, body);
}
static esp_err_t page(httpd_req_t *r) {
  httpd_resp_set_type(r, "text/html; charset=utf-8");
  return httpd_resp_send(r, (const char *)page_start, page_end - page_start);
}
void web_server_init(void) {
  async_slots = xSemaphoreCreateCounting(2, 2);
  configASSERT(async_slots);
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;
  config.max_uri_handlers = 24;
  config.max_open_sockets = 6;
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;
  ESP_ERROR_CHECK(httpd_start(&server, &config));
  hid_control_register(server);
  httpd_uri_t home = {.uri = "/", .method = HTTP_GET, .handler = page};
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &home));
  httpd_uri_t get = {.uri = "/api/*", .method = HTTP_GET, .handler = api};
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get));
  httpd_uri_t post = {.uri = "/api/*", .method = HTTP_POST, .handler = api};
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &post));
}
