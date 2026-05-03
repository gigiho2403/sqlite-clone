/*
 * db.c — Build Your Own SQLite
 * Tutorial: https://cstack.github.io/db_tutorial/
 *
 * Part 1: REPL skeleton
 * Part 2: SQL compiler + virtual machine stubs
 * Part 3: In-memory, append-only, single-table database
 * Part 4: Tests — validation (string too long, negative id)
 * Part 5: Persistence to disk via Pager
 * Part 6: Cursor abstraction
 * Part 7: B-Tree leaf node format
 * Part 8: Leaf node splitting + internal node root
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ─── Column / Row constants ─────────────────────────────────────────────── */

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE    255
#define TABLE_MAX_PAGES      100

#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0)->Attribute)

/* In-memory Row — strings +1 for null terminator */
typedef struct {
    uint32_t id;
    char     username[COLUMN_USERNAME_SIZE + 1];
    char     email[COLUMN_EMAIL_SIZE + 1];
} Row;

/* Packed sizes on disk (no null terminators) */
const uint32_t ID_SIZE         = sizeof(uint32_t);
const uint32_t USERNAME_SIZE   = COLUMN_USERNAME_SIZE;
const uint32_t EMAIL_SIZE      = COLUMN_EMAIL_SIZE;
const uint32_t ID_OFFSET       = 0;
const uint32_t USERNAME_OFFSET = sizeof(uint32_t);
const uint32_t EMAIL_OFFSET    = sizeof(uint32_t) + COLUMN_USERNAME_SIZE;
const uint32_t ROW_SIZE        = sizeof(uint32_t) + COLUMN_USERNAME_SIZE + COLUMN_EMAIL_SIZE;

/* 4 + 32 + 255 = 291 bytes per row */

const uint32_t PAGE_SIZE = 4096;

/* ─── B-Tree Node Layout ─────────────────────────────────────────────────── */

typedef enum { NODE_INTERNAL, NODE_LEAF } NodeType;

/*
 * Common Node Header (6 bytes total):
 *   offset 0: node_type  — uint8_t
 *   offset 1: is_root    — uint8_t
 *   offset 2: parent_ptr — uint32_t
 */
const uint32_t NODE_TYPE_SIZE          = sizeof(uint8_t);
const uint32_t NODE_TYPE_OFFSET        = 0;
const uint32_t IS_ROOT_SIZE            = sizeof(uint8_t);
const uint32_t IS_ROOT_OFFSET          = sizeof(uint8_t);
const uint32_t PARENT_POINTER_SIZE     = sizeof(uint32_t);
const uint32_t PARENT_POINTER_OFFSET   = sizeof(uint8_t) + sizeof(uint8_t);
const uint32_t COMMON_NODE_HEADER_SIZE = sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t);

/*
 * Leaf Node Header (14 bytes total = 6 common + 4 num_cells + 4 next_leaf):
 *   offset  6: num_cells — uint32_t
 *   offset 10: next_leaf — uint32_t  (page number of right sibling; 0 = none)
 */
const uint32_t LEAF_NODE_NUM_CELLS_SIZE   = sizeof(uint32_t);
const uint32_t LEAF_NODE_NUM_CELLS_OFFSET = sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t);
const uint32_t LEAF_NODE_NEXT_LEAF_SIZE   = sizeof(uint32_t);
const uint32_t LEAF_NODE_NEXT_LEAF_OFFSET = sizeof(uint8_t) + sizeof(uint8_t)
                                          + sizeof(uint32_t) + sizeof(uint32_t);
const uint32_t LEAF_NODE_HEADER_SIZE      = sizeof(uint8_t) + sizeof(uint8_t)
                                          + sizeof(uint32_t) + sizeof(uint32_t)
                                          + sizeof(uint32_t);

/*
 * Leaf Node Body — array of cells:
 *   cell = key (4 bytes) + value (ROW_SIZE = 291 bytes) = 295 bytes
 *
 * LEAF_NODE_SPACE_FOR_CELLS = 4096 - 14 = 4082
 * LEAF_NODE_MAX_CELLS       = 4082 / 295 = 13
 */
const uint32_t LEAF_NODE_KEY_SIZE        = sizeof(uint32_t);
const uint32_t LEAF_NODE_KEY_OFFSET      = 0;
const uint32_t LEAF_NODE_VALUE_SIZE      = sizeof(uint32_t) + COLUMN_USERNAME_SIZE + COLUMN_EMAIL_SIZE;
const uint32_t LEAF_NODE_VALUE_OFFSET    = sizeof(uint32_t);
const uint32_t LEAF_NODE_CELL_SIZE       = sizeof(uint32_t)
                                         + sizeof(uint32_t) + COLUMN_USERNAME_SIZE + COLUMN_EMAIL_SIZE;
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = 4096
                                         - (sizeof(uint8_t) + sizeof(uint8_t)
                                            + sizeof(uint32_t) + sizeof(uint32_t)
                                            + sizeof(uint32_t));
const uint32_t LEAF_NODE_MAX_CELLS       = (4096 - (sizeof(uint8_t) + sizeof(uint8_t)
                                            + sizeof(uint32_t) + sizeof(uint32_t)
                                            + sizeof(uint32_t)))
                                         / (sizeof(uint32_t) + sizeof(uint32_t)
                                            + COLUMN_USERNAME_SIZE + COLUMN_EMAIL_SIZE);

/*
 * Split counts: distribute LEAF_NODE_MAX_CELLS+1 items across two nodes.
 * RIGHT = (13+1)/2 = 7 — goes into the new (right) node.
 * LEFT  = (13+1)-7 = 7 — stays in the old (left) node.
 */
