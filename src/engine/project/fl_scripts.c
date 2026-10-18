#include "fl_scripts_internal.h"
#include "components/fl_script.h"
#include <stdlib.h>
#include <string.h>

#define MAX_SCRIPTS 4096u

const Fl_ScriptSource *fl_scene_script(const Fl_Scene *s, Fl_Id id)
{
    for (size_t i = 0; s && id && i < s->script_count; ++i) if (s->scripts[i].id == id) return &s->scripts[i];
    return NULL;
}

const Fl_ScriptSource *fl_scene_script_named(const Fl_Scene *s, const char *name)
{
    for (size_t i = 0; s && name && i < s->script_count; ++i) if (!strcmp(s->scripts[i].name, name)) return &s->scripts[i];
    return NULL;
}

size_t fl_scene_script_uses(const Fl_Scene *s, Fl_Id script)
{
    size_t uses = 0;
    Fl_Script attached;
    for (size_t i = 0; s && i < s->entity_count; ++i)
        uses += fl_script_decode(fl_entity_component(&s->entities[i], FL_COMPONENT_SCRIPT), &attached) && attached.script == script;
    return uses;
}

/* Release one script's owned strings. */
static void free_source(Fl_ScriptSource *script)
{
    free((void *)script->name); free((void *)script->source);
}

void fl_scripts_free(Fl_Scene *s)
{
    for (size_t i = 0; i < s->script_count; ++i) free_source((Fl_ScriptSource *)&s->scripts[i]);
    free((void *)s->scripts);
    s->scripts = NULL; s->script_count = 0;
}

/* Duplicate size bytes of text into a NUL-terminated allocation. */
static char *copy_text(const char *text, size_t size)
{
    char *copy = malloc(size + 1);
    if (copy) {memcpy(copy, text, size); copy[size] = 0;}
    return copy;
}

/* Whether another script of s already uses name. */
static bool name_taken(const Fl_Scene *s, const char *name, Fl_Id except)
{
    const Fl_ScriptSource *found = fl_scene_script_named(s, name);
    return found && found->id != except;
}

/* Remove every Script component that runs script from the scene's entities. */
static void detach_all(Fl_Scene *s, Fl_Id script)
{
    for (size_t i = 0; i < s->entity_count; ++i) {
        Fl_Entity *e = (Fl_Entity *)&s->entities[i];
        Fl_Script attached;
        for (size_t j = 0; j < e->component_count; ++j) {
            if (!fl_script_decode(&e->components[j], &attached) || attached.script != script) continue;
            Fl_Component *all = (Fl_Component *)e->components;
            fl_component_clear(&all[j]);
            memmove(all + j, all + j + 1, (e->component_count - j - 1) * sizeof(*all));
            --e->component_count;
            break;
        }
    }
}

bool fl_scripts_apply(Fl_Scene *s, const Fl_Op *op, uint64_t *next_id, Fl_Id *created)
{
    const Fl_ScriptSource *existing = op->script ? fl_scene_script(s, op->script) : NULL;
    if (op->kind == FL_OP_SCRIPT_REMOVE) {
        if (!existing) return false;
        Fl_ScriptSource *all = (Fl_ScriptSource *)s->scripts;
        size_t index = (size_t)(existing - all);
        detach_all(s, existing->id);
        free_source(&all[index]);
        memmove(all + index, all + index + 1, (s->script_count - index - 1) * sizeof(*all));
        --s->script_count;
        return true;
    }
    if (op->script && !existing) return false;
    const bool changes_name = op->name != NULL, changes_source = op->source != NULL;
    if ((changes_name && (!fl_script_name_valid(op->name) || name_taken(s, op->name, op->script))) ||
        (changes_source && !fl_script_source_valid(op->source, op->source_size))) return false;
    if (!existing) {
        if (!changes_name || !changes_source || *next_id >= UINT64_MAX - 1 || s->script_count >= MAX_SCRIPTS) return false;
        char *name = copy_text(op->name, strlen(op->name)), *source = copy_text(op->source, op->source_size);
        Fl_ScriptSource *scripts = realloc((void *)s->scripts, (s->script_count + 1) * sizeof(*scripts));
        if (!name || !source || !scripts) {free(name); free(source); if (scripts) s->scripts = scripts; return false;}
        scripts[s->script_count++] = (Fl_ScriptSource){*created = (*next_id)++, name, source, op->source_size};
        s->scripts = scripts;
        return true;
    }
    if (!changes_name && !changes_source) return false;
    char *name = changes_name ? copy_text(op->name, strlen(op->name)) : NULL;
    char *source = changes_source ? copy_text(op->source, op->source_size) : NULL;
    if ((changes_name && !name) || (changes_source && !source)) {free(name); free(source); return false;}
    Fl_ScriptSource *target = (Fl_ScriptSource *)existing;
    if (name) {free((void *)target->name); target->name = name;}
    if (source) {free((void *)target->source); target->source = source; target->source_size = op->source_size;}
    return true;
}

bool fl_scripts_references_valid(const Fl_Scene *s, const Fl_Component *components, size_t count)
{
    Fl_Script attached;
    for (size_t i = 0; components && i < count; ++i)
        if (components[i].type == FL_COMPONENT_SCRIPT && (!fl_script_decode(&components[i], &attached) || !fl_scene_script(s, attached.script))) return false;
    return true;
}

bool fl_scripts_valid(const Fl_Scene *s, uint64_t next_id)
{
    for (size_t i = 0; i < s->script_count; ++i) {
        const Fl_ScriptSource *script = &s->scripts[i];
        if (!script->id || script->id >= next_id || !fl_script_name_valid(script->name) || !fl_script_source_valid(script->source, script->source_size)) return false;
        for (size_t j = 0; j < i; ++j) if (!strcmp(s->scripts[j].name, script->name)) return false;
    }
    for (size_t i = 0; i < s->entity_count; ++i)
        if (!fl_scripts_references_valid(s, s->entities[i].components, s->entities[i].component_count)) return false;
    return true;
}

void fl_scripts_ids(const Fl_Scene *s, Fl_Id *ids)
{
    for (size_t i = 0; i < s->script_count; ++i) ids[i] = s->scripts[i].id;
}

void fl_scripts_save(const Fl_Scene *s, Fl_Binary *b)
{
    fl_binary_u64(b, s->script_count);
    for (size_t i = 0; i < s->script_count; ++i) {
        const Fl_ScriptSource *script = &s->scripts[i];
        fl_binary_u64(b, script->id); fl_binary_string(b, script->name);
        fl_binary_u64(b, script->source_size); fl_binary_write(b, script->source, script->source_size);
    }
}

bool fl_scripts_load(Fl_Scene *s, Fl_Binary *b)
{
    uint64_t count = fl_binary_get_u64(b);
    if (b->failed || count > MAX_SCRIPTS) return false;
    Fl_ScriptSource *scripts = count ? calloc((size_t)count, sizeof(*scripts)) : NULL;
    if (count && !scripts) return false;
    s->scripts = scripts;
    for (uint64_t i = 0; i < count; ++i) {
        Fl_ScriptSource *script = &scripts[i];
        ++s->script_count;
        script->id = fl_binary_get_u64(b); script->name = fl_binary_get_string(b);
        uint64_t size = fl_binary_get_u64(b);
        if (b->failed || size == 0 || size > FL_SCRIPT_SOURCE_LIMIT || size > b->size - b->cursor) return false;
        char *source = malloc((size_t)size + 1);
        if (!source) return false;
        fl_binary_read(b, source, (size_t)size);
        source[size] = 0;
        script->source = source; script->source_size = (size_t)size;
    }
    return !b->failed;
}
