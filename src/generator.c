/* Copyright (c) 2010-2017 the corto developers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "corto/g/g.h"

/* Close file */
static int g_closeFile(void* o, void* ctx) {
    g_file file;
    CORTO_UNUSED(ctx);

    file = o;

    g_fileClose(file);

    return 1;
}

static void g_reset(g_generator g) {
    if (g->library) {
        corto_dl_close(g->library);
        g->library = NULL;
    }

    if (g->files) {
        corto_ll_walk(g->files, g_closeFile, NULL);
        corto_ll_free(g->files);
        g->files = NULL;
    }

    /* Set id-generation to default */
    g->idKind = CORTO_GENERATOR_ID_DEFAULT;
    
    /* Set action-callbacks */
    g->start_action = NULL;
    g->id_action = NULL;

    /* No library loaded */
    g->library = NULL;

    /* Current will be set by object walk */
    g->current = NULL;
    g->inWalk = FALSE;

    if (g->objects) {
        corto_iter it = corto_ll_iter(g->objects);
        while (corto_iter_hasNext(&it)) {
            g_object *obj = corto_iter_next(&it);
            if (obj->parseSelf || obj->parseScope) {
                g->current = corto_ll_get(g->objects, 0);
                break;
            }
        }
    }
}

/* Generator functions */
g_generator g_new(char* name, char* language) {
    g_generator result;

    result = corto_calloc(sizeof(struct g_generator_s));

    /* Set name */
    if (name) {
        result->name = corto_strdup(name);
    }

    if (language) {
        result->language = corto_strdup(language);
    } else {
        result->language = corto_strdup("c"); /* Take 'c' as default language */
    }

    g_reset(result);

    return result;
}

/* Control how id's are generated */
g_idKind g_setIdKind(g_generator g, g_idKind kind) {
    g_idKind prev;
    prev = g->idKind;
    g->idKind = kind;
    return prev;
}

/* Get name, or if no name is provided, return name of current parse-object */
char* g_getName(g_generator g) {
    char* result;

    result = NULL;
    if (g->name) {
        result = g->name;
    } else if (g->current) {
        result = corto_idof(g->current->o);
    }

    return result;
}

/* Get name from generator name (strip path) */
char* g_getProjectName(g_generator g) {
    char *package = g_getName(g);
    char *ptr = &package[strlen(package) - 1];
    while ((ptr != package)) {
        ptr --;
        if (*ptr == '/') {
            ptr ++;
            break;
        }
        if (*ptr == ':') {
            ptr ++;
            break;
        }
    }
    return ptr;
}

/* Get current object */
corto_object g_getCurrent(g_generator g) {
    corto_object result;

    result = NULL;

    if (g->current) {
        result = g->current->o;
    }

    return result;
}

/* Add to-parse object */
void g_parse(g_generator g, corto_object object, corto_bool parseSelf, corto_bool parseScope, char* prefix) {
    g_object* o = NULL;
    corto_iter objectIter;

    /* First do a lookup, check if the object already exists */
    if (g->objects) {
        g_object *gObj;
        objectIter = corto_ll_iter(g->objects);
        while(corto_iter_hasNext(&objectIter)) {
            gObj = corto_iter_next(&objectIter);
            if (gObj->o == object) {
                o = gObj;
                break;
            }
        }
    }

    if (!o) {
        o = corto_alloc(sizeof(g_object));
        corto_claim(object);
        o->o = object;
        o->parseSelf = parseSelf;
        o->parseScope = parseScope;

        if (prefix) {
            if (strlen(prefix) >= sizeof(corto_id)) {
                corto_error("prefix cannot be longer than %d characters", sizeof(corto_id));
                o->prefix = NULL;
            } else {
                o->prefix = corto_strdup(prefix);
            }
        } else {
            o->prefix = NULL;
        }

        if (!g->objects) {
            g->objects = corto_ll_new();
        }
        corto_ll_append(g->objects, o);

        if ((parseSelf || parseScope) && !g->current) {
            g->current = o;
        }
    } else {
        /* Prefixes can be overridden if NULL. */
        if (!o->prefix) {
            o->prefix = corto_strdup(prefix);
        }
    }
}

static int g_genAttributeFind(void *value, void *userData) {
    g_attribute *attr = value;
    if(!strcmp(attr->key, *(char**)userData)) {
        *(void**)userData = attr;
        return 0;
    }
    return 1;
}

/* Set attribute */
void g_setAttribute(g_generator g, char* key, char* value) {
    g_attribute* attr = NULL;

    if (!g->attributes) {
        g->attributes = corto_ll_new();
    }else {
        void* userData = key;
        if(!corto_ll_walk(g->attributes, g_genAttributeFind, &userData)) {
            attr = userData;
        }
    }

    if(!attr) {
        attr = corto_alloc(sizeof(g_attribute));
        attr->key = corto_strdup(key);
        corto_ll_append(g->attributes, attr);
    }else {
        corto_dealloc(attr->value);
    }
    attr->value = corto_strdup(value);
}

