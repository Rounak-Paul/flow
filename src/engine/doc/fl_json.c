#include "fl_json.h"
#include "platform/fl_fs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool fl_json_fail(Fl_Json *c, const char *message)
{
    if (c && !c->error[0]) snprintf(c->error, sizeof(c->error), "%s", message);
    return false;
}

/* Reject invalid UTF-8, including overlong sequences, surrogate codepoints and embedded NUL. */
static bool valid_utf8(const unsigned char *s, size_t size)
{
    for (size_t i = 0; i < size;) {
        unsigned value = s[i++], more = 0, minimum = 0;
        if (!value) return false;
        if (value < 128) continue;
        if (value >= 0xc2 && value <= 0xdf) {value &= 31; more = 1; minimum = 128;}
        else if (value >= 0xe0 && value <= 0xef) {value &= 15; more = 2; minimum = 2048;}
        else if (value >= 0xf0 && value <= 0xf4) {value &= 7; more = 3; minimum = 65536;}
        else return false;
        if (more > size - i) return false;
        while (more--) {unsigned next = s[i++]; if ((next & 0xc0) != 0x80) return false; value = (value << 6) | (next & 63);}
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

bool fl_json_utf8_valid(const char *text, size_t size)
{
    return text && valid_utf8((const unsigned char *)text,size);
}

/* Consume only JSON whitespace, not the broader C locale whitespace set. */
static void skip_space(const char **p, const char *end)
{
    while (*p<end && (**p==' ' || **p=='\t' || **p=='\r' || **p=='\n')) ++*p;
}

/* Validate JSON string syntax before cJSON can discard string lengths. */
static bool syntax_string(const char **p, const char *end)
{
    if (*p==end || *(*p)++!='"') return false;
    while (*p<end) {
        unsigned char ch=(unsigned char)*(*p)++;
        if (ch=='"') return true;
        if (ch<32) return false;
        if (ch!='\\') continue;
        if (*p==end) return false;
        ch=(unsigned char)*(*p)++;
        if (strchr("\"\\/bfnrt",ch)) continue;
        if (ch!='u' || end-*p<4) return false;
        unsigned value=0;
        for (unsigned i=0;i<4;++i) {
            ch=(unsigned char)*(*p)++;
            unsigned digit=ch>='0' && ch<='9' ? ch-'0' : ch>='a' && ch<='f' ? ch-'a'+10 : ch>='A' && ch<='F' ? ch-'A'+10 : 16;
            if (digit>15) return false;
            value=(value<<4)|digit;
        }
        if (!value) return false;
    }
    return false;
}

/* Bound parser allocations and reject permissive numeric/string forms before constructing a tree. */
static bool syntax_value(const char **p, const char *end, unsigned depth, size_t *budget)
{
    skip_space(p,end);
    if (*p==end || depth>64 || !*budget) return false;
    --*budget;
    if (**p=='"') return syntax_string(p,end);
    if (**p=='{' || **p=='[') {
        bool object=**p=='{'; char close=object ? '}' : ']'; ++*p; skip_space(p,end);
        if (*p<end && **p==close) {++*p; return true;}
        size_t count=0;
        for (;;) {
            if (object) {
                if (++count>256 || !syntax_string(p,end)) return false;
                skip_space(p,end); if (*p==end || *(*p)++!=':') return false;
            }
            if (!syntax_value(p,end,depth+1,budget)) return false;
            skip_space(p,end); if (*p==end) return false;
            if (**p==close) {++*p; return true;}
            if (*(*p)++!=',') return false;
            skip_space(p,end);
        }
    }
    const char *literal=**p=='t' ? "true" : **p=='f' ? "false" : **p=='n' ? "null" : NULL;
    if (literal) {
        size_t size=strlen(literal);
        if ((size_t)(end-*p)<size || memcmp(*p,literal,size)) return false;
        *p+=size; return true;
    }
    if (**p=='-') ++*p;
    if (*p==end) return false;
    if (**p=='0') ++*p;
    else {
        if (**p<'1' || **p>'9') return false;
        do {++*p;} while (*p<end && **p>='0' && **p<='9');
    }
    if (*p<end && **p=='.') {
        ++*p; if (*p==end || **p<'0' || **p>'9') return false;
        do {++*p;} while (*p<end && **p>='0' && **p<='9');
    }
    if (*p<end && (**p=='e' || **p=='E')) {
        ++*p; if (*p<end && (**p=='+' || **p=='-')) ++*p;
        if (*p==end || **p<'0' || **p>'9') return false;
        do {++*p;} while (*p<end && **p>='0' && **p<='9');
    }
    return true;
}

/* Check parser output independently of cJSON's permissive duplicate-member semantics. */
static bool valid_tree(Fl_Json *c, const cJSON *v, unsigned depth, size_t *budget)
{
    if (!v || depth > 64 || !*budget) return fl_json_fail(c, "JSON nesting or node budget exceeded");
    --*budget;
    if (cJSON_IsNumber(v) && !isfinite(v->valuedouble)) return fl_json_fail(c, "Nonfinite number");
    if (cJSON_IsString(v) && !valid_utf8((const unsigned char *)v->valuestring, strlen(v->valuestring))) return fl_json_fail(c, "Invalid UTF-8 string");
    for (const cJSON *item = v->child; item; item = item->next) {
        if (cJSON_IsObject(v)) {
            if (!item->string) return fl_json_fail(c, "Invalid object field");
            for (const cJSON *previous = v->child; previous != item; previous = previous->next)
                if (!strcmp(previous->string, item->string)) return fl_json_fail(c, "Duplicate JSON field");
        }
        if (!valid_tree(c, item, depth + 1, budget)) return false;
    }
    return true;
}

cJSON *fl_json_parse(Fl_Json *c, const char *text, size_t size)
{
    if (!text || !size || size > FL_DOCUMENT_LIMIT || !valid_utf8((const unsigned char *)text, size)) {
        fl_json_fail(c, "Document is empty, too large or invalid UTF-8"); return NULL;
    }
    const char *cursor=text; size_t syntax_budget=2000000;
    if (!syntax_value(&cursor,text+size,0,&syntax_budget)) {fl_json_fail(c,"Invalid JSON syntax or document budget"); return NULL;}
    skip_space(&cursor,text+size);
    if (cursor!=text+size) {fl_json_fail(c,"Trailing JSON data"); return NULL;}
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, size, &end, false);
    if (root) while (end < text + size && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) ++end;
    size_t budget = 2000000;
    if (!root || end != text + size || !valid_tree(c, root, 0, &budget)) {
        cJSON_Delete(root); fl_json_fail(c, "Malformed JSON document"); return NULL;
    }
    return root;
}

bool fl_json_add(Fl_Json *c, cJSON *parent, const char *key, cJSON *value)
{
    if (!parent || !value || (key ? !cJSON_AddItemToObject(parent, key, value) : !cJSON_AddItemToArray(parent, value))) {
        cJSON_Delete(value); return fl_json_fail(c, "Cannot allocate JSON document");
    }
    return true;
}

bool fl_json_object(Fl_Json *c, const cJSON *object, const char *fields)
{
    if (!cJSON_IsObject(object)) return fl_json_fail(c, "Expected an object");
    for (const cJSON *item = object->child; item; item = item->next) {
        bool found = false;
        for (const char *p = fields; *p;) {
            while (*p == ' ') ++p;
            const char *end = strchr(p, ' '); size_t n = end ? (size_t)(end - p) : strlen(p);
            if (strlen(item->string) == n && !memcmp(item->string, p, n)) found = true;
            p += n;
        }
        if (!found) return fl_json_fail(c, "Unknown document field");
    }
    return true;
}

const cJSON *fl_json_member(Fl_Json *c, const cJSON *object, const char *key, int type)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!v || (type && (v->type & 255) != type)) {
        if (c && !c->error[0]) snprintf(c->error, sizeof(c->error), "Missing or invalid field: %s", key);
        return NULL;
    }
    return v;
}

