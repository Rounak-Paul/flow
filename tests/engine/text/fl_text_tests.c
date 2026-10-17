#include "flow_test.h"
#include "core/fl_clock.h"
#include "project/fl_asset_import.h"
#include "platform/fl_fs.h"
#include "text/fl_embedded_fonts.h"
#include "text/fl_fonts.h"
#include "text/fl_text_raster.h"
#include <stdlib.h>
#include <string.h>

static void test_matching(void)
{
    const Fl_FontFace faces[] = {
        {.family = "F", .weight = 300}, {.family = "F", .weight = 400}, {.family = "F", .weight = 400, .italic = true},
        {.family = "F", .weight = 600}, {.family = "F", .weight = 900}};
    CHECK(fl_font_match_faces(faces, 5, 400, false) == &faces[1]);
    CHECK(fl_font_match_faces(faces, 5, 400, true) == &faces[2]);
    CHECK(fl_font_match_faces(faces, 5, 700, true) == &faces[2]);
    CHECK(fl_font_match_faces(faces, 5, 500, false) == &faces[1]);
    CHECK(fl_font_match_faces(faces, 5, 700, false) == &faces[4]);
    CHECK(fl_font_match_faces(faces, 5, 200, false) == &faces[0]);
    CHECK(fl_font_match_faces(faces, 5, 350, false) == &faces[0]);
    CHECK(fl_font_match_faces(faces, 5, 450, false) == &faces[1]);
    const Fl_FontFace light[] = {{.family = "L", .weight = 300}, {.family = "L", .weight = 700}};
    CHECK(fl_font_match_faces(light, 2, 450, false) == &light[0]);
    CHECK(fl_font_match_faces(light, 2, 550, false) == &light[1]);
    CHECK(!fl_font_match_faces(NULL, 0, 400, false));
    CHECK(fl_font_family_compare("Inter", "iNTER") == 0 && fl_font_family_compare("Arial", "Inter") < 0);
}

static void test_catalog(void)
{
    Fl_FontCatalog *memory = fl_font_catalog_from_memory(fl_embedded_inter_regular, fl_embedded_inter_regular_size);
    CHECK(memory && fl_font_catalog_family_count(memory) == 1 && !strcmp(fl_font_catalog_family(memory, 0), "Inter"));
    const Fl_FontFace *face = fl_font_catalog_match(memory, "inter", 400, false);
    CHECK(face && !face->path && face->data == fl_embedded_inter_regular && face->weight == 400 && !face->italic);
    CHECK(!fl_font_catalog_match(memory, "Missing", 400, false) && !fl_font_catalog_family(memory, 1));
    fl_font_catalog_destroy(memory);
    CHECK(!fl_font_catalog_from_memory(NULL, 0));
#ifdef FLOW_TEST_FONT
    const char *paths[] = {FLOW_TEST_FONT, FLOW_TEST_FONT, "/nonexistent/font.ttf"};
    Fl_FontCatalog *files = fl_font_catalog_from_files(paths, 3);
    const Fl_FontFace *faces;
    CHECK(files && fl_font_catalog_faces(files, "Inter", &faces) == 1 && faces[0].path && !strcmp(faces[0].path, FLOW_TEST_FONT));
    fl_font_catalog_destroy(files);
    CHECK(fl_font_file_valid(FLOW_TEST_FONT) && !fl_font_file_valid("/nonexistent/font.ttf"));
#endif
}

/* Sum coverage to compare rasterized ink between styles. */
static uint64_t ink(const unsigned char *mask, uint32_t w, uint32_t h)
{
    uint64_t total = 0;
    for (size_t i = 0; mask && i < (size_t)w * h; ++i) total += mask[i];
    return total;
}

static void test_raster(void)
{
    Fl_FontCatalog *catalog = fl_font_catalog_from_memory(fl_embedded_inter_regular, fl_embedded_inter_regular_size);
    Fl_TextRasterizer *r = fl_text_rasterizer_create();
    CHECK(catalog && r);
    if (!catalog || !r) { fl_font_catalog_destroy(catalog); fl_text_rasterizer_destroy(r); return; }
    const Fl_FontFace *face = fl_font_catalog_match(catalog, "Inter", 400, false);
    const unsigned char text[] = "Flow\nText";
    uint32_t w, h, bw, bh, iw, ih;
    Fl_TextStyle regular = {face, 32, false, false}, bold = {face, 32, true, false}, italic = {face, 32, false, true};
    unsigned char *a = fl_text_rasterize(r, &regular, text, sizeof(text) - 1, 200, 100, &w, &h);
    unsigned char *b = fl_text_rasterize(r, &bold, text, sizeof(text) - 1, 200, 100, &bw, &bh);
    unsigned char *c = fl_text_rasterize(r, &italic, text, sizeof(text) - 1, 200, 100, &iw, &ih);
    CHECK(a && b && c && w == 200 && h == 100 && bw == w && iw == w);
    CHECK(ink(a, w, h) > 0 && ink(b, bw, bh) > ink(a, w, h) && (!c || !a || memcmp(a, c, (size_t)w * h)));
    size_t lower = 0;
    for (size_t y = 50; a && y < h; ++y) for (size_t x = 0; x < w; ++x) lower += a[y * w + x] != 0;
    CHECK(lower > 0);
    free(a); free(b); free(c);
    unsigned char *large = fl_text_rasterize(r, &regular, text, 4, 10000, 100, &w, &h);
    CHECK(large && w == 4096 && h == 41);
    free(large);
    unsigned char *empty = fl_text_rasterize(r, &regular, NULL, 0, 10, 10, &w, &h);
    CHECK(empty && ink(empty, w, h) == 0);
    free(empty);
    const unsigned char invalid[] = {0xff, 0xc3, 'A'};
    unsigned char *replaced = fl_text_rasterize(r, &regular, invalid, 3, 100, 50, &w, &h);
    CHECK(replaced != NULL); free(replaced);
    CHECK(!fl_text_rasterize(r, &regular, text, 4, 0, 10, &w, &h));
    CHECK(!fl_text_rasterize(r, &(Fl_TextStyle){face, 0.5, false, false}, text, 4, 10, 10, &w, &h));
    CHECK(!fl_text_rasterize(r, &(Fl_TextStyle){NULL, 12, false, false}, text, 4, 10, 10, &w, &h));
    fl_text_rasterizer_flush(r);
    unsigned char *again = fl_text_rasterize(r, &regular, text, 4, 50, 50, &w, &h);
    CHECK(again != NULL); free(again);
    fl_text_rasterizer_destroy(r);
    fl_font_catalog_destroy(catalog);
}