/* Get attribute */
char* g_getAttribute(g_generator g, char* key) {
    char* result = NULL;

    if(g->attributes) {
        void *userData = key;
        if(!corto_ll_walk(g->attributes, g_genAttributeFind, &userData)) {
            result = ((g_attribute*)userData)->value;
        }
    }

    if (!result) {
        result = "";
    }

    return result;
}

/* Load generator actions from library */
corto_int16 g_load(g_generator g, char* library) {

    /* Reset generator to initial state in case this is not the first library
     * that is processed. */
    g_reset(g);

    /* Load library from generator path */
    char* package = corto_asprintf("driver/gen/%s", library);
    char* lib = corto_locate(package, &g->library, CORTO_LOCATION_LIB);
    if (!lib) {
        corto_throw("generator '%s' not found", package);
        goto error;
    }

    corto_assert(g->library != NULL, "generator located but dl_out is NULL");

    /* Load actions */
    g->start_action = (g_startAction)corto_dl_proc(g->library, "corto_genMain");
    if (!g->start_action) {
        corto_throw("g_load: %s: unresolved SYMBOL 'corto_genMain'", lib);
        corto_dealloc(lib);
        goto error;
    }
    g->id_action = (g_idAction)corto_dl_proc(g->library, "corto_genId");

    /* Function is allowed to be absent. */

    corto_dealloc(lib);
    corto_dealloc(package);

    return 0;
error:
    corto_dealloc(package);
    return -1;
}

static int g_freeObjects(void* _o, void* ctx) {
    g_object* o;

    CORTO_UNUSED(ctx);

    o = _o;
    if (o->prefix) {
        corto_dealloc(o->prefix);
    }
    corto_release(o->o);
    corto_dealloc(o);

    return 1;
}

/* Free snippet */
static int g_freeSnippet(void* o, void* ctx) {
    g_fileSnippet* snippet;
    g_file file;

    snippet = o;
    file = ctx;

    if (!snippet->used) {
        g_fileWrite(file, "%s(%s)", snippet->option, snippet->id);
        g_fileWrite(file, "%s", snippet->src);
        g_fileWrite(file, "$end\n");
        corto_warning("%s: code-snippet '%s' is not used, manually merge or remove from file.", file->name, snippet->id);
    }

    if (snippet->id) {
        corto_dealloc(snippet->id);
    }
    if (snippet->src) {
        corto_dealloc(snippet->src);
    }

    corto_dealloc(snippet);

    return 1;
}

static int g_freeAttribute(void* _o, void* ctx) {
    g_attribute* o;

    CORTO_UNUSED(ctx);

    o = _o;
    if (o->key) {
        corto_dealloc(o->key);
    }
    if (o->value) {
        corto_dealloc(o->value);
    }

    corto_dealloc(o);

    return 1;
}

static int g_freeImport(void* _o, void* ctx) {
    CORTO_UNUSED(ctx);

    corto_release(_o);

    return 1;
}

/* Free generator */
void g_free(g_generator g) {
    g_reset(g);

    if (g->objects) {
        corto_ll_walk(g->objects, g_freeObjects, NULL);
        corto_ll_free(g->objects);
        g->objects = NULL;
    }

    if (g->attributes) {
        corto_ll_walk(g->attributes, g_freeAttribute, NULL);
        corto_ll_free(g->attributes);
        g->attributes = NULL;
    }

    if (g->imports) {
        corto_ll_walk(g->imports, g_freeImport, NULL);
        corto_ll_free(g->imports);
        g->imports = NULL;
    }

    if (g->anonymousObjects) {
        corto_ll_free(g->anonymousObjects);
    }

    if (g->name) {
        corto_dealloc(g->name);
    }
    if (g->language) {
        corto_dealloc(g->language);
    }

    corto_dealloc(g);
}

corto_int16 g_loadPrefixes(g_generator g, corto_ll list) {
    corto_iter iter = corto_ll_iter(list);

    while (corto_iter_hasNext(&iter)) {
        corto_object p = corto_iter_next(&iter);
        char* prefix;
        char* includePath =
            corto_locate(
                corto_path(NULL, root_o, p, "/"), NULL, CORTO_LOCATION_INCLUDE);

        if (!includePath) {
            corto_throw("package '%s' not found", corto_path(NULL, root_o, p, "/"));
            goto error;
        }

        char *prefixFileStr = corto_asprintf("%s/.prefix", includePath);
        prefix = corto_file_load(prefixFileStr);
        if (prefix) {
            if (prefix[strlen(prefix) - 1] == '\n') {
                prefix[strlen(prefix) - 1] = '\0';
            }
            if (prefix) {
                g_parse(g, p, FALSE, FALSE, prefix);
            }
            corto_dealloc(prefix);
        } else {
            corto_catch();
        }
        corto_dealloc(prefixFileStr);
        corto_dealloc(includePath);
    }

    return 0;
error:
    return -1;
}

/* Start generator */
corto_int16 g_start(g_generator g) {

    /* packages.txt may contain more packages than is found by looking at the
     * metadata, however no code will be generated based on those packages so
     * they don't have to be configured for code generators */

    /* Find include paths for packages, load prefix file into generator */
    if (g->imports) {
        if (g_loadPrefixes(g, g->imports)) {
            corto_throw("failed to load package prefixes");
            goto error;
        }
    }
    if (g->importsNested) {
        if (g_loadPrefixes(g, g->importsNested)) {
            corto_throw("failed to load prefixes for nested packages");
            goto error;
        }
    }

    corto_int16 ret = g->start_action(g);
    if (ret)  {
        corto_throw("generator failed");
    }

    return ret;
error:
    return -1;
}

