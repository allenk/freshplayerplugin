/*
 Parson ( http://kgabis.github.com/parson/ )
 Copyright (c) 2012 - 2014 Krzysztof Gabis

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "parson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define STARTING_CAPACITY         15
#define ARRAY_MAX_CAPACITY    122880 /* 15*(2^13) */
#define OBJECT_MAX_CAPACITY      960 /* 15*(2^6)  */
#define MAX_NESTING               19
#define DOUBLE_SERIALIZATION_FORMAT "%f"

#define SIZEOF_TOKEN(a)       (sizeof(a) - 1)
#define SKIP_CHAR(str)        ((*str)++)
#define SKIP_WHITESPACES(str) while (isspace(**str)) { SKIP_CHAR(str); }
#define MAX(a, b)             ((a) > (b) ? (a) : (b))

#define PARSON_MALLOC(a)     malloc(a)
#define PARSON_FREE(a)       free((void*)(a))
#define PARSON_REALLOC(a, b) realloc((a), (b))

#define PRINT_AND_SKIP(str, to_append) str += sprintf(str, to_append);
#define PRINTF_AND_SKIP(str, format, to_append) str += sprintf(str, format, to_append);

/* Type definitions */
typedef union json_value_value {
    const char  *string;
    double       number;
    JSON_Object *object;
    JSON_Array  *array;
    int          boolean;
    int          null;
} JSON_Value_Value;

struct json_value_t {
    JSON_Value_Type     type;
    JSON_Value_Value    value;
};

struct json_object_t {
    const char **names;
    JSON_Value **values;
    size_t       count;
    size_t       capacity;
};

struct json_array_t {
    JSON_Value **items;
    size_t       count;
    size_t       capacity;
};

/* Various */
static char * read_file(const char *filename);
static void   remove_comments(char *string, const char *start_token, const char *end_token);
static int    try_realloc(void **ptr, size_t new_size);
static char * parson_strndup(const char *string, size_t n);
static char * parson_strdup(const char *string);
static int    is_utf(const unsigned char *string);
static int    is_decimal(const char *string, size_t length);
static size_t parson_strlen(const char *string);

/* JSON Object */
static JSON_Object * json_object_init(void);
static JSON_Status   json_object_add(JSON_Object *object, const char *name, JSON_Value *value);
static JSON_Status   json_object_resize(JSON_Object *object, size_t capacity);
static JSON_Value  * json_object_nget_value(const JSON_Object *object, const char *name, size_t n);
static void          json_object_free(JSON_Object *object);

/* JSON Array */
static JSON_Array * json_array_init(void);
static JSON_Status  json_array_add(JSON_Array *array, JSON_Value *value);
static JSON_Status  json_array_resize(JSON_Array *array, size_t capacity);
static void         json_array_free(JSON_Array *array);

/* JSON Value */
static JSON_Value * json_value_init_string_no_copy(const char *string);

/* Parser */
static void         skip_quotes(const char **string);
static int          parse_utf_16(const char **unprocessed, char **processed);
static char*        process_string(const char *input, size_t len);
static const char * get_quoted_string(const char **string);
static JSON_Value * parse_object_value(const char **string, size_t nesting);
static JSON_Value * parse_array_value(const char **string, size_t nesting);
static JSON_Value * parse_string_value(const char **string);
static JSON_Value * parse_boolean_value(const char **string);
static JSON_Value * parse_number_value(const char **string);
static JSON_Value * parse_null_value(const char **string);
static JSON_Value * parse_value(const char **string, size_t nesting);

/* Serialization */
static size_t json_serialization_size_r(const JSON_Value *value, char *buf);
static char * json_serialize_to_buffer_r(const JSON_Value *value, char *buf);
static char * json_serialize_string(const char *string, char *buf);

/* Various */
static int try_realloc(void **ptr, size_t new_size) {
    void *reallocated_ptr = NULL;
    if (new_size == 0) {
        return JSONFailure;
    }
    reallocated_ptr = PARSON_REALLOC(*ptr, new_size);
    if (reallocated_ptr == NULL) {
        return JSONFailure;
    }
    *ptr = reallocated_ptr;
    return JSONSuccess;
}

static char * parson_strndup(const char *string, size_t n) {
    char *output_string = (char*)PARSON_MALLOC(n + 1);
    if (!output_string)
        return NULL;
    output_string[n] = '\0';
    strncpy(output_string, string, n);
    return output_string;
}

static char * parson_strdup(const char *string) {
    return parson_strndup(string, strlen(string));
}

static int is_utf(const unsigned char *s) {
    return isxdigit(s[0]) && isxdigit(s[1]) && isxdigit(s[2]) && isxdigit(s[3]);
}

static int is_decimal(const char *string, size_t length) {
    if (length > 1 && string[0] == '0' && string[1] != '.')
        return 0;
    if (length > 2 && !strncmp(string, "-0", 2) && string[2] != '.')
        return 0;
    while (length--)
        if (strchr("xX", string[length]))
            return 0;
    return 1;
}

static size_t parson_strlen(const char *string) {
    size_t result = 0;
    size_t i = 0, len = strlen(string);
    for (i = 0; i < len; i++) {
        if (strchr("\"\\\b\f\n\r\t", string[i])) /* must be escaped */
            result += 2;
        else
            result += 1;
    }
    return result;
}

static char * read_file(const char * filename) {
    FILE *fp = fopen(filename, "r");
    size_t file_size;
    long pos;
    char *file_contents;
    if (!fp)
        return NULL;
    fseek(fp, 0L, SEEK_END);
    pos = ftell(fp);
    if (pos < 0) {
        fclose(fp);
        return NULL;
    }
    file_size = pos;
    rewind(fp);
    file_contents = (char*)PARSON_MALLOC(sizeof(char) * (file_size + 1));
    if (!file_contents) {
        fclose(fp);
        return NULL;
    }
    if (fread(file_contents, file_size, 1, fp) < 1) {
        if (ferror(fp)) {
            fclose(fp);
            PARSON_FREE(file_contents);
            return NULL;
        }
    }
    fclose(fp);
    file_contents[file_size] = '\0';
    return file_contents;
}

