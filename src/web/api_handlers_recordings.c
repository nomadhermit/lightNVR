#define _XOPEN_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <glob.h>
#include <dirent.h>
#include <sqlite3.h>
#include <pthread.h>

#include "web/api_handlers_recordings.h"
#include "web/api_handlers_common.h"
#include "core/config.h"
#include "core/logger.h"
#include "database/database_manager.h"
#include "storage/storage_manager.h"
#include "web/request_response.h"

/* If MAX_CODEC_LENGTH is not defined, define it */
#ifndef MAX_CODEC_LENGTH
#define MAX_CODEC_LENGTH 32
#endif

extern config_t config;

/**
 * Get the total count of recordings matching given filters
 * This function performs a lightweight COUNT query against the database
 *
 * @param start_time    Start time filter (0 for no filter)
 * @param end_time      End time filter (0 for no filter)
 * @param stream_name   Stream name filter (NULL for all streams)
 *
 * @return Total number of matching recordings, or -1 on error
 */
int get_recording_count(time_t start_time, time_t end_time, const char *stream_name) {
    char sql[256] = {0};
    int sql_len = 0;

    /* Build SQL query with COUNT function */
    sql_len = snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM recordings WHERE 1=1");

    /* Add time filters if specified */
    if (start_time > 0 && end_time > 0) {
        sql_len += snprintf(sql + sql_len, sizeof(sql) - sql_len,
                 " AND start_time >= %lld AND end_time <= %lld",
                 (long long)start_time, (long long)end_time);
    }

    /* Add stream filter if specified */
    if (stream_name && stream_name[0] != '\0') {
        sql_len += snprintf(sql + sql_len, sizeof(sql) - sql_len,
                 " AND stream_name = '%s'", stream_name);
    }

    log_debug("Count query: %s", sql);

    sqlite3 *db = NULL;
    int rc = SQLITE_OK;

    /* Get path to database file */
    const char *db_path = config.db_path;
    if (!db_path) {
        log_error("Failed to get database file path from config");
        return -1;
    }

    /* Open database */
    rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        log_error("Failed to open database: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        log_error("Failed to prepare count statement: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    int count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    } else {
        log_error("Failed to get count from database: %s", sqlite3_errmsg(db));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    log_debug("Recording count: %d (filters: start=%lld, end=%lld, stream=%s)",
             count, (long long)start_time, (long long)end_time,
             stream_name ? stream_name : "all");

    return count;
}


/**
 * Get paginated recording metadata from the database
 * This function should be implemented to fetch only the specified page of results
 */
int get_recording_metadata_paginated(time_t start_time, time_t end_time, const char *stream_name,
                                   int offset, int limit, recording_metadata_t *recordings) {
    // This would be a new database function that supports pagination directly
    // It should only fetch the specified limit of records starting at offset

    // For now, implement a fallback using the existing function:

    // Allocate a temporary buffer for all records up to offset+limit
    recording_metadata_t *temp_buffer = malloc((offset + limit) * sizeof(recording_metadata_t));
    if (!temp_buffer) return -1;

    // Get recordings
    int total_count = get_recording_metadata(start_time, end_time, stream_name, temp_buffer, offset + limit);

    // Check for errors
    if (total_count < 0) {
        free(temp_buffer);
        return -1;
    }

    // Calculate how many records we can actually copy
    int available = total_count - offset;
    int to_copy = (available > 0) ? (available < limit ? available : limit) : 0;

    // Copy the requested page of records
    if (to_copy > 0) {
        memcpy(recordings, temp_buffer + offset, to_copy * sizeof(recording_metadata_t));
    }

    // Free temporary buffer
    free(temp_buffer);

    return to_copy;
}

/**
 * Handle GET request for recordings with pagination
 * Optimized for embedded devices with minimal memory usage and efficient resource handling
 */
void handle_get_recordings(const http_request_t *request, http_response_t *response) {
    // Get query parameters with bounds checking
    char date_str[32] = {0};
    char stream_name[MAX_STREAM_NAME] = {0};
    char page_str[16] = {0};
    char limit_str[16] = {0};
    time_t start_time = 0;
    time_t end_time = 0;
    int page = 1;
    int limit = 20;

    // Get date filter if provided
    if (get_query_param(request, "date", date_str, sizeof(date_str)) == 0) {
        // Parse date string efficiently (format: YYYY-MM-DD)
        int year = 0, month = 0, day = 0;
        if (sscanf(date_str, "%d-%d-%d", &year, &month, &day) == 3) {
            struct tm tm = {0};
            tm.tm_year = year - 1900;
            tm.tm_mon = month - 1;
            tm.tm_mday = day;

            // Set start time to beginning of day
            tm.tm_hour = 0;
            tm.tm_min = 0;
            tm.tm_sec = 0;
            start_time = mktime(&tm);

            // Set end time to end of day
            tm.tm_hour = 23;
            tm.tm_min = 59;
            tm.tm_sec = 59;
            end_time = mktime(&tm);
        } else {
            log_warn("Invalid date format: %s (expected YYYY-MM-DD)", date_str);
        }
    }

    // Get stream filter if provided
    get_query_param(request, "stream", stream_name, sizeof(stream_name));

    // If no stream name provided or "all" specified, set to NULL for all streams
    if (stream_name[0] == '\0' || strcmp(stream_name, "all") == 0) {
        stream_name[0] = '\0';
    }

    log_debug("Filtering recordings by stream: %s", stream_name[0] ? stream_name : "all streams");

    // Get pagination parameters if provided
    if (get_query_param(request, "page", page_str, sizeof(page_str)) == 0) {
        int parsed_page = atoi(page_str);
        if (parsed_page > 0) {
            page = parsed_page;
        }
    }

    if (get_query_param(request, "limit", limit_str, sizeof(limit_str)) == 0) {
        int parsed_limit = atoi(limit_str);
        if (parsed_limit > 0 && parsed_limit <= 100) {
            limit = parsed_limit;
        } else if (parsed_limit > 100) {
            // Cap the limit to prevent excessive memory usage
            limit = 100;
            log_info("Requested limit %d exceeds maximum, capped to 100", parsed_limit);
        }
    }

    log_info("Fetching recordings with pagination: page=%d, limit=%d", page, limit);

    // First, get total count using the optimized count function
    int total_count = get_recording_count(start_time, end_time, stream_name[0] ? stream_name : NULL);
    if (total_count < 0) {
        log_error("Failed to get recordings count from database");
        create_json_response(response, 500, "{\"error\": \"Failed to get recordings count\"}");
        return;
    }

    // Calculate pagination values
    int total_pages = (total_count + limit - 1) / limit; // Ceiling division
    if (total_pages == 0) total_pages = 1;

    // Validate page number
    if (page > total_pages) {
        page = total_pages;
    }

    // Calculate offset
    int offset = (page - 1) * limit;

    // Calculate actual limit for the current page
    int actual_limit = (offset + limit <= total_count) ? limit : (total_count - offset);
    if (actual_limit < 0) actual_limit = 0;

    // Only allocate memory and fetch recordings if there are any to fetch
    recording_metadata_t *recordings = NULL;
    if (actual_limit > 0) {
        // Allocate only what we need for this page using calloc
        recordings = (recording_metadata_t*)calloc(actual_limit, sizeof(recording_metadata_t));
        if (!recordings) {
            log_error("Failed to allocate memory for recordings");
            create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
            return;
        }

        // Get paginated recordings directly using the optimized function
        int count = get_recording_metadata_paginated(
            start_time, end_time,
            stream_name[0] ? stream_name : NULL,
            offset, actual_limit, recordings
        );

        if (count < 0) {
            free(recordings);
            log_error("Failed to get paginated recordings from database");
            create_json_response(response, 500, "{\"error\": \"Failed to get recordings\"}");
            return;
        }

        if (count != actual_limit) {
            log_warn("Expected %d records but got %d", actual_limit, count);
            actual_limit = count; // Adjust to actual count received
        }
    }

    // Create a buffer for the JSON response with a reasonable initial size
    // Base size + estimated size per recording + pagination info
    size_t json_capacity = 256 + (actual_limit * 512);
    char *json = (char*)malloc(json_capacity);
    if (!json) {
        if (recordings) free(recordings);
        log_error("Failed to allocate memory for JSON response");
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }

    // Start building the JSON response with pagination metadata
    int pos = snprintf(json, json_capacity,
                "{"
                "\"pagination\": {"
                "\"total\": %d,"
                "\"page\": %d,"
                "\"limit\": %d,"
                "\"pages\": %d"
                "},"
                "\"recordings\": [",
                total_count, page, limit, total_pages);

    // Add each recording to the JSON array
    for (int i = 0; i < actual_limit; i++) {
        // Format start and end times
        char start_time_str[32];
        char end_time_str[32];

        // Use localtime with error checking
        struct tm tm_start_buf, tm_end_buf;
        struct tm *tm_start = localtime_r(&recordings[i].start_time, &tm_start_buf);
        struct tm *tm_end = localtime_r(&recordings[i].end_time, &tm_end_buf);

        if (tm_start && tm_end) {
            strftime(start_time_str, sizeof(start_time_str), "%Y-%m-%d %H:%M:%S", tm_start);
            strftime(end_time_str, sizeof(end_time_str), "%Y-%m-%d %H:%M:%S", tm_end);
        } else {
            // Fallback if localtime fails
            snprintf(start_time_str, sizeof(start_time_str), "%lld", (long long)recordings[i].start_time);
            snprintf(end_time_str, sizeof(end_time_str), "%lld", (long long)recordings[i].end_time);
        }

        // Calculate duration in seconds
        int duration_sec = recordings[i].end_time - recordings[i].start_time;
        if (duration_sec < 0) duration_sec = 0;

        // Format duration as HH:MM:SS
        char duration_str[16];
        int hours = duration_sec / 3600;
        int minutes = (duration_sec % 3600) / 60;
        int seconds = duration_sec % 60;
        snprintf(duration_str, sizeof(duration_str), "%02d:%02d:%02d", hours, minutes, seconds);

        // Format size in human-readable format
        char size_str[16];
        if (recordings[i].size_bytes < 1024) {
            snprintf(size_str, sizeof(size_str), "%llu B", (unsigned long long)recordings[i].size_bytes);
        } else if (recordings[i].size_bytes < 1024 * 1024) {
            snprintf(size_str, sizeof(size_str), "%.1f KB", (float)recordings[i].size_bytes / 1024);
        } else if (recordings[i].size_bytes < 1024 * 1024 * 1024) {
            snprintf(size_str, sizeof(size_str), "%.1f MB", (float)recordings[i].size_bytes / (1024 * 1024));
        } else {
            snprintf(size_str, sizeof(size_str), "%.1f GB", (float)recordings[i].size_bytes / (1024 * 1024 * 1024));
        }

        // Check if we need more space
        if (pos + 512 >= json_capacity) {
            // Double the buffer size
            size_t new_capacity = json_capacity * 2;
            char *new_json = (char*)realloc(json, new_capacity);
            if (!new_json) {
                free(json);
                free(recordings);
                log_error("Failed to resize JSON buffer");
                create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
                return;
            }
            json = new_json;
            json_capacity = new_capacity;
        }

        // Add comma if not the first item
        if (i > 0) {
            json[pos++] = ',';
        }

        // Format recording JSON
        int written = snprintf(json + pos, json_capacity - pos,
                "{"
                "\"id\": %llu,"
                "\"stream\": \"%s\","
                "\"start_time\": \"%s\","
                "\"end_time\": \"%s\","
                "\"duration\": \"%s\","
                "\"size\": \"%s\","
                "\"path\": \"%s\","
                "\"width\": %d,"
                "\"height\": %d,"
                "\"fps\": %d,"
                "\"codec\": \"%s\","
                "\"complete\": %s"
                "}",
                (unsigned long long)recordings[i].id,
                recordings[i].stream_name,
                start_time_str,
                end_time_str,
                duration_str,
                size_str,
                recordings[i].file_path,
                recordings[i].width,
                recordings[i].height,
                recordings[i].fps,
                recordings[i].codec,
                recordings[i].is_complete ? "true" : "false");

        if (written > 0) {
            pos += written;
        }
    }

    // Check if we need more space for closing
    if (pos + 4 >= json_capacity) {
        size_t new_capacity = json_capacity + 8;
        char *new_json = (char*)realloc(json, new_capacity);
        if (!new_json) {
            free(json);
            if (recordings) free(recordings);
            log_error("Failed to resize JSON buffer for closing");
            create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
            return;
        }
        json = new_json;
        json_capacity = new_capacity;
    }

    // Close array and object
    pos += snprintf(json + pos, json_capacity - pos, "]}");

    // Create response
    create_json_response(response, 200, json);

    // Free resources
    free(json);
    if (recordings) free(recordings);

    log_info("Served recordings page %d of %d (limit: %d, total: %d)",
             page, total_pages, limit, total_count);
}

/**
 * Handle GET request for a specific recording
 */
void handle_get_recording(const http_request_t *request, http_response_t *response) {
    // Extract recording ID from the URL
    // URL format: /api/recordings/{id}
    const char *id_str = strrchr(request->path, '/');
    if (!id_str || strlen(id_str) <= 1) {
        create_json_response(response, 400, "{\"error\": \"Invalid recording ID\"}");
        return;
    }
    
    // Skip the '/'
    id_str++;
    
    // Convert ID to integer
    uint64_t id = strtoull(id_str, NULL, 10);
    if (id == 0) {
        create_json_response(response, 400, "{\"error\": \"Invalid recording ID\"}");
        return;
    }
    
    // Get recording metadata from database
    recording_metadata_t metadata;
    int result = get_recording_metadata_by_id(id, &metadata);
    
    if (result != 0) {
        create_json_response(response, 404, "{\"error\": \"Recording not found\"}");
        return;
    }
    
    // Format start and end times with error checking
    char start_time_str[32];
    char end_time_str[32];
    
    // Use localtime with error checking
    struct tm tm_start_buf, tm_end_buf;
    struct tm *tm_start = localtime_r(&metadata.start_time, &tm_start_buf);
    struct tm *tm_end = localtime_r(&metadata.end_time, &tm_end_buf);
    
    if (tm_start) {
        strftime(start_time_str, sizeof(start_time_str), "%Y-%m-%d %H:%M:%S", tm_start);
    } else {
        // Fallback if localtime fails
        snprintf(start_time_str, sizeof(start_time_str), "%lld", (long long)metadata.start_time);
    }
    
    if (tm_end) {
        strftime(end_time_str, sizeof(end_time_str), "%Y-%m-%d %H:%M:%S", tm_end);
    } else {
        // Fallback if localtime fails
        snprintf(end_time_str, sizeof(end_time_str), "%lld", (long long)metadata.end_time);
    }
    
    // Calculate duration in seconds
    int duration_sec = metadata.end_time - metadata.start_time;
    if (duration_sec < 0) duration_sec = 0;
    
    // Format duration as HH:MM:SS
    char duration_str[16];
    int hours = duration_sec / 3600;
    int minutes = (duration_sec % 3600) / 60;
    int seconds = duration_sec % 60;
    snprintf(duration_str, sizeof(duration_str), "%02d:%02d:%02d", hours, minutes, seconds);
    
    // Format size in human-readable format
    char size_str[16];
    if (metadata.size_bytes < 1024) {
        snprintf(size_str, sizeof(size_str), "%llu B", (unsigned long long)metadata.size_bytes);
    } else if (metadata.size_bytes < 1024 * 1024) {
        snprintf(size_str, sizeof(size_str), "%.1f KB", (float)metadata.size_bytes / 1024);
    } else if (metadata.size_bytes < 1024 * 1024 * 1024) {
        snprintf(size_str, sizeof(size_str), "%.1f MB", (float)metadata.size_bytes / (1024 * 1024));
    } else {
        snprintf(size_str, sizeof(size_str), "%.1f GB", (float)metadata.size_bytes / (1024 * 1024 * 1024));
    }
    
    // Create JSON response with dynamic allocation
    // Estimate the size needed for the JSON response
    size_t json_capacity = 512 + strlen(metadata.stream_name) + strlen(metadata.file_path) + 
                          strlen(start_time_str) + strlen(end_time_str) + 
                          strlen(duration_str) + strlen(size_str) + strlen(metadata.codec);
    
    char *json = (char*)malloc(json_capacity);
    if (!json) {
        log_error("Failed to allocate memory for JSON response");
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }
    
    int written = snprintf(json, json_capacity,
             "{"
             "\"id\": %llu,"
             "\"stream\": \"%s\","
             "\"start_time\": \"%s\","
             "\"end_time\": \"%s\","
             "\"duration\": \"%s\","
             "\"size\": \"%s\","
             "\"path\": \"%s\","
             "\"width\": %d,"
             "\"height\": %d,"
             "\"fps\": %d,"
             "\"codec\": \"%s\","
             "\"complete\": %s,"
             "\"url\": \"/api/recordings/%llu/download\""
             "}",
             (unsigned long long)metadata.id,
             metadata.stream_name,
             start_time_str,
             end_time_str,
             duration_str,
             size_str,
             metadata.file_path,
             metadata.width,
             metadata.height,
             metadata.fps,
             metadata.codec,
             metadata.is_complete ? "true" : "false",
             (unsigned long long)metadata.id);
    
    if (written >= json_capacity) {
        log_warn("JSON response was truncated (needed %d bytes, had %zu)", 
                written, json_capacity);
    }
    
    // Create response
    create_json_response(response, 200, json);
    
    // Free allocated memory
    free(json);
}

/**
 * Handle DELETE request to remove a recording
 */
void handle_delete_recording(const http_request_t *request, http_response_t *response) {
    // Extract recording ID from the URL
    // URL format: /api/recordings/{id}
    const char *id_str = strrchr(request->path, '/');
    if (!id_str || strlen(id_str) <= 1) {
        create_json_response(response, 400, "{\"error\": \"Invalid recording ID\"}");
        return;
    }

    // Skip the '/'
    id_str++;

    // Convert ID to integer
    uint64_t id = strtoull(id_str, NULL, 10);
    if (id == 0) {
        create_json_response(response, 400, "{\"error\": \"Invalid recording ID\"}");
        return;
    }

    log_info("Attempting to delete recording with ID: %llu", (unsigned long long)id);

    // Get recording metadata from database
    recording_metadata_t metadata;
    int result = get_recording_metadata_by_id(id, &metadata);

    if (result != 0) {
        log_error("Recording with ID %llu not found in database", (unsigned long long)id);
        create_json_response(response, 404, "{\"error\": \"Recording not found\"}");
        return;
    }

    log_info("Found recording in database: ID=%llu, Path=%s", (unsigned long long)id, metadata.file_path);

    // Determine directory where recording segments are stored
    char dir_path[MAX_PATH_LENGTH];
    const char *last_slash = strrchr(metadata.file_path, '/');
    if (last_slash) {
        size_t dir_len = last_slash - metadata.file_path + 1; // Include the slash
        strncpy(dir_path, metadata.file_path, dir_len);
        dir_path[dir_len] = '\0';
        log_info("Recording directory: %s", dir_path);

        // Delete all TS segment files in this directory using direct file operations
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and .. directories
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                
                // Check if file has .ts, .mp4, or .m3u8 extension
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && (strcasecmp(ext, ".ts") == 0 || 
                           strcasecmp(ext, ".mp4") == 0 || 
                           strcasecmp(ext, ".m3u8") == 0)) {
                    
                    char full_path[MAX_PATH_LENGTH];
                    snprintf(full_path, sizeof(full_path), "%s%s", dir_path, entry->d_name);
                    
                    // Close any open file handles before deletion
                    // This is a best-effort approach - we can't know all open handles
                    // but we can sync to ensure data is written to disk
                    sync();
                    
                    // Delete the file
                    if (unlink(full_path) != 0) {
                        log_warn("Failed to delete file: %s (error: %s)",
                                full_path, strerror(errno));
                    } else {
                        log_info("Successfully deleted file: %s", full_path);
                    }
                }
            }
            closedir(dir);
        } else {
            log_warn("Failed to open directory: %s (error: %s)",
                    dir_path, strerror(errno));
        }
    }

    // Explicitly try to delete the main file
    if (access(metadata.file_path, F_OK) == 0) {
        // Sync to ensure data is written to disk
        sync();
        
        if (unlink(metadata.file_path) != 0) {
            log_warn("Failed to delete recording file: %s (error: %s)",
                    metadata.file_path, strerror(errno));
            // Continue anyway - we'll still delete the metadata
        } else {
            log_info("Successfully deleted recording file: %s", metadata.file_path);
        }
    } else {
        log_warn("Recording file not found on disk: %s", metadata.file_path);
    }

    // Delete MP4 recordings if they exist - using direct file operations instead of glob
    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            // Skip . and .. directories
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            
            // Check if file is a recording*.mp4 file
            if (strncmp(entry->d_name, "recording", 9) == 0) {
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && strcasecmp(ext, ".mp4") == 0) {
                    char full_path[MAX_PATH_LENGTH];
                    snprintf(full_path, sizeof(full_path), "%s%s", dir_path, entry->d_name);
                    
                    // Sync to ensure data is written to disk
                    sync();
                    
                    // Delete the file
                    if (unlink(full_path) != 0) {
                        log_warn("Failed to delete MP4 file: %s (error: %s)",
                                full_path, strerror(errno));
                    } else {
                        log_info("Successfully deleted MP4 file: %s", full_path);
                    }
                }
            }
        }
        closedir(dir);
    }

    // Delete the recording metadata from database
    if (delete_recording_metadata(id) != 0) {
        log_error("Failed to delete recording metadata for ID: %llu", (unsigned long long)id);
        create_json_response(response, 500, "{\"error\": \"Failed to delete recording metadata\"}");
        return;
    }
    
    // Create success response with dynamic allocation
    char *json = malloc(256); // Allocate enough space for the response
    if (!json) {
        log_error("Failed to allocate memory for JSON response");
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }
    
    int written = snprintf(json, 256, 
                          "{\"success\": true, \"id\": %llu, \"message\": \"Recording deleted successfully\"}", 
                          (unsigned long long)id);
    
    if (written >= 256) {
        log_warn("JSON response was truncated (needed %d bytes, had 256)", written);
    }
    
    create_json_response(response, 200, json);
    
    // Free allocated memory
    free(json);
    
    log_info("Recording deleted successfully: ID=%llu, Path=%s", (unsigned long long)id, metadata.file_path);
}