/* ==== Generator utility functions */
typedef struct g_serializeImports_t {
    g_generator g;
    corto_object stack[1024]; /* Maximum serializer-depth */
    corto_uint32 count;
    corto_bool nested;
}g_serializeImports_t;

corto_int16 g_leafDependencies(
    g_generator g,
    corto_object parent)
{
    char* packageDir = corto_locate(
        corto_fullpath(NULL, parent),
        NULL,
        CORTO_LOCATION_LIBPATH
    );

    char* packagesTxt = corto_asprintf("%s/.corto/packages.txt", packageDir);

    corto_ll deps = corto_loadGetDependencies(packagesTxt);
    if (deps) {
        if (!g->importsNested) {
            g->importsNested = corto_ll_new();
        }
        corto_iter it = corto_ll_iter(deps);
        while (corto_iter_hasNext(&it)) {
            char* dep = corto_iter_next(&it);
            corto_object o = corto_resolve(NULL, dep);
            if (o) {
                if (!corto_ll_hasObject(g->importsNested, o)) {
                    corto_ll_append(g->importsNested, o);
                    corto_claim(o);
                } else {
                    corto_release(o);
                }
            }
        }
        corto_loadFreePackages(deps);
    }

    return 0;
}

corto_int16 g_import(g_generator g, corto_object package) {
    if (!g->imports) {
        g->imports = corto_ll_new();
    }
    if (!corto_ll_hasObject(g->imports, package)) {
        corto_ll_insert(g->imports, package);
        corto_claim(package);

        /* Recursively obtain imports */
        g_leafDependencies(g, package);
    }

    return 0;
}

struct g_walkObjects_t {
    g_walkAction action;
    void* userData;
};

int g_scopeWalk(corto_object o, int (*action)(corto_object,void*), void *data) {
    corto_objectseq scope = corto_scopeClaim(o);
    corto_int32 i;
    for (i = 0; i < scope.length; i ++) {
        if (!action(scope.buffer[i], data)) {
            break;
        }
    }
    corto_scopeRelease(scope);
    if (i != scope.length) {
        return 0;
    }
    return 1;
}

/* Recursively walk scopes */
int g_walkObjects(void* o, void* userData) {
    struct g_walkObjects_t* data;

    data = userData;

    if (!data->action(o, data->userData)) {
        goto stop;
    }

    return g_scopeWalk(o, g_walkObjects, userData);
stop:
    return 0;
}

static int g_walkIterObject(g_generator g, g_object *o, g_walkAction action, void* userData, corto_bool scopeWalk, corto_bool recursiveWalk) {

    /* Parse object */
    if (o->parseSelf) {
        g->current = o;
        if (!action(o->o, userData)) {
            goto stop;
        }
    }
    /* Walk scopes */
    if (o->parseScope && scopeWalk) {
        g->current = o;
        if (!recursiveWalk) {
            if (!g_scopeWalk(o->o, action, userData)) {
                goto stop;
            }
        } else {
            struct g_walkObjects_t walkData;
            walkData.action = action;
            walkData.userData = userData;

            /* Recursively walk scopes */
            if (!g_scopeWalk(o->o, g_walkObjects, &walkData)) {
                goto stop;
            }
        }
    }

    return 1;
stop:
    return 0;
}

/* Walk objects, choose between recursive scopewalk or only top-level objects */
static int g_walk_ext(g_generator g, g_walkAction action, void* userData, corto_bool scopeWalk, corto_bool recursiveWalk) {
    if (g->inWalk) {
        /* If already in a walk, continue */
        g_object *o = g->current;
        if (!g_walkIterObject(g, o, action, userData, scopeWalk, recursiveWalk)) {
            g->current = o;
            goto stop;
        }
        g->current = o;
    } else if (g->objects) {
        g->inWalk = TRUE;
        corto_iter iter = corto_ll_iter(g->objects);
        while(corto_iter_hasNext(&iter)) {
            g_object* o = corto_iter_next(&iter);
            if (!g_walkIterObject(g, o, action, userData, scopeWalk, recursiveWalk)) {
                g->inWalk = FALSE;
                goto stop;
            }
        }
        g->inWalk = FALSE;
    }

    return 1;
stop:
    return 0;
}

/* Walk objects */
int g_walk(g_generator g, g_walkAction action, void* userData) {
    return g_walk_ext(g, action, userData, TRUE, FALSE);
}

/* Walk objects, never walk scopes, even if object is required to. */
int g_walkNoScope(g_generator g, g_walkAction action, void* userData) {
    return g_walk_ext(g, action, userData, FALSE, FALSE);
}

/* Walk objects recursively */
int g_walkRecursive(g_generator g, g_walkAction action, void* userData) {
    return g_walk_ext(g, action, userData, TRUE, TRUE);
}

/* Find prefix for a given object. Search parse-object in generator
 * which is closest to the object passed to this function. */