static void remove_comments(char *string, const char *start_token, const char *end_token) {
    int in_string = 0, escaped = 0;
    size_t i;
    char *ptr = NULL, current_char;
    size_t start_token_len = strlen(start_token);
    size_t end_token_len = strlen(end_token);
    if (start_token_len == 0 || end_token_len == 0)
    	return;
    while ((current_char = *string) != '\0') {
        if (current_char == '\\' && !escaped) {
            escaped = 1;
            string++;
            continue;
        } else if (current_char == '\"' && !escaped) {
            in_string = !in_string;
        } else if (!in_string && strncmp(string, start_token, start_token_len) == 0) {
			for(i = 0; i < start_token_len; i++)
                string[i] = ' ';
        	string = string + start_token_len;
            ptr = strstr(string, end_token);
            if (!ptr)
                return;
            for (i = 0; i < (ptr - string) + end_token_len; i++)
                string[i] = ' ';
          	string = ptr + end_token_len - 1;
        }
        escaped = 0;
        string++;
    }
}

/* JSON Object */
static JSON_Object * json_object_init(void) {
    JSON_Object *new_obj = (JSON_Object*)PARSON_MALLOC(sizeof(JSON_Object));
    if (!new_obj)
        return NULL;
    new_obj->names = (const char**)NULL;
    new_obj->values = (JSON_Value**)NULL;
    new_obj->capacity = 0;
    new_obj->count = 0;
    return new_obj;
}

static JSON_Status json_object_add(JSON_Object *object, const char *name, JSON_Value *value) {
    size_t index = 0;
    if (object == NULL || name == NULL || value == NULL) {
        return JSONFailure;
    }
    if (object->count >= object->capacity) {
        size_t new_capacity = MAX(object->capacity * 2, STARTING_CAPACITY);
        if (new_capacity > OBJECT_MAX_CAPACITY)
            return JSONFailure;
        if (json_object_resize(object, new_capacity) == JSONFailure)
            return JSONFailure;
    }
    if (json_object_get_value(object, name) != NULL)
        return JSONFailure;
    index = object->count;
    object->names[index] = parson_strdup(name);
    if (object->names[index] == NULL)
        return JSONFailure;
    object->values[index] = value;
    object->count++;
    return JSONSuccess;
}

static JSON_Status json_object_resize(JSON_Object *object, size_t capacity) {
    if (try_realloc((void**)&object->names, capacity * sizeof(char*)) == JSONFailure)
        return JSONFailure;
    if (try_realloc((void**)&object->values, capacity * sizeof(JSON_Value*)) == JSONFailure)
        return JSONFailure;
    object->capacity = capacity;
    return JSONSuccess;
}

static JSON_Value * json_object_nget_value(const JSON_Object *object, const char *name, size_t n) {
    size_t i, name_length;
    for (i = 0; i < json_object_get_count(object); i++) {
        name_length = strlen(object->names[i]);
        if (name_length != n)
            continue;
        if (strncmp(object->names[i], name, n) == 0)
            return object->values[i];
    }
    return NULL;
}

static void json_object_free(JSON_Object *object) {
    while(object->count--) {
        PARSON_FREE(object->names[object->count]);
        json_value_free(object->values[object->count]);
    }
    PARSON_FREE(object->names);
    PARSON_FREE(object->values);
    PARSON_FREE(object);
}

/* JSON Array */
static JSON_Array * json_array_init(void) {
    JSON_Array *new_array = (JSON_Array*)PARSON_MALLOC(sizeof(JSON_Array));
    if (!new_array)
        return NULL;
    new_array->items = (JSON_Value**)NULL;
    new_array->capacity = 0;
    new_array->count = 0;
    return new_array;
}

static JSON_Status json_array_add(JSON_Array *array, JSON_Value *value) {
    if (array->count >= array->capacity) {
        size_t new_capacity = MAX(array->capacity * 2, STARTING_CAPACITY);
        if (new_capacity > ARRAY_MAX_CAPACITY)
            return JSONFailure;
        if (json_array_resize(array, new_capacity) == JSONFailure)
            return JSONFailure;
    }
    array->items[array->count] = value;
    array->count++;
    return JSONSuccess;
}

static JSON_Status json_array_resize(JSON_Array *array, size_t capacity) {
    if (try_realloc((void**)&array->items, capacity * sizeof(JSON_Value*)) == JSONFailure)
        return JSONFailure;
    array->capacity = capacity;
    return JSONSuccess;
}

static void json_array_free(JSON_Array *array) {
    while (array->count--)
        json_value_free(array->items[array->count]);
    PARSON_FREE(array->items);
    PARSON_FREE(array);
}

/* JSON Value */
static JSON_Value * json_value_init_string_no_copy(const char *string) {
    JSON_Value *new_value = (JSON_Value*)PARSON_MALLOC(sizeof(JSON_Value));
    if (!new_value)
        return NULL;
    new_value->type = JSONString;
    new_value->value.string = string;
    return new_value;
}

/* Parser */
static void skip_quotes(const char **string) {
    SKIP_CHAR(string);
    while (**string != '\"') {
        if (**string == '\0')
            return;
        if (**string == '\\') {
            SKIP_CHAR(string);
            if (**string == '\0')
                return;
        }
        SKIP_CHAR(string);
    }
    SKIP_CHAR(string);
}