/**
 * Handle GET request for debug database info
 */
void handle_get_debug_recordings(const http_request_t *request, http_response_t *response) {
    // Get recordings from database with no filters
    recording_metadata_t recordings[100]; // Limit to 100 recordings
    int count = get_recording_metadata(0, 0, NULL, recordings, 100);

    if (count < 0) {
        log_error("DEBUG: Failed to get recordings from database");
        create_json_response(response, 500, "{\"error\": \"Failed to get recordings\", \"count\": -1}");
        return;
    }

    // Cap count to prevent buffer overflows
    if (count > 100) {
        log_warn("DEBUG: Limiting recordings count from %d to 100", count);
        count = 100;
    }

    // Create a detailed debug response with dynamic allocation
    // Base size + estimated size per recording
    size_t json_capacity = 256 + (count * 512);
    char *debug_json = malloc(json_capacity);
    if (!debug_json) {
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }

    // Start building JSON
    int pos = snprintf(debug_json, json_capacity,
        "{\n"
        "  \"count\": %d,\n"
        "  \"recordings\": [\n", count);

    for (int i = 0; i < count; i++) {
        // Check if we need more space
        if (pos + 512 >= json_capacity) {
            // Double the buffer size
            size_t new_capacity = json_capacity * 2;
            char *new_json = (char*)realloc(debug_json, new_capacity);
            if (!new_json) {
                free(debug_json);
                log_error("Failed to resize debug JSON buffer");
                create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
                return;
            }
            debug_json = new_json;
            json_capacity = new_capacity;
        }

        // Add a comma between items (but not before the first item)
        if (i > 0) {
            pos += snprintf(debug_json + pos, json_capacity - pos, ",\n");
        }

        char path_status[32] = "unknown";
        struct stat st;
        if (stat(recordings[i].file_path, &st) == 0) {
            strncpy(path_status, "exists", sizeof(path_status) - 1);
            path_status[sizeof(path_status) - 1] = '\0';
        } else {
            strncpy(path_status, "missing", sizeof(path_status) - 1);
            path_status[sizeof(path_status) - 1] = '\0';
        }

        int written = snprintf(debug_json + pos, json_capacity - pos,
            "    {\n"
            "      \"id\": %llu,\n"
            "      \"stream\": \"%s\",\n"
            "      \"path\": \"%s\",\n"
            "      \"path_status\": \"%s\",\n"
            "      \"size\": %llu,\n"
            "      \"start_time\": %llu,\n"
            "      \"end_time\": %llu,\n"
            "      \"complete\": %s\n"
            "    }",
            (unsigned long long)recordings[i].id,
            recordings[i].stream_name,
            recordings[i].file_path,
            path_status,
            (unsigned long long)recordings[i].size_bytes,
            (unsigned long long)recordings[i].start_time,
            (unsigned long long)recordings[i].end_time,
            recordings[i].is_complete ? "true" : "false");
            
        if (written > 0) {
            pos += written;
        } else {
            log_error("Failed to format recording JSON for ID %llu", 
                     (unsigned long long)recordings[i].id);
            break;
        }
    }

    // Check if we need more space for closing
    if (pos + 8 >= json_capacity) {
        size_t new_capacity = json_capacity + 16;
        char *new_json = (char*)realloc(debug_json, new_capacity);
        if (!new_json) {
            free(debug_json);
            log_error("Failed to resize debug JSON buffer for closing");
            create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
            return;
        }
        debug_json = new_json;
        json_capacity = new_capacity;
    }

    // Close JSON
    pos += snprintf(debug_json + pos, json_capacity - pos, "\n  ]\n}");

    // Create response
    create_json_response(response, 200, debug_json);

    // Free resources
    free(debug_json);
}