static g_object* g_findObjectIntern(
    g_generator g,
    corto_object o,
    corto_object* match,
    corto_bool inclusive)
{
    corto_iter iter;
    g_object *result = NULL, *t;
    corto_object parent;
    unsigned int distance, minDistance;

    if (g->objects) {
        minDistance = -1;
        iter = corto_ll_iter(g->objects);
        while(corto_iter_hasNext(&iter)) {
            t = corto_iter_next(&iter);

            /* Check if object occurs in scope of 'o' and measure distance to 'o' */
            parent = o;
            distance = 0;
            while(parent && (parent != t->o)) {
                distance++;
                parent = corto_parentof(parent);
            }

            /* If a parent was found (parent of root is NULL), assign it to result if
             * distance is smaller than minDistance */
            if (parent) {
                if ((distance < minDistance) && (distance || inclusive)) {
                    result = t;
                    minDistance = distance;
                    if (match) {
                        *match = parent;
                    }
                }
            }
        }
    }

    return result;
}

g_object* g_findObject(
    g_generator g,
    corto_object o,
    corto_object* match)
{
    return g_findObjectIntern(g, o, match, FALSE);
}

g_object* g_findObjectInclusive(
    g_generator g,
    corto_object o,
    corto_object* match)
{
    return g_findObjectIntern(g, o, match, TRUE);
}

/* Obtain prefix */
char* g_getPrefix(g_generator g, corto_object o) {
    g_object* prefix;

    /* Lookup prefix */
    prefix = g_findObject(g, o, NULL);

    return (prefix != NULL) ? prefix->prefix : NULL;
}

/* Instead of looking at function overload attribute, check if there are functions
 * in the same scope that have the same name. The difference is that a method
 * may overload a method from a baseclass from a different scope. In that case
 * however, there is no danger of a name-clash in generated code, so a short
 * name can still be used. */
static corto_bool g_isOverloaded(corto_function o) {
    corto_bool result = FALSE;
    corto_int32 i, d = 0;
    corto_objectseq scope = corto_scopeClaim(corto_parentof(o));
    for (i = 0; i < scope.length; i ++) {
        if (corto_instanceof(corto_procedure_o, corto_typeof(scope.buffer[i]))) {
            corto_assert(corto_overload(scope.buffer[i], corto_idof(o), &d) == 0, "overloading error discovered in generator");
            if (d > 0 || d == CORTO_OVERLOAD_NOMATCH_OVERLOAD) {
                result = TRUE;
                break;
            }
        }
    }
    corto_scopeRelease(scope);
    return result;
}

/* Object transformations */
static corto_char* g_oidTransform(g_generator g, corto_object o, corto_id _id, g_idKind kind) {
    CORTO_UNUSED(g);

    /* If the object is a function with an argumentlist, cut the argumentlist
     * from the name if the function is not overloaded. This keeps processing
     * for generators trivial. */
    if (corto_class_instanceof(corto_procedure_o, corto_typeof(o))) {
        if (!g_isOverloaded(o)) {
            corto_char* ptr;
            ptr = strchr(_id, '(');
            if (ptr) {
                *ptr = '\0';
            }
        } else {
            /* If function is overloaded, construct the 'request' string, that
             * is, the string without the argument-names. This results in a
             * string with only the types, which is enough to generate unique
             * names in languages which do not support overloading. */
            corto_id tmp, buff;
            corto_int32 count, i;
            strcpy(tmp, _id);

            corto_signatureName(tmp, _id);
            strcat(_id, "(");

            count = corto_signatureParamCount(tmp);
            if (count == -1) {
                corto_throw("invalid signature '%s'", tmp);
                goto error;
            }

            /* strcat is not the most efficient function here, but it is the easiest, and this
             * part of the code is not performance-critical. */
            for(i=0; i<count; i++) {
                corto_signatureParamType(tmp, i, buff, NULL);
                if (i) {
                    strcat(_id, ",");
                }
                strcat(_id, buff);
            }
            strcat(_id, ")");
        }
    }

    /* Check if class-identifiers must be altered */
    if (kind != CORTO_GENERATOR_ID_DEFAULT && kind != CORTO_GENERATOR_ID_LOCAL) {
        corto_object i = o;
        corto_char* ptr;

        ptr = _id + strlen(_id);
        while(i) {
            while((ptr > _id) && (*ptr != '/')) {
                ptr--;
            }
            if ((corto_class_instanceof(corto_interface_o, i) && corto_type(i)->reference) || (i == corto_type(corto_object_o))) {
                corto_char *start = ptr;
                if (kind == CORTO_GENERATOR_ID_CLASS_UPPER) {
                    *start = toupper(*start);
                } else {
                    *start = tolower(*start);
                }
            }

            if (ptr == _id) {
                break;
            }

            i = corto_parentof(i);
            if (i) {
                if (*ptr == '/') {
                    ptr -= 1;
                }
            }
        }
    }

    return _id;
error:
    return NULL;
}