static int parse_utf_16(const char **unprocessed, char **processed) {
    unsigned int cp, lead, trail;
    char *processed_ptr = *processed;
    const char *unprocessed_ptr = *unprocessed;
    unprocessed_ptr++; /* skips u */
    if (!is_utf((const unsigned char*)unprocessed_ptr) || sscanf(unprocessed_ptr, "%4x", &cp) == EOF)
            return JSONFailure;
    if (cp < 0x80) {
        *processed_ptr = cp; /* 0xxxxxxx */
    } else if (cp < 0x800) {
        *processed_ptr++ = ((cp >> 6) & 0x1F) | 0xC0; /* 110xxxxx */
        *processed_ptr   = ((cp     ) & 0x3F) | 0x80; /* 10xxxxxx */
    } else if (cp < 0xD800 || cp > 0xDFFF) {
        *processed_ptr++ = ((cp >> 12) & 0x0F) | 0xE0; /* 1110xxxx */
        *processed_ptr++ = ((cp >> 6)  & 0x3F) | 0x80; /* 10xxxxxx */
        *processed_ptr   = ((cp     )  & 0x3F) | 0x80; /* 10xxxxxx */
    } else if (cp >= 0xD800 && cp <= 0xDBFF) { /* lead surrogate (0xD800..0xDBFF) */
        lead = cp;
        unprocessed_ptr += 4; /* should always be within the buffer, otherwise previous sscanf would fail */
        if (*unprocessed_ptr++ != '\\' || *unprocessed_ptr++ != 'u' || /* starts with \u? */
            !is_utf((const unsigned char*)unprocessed_ptr)          ||
            sscanf(unprocessed_ptr, "%4x", &trail) == EOF           ||
            trail < 0xDC00 || trail > 0xDFFF) { /* valid trail surrogate? (0xDC00..0xDFFF) */
                return JSONFailure;
        }
        cp = ((((lead-0xD800)&0x3FF)<<10)|((trail-0xDC00)&0x3FF))+0x010000;
        *processed_ptr++ = (((cp >> 18) & 0x07) | 0xF0); /* 11110xxx */
        *processed_ptr++ = (((cp >> 12) & 0x3F) | 0x80); /* 10xxxxxx */
        *processed_ptr++ = (((cp >> 6)  & 0x3F) | 0x80); /* 10xxxxxx */
        *processed_ptr   = (((cp     )  & 0x3F) | 0x80); /* 10xxxxxx */
    } else { /* trail surrogate before lead surrogate */
        return JSONFailure;
    }
    unprocessed_ptr += 3;
    *processed = processed_ptr;
    *unprocessed = unprocessed_ptr;
    return JSONSuccess;
}


/* Copies and processes passed string up to supplied length.
Example: "\u006Corem ipsum" -> lorem ipsum */
static char* process_string(const char *input, size_t len) {
    const char *input_ptr = input;
    char *output = (char*)PARSON_MALLOC((len + 1) * sizeof(char));
    char *output_ptr = output;
    while ((*input_ptr != '\0') && (size_t)(input_ptr - input) < len) {
        if (*input_ptr == '\\') {
            input_ptr++;
            switch (*input_ptr) {
                case '\"': *output_ptr = '\"'; break;
                case '\\': *output_ptr = '\\'; break;
                case '/':  *output_ptr = '/';  break;
                case 'b':  *output_ptr = '\b'; break;
                case 'f':  *output_ptr = '\f'; break;
                case 'n':  *output_ptr = '\n'; break;
                case 'r':  *output_ptr = '\r'; break;
                case 't':  *output_ptr = '\t'; break;
                case 'u':
                    if (parse_utf_16(&input_ptr, &output_ptr) == JSONFailure)
                        goto error;
                    break;
                default:
                    goto error;
            }
        } else if ((unsigned char)*input_ptr < 0x20) {
            goto error; /* 0x00-0x19 are invalid characters for json string (http://www.ietf.org/rfc/rfc4627.txt) */
        } else {
            *output_ptr = *input_ptr;
        }
        output_ptr++;
        input_ptr++;
    }
    *output_ptr = '\0';
    if (try_realloc((void**)&output, (size_t)(output_ptr-output) + 1) == JSONFailure) /* resize to new length */
        goto error;
    return output;
error:
    free(output);
    return NULL;
}

/* Return processed contents of a string between quotes and
   skips passed argument to a matching quote. */
static const char * get_quoted_string(const char **string) {
    const char *string_start = *string;
    size_t string_len = 0;
    skip_quotes(string);
    if (**string == '\0')
        return NULL;
    string_len = *string - string_start - 2; /* length without quotes */
    return process_string(string_start + 1, string_len);
}

static JSON_Value * parse_value(const char **string, size_t nesting) {
    if (nesting > MAX_NESTING)
        return NULL;
    SKIP_WHITESPACES(string);
    switch (**string) {
        case '{':
            return parse_object_value(string, nesting + 1);
        case '[':
            return parse_array_value(string, nesting + 1);
        case '\"':
            return parse_string_value(string);
        case 'f': case 't':
            return parse_boolean_value(string);
        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return parse_number_value(string);
        case 'n':
            return parse_null_value(string);
        default:
            return NULL;
    }
}

static JSON_Value * parse_object_value(const char **string, size_t nesting) {
    JSON_Value *output_value = json_value_init_object(), *new_value = NULL;
    JSON_Object *output_object = json_value_get_object(output_value);
    const char *new_key = NULL;
    if (output_value == NULL)
        return NULL;
    SKIP_CHAR(string);
    SKIP_WHITESPACES(string);
    if (**string == '}') { /* empty object */
        SKIP_CHAR(string);
        return output_value;
    }
    while (**string != '\0') {
        new_key = get_quoted_string(string);
        SKIP_WHITESPACES(string);
        if (new_key == NULL || **string != ':') {
            json_value_free(output_value);
            return NULL;
        }
        SKIP_CHAR(string);
        new_value = parse_value(string, nesting);
        if (new_value == NULL) {
            PARSON_FREE(new_key);
            json_value_free(output_value);
            return NULL;
        }
        if(json_object_add(output_object, new_key, new_value) == JSONFailure) {
            PARSON_FREE(new_key);
            PARSON_FREE(new_value);
            json_value_free(output_value);
            return NULL;
        }
        PARSON_FREE(new_key);
        SKIP_WHITESPACES(string);
        if (**string != ',')
            break;
        SKIP_CHAR(string);
        SKIP_WHITESPACES(string);
    }
    SKIP_WHITESPACES(string);
    if (**string != '}' || /* Trim object after parsing is over */
        json_object_resize(output_object, json_object_get_count(output_object)) == JSONFailure) {
            json_value_free(output_value);
            return NULL;
    }
    SKIP_CHAR(string);
    return output_value;
}

static JSON_Value * parse_array_value(const char **string, size_t nesting) {
    JSON_Value *output_value = json_value_init_array(), *new_array_value = NULL;
    JSON_Array *output_array = json_value_get_array(output_value);
    if (!output_value)
        return NULL;
    SKIP_CHAR(string);
    SKIP_WHITESPACES(string);
    if (**string == ']') { /* empty array */
        SKIP_CHAR(string);
        return output_value;
    }
    while (**string != '\0') {
        new_array_value = parse_value(string, nesting);
        if (!new_array_value) {
            json_value_free(output_value);
            return NULL;
        }
        if(json_array_add(output_array, new_array_value) == JSONFailure) {
            PARSON_FREE(new_array_value);
            json_value_free(output_value);
            return NULL;
        }
        SKIP_WHITESPACES(string);
        if (**string != ',')
            break;
        SKIP_CHAR(string);
        SKIP_WHITESPACES(string);
    }
    SKIP_WHITESPACES(string);
    if (**string != ']' || /* Trim array after parsing is over */
        json_array_resize(output_array, json_array_get_count(output_array)) == JSONFailure) {
            json_value_free(output_value);
            return NULL;
    }
    SKIP_CHAR(string);
    return output_value;
}

