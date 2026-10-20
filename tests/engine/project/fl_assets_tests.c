#include "flow_test.h"
#include "core/fl_clock.h"
#include "core/fl_sha256.h"
#include "media/fl_media.h"
#include "media/fl_thumbnail.h"
#include "project/fl_asset_import.h"
#include "project/fl_history.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define remove_directory _rmdir
#else
#include <unistd.h>
#define remove_directory rmdir
#endif

static const unsigned char PNG[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x03,0x00,0x00,0x00,0x02,
    0x08,0x06,0x00,0x00,0x00,0x9d,0x74,0x66,0x1a,0x00,0x00,0x00,0x14,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,0xcf,0xc0,0xf0,
    0x1f,0x08,0x1b,0x40,0x14,0x03,0x32,0x07,0x00,0x7b,0x2a,0x08,0xf9,0xe2,0xc6,0xb2,0xa3,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,
    0x44,0xae,0x42,0x60,0x82};

static bool remove_entry(const char *name, bool directory, void *data);

/* Delete a scratch tree left by a previous run so every run starts empty. */
static void remove_tree(const char *path)
{
    fl_fs_list_dir(path, remove_entry, (void *)path);
    remove_directory(path);
}
static bool remove_entry(const char *name, bool directory, void *data)
{
    char child[FL_PATH_MAX];
    if (fl_fs_join(child, sizeof(child), data, name)) { if (directory) remove_tree(child); else fl_fs_remove(child); }
    return true;
}

/* Hash text and compare against a FIPS 180-4 test vector. */
static bool sha_matches(const void *data, size_t size, size_t chunk, const char *expected)
{
    Fl_Sha256 hash; unsigned char digest[32]; char hex[65];
    fl_sha256_init(&hash);
    for (size_t at = 0; at < size; at += chunk) fl_sha256_update(&hash, (const unsigned char *)data + at, size - at < chunk ? size - at : chunk);
    fl_sha256_final(&hash, digest); fl_sha256_hex(digest, hex);
    return !strcmp(hex, expected);
}

