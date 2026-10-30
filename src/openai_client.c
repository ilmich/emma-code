#include "cjson/cJSON.h"
#include "openai_client.h"
#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

// Define constants
#define MAX_JSON_PAYLOAD_SIZE 4096
#define MAX_RESPONSE_BODY_SIZE 8192
#define OPENAI_API_URL "/v1/chat/completions"

/**
 * @brief Constructs the JSON request body for the chat completion API.
 *
 * @param config The client configuration.
 * @param messages Array of messages.
 * @param message_count Number of messages.
 * @param model_name Model to use.
 * @param payload Buffer to write the JSON into.
 * @param payload_size Size of the buffer.
 * @return size_t The length of the written JSON string.
 */
static char* build_json_payload(
    const openai_config_t *config,
    const openai_message_t *messages,
    size_t message_count) 
{
    // Create the root JSON object
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    if (config->model_name == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    // Add model name
    if (cJSON_AddStringToObject(root, "model", config->model_name) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    // Add sampling temperature
    if (cJSON_AddNumberToObject(root, "temperature", config->temperature) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    // Create and populate messages array
    cJSON *messages_array = cJSON_CreateArray();
    if (messages_array == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    for (size_t i = 0; i < message_count; ++i) {
        if (messages[i].role == NULL || messages[i].content == NULL) {
            cJSON_Delete(messages_array);
            cJSON_Delete(root);
            return NULL;
        }
        cJSON *message_item = cJSON_CreateObject();
        if (message_item == NULL) {
            cJSON_Delete(messages_array);
            cJSON_Delete(root);
            return NULL;
        }
        if (cJSON_AddStringToObject(message_item, "role", messages[i].role) == NULL ||
            cJSON_AddStringToObject(message_item, "content", messages[i].content) == NULL) {
            cJSON_Delete(message_item);
            cJSON_Delete(messages_array);
            cJSON_Delete(root);
            return NULL;
        }
        if (cJSON_AddItemToArray(messages_array, message_item) == 0) {
            cJSON_Delete(message_item);
            cJSON_Delete(messages_array);
            cJSON_Delete(root);
            return NULL;
        }
    }

    if (cJSON_AddItemToObject(root, "messages", messages_array) == 0) {
        cJSON_Delete(messages_array); // Not attached: free it separately
        cJSON_Delete(root);
        return NULL;
    }

    // Serialize the JSON object to a string
    char *json_string = cJSON_Print(root);
    if (json_string == NULL) {
        fprintf(stderr, "Error: Failed to serialize JSON payload.\n");
    }
    
    // Clean up the cJSON structure
    cJSON_Delete(root);
    
    return json_string;
}
/**
 * @brief Parses the response body to extract the assistant's content using cJSON.
 *
 * @param body The raw JSON response body.
 * @param response Pointer to the response structure to populate.
 * @return 0 on success, 1 on failure (parsing error).
 */
static int parse_response(const char *body, openai_response_t *response) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        fprintf(stderr, "Error: Failed to parse JSON response.\n");
        return 1;
    }

    // Navigate to choices array
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        fprintf(stderr, "Error: 'choices' array missing or empty in response.\n");
        cJSON_Delete(root);
        return 1;
    }

    // Navigate to the first choice
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    
    // Navigate to message object
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!cJSON_IsObject(message)) {
        fprintf(stderr, "Error: 'message' object missing in choice.\n");
        cJSON_Delete(root);
        return 1;
    }

    // Navigate to content field
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (!cJSON_IsString(content)) {
        fprintf(stderr, "Error: 'content' string missing in message.\n");
        cJSON_Delete(root);
        return 1;
    }

    // Extract and copy the content string
    const char *content_str = cJSON_GetStringValue(content);
    size_t length = strlen(content_str);

    // Allocate memory for the content
    char *content_copy = (char *)malloc(length + 1);
    if (!content_copy) {
        perror("Failed to allocate memory for response content");
        cJSON_Delete(root);
        return 1;
    }

    memcpy(content_copy, content_str, length + 1);
    response->content = content_copy;

    // Clean up
    cJSON_Delete(root);
    return 0;
}

int openai_chat_completion(
    const openai_config_t *config,
    const openai_message_t *messages,
    size_t message_count,
    openai_response_t *response) 
{
    // 1. Construct the full URL
    // For simplicity, we assume config->base_url is just the root (e.g., "https://api.openai.com")
    // and we append the endpoint.
    char url_buffer[512];
    char auth_header[512];
    char *headers[2];
    size_t headers_count = 0;
    int n;

    // The API key is optional: only add the Authorization header when one
    // is configured.
    headers[headers_count++] = "Content-Type: application/json";
    if (config->api_key != NULL) {
        n = snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", config->api_key);
        if (n < 0 || (size_t)n >= sizeof(auth_header)) {
            fprintf(stderr, "Error: auth_header buffer truncated\n");
            return 1;
        }
        headers[headers_count++] = auth_header;
    }
    n = snprintf(url_buffer, sizeof(url_buffer), "%s%s", config->base_url, OPENAI_API_URL);
    if (n < 0 || (size_t)n >= sizeof(url_buffer)) {
        fprintf(stderr, "Error: url_buffer truncated\n");
        return 1;
    }

    // 2. Build the JSON payload
    char *payload = build_json_payload(
        config, messages, message_count
    );
    if (payload == NULL) {
        fprintf(stderr, "Error: Failed to build JSON payload.\n");
        return 1;
    }

    // 3. Prepare HTTP Request
    struct http_request_t req = {
        .url = url_buffer,
        .method = "POST",
        .body = payload,
        .headers = headers,
        .headers_count = headers_count,
        .callback = NULL, // Non-streaming request
        .user_data = NULL
    };

    // 4. Execute Request
    struct http_response_t resp = {0};
    int result = http_request(&req, &resp);

    if (result != 0) {
        fprintf(stderr, "HTTP Request failed.\n");
	http_response_free(&resp);
	free(payload);	
        return 1;
    }

    // 5. Handle Response
    if (resp.status_code != 200) {
        fprintf(stderr, "API Error: Received status code %d. Body: %s\n", resp.status_code, resp.body ? resp.body : "No body");
        // Free response body before returning
        http_response_free(&resp);
	free(payload);
        return 1;
    }

    // 6. Parse the response
    int parse_status = parse_response(resp.body, response);

    // 7. Cleanup
    http_response_free(&resp);
    free(payload);
    
    return parse_status;
}

void openai_response_free(openai_response_t *response) {
    if (response && response->content) {
        free(response->content);
        response->content = NULL;
    }
}