static JSON_Value * parse_string_value(const char **string) {
    const char *new_string = get_quoted_string(string);
    if (!new_string)
        return NULL;
    return json_value_init_string_no_copy(new_string);
}

static JSON_Value * parse_boolean_value(const char **string) {
    size_t true_token_size = SIZEOF_TOKEN("true");
    size_t false_token_size = SIZEOF_TOKEN("false");
    if (strncmp("true", *string, true_token_size) == 0) {
        *string += true_token_size;
        return json_value_init_boolean(1);
    } else if (strncmp("false", *string, false_token_size) == 0) {
        *string += false_token_size;
        return json_value_init_boolean(0);
    }
    return NULL;
}

static JSON_Value * parse_number_value(const char **string) {
    char *end;
    double number = strtod(*string, &end);
    JSON_Value *output_value;
    if (is_decimal(*string, end - *string)) {
        *string = end;
        output_value = json_value_init_number(number);
    } else {
        output_value = NULL;
    }
    return output_value;
}

static JSON_Value * parse_null_value(const char **string) {
    size_t token_size = SIZEOF_TOKEN("null");
    if (strncmp("null", *string, token_size) == 0) {
        *string += token_size;
        return json_value_init_null();
    }
    return NULL;
}

/* Serialization */
static size_t json_serialization_size_r(const JSON_Value *value, char *buf) {
    size_t result_size = 0;
    const char *key = NULL;
    JSON_Value *temp_value = NULL;
    JSON_Array *array = NULL;
    JSON_Object *object = NULL;
    size_t i = 0, count = 0;
    double num = 0.0;
    switch (json_value_get_type(value)) {
        case JSONArray:
            array = json_value_get_array(value);
            count = json_array_get_count(array);
            result_size += 2; /* [ and ] brackets */
            if (count > 0)
                result_size += count - 1; /* , between items */
            for (i = 0; i < count; i++) {
                temp_value = json_array_get_value(array, i);
                result_size += json_serialization_size_r(temp_value, buf);
            }
            return result_size;
        case JSONObject:
            object = json_value_get_object(value);
            count  = json_object_get_count(object);
            result_size += 2; /* { and } brackets */
            if (count > 0)
                result_size += (count * 2) - 1; /* : between keys and values and , between items */
            for (i = 0; i < count; i++) {
                key = json_object_get_name(object, i);
                result_size += parson_strlen(key) + 2; /* string and quotes */
                result_size += json_serialization_size_r(json_object_get_value(object, key), buf);
            }
            return result_size;
        case JSONString:
            return parson_strlen(json_value_get_string(value)) + 2; /* string and quotes */
        case JSONBoolean:
            if (json_value_get_boolean(value))
                return 4; /* strlen("true"); */
            else
                return 5; /* strlen("false"); */
        case JSONNumber:
            num = json_value_get_number(value);
            if (num == ((double)(int)num) ) /*  check if num is integer */
                return (size_t)sprintf(buf, "%d", (int)num);
            return (size_t)sprintf(buf, DOUBLE_SERIALIZATION_FORMAT, num);
        case JSONNull:
            return 4; /* strlen("null"); */
        case JSONError:
            return 0;
        default:
            return 0;
    }
}

char* json_serialize_to_buffer_r(const JSON_Value *value, char *buf)
{
    const char *key = NULL, *string = NULL;
    JSON_Value *temp_value = NULL;
    JSON_Array *array = NULL;
    JSON_Object *object = NULL;
    size_t i = 0, count = 0;
    double num = 0.0;
    switch (json_value_get_type(value)) {
        case JSONArray:
            array = json_value_get_array(value);
            count = json_array_get_count(array);
            PRINT_AND_SKIP(buf, "[");
            for (i = 0; i < count; i++) {
                temp_value = json_array_get_value(array, i);
                buf = json_serialize_to_buffer_r(temp_value, buf);
                if (buf == NULL)
                    return NULL;
                if (i < (count - 1))
                    PRINT_AND_SKIP(buf, ",");
            }
            PRINT_AND_SKIP(buf, "]");
            return buf;
        case JSONObject:
            object = json_value_get_object(value);
            count  = json_object_get_count(object);
            PRINT_AND_SKIP(buf, "{");
            for (i = 0; i < count; i++) {
                key = json_object_get_name(object, i);
                buf = json_serialize_string(key, buf);
                if (buf == NULL)
                    return NULL;
                PRINT_AND_SKIP(buf, ":");
                temp_value = json_object_get_value(object, key);
                buf = json_serialize_to_buffer_r(temp_value, buf);
                if (buf == NULL)
                    return NULL;
                if (i < (count - 1))
                    PRINT_AND_SKIP(buf, ",");
            }
            PRINT_AND_SKIP(buf, "}");
            return buf;
        case JSONString:
            string = json_value_get_string(value);
            buf = json_serialize_string(string, buf);
            return buf;
        case JSONBoolean:
            if (json_value_get_boolean(value)) {
                PRINT_AND_SKIP(buf, "true");
            } else {
                PRINT_AND_SKIP(buf, "false");
            }
            return buf;
        case JSONNumber:
            num = json_value_get_number(value);
            if (num == ((double)(int)num)) { /*  check if num is integer */
                PRINTF_AND_SKIP(buf, "%d", (int)num);
            } else {
                PRINTF_AND_SKIP(buf, DOUBLE_SERIALIZATION_FORMAT, num);
            }
            return buf;
        case JSONNull:
            PRINT_AND_SKIP(buf, "null");
            return buf;
        case JSONError:
            return NULL;
        default:
            return NULL;
    }
}