/**
 * Handle GET request to download a recording - With fixed query parameter handling
 */
void handle_download_recording(const http_request_t *request, http_response_t *response) {
    // Extract recording ID from the URL
    const char *path = request->path;
    const char *prefix = "/api/recordings/download/";

    // Verify path starts with expected prefix
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        log_error("Invalid request path: %s", path);
        create_json_response(response, 400, "{\"error\": \"Invalid request path\"}");
        return;
    }

    // Extract the recording ID (everything after the prefix)
    const char *id_str = path + strlen(prefix);

    // Skip any leading slashes in the ID part
    while (*id_str == '/') {
        id_str++;
    }

    // Find query string if present and truncate
    char *id_str_copy = strdup(id_str);
    if (!id_str_copy) {
        log_error("Memory allocation failed for recording ID");
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }

    char *query_start = strchr(id_str_copy, '?');
    if (query_start) {
        *query_start = '\0'; // Truncate at query string
    }

    // Convert ID to integer
    uint64_t id = strtoull(id_str_copy, NULL, 10);
    if (id == 0) {
        log_error("Invalid recording ID: %s", id_str_copy);
        free(id_str_copy);
        create_json_response(response, 400, "{\"error\": \"Invalid recording ID\"}");
        return;
    }

    free(id_str_copy); // Free this as we don't need it anymore

    // Check for force download parameter - use the request's query params directly
    bool force_download = false;

    // Get 'download' parameter directly from request's query params
    char download_param[10] = {0};
    if (get_query_param(request, "download", download_param, sizeof(download_param)) == 0) {
        // Check if the parameter is set to "1" or "true"
        if (strcmp(download_param, "1") == 0 || strcmp(download_param, "true") == 0) {
            force_download = true;
            log_info("Force download requested for recording ID %llu (via query param)", (unsigned long long)id);
        }
    }

    // Get recording metadata from database
    recording_metadata_t metadata;
    int result = get_recording_metadata_by_id(id, &metadata);

    if (result != 0) {
        log_error("Recording with ID %llu not found in database", (unsigned long long)id);
        create_json_response(response, 404, "{\"error\": \"Recording not found\"}");
        return;
    }

    log_info("Found recording in database: ID=%llu, Path=%s, Download=%s",
             (unsigned long long)id, metadata.file_path, force_download ? "true" : "false");

    // Check if the file exists
    struct stat st;
    if (stat(metadata.file_path, &st) != 0) {
        log_error("Recording file not found on disk: %s (error: %s)",
                 metadata.file_path, strerror(errno));
        create_json_response(response, 404, "{\"error\": \"Recording file not found\"}");
        return;
    }

    // Determine if this is an MP4 file
    const char *ext = strrchr(metadata.file_path, '.');
    bool is_mp4 = (ext && strcasecmp(ext, ".mp4") == 0);

    // Generate a filename for download
    char filename[128];
    if (is_mp4) {
        snprintf(filename, sizeof(filename), "%s_%lld.mp4",
                metadata.stream_name, (long long)metadata.start_time);
    } else {
        // Use whatever extension the file has, or default to .mp4
        if (ext) {
            snprintf(filename, sizeof(filename), "%s_%lld%s",
                    metadata.stream_name, (long long)metadata.start_time, ext);
        } else {
            snprintf(filename, sizeof(filename), "%s_%lld.mp4",
                    metadata.stream_name, (long long)metadata.start_time);
        }
    }

    if (is_mp4 && !force_download) {
        // For MP4 files, serve with video/mp4 content type for playback
        log_info("Serving MP4 file with video/mp4 content type for playback: %s", metadata.file_path);

        // Set content type to video/mp4 for playback
        set_response_header(response, "Content-Type", "video/mp4");

        // Set content length
        char content_length[32];
        snprintf(content_length, sizeof(content_length), "%lld", (long long)st.st_size);
        set_response_header(response, "Content-Length", content_length);

        // Create file response with video/mp4 content type
        int result = create_file_response(response, 200, metadata.file_path, "video/mp4");
        if (result != 0) {
            log_error("Failed to create file response: %s", metadata.file_path);
            create_json_response(response, 500, "{\"error\": \"Failed to serve recording file\"}");
        }
    } else if (is_mp4) {
        // For MP4 files with forced download, use the serve_mp4_file function
        log_info("Serving MP4 file with attachment disposition for download: %s", metadata.file_path);
        serve_mp4_file(response, metadata.file_path, filename);
    } else {
        // For non-MP4 files, use the direct download approach
        serve_direct_download(response, id, &metadata);
    }
}