/* Translate object-id */
char* g_fullOidExt(g_generator g, corto_object o, corto_id id, g_idKind kind) {
    g_object* prefix;
    corto_object match;
    corto_id _id;

    id[0] = '\0';

    /* Find prefix for object */
    match = NULL;

    /* TODO: prefix i.c.m. !CORTO_GENERATOR_ID_DEFAULT & nested classes i.c.m. !CORTO_GENERATOR_ID_DEFAULT */

    if (corto_checkAttr(o, CORTO_ATTR_NAMED) && corto_childof(root_o, o)) {
        /* For local identifiers, strip path from name */
        if ((kind == CORTO_GENERATOR_ID_LOCAL) && 
            !corto_instanceof(corto_package_o, o)) 
        {
            corto_object parent = o;
            do {
                parent = corto_parentof(parent);
            } while (!corto_instanceof(corto_package_o, parent));

            corto_id signatureName;
            corto_signatureName(corto_idof(o), signatureName);

            /* Only use shorter name if id of parent is not equal to id of object. 
             * Otherwise, this may result in name clashes. */
            if (parent && corto_idof(parent) && strcmp(signatureName, corto_idof(parent))) {
                corto_path(_id, parent, o, "/");
            } else {
                return g_fullOidExt(g, o, id, CORTO_GENERATOR_ID_DEFAULT);
            }

        /* If prefix is found, replace the scope up until the found object with the prefix */
        } else {
            prefix = g_findObject(g, o, &match);

            if (prefix && prefix->prefix) {

                if (strcmp(prefix->prefix, ".")) {
                    corto_uint32 count;
                    corto_object scopes[CORTO_MAX_SCOPE_DEPTH];

                    /* Obtain list of scopes from matched to object */
                    count = 0;
                    scopes[count] = o;
                    while(scopes[count] != match) {
                        scopes[count+1] = corto_parentof(scopes[count]);
                        count++;
                    }

                    /* Paste in prefix */
                    strcpy(_id, prefix->prefix);
                    while(count) {
                        count--;
                        strcat(_id, "/");
                        strcat(_id, corto_idof(scopes[count]));
                    }
                } else {
                    corto_path(_id, g_getCurrent(g), o, "/");
                }
            /* If no prefix is found for object, just use the scoped identifier */
            } else {
                corto_fullpath(_id, o);
            }
        }

        g_oidTransform(g, o, _id, kind);
    } else {
        corto_uint32 count = 0;

        if (!g->anonymousObjects) {
            g->anonymousObjects = corto_ll_new();
        }
        corto_iter it = corto_ll_iter(g->anonymousObjects);
        while (corto_iter_hasNext(&it)) {
            corto_object e = corto_iter_next(&it);
            if (e == o) {
                break;
            } else if (corto_compare(e, o) == CORTO_EQ) {
                break;
            }
            count ++;
        }
        if (count == corto_ll_size(g->anonymousObjects)) {
            corto_ll_append(g->anonymousObjects, o);
        }

        corto_object cur = g_getCurrent(g);
        if (corto_instanceof(corto_package_o, cur)) {
            corto_id packageId;
            g_fullOid(g, cur, packageId);
            sprintf(_id, "anonymous_%s_%u", packageId, count);
        } else {
            sprintf(_id, "anonymous_%u", count);
        }
    } 

    if (g->id_action) {
        g->id_action(_id, id);
    } else {
        strcpy(id, _id);
    }

    return id;
}

/* Translate an object to a language-specific identifier with idKind provided. */
char* g_fullOid(g_generator g, corto_object o, corto_id id) {
    return g_fullOidExt(g, o, id, g->idKind);
}

char* g_localOid(g_generator g, corto_object o, corto_id id) {
    return g_fullOidExt(g, o, id, CORTO_GENERATOR_ID_LOCAL);
}

/* Translate an id to language representation */
char* g_id(g_generator g, char* str, corto_id id) {
    char* result;

    if (g->id_action) {
        result = g->id_action(str, id);
    } else {
        result = str;
    }

    return result;
}

/* Translate a class id to language representation */
char* g_oid(g_generator g, corto_object o, corto_id id) {
    char* result;
    corto_id cid;
    g_object* prefix;
    corto_object match;

    /* Find prefix for object */
    match = NULL;
    prefix = g_findObject(g, o, &match);

   /* If prefix is found, replace the scope up until the found object with the prefix */
    if (prefix && prefix->prefix) {
        if (prefix->o == o) {
            strcpy(cid, prefix->prefix);
        } else {
            strcpy(cid, corto_idof(o));
        }

    /* If no prefix is found for object, just use the identifier */
    } else {
        strcpy(cid, corto_idof(o));
    }

    g_oidTransform(g, o, cid, g->idKind);

    if (g->id_action) {
        result = g->id_action(cid, id);
    } else {
        result = id;
    }

    return result;
}


/* ==== Generator file-utility class */

/* Convert a filename to a filepath, depending on it's extension. */
static char* g_filePath_intern(g_generator g, char* filename, corto_char* buffer) {
    char* result;
    corto_id path;

    result = filename;

    if (g->attributes) {
        char* ext = NULL;
        char* fext, *ptr;

        /* Get file-extension */
        fext = NULL;
        ptr = filename;
        while((ptr = strchr(ptr, '.'))) {
            ptr++;
            fext = ptr;
        }

        /* Check whether there is an attribute with the file extension - determines where to put the file */
        if(fext) {
            ext = g_getAttribute(g, fext);
        }

        /* Append filename to location. */
        if (ext && *ext) {
            sprintf(buffer, "%s/%s", ext, filename);
            result = buffer;
        }
    }

    /* Ensure path exists */
    if (corto_file_path(result, path)) {
        if (corto_mkdir(path)) {
            goto error;
        }
    }

    return result;
error:
    return NULL;
}

