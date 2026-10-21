#include "flow_test.h"
#include "core/fl_clock.h"
#include "media/fl_audio.h"
#include "media/fl_audio_mixer.h"
#include "media/fl_audio_player.h"
#include "media/fl_thumbnail.h"
#include "project/fl_asset_import.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "project/fl_prototype.h"
#include "project/fl_prototype_authoring.h"
#include "components/fl_prototype_instance.h"
#include "time/fl_audio_plan.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define remove_directory _rmdir
#else
#include <unistd.h>
#define remove_directory rmdir
#endif

#define RATE 44100u
#define FRAMES 11025u

static bool remove_entry(const char *name, bool directory, void *data);

/* Delete the scratch tree of a previous run. */
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

/* Append a little-endian integer of size bytes. */
static void put(unsigned char **at, uint32_t value, int size)
{
    for (int i = 0; i < size; ++i) *(*at)++ = (unsigned char)(value >> (8 * i));
}

/* Write a 16-bit stereo PCM WAV holding a half-amplitude 440 Hz sine. */
static bool write_wav(const char *path)
{
    size_t data = (size_t)FRAMES * 4, size = 44 + data;
    unsigned char *wav = malloc(size), *at = wav;
    if (!wav) return false;
    memcpy(at, "RIFF", 4); at += 4; put(&at, (uint32_t)(size - 8), 4); memcpy(at, "WAVEfmt ", 8); at += 8;
    put(&at, 16, 4); put(&at, 1, 2); put(&at, 2, 2); put(&at, RATE, 4); put(&at, RATE * 4, 4); put(&at, 4, 2); put(&at, 16, 2);
    memcpy(at, "data", 4); at += 4; put(&at, (uint32_t)data, 4);
    for (uint32_t i = 0; i < FRAMES; ++i) {
        int16_t v = (int16_t)lrint(sin(i * 2 * 3.14159265358979 * 440 / RATE) * 16383);
        put(&at, (uint16_t)v, 2); put(&at, (uint16_t)v, 2);
    }
    bool ok = fl_fs_write_atomic(path, wav, size);
    free(wav);
    return ok;
}

static void test_decode(const char *wav, const char *broken)
{
    Fl_AudioInfo info;
    CHECK(fl_audio_probe(wav, &info) && info.sample_rate == RATE && info.channels == 2 && info.frames == FRAMES);
    Fl_AudioReader *r = fl_audio_open(wav, &info);
    CHECK(r != NULL); if (!r) return;
    float *buffer = malloc((size_t)FRAMES * 2 * sizeof(float));
    uint64_t total = 0, n; float peak = 0;
    while (buffer && (n = fl_audio_read(r, buffer, 1000)) > 0) {
        for (uint64_t i = 0; i < n * 2; ++i) if (fabsf(buffer[i]) > peak) peak = fabsf(buffer[i]);
        total += n;
    }
    CHECK(total == FRAMES && peak > 0.49f && peak < 0.51f);
    CHECK(fl_audio_seek(r, 0) && fl_audio_read(r, buffer, 10) == 10);
    free(buffer); fl_audio_close(r);
    float peaks[2 * 32];
    CHECK(fl_audio_waveform(wav, peaks, 32));
    for (int i = 0; i < 32; ++i) CHECK(peaks[2 * i] < -0.45f && peaks[2 * i + 1] > 0.45f && peaks[2 * i + 1] <= 0.51f);
    CHECK(!fl_audio_probe(broken, &info) && !fl_audio_open(broken, &info) && !fl_audio_waveform(broken, peaks, 32));
    CHECK(!fl_audio_open("/nonexistent/sound.wav", &info));
}

/* Count entries in the project's asset store. */
static bool count(const char *name, bool directory, void *data) { (void)name; *(size_t *)data += !directory; return true; }