static char * json_serialize_string(const char *string, char *buf) {
    size_t i = 0, len = strlen(string);
    char c = '\0';
    PRINT_AND_SKIP(buf, "\"")
    for (i = 0; i < len; i++) {
        c = string[i];
        switch (c) {
            case '\"': PRINT_AND_SKIP(buf, "\\\"");   break;
            case '\\': PRINT_AND_SKIP(buf, "\\\\");   break;
            case '\b': PRINT_AND_SKIP(buf, "\\b");    break;
            case '\f': PRINT_AND_SKIP(buf, "\\f");    break;
            case '\n': PRINT_AND_SKIP(buf, "\\n");    break;
            case '\r': PRINT_AND_SKIP(buf, "\\r");    break;
            case '\t': PRINT_AND_SKIP(buf, "\\t");    break;
            default:   PRINTF_AND_SKIP(buf, "%c", c); break;
        }
    }
    PRINT_AND_SKIP(buf, "\"");
    return buf;
}

/* Parser API */
JSON_Value * json_parse_file(const char *filename) {
    char *file_contents = read_file(filename);
    JSON_Value *output_value = NULL;
    if (file_contents == NULL)
        return NULL;
    output_value = json_parse_string(file_contents);
    PARSON_FREE(file_contents);
    return output_value;
}

JSON_Value * json_parse_file_with_comments(const char *filename) {
    char *file_contents = read_file(filename);
    JSON_Value *output_value = NULL;
    if (file_contents == NULL)
        return NULL;
    output_value = json_parse_string_with_comments(file_contents);
    PARSON_FREE(file_contents);
    return output_value;
}

JSON_Value * json_parse_string(const char *string) {
    if (string == NULL)
        return NULL;
    SKIP_WHITESPACES(&string);
    if (*string != '{' && *string != '[')
        return NULL;
    return parse_value((const char**)&string, 0);
}

JSON_Value * json_parse_string_with_comments(const char *string) {
    JSON_Value *result = NULL;
    char *string_mutable_copy = NULL, *string_mutable_copy_ptr = NULL;
    string_mutable_copy = parson_strdup(string);
    if (string_mutable_copy == NULL)
        return NULL;
    remove_comments(string_mutable_copy, "/*", "*/");
    remove_comments(string_mutable_copy, "//", "\n");
    string_mutable_copy_ptr = string_mutable_copy;
    SKIP_WHITESPACES(&string_mutable_copy_ptr);
    if (*string_mutable_copy_ptr != '{' && *string_mutable_copy_ptr != '[') {
        PARSON_FREE(string_mutable_copy);
        return NULL;
    }
    result = parse_value((const char**)&string_mutable_copy_ptr, 0);
    PARSON_FREE(string_mutable_copy);
    return result;
}


/* JSON Object API */

JSON_Value * json_object_get_value(const JSON_Object *object, const char *name) {
    if (object == NULL || name == NULL)
        return NULL;
    return json_object_nget_value(object, name, strlen(name));
}

const char * json_object_get_string(const JSON_Object *object, const char *name) {
    return json_value_get_string(json_object_get_value(object, name));
}

double json_object_get_number(const JSON_Object *object, const char *name) {
    return json_value_get_number(json_object_get_value(object, name));
}

JSON_Object * json_object_get_object(const JSON_Object *object, const char *name) {
    return json_value_get_object(json_object_get_value(object, name));
}

JSON_Array * json_object_get_array(const JSON_Object *object, const char *name) {
    return json_value_get_array(json_object_get_value(object, name));
}

int json_object_get_boolean(const JSON_Object *object, const char *name) {
    return json_value_get_boolean(json_object_get_value(object, name));
}

JSON_Value * json_object_dotget_value(const JSON_Object *object, const char *name) {
    const char *dot_position = strchr(name, '.');
    if (!dot_position)
        return json_object_get_value(object, name);
    object = json_value_get_object(json_object_nget_value(object, name, dot_position - name));
    return json_object_dotget_value(object, dot_position + 1);
}

const char * json_object_dotget_string(const JSON_Object *object, const char *name) {
    return json_value_get_string(json_object_dotget_value(object, name));
}

double json_object_dotget_number(const JSON_Object *object, const char *name) {
    return json_value_get_number(json_object_dotget_value(object, name));
}

JSON_Object * json_object_dotget_object(const JSON_Object *object, const char *name) {
    return json_value_get_object(json_object_dotget_value(object, name));
}

JSON_Array * json_object_dotget_array(const JSON_Object *object, const char *name) {
    return json_value_get_array(json_object_dotget_value(object, name));
}

int json_object_dotget_boolean(const JSON_Object *object, const char *name) {
    return json_value_get_boolean(json_object_dotget_value(object, name));
}

size_t json_object_get_count(const JSON_Object *object) {
    return object ? object->count : 0;
}

const char * json_object_get_name(const JSON_Object *object, size_t index) {
    if (index >= json_object_get_count(object))
        return NULL;
    return object->names[index];
}

/* JSON Array API */
JSON_Value * json_array_get_value(const JSON_Array *array, size_t index) {
    if (index >= json_array_get_count(array))
        return NULL;
    return array->items[index];
}

const char * json_array_get_string(const JSON_Array *array, size_t index) {
    return json_value_get_string(json_array_get_value(array, index));
}

double json_array_get_number(const JSON_Array *array, size_t index) {
    return json_value_get_number(json_array_get_value(array, index));
}

JSON_Object * json_array_get_object(const JSON_Array *array, size_t index) {
    return json_value_get_object(json_array_get_value(array, index));
}

JSON_Array * json_array_get_array(const JSON_Array *array, size_t index) {
    return json_value_get_array(json_array_get_value(array, index));
}

int json_array_get_boolean(const JSON_Array *array, size_t index) {
    return json_value_get_boolean(json_array_get_value(array, index));
}

size_t json_array_get_count(const JSON_Array *array) {
    return array ? array->count : 0;
}

/* JSON Value API */
JSON_Value_Type json_value_get_type(const JSON_Value *value) {
    return value ? value->type : JSONError;
}

JSON_Object * json_value_get_object(const JSON_Value *value) {
    return json_value_get_type(value) == JSONObject ? value->value.object : NULL;
}

JSON_Array * json_value_get_array(const JSON_Value *value) {
    return json_value_get_type(value) == JSONArray ? value->value.array : NULL;
}

const char * json_value_get_string(const JSON_Value *value) {
    return json_value_get_type(value) == JSONString ? value->value.string : NULL;
}

double json_value_get_number(const JSON_Value *value) {
    return json_value_get_type(value) == JSONNumber ? value->value.number : 0;
}

