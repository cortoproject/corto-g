/* Copyright (c) 2010-2018 the corto developers
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

#include <corto/g/g.h>

#define CYCLE_DEPTH (1024)

typedef struct g_item* g_item;
struct g_item {
    void* o;
    corto_bool declared;
    corto_bool defined;
    corto_int32 declareCount;
    corto_int32 defineCount;
    corto_ll onDeclared;
    corto_ll onDefined;
};

typedef struct g_dependency* g_dependency;
struct g_dependency {
    corto_uint8 kind;
    g_item item;
    g_item dependency;
    corto_bool weak; /* A weak dependency may be degraded to DECLARED if a cycle can otherwise not be broken. */
    corto_uint32 marked;
    corto_bool processed; /* When a cycles are broken, it can occur that a dependency is resolved twice, as
                          a cycle is broken by resolving a dependency. This could cause refcounts to dive below
                          zero which is undesired. */
};

struct corto_depresolver_s {
    corto_ll items;
    corto_ll toPrint;
    corto_depresolver_action onDeclare;
    corto_depresolver_action onDefine;
    void* userData;
    g_dependency stack[CYCLE_DEPTH]; /* For cycle detection */
    corto_uint32 sp;
    corto_int32 iteration; /* dependency.marked equals this number when it has been marked in the current
                            * cycle detection iteration. */
    corto_bool bootstrap; /* If a bootstrap is detected, disregard all dependencies. This can only mean that
                          the builtin-types are being generated, since these are the only ones that can
                          introduce a bootstrap (typeof(class) == class).
                          In this case, dependencies don't matter (and are non-resolvable)*/
};

static int g_itemPrint(void* o, void* userData);

/* Create new item */
static
g_item g_itemNew(
    corto_object o,
    corto_depresolver data)
{
    g_item result;

    result = corto_alloc(sizeof(struct g_item));
    result->o = o;
    result->declared = FALSE;
    result->defined = FALSE;
    result->declareCount = 0;
    result->defineCount = 0;
    result->onDeclared = NULL;
    result->onDefined = NULL;

    if (o == root_o) {
        result->declared = TRUE;
        result->defined = TRUE;
    }

    corto_ll_insert(data->items, result);

    return result;
}

/* Delete item */
static
void g_itemFree(
    g_item item)
{
    g_dependency dep;

    /* Free onDeclared list */
    if (item->onDeclared) {
        while((dep = corto_ll_takeFirst(item->onDeclared))) {
            corto_dealloc(dep);
        }
        corto_ll_free(item->onDeclared);
    }

    /* Free onDefined list */
    if (item->onDefined) {
        while((dep = corto_ll_takeFirst(item->onDefined))) {
            corto_dealloc(dep);
        }
        corto_ll_free(item->onDefined);
    }

    corto_dealloc(item);
}

/* Lookup item in administration */
static
g_item g_itemLookup(
    corto_object o,
    corto_depresolver data)
{
    corto_iter iter;
    g_item item;

    /* Lookup item for 'o' in items list */
    item = NULL;
    iter = corto_ll_iter(data->items);
    while(!item && corto_iter_hasNext(&iter)) {
        item = corto_iter_next(&iter);
        if (item->o != o) {
            item = NULL;
        }
    }

    /* If item did not yet exist, insert it in data */
    if (!item) {
        item = g_itemNew(o, data);
    }

    return item;
}

