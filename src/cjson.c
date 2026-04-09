/*
  cJSON - Minimal implementation for RouteASCII
  Based on cJSON by Dave Gamble (MIT License)
  https://github.com/DaveGamble/cJSON
  
  For production use, replace with the full cJSON library from GitHub.
  This is a stripped-down version supporting only the features we need.
*/

#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <float.h>
#include <limits.h>
#include <ctype.h>

static const char *skip_whitespace(const char *in)
{
    while (in && *in && (unsigned char)*in <= ' ') in++;
    return in;
}

static const char *parse_string(cJSON *item, const char *str)
{
    const char *ptr = str + 1;
    char *out;
    int len = 0;
    const char *ptr2 = ptr;
    
    if (*str != '\"') return NULL;
    
    while (*ptr2 != '\"' && *ptr2) {
        if (*ptr2 == '\\') ptr2++;
        ptr2++;
        len++;
    }
    
    out = (char*)malloc(len + 1);
    if (!out) return NULL;
    
    ptr2 = ptr;
    char *out2 = out;
    while (*ptr2 != '\"' && *ptr2) {
        if (*ptr2 != '\\') {
            *out2++ = *ptr2++;
        } else {
            ptr2++;
            switch (*ptr2) {
                case 'b': *out2++ = '\b'; break;
                case 'f': *out2++ = '\f'; break;
                case 'n': *out2++ = '\n'; break;
                case 'r': *out2++ = '\r'; break;
                case 't': *out2++ = '\t'; break;
                case '\"': *out2++ = '\"'; break;
                case '\\': *out2++ = '\\'; break;
                case '/': *out2++ = '/'; break;
                case 'u': /* skip unicode for now */
                    ptr2 += 4;
                    *out2++ = '?';
                    break;
                default: *out2++ = *ptr2; break;
            }
            ptr2++;
        }
    }
    *out2 = '\0';
    
    item->valuestring = out;
    item->type = cJSON_String;
    return ptr2 + 1;
}

static const char *parse_number(cJSON *item, const char *num)
{
    double n = 0, sign = 1, scale = 0;
    int subscale = 0, signsubscale = 1;
    
    if (*num == '-') { sign = -1; num++; }
    if (*num == '0') num++;
    if (*num >= '1' && *num <= '9') {
        do { n = (n * 10.0) + (*num++ - '0'); } while (*num >= '0' && *num <= '9');
    }
    if (*num == '.' && num[1] >= '0' && num[1] <= '9') {
        num++;
        do { n = (n * 10.0) + (*num++ - '0'); scale--; } while (*num >= '0' && *num <= '9');
    }
    if (*num == 'e' || *num == 'E') {
        num++;
        if (*num == '+') num++;
        else if (*num == '-') { signsubscale = -1; num++; }
        while (*num >= '0' && *num <= '9') {
            subscale = (subscale * 10) + (*num++ - '0');
        }
    }
    
    n = sign * n * pow(10.0, (scale + subscale * signsubscale));
    item->valuedouble = n;
    item->valueint = (int)n;
    item->type = cJSON_Number;
    return num;
}

static const char *parse_value(cJSON *item, const char *value);

static const char *parse_array(cJSON *item, const char *value)
{
    cJSON *child;
    if (*value != '[') return NULL;
    item->type = cJSON_Array;
    value = skip_whitespace(value + 1);
    if (*value == ']') return value + 1;
    
    item->child = child = (cJSON*)calloc(1, sizeof(cJSON));
    if (!child) return NULL;
    
    value = skip_whitespace(parse_value(child, skip_whitespace(value)));
    if (!value) return NULL;
    
    while (*value == ',') {
        cJSON *new_item = (cJSON*)calloc(1, sizeof(cJSON));
        if (!new_item) return NULL;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip_whitespace(parse_value(child, skip_whitespace(value + 1)));
        if (!value) return NULL;
    }
    
    if (*value == ']') return value + 1;
    return NULL;
}