/* Find existing parts in the code that must not be overwritten. */
corto_int16 g_loadExisting(g_generator g, char* name, char* option, corto_ll *list) {
    char* code = NULL, *ptr = NULL;
    CORTO_UNUSED(g);

    if (!corto_file_test(name)) {

        /* Check if there is a .old file that can be restored */
        corto_id oldName;
        sprintf(oldName, "%s.old", name);
        if (corto_file_test(oldName)) {
            if (corto_rename(oldName, name)) {
                corto_warning("could not rename '%s' to '%s'", oldName, name);
                goto ok;
            }
        } else {
            goto ok;
        }
    }

    code = corto_file_load(name);
    if (code) {
        ptr = code;

        while((ptr = strstr(ptr, option))) {
            ptr += strlen(option);

            /* Find begin of identifier */
            if (*ptr == '(') {
                char* endptr;

                /* Find end of identifier */
                endptr = strstr(ptr, ") */");
                if (endptr) {
                    corto_id identifier;

                    /* Copy identifier string */
                    *endptr = '\0';

                    if (strlen(ptr) >= sizeof(corto_id)) {
                        corto_throw(
                            "%s: identifier of code-snippet exceeds %d characters", sizeof(corto_id));
                        goto error;
                    }

                    strcpy(identifier, ptr + 1);
                    ptr = endptr + 1;

                    /* Find $end */
                    endptr = strstr(ptr, "$end");
                    if (endptr) {
                        g_fileSnippet* existing;
                        char* src;

                        *endptr = '\0';
                        src = corto_strdup(ptr);

                        if (!*list) {
                            *list = corto_ll_new();
                        }

                        if(strstr(src, "$begin")) {
                            corto_throw("%s: code-snippet '%s(%s)' contains nested $begin (did you forget an $end?)",
                                name, option, identifier);
                            corto_dealloc(src);
                            goto error;
                        }

                        existing = corto_alloc(sizeof(g_fileSnippet));
                        existing->option = corto_strdup(option);
                        existing->id = corto_strdup(identifier);
                        existing->src = src;
                        existing->used = FALSE;
                        corto_ll_insert(*list, existing);

                        ptr = endptr + 1;

                    } else {
                        corto_warning("generator: missing $end after $begin(%s)", identifier);
                    }
                } else {
                    corto_warning("generator: missing ')' after %s(", option);
                }
            } else {
                corto_warning("generator: missing '(' after %s.", option);
            }
        }
        corto_dealloc(code);
    } else {
        /* Catch error */
        corto_catch();
    }

ok:
    return 0;
error:
    if (code) {
        corto_dealloc(code);
    }
    return -1;
}

void g_fileClose(g_file file) {
    /* Remove file from generator administration */
    corto_ll_remove(file->generator->files, file);

    /* Free snippets */
    if (file->snippets) {
        corto_ll_walk(file->snippets, g_freeSnippet, file);
        corto_ll_free(file->snippets);
    }
    if (file->headers) {
        corto_ll_walk(file->headers, g_freeSnippet, file);
        corto_ll_free(file->headers);
    }

    fclose(file->file);
    corto_dealloc(file->name);
    corto_dealloc(file);
}

static g_file g_fileOpenIntern(g_generator g, char* name) {
    g_file result;
    char ext[255];

    result = corto_alloc(sizeof(struct g_file_s));
    result->snippets = NULL;
    result->headers = NULL;
    result->scope = NULL;
    result->file = NULL;
    result->indent = 0;
    result->name = corto_strdup(name);
    result->generator = g;

    corto_file_extension(name, ext);

    /* First, load existing implementation if file exists */
    if (!strcmp(ext, "c") || !strcmp(ext, "cpp") || !strcmp(ext, "h") || !strcmp(ext, "hpp")) {
        if (g_loadExisting(g, name, "$header", &result->headers)) {
            corto_dealloc(result);
            goto error;
        }
        if (g_loadExisting(g, name, "$begin", &result->snippets)) {
            corto_dealloc(result);
            goto error;
        }
        if (g_loadExisting(g, name, "$body", &result->snippets)) {
            corto_dealloc(result);
            goto error;
        }
    }

    result->file = fopen(name, "w");
    if (!result->file) {
        corto_throw("'%s': %s", name, strerror(errno));
        corto_dealloc(result);
        goto error;
    }

    if (!g->files) {
        g->files = corto_ll_new();
    }
    corto_ll_insert(g->files, result);

    return result;
error:
    corto_throw("failed to open file '%s'", name);
    return NULL;
}

/* Get path for file */
char* g_filePath(g_generator g, corto_id buffer, char *name, ...) {
    corto_char namebuffer[512];
    va_list args;
    va_start(args, name);
    vsprintf(namebuffer, name, args);
    va_end(args);
    return g_filePath_intern(g, namebuffer, buffer);
}