/* Prototype audio shares picture clocks, loops, trimmed envelopes and deterministic PCM rendering. */
static void test_prototype_audio(Fl_Project *project, Fl_Id asset, const char *directory)
{
    Fl_Id scene=fl_project_scene_at(project,0)->id;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_SCENE_SETTINGS,.scene=scene,.width=100,.height=100,.duration=2*FL_TICKS_PER_SECOND},NULL));
    unsigned char bytes[56]; fl_transform_encode(&(Fl_Transform){.width=10,.height=10,.scale_x=1,.scale_y=1},bytes);
    Fl_Component transform={.id=1,.type=FL_COMPONENT_TRANSFORM,.version=1,.data=bytes,.size=56};
    Fl_Entity root={.id=1,.name="Sound",.visible=true,.components=&transform,.component_count=1};
    Fl_AudioClip clip={.id=2,.asset=asset,.start={.offset=FL_TICKS_PER_SECOND/10},.duration=FL_TICKS_PER_SECOND/5,.gain=0.5,.fade_in=FL_TICKS_PER_SECOND/20,.fade_out=FL_TICKS_PER_SECOND/20};
    Fl_Prototype prototype={.root=1,.next_id=3,.next_component_id=2,.scope={.duration=FL_TICKS_PER_SECOND,.entities=&root,.entity_count=1,.clips=&clip,.clip_count=1}};
    Fl_Json json={0}; cJSON *document=fl_prototype_write(&json,&prototype); char *source=document ? cJSON_PrintUnformatted(document) : NULL; cJSON_Delete(document);
    Fl_Id resource=0,owner=0;
    CHECK(source && fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_PROTOTYPE,.name="Audio prototype",.source=source,.source_size=strlen(source)},&resource)); free(source);
    CHECK(fl_prototype_instantiate(project,scene,0,resource,0,0,&owner));
    Fl_AudioPlan plan={0}; CHECK(fl_audio_plan_compile(fl_project_scene(project,scene),&plan) && plan.count==1);
    Fl_MixerClip resolved={0};
    if (plan.count==1) {
        CHECK(fabs(plan.segments[0].start-0.2)<1e-9 && fabs(plan.segments[0].duration-0.4)<1e-9 && plan.segments[0].rate==0.5);
        CHECK(fl_audio_segment_clip(project,&plan.segments[0],0,&resolved));
    }
    fl_audio_plan_clear(&plan);
    char output[FL_PATH_MAX],again[FL_PATH_MAX]; CHECK(fl_fs_join(output,sizeof(output),directory,"prototype.rf64") && fl_fs_join(again,sizeof(again),directory,"repeated.rf64"));
    CHECK(fl_audio_mixer_file(output,&resolved,1,2*FL_TICKS_PER_SECOND,48000,NULL));
    CHECK(fl_audio_mixer_file(again,&resolved,1,2*FL_TICKS_PER_SECOND,48000,NULL));
    size_t first_size=0,second_size=0; char *first=fl_fs_read_file(output,2*1024*1024,&first_size),*second=fl_fs_read_file(again,2*1024*1024,&second_size);
    CHECK(first && second && first_size==second_size && !memcmp(first,second,first_size)); free(first); free(second);
    Fl_AudioInfo info={0}; Fl_AudioReader *reader=fl_audio_open(output,&info); CHECK(reader && info.frames==96000 && info.channels==2);
    float samples[256*2];
    if (reader) {
        CHECK(fl_audio_read(reader,samples,256)==256);
        for (size_t i=0;i<512;++i) CHECK(samples[i]==0);
        CHECK(fl_audio_seek(reader,14400) && fl_audio_read(reader,samples,256)==256);
        double energy=0; for (size_t i=0;i<512;++i) energy+=fabs(samples[i]); CHECK(energy>20);
        CHECK(fl_audio_seek(reader,38400) && fl_audio_read(reader,samples,256)==256);
        for (size_t i=0;i<512;++i) CHECK(samples[i]==0); fl_audio_close(reader);
    }
    Fl_PrototypeInstance instance={.asset=resource,.time=fl_time_mapping_default()}; instance.time.rate=4; instance.time.loop=true;
    Fl_Component component={0}; CHECK(fl_prototype_instance_encode(&instance,&component));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=owner,.component=component},NULL)); fl_component_clear(&component);
    CHECK(fl_audio_plan_compile(fl_project_scene(project,scene),&plan) && plan.count==4);
    for (size_t i=0;i<plan.count;++i) CHECK(fabs(plan.segments[i].start-(0.05+0.5*i))<1e-9 && fabs(plan.segments[i].duration-0.1)<1e-9 && plan.segments[i].rate==2);
    fl_audio_plan_clear(&plan);
    atomic_bool cancel=true; CHECK(fl_fs_remove(again));
    CHECK(!fl_audio_mixer_file(again,&resolved,1,2*FL_TICKS_PER_SECOND,48000,&cancel) && !fl_fs_exists(again));
}

static void test_import(const char *root, const char *wav, const char *broken)
{
    char dir[FL_PATH_MAX], store[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), root, "audio-project"));
    Fl_Project *p = fl_project_create(dir, "Audio", NULL);
    CHECK(p != NULL); if (!p) return;
    Fl_StoredAsset s;
    CHECK(fl_asset_store_file(dir, broken, &s) == FL_IMPORT_UNSUPPORTED);
    size_t files = 0;
    CHECK(fl_fs_join(store, sizeof(store), dir, "assets") && fl_fs_list_dir(store, count, &files) && files == 0);
    CHECK(fl_asset_store_file(dir, wav, &s) == FL_IMPORT_ADDED);
    CHECK(s.kind == FL_ASSET_AUDIO && s.sample_rate == RATE && s.channels == 2 && s.duration == FL_TICKS_PER_SECOND / 4);
    Fl_Asset record = {.name = "Tone", .kind = s.kind, .extension = s.extension, .size = s.size,
                       .duration = s.duration, .sample_rate = s.sample_rate, .channels = s.channels};
    memcpy(record.hash, s.hash, 32);
    Fl_Id id;
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &record}, &id));
    Fl_Asset invalid = record; invalid.hash[0] ^= 1; invalid.sample_rate = 10000000;
    CHECK(!fl_project_apply(p, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &invalid}, NULL));
    Fl_ThumbnailSource src;
    uint32_t w = 0, h = 0;
    CHECK(fl_thumbnail_source(p, fl_project_asset(p, id), &src));
    unsigned char *wave = fl_thumbnail_get(&src, 64, &w, &h);
    CHECK(wave && w == 64 && h == 32 && wave[(16 * 64 + 10) * 4 + 3] == 230);
    free(wave);
    test_prototype_audio(p,id,root);
    CHECK(fl_project_save(p));
    fl_project_destroy(p);
    p = fl_project_open(dir, NULL);
    const Fl_Asset *loaded = p ? fl_project_asset(p, id) : NULL;
    CHECK(loaded && loaded->duration == FL_TICKS_PER_SECOND / 4 && loaded->sample_rate == RATE && loaded->channels == 2);
    fl_project_destroy(p);
}