static bool count_changes(const Fl_Event *event, void *data)
{
    (void)event; ++*(int *)data; return false;
}

static void test_service(const char *root)
{
    Fl_EventBus *bus = fl_event_bus_create(&(Fl_EventBusDesc){0});
    Fl_Fonts *fonts = fl_fonts_create(&(Fl_FontsDesc){.bus = bus});
    CHECK(bus && fonts);
    if (!bus || !fonts) return;
    CHECK(!fl_fonts_scanning(fonts) && fl_fonts_family_count(fonts) == 1);
    Fl_FontSource source;
    CHECK(!strcmp(fl_fonts_family(fonts, 0, &source), "Inter") && source == FL_FONT_SOURCE_BUNDLED);
    Fl_Typography missing = {"Definitely Not Installed", 700, true};
    Fl_FontMatch match = fl_fonts_match(fonts, &missing);
    CHECK(match.face && !match.found && match.source == FL_FONT_SOURCE_BUNDLED && match.synthetic_bold && match.synthetic_italic);
    Fl_Typography inter = {"inter", 400, false};
    match = fl_fonts_match(fonts, &inter);
    CHECK(match.found && !match.synthetic_bold && !match.synthetic_italic);
    int changes = 0;
    CHECK(fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(bus, FL_EVENT_FONTS_CHANGED), .fn = count_changes, .user_data = &changes}).generation);
#ifdef FLOW_TEST_FONT
    char dir[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), root, "fonts-project"));
    char manifest[FL_PATH_MAX];
    CHECK(fl_fs_join(manifest, sizeof(manifest), dir, "project.flow")); fl_fs_remove(manifest);
    Fl_Project *p = fl_project_create(dir, "Fonts", bus);
    CHECK(p != NULL);
    if (p) {
        uint64_t generation = fl_fonts_generation(fonts);
        fl_fonts_set_project(fonts, p);
        CHECK(fl_fonts_generation(fonts) == generation + 1 && changes == 1);
        Fl_StoredAsset stored;
        CHECK(fl_asset_store_file(dir, FLOW_TEST_FONT, &stored) == FL_IMPORT_ADDED);
        Fl_Asset record = {.name = "Inter", .kind = FL_ASSET_FONT, .extension = stored.extension, .size = stored.size};
        memcpy(record.hash, stored.hash, 32);
        Fl_Id id;
        CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &record}, &id));
        CHECK(changes == 2);
        match = fl_fonts_match(fonts, &inter);
        CHECK(match.found && match.source == FL_FONT_SOURCE_PROJECT && match.face->path);
        CHECK(fl_fonts_family_count(fonts) == 1 && !strcmp(fl_fonts_family(fonts, 0, &source), "Inter") && source == FL_FONT_SOURCE_PROJECT);
        const Fl_FontFace *faces;
        CHECK(fl_fonts_faces(fonts, "Inter", &faces) == 1 && faces[0].path);
        CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = id}, NULL));
        CHECK(changes == 3 && fl_fonts_match(fonts, &inter).source == FL_FONT_SOURCE_BUNDLED);
        fl_fonts_set_project(fonts, NULL);
        fl_project_destroy(p);
    }
#else
    (void)root;
#endif
    fl_fonts_destroy(fonts);
    fl_event_bus_destroy(bus);
}

static void test_system_scan(void)
{
    Fl_EventBus *bus = fl_event_bus_create(&(Fl_EventBusDesc){0});
    Fl_JobSystem *jobs = fl_jobs_create(&(Fl_JobSystemDesc){.worker_count = 1, .bus = bus});
    Fl_Fonts *fonts = fl_fonts_create(&(Fl_FontsDesc){.bus = bus, .jobs = jobs, .scan_system = true});
    CHECK(fonts && fl_fonts_scanning(fonts));
    for (uint64_t deadline = fl_clock_monotonic_ns() + UINT64_C(60000000000); fonts && fl_fonts_scanning(fonts) && fl_clock_monotonic_ns() < deadline; )
        fl_event_bus_dispatch(bus);
    CHECK(fonts && !fl_fonts_scanning(fonts));
    printf("system scan: %zu families\n", fl_fonts_family_count(fonts));
    fl_fonts_destroy(fonts);
    Fl_Fonts *abandoned = fl_fonts_create(&(Fl_FontsDesc){.bus = bus, .jobs = jobs, .scan_system = true});
    fl_fonts_destroy(abandoned);
    CHECK(!fl_fonts_create(&(Fl_FontsDesc){.bus = bus, .scan_system = true}));
    fl_jobs_destroy(jobs);
    fl_event_bus_destroy(bus);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); if (argc != 2) return 1;
    CHECK(fl_fs_mkdir_p(argv[1]));
    test_matching();
    test_catalog();
    test_raster();
    test_service(argv[1]);
    test_system_scan();
    return FLOW_TEST_RESULT("fl_text_tests");
}