const uint32_t LEAF_NODE_RIGHT_SPLIT_COUNT = (13 + 1) / 2;
const uint32_t LEAF_NODE_LEFT_SPLIT_COUNT  = (13 + 1) - (13 + 1) / 2;

/*
 * Internal Node Header (14 bytes total = 6 common + 4 num_keys + 4 right_child):
 *   offset  6: num_keys    — uint32_t
 *   offset 10: right_child — uint32_t (page number)
 *
 * Internal Node Body — array of cells:
 *   cell = child_ptr (4 bytes) + key (4 bytes) = 8 bytes
 */
const uint32_t INTERNAL_NODE_NUM_KEYS_SIZE      = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_NUM_KEYS_OFFSET    = sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t);
const uint32_t INTERNAL_NODE_RIGHT_CHILD_SIZE   = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_RIGHT_CHILD_OFFSET = sizeof(uint8_t) + sizeof(uint8_t)
                                                + sizeof(uint32_t) + sizeof(uint32_t);
const uint32_t INTERNAL_NODE_HEADER_SIZE        = sizeof(uint8_t) + sizeof(uint8_t)
                                                + sizeof(uint32_t) + sizeof(uint32_t)
                                                + sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CHILD_SIZE         = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_KEY_SIZE           = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CELL_SIZE          = sizeof(uint32_t) + sizeof(uint32_t);
/* Keep small so tests trigger internal-node splits with few rows */
const uint32_t INTERNAL_NODE_MAX_CELLS          = 3;

/* Internal node split bookkeeping */
#define INVALID_PAGE_NUM UINT32_MAX
const uint32_t INTERNAL_NODE_LEFT_SPLIT_COUNT  = (3 + 1) / 2; /* = 2 */
const uint32_t INTERNAL_NODE_RIGHT_SPLIT_COUNT = 3 - (3 + 1) / 2; /* = 1 */

/* ─── Node Type Accessors ────────────────────────────────────────────────── */

NodeType get_node_type(void* node) {
    uint8_t value = *((uint8_t*)((char*)node + NODE_TYPE_OFFSET));
    return (NodeType)value;
}

void set_node_type(void* node, NodeType type) {
    *((uint8_t*)((char*)node + NODE_TYPE_OFFSET)) = (uint8_t)type;
}

bool is_root(void* node) {
    uint8_t value = *((uint8_t*)((char*)node + IS_ROOT_OFFSET));
    return (bool)value;
}

void set_root(void* node, bool is_root_node) {
    *((uint8_t*)((char*)node + IS_ROOT_OFFSET)) = (uint8_t)is_root_node;
}

uint32_t* node_parent(void* node) {
    return (uint32_t*)((char*)node + PARENT_POINTER_OFFSET);
}

/* ─── Leaf Node Accessors ────────────────────────────────────────────────── */

uint32_t* leaf_node_num_cells(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NUM_CELLS_OFFSET);
}

uint32_t* leaf_node_next_leaf(void* node) {
    return (uint32_t*)((char*)node + LEAF_NODE_NEXT_LEAF_OFFSET);
}

void* leaf_node_cell(void* node, uint32_t cell_num) {
    return (char*)node + LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE;
}

uint32_t* leaf_node_key(void* node, uint32_t cell_num) {
    return (uint32_t*)leaf_node_cell(node, cell_num);
}

void* leaf_node_value(void* node, uint32_t cell_num) {
    return (char*)leaf_node_cell(node, cell_num) + LEAF_NODE_KEY_SIZE;
}

void initialize_leaf_node(void* node) {
    set_node_type(node, NODE_LEAF);
    set_root(node, false);
    *leaf_node_num_cells(node) = 0;
    *leaf_node_next_leaf(node) = 0;   /* 0 = no right sibling */
}

/* ─── Internal Node Accessors ────────────────────────────────────────────── */

uint32_t* internal_node_num_keys(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_NUM_KEYS_OFFSET);
}

uint32_t* internal_node_right_child(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}

uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_HEADER_SIZE
                       + cell_num * INTERNAL_NODE_CELL_SIZE);
}

/* child_num == num_keys → right_child; otherwise → cell[child_num].child_ptr */
uint32_t* internal_node_child(void* node, uint32_t child_num) {
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_num > num_keys) {
        printf("Tried to access child_num %u > num_keys %u\n", child_num, num_keys);
        exit(EXIT_FAILURE);
    }
    if (child_num == num_keys) {
        return internal_node_right_child(node);
    }
    return internal_node_cell(node, child_num);
}

uint32_t* internal_node_key(void* node, uint32_t key_num) {
    return (uint32_t*)((char*)internal_node_cell(node, key_num) + INTERNAL_NODE_CHILD_SIZE);
}

void initialize_internal_node(void* node) {
    set_node_type(node, NODE_INTERNAL);
    set_root(node, false);
    *internal_node_num_keys(node) = 0;
}

/*
 * internal_node_find_child — binary search the separator keys to find which
 * child subtree contains `key`.  Returns child index in [0 .. num_keys]:
 *   index == num_keys  →  right_child pointer
 *   index <  num_keys  →  cell[index].child_ptr
 */