int json_value_get_boolean(const JSON_Value *value) {
    return json_value_get_type(value) == JSONBoolean ? value->value.boolean : -1;
}

void json_value_free(JSON_Value *value) {
    switch (json_value_get_type(value)) {
        case JSONObject:
            json_object_free(value->value.object);
            break;
        case JSONString:
            if (value->value.string) { PARSON_FREE(value->value.string); }
            break;
        case JSONArray:
            json_array_free(value->value.array);
            break;
        default:
            break;
    }
    PARSON_FREE(value);
}

JSON_Value * json_value_init_object(void) {
    JSON_Value *new_value = (JSON_Value*)PARSON_MALLOC(sizeof(JSON_Value));
    if (!new_value)
        return NULL;
    new_value->type = JSONObject;
    new_value->value.object = json_object_init();
    if (!new_value->value.object) {
        PARSON_FREE(new_value);
        return NULL;
    }
    return new_value;
}

JSON_Value * json_value_init_array(void) {
    JSON_Value *new_value = (JSON_Value*)PARSON_MALLOC(sizeof(JSON_Value));
    if (!new_value)
        return NULL;
    new_value->type = JSONArray;
    new_value->value.array = json_array_init();
    if (!new_value->value.array) {
        PARSON_FREE(new_value);
        return NULL;
    }
    return new_value;
}

JSON_Value * json_value_init_string(const char *string) {
    char *copy = NULL;
    if (string == NULL)
        return NULL;
    copy = parson_strdup(string);
    if (copy == NULL)
        return NULL;
    return json_value_init_string_no_copy(copy);
}

JSON_Value * json_value_init_number(double number) {
    JSON_Value *new_value = (JSON_Value*)PARSON_MALLOC(sizeof(JSON_Value));
    if (!new_value)
        return NULL;
    new_value->type = JSONNumber;
    new_value->value.number = number;
    return new_value;
}

JSON_Value * json_value_init_boolean(int boolean) {
    JSON_Value *new_value = (JSON_Value*)PARSON_MALLOC(sizeof(JSON_Value));
    if (!new_value)
        return NULL;
    new_value->type = JSONBoolean;
    new_value->value.boolean = boolean ? 1 : 0;
    return new_value;
}

JSON_Value * json_value_init_null(void) {
    JSON_Value *new_value = (JSON_Value*)PARSON_MALLOC(sizeof(JSON_Value));
    if (!new_value)
        return NULL;
    new_value->type = JSONNull;
    return new_value;
}

JSON_Value * json_value_deep_copy(const JSON_Value *value) {
    size_t i = 0;
    JSON_Value *return_value = NULL, *temp_value_copy = NULL, *temp_value = NULL;
    const char *temp_string = NULL, *temp_string_copy = NULL, *temp_key = NULL;
    JSON_Array *temp_array = NULL, *temp_array_copy = NULL;
    JSON_Object *temp_object = NULL, *temp_object_copy = NULL;
    
    switch (json_value_get_type(value)) {
        case JSONArray:
            temp_array = json_value_get_array(value);
            return_value = json_value_init_array();
            if (return_value == NULL)
                return NULL;
            temp_array_copy = json_value_get_array(return_value);
            for (i = 0; i < json_array_get_count(temp_array); i++) {
                temp_value = json_array_get_value(temp_array, i);
                temp_value_copy = json_value_deep_copy(temp_value);
                if (temp_value_copy == NULL) {
                    json_value_free(return_value);
                    return NULL;
                }
                if (json_array_add(temp_array_copy, temp_value_copy) == JSONFailure) {
                    json_value_free(return_value);
                    json_value_free(temp_value_copy);
                    return NULL;
                }
            }
            return return_value;
        case JSONObject:
            temp_object = json_value_get_object(value);
            return_value = json_value_init_object();
            if (return_value == NULL)
                return NULL;
            temp_object_copy = json_value_get_object(return_value);
            for (i = 0; i < json_object_get_count(temp_object); i++) {
                temp_key = json_object_get_name(temp_object, i);
                temp_value = json_object_get_value(temp_object, temp_key);
                temp_value_copy = json_value_deep_copy(temp_value);
                if (temp_value_copy == NULL) {
                    json_value_free(return_value);
                    return NULL;
                }
                if (json_object_add(temp_object_copy, temp_key, temp_value_copy) == JSONFailure) {
                    json_value_free(return_value);
                    json_value_free(temp_value_copy);
                    return NULL;
                }
            }
            return return_value;
        case JSONBoolean:
            return json_value_init_boolean(json_value_get_boolean(value));
        case JSONNumber:
            return json_value_init_number(json_value_get_number(value));
        case JSONString:
            temp_string = json_value_get_string(value);
            temp_string_copy = parson_strdup(temp_string);
            if (temp_string_copy == NULL)
                return NULL;
            return json_value_init_string_no_copy(temp_string_copy);
        case JSONNull:
            return json_value_init_null();
        case JSONError:
            return NULL;
        default:
            return NULL;
    }
}

size_t json_serialization_size(const JSON_Value *value) {
    char buf[1100]; /* recursively allocating buffer on stack is a bad idea, so let's do it only once */
    return json_serialization_size_r(value, buf) + 1;
}

JSON_Status json_serialize_to_buffer(const JSON_Value *value, char *buf, size_t buf_size_in_bytes) {
    char *serialization_result = NULL;
    size_t needed_size_in_bytes = json_serialization_size(value);
    if (buf_size_in_bytes < needed_size_in_bytes) {
        return JSONFailure;
    }
    serialization_result = json_serialize_to_buffer_r(value, buf);
    if(serialization_result == NULL)
        return JSONFailure;
    return JSONSuccess;
}

JSON_Status json_serialize_to_file(const JSON_Value *value, const char *filename) {
    JSON_Status return_code = JSONSuccess;
    FILE *fp = NULL;
    char *serialized_string = json_serialize_to_string(value);
    if (serialized_string == NULL) {
        return JSONFailure;
    }
    fp = fopen (filename, "w");
    if (fp != NULL) {
        if (fputs (serialized_string, fp) == EOF) {
            return_code = JSONFailure;
        }
        if (fclose (fp) == EOF) {
            return_code = JSONFailure;
        }
    }
    json_free_serialized_string(serialized_string);
    return return_code;
}