static void test_sha256(void)
{
    CHECK(sha_matches("", 0, 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(sha_matches("abc", 3, 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha_matches(two_blocks, strlen(two_blocks), 7, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    char *million = malloc(1000000);
    CHECK(million != NULL);
    if (million) {
        memset(million, 'a', 1000000);
        CHECK(sha_matches(million, 1000000, 4093, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
        free(million);
    }
}

static void test_sniff(void)
{
    Fl_MediaFormat f;
    CHECK(fl_media_sniff(PNG, sizeof(PNG), &f) && f.kind == FL_ASSET_IMAGE && !strcmp(f.extension, "png"));
    CHECK(fl_media_sniff((const unsigned char *)"\xff\xd8\xff\xe0", 4, &f) && !strcmp(f.extension, "jpg"));
    CHECK(fl_media_sniff((const unsigned char *)"\x00\x00\x00\x18" "ftypmp42", 12, &f) && f.kind == FL_ASSET_VIDEO);
    CHECK(fl_media_sniff((const unsigned char *)"\x00\x00\x00\x18" "ftypqt  ", 12, &f) && !strcmp(f.extension, "mov"));
    CHECK(fl_media_sniff((const unsigned char *)"\x00\x00\x00\x18" "ftypM4A ", 12, &f) && f.kind == FL_ASSET_AUDIO);
    CHECK(fl_media_sniff((const unsigned char *)"RIFF\0\0\0\0WAVE", 12, &f) && !strcmp(f.extension, "wav"));
    CHECK(fl_media_sniff((const unsigned char *)"OTTO", 4, &f) && f.kind == FL_ASSET_FONT);
    CHECK(fl_media_sniff((const unsigned char *)"\x1a\x45\xdf\xa3", 4, &f) && f.kind == FL_ASSET_VIDEO);
    CHECK(!fl_media_sniff((const unsigned char *)"hello world", 11, &f));
    CHECK(!fl_media_sniff((const unsigned char *)"\x89PN", 3, &f));
    CHECK(!fl_media_sniff(NULL, 0, &f));
}

/* Write bytes to dir/name, returning the joined path in out. */
static bool write_file(const char *dir, const char *name, const void *data, size_t size, char out[FL_PATH_MAX])
{
    return fl_fs_join(out, FL_PATH_MAX, dir, name) && fl_fs_write_atomic(out, data, size);
}

static void test_store(const char *root)
{
    char project[FL_PATH_MAX], sources[FL_PATH_MAX], png[FL_PATH_MAX], text[FL_PATH_MAX], stored[FL_PATH_MAX];
    CHECK(fl_fs_join(project, sizeof(project), root, "store") && fl_fs_mkdir_p(project));
    CHECK(fl_fs_join(sources, sizeof(sources), root, "sources") && fl_fs_mkdir_p(sources));
    CHECK(write_file(sources, "picture.PNG", PNG, sizeof(PNG), png));
    CHECK(write_file(sources, "notes.txt", "plain text", 10, text));
    Fl_StoredAsset a, b;
    CHECK(fl_asset_store_file(project, png, &a) == FL_IMPORT_ADDED);
    CHECK(a.kind == FL_ASSET_IMAGE && a.width == 3 && a.height == 2 && a.size == sizeof(PNG) && a.created && !strcmp(a.extension, "png"));
    CHECK(fl_asset_store_path(project, a.hash, a.extension, stored, sizeof(stored)) && fl_fs_file_size(stored) == (int64_t)sizeof(PNG));
    CHECK(fl_asset_store_file(project, png, &b) == FL_IMPORT_ADDED && !b.created && !memcmp(a.hash, b.hash, 32));
    CHECK(fl_asset_store_file(project, text, &b) == FL_IMPORT_UNSUPPORTED);
    char fake[FL_PATH_MAX]; unsigned char broken[sizeof(PNG)]; memcpy(broken, PNG, 16); memset(broken + 16, 0, sizeof(PNG) - 16);
    CHECK(write_file(sources, "broken.png", broken, sizeof(broken), fake));
    CHECK(fl_asset_store_file(project, fake, &b) == FL_IMPORT_UNSUPPORTED);
    CHECK(write_file(sources, "fake.ttf", "\x00\x01\x00\x00garbage", 11, fake));
    CHECK(fl_asset_store_file(project, fake, &b) == FL_IMPORT_UNSUPPORTED);
    CHECK(fl_fs_join(fake, sizeof(fake), sources, "missing.png") && fl_asset_store_file(project, fake, &b) == FL_IMPORT_FAILED);
#ifdef FLOW_TEST_FONT
    CHECK(fl_asset_store_file(project, FLOW_TEST_FONT, &b) == FL_IMPORT_ADDED && b.kind == FL_ASSET_FONT && !strcmp(b.extension, "ttf"));
#endif
}

/* Count directory entries whose names start with prefix. */
typedef struct Fl_Count { const char *prefix; size_t count; } Fl_Count;
static bool count_entry(const char *name, bool directory, void *data)
{
    Fl_Count *c = data; (void)directory;
    if (!strncmp(name, c->prefix, strlen(c->prefix))) ++c->count;
    return true;
}

static void test_project_assets(const char *root)
{
    char dir[FL_PATH_MAX], source[FL_PATH_MAX], stored[FL_PATH_MAX], keep[FL_PATH_MAX], orphan[FL_PATH_MAX], temp[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), root, "project"));
    Fl_Project *p = fl_project_create(dir, "Assets", NULL);
    CHECK(p != NULL); if (!p) return;
    CHECK(write_file(root, "logo.png", PNG, sizeof(PNG), source));
    Fl_StoredAsset s;
    CHECK(fl_asset_store_file(dir, source, &s) == FL_IMPORT_ADDED);
    Fl_Asset record = {.name = "Logo", .kind = s.kind, .extension = s.extension, .size = s.size, .width = s.width, .height = s.height};
    memcpy(record.hash, s.hash, 32);
    Fl_Id id = 0, other = 0;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &record}, &id) && id);
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &record}, &other));
    Fl_Asset invalid = record; invalid.hash[0] ^= 1; invalid.extension = "PNG";
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &invalid}, NULL));
    invalid.extension = "png"; invalid.name = "";
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &invalid}, NULL));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_RENAME, .asset = id, .name = "Brand"}, NULL));
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_RENAME, .asset = id, .name = ""}, NULL));
    CHECK(fl_project_asset_by_hash(p, s.hash) == fl_project_asset(p, id) && !strcmp(fl_project_asset(p, id)->name, "Brand"));
    CHECK(fl_project_save(p));
    fl_project_destroy(p);
    CHECK(fl_fs_join(keep, sizeof(keep), dir, "assets/user-notes.png") && fl_fs_write_atomic(keep, "x", 1));
    CHECK(fl_fs_join(orphan, sizeof(orphan), dir, "assets/0000000000000000000000000000000000000000000000000000000000000000.png") &&
          fl_fs_write_atomic(orphan, "x", 1));
    CHECK(fl_fs_join(temp, sizeof(temp), dir, "assets/.import-1-2-3.tmp") && fl_fs_write_atomic(temp, "x", 1));
    p = fl_project_open(dir, NULL);
    CHECK(p != NULL); if (!p) return;
    CHECK(fl_fs_exists(keep) && !fl_fs_exists(orphan) && !fl_fs_exists(temp));
    const Fl_Asset *loaded = fl_project_asset(p, id);
    CHECK(loaded && loaded->kind == FL_ASSET_IMAGE && loaded->width == 3 && loaded->height == 2 && !strcmp(loaded->extension, "png"));
    CHECK(fl_project_asset_path(p, loaded, stored, sizeof(stored)) && fl_fs_exists(stored));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = id}, NULL));
    CHECK(!fl_project_asset(p, id) && fl_fs_exists(stored));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &record}, &other));
    CHECK(fl_project_save(p) && fl_fs_exists(stored));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = other}, NULL));
    CHECK(fl_project_save(p) && fl_fs_exists(stored) && fl_project_asset_count(p) == 0);
    char undo_label[FL_HISTORY_LABEL_MAX]; bool conflict=false;
    CHECK(fl_history_undo(fl_project_history(p),p,undo_label,&conflict) && fl_project_asset(p,other) && fl_fs_exists(stored));
    CHECK(fl_history_redo(fl_project_history(p),p,undo_label,&conflict) && !fl_project_asset(p,other));
    fl_history_clear(fl_project_history(p));
    CHECK(fl_project_save(p) && !fl_fs_exists(stored));
    fl_project_destroy(p);
}