/* Resolve dependency, decrease refcount */
static
int g_itemResolveDependency(
    void* o,
    void* userData)
{
    g_dependency dep;
    corto_depresolver data;

    dep = o;
    data = userData;

    if (!dep->processed) {
        corto_debug("depresolver: resolve dependency: %s '%s' before ? '%s'",
                corto_ptr_str(&dep->kind, corto_state_o, 0),
                corto_fullpath(NULL, dep->item->o),
                corto_fullpath(NULL, dep->dependency->o));

        switch(dep->kind) {
        case CORTO_DECLARED:
            dep->item->declareCount--;

            corto_assert(dep->item->declareCount >= 0, "negative declareCount for item '%s'.", corto_idof(dep->item->o));

            if (!dep->item->declareCount) {
                corto_ll_insert(data->toPrint, dep->item);
            }
            break;
        case CORTO_VALID:
            dep->item->defineCount--;

            corto_assert(dep->item->defineCount >= 0, "negative defineCount for item '%s'.", corto_idof(dep->item->o));

            if (!dep->item->defineCount) {
                corto_ll_insert(data->toPrint, dep->item);
            }
            break;
        }
    }

    dep->processed = TRUE;

    return 1;
}

/* Declare item */
static
void g_itemDeclare(
    g_item item,
    corto_depresolver data)
{
    if (data->onDeclare) {
        data->onDeclare(item->o, data->userData);
    }
}

/* Define item */
static
void g_itemDefine(
    g_item item,
    corto_depresolver data)
{
    if (data->onDefine) {
        data->onDefine(item->o, data->userData);
    }
}

/* Declare an item */
static
int g_itemPrint(
    void* o,
    void* userData)
{
    g_item item;
    corto_depresolver data;

    item = o;
    data = userData;

    /* Walk DECLARED dependencies */
    if (!item->declared && !item->declareCount) {
        item->declared = TRUE;
        corto_debug("depresolver: declare '%s'", corto_fullpath(NULL, item->o));
        g_itemDeclare(item, data);
        if (item->onDeclared) {
            corto_ll_walk(item->onDeclared, g_itemResolveDependency, data);
        }
    }

    /* Walk DECLARED | DEFINED dependencies */
    if (item->declared && !item->defined && !item->defineCount) {
        item->defined = TRUE;
        corto_debug("depresolver: define '%s'", corto_fullpath(NULL, item->o));
        g_itemDefine(item, data);
        if (item->onDefined) {
            corto_ll_walk(item->onDefined, g_itemResolveDependency, data);
        }
    }

    return 1;
}

/* Collect initial objects */
static
int g_itemCollectinitial(
    void* o,
    void* userData)
{
    g_item item;
    corto_depresolver data;

    item = o;
    data = userData;

    if (!item->declareCount) {
        corto_ll_insert(data->toPrint, item);
    }

    return 1;
}

/* Print items (forward them to declare & define callbacks) */
static
int g_itemPrintItems(struct corto_depresolver_s* data) {
    g_item item;

    /* Collect initial items */
    if (!corto_ll_walk(data->items, g_itemCollectinitial, data)) {
        goto error;
    }

    /* Print items */
    while((item = corto_ll_takeFirst(data->toPrint))) {
        if (!g_itemPrint(item, data)) {
            goto error;
        }
    }

    return 0;
error:
    return -1;
}

static
int g_itemResolveCycles(
    g_item item,
    struct corto_depresolver_s* data);

/* Scan stack for occurrence of dependency. */
static
corto_uint32 g_dependencyOnStack(
    g_dependency dep,
    struct corto_depresolver_s* data)
{
    corto_uint32 i;
    corto_bool found;

    i = 0;
    found = FALSE;
    for(i=0; i<data->sp; i++) {
        if (data->stack[i] == dep) {
            found = TRUE;
            break;
        }
    }

    return found ? i + 1 : 0; /* zero indicates that the dependency is not found. */
}

/* Resolve cycles for dependency */
static
void g_itemResolveDependencyCycles(
    g_dependency dep,
    struct corto_depresolver_s* data)
{
    corto_uint32 sp;