uint32_t internal_node_find_child(void* node, uint32_t key) {
    uint32_t num_keys  = *internal_node_num_keys(node);
    uint32_t min_index = 0;
    uint32_t max_index = num_keys;   /* right child lives past all keys */

    while (min_index != max_index) {
        uint32_t index        = (min_index + max_index) / 2;
        uint32_t key_to_right = *internal_node_key(node, index);
        if (key_to_right >= key) {
            max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    return min_index;
}

/*
 * update_internal_node_key — after a child's maximum key changes (e.g. the
 * child was split), find the separator key that used to be `old_key` and
 * overwrite it with `new_key`.
 */
void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
    uint32_t old_child_index = internal_node_find_child(node, old_key);
    *internal_node_key(node, old_child_index) = new_key;
}

/* ─── Node Utilities ─────────────────────────────────────────────────────── */

/* Forward declarations — Pager and get_page are defined below */
typedef struct Pager Pager;
void* get_page(Pager* pager, uint32_t page_num);

/*
 * get_node_max_key — largest key in the subtree rooted at `node`.
 * Leaf: last cell key.
 * Internal: follow right_child recursively — the right_child subtree always
 *           holds the largest keys; separator keys are NOT the subtree max.
 */
uint32_t get_node_max_key(Pager* pager, void* node) {
    if (get_node_type(node) == NODE_LEAF) {
        return *leaf_node_key(node, *leaf_node_num_cells(node) - 1);
    }
    void* right_child = get_page(pager, *internal_node_right_child(node));
    return get_node_max_key(pager, right_child);
}

/* ─── Pager — page cache + file I/O ─────────────────────────────────────── */

struct Pager {
    int      file_descriptor;
    uint32_t file_length;
    uint32_t num_pages;          /* number of pages currently tracked */
    void*    pages[TABLE_MAX_PAGES];
};

Pager* pager_open(const char* filename) {
    int fd = open(filename,
                  O_RDWR | O_CREAT,
                  S_IRUSR | S_IWUSR);
    if (fd == -1) {
        printf("Unable to open file '%s': %s\n", filename, strerror(errno));
        exit(EXIT_FAILURE);
    }

    off_t file_length = lseek(fd, 0, SEEK_END);
    if (file_length == -1) {
        printf("lseek error: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (file_length % PAGE_SIZE != 0) {
        printf("Db file is not a whole number of pages. Corrupt file.\n");
        exit(EXIT_FAILURE);
    }

    Pager* p           = malloc(sizeof(Pager));
    p->file_descriptor = fd;
    p->file_length     = (uint32_t)file_length;
    p->num_pages       = (uint32_t)file_length / PAGE_SIZE;
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) p->pages[i] = NULL;
    return p;
}

/*
 * get_page — returns a pointer to the in-memory page.
 * On cache miss: allocates a blank page and reads from disk if the page exists.
 * Also updates pager->num_pages when a new page beyond the current count is touched.
 */
void* get_page(Pager* pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) {
        printf("Tried to fetch page %u out of bounds (%u).\n",
               page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }

    if (pager->pages[page_num] == NULL) {
        void* page = malloc(PAGE_SIZE);

        if (page_num < pager->num_pages) {
            /* Page exists on disk — read it in */
            off_t offset = (off_t)page_num * PAGE_SIZE;
            if (lseek(pager->file_descriptor, offset, SEEK_SET) == -1) {
                printf("lseek error: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
            ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
            if (bytes_read == -1) {
                printf("read error: %s\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
        }

        pager->pages[page_num] = page;

        if (page_num >= pager->num_pages) {
            pager->num_pages = page_num + 1;
        }
    }

    return pager->pages[page_num];
}

/*
 * pager_flush — write a full page to disk.
 * All nodes are PAGE_SIZE; there are no partial pages in the B-tree format.
 */
void pager_flush(Pager* pager, uint32_t page_num) {
    if (pager->pages[page_num] == NULL) {
        printf("Tried to flush null page.\n");
        exit(EXIT_FAILURE);
    }

    off_t offset = (off_t)page_num * PAGE_SIZE;
    if (lseek(pager->file_descriptor, offset, SEEK_SET) == -1) {
        printf("lseek error: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    ssize_t bytes_written = write(pager->file_descriptor,
                                  pager->pages[page_num], PAGE_SIZE);
    if (bytes_written == -1) {
        printf("write error: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

/* Returns the next available (never-written) page number. */
uint32_t get_unused_page_num(Pager* pager) {
    return pager->num_pages;
}

/* ─── Tree Printer ───────────────────────────────────────────────────────── */

void indent(uint32_t level) {
    for (uint32_t i = 0; i < level; i++) {
        printf("  ");
    }
}

void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level) {
    void*    node     = get_page(pager, page_num);
    uint32_t num_keys;
    uint32_t child_page_num;

    switch (get_node_type(node)) {
        case NODE_LEAF:
            num_keys = *leaf_node_num_cells(node);
            indent(indentation_level);
            printf("- leaf (size %u)\n", num_keys);
            for (uint32_t i = 0; i < num_keys; i++) {
                indent(indentation_level + 1);
                printf("- %u\n", *leaf_node_key(node, i));
            }
            break;
        case NODE_INTERNAL:
            num_keys = *internal_node_num_keys(node);
            indent(indentation_level);
            printf("- internal (size %u)\n", num_keys);
            for (uint32_t i = 0; i < num_keys; i++) {
                child_page_num = *internal_node_child(node, i);
                print_tree(pager, child_page_num, indentation_level + 1);
                indent(indentation_level + 1);
                printf("- key %u\n", *internal_node_key(node, i));
            }
            child_page_num = *internal_node_right_child(node);
            print_tree(pager, child_page_num, indentation_level + 1);
            break;
    }
}

/* ─── Table ──────────────────────────────────────────────────────────────── */

typedef struct {
    Pager*   pager;
    uint32_t root_page_num;
} Table;

/* Forward declaration — internal_node_insert is defined later, after create_new_root */
void internal_node_insert(Table* table, uint32_t parent_page_num,
                          uint32_t child_page_num);

Table* db_open(const char* filename) {
    Pager* pager     = pager_open(filename);
    Table* t         = malloc(sizeof(Table));
    t->pager         = pager;
    t->root_page_num = 0;

    if (pager->num_pages == 0) {
        /* New database file — initialise page 0 as an empty leaf root */
        void* root_node = get_page(pager, 0);
        initialize_leaf_node(root_node);
        set_root(root_node, true);
    }

    return t;
}

void db_close(Table* t) {
    Pager* pager = t->pager;

    /* Flush every in-memory page (always full PAGE_SIZE in B-tree format) */
    for (uint32_t i = 0; i < pager->num_pages; i++) {
        if (pager->pages[i] == NULL) continue;
        pager_flush(pager, i);
        free(pager->pages[i]);
        pager->pages[i] = NULL;
    }

    if (close(pager->file_descriptor) == -1) {
        printf("Error closing db file: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Safety sweep for any remaining allocated pages */
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (pager->pages[i]) {
            free(pager->pages[i]);
            pager->pages[i] = NULL;
        }
    }
    free(pager);
    free(t);
}

/* ─── Serialization ──────────────────────────────────────────────────────── */

void serialize_row(Row* src, void* dst) {
    memcpy(dst + ID_OFFSET,       &src->id,       ID_SIZE);
    memcpy(dst + USERNAME_OFFSET,  src->username,  USERNAME_SIZE);
    memcpy(dst + EMAIL_OFFSET,     src->email,     EMAIL_SIZE);
}

void deserialize_row(void* src, Row* dst) {
    memcpy(&dst->id,       src + ID_OFFSET,       ID_SIZE);
    memcpy( dst->username, src + USERNAME_OFFSET,  USERNAME_SIZE);
    dst->username[USERNAME_SIZE] = '\0';
    memcpy( dst->email,    src + EMAIL_OFFSET,     EMAIL_SIZE);
    dst->email[EMAIL_SIZE] = '\0';
}

void print_row(Row* r) {
    printf("(%u, %s, %s)\n", r->id, r->username, r->email);
}

/* ─── Cursor ─────────────────────────────────────────────────────────────── */

typedef struct {
    Table*   table;
    uint32_t page_num;
    uint32_t cell_num;
    bool     end_of_table;   /* one position past the last cell */
} Cursor;

/*
 * leaf_node_find — binary search within a leaf for the first cell whose key
 * is >= the target key.  Returns a cursor pointing at that cell (or past the
 * end if all keys are smaller).
 */
Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void*    node      = get_page(table->pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    Cursor* cursor    = malloc(sizeof(Cursor));
    cursor->table     = table;
    cursor->page_num  = page_num;
    cursor->end_of_table = false;

    uint32_t min_index    = 0;
    uint32_t one_past_max = num_cells;

    while (one_past_max != min_index) {
        uint32_t index         = (min_index + one_past_max) / 2;
        uint32_t key_at_index  = *leaf_node_key(node, index);
        if (key == key_at_index) {
            cursor->cell_num = index;
            return cursor;
        }
        if (key < key_at_index) {
            one_past_max = index;
        } else {
            min_index = index + 1;
        }
    }

    cursor->cell_num = min_index;
    return cursor;
}

/*
 * internal_node_find — descend through an internal node to find the leaf
 * that should contain the given key.
 */
Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void*    node     = get_page(table->pager, page_num);
    uint32_t num_keys = *internal_node_num_keys(node);

    /* Binary search for the child to descend into */
    uint32_t min_index = 0;
    uint32_t max_index = num_keys; /* right_child lives at index num_keys */

    while (min_index != max_index) {
        uint32_t index        = (min_index + max_index) / 2;
        uint32_t key_to_right = *internal_node_key(node, index);
        if (key_to_right >= key) {
            max_index = index;
        } else {
            min_index = index + 1;
        }
    }

    uint32_t child_num = *internal_node_child(node, min_index);
    void*    child     = get_page(table->pager, child_num);
    switch (get_node_type(child)) {
        case NODE_LEAF:
            return leaf_node_find(table, child_num, key);
        case NODE_INTERNAL:
            return internal_node_find(table, child_num, key);
    }
    exit(EXIT_FAILURE);
}

/*
 * table_find — returns a cursor pointing to the position of the given key,
 * or where it would be inserted if absent.
 */
Cursor* table_find(Table* table, uint32_t key) {
    uint32_t root_page_num = table->root_page_num;
    void*    root_node     = get_page(table->pager, root_page_num);

    if (get_node_type(root_node) == NODE_LEAF) {
        return leaf_node_find(table, root_page_num, key);
    } else {
        return internal_node_find(table, root_page_num, key);
    }
}

/* table_start — cursor at the very first row (smallest key) */
Cursor* table_start(Table* t) {
    Cursor* c    = table_find(t, 0);
    void*   node = get_page(t->pager, c->page_num);
    c->end_of_table = (*leaf_node_num_cells(node) == 0);
    return c;
}

Cursor* table_end(Table* t) {
    Cursor*  c    = malloc(sizeof(Cursor));
    c->table      = t;
    c->end_of_table = true;

    /* Traverse right_child pointers until we reach a leaf */
    uint32_t page_num = t->root_page_num;
    void*    node     = get_page(t->pager, page_num);
    while (get_node_type(node) == NODE_INTERNAL) {
        page_num = *internal_node_right_child(node);
        node     = get_page(t->pager, page_num);
    }

    c->page_num  = page_num;
    c->cell_num  = *leaf_node_num_cells(node);
    return c;
}

/* Returns pointer to the value slot the cursor points at (inside the leaf node) */
void* cursor_value(Cursor* c) {
    void* page = get_page(c->table->pager, c->page_num);
    return leaf_node_value(page, c->cell_num);
}

void cursor_advance(Cursor* c) {
    void*    node = get_page(c->table->pager, c->page_num);
    c->cell_num  += 1;
    if (c->cell_num >= *leaf_node_num_cells(node)) {
        /* Move to right sibling if one exists */
        uint32_t next_page_num = *leaf_node_next_leaf(node);
        if (next_page_num == 0) {
            c->end_of_table = true;
        } else {
            c->page_num = next_page_num;
            c->cell_num = 0;
        }
    }
}

/* ─── InputBuffer ─────────────────────────────────────────────────────────── */

typedef struct {
    char*   buffer;
    size_t  buffer_length;
    ssize_t input_length;
} InputBuffer;

InputBuffer* new_input_buffer() {
    InputBuffer* ib   = malloc(sizeof(InputBuffer));
    ib->buffer        = NULL;
    ib->buffer_length = 0;
    ib->input_length  = 0;
    return ib;
}

void close_input_buffer(InputBuffer* ib) {
    free(ib->buffer);
    free(ib);
}

void print_prompt() { printf("db > "); }

void read_input(InputBuffer* ib) {
    ssize_t bytes_read = getline(&ib->buffer, &ib->buffer_length, stdin);
    if (bytes_read <= 0) {
        printf("Error reading input\n");
        exit(EXIT_FAILURE);
    }
    ib->input_length           = bytes_read - 1;
    ib->buffer[bytes_read - 1] = '\0';
}

/* ─── Meta-commands ──────────────────────────────────────────────────────── */

typedef enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND,
} MetaCommandResult;

MetaCommandResult do_meta_command(InputBuffer* ib, Table* table) {
    if (strcmp(ib->buffer, ".exit") == 0) {
        close_input_buffer(ib);
        db_close(table);
        exit(EXIT_SUCCESS);
    }
    if (strcmp(ib->buffer, ".btree") == 0) {
        printf("Tree:\n");
        print_tree(table->pager, 0, 0);
        return META_COMMAND_SUCCESS;
    }
    if (strcmp(ib->buffer, ".constants") == 0) {
        printf("Constants:\n");
        printf("ROW_SIZE: %u\n",                  ROW_SIZE);
        printf("COMMON_NODE_HEADER_SIZE: %u\n",   COMMON_NODE_HEADER_SIZE);
        printf("LEAF_NODE_HEADER_SIZE: %u\n",     LEAF_NODE_HEADER_SIZE);
        printf("LEAF_NODE_CELL_SIZE: %u\n",       LEAF_NODE_CELL_SIZE);
        printf("LEAF_NODE_SPACE_FOR_CELLS: %u\n", LEAF_NODE_SPACE_FOR_CELLS);
        printf("LEAF_NODE_MAX_CELLS: %u\n",       LEAF_NODE_MAX_CELLS);
        return META_COMMAND_SUCCESS;
    }
    return META_COMMAND_UNRECOGNIZED_COMMAND;
}

/* ─── Statement / prepare ────────────────────────────────────────────────── */

typedef enum {
    PREPARE_SUCCESS,
    PREPARE_NEGATIVE_ID,
    PREPARE_STRING_TOO_LONG,
    PREPARE_SYNTAX_ERROR,
    PREPARE_UNRECOGNIZED_STATEMENT,
} PrepareResult;

typedef enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
} StatementType;

typedef struct {
    StatementType type;
    Row           row_to_insert;
} Statement;

PrepareResult prepare_insert(InputBuffer* ib, Statement* stmt) {
    stmt->type = STATEMENT_INSERT;

    int  id;
    char username[COLUMN_USERNAME_SIZE + 2];
    char email[COLUMN_EMAIL_SIZE + 2];

    int assigned = sscanf(ib->buffer, "insert %d %33s %256s", &id, username, email);
    if (assigned < 3) return PREPARE_SYNTAX_ERROR;

    if (id < 0)                                   return PREPARE_NEGATIVE_ID;
    if (strlen(username) > COLUMN_USERNAME_SIZE)  return PREPARE_STRING_TOO_LONG;
    if (strlen(email)    > COLUMN_EMAIL_SIZE)     return PREPARE_STRING_TOO_LONG;

    stmt->row_to_insert.id = (uint32_t)id;
    strncpy(stmt->row_to_insert.username, username, COLUMN_USERNAME_SIZE);
    strncpy(stmt->row_to_insert.email,    email,    COLUMN_EMAIL_SIZE);
    stmt->row_to_insert.username[COLUMN_USERNAME_SIZE] = '\0';
    stmt->row_to_insert.email[COLUMN_EMAIL_SIZE]       = '\0';

    return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer* ib, Statement* stmt) {
    if (strncmp(ib->buffer, "insert", 6) == 0) return prepare_insert(ib, stmt);
    if (strcmp(ib->buffer,  "select") == 0) {
        stmt->type = STATEMENT_SELECT;
        return PREPARE_SUCCESS;
    }
    return PREPARE_UNRECOGNIZED_STATEMENT;
}

/* ─── Execute ────────────────────────────────────────────────────────────── */

typedef enum {
    EXECUTE_SUCCESS,
    EXECUTE_DUPLICATE_KEY,
} ExecuteResult;

/*
 * create_new_root — called after splitting the root leaf.
 * Copies the old root into a new left-child page; converts page 0 into an
 * internal root with one key separating the left and right children.
 */
void create_new_root(Table* table, uint32_t right_child_page_num) {
    void*    root                = get_page(table->pager, table->root_page_num);
    void*    right_child         = get_page(table->pager, right_child_page_num);
    uint32_t left_child_page_num = get_unused_page_num(table->pager);
    void*    left_child          = get_page(table->pager, left_child_page_num);

    /* Copy old root (currently a leaf with left-half keys) into the new left child */
    memcpy(left_child, root, PAGE_SIZE);
    set_root(left_child, false);

    /* Convert root page into an internal node */
    initialize_internal_node(root);
    set_root(root, true);
    *internal_node_num_keys(root)    = 1;
    *internal_node_child(root, 0)    = left_child_page_num;
    uint32_t left_child_max_key      = get_node_max_key(table->pager, left_child);
    *internal_node_key(root, 0)      = left_child_max_key;
    *internal_node_right_child(root) = right_child_page_num;

    *node_parent(left_child)  = table->root_page_num;
    *node_parent(right_child) = table->root_page_num;
}

/*
 * internal_node_split_and_insert — called when `old_node` (an internal node)
 * is full and needs to absorb one more child (`child_page_num`).
 *
 * Algorithm (MAX_CELLS = 3, so 4 existing children + 1 new = 5 total):
 *   1. Build a sorted array all_children[5] of all child page numbers.
 *   2. Left node  (old_node)  gets LEFT_SPLIT_COUNT  = 2 cells  +  right_child = all_children[2].
 *   3. The max key of all_children[2] is the promoted separator key.
 *   4. Right node (new_node)  gets RIGHT_SPLIT_COUNT = 1 cell   +  right_child = all_children[4].
 *   5. Promote the separator to old_node's parent (or create a new root).
 */
void internal_node_split_and_insert(Table* table, uint32_t old_page_num,
                                     uint32_t child_page_num) {
    Pager*   pager         = table->pager;
    void*    old_node      = get_page(pager, old_page_num);
    uint32_t old_max       = get_node_max_key(pager, old_node);
    void*    child         = get_page(pager, child_page_num);
    uint32_t child_max_key = get_node_max_key(pager, child);
    bool     splitting_root = is_root(old_node);

    void*    new_node;
    uint32_t new_page_num;
    uint32_t parent_page_num;

    if (splitting_root) {
        /*
         * Move old root content to a fresh left-child page.
         * We must call get_page for left first so num_pages advances before
         * we allocate new_page_num — otherwise both would get the same number.
         */
        uint32_t left_page_num = get_unused_page_num(pager);
        void*    left_node     = get_page(pager, left_page_num);   /* advances num_pages */
        memcpy(left_node, old_node, PAGE_SIZE);
        set_root(left_node, false);

        new_page_num = get_unused_page_num(pager);
        new_node     = get_page(pager, new_page_num);              /* advances num_pages */
        initialize_internal_node(new_node);

        old_page_num    = left_page_num;
        old_node        = left_node;
        parent_page_num = table->root_page_num;
    } else {
        parent_page_num = *node_parent(old_node);
        new_page_num    = get_unused_page_num(pager);
        new_node        = get_page(pager, new_page_num);
        initialize_internal_node(new_node);
    }

    /*
     * Build all_children[MAX_CELLS+2] = sorted array of all 5 child page nums.
     * old_node has MAX_CELLS keys → MAX_CELLS+1 children;  + 1 new = MAX_CELLS+2 total.
     */
    uint32_t num_keys = *internal_node_num_keys(old_node); /* = MAX_CELLS = 3 */
    uint32_t total    = num_keys + 2;                       /* = 5 */

    /* Collect the num_keys+1 = 4 existing children in sorted order */
    uint32_t existing[4];   /* num_keys + 1 slots */
    for (uint32_t i = 0; i <= num_keys; i++) {
        existing[i] = *internal_node_child(old_node, i);
    }

    /*
     * Find the sorted insert position by scanning existing children's max keys.
     * Default: new child goes last (position total-1 = 4).
     */
    uint32_t insert_pos = total - 1;
    for (uint32_t i = 0; i <= num_keys; i++) {
        void* ec = get_page(pager, existing[i]);
        if (get_node_max_key(pager, ec) > child_max_key) {
            insert_pos = i;
            break;
        }
    }

    /* Merge into sorted all_children[5] */
    uint32_t all_children[5];
    uint32_t j = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (i == insert_pos) {
            all_children[i] = child_page_num;
        } else {
            all_children[i] = existing[j++];
        }
    }

    /*
     * Redistribute:
     *   Left (old_node):  children [0 .. LEFT_SPLIT_COUNT-1] as cells,
     *                     child [LEFT_SPLIT_COUNT] as right_child.
     *   Promoted key:     max key of all_children[LEFT_SPLIT_COUNT].
     *   Right (new_node): children [LEFT_SPLIT_COUNT+1 .. total-2] as cells,
     *                     child [total-1] as right_child.
     *
     * With MAX=3: LEFT=2, total=5
     *   Left  cells: [0]=leaf1-7, [1]=leaf8-14;  right_child=[2]=leaf15-21
     *   Promoted key: max(leaf15-21) = 21
     *   Right cells:  [3]=leaf22-28;              right_child=[4]=leaf29-35
     */

    /* Rebuild old_node (left) */
    *internal_node_num_keys(old_node) = INTERNAL_NODE_LEFT_SPLIT_COUNT;
    for (uint32_t i = 0; i < INTERNAL_NODE_LEFT_SPLIT_COUNT; i++) {
        *internal_node_child(old_node, i) = all_children[i];
        void* c = get_page(pager, all_children[i]);
        *internal_node_key(old_node, i) = get_node_max_key(pager, c);
        *node_parent(c) = old_page_num;
    }
    uint32_t left_rc = all_children[INTERNAL_NODE_LEFT_SPLIT_COUNT];
    *internal_node_right_child(old_node) = left_rc;
    *node_parent(get_page(pager, left_rc)) = old_page_num;

    /* Promoted separator = max key of left's right_child */
    uint32_t promoted_key = get_node_max_key(pager, get_page(pager, left_rc));

    /* Rebuild new_node (right) */
    initialize_internal_node(new_node);
    *internal_node_num_keys(new_node) = INTERNAL_NODE_RIGHT_SPLIT_COUNT;
    uint32_t right_start = INTERNAL_NODE_LEFT_SPLIT_COUNT + 1;
    for (uint32_t i = 0; i < INTERNAL_NODE_RIGHT_SPLIT_COUNT; i++) {
        *internal_node_child(new_node, i) = all_children[right_start + i];
        void* c = get_page(pager, all_children[right_start + i]);
        *internal_node_key(new_node, i) = get_node_max_key(pager, c);
        *node_parent(c) = new_page_num;
    }
    uint32_t right_rc = all_children[total - 1];
    *internal_node_right_child(new_node) = right_rc;
    *node_parent(get_page(pager, right_rc)) = new_page_num;

    /* Set parent pointers and is_root flags */
    set_root(old_node, false);
    set_root(new_node, false);
    *node_parent(old_node) = parent_page_num;
    *node_parent(new_node) = parent_page_num;

    if (splitting_root) {
        /* Wire up the existing root page as the new internal root */
        void* root = get_page(pager, table->root_page_num);
        initialize_internal_node(root);
        set_root(root, true);
        *internal_node_num_keys(root)    = 1;
        *internal_node_child(root, 0)    = old_page_num;
        *internal_node_key(root, 0)      = promoted_key;
        *internal_node_right_child(root) = new_page_num;
        *node_parent(old_node) = table->root_page_num;
        *node_parent(new_node) = table->root_page_num;
    } else {
        /* Update separator key in parent and insert new_node */
        void* parent = get_page(pager, parent_page_num);
        update_internal_node_key(parent, old_max, promoted_key);
        internal_node_insert(table, parent_page_num, new_page_num);
    }
}

/*
 * internal_node_insert — add `child_page_num` into the parent internal node.
 *
 * If the parent is already full, delegate to internal_node_split_and_insert.
 */
void internal_node_insert(Table* table, uint32_t parent_page_num,
                          uint32_t child_page_num) {
    void*    parent   = get_page(table->pager, parent_page_num);
    void*    child    = get_page(table->pager, child_page_num);
    uint32_t child_max_key     = get_node_max_key(table->pager, child);
    uint32_t index             = internal_node_find_child(parent, child_max_key);
    uint32_t original_num_keys = *internal_node_num_keys(parent);

    if (original_num_keys >= INTERNAL_NODE_MAX_CELLS) {
        internal_node_split_and_insert(table, parent_page_num, child_page_num);
        return;
    }

    /* Bump the key count — the new slot is now valid memory */
    *internal_node_num_keys(parent) = original_num_keys + 1;

    uint32_t right_child_page_num = *internal_node_right_child(parent);
    void*    right_child          = get_page(table->pager, right_child_page_num);

    if (child_max_key > get_node_max_key(table->pager, right_child)) {
        /*
         * Case (a): new child is the new rightmost.
         * Demote the current right_child to cell[original_num_keys],
         * then point right_child at the new child.
         */
        *internal_node_child(parent, original_num_keys) = right_child_page_num;
        *internal_node_key(parent, original_num_keys)   = get_node_max_key(table->pager, right_child);
        *internal_node_right_child(parent)              = child_page_num;
    } else {
        /*
         * Case (b): new child goes somewhere in the middle.
         * Shift cells from [index .. original_num_keys-1] one slot to the right,
         * then write the new child at `index`.
         */
        for (uint32_t i = original_num_keys; i > index; i--) {
            void* dst = internal_node_cell(parent, i);
            void* src = internal_node_cell(parent, i - 1);
            memcpy(dst, src, INTERNAL_NODE_CELL_SIZE);
        }
        *internal_node_child(parent, index) = child_page_num;
        *internal_node_key(parent, index)   = child_max_key;
    }
}

/*
 * leaf_node_split_and_insert — the leaf at cursor->page_num is full.
 * Allocate a new leaf, distribute all (LEAF_NODE_MAX_CELLS + 1) items
 * evenly, then promote the split up to the parent (or create a new root).
 */
void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, Row* value) {
    void*    old_node     = get_page(cursor->table->pager, cursor->page_num);
    uint32_t old_max      = get_node_max_key(cursor->table->pager, old_node);   /* capture BEFORE the split */
    uint32_t new_page_num = get_unused_page_num(cursor->table->pager);
    void*    new_node     = get_page(cursor->table->pager, new_page_num);

    initialize_leaf_node(new_node);
    *node_parent(new_node)         = *node_parent(old_node);
    *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
    *leaf_node_next_leaf(old_node) = new_page_num;

    /*
     * Distribute LEAF_NODE_MAX_CELLS+1 logical items (the existing cells plus
     * the new one) across old_node (left) and new_node (right).
     * Iterate right-to-left so we can read from old_node while writing to it.
     */
    for (int32_t i = (int32_t)LEAF_NODE_MAX_CELLS; i >= 0; i--) {
        uint32_t ui = (uint32_t)i;
        void*    destination_node;

        if (ui >= LEAF_NODE_LEFT_SPLIT_COUNT) {
            destination_node = new_node;
        } else {
            destination_node = old_node;
        }

        uint32_t index_within_node = ui % LEAF_NODE_LEFT_SPLIT_COUNT;

        if (ui == cursor->cell_num) {
            /* Write the new key/value at its sorted position */
            serialize_row(value, leaf_node_value(destination_node, index_within_node));
            *leaf_node_key(destination_node, index_within_node) = key;
        } else if (ui > cursor->cell_num) {
            /* Items after insertion point shift right by one slot */
            memcpy(leaf_node_cell(destination_node, index_within_node),
                   leaf_node_cell(old_node, ui - 1),
                   LEAF_NODE_CELL_SIZE);
        } else {
            /* Items before insertion point stay in place */
            memcpy(leaf_node_cell(destination_node, index_within_node),
                   leaf_node_cell(old_node, ui),
                   LEAF_NODE_CELL_SIZE);
        }
    }

    *(leaf_node_num_cells(old_node)) = LEAF_NODE_LEFT_SPLIT_COUNT;
    *(leaf_node_num_cells(new_node)) = LEAF_NODE_RIGHT_SPLIT_COUNT;

    if (is_root(old_node)) {
        create_new_root(cursor->table, new_page_num);
    } else {
        uint32_t parent_page_num = *node_parent(old_node);
        uint32_t new_max         = get_node_max_key(cursor->table->pager, old_node);
        void*    parent          = get_page(cursor->table->pager, parent_page_num);
        update_internal_node_key(parent, old_max, new_max);
        internal_node_insert(cursor->table, parent_page_num, new_page_num);
    }
}

/*
 * leaf_node_insert — write (key, value) into the leaf node at cursor position.
 * Cells to the right of cell_num are shifted right to make room.
 * If the leaf is full, delegates to leaf_node_split_and_insert.
 */
void leaf_node_insert(Cursor* c, uint32_t key, Row* value) {
    void*    node      = get_page(c->table->pager, c->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (num_cells >= LEAF_NODE_MAX_CELLS) {
        leaf_node_split_and_insert(c, key, value);
        return;
    }

    if (c->cell_num < num_cells) {
        /* Shift existing cells right to make room */
        for (uint32_t i = num_cells; i > c->cell_num; i--) {
            memcpy(leaf_node_cell(node, i),
                   leaf_node_cell(node, i - 1),
                   LEAF_NODE_CELL_SIZE);
        }
    }

    *(leaf_node_num_cells(node))        += 1;
    *(leaf_node_key(node, c->cell_num))  = key;
    serialize_row(value, leaf_node_value(node, c->cell_num));
}

ExecuteResult execute_insert(Statement* stmt, Table* t) {
    uint32_t key_to_insert = stmt->row_to_insert.id;
    Cursor*  c             = table_find(t, key_to_insert);

    void*    node      = get_page(t->pager, c->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (c->cell_num < num_cells &&
        *leaf_node_key(node, c->cell_num) == key_to_insert) {
        free(c);
        return EXECUTE_DUPLICATE_KEY;
    }

    leaf_node_insert(c, key_to_insert, &stmt->row_to_insert);
    free(c);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* stmt __attribute__((unused)), Table* t) {
    Cursor* c = table_start(t);
    Row row;
    while (!c->end_of_table) {
        deserialize_row(cursor_value(c), &row);
        print_row(&row);
        cursor_advance(c);
    }
    free(c);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement* stmt, Table* t) {
    switch (stmt->type) {
        case STATEMENT_INSERT: return execute_insert(stmt, t);
        case STATEMENT_SELECT: return execute_select(stmt, t);
    }
    return EXECUTE_SUCCESS;
}

/* ─── main REPL ──────────────────────────────────────────────────────────── */

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }

    Table*       table = db_open(argv[1]);
    InputBuffer* ib    = new_input_buffer();

    while (true) {
        print_prompt();
        read_input(ib);

        if (ib->buffer[0] == '.') {
            switch (do_meta_command(ib, table)) {
                case META_COMMAND_SUCCESS:
                    continue;
                case META_COMMAND_UNRECOGNIZED_COMMAND:
                    printf("Unrecognized command '%s'.\n", ib->buffer);
                    continue;
            }
        }

        Statement stmt;
        switch (prepare_statement(ib, &stmt)) {
            case PREPARE_SUCCESS:             break;
            case PREPARE_NEGATIVE_ID:         printf("ID must be positive.\n");                     continue;
            case PREPARE_STRING_TOO_LONG:     printf("String is too long.\n");                      continue;
            case PREPARE_SYNTAX_ERROR:        printf("Syntax error. Could not parse statement.\n");  continue;
            case PREPARE_UNRECOGNIZED_STATEMENT:
                printf("Unrecognized keyword at start of '%s'.\n", ib->buffer);
                continue;
        }

        switch (execute_statement(&stmt, table)) {
            case EXECUTE_SUCCESS:       printf("Executed.\n");           break;
            case EXECUTE_DUPLICATE_KEY: printf("Error: Duplicate key.\n"); break;
        }
    }
}
