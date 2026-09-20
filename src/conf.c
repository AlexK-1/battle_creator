#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "conf.h"


/* <====================================================== VALUES ====================================================> */

ConfigValue config_int(long int x) {
    return (ConfigValue){.type = CONF_INT, .v.i = x, .displayed = true};
}

ConfigValue config_bool(bool x) {
    return (ConfigValue){.type = CONF_BOOL, .v.b = x, .displayed = true};
}

ConfigValue config_string(const char *x) {
    int len = strlen(x);
    char *str = malloc(len + 1);
    memcpy(str, x, len);
    str[len] = '\0';
    
    return (ConfigValue){.type = CONF_STRING, .v.s.p = str, .v.s.l = len, .displayed = true};
}

ConfigValue config_table() {
    return (ConfigValue){.type = CONF_TABLE, .v.t = config_table_init(), .displayed = true};
}

ConfigValue config_array() {
    return (ConfigValue){.type = CONF_ARRAY, .v.a = config_array_init(), .displayed = true};
}

static void value_free(const ConfigValue *value) {
    if (value == NULL)
        return;
    
    if (value->type == CONF_STRING)
        free(value->v.s.p);
    else if (value->type == CONF_TABLE)
        config_table_free(value->v.t);
    else if (value->type == CONF_ARRAY)
        config_array_free(value->v.a);
}

void config_value_set(ConfigValue *value, ConfigValue new_value) {
    value_free(value);
    *value = new_value;
}


/* <====================================================== TABLE =====================================================> */

// Hash function
static inline uint32_t fnv_1(const char *str, int len) {
    uint32_t hash = 0x811c9dc5;
    for (int i = 0; i < len; i++) {
        hash *= 0x01000193;
        hash ^= str[i];
    }
    return hash;
}

static void node_free(ConfigTreeNode *node);

ConfigTable *config_table_init() {
    ConfigTable *table = malloc(sizeof(*table));

    table->data.capacity = table->labels.capacity = 5;
    table->data.count = table->labels.count = 0;
    table->data.nodes = calloc(table->data.capacity, sizeof(*table->data.nodes));
    table->labels.nodes = calloc(table->labels.capacity, sizeof(*table->labels.nodes));
    table->nodes = NULL;
    
    return table;
}

// Intaernal function
// key must be allocated in heap
static void table_insert(ConfigTable *table, char *key, ConfigValue value) {
    uint32_t hash = fnv_1(key, strlen(key));
    
    ConfigTreeNode *new_node = malloc(sizeof(*new_node));
    new_node->key = key;
    new_node->hash = hash;
    new_node->value = value;
    new_node->l = new_node->r = NULL;

    ConfigTreeNode **node = &table->nodes, *parent = NULL;
    bool left_child = false;

    while (*node != NULL && (*node)->value.type != CONF_REMOVED) {
        // Replace the value with the same key
        if (hash == (*node)->hash && strcmp(key, (*node)->key) == 0) {
            (*node)->value.type = CONF_REMOVED;
            break;
        }

        parent = *node;
        if (hash <= (*node)->hash) {
            left_child = true;
            node = &(*node)->l;
        } else {
            left_child = false;
            node = &(*node)->r;
        }
    }

    bool old_value_removed = *node != NULL && (*node)->value.type == CONF_REMOVED;
    ConfigTreeNode *old_node = *node;
    
    if (old_value_removed) {
        // Copy chieldren from old removed node to new node
        new_node->l = (*node)->l;
        new_node->r = (*node)->r;

        // Free old node
        node_free(*node);
    }

    *node = new_node;

    struct config_table_nodes_t *nodes_list = (value.type == CONF_TABLE || value.type == CONF_ARRAY) ? &table->labels : &table->data;

    if (old_value_removed && parent != NULL) {
        // Replace child-pointer in the parent
        if (left_child)
            parent->l = new_node;
        else
            parent->r = new_node;
        
        // Replace old node-pointer from the nodes list
        for (int i = 0; i < nodes_list->count; i++) {
            if (nodes_list->nodes[i] == old_node)
                nodes_list->nodes[i] = new_node;
        }
    } else {
        // Add new node to the list
        if (nodes_list->count == nodes_list->capacity) {
            nodes_list->capacity *= 3;
            nodes_list->nodes = realloc(nodes_list->nodes, sizeof(*nodes_list->nodes) * nodes_list->capacity);
        }
        nodes_list->nodes[nodes_list->count++] = new_node;
    }
}