/* Get path for hidden file */
char* g_hiddenFilePath(g_generator g, corto_id buffer, char *name, ...) {
    CORTO_UNUSED(g);
    corto_char namebuffer[512];
    va_list args;
    va_start(args, name);
    vsprintf(namebuffer, name, args);
    va_end(args);

    char *hidden;
    if (!(hidden = g_getAttribute(g, "hidden"))) {
        hidden = ".corto";
    }

    sprintf(buffer, "%s/%s", hidden, namebuffer);
    return buffer;    
}

/* Open file */
g_file g_fileOpen(g_generator g, char* name, ...) {
    corto_char filepath[512];
    corto_char namebuffer[512];
    va_list args;
    va_start(args, name);
    vsprintf(namebuffer, name, args);
    va_end(args);
    return g_fileOpenIntern(g, g_filePath_intern(g, namebuffer, filepath));
}

/* Open hidden file for writing. */
g_file g_hiddenFileOpen(g_generator g, char* name, ...) {
    corto_char filepath[512];
    corto_char namebuffer[512];
    va_list args;
    va_start(args, name);
    vsprintf(namebuffer, name, args);
    va_end(args);

    char *hidden;
    if (!(hidden = g_getAttribute(g, "hidden"))) {
        hidden = ".corto";
    }

    if (corto_file_test(hidden) != 1) {
        if (corto_mkdir(hidden)) {
            goto error;
        }
    }

    sprintf(filepath, "%s/%s", hidden, namebuffer);
    return g_fileOpenIntern(g, filepath);
error:
    return NULL;
}

/* Lookup an existing code-snippet */
char* g_fileLookupSnippetIntern(g_file file, char* snippetId, corto_ll list) {
    corto_iter iter;
    g_fileSnippet* snippet;
    CORTO_UNUSED(file);

    snippet = NULL;

    if (list) {
        iter = corto_ll_iter(list);
        while(corto_iter_hasNext(&iter)) {
            snippet = corto_iter_next(&iter);
            corto_id path; strcpy(path, snippet->id);
            char *snippetPtr = path;

            /* Ignore initial scope character */
            if (*snippetPtr == '/') {
                snippetPtr = path + 1;
            }

            if (*snippetId == '/') {
                snippetId ++;
            }

            if (!stricmp(snippetPtr, snippetId) || !strcmp(path, snippetId)) {
                snippet->used = TRUE;
                break;
            } else {
                snippet = NULL;
            }
        }
    }

    return snippet ? snippet->src : NULL;
}

char* g_fileLookupSnippet(g_file file, char* snippetId) {
    return g_fileLookupSnippetIntern(file, snippetId, file->snippets);
}

char* g_fileLookupHeader(g_file file, char* snippetId) {
    return g_fileLookupSnippetIntern(file, snippetId, file->headers);
}

/* Test if object must be parsed */
int g_checkParseWalk(void* o, void* userData) {
    g_object* _o;
    int result;

    _o = o;
    result = 1;

    /* If parseSelf, and object equals generatorObject, object must be parsed. */
    if (_o->parseSelf && (_o->o == userData)) {
        result = 0;

    /* Look for generator object in object-scope */
    } else if (_o->parseScope) {
        corto_object ptr = userData;

        /* Walk object-scope */
        while((ptr = corto_parentof(ptr)) && (ptr != _o->o));
        if (ptr) {
            result = 0;
        }
    }

    return result;
}

corto_bool g_mustParse(g_generator g, corto_object o) {
    corto_bool result;

    result = TRUE;
    if (corto_checkAttr(o, CORTO_ATTR_NAMED) && corto_childof(root_o, o)) {
        result = !g_checkParseWalk(g->current, o);
    }

    return result;
}

/* Increase indentation */
void g_fileIndent(g_file file) {
    file->indent++;
}

/* Decrease indentation */
void g_fileDedent(g_file file) {
    file->indent--;
}

/* Set scope of file */
void g_fileScopeSet(g_file file, corto_object o) {
    file->scope = o;
}

/* Get scope of file */
corto_object g_fileScopeGet(g_file file) {
    return file->scope;
}

/* Write to file */
int g_fileWrite(g_file file, char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    char *buffer = corto_vasprintf(fmt, args);
    va_end(args);

    /* Write indentation & string */
    if (file->indent && file->endLine) {
        if (fprintf(file->file, "%*s%s", file->indent * 4, " ", buffer) < 0) {
            corto_throw("g_fileWrite: writing to outputfile failed.");
            goto error;
        }
    } else {
        if (fprintf(file->file, "%s", buffer) < 0) {
            corto_throw("g_fileWrite: writing to outputfile failed.");
            goto error;
        }
    }

    file->endLine = buffer[strlen(buffer)-1] == '\n';

    corto_dealloc(buffer);

    return 0;
error:
    return -1;
}

/* Get generator */
g_generator g_fileGetGenerator(g_file file) {
    return file->generator;
}


/* Translate names of members so that they can be used in the same scope (for example when used as function parameter) */
typedef struct corto_genWalkMember_t {
    corto_member member;
    corto_uint32 occurred;
}corto_genWalkMember_t;

