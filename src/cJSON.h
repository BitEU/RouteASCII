/*
  cJSON - Minimal header for RouteASCII
  Compatible with the stripped-down implementation in cjson.c.
*/

#ifndef CJSON_H
#define CJSON_H

#ifdef __cplusplus
extern "C" {
#endif

/* cJSON Types */
#define cJSON_Invalid 0
#define cJSON_False  (1 << 0)
#define cJSON_True   (1 << 1)
#define cJSON_NULL   (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array  (1 << 5)
#define cJSON_Object (1 << 6)

/* The cJSON structure used by this project. */
typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;

    int type;

    char *valuestring;
    int valueint;
    double valuedouble;

    char *string;
} cJSON;

cJSON *cJSON_Parse(const char *value);
void cJSON_Delete(cJSON *item);

cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
int cJSON_GetArraySize(const cJSON *array);

int cJSON_IsArray(const cJSON *item);
int cJSON_IsString(const cJSON *item);
int cJSON_IsNumber(const cJSON *item);
int cJSON_IsObject(const cJSON *item);

char *cJSON_GetStringValue(const cJSON *item);
double cJSON_GetNumberValue(const cJSON *item);

#ifdef __cplusplus
}
#endif

#endif /* CJSON_H */
