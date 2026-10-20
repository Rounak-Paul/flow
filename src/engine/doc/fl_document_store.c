#include "fl_document_store.h"
#include "core/fl_sha256.h"
#include "platform/fl_fs.h"
#include <stdlib.h>
#include <string.h>

bool fl_document_path_valid(const char *path)
{
    if (!path || !*path || path[0]=='/' || strchr(path,'\\') || strchr(path,':') || strlen(path)>=FL_PATH_MAX) return false;
    const char *part=path;
    for (const char *p=path;;++p) if (*p=='/' || !*p) {
        size_t size=(size_t)(p-part);
        if (!size || (size==1 && part[0]=='.') || (size==2 && part[0]=='.' && part[1]=='.')) return false;
        if (!*p) break;
        part=p+1;
    }
    return !strcmp(path,"project.flow.json") || !strncmp(path,"scenes/",7) || !strncmp(path,"assets/",7);
}

bool fl_document_fingerprint(const char *path, unsigned char out[32], bool *existed)
{
    memset(out,0,32); *existed=fl_fs_exists(path);
    if (!*existed) return true;
    FILE *file=fl_fs_fopen(path,"rb"); if (!file) return false;
    Fl_Sha256 hash; fl_sha256_init(&hash); unsigned char buffer[65536]; size_t size;
    while ((size=fread(buffer,1,sizeof(buffer),file))) fl_sha256_update(&hash,buffer,size);
    bool ok=!ferror(file); if (fclose(file)) ok=false;
    if (ok) fl_sha256_final(&hash,out);
    return ok;
}

/* Hash an immutable replacement buffer. */
static void fingerprint_text(const char *text, size_t size, unsigned char out[32])
{
    Fl_Sha256 hash; fl_sha256_init(&hash); fl_sha256_update(&hash,text,size); fl_sha256_final(&hash,out);
}

/* Compare expected bytes or an already-applied replacement without overwriting an external edit. */
static bool can_replace(Fl_Json *c, const char *directory, const Fl_DocumentWrite *w, bool allow_applied, bool *applied)
{
    char path[FL_PATH_MAX]; unsigned char hash[32], next[32]; bool exists;
    *applied=false;
    if (!fl_document_path_valid(w->path) || !fl_fs_join(path,sizeof(path),directory,w->path) || !fl_document_fingerprint(path,hash,&exists)) return fl_json_fail(c,"Cannot inspect document file");
    fingerprint_text(w->text,w->size,next);
    if (allow_applied && exists && !memcmp(hash,next,32)) {*applied=true; return true;}
    if (exists!=w->existed || (exists && memcmp(hash,w->expected,32))) return fl_json_fail(c,"External edit conflicts with document save");
    return true;
}

/* Apply a checked prepared file set; the intent remains available after any failure. */
static bool finish(Fl_Json *c, const char *directory, const Fl_DocumentWrite *writes, size_t count)
{
    bool applied;
    for (size_t i=0;i<count;++i) if (!can_replace(c,directory,&writes[i],true,&applied)) return false;
    for (size_t i=0;i<count;++i) {
        if (!can_replace(c,directory,&writes[i],true,&applied)) return false;
        if (applied) continue;
        char path[FL_PATH_MAX], parent[FL_PATH_MAX];
        if (!fl_fs_join(path,sizeof(path),directory,writes[i].path)) return fl_json_fail(c,"Document path too long");
        strcpy(parent,path); char *slash=strrchr(parent,'/');
#ifdef _WIN32
        char *backslash=strrchr(parent,'\\'); if (backslash && (!slash || backslash>slash)) slash=backslash;
#endif
        if (slash) *slash=0;
        if (!slash || !fl_fs_mkdir_p(parent) || !fl_fs_write_atomic(path,writes[i].text,writes[i].size)) return fl_json_fail(c,"Cannot commit document file");
    }
    char intent[FL_PATH_MAX];
    return (fl_fs_join(intent,sizeof(intent),directory,".flow/local/pending.json") && fl_fs_remove(intent)) || fl_json_fail(c,"Cannot clear completed document transaction");
}