    /* If item is already marked, there is no need to investigate it further. */
    if (dep->marked != data->iteration) {
        /* If dependency is not on stack, scan further, if on stack, a cycle is found. */
        if (!(sp = g_dependencyOnStack(dep, data))) {
            data->stack[data->sp] = dep;
            data->sp++;
            corto_assert(data->sp < CYCLE_DEPTH, "stack-bound overflow.");

            /* Forward item and mark it. */
            g_itemResolveCycles(dep->item, data);
            dep->marked = data->iteration;
            data->sp--;

        /* If a cycle is found, look on the stack for a weak dependency. */
        } else {
            corto_uint32 i;

            corto_debug("depresolver: >> begin breaking cycle [%p]", dep);
            for(i = sp - 1; i < data->sp; i++) {
                corto_debug("depresolver: on stack: can't %s '%s' before DECLARED|DEFINED '%s'",
                    corto_ptr_str(&data->stack[i]->kind, corto_state_o, 0),
                    corto_fullpath(NULL, data->stack[i]->item->o),
                    corto_fullpath(NULL, data->stack[i]->dependency->o));
            }

            for(i = sp - 1; i < data->sp; i++) {
                /* Break first weak dependency */
                if (data->stack[i]->weak && data->stack[i]->dependency->declared) {
                    g_itemResolveDependency(data->stack[i], data);

                    corto_debug("depresolver: break can't %s '%s' before DECLARED|DEFINED '%s'",
                        corto_ptr_str(&data->stack[i]->kind, corto_state_o, 0),
                        corto_fullpath(NULL, data->stack[i]->item->o),
                        corto_fullpath(NULL, data->stack[i]->dependency->o));

                    /* This dependency is already weakened, it cannot be weakened again. */
                    data->stack[i]->weak = FALSE;
                    break;
                }
            }
            corto_debug("depresolver: << end breaking cycle [%p]", dep);
        }
    }
}

/* Resolve cycles.
 *
 * If there are cycles, the only cycles that can be broken are the DECLARED | DEFINED dependencies, which
 * are stored as dependency objects with the 'weak' flag set to TRUE.
 */
static
int g_itemResolveCycles(
    g_item item,
    struct corto_depresolver_s* data)
{
    corto_iter iter;
    g_dependency dep;
    corto_uint32 sp;

    sp = data->sp;


    /* If item has not yet been declared, search onDeclared list. If the item is already declared, the
     * dependencies in this list have already been resolved, thus need not to be evaluated again. */

    if (!item->declared && item->onDeclared) {
        /* Walk dependencies */
        iter = corto_ll_iter(item->onDeclared);
        while((corto_iter_hasNext(&iter))) {
            dep = corto_iter_next(&iter);
            g_itemResolveDependencyCycles(dep, data);
        }
    }

    /* Walk onDefined list if item is not yet defined. */
    if (!item->defined && item->onDefined) {
        /* Walk dependencies */
        iter = corto_ll_iter(item->onDefined);
        while((corto_iter_hasNext(&iter))) {
            dep = corto_iter_next(&iter);

            corto_debug("depresolver: onDefine: can't %s '%s' before DECLARED|DEFINED '%s' (marked = %d, iteration = %d)",
                corto_ptr_str(&dep->kind, corto_state_o, 0),
                corto_fullpath(NULL, dep->item->o),
                corto_fullpath(NULL, dep->dependency->o),
                dep->marked,
                data->iteration);

            g_itemResolveDependencyCycles(dep, data);
        }
     }

    data->sp = sp;

    return 0;
}

/* Walk objects in correct dependency order. */
corto_depresolver corto_depresolverCreate(
    corto_depresolver_action onDeclare,
    corto_depresolver_action onDefine,
    void* userData)
{
    corto_depresolver result;

    result = corto_alloc(sizeof(struct corto_depresolver_s));

    result->items = corto_ll_new();
    result->toPrint = corto_ll_new();
    result->onDeclare = onDeclare;
    result->onDefine = onDefine;
    result->userData = userData;
    result->iteration = 0;

    return result;
}

/* Insert dependency relation.
 *   @param o The dependee object.
 *   @param kind Specifies whether the dependee potentially may be defined or declared after the dependency is resolved.
 *   @param dependency The dependency object.
 *   @param dependencyKind The dependency object must reach at least this state before the dependency can be resolved.
 */
