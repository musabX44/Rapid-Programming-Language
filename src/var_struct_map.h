#ifndef VAR_STRUCT_MAP_H
#define VAR_STRUCT_MAP_H

void var_struct_map_set(const char *var, const char *struct_name);
const char *var_struct_map_get(const char *var);
/* M36: her fonksiyon govdesi baslarken cagrilir; haritayi (scope'suz global
   olmaktan cikarip) fonksiyon-yerel yapar. */
void var_struct_map_clear(void);

#endif