static corto_uint32 corto_genMemberCacheCount(corto_ll cache, corto_member m) {
    corto_iter memberIter;
    corto_genWalkMember_t *member;
    corto_uint32 result = 0;

    memberIter = corto_ll_iter(cache);
    while(corto_iter_hasNext(&memberIter)) {
        member = corto_iter_next(&memberIter);
        if (!strcmp(corto_idof(member->member), corto_idof(m))) {
            result++;
        }
    }

    return result;
}

static corto_uint32 corto_genMemberCacheGet(corto_ll cache, corto_member m) {
    corto_iter memberIter;
    corto_genWalkMember_t *member;
    corto_uint32 result = 0;

    memberIter = corto_ll_iter(cache);
    while(corto_iter_hasNext(&memberIter)) {
        member = corto_iter_next(&memberIter);
        if (member->member == m) {
            result = member->occurred;
            break;
        }
    }

    return result;
}

static corto_int16 corto_genMemberCache_member(corto_walk_opt* s, corto_value *info, void* userData) {
    corto_ll cache;
    CORTO_UNUSED(s);

    cache = userData;

    if (info->kind == CORTO_MEMBER) {
        corto_genWalkMember_t *parameter;
        corto_member m = info->is.member.t;

        parameter = corto_alloc(sizeof(corto_genWalkMember_t));
        parameter->member = m;
        parameter->occurred = corto_genMemberCacheCount(cache, m);
        corto_ll_append(cache, parameter);
    } else {
        corto_walk_members(s, info, userData);
    }

    return 0;
}

corto_char* corto_genMemberName(g_generator g, corto_ll cache, corto_member m, corto_char *result) {
    corto_uint32 count;
    corto_id temp;

    if ((count = corto_genMemberCacheGet(cache, m))) {
        sprintf(temp, "%s_%d", corto_idof(m), count);
    } else {
        strcpy(temp, corto_idof(m));
    }

    g_id(g, temp, result);

    return result;
}

/* Build cache to determine whether membernames occur more than once (due to inheritance) */
corto_ll corto_genMemberCacheBuild(corto_interface o) {
    corto_walk_opt s;
    corto_ll result;

    corto_walk_init(&s);
    s.access = CORTO_LOCAL | CORTO_PRIVATE;
    s.accessKind = CORTO_NOT;
    s.metaprogram[CORTO_MEMBER] = corto_genMemberCache_member;
    result = corto_ll_new();

    corto_metawalk(&s, corto_type(o), result);

    return result;
}

void corto_genMemberCacheClean(corto_ll cache) {
    corto_iter memberIter;
    corto_genWalkMember_t *member;

    memberIter = corto_ll_iter(cache);
    while(corto_iter_hasNext(&memberIter)) {
        member = corto_iter_next(&memberIter);
        corto_dealloc(member);
    }
    corto_ll_free(cache);
}

typedef struct g_depWalk_t {
    g_generator g;
    corto_ll dependencies;
} g_depWalk_t;

static corto_package g_addDepencency(g_generator g, corto_object o, g_depWalk_t *data) {
    corto_package result = NULL;

    if (!g_mustParse(g, o)) {
        corto_object parent = o;
        while (parent && !corto_instanceof(corto_package_o, parent)) {
            parent = corto_parentof(parent);
        }

        if (parent && !corto_childof(g_getCurrent(g), parent)) {
            result = parent;
        }
    }

    if (result) {
        if (!data->dependencies) {
            data->dependencies = corto_ll_new();
        }
        if (!corto_ll_hasObject(data->dependencies, result)) {
            corto_ll_append(data->dependencies, result);
        }
    }

    return result;
}

/* Serialize dependencies on references */
static corto_int16 g_evalRef(corto_walk_opt* s, corto_value* info, void* userData) {
    g_depWalk_t *data = userData;

    CORTO_UNUSED(s);

    corto_object dep = *(corto_object*)corto_value_ptrof(info);
    if (dep) {
        g_addDepencency(data->g, dep, data);
    }

    return 0;
}

/* Serialize object type */
static corto_int16 g_evalObject(corto_walk_opt* s, corto_value* info, void* userData) {
    g_depWalk_t *data = userData;
    corto_object o = corto_value_objectof(info);

    CORTO_UNUSED(s);

    g_addDepencency(data->g, corto_typeof(o), data);

    return corto_walk_value(s, info, userData);
}

/* Dependency serializer */
corto_walk_opt g_depSerializer(void) {
    corto_walk_opt s;

    corto_walk_init(&s);
    s.reference = g_evalRef;
    s.metaprogram[CORTO_OBJECT] = g_evalObject;
    s.access = CORTO_LOCAL;
    s.accessKind = CORTO_NOT;

    return s;
}

static int g_collectDependency(corto_object o, void *userData) {
    corto_walk_opt s = g_depSerializer();
    corto_walk(&s, o, userData);
    return 1;
}

corto_ll g_getDependencies(g_generator g) {
    g_depWalk_t walkData = {.g = g};

    /* Walk objects in dependency order */
    if (corto_genDepWalk(g, NULL, g_collectDependency, &walkData)) {
        goto error;
    }

    return walkData.dependencies;
error:
    return NULL;
}