bool fl_json_uint(Fl_Json *c, const cJSON *v, uint64_t *out)
{
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 9007199254740991.0 && floor(v->valuedouble) == v->valuedouble) {
        *out = (uint64_t)v->valuedouble; return true;
    }
    if (cJSON_IsString(v) && v->valuestring[0]) {
        uint64_t n = 0;
        for (const char *p = v->valuestring; *p; ++p) {
            if (*p < '0' || *p > '9' || n > (UINT64_MAX - (unsigned)(*p - '0')) / 10) return fl_json_fail(c, "Invalid unsigned integer");
            n = n * 10 + (unsigned)(*p - '0');
        }
        *out = n; return true;
    }
    return fl_json_fail(c, "Expected an exact unsigned integer");
}

bool fl_json_int(Fl_Json *c, const cJSON *v, int64_t *out)
{
    bool negative = cJSON_IsString(v) && v->valuestring[0] == '-';
    cJSON positive = v ? *v : (cJSON){0};
    if (negative) ++positive.valuestring;
    if (cJSON_IsNumber(v) && v->valuedouble < 0) {positive.valuedouble = -v->valuedouble; negative = true;}
    uint64_t n;
    if (!fl_json_uint(c, &positive, &n) || n > (uint64_t)INT64_MAX + (negative ? 1u : 0u)) return fl_json_fail(c, "Signed integer overflow");
    *out = negative ? (n == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)n) : (int64_t)n;
    return true;
}

cJSON *fl_json_u64(uint64_t n) {char s[32]; snprintf(s, sizeof(s), "%llu", (unsigned long long)n); return cJSON_CreateString(s);}
cJSON *fl_json_i64(int64_t n) {char s[32]; snprintf(s, sizeof(s), "%lld", (long long)n); return cJSON_CreateString(s);}
char *fl_json_string(Fl_Json *c, const cJSON *v)
{
    if (!cJSON_IsString(v)) {fl_json_fail(c, "Expected a string"); return NULL;}
    size_t size = strlen(v->valuestring) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, v->valuestring, size); else fl_json_fail(c, "Cannot allocate string");
    return copy;
}
bool fl_json_number(Fl_Json *c, const cJSON *v, double *out)
{
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) return fl_json_fail(c, "Expected a finite number");
    *out = v->valuedouble; return true;
}

bool fl_json_save(Fl_Json *c, const char *path, const cJSON *root)
{
    if (!root || (c && c->error[0])) return false;
    char *text = cJSON_Print(root);
    if (!text) return fl_json_fail(c, "Cannot format document");
    size_t size=strlen(text);
    bool ok = size<=FL_DOCUMENT_LIMIT && fl_fs_write_atomic(path, text, size);
    cJSON_free(text);
    return ok || fl_json_fail(c, "Cannot save document");
}