void config_table_insert(ConfigTable *table, const char *key, ConfigValue value) {
    int len = strlen(key);
    char *str = malloc(len + 1);
    memcpy(str, key, len);
    str[len] = '\0';

    table_insert(table, str, value);
}

ConfigValue *config_table_get(ConfigTable *table, const char *key) {
    uint32_t hash = fnv_1(key, strlen(key));

    ConfigTreeNode *node = table->nodes;

    while (node != NULL) {
        if (hash < node->hash)
            node = node->l;
        else if (hash > node->hash)
            node = node->r;
        else {
            if (strcmp(node->key, key) == 0 && node->value.type != CONF_REMOVED)
                break;
            else
                node = node->l;
        }
    }
    
    if (node == NULL)
        return NULL;
    else
        return &node->value;
}

void config_table_remove(ConfigTable *table, const char *key) {
    ConfigValue *value = config_table_get(table, key);
    value_free(value);
    value->type = CONF_REMOVED;
}

static void node_free(ConfigTreeNode *node) {
    if (node == NULL)
        return;

    value_free(&node->value);
    free(node->key);
    free(node);
}

static void tree_free(ConfigTreeNode *node) {
    if (node == NULL)
        return;
    
    if (node->l != NULL)
        tree_free(node->l);
    if (node->r != NULL)
        tree_free(node->r);

    node_free(node);
}

void config_table_free(ConfigTable *table) {
    if (table == NULL)
        return;

    tree_free(table->nodes);
    free(table->data.nodes);
    free(table->labels.nodes);
    free(table);
}


/* <====================================================== ARRAY =====================================================> */

ConfigArray *config_array_init() {
    ConfigArray *array = malloc(sizeof(*array));

    array->capacity = 5;
    array->count = 0;
    array->values = calloc(array->capacity, sizeof(*array->values));

    return array;
}

int config_array_len(ConfigArray *array) {
    return array->count;
}

ConfigValue *config_array_get(ConfigArray *array, int index) {
    if (index >= array->count)
        return NULL;
    return &array->values[index];
}

int config_array_append(ConfigArray *array, ConfigValue value) {
    if (array->count == array->capacity) {
        array->capacity *= 3;
        array->values = realloc(array->values, sizeof(*array->values) * array->capacity);
    }

    array->values[array->count++] = value;

    return array->count-1;
}

int config_array_insert(ConfigArray *array, ConfigValue value, int index) {
    if (array->count == array->capacity) {
        array->capacity *= 3;
        array->values = realloc(array->values, sizeof(*array->values) * array->capacity);
    }
    
    if (index < array->count)
        memmove(&array->values[index + 1], &array->values[index], (array->count - index)*sizeof(*array->values));

    array->values[index] = value;
    array->count++;

    return index;;
}

void config_array_remove(ConfigArray *array, int index) {
    value_free(&array->values[index]);
    if (index < array->count-1)
        memmove(&array->values[index], &array->values[index + 1], (array->count - index - 1)*sizeof(*array->values));
    array->count--;
}

void config_array_free(ConfigArray *array) {
    if (array == NULL)
        return;
    
    for (int i = 0; i < array->count; i++)
        value_free(&array->values[i]);
    free(array->values);
    free(array);
}


/* <===================================================== PARSING ====================================================> */

static struct {
    const char *str, *ptr, *end;
    int strlen;
    Pos pos;
} ctx;

typedef enum {
    TOKEN_INVALID,
    TOKEN_NEWLINE,
    TOKEN_STRING,
    TOKEN_INT,
    TOKEN_BOOL,
    TOKEN_NAME,
    TOKEN_EQUAL,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_END
} TokenType;

