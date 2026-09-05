#include "var_struct_map.h"
#include <stdlib.h>
#include <string.h>

typedef struct VarMap {
    char *var;
    char *struct_name;
    struct VarMap *next;
} VarMap;

static VarMap *g_map = NULL;

void var_struct_map_set(const char *var, const char *struct_name) {
    for (VarMap *m = g_map; m; m = m->next) {
        if (strcmp(m->var, var) == 0) {
            free(m->struct_name);
            m->struct_name = strdup(struct_name);
            return;
        }
    }
    VarMap *new = malloc(sizeof(VarMap));
    new->var = strdup(var);
    new->struct_name = strdup(struct_name);
    new->next = g_map;
    g_map = new;
}

const char *var_struct_map_get(const char *var) {
    for (VarMap *m = g_map; m; m = m->next) {
        if (strcmp(m->var, var) == 0) return m->struct_name;
    }
    return NULL;
}