void corto_depresolver_depend(
    corto_depresolver this,
    void* o,
    corto_state kind,
    void* d,
    corto_state dependencyKind)
{
    g_dependency dep;
    g_item dependent, dependency;

    corto_debug("depresolver: can't %s '%s' before %s '%s'",
        corto_ptr_str(&kind, corto_state_o, 0),
        corto_fullpath(NULL, o),
        corto_ptr_str(&dependencyKind, corto_state_o, 0),
        corto_fullpath(NULL, d));

    dependent = g_itemLookup(o, this);
    dependency = g_itemLookup(d, this);

    if (dependent->o != dependency->o) {

        /* Create dependency object */
        dep = corto_alloc(sizeof(struct g_dependency));
        dep->kind = kind;
        dep->item = dependent;
        dep->dependency = dependency;
        dep->weak = FALSE;
        dep->marked = FALSE;
        dep->processed = FALSE;

        /* Increase corresponding counter */
        switch(kind) {
        case CORTO_DECLARED:
            dependent->declareCount++;
            break;
        case CORTO_VALID:
            dependent->defineCount++;
            break;
        default:
            corto_assert(0, "invalid dependee-kind.");
            break;
        }

        /* Insert in corresponding list of dependency */
        switch(dependencyKind) {
        case CORTO_DECLARED:
            if (!dependency->onDeclared) {
                dependency->onDeclared = corto_ll_new();
            }
            corto_ll_insert(dependency->onDeclared, dep);
            break;
        case CORTO_DECLARED | CORTO_VALID:
            dep->weak = TRUE;
            /* no break */
        case CORTO_VALID:
            if (!dependency->onDefined) {
                dependency->onDefined = corto_ll_new();
            }
            corto_ll_insert(dependency->onDefined, dep);
            break;
        default:
            corto_assert(0, "invalid dependency-kind (%d)", dependencyKind);
            break;
        }
    }
}

void corto_depresolver_insert(corto_depresolver this, void *item) {
    g_itemLookup(item, this);
}

int corto_depresolver_walk(corto_depresolver this) {
    corto_iter iter;
    g_item item;
    corto_uint32 unresolved = 0;

    /* Print initial items */
    if (g_itemPrintItems(this)) {
        goto error;
    }

    /* Resolve items with cycles */
    iter = corto_ll_iter(this->items);
    while(corto_iter_hasNext(&iter)) {
        item = corto_iter_next(&iter);

        this->iteration ++;

        /* Process objects that have not yet been defined or declared */
        if (!item->defined) {
            corto_debug("depresolver: item '%s' has cycles (declareCount = %d, defineCount = %d, onDeclare = %d, onDefine = %d)",
                corto_fullpath(NULL, item->o),
                item->declareCount,
                item->defineCount,
                item->onDeclared ? corto_ll_count(item->onDeclared) : 0,
                item->onDefined ? corto_ll_count(item->onDefined) : 0);

            /* Locate and resolve cycles */
            this->sp = 0;
            if (g_itemResolveCycles(item, this)) {
                goto error;
            }

            /* Print items after resolving cycle(s) for item. */
            if (g_itemPrintItems(this)) {
                goto error;
            }
        }
    }


    /* Free items and check if there are still undeclared or undefined objects. */
    while((item = corto_ll_takeFirst(this->items))) {
        if (!item->defined) {
            if (!item->declared) {
                corto_warning("not declared/defined: '%s'",
                    corto_fullpath(NULL, item->o));
                unresolved++;
            } else if (!item->defined){
                corto_warning("not defined: '%s'",
                    corto_fullpath(NULL, item->o));
                unresolved++;
            }
        }
        g_itemFree(item);
    }

    /* Free lists */
    corto_ll_free(this->toPrint);
    corto_ll_free(this->items);

    /* Free this */
    corto_dealloc(this);

    if (unresolved) {
        corto_throw("unsolvable dependecy cycles encountered in data");
        goto error;
    }

    return 0;
error:
    return -1;
}