char * json_serialize_to_string(const JSON_Value *value) {
    JSON_Status serialization_result = JSONFailure;
    size_t buf_size_bytes = json_serialization_size(value);
    char *buf = (char*)PARSON_MALLOC(buf_size_bytes);
    if (buf == NULL)
        return NULL;
    serialization_result = json_serialize_to_buffer(value, buf, buf_size_bytes);
    if (serialization_result == JSONFailure) {
        json_free_serialized_string(buf);
        return NULL;
    }
    return buf;
}

void json_free_serialized_string(char *string) {
    PARSON_FREE(string);
}

JSON_Status json_array_remove(JSON_Array *array, size_t ix) {
    size_t last_element_ix = 0;
    if (array == NULL || ix >= json_array_get_count(array)) {
        return JSONFailure;
    }
    last_element_ix = json_array_get_count(array) - 1;
    json_value_free(json_array_get_value(array, ix));
    array->count -= 1;
    if (ix != last_element_ix) /* Replace value with one from the end of array */
        array->items[ix] = json_array_get_value(array, last_element_ix);
    return JSONSuccess;
}

JSON_Status json_array_replace_value(JSON_Array *array, size_t ix, JSON_Value *value) {
    if (array == NULL || value == NULL || ix >= json_array_get_count(array)) {
        return JSONFailure;
    }
    json_value_free(json_array_get_value(array, ix));
    array->items[ix] = value;
    return JSONSuccess;
}

JSON_Status json_array_replace_string(JSON_Array *array, size_t i, const char* string) {
    return json_array_replace_value(array, i, json_value_init_string(string));
}

JSON_Status json_array_replace_number(JSON_Array *array, size_t i, double number) {
    return json_array_replace_value(array, i, json_value_init_number(number));
}

JSON_Status json_array_replace_boolean(JSON_Array *array, size_t i, int boolean) {
    return json_array_replace_value(array, i, json_value_init_boolean(boolean));
}

JSON_Status json_array_replace_null(JSON_Array *array, size_t i) {
    return json_array_replace_value(array, i, json_value_init_null());
}

JSON_Status json_array_clear(JSON_Array *array) {
    size_t i = 0;
    if (array == NULL)
        return JSONFailure;
    for (i = 0; i < json_array_get_count(array); i++) {
        json_value_free(json_array_get_value(array, i));
    }
    array->count = 0;
    return JSONSuccess;
}

JSON_Status json_array_append_value(JSON_Array *array, JSON_Value *value) {
    if (array == NULL || value == NULL)
        return JSONFailure;
    return json_array_add(array, value);
}

JSON_Status json_array_append_string(JSON_Array *array, const char *string) {
    return json_array_append_value(array, json_value_init_string(string));
}

JSON_Status json_array_append_number(JSON_Array *array, double number) {
    return json_array_append_value(array, json_value_init_number(number));
}

JSON_Status json_array_append_boolean(JSON_Array *array, int boolean) {
    return json_array_append_value(array, json_value_init_boolean(boolean));
}

JSON_Status json_array_append_null(JSON_Array *array) {
    return json_array_append_value(array, json_value_init_null());
}

JSON_Status json_object_set_value(JSON_Object *object, const char *name, JSON_Value *value) {
    size_t i = 0;
    JSON_Value *old_value;
    if (object == NULL || name == NULL || value == NULL)
        return JSONFailure;
    old_value = json_object_get_value(object, name);
    if (old_value != NULL) { /* free and overwrite old value */
        json_value_free(old_value);
        for (i = 0; i < json_object_get_count(object); i++) {
            if (strcmp(object->names[i], name) == 0) {
                object->values[i] = value;
                return JSONSuccess;
            }
        }
    }
    /* add new key value pair */
    return json_object_add(object, name, value);
}

JSON_Status json_object_set_string(JSON_Object *object, const char *name, const char *string) {
    return json_object_set_value(object, name, json_value_init_string(string));
}

JSON_Status json_object_set_number(JSON_Object *object, const char *name, double number) {
    return json_object_set_value(object, name, json_value_init_number(number));
}

JSON_Status json_object_set_boolean(JSON_Object *object, const char *name, int boolean) {
    return json_object_set_value(object, name, json_value_init_boolean(boolean));
}

JSON_Status json_object_set_null(JSON_Object *object, const char *name) {
    return json_object_set_value(object, name, json_value_init_null());
}

JSON_Status json_object_dotset_value(JSON_Object *object, const char *name, JSON_Value *value) {
    const char *dot_pos = NULL;
    const char *current_name = NULL;
    JSON_Object *temp_obj = NULL;
    JSON_Value *new_value = NULL;
    if (value == NULL || name == NULL || value == NULL)
        return JSONFailure;
    dot_pos = strchr(name, '.');
    if (dot_pos == NULL) {
        return json_object_set_value(object, name, value);
    } else {
        current_name = parson_strndup(name, dot_pos - name);
        temp_obj = json_object_get_object(object, current_name);
        if (temp_obj == NULL) {
            new_value = json_value_init_object();
            if (new_value == NULL) {
                PARSON_FREE(current_name);
                return JSONFailure;
            }
            if (json_object_add(object, current_name, new_value) == JSONFailure) {
                json_value_free(new_value);
                PARSON_FREE(current_name);
                return JSONFailure;
            }
            temp_obj = json_object_get_object(object, current_name);
        }
        PARSON_FREE(current_name);
        return json_object_dotset_value(temp_obj, dot_pos + 1, value);
    }
    return JSONFailure;
}

JSON_Status json_object_dotset_string(JSON_Object *object, const char *name, const char *string) {
    return json_object_dotset_value(object, name, json_value_init_string(string));
}

JSON_Status json_object_dotset_number(JSON_Object *object, const char *name, double number) {
    return json_object_dotset_value(object, name, json_value_init_number(number));
}

JSON_Status json_object_dotset_boolean(JSON_Object *object, const char *name, int boolean) {
    return json_object_dotset_value(object, name, json_value_init_boolean(boolean));
}

JSON_Status json_object_dotset_null(JSON_Object *object, const char *name) {
    return json_object_dotset_value(object, name, json_value_init_null());
}

