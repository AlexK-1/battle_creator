/*
TOML-like parser

Syntax:
```
    string = "ab\ncd"
    bool1 = true
    bool2 = false

    # Comment
    
    [table]
    int1 = 1234
    int2 = 0xABCD
    int3 = 0o7201
    int4 = 0b110101110001

    [[array]]
    value = 123
    data = "qwerty"

    [[array]]
    value = 456
    data = "azerty"
```

There are NO JSON-like tables and arrays

Example program:
```c
    #include "conf.h"

    int main() {
        // Open file
        FILE *f = fopen("test.cfg", "r");

        // Parse file
        ConfigFailData fail;
        ConfigTable *table = config_parse_file(f, &fail);
        fclose(f);
        if (fail.fail) {
            // Print error
            printf("ERROR at %d:%d\n", fail.pos.line, fail.pos.col);
            config_table_free(table);
            return 1;
        }

        ConfigValue *string_value = config_table_get(table, "string"); // Get field
        printf("string: \"%s\"\n", string_value->v.s.p); // Get and print string

        puts(config_table_get(table, "bool1")->v.b ? "true" : "false");

        // Get inner table
        ConfigTable *t = config_table_get(table, "table")->v.t;
        printf("%ld\n", config_table_get(t, "int1")->v.i);

        // Get array
        ConfigArray *a = config_table_get(table, "array")->v.a;
        for (int i = 0; i < config_array_len(a); i++) {
            ConfigTable *t = config_array_get(a, i)->v.t; // Get item of array
            printf("%d: %s %ld\n",
                   i,
                   config_table_get(t, "data")->v.s.p,
                   config_table_get(t, "value")->v.i);
        }

        putchar('\n');

        // Create new table
        ConfigValue new_table = config_table();
        config_table_insert(new_table.v.t, "data", config_string("New string!")); // Add field
        config_table_insert(new_table.v.t, "value", config_int(789)); // Add field

        // Add table to array
        config_array_append(a, new_table);

        // Print table to the console
        config_write_table(table, stdout);

        config_table_free(table);
    }
```
*/

#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    CONF_REMOVED,
    CONF_INT,
    CONF_BOOL,
    CONF_STRING,
    CONF_TABLE,
    CONF_ARRAY
} ConfigValueType;

typedef struct {
    ConfigValueType type;
    bool displayed;
    union {
        struct {
            char *p; // string pointer
            int l; // string length
        } s;
        bool b;
        long int i;
        struct config_table_t *t;
        struct config_array_t *a;
    } v;
} ConfigValue;

typedef struct config_tree_node_t {
    char *key;
    uint32_t hash;
    ConfigValue value;
    struct config_tree_node_t *l, *r;
} ConfigTreeNode;

typedef struct config_table_t {
    ConfigTreeNode *nodes;
    struct config_table_nodes_t {
        ConfigTreeNode **nodes;
        int count, capacity;
    } data /*int, bool, str*/, labels /*table, array*/;
} ConfigTable;

typedef struct config_array_t {
    ConfigValue *values;
    int count;
    int capacity;
} ConfigArray;

typedef struct {
    int col, line;
} Pos;

typedef struct {
    bool fail;
    Pos pos;
} ConfigFailData;

ConfigTable *config_parse(const char *str, int len, ConfigFailData *fail_data);
ConfigTable *config_parse_file(FILE *file, ConfigFailData *fail_data);
void config_write_table(ConfigTable *table, FILE *file);

ConfigTable *config_table_init();
void config_table_free(ConfigTable *table);
ConfigValue *config_table_get(ConfigTable *table, const char *key);
void config_table_insert(ConfigTable *table, const char *key, ConfigValue value);
void config_table_remove(ConfigTable *table, const char *key);

ConfigArray *config_array_init();
void config_array_free(ConfigArray *array);
ConfigValue *config_array_get(ConfigArray *array, int index);
int config_array_append(ConfigArray *array, ConfigValue value);
int config_array_insert(ConfigArray *array, ConfigValue value, int index);
void config_array_remove(ConfigArray *array, int index);
int config_array_len(ConfigArray *array);

void config_value_set(ConfigValue *value, ConfigValue new_value);
ConfigValue config_int(long int x);
ConfigValue config_bool(bool x);
ConfigValue config_string(const char *x);
ConfigValue config_table();
ConfigValue config_array();

#endif // CONFIG_H