typedef struct Fl_Ended { uint64_t token; int count; } Fl_Ended;
static bool ended(const Fl_Event *event, void *data)
{
    const Fl_AudioPlayback *change = event->payload; Fl_Ended *e = data;
    if (!change->playing) { e->token = change->token; ++e->count; }
    return false;
}

static void test_player(const char *wav)
{
    Fl_EventBus *bus = fl_event_bus_create(&(Fl_EventBusDesc){0});
    Fl_AudioPlayer *player = fl_audio_player_create(&(Fl_AudioPlayerDesc){.bus = bus, .silent = true});
    CHECK(bus && player); if (!bus || !player) return;
    Fl_Ended result = {0};
    CHECK(fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = fl_event_type_find(bus, FL_EVENT_AUDIO_PLAYBACK), .fn = ended, .user_data = &result}).generation);
    CHECK(!fl_audio_player_play(player, "/nonexistent/sound.wav") && !fl_audio_player_current(player));
    uint64_t first = fl_audio_player_play(player, wav);
    CHECK(first && fl_audio_player_current(player) == first);
    fl_audio_player_stop(player);
    CHECK(!fl_audio_player_current(player));
    uint64_t second = fl_audio_player_play(player, wav);
    CHECK(second && second != first);
    for (uint64_t deadline = fl_clock_monotonic_ns() + UINT64_C(5000000000); !result.count && fl_clock_monotonic_ns() < deadline; )
        fl_event_bus_dispatch(bus);
    CHECK(result.count == 1 && result.token == second && !fl_audio_player_current(player));
    uint64_t third = fl_audio_player_play(player, wav);
    CHECK(third != 0);
    fl_audio_player_destroy(player);
    fl_event_bus_destroy(bus);
}

static void test_mixer(const char *wav)
{
    Fl_AudioMixer *mixer = fl_audio_mixer_create(&(Fl_AudioMixerDesc){.silent = true});
    CHECK(mixer != NULL); if (!mixer) return;
    Fl_Tick tick = -1;
    CHECK(!fl_audio_mixer_position(mixer, &tick));
    Fl_MixerClip clips[2] = {{.start = FL_TICKS_PER_SECOND / 10, .gain = 1, .fade_in = FL_TICKS_PER_SECOND / 20},
                             {.start = 0, .source_offset = FL_TICKS_PER_SECOND / 20, .duration = FL_TICKS_PER_SECOND / 10, .gain = 0.5}};
    snprintf(clips[0].path, sizeof(clips[0].path), "%s", wav); snprintf(clips[1].path, sizeof(clips[1].path), "%s", wav);
    Fl_Tick from = FL_TICKS_PER_SECOND / 20;
    CHECK(fl_audio_mixer_start(mixer, clips, 2, from));
    CHECK(fl_audio_mixer_position(mixer, &tick) && tick >= from);
    Fl_Tick later = tick;
    for (uint64_t deadline = fl_clock_monotonic_ns() + UINT64_C(3000000000); later < from + FL_TICKS_PER_SECOND / 10 &&
         fl_clock_monotonic_ns() < deadline; ) fl_audio_mixer_position(mixer, &later);
    CHECK(later >= from + FL_TICKS_PER_SECOND / 10);
    CHECK(fl_audio_mixer_start(mixer, clips, 2, 10 * FL_TICKS_PER_SECOND));
    fl_audio_mixer_stop(mixer);
    CHECK(!fl_audio_mixer_position(mixer, &tick));
    fl_audio_mixer_destroy(mixer);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); if (argc != 2) return 1;
    char wav[FL_PATH_MAX], broken[FL_PATH_MAX];
    remove_tree(argv[1]);
    CHECK(fl_fs_mkdir_p(argv[1]) && fl_fs_join(wav, sizeof(wav), argv[1], "tone.wav") && fl_fs_join(broken, sizeof(broken), argv[1], "broken.wav"));
    CHECK(write_wav(wav));
    const char header[] = "RIFF\x24\x00\x00\x00WAVEgarbage-that-is-not-a-format-chunk";
    CHECK(fl_fs_write_atomic(broken, header, sizeof(header) - 1));
    test_decode(wav, broken);
    test_import(argv[1], wav, broken);
    test_player(wav);
    test_mixer(wav);
    return FLOW_TEST_RESULT("fl_audio_tests");
}