JSON_Status json_object_remove(JSON_Object *object, const char *name) {
    size_t i = 0, last_item_index = 0;
    if (object == NULL || json_object_get_value(object, name) == NULL)
        return JSONFailure;
    last_item_index = json_object_get_count(object) - 1;
    for (i = 0; i < json_object_get_count(object); i++) {
        if (strcmp(object->names[i], name) == 0) {
            PARSON_FREE(object->names[i]);
            json_value_free(object->values[i]);
            if (i != last_item_index) { /* Replace key value pair with one from the end */
                object->names[i] = object->names[last_item_index];
                object->values[i] = object->values[last_item_index];
            }
            object->count -= 1;
            return JSONSuccess;
        }
    }
    return JSONFailure; /* No execution path should end here */
}

JSON_Status json_object_dotremove(JSON_Object *object, const char *name) {
    const char *dot_pos = strchr(name, '.');
    const char *current_name = NULL;
    JSON_Object *temp_obj = NULL;
    if (dot_pos == NULL) {
        return json_object_remove(object, name);
    } else {
        current_name = parson_strndup(name, dot_pos - name);
        temp_obj = json_object_get_object(object, current_name);
        if (temp_obj == NULL) {
            PARSON_FREE(current_name);
            return JSONFailure;
        }
        PARSON_FREE(current_name);
        return json_object_dotremove(temp_obj, dot_pos + 1);
    }
    return JSONFailure;
}

JSON_Status json_object_clear(JSON_Object *object) {
    size_t i = 0;
    if (object == NULL) {
        return JSONFailure;
    }
    for (i = 0; i < json_object_get_count(object); i++) {        
        PARSON_FREE(object->names[i]);
        json_value_free(object->values[i]);
    }
    object->count = 0;
    return JSONSuccess;
}

JSON_Status json_validate(const JSON_Value *schema, const JSON_Value *value) {
    JSON_Value *temp_schema_value = NULL, *temp_value = NULL;
    JSON_Array *schema_array = NULL, *value_array = NULL;
    JSON_Object *schema_object = NULL, *value_object = NULL;
    JSON_Value_Type schema_type = JSONError, value_type = JSONError;
    const char *key = NULL;
    size_t i = 0, count = 0;
    if (schema == NULL || value == NULL)
        return JSONFailure;
    schema_type = json_value_get_type(schema);
    value_type = json_value_get_type(value);
    if (schema_type != value_type && schema_type != JSONNull) /* null represents all values */
        return JSONFailure;
    switch (schema_type) {
        case JSONArray:
            schema_array = json_value_get_array(schema);
            value_array = json_value_get_array(value);
            count = json_array_get_count(schema_array);
            if (count == 0)
                return JSONSuccess; /* Empty array allows all types */
            /* Get first value from array, rest is ignored */
            temp_schema_value = json_array_get_value(schema_array, 0);
            for (i = 0; i < json_array_get_count(value_array); i++) {
                temp_value = json_array_get_value(value_array, i);
                if (json_validate(temp_schema_value, temp_value) == 0) {
                    return JSONFailure;
                }
            }
            return JSONSuccess;
        case JSONObject:
            schema_object = json_value_get_object(schema);
            value_object = json_value_get_object(value);
            count = json_object_get_count(schema_object);
            if (count == 0)
                return JSONSuccess; /* Empty object allows all objects */
            else if (json_object_get_count(value_object) < count)
                return JSONFailure; /* Tested object mustn't have less name-value pairs than schema */
            for (i = 0; i < count; i++) {
                key = json_object_get_name(schema_object, i);
                temp_schema_value = json_object_get_value(schema_object, key);
                temp_value = json_object_get_value(value_object, key);
                if (temp_value == NULL)
                    return JSONFailure;
                if (json_validate(temp_schema_value, temp_value) == JSONFailure)
                    return JSONFailure;
            }
            return JSONSuccess;
        case JSONString: case JSONNumber: case JSONBoolean: case JSONNull:
            return JSONSuccess; /* equality already tested before switch */
        case JSONError: default:
            return JSONFailure;
    }
}

JSON_Status json_value_equals(const JSON_Value *a, const JSON_Value *b) {
    JSON_Object *a_object = NULL, *b_object = NULL;
    JSON_Array *a_array = NULL, *b_array = NULL;
    const char *a_string = NULL, *b_string = NULL;
    const char *key = NULL;
    size_t a_count = 0, b_count = 0, i = 0;
    JSON_Value_Type a_type, b_type;
    a_type = json_value_get_type(a);
    b_type = json_value_get_type(b);
    if (a_type != b_type) {
        return 0;
    }
    switch (a_type) {
        case JSONArray:
            a_array = json_value_get_array(a);
            b_array = json_value_get_array(b);
            a_count = json_array_get_count(a_array);
            b_count = json_array_get_count(b_array);
            if (a_count != b_count) {
                return 0;
            }
            for (i = 0; i < a_count; i++) {
                if (!json_value_equals(json_array_get_value(a_array, i),
                                       json_array_get_value(b_array, i))) {
                    return 0;
                }
            }
            return 1;
        case JSONObject:
            a_object = json_value_get_object(a);
            b_object = json_value_get_object(b);
            a_count = json_object_get_count(a_object);
            b_count = json_object_get_count(b_object);
            if (a_count != b_count) {
                return 0;
            }
            for (i = 0; i < a_count; i++) {
                key = json_object_get_name(a_object, i);
                if (!json_value_equals(json_object_get_value(a_object, key),
                                       json_object_get_value(b_object, key))) {
                    return 0;
                }
            }
            return 1;
        case JSONString:
            a_string = json_value_get_string(a);
            b_string = json_value_get_string(b);
            return strcmp(a_string, b_string) == 0;
        case JSONBoolean:
            return json_value_get_boolean(a) == json_value_get_boolean(b);
        case JSONNumber:
            return fabs(json_value_get_number(a) - json_value_get_number(b)) < 0.000001; /* EPSILON */
        case JSONError:
            return 1;
        case JSONNull:
            return 1;
        default:
            return 1;
    }
}

JSON_Value_Type json_type(const JSON_Value *value) {
    return json_value_get_type(value);
}

JSON_Object * json_object (const JSON_Value *value) {
    return json_value_get_object(value);
}

JSON_Array * json_array  (const JSON_Value *value) {
    return json_value_get_array(value);
}

const char * json_string (const JSON_Value *value) {
    return json_value_get_string(value);
}

double json_number (const JSON_Value *value) {
    return json_value_get_number(value);
}

int json_boolean(const JSON_Value *value) {
    return json_value_get_boolean(value);
}