static const char *parse_object(cJSON *item, const char *value)
{
    cJSON *child;
    if (*value != '{') return NULL;
    item->type = cJSON_Object;
    value = skip_whitespace(value + 1);
    if (*value == '}') return value + 1;
    
    item->child = child = (cJSON*)calloc(1, sizeof(cJSON));
    if (!child) return NULL;
    
    value = skip_whitespace(parse_string(child, skip_whitespace(value)));
    if (!value) return NULL;
    child->string = child->valuestring;
    child->valuestring = NULL;
    child->type = cJSON_Invalid;
    
    if (*value != ':') return NULL;
    value = skip_whitespace(parse_value(child, skip_whitespace(value + 1)));
    if (!value) return NULL;
    
    while (*value == ',') {
        cJSON *new_item = (cJSON*)calloc(1, sizeof(cJSON));
        if (!new_item) return NULL;
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip_whitespace(parse_string(child, skip_whitespace(value + 1)));
        if (!value) return NULL;
        child->string = child->valuestring;
        child->valuestring = NULL;
        child->type = cJSON_Invalid;
        
        if (*value != ':') return NULL;
        value = skip_whitespace(parse_value(child, skip_whitespace(value + 1)));
        if (!value) return NULL;
    }
    
    if (*value == '}') return value + 1;
    return NULL;
}

static const char *parse_value(cJSON *item, const char *value)
{
    if (!value) return NULL;
    if (!strncmp(value, "null", 4))  { item->type = cJSON_NULL; return value + 4; }
    if (!strncmp(value, "false", 5)) { item->type = cJSON_False; return value + 5; }
    if (!strncmp(value, "true", 4))  { item->type = cJSON_True; item->valueint = 1; return value + 4; }
    if (*value == '\"') return parse_string(item, value);
    if (*value == '-' || (*value >= '0' && *value <= '9')) return parse_number(item, value);
    if (*value == '[') return parse_array(item, value);
    if (*value == '{') return parse_object(item, value);
    return NULL;
}

cJSON *cJSON_Parse(const char *value)
{
    cJSON *c = (cJSON*)calloc(1, sizeof(cJSON));
    if (!c) return NULL;
    if (!parse_value(c, skip_whitespace(value))) {
        cJSON_Delete(c);
        return NULL;
    }
    return c;
}

void cJSON_Delete(cJSON *c)
{
    cJSON *next;
    while (c) {
        next = c->next;
        if (c->child) cJSON_Delete(c->child);
        free(c->valuestring);
        free(c->string);
        free(c);
        c = next;
    }
}

cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string)
{
    if (!object || !string) return NULL;
    cJSON *c = object->child;
    while (c) {
        if (c->string && strcmp(c->string, string) == 0) return c;
        c = c->next;
    }
    return NULL;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (!array) return NULL;
    cJSON *c = array->child;
    while (c && index > 0) { c = c->next; index--; }
    return c;
}

int cJSON_GetArraySize(const cJSON *array)
{
    if (!array) return 0;
    cJSON *c = array->child;
    int i = 0;
    while (c) { i++; c = c->next; }
    return i;
}

int cJSON_IsArray(const cJSON *item)  { return item ? (item->type & cJSON_Array)  : 0; }
int cJSON_IsString(const cJSON *item) { return item ? (item->type & cJSON_String) : 0; }
int cJSON_IsNumber(const cJSON *item) { return item ? (item->type & cJSON_Number) : 0; }
int cJSON_IsObject(const cJSON *item) { return item ? (item->type & cJSON_Object) : 0; }

char *cJSON_GetStringValue(const cJSON *item)
{
    if (!item || !(item->type & cJSON_String)) return NULL;
    return item->valuestring;
}

double cJSON_GetNumberValue(const cJSON *item)
{
    if (!item || !(item->type & cJSON_Number)) return NAN;
    return item->valuedouble;
}