bool fl_document_commit(Fl_Json *c, const char *directory, const Fl_DocumentWrite *writes, size_t count)
{
    char path[FL_PATH_MAX]; bool applied;
    if (!count || !fl_fs_join(path,sizeof(path),directory,".flow/local") || !fl_fs_mkdir_p(path) ||
        !fl_fs_join(path,sizeof(path),directory,".flow/local/pending.json") || fl_fs_exists(path)) return fl_json_fail(c,"Unresolved document transaction");
    cJSON *root=cJSON_CreateObject(), *files=cJSON_CreateArray();
    fl_json_add(c,root,"version",cJSON_CreateNumber(1));
    for (size_t i=0;i<count && !c->error[0];++i) {
        const Fl_DocumentWrite *w=&writes[i];
        if (!w->text || strlen(w->text)!=w->size) {fl_json_fail(c,"Invalid document replacement"); break;}
        if (!can_replace(c,directory,w,false,&applied)) break;
        char hash[65]; fl_sha256_hex(w->expected,hash); cJSON *item=cJSON_CreateObject();
        fl_json_add(c,item,"path",cJSON_CreateString(w->path)); fl_json_add(c,item,"existed",cJSON_CreateBool(w->existed));
        fl_json_add(c,item,"expected",cJSON_CreateString(hash)); fl_json_add(c,item,"text",cJSON_CreateString(w->text)); fl_json_add(c,files,NULL,item);
    }
    fl_json_add(c,root,"files",files);
    bool ok=!c->error[0] && fl_json_save(c,path,root);
    cJSON_Delete(root);
    return ok && finish(c,directory,writes,count);
}

bool fl_document_recover(Fl_Json *c, const char *directory)
{
    char path[FL_PATH_MAX];
    if (!fl_fs_join(path,sizeof(path),directory,".flow/local/pending.json")) return false;
    if (!fl_fs_exists(path)) return true;
    size_t size; char *text=fl_fs_read_file(path,FL_DOCUMENT_LIMIT,&size);
    cJSON *root=text ? fl_json_parse(c,text,size) : NULL; free(text);
    if (!root) return fl_json_fail(c,"Cannot read document recovery intent");
    uint64_t version=0;
    bool ok=fl_json_object(c,root,"version files") && fl_json_uint(c,fl_json_member(c,root,"version",0),&version) && version==1;
    const cJSON *files=fl_json_member(c,root,"files",cJSON_Array);
    size_t count=files ? (size_t)cJSON_GetArraySize(files) : 0;
    Fl_DocumentWrite *writes=count ? calloc(count,sizeof(*writes)) : NULL;
    if (!count || !writes) ok=false;
    size_t i=0;
    for (const cJSON *v=files ? files->child : NULL;ok && v;v=v->next,++i) {
        if (!fl_json_object(c,v,"path existed expected text")) {ok=false; break;}
        const cJSON *name=fl_json_member(c,v,"path",cJSON_String), *exists=fl_json_member(c,v,"existed",0),
                    *hash=fl_json_member(c,v,"expected",cJSON_String), *body=fl_json_member(c,v,"text",cJSON_String);
        if (!name || !cJSON_IsBool(exists) || !hash || strlen(hash->valuestring)!=64 || !body || !fl_document_path_valid(name->valuestring)) {ok=false; break;}
        Fl_DocumentWrite *w=&writes[i]; w->path=name->valuestring; w->existed=cJSON_IsTrue(exists); w->text=body->valuestring; w->size=strlen(w->text);
        for (size_t j=0;j<i;++j) if (!strcmp(writes[j].path,w->path)) ok=false;
        for (size_t j=0;j<64;++j) {
            char digit=hash->valuestring[j]; unsigned n=digit>='0' && digit<='9' ? (unsigned)(digit-'0') : digit>='a' && digit<='f' ? (unsigned)(digit-'a'+10) : 16;
            if (n>15) {ok=false; break;}
            w->expected[j/2]=(unsigned char)((w->expected[j/2]<<4)|n);
        }
    }
    ok=ok && !c->error[0] && finish(c,directory,writes,count);
    free(writes); cJSON_Delete(root);
    return ok || fl_json_fail(c,"Document recovery conflict or invalid intent");
}