static void test_thumbnails(const char *root)
{
    char dir[FL_PATH_MAX], source[FL_PATH_MAX], cache[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), root, "thumbs"));
    Fl_Project *p = fl_project_create(dir, "Thumbs", NULL);
    CHECK(p != NULL); if (!p) return;
    CHECK(write_file(root, "thumb.png", PNG, sizeof(PNG), source));
    Fl_StoredAsset s;
    CHECK(fl_asset_store_file(dir, source, &s) == FL_IMPORT_ADDED);
    Fl_Asset record = {.name = "Thumb", .kind = s.kind, .extension = s.extension, .size = s.size, .width = s.width, .height = s.height};
    memcpy(record.hash, s.hash, 32);
    Fl_Id id;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &record}, &id));
    Fl_ThumbnailSource src;
    CHECK(fl_thumbnail_source(p, fl_project_asset(p, id), &src));
    uint32_t w = 0, h = 0, w2 = 0, h2 = 0;
    unsigned char *first = fl_thumbnail_get(&src, 2, &w, &h), *second = fl_thumbnail_get(&src, 2, &w2, &h2);
    CHECK(first && second && w == 1 && h == 1 && w2 == w && h2 == h && !memcmp(first, second, 4));
    free(first); free(second);
    Fl_Count cached = {"", 0};
    CHECK(fl_fs_join(cache, sizeof(cache), dir, ".flow/cache/thumbnails") && fl_fs_list_dir(cache, count_entry, &cached) && cached.count == 1);
    Fl_ThumbnailSource audio = src; audio.kind = FL_ASSET_AUDIO;
    CHECK(!fl_thumbnail_get(&audio, 64, &w, &h));