typedef struct {
    TokenType type;
    const char *ptr;
    int len;
    Pos pos;
} Token;

static int next_char() {
    if (*ctx.ptr == '\n') {
        ctx.pos.line++;
        ctx.pos.col = 0; // It will be incremented
    }

    ctx.ptr++;
    ctx.pos.col++;
    
    return (ctx.ptr == ctx.end);
}

static Token get_token(TokenType type, int len) {
    return (Token){.type = type, .ptr = ctx.ptr, .len = len, .pos = ctx.pos};
}

static Token next_token() {
    if (ctx.ptr == ctx.end) return get_token(TOKEN_END, 0);

    while (isspace(*ctx.ptr) && *ctx.ptr != '\n') {
        if (next_char()) return get_token(TOKEN_END, 0);
    }

    while (1) {
        Token token;
        switch (*ctx.ptr) {
        case '#': // Skip comment
            while (*ctx.ptr != '\n') {
                if (next_char()) return get_token(TOKEN_END, 0);
            }
            break;
        case '"':
            if (next_char()) return get_token(TOKEN_END, 0); // Skip '"'
            token = get_token(TOKEN_STRING, 0);
            
            // (") or (\\") and not (\") - end of string
            while (!(*ctx.ptr == '"' && (*(ctx.ptr-1) != '\\' || *(ctx.ptr-2) == '\\'))) {
                if (next_char()) return get_token(TOKEN_END, 0);
                token.len++;
            }

            if (next_char()) return get_token(TOKEN_END, 0); // Skip '"'
            return token;
        case '=':
            token = get_token(TOKEN_EQUAL, 1);
            next_char();
            return token;
        case '[':
            token = get_token(TOKEN_LBRACKET, 1);
            next_char();
            return token;
        case ']':
            token = get_token(TOKEN_RBRACKET, 1);
            next_char();
            return token;
        case ';':
        case '\n':
            token = get_token(TOKEN_NEWLINE, 1);
            next_char();
            return token;
        case '\r':
            break;
        case '\0':
            return get_token(TOKEN_END, 0);
        default:
            if (isdigit(*ctx.ptr)) { // number: 123 | 0x123 | 0o123 | 0b123
                token = get_token(TOKEN_INT, 0);
                while (isdigit(*ctx.ptr) || // decimal
                       (token.len >= 2 && isxdigit(*ctx.ptr)) || // hex
                       (token.len == 1 && token.ptr[0] == '0' && (*ctx.ptr == 'x' || *ctx.ptr == 'o' || *ctx.ptr == 'b')) // 0x 0o 0b
                      ) {
                    token.len++;
                    if (next_char()) break;
                }
                return token;
            } else if (isalnum(*ctx.ptr) || *ctx.ptr == '-' || *ctx.ptr == '_') { // name
                token = get_token(TOKEN_INVALID, 0);
                while (isalnum(*ctx.ptr) || *ctx.ptr == '-' || *ctx.ptr == '_') {
                    token.len++;
                    if (next_char()) break;;
                }
                
                if (strncmp(token.ptr, "true", token.len) == 0 || strncmp(token.ptr, "false", token.len) == 0) {
                    token.type = TOKEN_BOOL;
                    return token;
                } else {
                    token.type = TOKEN_NAME;
                    return token;
                }
            }

            return get_token(TOKEN_INVALID, 0);
        }
    }

    return get_token(TOKEN_INVALID, 0);
}

static char *escape_to_basic_string(const char *str, int strlen, int *newlen) {
    char *newstr = malloc(strlen + 1);
    int j = 0;
    
    bool escape = false;
    for (int i = 0; i < strlen; i++) {
        char c = str[i];
        if (escape) {
            switch (c) {
                case 'n': newstr[j++] = '\n'; break;
                case '"': newstr[j++] = '\"'; break;
                case 'r': newstr[j++] = '\r'; break;
                case 't': newstr[j++] = '\t'; break;
                case 'b': newstr[j++] = '\b'; break;
                case 'a': newstr[j++] = '\a'; break;
                case '0': newstr[j++] = '\0'; break;
                case '\\': newstr[j++] = '\\'; break;
                default: newstr[j++] = '\\'; newstr[j++] = c; break;
            }
            escape = false;
        } else {
            if (c == '\\')
                escape = true;
            else
                newstr[j++] = c;
        }
    }
    newstr[j] = '\0';

    *newlen = j;

    return newstr;
}