#ifdef FLOW_TEST_FONT
    Fl_StoredAsset f;
    CHECK(fl_asset_store_file(dir, FLOW_TEST_FONT, &f) == FL_IMPORT_ADDED);
    Fl_ThumbnailSource font = src; font.kind = FL_ASSET_FONT; memcpy(font.hash, f.hash, 32);
    snprintf(font.extension, sizeof(font.extension), "%s", f.extension);
    unsigned char *specimen = fl_thumbnail_get(&font, 96, &w, &h);
    CHECK(specimen && w > h && w <= 96 && specimen[3] == 0 && specimen[0] == 255);
    free(specimen);
#endif
    CHECK(fl_project_save(p));
    fl_project_destroy(p);
    p = fl_project_open(dir, NULL);
    cached.count = 0;
    CHECK(p && fl_fs_list_dir(cache, count_entry, &cached) && cached.count == 1);
    fl_project_destroy(p);
}

typedef struct Fl_Results { Fl_AssetImported events[8]; size_t count; } Fl_Results;
static bool collect(const Fl_Event *event, void *data)
{
    Fl_Results *r = data;
    if (r->count < 8) r->events[r->count++] = *(const Fl_AssetImported *)event->payload;
    return false;
}

static void test_importer(const char *root)
{
    char dir[FL_PATH_MAX], png[FL_PATH_MAX], text[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), root, "async"));
    CHECK(write_file(root, "async.png", PNG, sizeof(PNG), png) && write_file(root, "async.txt", "nope", 4, text));
    Fl_EventBus *bus = fl_event_bus_create(&(Fl_EventBusDesc){0});
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){.worker_count = 2, .bus = bus});
    Fl_Project *p = fl_project_create(dir, "Async", bus);
    Fl_AssetImporter *im = fl_asset_importer_create(p, jobs, bus);
    CHECK(bus && jobs && p && im);
    if (!bus || !jobs || !p || !im) return;
    Fl_Results results = {0};
    CHECK(fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(bus, FL_EVENT_ASSET_IMPORTED), .fn = collect, .user_data = &results}).generation);
    uint64_t a = fl_asset_import(im, png), b = fl_asset_import(im, text), c = fl_asset_import(im, png);
    CHECK(a && b && c && a != b && fl_asset_importer_pending(im) == 3);
    CHECK(!fl_asset_import(im, "") && !fl_asset_import(NULL, png));
    for (uint64_t deadline = fl_clock_monotonic_ns() + UINT64_C(10000000000); results.count < 3 && fl_clock_monotonic_ns() < deadline; )
        fl_event_bus_dispatch(bus);
    CHECK(results.count == 3 && fl_asset_importer_pending(im) == 0);
    size_t added = 0, existing = 0, unsupported = 0;
    for (size_t i = 0; i < results.count; ++i) {
        added += results.events[i].status == FL_IMPORT_ADDED;
        existing += results.events[i].status == FL_IMPORT_EXISTING;
        unsupported += results.events[i].status == FL_IMPORT_UNSUPPORTED && results.events[i].request == b;
        if (results.events[i].status == FL_IMPORT_ADDED) CHECK(!strcmp(fl_project_asset(p, results.events[i].asset)->name, "async"));
    }
    CHECK(added == 1 && existing == 1 && unsupported == 1 && fl_project_asset_count(p) == 1);
    uint64_t late = fl_asset_import(im, png);
    CHECK(late != 0);
    fl_asset_importer_destroy(im);
    char folder[FL_PATH_MAX]; Fl_Count stray = {".import-", 0};
    CHECK(fl_fs_join(folder, sizeof(folder), dir, "assets") && fl_fs_list_dir(folder, count_entry, &stray) && stray.count == 0);
    CHECK(fl_project_asset_count(p) == 1);
    fl_project_destroy(p); fl_jobs_destroy(jobs); fl_event_bus_destroy(bus);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); if (argc != 2) return 1;
    remove_tree(argv[1]);
    CHECK(fl_fs_mkdir_p(argv[1]));
    test_sha256();
    test_sniff();
    test_store(argv[1]);
    test_project_assets(argv[1]);
    test_importer(argv[1]);
    test_thumbnails(argv[1]);
    return FLOW_TEST_RESULT("fl_assets_tests");
}