static void basic_to_escape_string(char *str, int strlen, FILE *f) {
    for (int i = 0; i < strlen; i++) {
        char c = str[i];
        switch (c) {
            case '\n': fputs("\\n", f); break;
            case '\"': fputs("\\\"", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            case '\b': fputs("\\b", f); break;
            case '\a': fputs("\\a", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\0': return;
            default: fputc(c, f); break;
        }
    }
}

#define FAIL()                                                      \
    do {                                                            \
        fail_data->fail = true;                                     \
        goto parse_error;                                           \
    } while (0)

#define TOKEN()                                                     \
    do {                                                            \
        prev_token = token;                                         \
        token = next_token();                                       \
        if (token.type == TOKEN_END || token.type == TOKEN_INVALID) \
            FAIL();                                                 \
    } while (0)

ConfigTable *config_parse(const char *str, int len, ConfigFailData *fail_data) {
    ctx.pos.line = ctx.pos.col = 1;
    ctx.str = ctx.ptr = str;
    ctx.strlen = len;
    ctx.end = ctx.str + ctx.strlen;

    fail_data->fail = false;

    ConfigTable *root_table = config_table_init();
    ConfigTable *table = root_table; // current table
    
    Token token = get_token(TOKEN_NEWLINE, 1);
    Token prev_token = token;

    while (1) {
        prev_token = token;
        token = next_token();
        if (token.type == TOKEN_END)
            break;
        else if (token.type == TOKEN_INVALID)
            FAIL();

        // key = value
        if (prev_token.type == TOKEN_NEWLINE && token.type == TOKEN_NAME) {
            const char *name = token.ptr;
            int name_len = token.len;

            TOKEN();
            if (token.type != TOKEN_EQUAL) FAIL();

            TOKEN();
            ConfigValue value = {.displayed = true};
            if (token.type == TOKEN_STRING) {
                value.type = CONF_STRING;

                value.v.s.p = escape_to_basic_string(token.ptr, token.len, &value.v.s.l);
            } else if (token.type == TOKEN_INT) {
                value.type = CONF_INT;

                int base = 10;
                const char *int_ptr = token.ptr;
                if (int_ptr[0] == '0' && token.len >= 2 && isalpha(int_ptr[1])) {
                    if (int_ptr[1] == 'b') base = 2;
                    else if (int_ptr[1] == 'o') base = 8;
                    else if (int_ptr[1] == 'x') base = 16;
                    else FAIL();
                    int_ptr += 2;
                }

                char *p;
                value.v.i = strtol(int_ptr, &p, base);

                if (*p != '\n' && *p != ';' && *p != '\0') FAIL();
            } else if (token.type == TOKEN_BOOL) {
                value.type = CONF_BOOL;
                if (strncmp(token.ptr, "true", token.len) == 0)
                    value.v.b = true;
                else if (strncmp(token.ptr, "false", token.len) == 0)
                    value.v.b = false;
                else
                    FAIL();
            } else
                FAIL();

            char *new_name = malloc(name_len + 1);
            memcpy(new_name, name, name_len);
            new_name[name_len] = '\0';
            
            table_insert(table, new_name, value);

            // Skip \n or ';'
            prev_token = token;
            token = next_token();
            if (token.type == TOKEN_END)
                break;
            else if (token.type != TOKEN_NEWLINE)
                FAIL();
        } 

        // [[array]]
        // [table]
        else if (prev_token.type == TOKEN_NEWLINE && token.type == TOKEN_LBRACKET) {
            TOKEN();

            if (token.type == TOKEN_LBRACKET) { // [[array]]
                TOKEN();
                if (token.type != TOKEN_NAME) FAIL();
            
                const char *name = token.ptr;
                int name_len = token.len;

                TOKEN();
                if (token.type != TOKEN_RBRACKET) FAIL();
                TOKEN();
                if (token.type != TOKEN_RBRACKET) FAIL();

                char *new_name = malloc(name_len + 1);
                memcpy(new_name, name, name_len);
                new_name[name_len] = '\0';
            
                ConfigValue *array_value_ptr = config_table_get(root_table, new_name);
                ConfigArray *array;
            
                if (array_value_ptr == NULL) {
                    // Create array
                    ConfigValue array_value = {.type = CONF_ARRAY, .displayed = true};
                    array_value.v.a = array = config_array_init();
                    table_insert(root_table, new_name, array_value);
                } else {
                    array = array_value_ptr->v.a;
                    free(new_name);
                }
            
                ConfigValue table_value = {.type = CONF_TABLE, .v.t = config_table_init(), .displayed = true};
                config_array_append(array, table_value);

                table = table_value.v.t;
            } else if (token.type == TOKEN_NAME) { // [table]
                const char *name = token.ptr;
                int name_len = token.len;

                TOKEN();
                if (token.type != TOKEN_RBRACKET) FAIL();

                char *new_name = malloc(name_len + 1);
                memcpy(new_name, name, name_len);
                new_name[name_len] = '\0';

                ConfigValue table_value = {.type = CONF_TABLE, .v.t = config_table_init(), .displayed = true};
                table_insert(root_table, new_name, table_value);

                table = table_value.v.t;
            }
        }

        else if (token.type == TOKEN_NEWLINE) {
            ;
        } else
            FAIL();
    }

    parse_error:
    if (fail_data->fail) {
        fail_data->pos = token.pos;
    }

    return root_table;
}

ConfigTable *config_parse_file(FILE *file, ConfigFailData *fail_data) {
    fseek(file, 0, SEEK_END);
    int size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *str = malloc(size);
    fread(str, 1, size, file);

    ConfigTable *table = config_parse(str, size, fail_data);
    
    free(str);
    return table;
}


/* <===================================================== WRITING ====================================================> */

static void write_array(char *key, ConfigArray *array, FILE *file);
static void write_table(char *key, ConfigTable *table, bool array, FILE *file);

static void write_value(char *key, ConfigValue *value, FILE *file) {
    if (!value->displayed)
        return;
    
    switch (value->type) {
        case CONF_REMOVED: break;
        case CONF_STRING:
            fprintf(file, "%s = \"", key);
            basic_to_escape_string(value->v.s.p, strlen(value->v.s.p), file);
            fputs("\"\n", file);
            break;
        case CONF_INT:     fprintf(file, "%s = %ld\n", key, value->v.i); break;
        case CONF_BOOL:    fprintf(file, "%s = %s\n", key, value->v.b ? "true" : "false"); break;
        case CONF_TABLE:   write_table(key, value->v.t, false, file); break;
        case CONF_ARRAY:   write_array(key, value->v.a, file); break;
    }
}

static void write_array(char *key, ConfigArray *array, FILE *file) {
    for (int i = 0; i < array->count; i++) {
        ConfigValue *value = &array->values[i];
        if (value->type == CONF_TABLE)
            write_table(key, value->v.t, true, file);
        else
            write_value(key, value, file);
    }
}

static void write_table(char *key, ConfigTable *table, bool array, FILE *file) {
    if (key != NULL) {
        if (array)
            fprintf(file, "\n[[%s]]\n", key);
        else
            fprintf(file, "\n[%s]\n", key);
    }

    for (int i = 0; i < table->data.count; i++) {
        ConfigTreeNode *node = table->data.nodes[i];
        write_value(node->key, &node->value, file);
    }

    for (int i = 0; i < table->labels.count; i++) {
        ConfigTreeNode *node = table->labels.nodes[i];
        write_value(node->key, &node->value, file);
    }
}

void config_write_table(ConfigTable *table, FILE *file) {
    write_table(NULL, table, false, file);
}
